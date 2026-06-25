#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "graphics/graphics.h"
#include "common/storage.h"
#include "api/web_server.h"
#include "api/wifi.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY_ATTEMPTS 5
#define RETRY_TIMEOUT_MS   30000  // 30 seconds total timeout

static const char *TAG = "wifi";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

extern device_config_t config;

void wifi_build_default_ap_ssid(char *out, size_t out_sz)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, out_sz, WIFI_AP_SSID "_%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

esp_err_t wifi_validate_custom_ap_ssid(const char *ssid)
{
    if (!ssid || !ssid[0]) {
        return ESP_OK;
    }
    size_t n = strlen(ssid);
    if (n > MAX_WIFI_AP_SSID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t wifi_validate_custom_ap_psk(const char *psk)
{
    if (!psk || !psk[0]) {
        return ESP_OK;
    }
    size_t n = strlen(psk);
    if (n < 8 || n > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

void wifi_get_ap_ssid_effective(char *out, size_t out_sz)
{
    const char *custom = config.wifi_ap_ssid.value.str;
    if (custom && custom[0]) {
        snprintf(out, out_sz, "%.*s", MAX_WIFI_AP_SSID_LEN, custom);
    } else {
        wifi_build_default_ap_ssid(out, out_sz);
    }
}

void wifi_get_current_ip(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) {
        return;
    }

    out[0] = '\0';

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = NULL;

    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(out, out_sz, IPSTR, IP2STR(&ip_info.ip));
        return;
    }

    netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(out, out_sz, IPSTR, IP2STR(&ip_info.ip));
        return;
    }

    snprintf(out, out_sz, "(not connected)");
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Connecting to WiFi...");
        disp_show_temp_label("Connecting to wifi...", 0);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY_ATTEMPTS) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry %d/%d connecting to AP", s_retry_num, MAX_RETRY_ATTEMPTS);
        } else {
            ESP_LOGI(TAG, "Failed to connect after %d attempts", MAX_RETRY_ATTEMPTS);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));

        esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ps_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) failed: %s", esp_err_to_name(ps_err));
        } else {
            ESP_LOGI(TAG, "WiFi power save disabled");
        }

        char ip_str[64];
        snprintf(ip_str, sizeof(ip_str), "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        disp_show_temp_label(ip_str, 10000);

        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station %02x:%02x:%02x:%02x:%02x:%02x joined AP",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5]);
        disp_show_temp_label("192.168.4.1", 0);
    }
}

static void start_ap_mode(void)
{
    ESP_LOGI(TAG, "Starting AP mode");

    char ap_ssid[33];
    const char *custom_ssid = config.wifi_ap_ssid.value.str;
    if (custom_ssid && custom_ssid[0]) {
        strncpy(ap_ssid, custom_ssid, sizeof(ap_ssid) - 1);
        ap_ssid[sizeof(ap_ssid) - 1] = '\0';
    } else {
        wifi_build_default_ap_ssid(ap_ssid, sizeof(ap_ssid));
    }

    const char *ap_pass = WIFI_AP_PASS;
    if (config.wifi_ap_psk.value.str && config.wifi_ap_psk.value.str[0]) {
        ap_pass = config.wifi_ap_psk.value.str;
    }

    esp_wifi_stop();

    esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_config = { 0 };
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    size_t ssid_len = strlen(ap_ssid);
    if (ssid_len > sizeof(wifi_config.ap.ssid)) {
        ssid_len = sizeof(wifi_config.ap.ssid);
    }
    memcpy(wifi_config.ap.ssid, ap_ssid, ssid_len);
    wifi_config.ap.ssid_len = ssid_len;

    strncpy((char*)wifi_config.ap.password, ap_pass, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.password[sizeof(wifi_config.ap.password) - 1] = '\0';

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started. SSID: %s, IP: 192.168.4.1", ap_ssid);

    char disp_msg[96];
    snprintf(disp_msg, sizeof(disp_msg), "WIFI AP: %s", ap_ssid);
    disp_show_temp_label(disp_msg, 10000);

    start_web_server();
}

static void wifi_init_common_ap_events(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
}

static bool try_connect_sta(const char *ssid, const char *pass)
{
    s_wifi_event_group = xEventGroupCreate();
    s_retry_num = 0;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(RETRY_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to SSID: %s", ssid);
        start_web_server();
        return true;
    }
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID: %s", ssid);
        return false;
    }
    ESP_LOGI(TAG, "Connection timeout");
    return false;
}

void wifi_init_with_fallback(void)
{
    const bool sta_creds = config.wifi_ssid.value.str != NULL
                         && config.wifi_psk.value.str != NULL;
    const bool ap_only = config.wifi_mode_pref.value.u8 == WIFI_MODE_PREF_AP_ONLY;

    if (ap_only || !sta_creds) {
        if (ap_only && sta_creds) {
            ESP_LOGI(TAG, "AP-only mode: skipping STA");
        } else if (!sta_creds) {
            ESP_LOGI(TAG, "No STA credentials, starting in AP mode");
        }
        wifi_init_common_ap_events();
        start_ap_mode();
        return;
    }

    ESP_LOGI(TAG, "STA-first: attempting STA connection...");
    if (!try_connect_sta(config.wifi_ssid.value.str, config.wifi_psk.value.str)) {
        ESP_LOGW(TAG, "STA connection failed, falling back to AP mode");
        start_ap_mode();
    }
}
