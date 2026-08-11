#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_webdav_priv.h"

#define XML_HEADER "<?xml version=\"1.0\" encoding=\"utf-8\"?>"

/* Shorthand for "send this literal/escaped chunk, bail out on error" --
 * scoped to this file, used only inside functions that have a local `req`
 * and can `return` an esp_err_t directly. */
#define SEND(str)                                                              \
    do {                                                                       \
        esp_err_t _e = httpd_resp_send_chunk(req, (str), HTTPD_RESP_USE_STRLEN); \
        if (_e != ESP_OK) {                                                    \
            return _e;                                                        \
        }                                                                      \
    } while (0)
#define SEND_LEN(buf, len)                                          \
    do {                                                            \
        esp_err_t _e = httpd_resp_send_chunk(req, (buf), (len));    \
        if (_e != ESP_OK) {                                         \
            return _e;                                              \
        }                                                           \
    } while (0)
#define SEND_FREE(str)                                                         \
    do {                                                                       \
        esp_err_t _e = httpd_resp_send_chunk(req, (str), HTTPD_RESP_USE_STRLEN); \
        if (_e != ESP_OK) {                                                    \
            free(scratch);                                                     \
            return _e;                                                        \
        }                                                                      \
    } while (0)
#define SEND_ESC(str)                                    \
    do {                                                 \
        esp_err_t _e = webdav_send_xml_escaped(req, (str)); \
        if (_e != ESP_OK) {                               \
            return _e;                                    \
        }                                                 \
    } while (0)

static esp_err_t drain_request_body(httpd_req_t *req)
{
    char buf[128];
    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        int r = webdav_recv_body(req, buf, want);
        if (r <= 0) {
            return ESP_FAIL;
        }
        remaining -= (size_t)r;
    }
    return ESP_OK;
}

/* Append XML-escaped text to buf. Returns false if it would not fit. */
static bool append_escaped(char *buf, size_t cap, size_t *len, const char *src)
{
    for (size_t i = 0; src[i] != '\0'; i++) {
        const char *ent = NULL;
        switch (src[i]) {
            case '&':  ent = "&amp;";  break;
            case '<':  ent = "&lt;";   break;
            case '>':  ent = "&gt;";   break;
            case '"':  ent = "&quot;"; break;
            case '\'': ent = "&apos;"; break;
            default: break;
        }
        if (ent) {
            size_t n = strlen(ent);
            if (*len + n >= cap) {
                return false;
            }
            memcpy(buf + *len, ent, n);
            *len += n;
        } else {
            if (*len + 1 >= cap) {
                return false;
            }
            buf[(*len)++] = src[i];
        }
    }
    buf[*len] = '\0';
    return true;
}

static bool append_fmt(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len) {
        return false;
    }
    *len += (size_t)n;
    return true;
}

/*
 * Emit a single <D:response> element describing fs_path/rel_path.
 *
 * Built up in `scratch` and written with one httpd_resp_send_chunk() rather
 * than the eight-or-so it used to take. Each chunk call is several send()
 * syscalls of its own (size line, payload, CRLF), so a directory listing was
 * running to hundreds of tiny packets.
 */
