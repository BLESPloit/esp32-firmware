#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "graphics/graphics.h"
#include "common/storage.h"
#include "api/web_server.h"
#include "api/usb_net.h"
#include "api/wifi.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY_ATTEMPTS 5
#define RETRY_TIMEOUT_MS   30000  // 30 seconds total timeout

static const char *TAG = "wifi";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool s_sta_got_ip = false;
static bool s_ip_events_registered = false;

typedef struct {
    bool wifi_running;
    char active_mode[8];
    bool sta_connected;
    char sta_ip[20];
    bool ap_up;
    char ap_ip[20];
    bool usb_link_up;
    char usb_ip[20];
    char current_ip[32];
} wifi_status_snapshot_t;

static wifi_status_snapshot_t s_last_status = {0};
static bool s_last_status_valid = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);

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

static bool netif_ip_str(const char *ifkey, char *out, size_t out_sz)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    if (!netif) {
        return false;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }
    snprintf(out, out_sz, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

static const char *wifi_primary_iface_key(void)
{
    char ip[20];
    if (netif_ip_str("WIFI_STA_DEF", ip, sizeof(ip))) {
        return "WIFI_STA_DEF";
    }
    if (netif_ip_str("WIFI_AP_DEF", ip, sizeof(ip))) {
        return "WIFI_AP_DEF";
    }
    return NULL;
}

static void wifi_status_fill_snapshot(wifi_status_snapshot_t *snap, const cJSON *status)
{
    if (!snap || !status) {
        return;
    }

    memset(snap, 0, sizeof(*snap));

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(status, "wifi_running");
    snap->wifi_running = cJSON_IsTrue(item);

    item = cJSON_GetObjectItemCaseSensitive(status, "active_mode");
    if (cJSON_IsString(item)) {
        strncpy(snap->active_mode, item->valuestring, sizeof(snap->active_mode) - 1);
    }

    item = cJSON_GetObjectItemCaseSensitive(status, "current_ip");
    if (cJSON_IsString(item)) {
        strncpy(snap->current_ip, item->valuestring, sizeof(snap->current_ip) - 1);
    }

    const cJSON *sta = cJSON_GetObjectItemCaseSensitive(status, "sta");
    if (sta) {
        item = cJSON_GetObjectItemCaseSensitive(sta, "connected");
        snap->sta_connected = cJSON_IsTrue(item);
        item = cJSON_GetObjectItemCaseSensitive(sta, "ip");
        if (cJSON_IsString(item)) {
            strncpy(snap->sta_ip, item->valuestring, sizeof(snap->sta_ip) - 1);
        }
    }

    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(status, "ap");
    if (ap) {
        item = cJSON_GetObjectItemCaseSensitive(ap, "up");
        snap->ap_up = cJSON_IsTrue(item);
        item = cJSON_GetObjectItemCaseSensitive(ap, "ip");
        if (cJSON_IsString(item)) {
            strncpy(snap->ap_ip, item->valuestring, sizeof(snap->ap_ip) - 1);
        }
    }

    const cJSON *usb = cJSON_GetObjectItemCaseSensitive(status, "usb");
    if (usb) {
        item = cJSON_GetObjectItemCaseSensitive(usb, "link_up");
        snap->usb_link_up = cJSON_IsTrue(item);
        item = cJSON_GetObjectItemCaseSensitive(usb, "ip");
        if (cJSON_IsString(item)) {
            strncpy(snap->usb_ip, item->valuestring, sizeof(snap->usb_ip) - 1);
        }
    }
}

static bool wifi_status_snapshot_changed(const wifi_status_snapshot_t *snap)
{
    if (!snap) {
        return false;
    }
    if (!s_last_status_valid) {
        return true;
    }
    return memcmp(snap, &s_last_status, sizeof(*snap)) != 0;
}

char *wifi_status_message_new(void)
{
    cJSON *status = wifi_status_to_json();
    if (!status) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(status);
        return NULL;
    }

    cJSON_AddStringToObject(root, "type", "wifi_status");
    cJSON_AddItemToObject(root, "status", status);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

void wifi_broadcast_status(void)
{
    cJSON *status = wifi_status_to_json();
    if (!status) {
        return;
    }

    wifi_status_snapshot_t snap;
    wifi_status_fill_snapshot(&snap, status);
    cJSON_Delete(status);

    if (!wifi_status_snapshot_changed(&snap)) {
        return;
    }

    s_last_status = snap;
    s_last_status_valid = true;

    char *json = wifi_status_message_new();
    if (!json) {
        return;
    }

    websocket_broadcast_json_transient(json);
    free(json);
}

void wifi_status_register_events(void)
{
    if (s_ip_events_registered) {
        return;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));
    s_ip_events_registered = true;
}

