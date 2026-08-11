#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_webdav_priv.h"

static const char *TAG = "webdav_copymove";

static bool get_overwrite_allowed(httpd_req_t *req)
{
    char val[4];
    if (httpd_req_get_hdr_value_str(req, "Overwrite", val, sizeof(val)) != ESP_OK) {
        return true; /* RFC 4918: absent Overwrite defaults to "T" */
    }
    return !(val[0] == 'F' || val[0] == 'f');
}

/* Strip an optional "scheme://host[:port]" prefix from a Destination header
 * value, leaving just the path part. */
static const char *strip_authority(const char *dest)
{
    const char *scheme = strstr(dest, "://");
    if (!scheme) {
        return dest;
    }
    const char *after_host = strchr(scheme + 3, '/');
    return after_host ? after_host : "/";
}

static esp_err_t resolve_destination(struct esp_webdav_server *srv, httpd_req_t *req,
                                      char *out, size_t out_size, const char **out_rel)
{
    size_t hlen = httpd_req_get_hdr_value_len(req, "Destination");
    if (hlen == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char hdr[WEBDAV_MAX_PATH_LEN];
    if (hlen >= sizeof(hdr)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (httpd_req_get_hdr_value_str(req, "Destination", hdr, sizeof(hdr)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    return webdav_resolve_uri_path(srv, strip_authority(hdr), out, out_size, out_rel);
}

/* Copy a single regular file's contents. Caller has already verified src
 * is a regular file. */
static esp_err_t copy_file_contents(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        return ESP_FAIL;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    char *buf = malloc(WEBDAV_IO_BUF_SIZE);
    if (!buf) {
        ret = ESP_ERR_NO_MEM;
    } else {
        size_t n;
        while ((n = fread(buf, 1, WEBDAV_IO_BUF_SIZE, in)) > 0) {
            if (fwrite(buf, 1, n, out) != n) {
                ret = ESP_FAIL;
                break;
            }
        }
        if (ret == ESP_OK && ferror(in)) {
            ret = ESP_FAIL;
        }
        free(buf);
    }

    fclose(in);
    /* Buffered stdio flushes the tail at fclose(), so a write error -- ENOSPC
     * above all -- can surface only here. */
    if (fclose(out) != 0 && ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    return ret;
}

/* Recursively copy src to dst. `depth0` restricts a collection copy to just
 * the collection itself (Depth: 0 on COPY), matching RFC 4918 semantics. */
static esp_err_t copy_recursive(char *src, size_t src_len, char *dst, size_t dst_len,
                                 size_t cap, bool depth0, int depth)
{
    if (depth > WEBDAV_MAX_DEPTH) {
        return ESP_ERR_INVALID_SIZE;
    }

    struct stat st;
    if (stat(src, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!S_ISDIR(st.st_mode)) {
        return copy_file_contents(src, dst);
    }

    if (mkdir(dst, 0755) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    if (depth0) {
        return ESP_OK;
    }

    DIR *dir = opendir(src);
    if (!dir) {
        return ESP_FAIL;
    }
    esp_err_t ret = ESP_OK;
    struct dirent *ent;
    /* One buffer per side, extended and restored around each child, so the
     * stack cost per level is a few pointers rather than 2x
     * WEBDAV_MAX_PATH_LEN. */
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        size_t s_len = webdav_path_push(src, src_len, cap, ent->d_name);
        size_t d_len = webdav_path_push(dst, dst_len, cap, ent->d_name);
        if (s_len == 0 || d_len == 0) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        ret = copy_recursive(src, s_len, dst, d_len, cap, false, depth + 1);
        src[src_len] = '\0';
        dst[dst_len] = '\0';
        if (ret != ESP_OK) {
            break;
        }
    }
    closedir(dir);
    return ret;
}

/* True if `dst` sits inside the collection at `src`. Copying or moving a
 * collection into itself would otherwise walk into the copy being created. */
static bool dst_within_src(const char *src, const char *dst)
{
    size_t n = strlen(src);
    return strncmp(dst, src, n) == 0 && dst[n] == '/';
}

/*
 * Shared MOVE/COPY precondition: refuse a destination that is the source, or
 * inside it. Without this the "delete the destination first" step below would
 * delete the *source* when the two are the same path -- losing the file
 * outright -- and a collection copied into its own subtree would recurse
 * until the stack ran out. RFC 4918 9.8.5/9.9.4 specify 403 here.
 */
static esp_err_t check_dst_sane(httpd_req_t *req, const char *src, const struct stat *src_st,
                                 const char *dst, bool *rejected)
{
    *rejected = false;
    if (strcmp(src, dst) == 0) {
        *rejected = true;
        return webdav_reply_error(req, "403 Forbidden");
    }
    if (S_ISDIR(src_st->st_mode) && dst_within_src(src, dst)) {
        *rejected = true;
        return webdav_reply_error(req, "403 Forbidden");
    }
    return ESP_OK;
}

esp_err_t webdav_handle_move(httpd_req_t *req)
{
    struct esp_webdav_server *srv = (struct esp_webdav_server *)req->user_ctx;
    if (srv->read_only) {
        return webdav_reply_error(req, "403 Forbidden");
    }

    char depth[16];
    if (httpd_req_get_hdr_value_str(req, "Depth", depth, sizeof(depth)) == ESP_OK
        && strcasecmp(depth, "infinity") != 0) {
        return webdav_reply_error(req, "400 Bad Request");
    }

    char src[WEBDAV_MAX_PATH_LEN];
    esp_err_t err = webdav_resolve_path(srv, req, src, sizeof(src), NULL);
    if (err != ESP_OK) {
        return webdav_reply_error(req, "400 Bad Request");
    }
    struct stat src_st;
    if (stat(src, &src_st) != 0) {
        return webdav_reply_error(req, "404 Not Found");
    }

    char dst[WEBDAV_MAX_PATH_LEN];
    err = resolve_destination(srv, req, dst, sizeof(dst), NULL);
    if (err != ESP_OK) {
        return webdav_reply_error(req, "400 Bad Request");
    }

    bool rejected = false;
    esp_err_t pre = check_dst_sane(req, src, &src_st, dst, &rejected);
    if (rejected) {
        return pre;
    }

    struct stat dst_st;
    bool dst_exists = (stat(dst, &dst_st) == 0);
    if (dst_exists && !get_overwrite_allowed(req)) {
        return webdav_reply_error(req, "412 Precondition Failed");
    }
    if (dst_exists) {
        err = webdav_remove_recursive(dst);
        if (err != ESP_OK) {
            return webdav_reply_error(req, "500 Internal Server Error");
        }
    }

    if (rename(src, dst) != 0) {
        ESP_LOGW(TAG, "rename(%s -> %s) failed: errno=%d", src, dst, errno);
        return webdav_reply_error(req, "502 Bad Gateway");
    }

    httpd_resp_set_status(req, dst_exists ? "204 No Content" : "201 Created");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t webdav_handle_copy(httpd_req_t *req)
{
    struct esp_webdav_server *srv = (struct esp_webdav_server *)req->user_ctx;
    if (srv->read_only) {
        return webdav_reply_error(req, "403 Forbidden");
    }

    bool depth0 = false;
    char depth[16];
    if (httpd_req_get_hdr_value_str(req, "Depth", depth, sizeof(depth)) == ESP_OK) {
        if (strcmp(depth, "0") == 0) {
            depth0 = true;
        } else if (strcasecmp(depth, "infinity") != 0) {
            return webdav_reply_error(req, "400 Bad Request");
        }
    }

    char src[WEBDAV_MAX_PATH_LEN];
    esp_err_t err = webdav_resolve_path(srv, req, src, sizeof(src), NULL);
    if (err != ESP_OK) {
        return webdav_reply_error(req, "400 Bad Request");
    }
    struct stat src_st;
    if (stat(src, &src_st) != 0) {
        return webdav_reply_error(req, "404 Not Found");
    }

    char dst[WEBDAV_MAX_PATH_LEN];
    err = resolve_destination(srv, req, dst, sizeof(dst), NULL);
    if (err != ESP_OK) {
        return webdav_reply_error(req, "400 Bad Request");
    }

    bool rejected = false;
    esp_err_t pre = check_dst_sane(req, src, &src_st, dst, &rejected);
    if (rejected) {
        return pre;
    }

    struct stat dst_st;
    bool dst_exists = (stat(dst, &dst_st) == 0);
    if (dst_exists && !get_overwrite_allowed(req)) {
        return webdav_reply_error(req, "412 Precondition Failed");
    }
    if (dst_exists) {
        err = webdav_remove_recursive(dst);
        if (err != ESP_OK) {
            return webdav_reply_error(req, "500 Internal Server Error");
        }
    }

    err = copy_recursive(src, strlen(src), dst, strlen(dst), sizeof(src), depth0, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "copy(%s -> %s) failed: %s", src, dst, esp_err_to_name(err));
        return webdav_reply_error(req, "500 Internal Server Error");
    }

    httpd_resp_set_status(req, dst_exists ? "204 No Content" : "201 Created");
    return httpd_resp_send(req, NULL, 0);
}
