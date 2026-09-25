#include "net.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "sdkconfig.h"

static const char *TAG = "net";

#define STA_CONNECT_TIMEOUT_MS 15000
#define GOT_IP_BIT BIT0

static EventGroupHandle_t s_events;
static bool s_sta_active;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_sta_active) {
            ESP_LOGW(TAG, "Disconnected from router, retrying...");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "Connected - open http://" IPSTR "/ or http://%s.local/",
                 IP2STR(&ev->ip_info.ip), CONFIG_WIFI_HOSTNAME);
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "A device joined the access point");
    }
}

static bool try_station(void)
{
    if (strlen(CONFIG_WIFI_SSID) == 0) {
        return false;
    }

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, CONFIG_WIFI_HOSTNAME);

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, CONFIG_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_WIFI_PASSWORD, sizeof(cfg.sta.password));

    ESP_LOGI(TAG, "Connecting to \"%s\"...", CONFIG_WIFI_SSID);
    s_sta_active = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));
    if (bits & GOT_IP_BIT) {
        return true;
    }

    ESP_LOGW(TAG, "Could not connect to \"%s\", falling back to access point", CONFIG_WIFI_SSID);
    s_sta_active = false;
    esp_wifi_stop();
    esp_netif_destroy_default_wifi(netif);
    return false;
}

// Use the configured AP password, or a random per-device one kept in NVS so
// no shared default password ships with the firmware.
static void get_ap_password(char *out, size_t len)
{
    if (strlen(CONFIG_WIFI_AP_PASSWORD) >= 8) {
        strlcpy(out, CONFIG_WIFI_AP_PASSWORD, len);
        return;
    }
    if (strlen(CONFIG_WIFI_AP_PASSWORD) > 0) {
        ESP_LOGW(TAG, "Configured AP password is under 8 chars - using a random one instead");
    }

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("net", NVS_READWRITE, &nvs));
    size_t stored_len = len;
    if (nvs_get_str(nvs, "ap_pass", out, &stored_len) != ESP_OK) {
        // No look-alike characters (0/O, 1/l/I) so it's easy to type
        static const char charset[] = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        const size_t pass_len = 12;
        for (size_t i = 0; i < pass_len && i < len - 1; i++) {
            out[i] = charset[esp_random() % (sizeof(charset) - 1)];
        }
        out[pass_len < len - 1 ? pass_len : len - 1] = '\0';
        ESP_ERROR_CHECK(nvs_set_str(nvs, "ap_pass", out));
        ESP_ERROR_CHECK(nvs_commit(nvs));
    }
    nvs_close(nvs);
}

static void start_access_point(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_config_t cfg = {
        .ap = {
            .channel = 6,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)cfg.ap.ssid, CONFIG_WIFI_AP_SSID, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(CONFIG_WIFI_AP_SSID);
    get_ap_password((char *)cfg.ap.password, sizeof(cfg.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Access point \"%s\" up (password: %s) - join it and open http://192.168.4.1/",
             CONFIG_WIFI_AP_SSID, (const char *)cfg.ap.password);
}

esp_err_t net_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_events = xEventGroupCreate();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    if (!try_station()) {
        start_access_point();
    }
    // Power save adds a lot of latency to the video stream
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(CONFIG_WIFI_HOSTNAME);
        mdns_instance_name_set("ESP32-CAM");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    return ESP_OK;
}