static bool wifi_driver_running(wifi_mode_t *mode_out)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK || mode == WIFI_MODE_NULL) {
        return false;
    }
    if (mode_out) {
        *mode_out = mode;
    }
    return true;
}

static const char *wifi_active_mode_str(wifi_mode_t mode, bool sta_connected)
{
    switch (mode) {
    case WIFI_MODE_STA:
        return "sta";
    case WIFI_MODE_AP:
        return "ap";
    case WIFI_MODE_APSTA:
        return sta_connected ? "sta" : "ap";
    default:
        return "off";
    }
}

static esp_err_t apply_wifi_mode_json(cJSON *jmode)
{
    if (!jmode) {
        return ESP_OK;
    }
    if (cJSON_IsString(jmode)) {
        const char *s = jmode->valuestring;
        if (strcasecmp(s, "sta_first") == 0 || strcasecmp(s, "sta") == 0) {
            config.wifi_mode_pref.value.u8 = WIFI_MODE_PREF_STA_FIRST;
            return ESP_OK;
        }
        if (strcasecmp(s, "ap_only") == 0 || strcasecmp(s, "ap") == 0) {
            config.wifi_mode_pref.value.u8 = WIFI_MODE_PREF_AP_ONLY;
            return ESP_OK;
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_IsNumber(jmode)) {
        int v = (int)cJSON_GetNumberValue(jmode);
        if (v == WIFI_MODE_PREF_STA_FIRST || v == WIFI_MODE_PREF_AP_ONLY) {
            config.wifi_mode_pref.value.u8 = (uint8_t)v;
            return ESP_OK;
        }
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_INVALID_ARG;
}

static void wifi_set_err(char *err_buf, size_t err_sz, const char *msg)
{
    if (err_buf && err_sz > 0) {
        strncpy(err_buf, msg, err_sz - 1);
        err_buf[err_sz - 1] = '\0';
    }
}

cJSON *wifi_config_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddBoolToObject(root, "enabled", config.net_enabled.value.u8 != 0);
    cJSON_AddStringToObject(root, "ssid",
        config.wifi_ssid.value.str ? config.wifi_ssid.value.str : "");
    cJSON_AddBoolToObject(root, "has_psk",
        config.wifi_psk.value.str && strlen(config.wifi_psk.value.str) > 0);

    const char *mode_str = config.wifi_mode_pref.value.u8 == WIFI_MODE_PREF_AP_ONLY
                               ? "ap_only"
                               : "sta_first";
    cJSON_AddStringToObject(root, "mode", mode_str);

    cJSON_AddStringToObject(root, "ap_ssid",
        config.wifi_ap_ssid.value.str ? config.wifi_ap_ssid.value.str : "");

    char eff[48];
    wifi_get_ap_ssid_effective(eff, sizeof(eff));
    cJSON_AddStringToObject(root, "ap_ssid_effective", eff);

    cJSON_AddBoolToObject(root, "has_ap_psk",
        config.wifi_ap_psk.value.str && strlen(config.wifi_ap_psk.value.str) > 0);

    const bool has_sta = config.wifi_ssid.value.str && config.wifi_ssid.value.str[0]
                      && config.wifi_psk.value.str && config.wifi_psk.value.str[0];
    cJSON_AddBoolToObject(root, "has_sta_creds", has_sta);

    return root;
}

cJSON *wifi_status_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    const bool running = wifi_driver_running(&mode);

    wifi_ap_record_t ap_info = {0};
    const bool sta_connected = running
        && (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA)
        && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;

    char current_ip[32];
    wifi_get_current_ip(current_ip, sizeof(current_ip));

    const char *primary = wifi_primary_iface_key();

    cJSON_AddBoolToObject(root, "wifi_running", running);
    cJSON_AddStringToObject(root, "active_mode", running ? wifi_active_mode_str(mode, sta_connected) : "off");
    cJSON_AddStringToObject(root, "current_ip", current_ip);
    if (primary) {
        cJSON_AddStringToObject(root, "primary_iface", primary);
    } else {
        cJSON_AddNullToObject(root, "primary_iface");
    }

    cJSON *sta = cJSON_CreateObject();
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    cJSON_AddBoolToObject(sta, "up", sta_netif != NULL);
    cJSON_AddBoolToObject(sta, "connected", sta_connected);
    if (sta_connected) {
        cJSON_AddStringToObject(sta, "ssid", (const char *)ap_info.ssid);
        cJSON_AddNumberToObject(sta, "rssi", ap_info.rssi);
    } else {
        cJSON_AddNullToObject(sta, "ssid");
    }
    char sta_ip[20];
    if (netif_ip_str("WIFI_STA_DEF", sta_ip, sizeof(sta_ip))) {
        cJSON_AddStringToObject(sta, "ip", sta_ip);
    } else {
        cJSON_AddNullToObject(sta, "ip");
    }
    cJSON_AddItemToObject(root, "sta", sta);

    cJSON *ap = cJSON_CreateObject();
    char ap_eff[48];
    wifi_get_ap_ssid_effective(ap_eff, sizeof(ap_eff));
    cJSON_AddStringToObject(ap, "ssid_effective", ap_eff);
    char ap_ip[20];
    const bool ap_has_ip = netif_ip_str("WIFI_AP_DEF", ap_ip, sizeof(ap_ip));
    const bool ap_up = running && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) && ap_has_ip;
    cJSON_AddBoolToObject(ap, "up", ap_up);
    if (ap_has_ip) {
        cJSON_AddStringToObject(ap, "ip", ap_ip);
    } else {
        cJSON_AddNullToObject(ap, "ip");
    }
    int ap_clients = 0;
    if (ap_up) {
        wifi_sta_list_t sta_list = {0};
        if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
            ap_clients = sta_list.num;
        }
    }
    cJSON_AddNumberToObject(ap, "clients", ap_clients);
    cJSON_AddItemToObject(root, "ap", ap);

    cJSON *usb = cJSON_CreateObject();
    esp_netif_t *usb_netif = esp_netif_get_handle_from_ifkey("USB_NCM");
    cJSON_AddBoolToObject(usb, "up", usb_netif != NULL);
    char usb_ip[20];
    usb_net_get_ip(usb_ip, sizeof(usb_ip));
    if (usb_ip[0] && strcmp(usb_ip, "(not configured)") != 0 && strcmp(usb_ip, "(disabled)") != 0) {
        cJSON_AddStringToObject(usb, "ip", usb_ip);
    } else {
        cJSON_AddNullToObject(usb, "ip");
    }
    cJSON_AddBoolToObject(usb, "link_up", usb_net_link_up());
    cJSON_AddItemToObject(root, "usb", usb);

    cJSON_AddBoolToObject(root, "http_server", web_server_is_running());
    cJSON_AddBoolToObject(root, "pending_reboot", false);

    return root;
}

