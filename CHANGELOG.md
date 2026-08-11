# Changelog

All notable changes to this component are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versions correspond to releases of `erikmeinders/webdav` on the
[ESP Component Registry](https://components.espressif.com/components/erikmeinders/webdav).

## 0.2.1 — 2026-08-11

Packaging, metadata and example only — no change to the component's source,
so upgrading from 0.2.0 cannot affect behaviour.

### Added

- Example: advertise `_device-info._tcp` (`model=Xserve`) so macOS Finder
  shows a server icon for the share instead of a generic box. It is a
  TXT-only pseudo-service, advertised on port 0 as Samba does.
- This changelog. The registry renders a component's root `CHANGELOG.md`
  alongside its README (one inside an example directory is ignored).

### Changed

- Packaging: agent instructions (`CLAUDE.md`), editor and IDE config
  (`.clangd`, `.cache/`, `.claude/`, `*.code-workspace`), `.gitignore`,
  `.envrc` and the `compile_commands.json` symlink are no longer included in
  the published archive. `compote` packs from the working tree rather than
  from git, so gitignoring these was not enough to keep them out.
- Dropped the redundant `examples:` key from the manifest — the registry
  already discovers `examples/` recursively, and naming a path already under
  it risks a collision rename.
- Declared a maintainer in the manifest.

## 0.2.0 — 2026-08-11

### Added

- macOS Finder uploads. Finder sends `Transfer-Encoding: Chunked` with
  `X-Expected-Entity-Length`, which `esp_http_server` cannot deliver to a
  handler; a session `recv` override now rewrites such a request into an
  ordinary `Content-Length` PUT and de-chunks the body below the server.
  Chunked PUTs without that header still get 411.
- Byte-range GET (`206 Partial Content`), one range per request.
- `ESP_WEBDAV_MAX_DEPTH` (default 64) as a backstop on recursive walks.
- Example: `_http._tcp` advertised alongside `_webdav._tcp`, and the
  conventional `path` TXT key added to the `_webdav._tcp` record.

### Fixed

- MOVE/COPY now reject a destination equal to, or inside, the source (403,
  per RFC 4918 9.8.5/9.9.4). Previously `COPY` onto itself deleted the
  destination first — which *is* the source — destroying the file, and
  copying a collection into its own subtree recursed into the copy it was
  creating until the stack ran out.
- Recursive walks (`Depth: infinity` PROPFIND, COPY, DELETE) extend and
  restore one shared path buffer instead of placing per-level buffers on the
  stack. The old cost was ~1.25 KB per level against an 8 KB task stack, so
  trees deeper than about five levels overflowed it.
- PUT checks `fclose()`. Buffered stdio flushes the tail there, so write
  errors (`ENOSPC` above all) were discarded and truncated uploads were
  reported as `201 Created`.
- `Connection: close` is honoured. `esp_http_server` ignores it entirely, so
  such a request sat idle until the 5 s timeout emitted a stray 408.
- A stalled upload no longer wedges the whole server (all handlers run in a
  single task).
- Connections stay alive on routine errors, and the duplicate
  `Content-Length` on those responses is gone.
- An error response with an unread request body always closes the connection.
- macOS WebDAVFS uploads are refused outright rather than creating empty
  files (superseded by real support, above).
- Example: reconnect Wi-Fi after a drop.

### Changed

- PROPFIND assembles each `<D:response>` into one buffer and sends it as a
  single chunk, instead of ~8 chunk calls each costing several `send()`
  syscalls — a directory listing was running to hundreds of tiny packets.
- `TCP_NODELAY` set on client sockets (Nagle disabled); responses logged at
  debug level.

## 0.1.0 — 2026-08-10

### Added

- Initial release: WebDAV server component for ESP-IDF serving a mounted VFS
  directory over `esp_http_server`, with GET/HEAD/PUT/DELETE/MKCOL/OPTIONS/
  PROPFIND/PROPPATCH/MOVE/COPY/LOCK/UNLOCK, and a LittleFS example project.
