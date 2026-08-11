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

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#if CONFIG_ESP_WEBDAV_DEBUG_CONNECTIONS

/* Large enough for a full WebDAV request header block -- clients send long
 * URIs plus Host/User-Agent/Content-Length/Depth, and a truncated dump hides
 * exactly the headers you need (Content-Length, Transfer-Encoding,
 * X-Expected-Entity-Length). */
#define WEBDAV_DEBUG_DUMP_MAX 320

/* esp_http_server parses in 128-byte blocks, so one read never covers a whole
 * WebDAV header block. Dump the first few reads of each connection instead of
 * just the first -- enough to see every header -- while still stopping well
 * before an upload body buries the log. Indexed by fd modulo the table size,
 * which is fine for a debug aid: lwip hands out a small, dense fd range. */
#define WEBDAV_DEBUG_SLOTS       64
#define WEBDAV_DEBUG_READS_SHOWN 3
/* Responses are worth more dumps than requests: PROPFIND emits its XML as
 * many small chunks, so the interesting part is spread over several writes. */
#define WEBDAV_DEBUG_SENDS_SHOWN 10
static uint8_t s_debug_reads[WEBDAV_DEBUG_SLOTS];
static uint8_t s_debug_sends[WEBDAV_DEBUG_SLOTS];
#define WEBDAV_DEBUG_SLOT(fd) ((unsigned)(fd) % WEBDAV_DEBUG_SLOTS)

/* Budget for "this doesn't start like an HTTP request" dumps, so that a
 * binary upload (whose body reads also come through here) can't flood the
 * log while still catching a bad request on a *reused* connection, which the
 * first-read dump alone would miss. */
static int s_debug_odd_budget = 10;

/* First byte of every method in http_parser's table. */
static bool webdav_debug_starts_like_method(const char *buf, int len)
{
    return len > 0 && buf[0] != '\0' && strchr("ABCDGHLMNOPRSTU", buf[0]) != NULL;
}

static int webdav_debug_recv(httpd_handle_t hd, int sockfd, char *buf, size_t buf_len, int flags)
{
    (void)hd;
    int ret = recv(sockfd, buf, buf_len, flags);
    if (ret > 0) {
        unsigned slot = WEBDAV_DEBUG_SLOT(sockfd);
        bool early = s_debug_reads[slot] < WEBDAV_DEBUG_READS_SHOWN;
        /* Also dump a read that doesn't begin like an HTTP request even after
         * the early ones, which is how a desynced keep-alive connection shows
         * up -- the first-reads window alone would miss it. Budgeted so a
         * binary upload body can't flood the log. */
        bool odd = !early && !webdav_debug_starts_like_method(buf, ret) && s_debug_odd_budget > 0;
        if (early || odd) {
            if (odd) {
                s_debug_odd_budget--;
            }
            int n = ret < WEBDAV_DEBUG_DUMP_MAX ? ret : WEBDAV_DEBUG_DUMP_MAX;
            ESP_LOGI(TAG, "fd=%d read #%u%s: %d bytes (dumping %d)", sockfd,
                     (unsigned)s_debug_reads[slot] + 1, odd ? " NON-HTTP" : "", ret, n);
            ESP_LOG_BUFFER_HEXDUMP(TAG, buf, n, ESP_LOG_INFO);
        }
        if (s_debug_reads[slot] < 255) {
            s_debug_reads[slot]++;
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

static int webdav_debug_send(httpd_handle_t hd, int sockfd, const char *buf, size_t buf_len,
                             int flags)
{
    (void)hd;
    unsigned slot = WEBDAV_DEBUG_SLOT(sockfd);
    if (s_debug_sends[slot] < WEBDAV_DEBUG_SENDS_SHOWN) {
        int n = buf_len < WEBDAV_DEBUG_DUMP_MAX ? (int)buf_len : WEBDAV_DEBUG_DUMP_MAX;
        ESP_LOGI(TAG, "fd=%d SEND #%u: %u bytes (dumping %d)", sockfd,
                 (unsigned)s_debug_sends[slot] + 1, (unsigned)buf_len, n);
        ESP_LOG_BUFFER_HEXDUMP(TAG, buf, n, ESP_LOG_INFO);
    }
    if (s_debug_sends[slot] < 255) {
        s_debug_sends[slot]++;
    }

    int ret = send(sockfd, buf, buf_len, flags);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return HTTPD_SOCK_ERR_TIMEOUT;
        }
        if (errno == EINVAL || errno == EBADF || errno == EFAULT || errno == ENOTSOCK) {
            return HTTPD_SOCK_ERR_INVALID;
        }
        ESP_LOGW(TAG, "fd=%d send failed after %u writes: errno=%d", sockfd,
                 (unsigned)s_debug_sends[slot], errno);
        return HTTPD_SOCK_ERR_FAIL;
    }
    return ret;
}

static void webdav_debug_attach(httpd_handle_t hd, int sockfd)
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

    s_debug_reads[WEBDAV_DEBUG_SLOT(sockfd)] = 0;
    s_debug_sends[WEBDAV_DEBUG_SLOT(sockfd)] = 0;
    httpd_sess_set_recv_override(hd, sockfd, webdav_debug_recv);
    httpd_sess_set_send_override(hd, sockfd, webdav_debug_send);
}
#endif /* CONFIG_ESP_WEBDAV_DEBUG_CONNECTIONS */

