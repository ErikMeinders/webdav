#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_webdav_priv.h"

static const char *TAG = "esp_webdav";

#if CONFIG_ESP_WEBDAV_DEBUG_CONNECTIONS
#include <arpa/inet.h>
#include <sys/socket.h>

#define WEBDAV_DEBUG_DUMP_MAX 96

/* One bit per socket, so only the *first* read on each connection is dumped
 * -- otherwise a single upload buries the log. Indexed by fd modulo the
 * bitmap width, which is fine for a debug aid: lwip hands out a small,
 * dense range of descriptors. */
static uint64_t s_debug_dumped;
#define WEBDAV_DEBUG_BIT(fd) (1ULL << ((unsigned)(fd) % 64))

static int webdav_debug_recv(httpd_handle_t hd, int sockfd, char *buf, size_t buf_len, int flags)
{
    (void)hd;
    int ret = recv(sockfd, buf, buf_len, flags);
    if (ret > 0) {
        if (!(s_debug_dumped & WEBDAV_DEBUG_BIT(sockfd))) {
            s_debug_dumped |= WEBDAV_DEBUG_BIT(sockfd);
            int n = ret < WEBDAV_DEBUG_DUMP_MAX ? ret : WEBDAV_DEBUG_DUMP_MAX;
            ESP_LOGI(TAG, "fd=%d first read: %d bytes (dumping %d)", sockfd, ret, n);
            ESP_LOG_BUFFER_HEXDUMP(TAG, buf, n, ESP_LOG_INFO);
        }
        return ret;
    }
    if (ret < 0) {
        /* Must mirror esp_http_server's own errno mapping exactly, or its
         * timeout/retry handling breaks when we override recv. */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return HTTPD_SOCK_ERR_TIMEOUT;
        }
        if (errno == EINVAL || errno == EBADF || errno == EFAULT || errno == ENOTSOCK) {
            return HTTPD_SOCK_ERR_INVALID;
        }
        return HTTPD_SOCK_ERR_FAIL;
    }
    return ret; /* 0 == peer closed */
}

static esp_err_t webdav_debug_open(httpd_handle_t hd, int sockfd)
{
    struct sockaddr_storage peer;
    socklen_t len = sizeof(peer);
    char ip[INET6_ADDRSTRLEN] = "unknown";

    if (getpeername(sockfd, (struct sockaddr *)&peer, &len) == 0) {
        if (peer.ss_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr, ip, sizeof(ip));
        } else {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&peer)->sin6_addr, ip, sizeof(ip));
        }
    }
    ESP_LOGI(TAG, "connection opened: fd=%d peer=%s", sockfd, ip);

    s_debug_dumped &= ~WEBDAV_DEBUG_BIT(sockfd);
    return httpd_sess_set_recv_override(hd, sockfd, webdav_debug_recv);
}
#endif /* CONFIG_ESP_WEBDAV_DEBUG_CONNECTIONS */

esp_err_t webdav_reply_error(httpd_req_t *req, const char *status)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, status, HTTPD_RESP_USE_STRLEN);
}

esp_err_t webdav_reply_error_close(httpd_req_t *req, const char *status)
{
    if (req->content_len == 0) {
        /* No body to leave behind, so the connection is still in a clean
         * state -- keep it alive. Tearing down a keep-alive connection for
         * an ordinary error (a 404 probe, say) just makes clients reconnect
         * in a loop. */
        return webdav_reply_error(req, status);
    }

    httpd_resp_set_hdr(req, "Connection", "close");
    webdav_reply_error(req, status);
    /* Returning ESP_FAIL makes esp_http_server close the socket. Required
     * when we answer without having read the whole request body: the unread
     * bytes would otherwise be parsed as the next request on this
     * keep-alive connection, which surfaces as
     * "httpd_parse: parse_block: incomplete (0/N) with parser error = 16"
     * (HPE_INVALID_METHOD) and kills the connection anyway -- but only
     * after corrupting whatever request the client sent next. */
    return ESP_FAIL;
}

/* ---------------------------------------------------------------------- */
/* GET / HEAD                                                              */
/* ---------------------------------------------------------------------- */