static esp_err_t send_entry(httpd_req_t *req, const struct esp_webdav_server *srv,
                             const char *rel_path, bool is_dir, const struct stat *st,
                             char *scratch, size_t scratch_cap)
{
    char href[WEBDAV_MAX_PATH_LEN];
    if (!webdav_build_href(srv, rel_path, is_dir, href, sizeof(href))) {
        return ESP_ERR_INVALID_SIZE;
    }

    const char *name = strrchr(rel_path, '/');
    name = name ? name + 1 : rel_path;
    if (name[0] == '\0') {
        name = "/";
    }

    char lastmod[40];
    webdav_format_http_date(st->st_mtime, lastmod, sizeof(lastmod));
    char created[24];
    webdav_format_iso8601(st->st_mtime, created, sizeof(created));

    size_t n = 0;
    bool ok = append_fmt(scratch, scratch_cap, &n, "<D:response><D:href>");
    ok = ok && append_escaped(scratch, scratch_cap, &n, href);
    ok = ok && append_fmt(scratch, scratch_cap, &n,
                          "</D:href><D:propstat><D:prop><D:displayname>");
    ok = ok && append_escaped(scratch, scratch_cap, &n, name);
    ok = ok && append_fmt(scratch, scratch_cap, &n, "</D:displayname>");

    if (is_dir) {
        ok = ok && append_fmt(scratch, scratch_cap, &n,
                              "<D:resourcetype><D:collection/></D:resourcetype>");
    } else {
        ok = ok && append_fmt(scratch, scratch_cap, &n,
                              "<D:resourcetype/><D:getcontentlength>%ld</D:getcontentlength>"
                              "<D:getcontenttype>",
                              (long)st->st_size);
        ok = ok && append_escaped(scratch, scratch_cap, &n, webdav_guess_mime_type(name));
        ok = ok && append_fmt(scratch, scratch_cap, &n,
                              "</D:getcontenttype><D:getetag>\"%lx-%lx\"</D:getetag>",
                              (unsigned long)st->st_size, (unsigned long)st->st_mtime);
    }

    ok = ok && append_fmt(scratch, scratch_cap, &n,
                          "<D:creationdate>%s</D:creationdate>"
                          "<D:getlastmodified>%s</D:getlastmodified>"
                          "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
                          "</D:response>",
                          created, lastmod);
    if (!ok) {
        return ESP_ERR_INVALID_SIZE;
    }
    return httpd_resp_send_chunk(req, scratch, n);
}

/*
 * List the children of the directory at `path` (length `len`). `levels` is how
 * many further levels of grandchildren to include: 0 = just these children,
 * -1 = unlimited (Depth: infinity).
 *
 * `path` is extended and restored around each child rather than copied into a
 * per-level buffer, so recursion costs a few pointers per level instead of two
 * WEBDAV_MAX_PATH_LEN buffers -- which used to exhaust an 8 KB task stack after
 * roughly five levels.
 */
static esp_err_t send_children(httpd_req_t *req, const struct esp_webdav_server *srv,
                                char *path, size_t len, size_t cap, int levels, int depth,
                                char *scratch, size_t scratch_cap)
{
    if (depth > WEBDAV_MAX_DEPTH) {
        return ESP_OK; /* stop descending; the listing so far stays valid */
    }

    DIR *dir = opendir(path);
    if (!dir) {
        return ESP_OK; /* vanished between stat() and opendir(), not fatal */
    }

    esp_err_t ret = ESP_OK;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        size_t child_len = webdav_path_push(path, len, cap, ent->d_name);
        if (child_len == 0) {
            continue; /* path too long to represent, silently skip */
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            path[len] = '\0';
            continue;
        }
        bool is_dir = S_ISDIR(st.st_mode);
        ret = send_entry(req, srv, path + srv->root_len, is_dir, &st, scratch, scratch_cap);
        if (ret == ESP_OK && is_dir && levels != 0) {
            ret = send_children(req, srv, path, child_len, cap, levels == -1 ? -1 : levels - 1,
                                depth + 1, scratch, scratch_cap);
        }
        path[len] = '\0';
        if (ret != ESP_OK) {
            break;
        }
    }
    closedir(dir);
    return ret;
}

