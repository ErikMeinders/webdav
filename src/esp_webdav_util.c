#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "esp_webdav_priv.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char s_hex[] = "0123456789ABCDEF";

int webdav_recv_body(httpd_req_t *req, char *buf, size_t len)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(WEBDAV_BODY_TIMEOUT_MS);

    while (true) {
        int r = httpd_req_recv(req, buf, len);
        if (r != HTTPD_SOCK_ERR_TIMEOUT) {
            return r;
        }
        /* Unsigned tick arithmetic, so this stays correct across a tick
         * counter wrap. Each call gets a fresh window: the timeout is
         * "silent for this long", not "slow overall", so a genuinely slow
         * but progressing upload is never cut off. */
        if ((TickType_t)(xTaskGetTickCount() - start) >= limit) {
            return HTTPD_SOCK_ERR_TIMEOUT;
        }
    }
}

bool webdav_url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        char c = src[i];
        if (c == '%') {
            if (!isxdigit((unsigned char)src[i + 1]) || !isxdigit((unsigned char)src[i + 2])) {
                return false;
            }
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            c = (char)strtol(hex, NULL, 16);
            if (c == '\0') {
                return false; /* embedded NUL is never valid in a path */
            }
            i += 2;
        }
        if (o + 1 >= dst_size) {
            return false;
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
    return true;
}

bool webdav_url_encode_path(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        bool safe = isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (safe) {
            if (o + 1 >= dst_size) {
                return false;
            }
            dst[o++] = (char)c;
        } else {
            if (o + 3 >= dst_size) {
                return false;
            }
            dst[o++] = '%';
            dst[o++] = s_hex[c >> 4];
            dst[o++] = s_hex[c & 0x0F];
        }
    }
    dst[o] = '\0';
    return true;
}

esp_err_t webdav_resolve_path(const struct esp_webdav_server *srv, httpd_req_t *req,
                               char *out, size_t out_size, const char **out_rel)
{
    char raw[WEBDAV_MAX_PATH_LEN];
    const char *uri = req->uri;
    size_t raw_len = 0;
    for (; uri[raw_len] != '\0' && uri[raw_len] != '?'; raw_len++) {
        if (raw_len + 1 >= sizeof(raw)) {
            return ESP_ERR_INVALID_SIZE;
        }
        raw[raw_len] = uri[raw_len];
    }
    raw[raw_len] = '\0';

    return webdav_resolve_uri_path(srv, raw, out, out_size, out_rel);
}

esp_err_t webdav_resolve_uri_path(const struct esp_webdav_server *srv, const char *uri_path,
                                   char *out, size_t out_size, const char **out_rel)
{
    char decoded[WEBDAV_MAX_PATH_LEN];
    if (!webdav_url_decode(uri_path, decoded, sizeof(decoded))) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *rest = decoded;
    if (srv->prefix_len > 0) {
        if (strncmp(decoded, srv->uri_prefix, srv->prefix_len) != 0) {
            return ESP_ERR_NOT_FOUND;
        }
        rest = decoded + srv->prefix_len;
        if (*rest != '\0' && *rest != '/') {
            return ESP_ERR_NOT_FOUND;
        }
    }

    /* Canonicalize: collapse "//", drop ".", reject ".." outright rather
     * than trying to resolve it -- simplest way to guarantee the result
     * never escapes root_path. */
    char canon[WEBDAV_MAX_PATH_LEN];
    size_t canon_len = 0;
    canon[0] = '\0';
    const char *p = rest;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *seg = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t seg_len = (size_t)(p - seg);
        if (seg_len == 1 && seg[0] == '.') {
            continue;
        }
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            return ESP_ERR_INVALID_ARG;
        }
        if (canon_len + 1 + seg_len >= sizeof(canon)) {
            return ESP_ERR_INVALID_SIZE;
        }
        canon[canon_len++] = '/';
        memcpy(canon + canon_len, seg, seg_len);
        canon_len += seg_len;
        canon[canon_len] = '\0';
    }

    if (srv->root_len + canon_len >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, srv->root_path, srv->root_len);
    memcpy(out + srv->root_len, canon, canon_len + 1);

    if (out_rel) {
        *out_rel = out + srv->root_len;
    }
    return ESP_OK;
}