static esp_err_t send_directory_listing(httpd_req_t *req, const char *fs_path)
{
    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_send_chunk(req, "<!DOCTYPE html><html><body><ul>",
                                           HTTPD_RESP_USE_STRLEN);

    DIR *dir = (err == ESP_OK) ? opendir(fs_path) : NULL;
    if (dir) {
        struct dirent *ent;
        char child[WEBDAV_MAX_PATH_LEN];
        char enc[WEBDAV_MAX_PATH_LEN];
        while (err == ESP_OK && (ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            int n = snprintf(child, sizeof(child), "%s/%s", fs_path, ent->d_name);
            bool is_dir = (n > 0 && (size_t)n < sizeof(child)) ? webdav_is_dir(child) : false;

            err = httpd_resp_send_chunk(req, "<li><a href=\"", HTTPD_RESP_USE_STRLEN);
            if (err == ESP_OK && webdav_url_encode_path(ent->d_name, enc, sizeof(enc))) {
                err = httpd_resp_send_chunk(req, enc, HTTPD_RESP_USE_STRLEN);
            }
            if (err == ESP_OK && is_dir) {
                err = httpd_resp_send_chunk(req, "/", HTTPD_RESP_USE_STRLEN);
            }
            if (err == ESP_OK) {
                err = httpd_resp_send_chunk(req, "\">", HTTPD_RESP_USE_STRLEN);
            }
            if (err == ESP_OK) {
                err = webdav_send_xml_escaped(req, ent->d_name);
            }
            if (err == ESP_OK) {
                err = httpd_resp_send_chunk(req, is_dir ? "/</a></li>" : "</a></li>",
                                             HTTPD_RESP_USE_STRLEN);
            }
        }
        closedir(dir);
    }
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_send_chunk(req, "</ul></body></html>", HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t webdav_handle_get(httpd_req_t *req, bool send_body)
{
    struct esp_webdav_server *srv = (struct esp_webdav_server *)req->user_ctx;

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

    const char *name;
    if (S_ISDIR(st.st_mode)) {
        /* A plain browser GET on a directory serves its index.html if
         * present (matching e.g. Apache's mod_dav + DirectoryIndex),
         * falling back to an auto-generated listing otherwise. WebDAV
         * clients never hit this branch -- they list directories via
         * PROPFIND, which is unaffected by an index.html's presence. */
        char index_path[WEBDAV_MAX_PATH_LEN];
        struct stat index_st;
        int n = snprintf(index_path, sizeof(index_path), "%s/index.html", path);
        bool have_index = (n > 0 && (size_t)n < sizeof(index_path) &&
                           stat(index_path, &index_st) == 0 && S_ISREG(index_st.st_mode));
        if (!have_index) {
            if (!send_body) {
                httpd_resp_set_type(req, "text/html");
                return httpd_resp_send(req, NULL, 0);
            }
            return send_directory_listing(req, path);
        }
        memcpy(path, index_path, (size_t)n + 1);
        st = index_st;
        name = "index.html";
    } else {
        name = strrchr(rel, '/');
        name = name ? name + 1 : rel;
    }
    httpd_resp_set_type(req, webdav_guess_mime_type(name));

    if (!send_body) {
        /* Deliberately no Content-Length here, even though a HEAD response
         * ought to carry the size the GET would return. httpd_resp_send()
         * unconditionally writes its own "Content-Length: <buf_len>" ahead
         * of any header set via httpd_resp_set_hdr(), so adding one would
         * put two conflicting Content-Length values in the response -- which
         * RFC 7230 3.3.3 makes an unrecoverable message that clients must
         * reject. Reporting 0 is inaccurate but correctly framed, and WebDAV
         * clients take the real size from PROPFIND's getcontentlength. */
        return httpd_resp_send(req, NULL, 0);
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return webdav_reply_error(req, "404 Not Found");
    }

    char *buf = malloc(WEBDAV_IO_BUF_SIZE);
    if (!buf) {
        fclose(f);
        return webdav_reply_error(req, "500 Internal Server Error");
    }

    esp_err_t send_err = ESP_OK;
    size_t n;
    while ((n = fread(buf, 1, WEBDAV_IO_BUF_SIZE, f)) > 0) {
        send_err = httpd_resp_send_chunk(req, buf, n);
        if (send_err != ESP_OK) {
            break;
        }
    }
    free(buf);
    fclose(f);
    if (send_err != ESP_OK) {
        return send_err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---------------------------------------------------------------------- */
/* PUT                                                                      */
/*                                                                          */
/* httpd_req_recv() refuses to read past req->content_len, which is        */
/* always 0 for a request sent with "Transfer-Encoding: chunked" instead   */
/* of Content-Length (e.g. macOS's built-in WebDAV client for some         */
/* uploads). Worse, esp_http_server's own header parser may already have   */
/* consumed some leading body bytes into a private buffer we have no       */
/* access to, so there is no correct way to read a chunked body from a     */
/* URI handler at all -- not even by going around httpd_req_recv() via the */
/* raw socket. Reject it cleanly instead of hanging or silently writing a  */
/* truncated file.                                                         */
/* ---------------------------------------------------------------------- */

static bool webdav_is_chunked_request(httpd_req_t *req)
{
    char te[32];
    if (httpd_req_get_hdr_value_str(req, "Transfer-Encoding", te, sizeof(te)) != ESP_OK) {
        return false;
    }
    for (char *p = te; *p != '\0'; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    return strstr(te, "chunked") != NULL;
}

esp_err_t webdav_handle_put(httpd_req_t *req)
{
    struct esp_webdav_server *srv = (struct esp_webdav_server *)req->user_ctx;

    /* Every early return below answers before the body has been read, so all
     * of them must close the connection rather than leave unread body bytes
     * queued up to be mis-parsed as the next request. */
    if (srv->read_only) {
        return webdav_reply_error_close(req, "403 Forbidden");
    }

    if (webdav_is_chunked_request(req) ||
        (req->content_len == 0 && httpd_req_get_hdr_value_len(req, "Content-Length") == 0)) {
        /* Chunked, or neither Content-Length nor chunked -- either way we
         * can't reliably read the body. */
        return webdav_reply_error_close(req, "411 Length Required");
    }

    char path[WEBDAV_MAX_PATH_LEN];
    esp_err_t err = webdav_resolve_path(srv, req, path, sizeof(path), NULL);
    if (err != ESP_OK) {
        return webdav_reply_error_close(req, "400 Bad Request");
    }

    if (webdav_is_dir(path)) {
        return webdav_reply_error_close(req, "409 Conflict");
    }
    bool existed = (access(path, F_OK) == 0);

    FILE *f = fopen(path, "wb");
    if (!f) {
        /* Most likely the parent collection doesn't exist. */
        return webdav_reply_error_close(req, "409 Conflict");
    }

    char *buf = malloc(WEBDAV_IO_BUF_SIZE);
    if (!buf) {
        fclose(f);
        unlink(path);
        return webdav_reply_error_close(req, "500 Internal Server Error");
    }

    esp_err_t io_err = ESP_OK;
    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t want = remaining < WEBDAV_IO_BUF_SIZE ? remaining : WEBDAV_IO_BUF_SIZE;
        int r = webdav_recv_body(req, buf, want);
        if (r <= 0) {
            io_err = (r == HTTPD_SOCK_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : ESP_FAIL;
            break;
        }
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
            io_err = ESP_FAIL;
            break;
        }
        remaining -= (size_t)r;
    }
    free(buf);
    fclose(f);

    if (io_err != ESP_OK) {
        /* Either way the body was left partly unread, so the connection has
         * to go: see webdav_reply_error_close(). */
        unlink(path);
        if (io_err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "PUT %s timed out with %u of %u bytes left unread", path,
                     (unsigned)remaining, (unsigned)req->content_len);
            return webdav_reply_error_close(req, "408 Request Timeout");
        }
        return webdav_reply_error_close(req, "500 Internal Server Error");
    }

    httpd_resp_set_status(req, existed ? "204 No Content" : "201 Created");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------------------------------------------------------------------- */
/* DELETE                                                                   */
/* ---------------------------------------------------------------------- */

esp_err_t webdav_handle_delete(httpd_req_t *req)
{
    struct esp_webdav_server *srv = (struct esp_webdav_server *)req->user_ctx;
    if (srv->read_only) {
        return webdav_reply_error(req, "403 Forbidden");
    }

    char path[WEBDAV_MAX_PATH_LEN];
    esp_err_t err = webdav_resolve_path(srv, req, path, sizeof(path), NULL);
    if (err != ESP_OK) {
        return webdav_reply_error(req, "400 Bad Request");
    }

    if (strcmp(path, srv->root_path) == 0) {
        return webdav_reply_error(req, "403 Forbidden");
    }

    err = webdav_remove_recursive(path);
    if (err == ESP_ERR_NOT_FOUND) {
        return webdav_reply_error(req, "404 Not Found");
    }
    if (err != ESP_OK) {
        return webdav_reply_error(req, "500 Internal Server Error");
    }

    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------------------------------------------------------------------- */
/* MKCOL                                                                    */
/* ---------------------------------------------------------------------- */

esp_err_t webdav_handle_mkcol(httpd_req_t *req)
{
    struct esp_webdav_server *srv = (struct esp_webdav_server *)req->user_ctx;
    if (srv->read_only) {
        return webdav_reply_error(req, "403 Forbidden");
    }

    if (req->content_len > 0) {
        /* Extended MKCOL (RFC 5689) with a request body isn't supported.
         * Close rather than leave that unread body on the connection. */
        return webdav_reply_error_close(req, "415 Unsupported Media Type");
    }

    char path[WEBDAV_MAX_PATH_LEN];
    esp_err_t err = webdav_resolve_path(srv, req, path, sizeof(path), NULL);
    if (err != ESP_OK) {
        return webdav_reply_error(req, "400 Bad Request");
    }

    if (mkdir(path, 0755) != 0) {
        return webdav_reply_error(req, errno == EEXIST ? "405 Method Not Allowed" : "409 Conflict");
    }

    httpd_resp_set_status(req, "201 Created");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------------------------------------------------------------------- */
/* OPTIONS                                                                   */
/* ---------------------------------------------------------------------- */

esp_err_t webdav_handle_options(httpd_req_t *req)
{
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "DAV", "1, 2");
    httpd_resp_set_hdr(req, "MS-Author-Via", "DAV");
    httpd_resp_set_hdr(req, "Allow",
                        "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, COPY, MOVE, "
                        "PROPFIND, PROPPATCH, LOCK, UNLOCK");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------------------------------------------------------------------- */
/* LOCK / UNLOCK                                                             */
/*                                                                          */
/* No real lock table is kept -- a single embedded device serving one or a  */
/* handful of trusted clients has little to gain from enforcing exclusive   */
/* locks, and RFC 4918 permits advertising class "2" (locking) support      */
/* while granting every LOCK request rather than truly serializing access.  */
/* This still satisfies clients (older Windows/macOS WebDAV stacks) that    */
/* refuse to edit a file without first obtaining a lock.                    */
/* ---------------------------------------------------------------------- */

esp_err_t webdav_handle_lock(httpd_req_t *req)
{
    struct esp_webdav_server *srv = (struct esp_webdav_server *)req->user_ctx;

    /* Drain the lock request's XML body before anything that can fail, so
     * the error paths below can answer without dropping the connection. */
    char discard[128];
    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t want = remaining < sizeof(discard) ? remaining : sizeof(discard);
        int r = webdav_recv_body(req, discard, want);
        if (r <= 0) {
            return webdav_reply_error_close(req, "400 Bad Request");
        }
        remaining -= (size_t)r;
    }

    if (srv->read_only) {
        return webdav_reply_error(req, "403 Forbidden");
    }

    char token[48];
    snprintf(token, sizeof(token), "opaquelocktoken:%08lx-%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    char token_hdr[52];
    snprintf(token_hdr, sizeof(token_hdr), "<%s>", token);

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    httpd_resp_set_hdr(req, "Lock-Token", token_hdr);

    char body[400];
    int n = snprintf(body, sizeof(body),
                      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>"
                      "<D:locktype><D:write/></D:locktype>"
                      "<D:lockscope><D:exclusive/></D:lockscope>"
                      "<D:depth>infinity</D:depth>"
                      "<D:timeout>Second-600</D:timeout>"
                      "<D:locktoken><D:href>%s</D:href></D:locktoken>"
                      "</D:activelock></D:lockdiscovery></D:prop>",
                      token);
    return httpd_resp_send(req, body, n);
}

esp_err_t webdav_handle_unlock(httpd_req_t *req)
{
    struct esp_webdav_server *srv = (struct esp_webdav_server *)req->user_ctx;
    if (srv->read_only) {
        return webdav_reply_error(req, "403 Forbidden");
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------------------------------------------------------------------- */
/* Dispatch + lifecycle                                                     */
/* ---------------------------------------------------------------------- */

static esp_err_t webdav_dispatch(httpd_req_t *req)
{
    switch (req->method) {
        case HTTP_GET:       return webdav_handle_get(req, true);
        case HTTP_HEAD:      return webdav_handle_get(req, false);
        case HTTP_PUT:       return webdav_handle_put(req);
        case HTTP_DELETE:    return webdav_handle_delete(req);
        case HTTP_MKCOL:     return webdav_handle_mkcol(req);
        case HTTP_PROPFIND:  return webdav_handle_propfind(req);
        case HTTP_PROPPATCH: return webdav_handle_proppatch(req);
        case HTTP_MOVE:      return webdav_handle_move(req);
        case HTTP_COPY:      return webdav_handle_copy(req);
        case HTTP_OPTIONS:   return webdav_handle_options(req);
        case HTTP_LOCK:      return webdav_handle_lock(req);
        case HTTP_UNLOCK:    return webdav_handle_unlock(req);
        default:             return webdav_reply_error(req, "405 Method Not Allowed");
    }
}

esp_err_t esp_webdav_start(const esp_webdav_config_t *config, esp_webdav_handle_t *out_handle)
{
    if (!config || !config->root_path || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat root_st;
    if (stat(config->root_path, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        ESP_LOGE(TAG, "root_path '%s' is not a mounted directory", config->root_path);
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(config->root_path, "/") == 0) {
        /* "/" would make path concatenation ambiguous (and unintentionally
         * expose the whole VFS namespace, not just one mounted filesystem).
         * Point root_path at the actual mount point, e.g. "/littlefs". */
        ESP_LOGE(TAG, "root_path must be a mounted subdirectory, not \"/\"");
        return ESP_ERR_INVALID_ARG;
    }

    struct esp_webdav_server *srv = calloc(1, sizeof(*srv));
    if (!srv) {
        return ESP_ERR_NO_MEM;
    }

    size_t root_len = strlen(config->root_path);
    while (root_len > 1 && config->root_path[root_len - 1] == '/') {
        root_len--;
    }
    if (root_len >= sizeof(srv->root_path)) {
        free(srv);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(srv->root_path, config->root_path, root_len);
    srv->root_path[root_len] = '\0';
    srv->root_len = root_len;

    const char *prefix = config->uri_prefix ? config->uri_prefix : "";
    size_t prefix_len = strlen(prefix);
    while (prefix_len > 0 && prefix[prefix_len - 1] == '/') {
        prefix_len--;
    }
    if (prefix_len >= sizeof(srv->uri_prefix) || (prefix_len > 0 && prefix[0] != '/')) {
        free(srv);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(srv->uri_prefix, prefix, prefix_len);
    srv->uri_prefix[prefix_len] = '\0';
    srv->prefix_len = prefix_len;

    srv->read_only = config->read_only;

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.server_port = config->server_port ? config->server_port : 80;
    httpd_cfg.max_open_sockets = config->max_clients ? config->max_clients : 7;
    httpd_cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_cfg.stack_size = CONFIG_ESP_WEBDAV_HTTPD_STACK_SIZE;

    /* Without this, a full socket table makes esp_http_server accept() a new
     * connection and then immediately close() it -- the client just sees a
     * TCP reset with no HTTP response. That matters here because WebDAV
     * clients open several parallel connections per mount, and a peer that
     * disappears (sleeping laptop, dropped WiFi) holds its slot with no idle
     * timeout to reclaim it, so the table fills up and stays full until the
     * device is rebooted. Evicting the least-recently-used connection instead
     * makes that self-healing. */
    httpd_cfg.lru_purge_enable = true;

#if CONFIG_ESP_WEBDAV_DEBUG_CONNECTIONS
    httpd_cfg.open_fn = webdav_debug_open;
#endif

    esp_err_t err = httpd_start(&srv->httpd, &httpd_cfg);
    if (err != ESP_OK) {
        free(srv);
        return err;
    }

    httpd_uri_t uri = {
        .uri = "/*",
        .method = HTTP_ANY,
        .handler = webdav_dispatch,
        .user_ctx = srv,
    };
    err = httpd_register_uri_handler(srv->httpd, &uri);
    if (err != ESP_OK) {
        httpd_stop(srv->httpd);
        free(srv);
        return err;
    }

    ESP_LOGI(TAG, "WebDAV server started: root=%s prefix=%s port=%u%s", srv->root_path,
             srv->prefix_len ? srv->uri_prefix : "/", (unsigned)httpd_cfg.server_port,
             srv->read_only ? " (read-only)" : "");

    *out_handle = srv;
    return ESP_OK;
}

esp_err_t esp_webdav_stop(esp_webdav_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = httpd_stop(handle->httpd);
    free(handle);
    return err;
}

httpd_handle_t esp_webdav_get_httpd_handle(esp_webdav_handle_t handle)
{
    return handle ? handle->httpd : NULL;
}
