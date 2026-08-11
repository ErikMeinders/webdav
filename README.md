# webdav

A WebDAV server component for ESP-IDF, built on `esp_http_server`. It exposes
a mounted VFS directory (LittleFS, SPIFFS, FATFS, ...) as a WebDAV share, so
it can be mounted as a network drive on macOS (Finder, Mountain Duck,
Cyberduck), Windows (Explorer), or Linux (GVfs/`davfs2`).

## Features

- `GET` / `HEAD` — download files, browse directories (serves a directory's
  `index.html` if present, otherwise renders a plain HTML index — for a
  regular web browser; WebDAV clients list directories via `PROPFIND`
  instead and are unaffected either way)
- Byte-range `GET` (`206 Partial Content`) — needed by macOS, which reads
  files in ~32 KB ranges rather than whole; also what makes resumable
  downloads and media seeking work
- `PUT` — upload/overwrite files, streamed to flash in chunks (no
  whole-file buffering)
- `DELETE` — recursive delete of files and directories
- `MKCOL` — create directories
- `MOVE` / `COPY` — rename/move and copy, including recursive directory
  copies and `Depth`/`Overwrite` header handling
- `PROPFIND` — directory listing metadata (`Depth: 0`, `1`, `infinity`),
  streamed so large directory trees don't need to fit in RAM at once
- `PROPPATCH`, `LOCK`, `UNLOCK` — enough of RFC 4918's locking dance for
  clients that insist on it before they'll write a file; not a real lock
  manager (see [Limitations](#limitations))
- **macOS Finder uploads** — Finder's client (`User-Agent: WebDAVFS`) uploads
  with `Transfer-Encoding: chunked` plus `X-Expected-Entity-Length`, which
  `esp_http_server` cannot deliver to a handler at all (it reports a chunked
  request's length as 0, and `httpd_req_recv()` clamps every read to that).
  A transport shim rewrites such a request into an ordinary `Content-Length`
  `PUT` and de-chunks the body before the server parses it, so Finder works
  as a read-write client. Verified on macOS 26 and 27.
- Path sanitization against directory traversal (`..` is rejected outright)
- Optional read-only mode

## Requirements

- ESP-IDF >= 5.0
- A VFS-mounted filesystem to serve (LittleFS via
  [`joltwallet/littlefs`](https://components.espressif.com/components/joltwallet/littlefs)
  is what the example uses; SPIFFS/FATFS work too since this component only
  talks to the mount point through standard POSIX file calls)

## Usage

Add it as a dependency via the [ESP Component Registry](https://components.espressif.com/components/erikmeinders/webdav):

```sh
idf.py add-dependency "erikmeinders/webdav^0.2.0"
```

or add it to your project's `idf_component.yml` directly:

```yaml
dependencies:
  erikmeinders/webdav: "^0.2.0"
```

For local development against an unpublished/modified copy, point
`EXTRA_COMPONENT_DIRS` at a checkout of this repo instead, or copy it into
your project's `components/` directory.

Then:

```c
#include "esp_webdav.h"

esp_webdav_config_t config = ESP_WEBDAV_CONFIG_DEFAULT();
config.root_path = "/littlefs";   // must already be mounted
// config.read_only = true;       // optional: expose for download only
// config.server_port = 8080;     // optional: default is 80

esp_webdav_handle_t webdav;
ESP_ERROR_CHECK(esp_webdav_start(&config, &webdav));

// ... later, if needed:
// esp_webdav_stop(webdav);
```

See `include/esp_webdav.h` for the full config struct and
`examples/littlefs_webdav` for a complete, flashable project (Wi-Fi + LittleFS
+ mDNS + WebDAV).

The example advertises the share over mDNS as `_webdav._tcp` and `_http._tcp`
(both with a `path=/` TXT record), plus a TXT-only `_device-info._tcp` on
port 0 carrying `model=Xserve`, which is what makes macOS draw a server icon
for it in Finder's Network view.

## Configuration (Kconfig)

Run `idf.py menuconfig` → `ESP WebDAV Server` to adjust:

- `ESP_WEBDAV_MAX_PATH_LEN` — stack buffer size for resolved filesystem
  paths (default 600)
- `ESP_WEBDAV_IO_BUF_SIZE` — chunk size for file transfers (default 4096)
- `ESP_WEBDAV_ALLOW_DEPTH_INFINITY` — whether `PROPFIND` may recurse an
  entire subtree in one request (default on)
- `ESP_WEBDAV_HTTPD_STACK_SIZE` — stack for the `esp_http_server` task
  (default 8192)
- `ESP_WEBDAV_BODY_TIMEOUT_S` — how long to wait on a client that stops
  sending a request body before returning `408` (default 30)
- `ESP_WEBDAV_MAX_DEPTH` — recursion limit for `PROPFIND`/`COPY`/`DELETE`
  (default 64)
- `ESP_WEBDAV_DEBUG_CONNECTIONS` — log peers and hex-dump traffic; verbose,
  and it logs `Authorization` headers, so leave it off in production
  (default off)

## Limitations

- **No authentication.** Anyone who can reach the device on the network can
  read and write its filesystem. Put it behind a VPN or a network you
  control; don't expose it to the internet as-is.
- **No TLS.** Plain HTTP only (`esp_http_server`, not `esp_https_server`).
- **LOCK/UNLOCK don't enforce anything.** Every `LOCK` request succeeds and
  hands back a token; there's no server-side lock table serializing access.
  This is normally fine for a device with one or a handful of trusted
  clients, but two clients editing the same file at once will race.
- **`PROPPATCH` always fails.** There's nowhere on a plain filesystem to
  durably store arbitrary WebDAV dead properties, so custom property writes
  are rejected (`403`) rather than silently discarded on next boot.
- **`PROPFIND` ignores the requested property list** and always returns the
  standard set (`displayname`, `resourcetype`, `getcontentlength`,
  `getcontenttype`, `getetag`, `creationdate`, `getlastmodified`). Extra
  properties in a response are harmless per RFC 4918 and every WebDAV client
  this was tested against (Finder, Mountain Duck, Cyberduck) is fine with it.
- Designed for a handful of concurrent clients on an embedded device, not as
  a general-purpose file server — directory listings are not paginated, and
  byte-range `GET` handles a single range per request (a multi-range request
  is answered with the whole entity, which RFC 7233 permits).
- A chunked `PUT` is only accepted when it also carries
  `X-Expected-Entity-Length` (see below); any other chunked upload is
  refused with `411 Length Required`, since `esp_http_server` cannot report
  a length for it.

## License

MIT — see [LICENSE](LICENSE).