bool webdav_build_href(const struct esp_webdav_server *srv, const char *rel_path,
                        bool is_dir, char *dst, size_t dst_size)
{
    size_t o = 0;
    if (srv->prefix_len > 0) {
        if (srv->prefix_len >= dst_size) {
            return false;
        }
        memcpy(dst, srv->uri_prefix, srv->prefix_len);
        o = srv->prefix_len;
    }
    dst[o] = '\0';

    if (rel_path == NULL || rel_path[0] == '\0') {
        if (o + 1 >= dst_size) {
            return false;
        }
        dst[o++] = '/';
        dst[o] = '\0';
    } else {
        char encoded[WEBDAV_MAX_PATH_LEN];
        if (!webdav_url_encode_path(rel_path, encoded, sizeof(encoded))) {
            return false;
        }
        size_t elen = strlen(encoded);
        if (o + elen >= dst_size) {
            return false;
        }
        memcpy(dst + o, encoded, elen);
        o += elen;
        dst[o] = '\0';
    }

    if (is_dir) {
        size_t len = strlen(dst);
        if (len == 0 || dst[len - 1] != '/') {
            if (len + 1 >= dst_size) {
                return false;
            }
            dst[len] = '/';
            dst[len + 1] = '\0';
        }
    }
    return true;
}

const char *webdav_guess_mime_type(const char *filename)
{
    static const struct {
        const char *ext;
        const char *mime;
    } table[] = {
        { "html", "text/html" },       { "htm", "text/html" },
        { "css", "text/css" },         { "js", "application/javascript" },
        { "json", "application/json" }, { "txt", "text/plain" },
        { "xml", "application/xml" },   { "png", "image/png" },
        { "jpg", "image/jpeg" },        { "jpeg", "image/jpeg" },
        { "gif", "image/gif" },         { "svg", "image/svg+xml" },
        { "ico", "image/x-icon" },      { "pdf", "application/pdf" },
        { "zip", "application/zip" },   { "gz", "application/gzip" },
        { "mp3", "audio/mpeg" },        { "mp4", "video/mp4" },
        { "wav", "audio/wav" },         { "csv", "text/csv" },
        { "md", "text/markdown" },
    };

    const char *slash = strrchr(filename, '/');
    const char *base = slash ? slash + 1 : filename;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) {
        return "application/octet-stream";
    }
    dot++;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcasecmp(dot, table[i].ext) == 0) {
            return table[i].mime;
        }
    }
    return "application/octet-stream";
}

void webdav_format_http_date(time_t mtime, char *dst, size_t dst_size)
{
    struct tm tm_utc;
    gmtime_r(&mtime, &tm_utc);
    strftime(dst, dst_size, "%a, %d %b %Y %H:%M:%S GMT", &tm_utc);
}

void webdav_format_iso8601(time_t mtime, char *dst, size_t dst_size)
{
    struct tm tm_utc;
    gmtime_r(&mtime, &tm_utc);
    strftime(dst, dst_size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

esp_err_t webdav_send_xml_escaped(httpd_req_t *req, const char *src)
{
    char buf[128];
    size_t o = 0;
    for (size_t i = 0;; i++) {
        char c = src[i];
        const char *ent = NULL;
        switch (c) {
            case '&': ent = "&amp;"; break;
            case '<': ent = "&lt;"; break;
            case '>': ent = "&gt;"; break;
            case '"': ent = "&quot;"; break;
            case '\'': ent = "&apos;"; break;
            default: break;
        }
        if (c == '\0' || ent != NULL || o >= sizeof(buf) - 6) {
            if (o > 0) {
                esp_err_t err = httpd_resp_send_chunk(req, buf, o);
                if (err != ESP_OK) {
                    return err;
                }
                o = 0;
            }
            if (c == '\0') {
                break;
            }
            if (ent != NULL) {
                esp_err_t err = httpd_resp_send_chunk(req, ent, strlen(ent));
                if (err != ESP_OK) {
                    return err;
                }
                continue;
            }
        }
        buf[o++] = c;
    }
    return ESP_OK;
}

bool webdav_is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

esp_err_t webdav_remove_recursive(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (!S_ISDIR(st.st_mode)) {
        return (unlink(path) == 0) ? ESP_OK : ESP_FAIL;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        return ESP_FAIL;
    }
    esp_err_t ret = ESP_OK;
    struct dirent *ent;
    char child[WEBDAV_MAX_PATH_LEN];
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        ret = webdav_remove_recursive(child);
        if (ret != ESP_OK) {
            break;
        }
    }
    closedir(dir);
    if (ret != ESP_OK) {
        return ret;
    }
    return (rmdir(path) == 0) ? ESP_OK : ESP_FAIL;
}
