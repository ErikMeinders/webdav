#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque handle to a running WebDAV server instance.
 */
typedef struct esp_webdav_server *esp_webdav_handle_t;

/**
 * Configuration for esp_webdav_start().
 */
typedef struct {
    const char *root_path;     /*!< POSIX/VFS directory the server exposes, e.g. "/littlefs".
                                     Must already be mounted before esp_webdav_start() is called. */
    const char *uri_prefix;    /*!< URL path prefix clients must use, e.g. "" for the root of the
                                     server or "/dav" to serve under http://host/dav/. Leave "" for
                                     most WebDAV clients (Mountain Duck, Finder, Cyberduck all
                                     default to mounting the root of the URL). May be NULL, same as "". */
    uint16_t server_port;      /*!< TCP port for the HTTP server. Default 80. */
    uint8_t max_clients;       /*!< Max simultaneous sockets (httpd max_open_sockets). Default 7.
                                     WebDAV clients typically open several parallel connections per
                                     mount, so setting this low makes transfers stall. esp_http_server
                                     needs CONFIG_LWIP_MAX_SOCKETS >= max_clients + 3. */
    bool read_only;            /*!< If true, PUT/DELETE/MKCOL/MOVE/COPY/PROPPATCH/LOCK are rejected
                                     with 403 Forbidden and the server advertises itself read-only. */
} esp_webdav_config_t;

#define ESP_WEBDAV_CONFIG_DEFAULT()   \
    {                                 \
        .root_path = "/littlefs",     \
        .uri_prefix = "",             \
        .server_port = 80,            \
        .max_clients = 7,             \
        .read_only = false,           \
    }

/**
 * Start a WebDAV server that exposes `config->root_path` for read/write access
 * over HTTP. Internally starts (and owns) an esp_http_server instance with a
 * single wildcard URI handler that dispatches GET/HEAD/PUT/DELETE/MKCOL/
 * PROPFIND/PROPPATCH/MOVE/COPY/OPTIONS/LOCK/UNLOCK.
 *
 * @param config     Server configuration. Not retained; may be stack-allocated.
 * @param out_handle Receives the server handle on success.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if config or root_path is
 *         missing, ESP_ERR_INVALID_STATE if root_path does not exist, or an
 *         error from httpd_start().
 */
esp_err_t esp_webdav_start(const esp_webdav_config_t *config, esp_webdav_handle_t *out_handle);

/**
 * Stop a server started with esp_webdav_start() and free its resources.
 */
esp_err_t esp_webdav_stop(esp_webdav_handle_t handle);

/**
 * Access the underlying esp_http_server handle, e.g. to register additional
 * (non-WebDAV) URI handlers such as a status page on the same port.
 */
httpd_handle_t esp_webdav_get_httpd_handle(esp_webdav_handle_t handle);

#ifdef __cplusplus
}
#endif
