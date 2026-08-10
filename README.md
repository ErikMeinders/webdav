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
idf.py add-dependency "erikmeinders/webdav^1.0.0"
```

or add it to your project's `idf_component.yml` directly:

```yaml
dependencies:
  erikmeinders/webdav: "^1.0.0"
```

For local development against an unpublished/modified copy, use
`EXTRA_COMPONENT_DIRS` instead (see `examples/littlefs_webdav/CMakeLists.txt`
for how the bundled example does this against this repo's own root), or copy
this repo into your project's `components/` directory.

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

## Configuration (Kconfig)

Run `idf.py menuconfig` → `ESP WebDAV Server` to adjust:

- `ESP_WEBDAV_MAX_PATH_LEN` — stack buffer size for resolved filesystem
  paths (default 600)
- `ESP_WEBDAV_IO_BUF_SIZE` — chunk size for file transfers (default 4096)
- `ESP_WEBDAV_ALLOW_DEPTH_INFINITY` — whether `PROPFIND` may recurse an
  entire subtree in one request (default on)

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
  a general-purpose file server — there's no byte-range `GET` support, no
  chunked-`PUT` support (requires `Content-Length`), and directory listings
  are not paginated.

## License

MIT — see [LICENSE](LICENSE).
