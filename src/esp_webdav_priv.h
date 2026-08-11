#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "sdkconfig.h"

#include "esp_webdav.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEBDAV_MAX_PATH_LEN    CONFIG_ESP_WEBDAV_MAX_PATH_LEN
#define WEBDAV_IO_BUF_SIZE     CONFIG_ESP_WEBDAV_IO_BUF_SIZE
#define WEBDAV_MAX_PREFIX_LEN  64
#define WEBDAV_BODY_TIMEOUT_MS (CONFIG_ESP_WEBDAV_BODY_TIMEOUT_S * 1000)

struct esp_webdav_server {
    httpd_handle_t httpd;
    char root_path[WEBDAV_MAX_PATH_LEN];
    size_t root_len;
    char uri_prefix[WEBDAV_MAX_PREFIX_LEN]; /* normalized: "" or "/xxx" (no trailing slash) */
    size_t prefix_len;
    bool read_only;
};

/* ---- esp_webdav_util.c ---------------------------------------------- */

/*
 * Resolve the request's decoded, sanitized URI onto a filesystem path under
 * srv->root_path, writing the NUL-terminated result into out (size out_size).
 * Also strips any query string and the configured uri_prefix.
 *
 * On success returns ESP_OK and *out_rel is set to point at the path portion
 * of `out` that is relative to root_path (starts with '/', "" for root).
 * Fails with ESP_ERR_INVALID_ARG (bad/unsafe URI) or ESP_ERR_INVALID_SIZE
 * (would overflow out_size / WEBDAV_MAX_PATH_LEN).
 */
esp_err_t webdav_resolve_path(const struct esp_webdav_server *srv, httpd_req_t *req,
                               char *out, size_t out_size, const char **out_rel);

/*
 * Same canonicalization/prefix-stripping as webdav_resolve_path(), but for an
 * arbitrary already-decoded-or-not URI path string (used for the Destination
 * header of MOVE/COPY, which isn't req->uri). `uri_path` must not include a
 * query string.
 */
esp_err_t webdav_resolve_uri_path(const struct esp_webdav_server *srv, const char *uri_path,
                                   char *out, size_t out_size, const char **out_rel);

/*
 * Receive request-body data like httpd_req_recv(), but bounded.
 *
 * esp_http_server maps a socket recv timeout (SO_RCVTIMEO, 5s by default)
 * to HTTPD_SOCK_ERR_TIMEOUT. Retrying that is correct for a briefly stalled
 * client, but retrying it forever wedges the single esp_http_server task and
 * with it the entire server -- so retries stop once the client has been
 * silent for WEBDAV_BODY_TIMEOUT_MS and HTTPD_SOCK_ERR_TIMEOUT is returned
 * for good. Any other non-positive return is a fatal socket error, as with
 * httpd_req_recv(). Callers should close the connection on either, since a
 * partially-read body would otherwise be parsed as the next request.
 */
int webdav_recv_body(httpd_req_t *req, char *buf, size_t len);

/* ---- esp_webdav_macos_put.c ------------------------------------------- */

/*
 * Session recv override that turns macOS Finder's chunked upload into an
 * ordinary Content-Length PUT before esp_http_server ever parses it (see the
 * comment at the top of esp_webdav_macos_put.c). Transparent to every other
 * request. Install with httpd_sess_set_recv_override(); call
 * webdav_macos_put_forget() when the socket closes so the per-connection
 * state is released.
 */
int webdav_macos_put_recv(httpd_handle_t hd, int sockfd, char *buf, size_t buf_len, int flags);
void webdav_macos_put_forget(int fd);

/* Percent-decode `src` (NUL-terminated) into dst (size dst_size). Returns
 * false if the result would not fit or a %XX escape is malformed. */
bool webdav_url_decode(const char *src, char *dst, size_t dst_size);

/* Percent-encode `src` (NUL-terminated path, '/' left unescaped) into dst
 * (size dst_size). Returns false if it would not fit. */
bool webdav_url_encode_path(const char *src, char *dst, size_t dst_size);

/* Append srv->uri_prefix + percent-encoded `rel_path` (already filesystem-
 * relative, starting with '/') into dst as an absolute href. Returns false
 * on overflow. `is_dir` appends a trailing '/' if not already present. */
bool webdav_build_href(const struct esp_webdav_server *srv, const char *rel_path,
                        bool is_dir, char *dst, size_t dst_size);

/* Best-effort MIME type for a file name, based on its extension. */
const char *webdav_guess_mime_type(const char *filename);

/* Format `mtime` as an RFC 1123 HTTP-date into dst (size dst_size, needs >=30). */
void webdav_format_http_date(time_t mtime, char *dst, size_t dst_size);

/* Format `mtime` as an ISO-8601 date (RFC 3339) into dst, for
 * DAV:creationdate / DAV:getlastmodified-adjacent use. dst_size needs >=21. */
void webdav_format_iso8601(time_t mtime, char *dst, size_t dst_size);

/* Write an XML-escaped copy of `src` via httpd_resp_send_chunk. */
esp_err_t webdav_send_xml_escaped(httpd_req_t *req, const char *src);

/* Recursively delete a file or directory at `path`. */
esp_err_t webdav_remove_recursive(const char *path);

/* true if `path` refers to a directory (via stat()). */
bool webdav_is_dir(const char *path);

/* ---- esp_webdav.c: method handlers shared internally ----------------- */

esp_err_t webdav_handle_get(httpd_req_t *req, bool send_body);
esp_err_t webdav_handle_put(httpd_req_t *req);
esp_err_t webdav_handle_delete(httpd_req_t *req);
esp_err_t webdav_handle_mkcol(httpd_req_t *req);
esp_err_t webdav_handle_options(httpd_req_t *req);
esp_err_t webdav_handle_lock(httpd_req_t *req);
esp_err_t webdav_handle_unlock(httpd_req_t *req);

/* Reply with a minimal plain-text error body and the given status line,
 * e.g. webdav_reply_error(req, "404 Not Found"). Only safe once the request
 * body has been fully read (or there wasn't one) -- otherwise use
 * webdav_reply_error_close(). */
esp_err_t webdav_reply_error(httpd_req_t *req, const char *status);

/* Same, but closes the connection afterwards (always returns ESP_FAIL).
 * Use for every early return that answers a request whose body has not been
 * read, so the unread bytes can't be mis-parsed as the next request. */
esp_err_t webdav_reply_error_close(httpd_req_t *req, const char *status);

/* ---- esp_webdav_propfind.c -------------------------------------------- */

esp_err_t webdav_handle_propfind(httpd_req_t *req);
esp_err_t webdav_handle_proppatch(httpd_req_t *req);

/* ---- esp_webdav_copy_move.c -------------------------------------------- */

esp_err_t webdav_handle_move(httpd_req_t *req);
esp_err_t webdav_handle_copy(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