esp_err_t webdav_handle_propfind(httpd_req_t *req)
{
    struct esp_webdav_server *srv = (struct esp_webdav_server *)req->user_ctx;

    /* Drain the body before anything that can fail. We don't parse the
     * <D:prop> selection (we always return the standard "allprop" set), but
     * the body still has to come off the socket or it would be mis-parsed as
     * the next request. Doing it first means every error path below can
     * answer normally and keep the connection -- important because a 404 here
     * is routine, not exceptional: WebDAV clients probe constantly for files
     * like .DS_Store and ._*, and closing on each one makes them reconnect in
     * a tight loop. */
    if (drain_request_body(req) != ESP_OK) {
        return webdav_reply_error_close(req, "400 Bad Request");
    }

    int depth = -1; /* infinity, the RFC 4918 default when the header is absent */
    char depth_hdr[16];
    if (httpd_req_get_hdr_value_str(req, "Depth", depth_hdr, sizeof(depth_hdr)) == ESP_OK) {
        if (strcmp(depth_hdr, "0") == 0) {
            depth = 0;
        } else if (strcmp(depth_hdr, "1") == 0) {
            depth = 1;
        } else {
            depth = -1;
        }
    }
#if !CONFIG_ESP_WEBDAV_ALLOW_DEPTH_INFINITY
    if (depth == -1) {
        depth = 1;
    }
#endif

    char path[WEBDAV_MAX_PATH_LEN];
    const char *rel = NULL;
    esp_err_t err = webdav_resolve_path(srv, req, path, sizeof(path), &rel);
    if (err != ESP_OK) {
        return webdav_reply_error(req, "400 Bad Request");
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return webdav_reply_error(req, "404 Not Found");
    }

    httpd_resp_set_status(req, "207 Multi-Status");
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    httpd_resp_set_hdr(req, "DAV", "1, 2");

    /* One scratch buffer for the whole response: each <D:response> is
     * assembled here and sent as a single chunk. */
    size_t scratch_cap = WEBDAV_MAX_PATH_LEN * 2 + 512;
    char *scratch = malloc(scratch_cap);
    if (!scratch) {
        return webdav_reply_error(req, "500 Internal Server Error");
    }

    SEND_FREE(XML_HEADER "<D:multistatus xmlns:D=\"DAV:\">");

    bool is_dir = S_ISDIR(st.st_mode);
    esp_err_t ret = send_entry(req, srv, rel, is_dir, &st, scratch, scratch_cap);
    if (ret == ESP_OK && is_dir && depth != 0) {
        ret = send_children(req, srv, path, strlen(path), sizeof(path),
                            depth == -1 ? -1 : 0, 0, scratch, scratch_cap);
    }
    if (ret != ESP_OK) {
        free(scratch);
        return ret;
    }
    free(scratch);

    SEND("</D:multistatus>");
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t webdav_handle_proppatch(httpd_req_t *req)
{
    struct esp_webdav_server *srv = (struct esp_webdav_server *)req->user_ctx;
    /* Drain first, for the same reason as PROPFIND above. */
    if (drain_request_body(req) != ESP_OK) {
        return webdav_reply_error_close(req, "400 Bad Request");
    }

    if (srv->read_only) {
        return webdav_reply_error(req, "403 Forbidden");
    }

    char path[WEBDAV_MAX_PATH_LEN];
    const char *rel = NULL;
    esp_err_t err = webdav_resolve_path(srv, req, path, sizeof(path), &rel);
    if (err != ESP_OK) {
        return webdav_reply_error(req, "400 Bad Request");
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return webdav_reply_error(req, "404 Not Found");
    }

    /* Dead/custom properties aren't stored anywhere on a plain filesystem;
     * report every PROPPATCH as rejected rather than silently pretending
     * to have saved a property that will vanish on the next request. */
    char href[WEBDAV_MAX_PATH_LEN];
    if (!webdav_build_href(srv, rel, S_ISDIR(st.st_mode), href, sizeof(href))) {
        return webdav_reply_error(req, "500 Internal Server Error");
    }

    httpd_resp_set_status(req, "207 Multi-Status");
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    SEND(XML_HEADER "<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>");
    SEND_ESC(href);
    SEND("</D:href><D:propstat><D:status>HTTP/1.1 403 Forbidden</D:status></D:propstat>"
         "</D:response></D:multistatus>");
    return httpd_resp_send_chunk(req, NULL, 0);
}

#undef SEND
#undef SEND_LEN
#undef SEND_ESC
