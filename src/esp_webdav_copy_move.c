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
    fclose(out);
    return ret;
}

/* Recursively copy src to dst. `depth0` restricts a collection copy to just
 * the collection itself (Depth: 0 on COPY), matching RFC 4918 semantics. */
static esp_err_t copy_recursive(const char *src, const char *dst, bool depth0)
{
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
    char child_src[WEBDAV_MAX_PATH_LEN];
    char child_dst[WEBDAV_MAX_PATH_LEN];
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        int n1 = snprintf(child_src, sizeof(child_src), "%s/%s", src, ent->d_name);
        int n2 = snprintf(child_dst, sizeof(child_dst), "%s/%s", dst, ent->d_name);
        if (n1 < 0 || (size_t)n1 >= sizeof(child_src) || n2 < 0 || (size_t)n2 >= sizeof(child_dst)) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        ret = copy_recursive(child_src, child_dst, false);
        if (ret != ESP_OK) {
            break;
        }
    }
    closedir(dir);
    return ret;
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

    err = copy_recursive(src, dst, depth0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "copy(%s -> %s) failed: %s", src, dst, esp_err_to_name(err));
        return webdav_reply_error(req, "500 Internal Server Error");
    }

    httpd_resp_set_status(req, dst_exists ? "204 No Content" : "201 Created");
    return httpd_resp_send(req, NULL, 0);
}