void wifi_config_set_enabled(bool on)
{
    config.net_enabled.value.u8 = on ? 1 : 0;
}

esp_err_t wifi_config_save(void)
{
    return write_config_nvs();
}

esp_err_t wifi_config_apply_json(cJSON *json, char *err_buf, size_t err_sz)
{
    if (!json) {
        wifi_set_err(err_buf, err_sz, "Missing JSON");
        return ESP_ERR_INVALID_ARG;
    }

    if (cJSON_GetObjectItemCaseSensitive(json, "enabled")) {
        cJSON *jen = cJSON_GetObjectItemCaseSensitive(json, "enabled");
        if (!cJSON_IsBool(jen)) {
            wifi_set_err(err_buf, err_sz, "Invalid enabled");
            return ESP_ERR_INVALID_ARG;
        }
        wifi_config_set_enabled(cJSON_IsTrue(jen));
    }

    if (cJSON_GetObjectItemCaseSensitive(json, "mode")) {
        cJSON *jmode = cJSON_GetObjectItemCaseSensitive(json, "mode");
        if (apply_wifi_mode_json(jmode) != ESP_OK) {
            wifi_set_err(err_buf, err_sz, "Invalid mode");
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (cJSON_GetObjectItemCaseSensitive(json, "ssid")) {
        cJSON *jssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
        if (!cJSON_IsString(jssid) || strlen(jssid->valuestring) == 0) {
            wifi_set_err(err_buf, err_sz, "Invalid ssid");
            return ESP_ERR_INVALID_ARG;
        }
        char *n = strdup(jssid->valuestring);
        if (!n) {
            wifi_set_err(err_buf, err_sz, "OOM");
            return ESP_ERR_NO_MEM;
        }
        free(config.wifi_ssid.value.str);
        config.wifi_ssid.value.str = n;
    }

    if (cJSON_GetObjectItemCaseSensitive(json, "psk")) {
        cJSON *jpsk = cJSON_GetObjectItemCaseSensitive(json, "psk");
        if (!cJSON_IsString(jpsk)) {
            wifi_set_err(err_buf, err_sz, "Invalid psk");
            return ESP_ERR_INVALID_ARG;
        }
        if (strlen(jpsk->valuestring) > 0) {
            char *n = strdup(jpsk->valuestring);
            if (!n) {
                wifi_set_err(err_buf, err_sz, "OOM");
                return ESP_ERR_NO_MEM;
            }
            free(config.wifi_psk.value.str);
            config.wifi_psk.value.str = n;
        }
    }

    if (cJSON_GetObjectItemCaseSensitive(json, "ap_ssid")) {
        cJSON *ja = cJSON_GetObjectItemCaseSensitive(json, "ap_ssid");
        if (!cJSON_IsString(ja)) {
            wifi_set_err(err_buf, err_sz, "Invalid ap_ssid");
            return ESP_ERR_INVALID_ARG;
        }
        const char *v = ja->valuestring;
        if (wifi_validate_custom_ap_ssid(v) != ESP_OK) {
            wifi_set_err(err_buf, err_sz, "Invalid ap_ssid length");
            return ESP_ERR_INVALID_ARG;
        }
        free(config.wifi_ap_ssid.value.str);
        config.wifi_ap_ssid.value.str = NULL;
        if (strlen(v) > 0) {
            char *n = strdup(v);
            if (!n) {
                wifi_set_err(err_buf, err_sz, "OOM");
                return ESP_ERR_NO_MEM;
            }
            config.wifi_ap_ssid.value.str = n;
        }
    }

    if (cJSON_GetObjectItemCaseSensitive(json, "ap_psk")) {
        cJSON *jp = cJSON_GetObjectItemCaseSensitive(json, "ap_psk");
        if (!cJSON_IsString(jp)) {
            wifi_set_err(err_buf, err_sz, "Invalid ap_psk");
            return ESP_ERR_INVALID_ARG;
        }
        const char *v = jp->valuestring;
        if (wifi_validate_custom_ap_psk(v) != ESP_OK) {
            wifi_set_err(err_buf, err_sz, "Invalid ap_psk");
            return ESP_ERR_INVALID_ARG;
        }
        free(config.wifi_ap_psk.value.str);
        config.wifi_ap_psk.value.str = NULL;
        if (strlen(v) > 0) {
            char *n = strdup(v);
            if (!n) {
                wifi_set_err(err_buf, err_sz, "OOM");
                return ESP_ERR_NO_MEM;
            }
            config.wifi_ap_psk.value.str = n;
        }
    }

    if (config.wifi_ssid.value.str && strlen(config.wifi_ssid.value.str) > 0
        && config.wifi_psk.value.str && strlen(config.wifi_psk.value.str) > 0) {
        config.net_enabled.value.u8 = 1;
    }

    return ESP_OK;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Connecting to WiFi...");
        disp_show_temp_label("Connecting to wifi...", 0);
        esp_wifi_connect();
        wifi_broadcast_status();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const bool had_ip = s_sta_got_ip;
        if (s_retry_num < MAX_RETRY_ATTEMPTS) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry %d/%d connecting to AP", s_retry_num, MAX_RETRY_ATTEMPTS);
        } else {
            ESP_LOGI(TAG, "Failed to connect after %d attempts", MAX_RETRY_ATTEMPTS);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            wifi_broadcast_status();
        }
        if (had_ip) {
            s_sta_got_ip = false;
            wifi_broadcast_status();
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
        s_sta_got_ip = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_broadcast_status();
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
    wifi_broadcast_status();
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
    wifi_status_register_events();

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
