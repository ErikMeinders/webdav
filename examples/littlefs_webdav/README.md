# littlefs_webdav example

Provisions Wi-Fi, mounts a LittleFS partition, and serves it over WebDAV on
port 80 — mountable as a network drive from a Mac or Windows machine.

## Hardware

Any ESP32 (or S2/S3/C3/C6/...) board with at least ~3 MB of free flash for
the `storage` LittleFS partition (see `partitions.csv` — adjust the offsets/
sizes there if your board's flash is smaller).

## Build, flash, monitor

```sh
idf.py set-target esp32   # or esp32s3, esp32c3, ...
idf.py build flash monitor
```

The first boot formats the `storage` partition (unless it was already
flashed with the seed content from `flash_data/` via
`littlefs_create_partition_image`, which happens automatically as part of
`idf.py flash`).

## Configure Wi-Fi (first boot)

There's no hardcoded SSID/password to edit. On first boot (or whenever no
Wi-Fi credentials are stored yet), the device opens an access point —
[esp-idf-wifi-provisioner](https://components.espressif.com/components/michmich/esp-idf-wifi-provisioner)
— and serves a captive-portal setup page:

1. Watch the serial monitor for `Captive portal started`.
2. From your phone/laptop, connect to the `esp32-webdav-setup` Wi-Fi network
   (open, no password by default).
3. A captive-portal page should pop up automatically; if not, browse to
   `http://192.168.4.1/`.
4. Enter your Wi-Fi SSID/password. The device stores them in NVS, connects
   as a station, and drops the setup AP.

Credentials persist across reboots — the portal only reappears if they're
erased (`wifi_prov_erase_credentials()`) or the stored network can't be
reached. To reconfigure a device already on your network, erase its NVS
(`idf.py erase-flash`) or wire up `wifi_prov_erase_credentials()` behind a
button/command.

Watch the serial monitor for the assigned IP address and mDNS hostname:

```
I (xxxx) webdav_example: Got IP: 192.168.1.42
I (xxxx) webdav_example: WebDAV server ready.
I (xxxx) webdav_example:   Mac Finder:   Go > Connect to Server... > http://esp32-webdav.local/
I (xxxx) webdav_example:   Mountain Duck: New Bookmark > WebDAV (HTTP) > Server: esp32-webdav.local, Port: 80
```

## Connect

**macOS Finder:** `Go` menu → `Connect to Server…` → `http://esp32-webdav.local/`
(or `http://<device-ip>/` if mDNS/`.local` resolution isn't working on your
network).

**Mountain Duck:** `+` → `WebDAV (HTTP)` → Server `esp32-webdav.local`,
Port `80`, leave username/password blank (no auth is configured).

**Windows Explorer:** `Map network drive…` → `http://esp32-webdav.local/`
(Windows may need the [WebClient service](https://learn.microsoft.com/troubleshoot/windows-server/networking/webclient-service-disabled-by-default)
running, and by default refuses non-HTTPS WebDAV over anything but
`localhost` — see `BasicAuthLevel` in the Windows WebDAV docs if you plan to
add authentication later).

Once mounted, you should see `welcome.txt` and a `notes/` folder — both are
seeded onto the LittleFS image at build time from this example's
`flash_data/` directory. Try creating, renaming, moving and deleting files
and folders from the client; everything is written straight to the device's
flash over WebDAV PUT/DELETE/MKCOL/MOVE/COPY.

## Notes

- The device advertises itself over mDNS as `esp32-webdav.local` with three
  records: `_webdav._tcp` and `_http._tcp` (both carrying `path=/`), plus a
  TXT-only `_device-info._tcp` on port 0 with `model=Xserve`. That last one
  is what makes macOS draw a server icon for the share in Finder's Network
  view instead of a generic box; nothing ever connects to it.
- No authentication is configured — anyone on the network who can reach the
  device can read/write its filesystem. Fine for a LAN demo; put it behind a
  VPN, an `esp_https_server`/reverse-proxy with auth, or a closed
  provisioning network before using this on anything untrusted.
- `esp_webdav_config_t.read_only = true` turns the whole mount read-only if
  you just want to expose files for download.
