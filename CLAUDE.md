# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An ESP-IDF component (`esp_webdav`) that serves a mounted VFS directory
(LittleFS, SPIFFS, FATFS, ...) as a WebDAV share over `esp_http_server`, so
it can be mounted as a network drive from macOS Finder, Mountain Duck,
Cyberduck, or Windows Explorer. The repo root *is* the component (has its
own `CMakeLists.txt`/`Kconfig`/`idf_component.yml`); `examples/littlefs_webdav`
is a complete flashable ESP-IDF project that consumes it.

## Build / run (example project)

All build commands are ESP-IDF (`idf.py`) and must be run from
`examples/littlefs_webdav` (there is nothing to build at the repo root by
itself — it's a library component, pulled in via `EXTRA_COMPONENT_DIRS`
pointing two levels up, see `examples/littlefs_webdav/CMakeLists.txt`):

```bash
cd examples/littlefs_webdav
idf.py set-target esp32                     # or esp32s3, etc.
idf.py build
idf.py -p /dev/tty.usbserial-XXXX flash monitor
```

Wi-Fi is provisioned at runtime, not hardcoded — see "Example project
specifics" below. There is no separate unit test suite in this repo —
verification is by flashing the example and exercising the server with a
real WebDAV client (Finder "Connect to Server" → `http://esp32-webdav.local/`,
or `curl`/`cadaver` against the device's IP).

## Architecture

The component is a single wildcard `esp_http_server` URI handler
(`"/*"`, `HTTP_ANY`) registered in `esp_webdav_start()`
([src/esp_webdav.c](src/esp_webdav.c)) that dispatches on `req->method` to
per-verb handlers. All handlers hang off one opaque
`struct esp_webdav_server` (root path, normalized URI prefix, read-only
flag) stored as `req->user_ctx`, defined in
[src/esp_webdav_priv.h](src/esp_webdav_priv.h) — that header is the map of
the whole component; read it first.

File layout mirrors verb groups:
- [src/esp_webdav.c](src/esp_webdav.c) — lifecycle (`esp_webdav_start`/`_stop`)
  and dispatch, plus GET/HEAD/PUT/DELETE/MKCOL/OPTIONS/LOCK/UNLOCK.
- [src/esp_webdav_propfind.c](src/esp_webdav_propfind.c) — PROPFIND
  (directory listing metadata, streamed) and PROPPATCH (always rejects with
  403 — see Limitations below).
- [src/esp_webdav_copy_move.c](src/esp_webdav_copy_move.c) — MOVE/COPY,
  including recursive directory copies and `Depth`/`Overwrite` header
  handling.
- [src/esp_webdav_util.c](src/esp_webdav_util.c) — shared plumbing: URI
  resolution/sanitization onto filesystem paths, percent-encoding, XML
  escaping, MIME guessing, date formatting, recursive delete.

Every handler is written to avoid buffering whole files/trees in RAM: GET
and PUT stream through a `WEBDAV_IO_BUF_SIZE`-sized heap buffer via
`httpd_resp_send_chunk`/`httpd_req_recv`, and PROPFIND streams XML per-entry
rather than building a full response body.

Path handling is centralized in `webdav_resolve_path()` /
`webdav_resolve_uri_path()` (util.c) — every handler goes through one of
these to turn a request URI (or a MOVE/COPY `Destination` header) into a
sanitized, prefix-stripped filesystem path under `root_path`. `..` traversal
is rejected here; don't bypass this by hand-building paths elsewhere.

Config knobs are `Kconfig` (`ESP_WEBDAV_MAX_PATH_LEN`,
`ESP_WEBDAV_IO_BUF_SIZE`, `ESP_WEBDAV_HTTPD_STACK_SIZE`,
`ESP_WEBDAV_ALLOW_DEPTH_INFINITY`) plus the runtime
`esp_webdav_config_t` in [include/esp_webdav.h](include/esp_webdav.h)
(`root_path`, `uri_prefix`, `server_port`, `max_clients`, `read_only`). The
public API surface is intentionally small — three functions
(`esp_webdav_start`/`_stop`/`_get_httpd_handle`) and that one config struct.

### Known, intentional limitations (don't "fix" without checking README first)

- No authentication, no TLS (plain HTTP only).
- LOCK/UNLOCK don't enforce anything — every LOCK succeeds and hands back a
  token; there's no real lock table. This is deliberate (see comment block
  above `webdav_handle_lock` in esp_webdav.c) — enough for clients that
  refuse to write without locking first, not real concurrency control.
- PROPPATCH always returns 403 — there's nowhere to durably store arbitrary
  WebDAV dead properties on a plain filesystem.
- PROPFIND ignores the requested property list and always returns the fixed
  standard set.
- No byte-range GET, no chunked-`PUT` (requires `Content-Length`), no
  pagination of directory listings.

See [README.md](README.md) for the full, current limitations list — treat
it as authoritative over this file if they ever diverge.

## Example project specifics (`examples/littlefs_webdav`)

[examples/littlefs_webdav/main/app_main.c](examples/littlefs_webdav/main/app_main.c)
wires together, in order: Wi-Fi provisioning via
[esp-idf-wifi-provisioner](https://components.espressif.com/components/michmich/esp-idf-wifi-provisioner)
(`wifi_prov_start()` + `wifi_prov_wait_for_connection()` — reads stored
credentials from NVS and connects, or falls back to an open SoftAP +
captive-portal page at `192.168.4.1` for entering SSID/password on first
boot; also handles NVS/netif/event-loop init internally, disables
modem-sleep power-save once connected since this device only ever responds
to inbound requests), LittleFS mount at `/littlefs` (partition label
`storage`, must match [partitions.csv](examples/littlefs_webdav/partitions.csv)),
mDNS advertisement (`esp32-webdav.local`, `_webdav._tcp` service), then
`esp_webdav_start()`. There are no Wi-Fi credentials in the source tree at
all — nothing to gitignore, nothing to hand-edit before building.

**Don't call `wifi_prov_stop()` after a successful connect** — it calls
`esp_wifi_stop()`/`esp_wifi_deinit()`, which tears down the STA connection
too, not just the provisioning AP/HTTP server. The library already frees
port 80 on its own by the time `wifi_prov_wait_for_connection()` returns
(the captive portal's HTTP server is stopped synchronously in the
credentials-received handler before that call unblocks, and the
stored-credentials fast path never starts one at all), so `esp_webdav_start()`
binding port 80 afterwards is safe without any extra cleanup call.

The example's `managed_components/` (espressif/mdns, joltwallet/littlefs,
michmich/esp-idf-wifi-provisioner) are fetched by the IDF Component Manager
per `examples/littlefs_webdav/main/idf_component.yml` — don't hand-edit
files under `managed_components/`, they're regenerated.
