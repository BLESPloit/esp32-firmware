#include <string.h>
#include <strings.h>
#include <stdint.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "common/storage.h"
#include "api/web_server.h"
#include "api/web_server_internal.h"
#include "api/wifi.h"

static const char *TAG = "web server wifi";
extern device_config_t config;

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

// GET /api/wifi  →  STA fields, mode, AP fields
static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
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

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, s, strlen(s));
    free(s);
    return r;
}

/* POST /api/wifi — partial updates allowed.
   STA: ssid key optional; when present must be non-empty string.
        psk omitted or empty string keeps existing STA password.
   mode: optional sta_first | ap_only (or 0 | 1).
   AP: ap_ssid key optional — empty string clears custom SSID (MAC default).
       ap_psk key optional — empty string clears custom AP password (default PASS).
*/
static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 768)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");

    char *buf = malloc(len + 1);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");

    int received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, buf + received, len - received);
        if (r <= 0) { free(buf); return ESP_FAIL; }
        received += r;
    }
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");

    if (cJSON_GetObjectItemCaseSensitive(json, "mode")) {
        cJSON *jmode = cJSON_GetObjectItemCaseSensitive(json, "mode");
        esp_err_t mer = apply_wifi_mode_json(jmode);
        if (mer != ESP_OK) {
            cJSON_Delete(json);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode");
        }
    }

    if (cJSON_GetObjectItemCaseSensitive(json, "ssid")) {
        cJSON *jssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
        if (!cJSON_IsString(jssid) || strlen(jssid->valuestring) == 0) {
            cJSON_Delete(json);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ssid");
        }
        char *n = strdup(jssid->valuestring);
        if (!n) {
            cJSON_Delete(json);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        }
        free(config.wifi_ssid.value.str);
        config.wifi_ssid.value.str = n;
    }

    if (cJSON_GetObjectItemCaseSensitive(json, "psk")) {
        cJSON *jpsk = cJSON_GetObjectItemCaseSensitive(json, "psk");
        if (!cJSON_IsString(jpsk)) {
            cJSON_Delete(json);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid psk");
        }
        if (strlen(jpsk->valuestring) > 0) {
            char *n = strdup(jpsk->valuestring);
            if (!n) {
                cJSON_Delete(json);
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
            }
            free(config.wifi_psk.value.str);
            config.wifi_psk.value.str = n;
        }
    }

    if (cJSON_GetObjectItemCaseSensitive(json, "ap_ssid")) {
        cJSON *ja = cJSON_GetObjectItemCaseSensitive(json, "ap_ssid");
        if (!cJSON_IsString(ja)) {
            cJSON_Delete(json);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ap_ssid");
        }
        const char *v = ja->valuestring;
        if (wifi_validate_custom_ap_ssid(v) != ESP_OK) {
            cJSON_Delete(json);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ap_ssid length");
        }
        free(config.wifi_ap_ssid.value.str);
        config.wifi_ap_ssid.value.str = NULL;
        if (strlen(v) > 0) {
            char *n = strdup(v);
            if (!n) {
                cJSON_Delete(json);
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
            }
            config.wifi_ap_ssid.value.str = n;
        }
    }

    if (cJSON_GetObjectItemCaseSensitive(json, "ap_psk")) {
        cJSON *jp = cJSON_GetObjectItemCaseSensitive(json, "ap_psk");
        if (!cJSON_IsString(jp)) {
            cJSON_Delete(json);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ap_psk");
        }
        const char *v = jp->valuestring;
        if (wifi_validate_custom_ap_psk(v) != ESP_OK) {
            cJSON_Delete(json);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                      "Invalid ap_psk (use 8..63 chars or empty for default)");
        }
        free(config.wifi_ap_psk.value.str);
        config.wifi_ap_psk.value.str = NULL;
        if (strlen(v) > 0) {
            char *n = strdup(v);
            if (!n) {
                cJSON_Delete(json);
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
            }
            config.wifi_ap_psk.value.str = n;
        }
    }

    cJSON_Delete(json);

    if (config.wifi_ssid.value.str && strlen(config.wifi_ssid.value.str) > 0
        && config.wifi_psk.value.str && strlen(config.wifi_psk.value.str) > 0) {
        config.net_enabled.value.u8 = 1;
    }

    esp_err_t err = write_config_nvs();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    if (err != ESP_OK)
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
    else
        cJSON_AddStringToObject(resp, "note", "Reboot to apply new WiFi settings");

    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, s, strlen(s));
    free(s);
    return r;
}

uint8_t register_wifi_config_handlers_in_webserver(httpd_handle_t *server)
{
    uint8_t n = 0;

    httpd_uri_t uris[] = {
        {
            .uri = "/api/wifi",
            .method = HTTP_GET,
            .handler = wifi_get_handler,
            .user_ctx = NULL,
            .is_websocket = false,
        },
        {
            .uri = "/api/wifi",
            .method = HTTP_POST,
            .handler = wifi_post_handler,
            .user_ctx = NULL,
            .is_websocket = false,
        },
    };

    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        httpd_register_uri_handler(*server, &uris[i]);
        n++;
    }

    ESP_LOGI(TAG, "WiFi config: %d HTTP handlers registered", n);
    return n;
}