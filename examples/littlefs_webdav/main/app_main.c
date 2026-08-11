/*
 * Minimal but fully functional example: provisions Wi-Fi (captive portal on
 * first boot, stored credentials afterwards), mounts a LittleFS partition,
 * and serves it over WebDAV so it can be mounted as a network drive from a
 * Mac (Finder "Connect to Server", Mountain Duck, Cyberduck) or Windows
 * Explorer ("Map network drive").
 */

#include <stdio.h>

#include "esp_event.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_webdav.h"
#include "mdns.h"
#include "wifi_provisioner.h"

static const char *TAG = "webdav_example";

#define WEBDAV_MDNS_HOSTNAME "esp32-webdav" /* reachable as http://esp32-webdav.local/ */
#define WEBDAV_LITTLEFS_BASE_PATH "/littlefs"
#define WEBDAV_LITTLEFS_PARTITION_LABEL "storage" /* must match partitions.csv */

static void on_wifi_connected(void)
{
    ESP_LOGI(TAG, "WiFi connected.");
}

/*
 * esp-idf-wifi-provisioner only keeps a WIFI_EVENT_STA_DISCONNECTED handler
 * registered while it is establishing the initial connection --
 * wifi_sta_connect() unregisters it as soon as the connection succeeds. After
 * that, nothing reconnects: one beacon timeout (AP reboot, interference, the
 * radio dozing off) leaves the device offline until it is power-cycled, which
 * for a file server means silently vanishing mid-transfer.
 *
 * Keep our own handler alive for the lifetime of the app instead.
 */
static void wifi_reconnect_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                    void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected -- reconnecting");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi reconnected -- IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void on_portal_start(void)
{
    ESP_LOGI(TAG, "No stored WiFi credentials -- connect to the '%s' access point and open "
                  "http://192.168.4.1/ to configure WiFi.",
             CONFIG_WIFI_PROV_AP_SSID);
}

static void wifi_connect(void)
{
    wifi_prov_config_t prov_config = WIFI_PROV_DEFAULT_CONFIG();
    prov_config.ap_ssid = WEBDAV_MDNS_HOSTNAME "-setup";
    prov_config.on_connected = on_wifi_connected;
    prov_config.on_portal_start = on_portal_start;

    /* Reads stored credentials from NVS and connects; if none are stored (or
     * the stored network can't be reached), falls back to an open AP + a
     * captive-portal web page for entering SSID/password, then stores them
     * and connects. Also handles NVS/netif/event-loop init. */
    ESP_ERROR_CHECK(wifi_prov_start(&prov_config));
    ESP_ERROR_CHECK(wifi_prov_wait_for_connection(portMAX_DELAY));

    /* Register only after provisioning has finished, so we don't interfere
     * with the library's own connect/retry handling while it runs. */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifi_reconnect_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_reconnect_handler, NULL, NULL));

    /* This device is a server -- clients initiate every request. The default
     * modem-sleep power save lets the radio doze between beacon intervals,
     * which adds 100-300ms of jitter to every inbound packet while it waits
     * to wake up. Disable it so WebDAV requests get answered promptly. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    esp_netif_ip_info_t ip_info;
    if (wifi_prov_get_ip_info(&ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ip_info.ip));
    }
}

static void littlefs_mount(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = WEBDAV_LITTLEFS_BASE_PATH,
        .partition_label = WEBDAV_LITTLEFS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&conf));

    size_t total = 0, used = 0;
    if (esp_littlefs_info(conf.partition_label, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted at %s: %u/%u KB used", conf.base_path,
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
}

static void mdns_start(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(WEBDAV_MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("ESP32 WebDAV example"));

    /* "path" tells a browsing client which URL path to mount; it is the
     * convention for _webdav._tcp and _http._tcp, and clients that read the
     * TXT record (Finder's Network view among them) will otherwise guess. */
    mdns_txt_item_t txt[] = {
        { "path", "/" },
        { "u", "" },   /* no username  */
        { "p", "" },   /* no password  */
    };

    ESP_ERROR_CHECK(mdns_service_add("ESP32 WebDAV", "_webdav", "_tcp", 80, txt,
                                      sizeof(txt) / sizeof(txt[0])));
    /* Advertise plain HTTP too, so the share also turns up for browsers and
     * anything scanning for _http._tcp rather than _webdav._tcp. */
    ESP_ERROR_CHECK(mdns_service_add("ESP32 WebDAV", "_http", "_tcp", 80, txt,
                                      sizeof(txt) / sizeof(txt[0])));
}

void app_main(void)
{
    wifi_connect();
    littlefs_mount();
    mdns_start();

    esp_webdav_config_t webdav_config = ESP_WEBDAV_CONFIG_DEFAULT();
    webdav_config.root_path = WEBDAV_LITTLEFS_BASE_PATH;

    esp_webdav_handle_t webdav;
    ESP_ERROR_CHECK(esp_webdav_start(&webdav_config, &webdav));

    ESP_LOGI(TAG, "WebDAV server ready.");
    ESP_LOGI(TAG, "  Mac Finder:   Go > Connect to Server... > http://%s.local/",
             WEBDAV_MDNS_HOSTNAME);
    ESP_LOGI(TAG, "  Mountain Duck: New Bookmark > WebDAV (HTTP) > Server: %s.local, Port: 80",
             WEBDAV_MDNS_HOSTNAME);
}
