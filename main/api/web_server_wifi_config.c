/*
 * WiFi config REST (/api/wifi) and WebSocket (type "wifi").
 *
 * WS actions:
 *   get_config  — NVS settings (passwords never returned)
 *   status      — live runtime (sta/ap/usb, current_ip WiFi-only, http_server)
 *   get         — both config and status
 *   set         — partial NVS update; optional reboot:true
 *   enable      — WiFi enabled at boot; optional reboot:true
 *   disable     — WiFi disabled at boot (USB stays); optional reboot:true
 *
 * Response type: wifi_response (echoes action + req_id)
 * Push type: wifi_status (server-initiated; full status object, no req_id)
 */

#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "common/storage.h"
#include "api/web_server.h"
#include "api/web_server_internal.h"
#include "api/wifi.h"

static const char *TAG = "web server wifi";

static bool wifi_json_reboot_flag(cJSON *json)
{
    cJSON *jr = cJSON_GetObjectItemCaseSensitive(json, "reboot");
    return cJSON_IsTrue(jr);
}

static void wifi_maybe_reboot(bool reboot)
{
    if (!reboot) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static char *wifi_ws_response(const char *action, cJSON *payload, const char *error,
                              const cJSON *request, bool *ok_out)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "wifi_response");
    cJSON_AddStringToObject(resp, "action", action ? action : "?");
    ws_json_echo_req_id(resp, request);

    if (error) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", error);
        if (ok_out) {
            *ok_out = false;
        }
    } else {
        cJSON_AddBoolToObject(resp, "ok", true);
        if (ok_out) {
            *ok_out = true;
        }
        if (payload) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, payload) {
                cJSON_AddItemToObject(resp, item->string, cJSON_Duplicate(item, true));
            }
        }
    }

    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return s;
}

static char *wifi_ws_query_response(const char *action, cJSON *config, cJSON *status,
                                    const cJSON *request)
{
    cJSON *payload = cJSON_CreateObject();
    if (config) {
        cJSON_AddItemToObject(payload, "config", cJSON_Duplicate(config, true));
    }
    if (status) {
        cJSON_AddItemToObject(payload, "status", cJSON_Duplicate(status, true));
    }
    bool ok_dummy = true;
    char *s = wifi_ws_response(action, payload, NULL, request, &ok_dummy);
    cJSON_Delete(payload);
    return s;
}

static char *wifi_ws_mutate_response(const char *action, esp_err_t err, const char *err_msg,
                                       bool reboot, const cJSON *request)
{
    if (err != ESP_OK) {
        return wifi_ws_response(action, NULL, err_msg ? err_msg : esp_err_to_name(err),
                                request, NULL);
    }

    cJSON *payload = cJSON_CreateObject();
    if (reboot) {
        cJSON_AddBoolToObject(payload, "rebooting", true);
    } else {
        cJSON_AddStringToObject(payload, "note", "Reboot to apply new WiFi settings");
    }
    bool ok_dummy = true;
    char *s = wifi_ws_response(action, payload, NULL, request, &ok_dummy);
    cJSON_Delete(payload);
    return s;
}

static bool wifi_ws_handler(const char *type, cJSON *json)
{
    if (strcmp(type, "wifi") != 0) {
        return false;
    }

    cJSON *act = cJSON_GetObjectItemCaseSensitive(json, "action");
    const char *action = cJSON_IsString(act) ? act->valuestring : NULL;
    if (!action) {
        char *resp = wifi_ws_response("?", NULL, "Missing action", json, NULL);
        if (resp) {
            websocket_broadcast_json_transient(resp);
            free(resp);
        }
        return true;
    }

    char *resp = NULL;

    if (strcmp(action, "get_config") == 0) {
        cJSON *cfg = wifi_config_to_json();
        if (!cfg) {
            resp = wifi_ws_response(action, NULL, "OOM", json, NULL);
        } else {
            resp = wifi_ws_query_response(action, cfg, NULL, json);
            cJSON_Delete(cfg);
        }
    } else if (strcmp(action, "status") == 0) {
        cJSON *st = wifi_status_to_json();
        if (!st) {
            resp = wifi_ws_response(action, NULL, "OOM", json, NULL);
        } else {
            resp = wifi_ws_query_response(action, NULL, st, json);
            cJSON_Delete(st);
        }
    } else if (strcmp(action, "get") == 0) {
        cJSON *cfg = wifi_config_to_json();
        cJSON *st = wifi_status_to_json();
        if (!cfg || !st) {
            resp = wifi_ws_response(action, NULL, "OOM", json, NULL);
        } else {
            resp = wifi_ws_query_response(action, cfg, st, json);
        }
        cJSON_Delete(cfg);
        cJSON_Delete(st);
    } else if (strcmp(action, "set") == 0) {
        char err_buf[64] = {0};
        esp_err_t err = wifi_config_apply_json(json, err_buf, sizeof(err_buf));
        bool reboot = wifi_json_reboot_flag(json);
        if (err == ESP_OK) {
            err = wifi_config_save();
        }
        resp = wifi_ws_mutate_response(action, err,
                                     err_buf[0] ? err_buf : NULL, reboot, json);
        if (resp) {
            websocket_broadcast_json_transient(resp);
            free(resp);
            wifi_maybe_reboot(reboot && err == ESP_OK);
            return true;
        }
    } else if (strcmp(action, "enable") == 0) {
        wifi_config_set_enabled(true);
        bool reboot = wifi_json_reboot_flag(json);
        esp_err_t err = wifi_config_save();
        resp = wifi_ws_mutate_response(action, err, NULL, reboot, json);
        if (resp) {
            websocket_broadcast_json_transient(resp);
            free(resp);
            wifi_maybe_reboot(reboot && err == ESP_OK);
            return true;
        }
    } else if (strcmp(action, "disable") == 0) {
        wifi_config_set_enabled(false);
        bool reboot = wifi_json_reboot_flag(json);
        esp_err_t err = wifi_config_save();
        resp = wifi_ws_mutate_response(action, err, NULL, reboot, json);
        if (resp) {
            websocket_broadcast_json_transient(resp);
            free(resp);
            wifi_maybe_reboot(reboot && err == ESP_OK);
            return true;
        }
    } else {
        resp = wifi_ws_response(action, NULL, "Unknown action", json, NULL);
    }

    if (resp) {
        websocket_broadcast_json_transient(resp);
        free(resp);
    }
    return true;
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }

    cJSON *cfg = wifi_config_to_json();
    cJSON *st = wifi_status_to_json();
    if (!cfg || !st) {
        cJSON_Delete(root);
        cJSON_Delete(cfg);
        cJSON_Delete(st);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }

    cJSON_AddItemToObject(root, "config", cfg);
    cJSON_AddItemToObject(root, "status", st);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, s, strlen(s));
    free(s);
    return r;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 768) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");
    }

    char *buf = malloc(len + 1);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }

    int received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, buf + received, len - received);
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += r;
    }
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    char err_buf[64] = {0};
    esp_err_t err = wifi_config_apply_json(json, err_buf, sizeof(err_buf));
    cJSON_Delete(json);

    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   err_buf[0] ? err_buf : "Invalid request");
    }

    err = wifi_config_save();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
    } else {
        cJSON_AddStringToObject(resp, "note", "Reboot to apply new WiFi settings");
    }

    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!s) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, s, strlen(s));
    free(s);
    return r;
}

uint8_t register_wifi_config_handlers_in_webserver(httpd_handle_t *server)
{
    ws_register_message_handler(wifi_ws_handler);

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

    ESP_LOGI(TAG, "WiFi config: %d HTTP handlers + WS handler registered", n);
    return n;
}