static esp_err_t webdav_socket_open(httpd_handle_t hd, int sockfd)
{
    /* PROPFIND streams its XML as many small chunks, and every chunk is
     * several send() calls (size line, payload, CRLF), so a directory listing
     * can be hundreds of tiny writes. With Nagle enabled the stack holds each
     * one back waiting for an ACK, adding up to ~40ms apiece -- enough that
     * clients time out mid-response and reset the connection. These responses
     * are latency-sensitive and small, so disable it. */
    int one = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
        ESP_LOGW(TAG, "TCP_NODELAY failed on fd=%d: errno=%d", sockfd, errno);
    }

#if CONFIG_ESP_WEBDAV_DEBUG_CONNECTIONS
    webdav_debug_attach(hd, sockfd);
#else
    (void)hd;
#endif
    return ESP_OK;
}

esp_err_t webdav_reply_error(httpd_req_t *req, const char *status)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, status, HTTPD_RESP_USE_STRLEN);
}

esp_err_t webdav_reply_error_close(httpd_req_t *req, const char *status)
{
    /* Deliberately unconditional. An earlier version skipped the close when
     * req->content_len == 0, reasoning that there was then no body left to
     * desync on -- but content_len is *exactly* 0 for the two cases that need
     * closing most: a chunked body, and macOS's "Content-Length: 0 plus
     * X-Expected-Entity-Length" upload. Both do have bytes on the wire, so
     * skipping the close left them to be read as the next request
     * ("parse_block: incomplete (0/N) with parser error = 16", followed by a
     * spurious 400).
     *
     * Callers that just want to report an error on a connection whose body is
     * already consumed should call webdav_reply_error() instead -- which is
     * why PROPFIND and PROPPATCH drain their body up front. */
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

/*
 * Parse a "Range: bytes=..." header against a file of `size` bytes.
 *
 * Returns false when there is no usable Range header, i.e. the caller should
 * send the whole file with 200. Returns true otherwise, with either
 * "unsatisfiable" set (caller sends 416), or out_start and out_end holding an
 * inclusive, clamped byte range.
 */
static bool parse_range_header(httpd_req_t *req, long size, long *out_start, long *out_end,
                                bool *unsatisfiable)
{
    *unsatisfiable = false;

    char hdr[64];
    if (httpd_req_get_hdr_value_str(req, "Range", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    if (strncasecmp(hdr, "bytes=", 6) != 0) {
        return false; /* "bytes" is the only range unit anyone actually uses */
    }
    const char *spec = hdr + 6;
    if (strchr(spec, ',') != NULL) {
        /* Multiple ranges. RFC 7233 explicitly lets a server ignore Range and
         * return the whole entity, which beats assembling a
         * multipart/byteranges response on a microcontroller. */
        return false;
    }
    const char *dash = strchr(spec, '-');
    if (dash == NULL) {
        return false;
    }

    long start, end;
    if (dash == spec) {
        /* "-N": the last N bytes. */
        long n = strtol(dash + 1, NULL, 10);
        if (n <= 0) {
            *unsatisfiable = true;
            return true;
        }
        if (n > size) {
            n = size;
        }
        start = size - n;
        end = size - 1;
    } else {
        start = strtol(spec, NULL, 10);
        end = (dash[1] == '\0') ? size - 1 : strtol(dash + 1, NULL, 10);
    }

    if (start < 0 || start >= size || end < start) {
        *unsatisfiable = true;
        return true;
    }
    if (end >= size) {
        end = size - 1; /* a too-large end is clamped, not an error */
    }
    *out_start = start;
    *out_end = end;
    return true;
}

/* httpd_send() may send less than asked; loop until it's all out. Socket
 * timeouts are retried on the same bounded basis as webdav_recv_body(), so a
 * client that stops reading can't pin the single server task forever. */
static esp_err_t raw_send_all(httpd_req_t *req, const char *buf, size_t len)
{
    size_t off = 0;
    int timeouts = 0;
    const int max_timeouts = (WEBDAV_BODY_TIMEOUT_MS / 5000) + 1;

    while (off < len) {
        int r = httpd_send(req, buf + off, len - off);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > max_timeouts) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;
        }
        timeouts = 0;
        off += (size_t)r;
    }
    return ESP_OK;
}

/*
 * Answer with 206 Partial Content for [start, end].
 *
 * Built with httpd_send() rather than httpd_resp_send*(): a partial response
 * needs an exact Content-Length alongside Content-Range, and
 * httpd_resp_send() would only give that by buffering the whole range in
 * RAM, while httpd_resp_send_chunk() would force Transfer-Encoding: chunked.
 */
static esp_err_t send_file_range(httpd_req_t *req, FILE *f, const char *mime, long start,
                                  long end, long size)
{
    long len = end - start + 1;

    char hdr[224];
    int n = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 206 Partial Content\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %ld\r\n"
                      "Content-Range: bytes %ld-%ld/%ld\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "\r\n",
                      mime, len, start, end, size);
    if (n < 0 || (size_t)n >= sizeof(hdr)) {
        return ESP_FAIL;
    }
    esp_err_t err = raw_send_all(req, hdr, (size_t)n);
    if (err != ESP_OK) {
        return err;
    }

    if (fseek(f, start, SEEK_SET) != 0) {
        return ESP_FAIL; /* headers are already out; all we can do is drop it */
    }

    char *buf = malloc(WEBDAV_IO_BUF_SIZE);
    if (!buf) {
        return ESP_FAIL;
    }

    long remaining = len;
    while (remaining > 0) {
        size_t want = (remaining < WEBDAV_IO_BUF_SIZE) ? (size_t)remaining : WEBDAV_IO_BUF_SIZE;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) {
            err = ESP_FAIL; /* file shrank under us */
            break;
        }
        err = raw_send_all(req, buf, got);
        if (err != ESP_OK) {
            break;
        }
        remaining -= (long)got;
    }
    free(buf);
    return err;
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
    const char *mime = webdav_guess_mime_type(name);
    httpd_resp_set_type(req, mime);
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");

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

    /* macOS's WebDAV client reads files in ranges rather than whole -- without
     * this it gets the entire file for each 32 KB it asked for, then resets
     * the connection, which is both very slow and a failed read. */
    long fsize = (long)st.st_size;
    long rstart = 0, rend = 0;
    bool unsatisfiable = false;
    if (parse_range_header(req, fsize, &rstart, &rend, &unsatisfiable)) {
        if (unsatisfiable) {
            fclose(f);
            /* cr must outlive the send: httpd_resp_set_hdr() stores the
             * pointer, and webdav_reply_error() sends while it's in scope. */
            char cr[48];
            snprintf(cr, sizeof(cr), "bytes */%ld", fsize);
            httpd_resp_set_hdr(req, "Content-Range", cr);
            return webdav_reply_error(req, "416 Range Not Satisfiable");
        }
        esp_err_t range_err = send_file_range(req, f, mime, rstart, rend, fsize);
        fclose(f);
        return range_err;
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

    /* macOS's built-in WebDAV client (User-Agent: WebDAVFS) announces an
     * upload as "Content-Length: 0" plus "X-Expected-Entity-Length: <real
     * size>", and then sends the body anyway. esp_http_server believes the
     * Content-Length, so httpd_req_recv() hands us nothing (it clamps reads
     * to remaining_len, which is 0) while the body piles up on the socket.
     *
     * Treating that as a normal empty PUT is the worst outcome: it creates a
     * 0-byte file, answers 201 Created, and leaves the body to be parsed as
     * the next request -- which is where "parse_block: incomplete (0/N) with
     * parser error = 16" comes from, and why files land empty. We can't read
     * the body either (esp_http_server has already buffered the front of it
     * into a private scratch buffer), so refuse the upload and close. */
    char xlen[24];
    if (req->content_len == 0 &&
        httpd_req_get_hdr_value_str(req, "X-Expected-Entity-Length", xlen, sizeof(xlen)) == ESP_OK &&
        strtoul(xlen, NULL, 10) > 0) {
        ESP_LOGW(TAG, "PUT %s: macOS-style upload (Content-Length: 0 with "
                      "X-Expected-Entity-Length: %s) cannot be read -- refusing",
                 req->uri, xlen);
        httpd_resp_set_hdr(req, "Connection", "close");
        webdav_reply_error(req, "411 Length Required");
        return ESP_FAIL; /* force close: content_len is 0, so the usual
                          * content_len>0 test wouldn't trigger one */
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
    httpd_cfg.open_fn = webdav_socket_open;

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
