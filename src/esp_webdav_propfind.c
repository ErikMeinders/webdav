#include <dirent.h>
#include <stdio.h>
#include <string.h>
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

/* Emit a single <D:response> element describing fs_path/rel_path. */
static esp_err_t send_entry(httpd_req_t *req, const struct esp_webdav_server *srv,
                             const char *fs_path, const char *rel_path, bool is_dir,
                             const struct stat *st)
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

    SEND("<D:response><D:href>");
    SEND_ESC(href);
    SEND("</D:href><D:propstat><D:prop><D:displayname>");
    SEND_ESC(name);
    SEND("</D:displayname>");

    if (is_dir) {
        SEND("<D:resourcetype><D:collection/></D:resourcetype>");
    } else {
        SEND("<D:resourcetype/>");
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "<D:getcontentlength>%ld</D:getcontentlength>",
                          (long)st->st_size);
        SEND_LEN(buf, n);
        SEND("<D:getcontenttype>");
        SEND_ESC(webdav_guess_mime_type(name));
        SEND("</D:getcontenttype>");
        n = snprintf(buf, sizeof(buf), "<D:getetag>\"%lx-%lx\"</D:getetag>",
                     (unsigned long)st->st_size, (unsigned long)st->st_mtime);
        SEND_LEN(buf, n);
    }

    char lastmod[40];
    webdav_format_http_date(st->st_mtime, lastmod, sizeof(lastmod));
    char created[24];
    webdav_format_iso8601(st->st_mtime, created, sizeof(created));
    char datebuf[160];
    int n = snprintf(datebuf, sizeof(datebuf),
                      "<D:creationdate>%s</D:creationdate><D:getlastmodified>%s</D:getlastmodified>",
                      created, lastmod);
    SEND_LEN(datebuf, n);

    SEND("</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>");
    return ESP_OK;
}

/*
 * List the children of dir_path/dir_rel. `levels` is how many further
 * levels of grandchildren to also include: 0 = just these children,
 * -1 = unlimited (Depth: infinity).
 */
static esp_err_t send_children(httpd_req_t *req, const struct esp_webdav_server *srv,
                                const char *dir_path, const char *dir_rel, int levels)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return ESP_OK; /* vanished between stat() and opendir(), not fatal */
    }

    esp_err_t ret = ESP_OK;
    struct dirent *ent;
    char child_path[WEBDAV_MAX_PATH_LEN];
    char child_rel[WEBDAV_MAX_PATH_LEN];
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        int n1 = snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, ent->d_name);
        int n2 = snprintf(child_rel, sizeof(child_rel), "%s/%s", dir_rel, ent->d_name);
        if (n1 < 0 || (size_t)n1 >= sizeof(child_path) || n2 < 0 || (size_t)n2 >= sizeof(child_rel)) {
            continue; /* path too long to represent, silently skip */
        }
        struct stat st;
        if (stat(child_path, &st) != 0) {
            continue;
        }
        bool is_dir = S_ISDIR(st.st_mode);
        ret = send_entry(req, srv, child_path, child_rel, is_dir, &st);
        if (ret != ESP_OK) {
            break;
        }
        if (is_dir && levels != 0) {
            ret = send_children(req, srv, child_path, child_rel, levels == -1 ? -1 : levels - 1);
            if (ret != ESP_OK) {
                break;
            }
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

    SEND(XML_HEADER "<D:multistatus xmlns:D=\"DAV:\">");

    bool is_dir = S_ISDIR(st.st_mode);
    esp_err_t ret = send_entry(req, srv, path, rel, is_dir, &st);
    if (ret == ESP_OK && is_dir && depth != 0) {
        ret = send_children(req, srv, path, rel, depth == -1 ? -1 : 0);
    }
    if (ret != ESP_OK) {
        return ret;
    }

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
