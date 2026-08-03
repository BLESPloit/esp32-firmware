#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "api/web_server.h"
#include "api/web_server_internal.h"

static const char *TAG = "web server - wslog";

#define WSLOG_MAX_TAG_FILTERS  16
#define WSLOG_TAG_LEN          32


typedef enum {
    WSLOG_LEVEL_E = 0,  // ERROR
    WSLOG_LEVEL_W = 1,  // WARN
    WSLOG_LEVEL_I = 2,  // INFO
} wslog_level_t;


typedef struct {
    bool           enabled;
    bool           info_enabled;
    // For INFO: only allowed_info_tags pass (if list is non-empty)
    char           allowed_info_tags[WSLOG_MAX_TAG_FILTERS][WSLOG_TAG_LEN];
    int            allowed_info_tag_count;  // 0 = block all INFO
} wslog_filter_t;

// ── State ────────────────────────────────────────────────── 

static wslog_filter_t s_filter = {
    .enabled                = true,
    .info_enabled           = false,
    .allowed_info_tag_count = 0,
};

static SemaphoreHandle_t s_mutex = NULL;

// ── Internal helpers ────────────────────────────────────────────────── 

static bool tag_in_list(const char list[][WSLOG_TAG_LEN], int count, const char *tag) {
    for (int i = 0; i < count; i++) {
        if (strstr(tag, list[i]) != NULL) return true;
    }
    return false;
}

static wslog_level_t char_to_level(char c) {
    if (c == 'E') return WSLOG_LEVEL_E;
    if (c == 'W') return WSLOG_LEVEL_W;
    return WSLOG_LEVEL_I;
}

static char level_to_char(wslog_level_t lvl) {
    if (lvl == WSLOG_LEVEL_E) return 'E';
    if (lvl == WSLOG_LEVEL_W) return 'W';
    return 'I';
}

// ── Public: init & send ────────────────────────────────────────────────── 

void wslog_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "wslog initialized");
}

void wslog_send(char level, const char *tag, const char *fmt, ...) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool enabled      = s_filter.enabled;
    bool info_enabled = s_filter.info_enabled;
    int  info_count   = s_filter.allowed_info_tag_count;
    char info_snap[WSLOG_MAX_TAG_FILTERS][WSLOG_TAG_LEN];
    memcpy(info_snap, s_filter.allowed_info_tags, sizeof(info_snap));

    xSemaphoreGive(s_mutex);

    if (!enabled) return;

    // E and W always pass; I checks the allowlist
    if (level == 'I') {
        if (!info_enabled) return;  // INFO globally disabled
        // If there are tag filters, apply partial matching
        if (info_count > 0 && !tag_in_list(info_snap, info_count, tag)) {
            return;  // tag not matched
        }
    }

    // Format message
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Sanitize for JSON inline
    for (char *p = buf; *p; p++) {
        if (*p == '"')              *p = '\'';
        else if (*p == '\n' || *p == '\r') *p = ' ';
        else if (*p == '\\')        *p = '/';
    }

    char json[320];
    snprintf(json, sizeof(json),
        "{\"type\":\"log\",\"level\":\"%c\",\"tag\":\"%s\",\"msg\":\"%s\"}",
        level, tag, buf);

    websocket_broadcast_json_presrc_safe(json);  // safe from any task
}


// ── GET /api/log/filter ────────────────────────────────────────────────── 

static cJSON *wslog_filter_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", s_filter.enabled);
    cJSON_AddBoolToObject(root, "info_enabled", s_filter.info_enabled);

    cJSON *allowed = cJSON_AddArrayToObject(root, "allowed_info_tags");
    for (int i = 0; i < s_filter.allowed_info_tag_count; i++)
        cJSON_AddItemToArray(allowed, cJSON_CreateString(s_filter.allowed_info_tags[i]));
    return root;
}

static void wslog_apply_filter_from_json(cJSON *json)
{
    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    if (cJSON_IsBool(enabled))
        s_filter.enabled = cJSON_IsTrue(enabled);

    cJSON *info_enabled = cJSON_GetObjectItem(json, "info_enabled");
    if (cJSON_IsBool(info_enabled))
        s_filter.info_enabled = cJSON_IsTrue(info_enabled);

    cJSON *allowed = cJSON_GetObjectItem(json, "allowed_info_tags");
    if (cJSON_IsArray(allowed)) {
        s_filter.allowed_info_tag_count = 0;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, allowed) {
            if (!cJSON_IsString(item)) continue;
            if (s_filter.allowed_info_tag_count >= WSLOG_MAX_TAG_FILTERS) break;
            strncpy(s_filter.allowed_info_tags[s_filter.allowed_info_tag_count],
                    item->valuestring, WSLOG_TAG_LEN - 1);
            s_filter.allowed_info_tags[s_filter.allowed_info_tag_count][WSLOG_TAG_LEN - 1] = '\0';
            s_filter.allowed_info_tag_count++;
        }
    }
}

static esp_err_t wslog_get_filter_handler(httpd_req_t *req) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    cJSON *root = wslog_filter_to_json();

    xSemaphoreGive(s_mutex);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ESP_OK;
}


// ── POST /api/log/filter ────────────────────────────────────────────────── 
// Body: { "enabled": true, "info_enabled": true, "allowed_info_tags": ["ble"] }

static esp_err_t wslog_set_filter_handler(httpd_req_t *req) {
    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);

    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *json = cJSON_Parse(content);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    wslog_apply_filter_from_json(json);

    xSemaphoreGive(s_mutex);
    cJSON_Delete(json);

    ESP_LOGI(TAG, "Filter updated: enabled=%d info_enabled=%d allowed info tags=%d",
        s_filter.enabled, s_filter.info_enabled, s_filter.allowed_info_tag_count);

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\"}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}


static bool wslog_ws_handler(const char *type, cJSON *json) {
    if (strcmp(type, "log_filter") != 0) return false;

    cJSON *act = cJSON_GetObjectItemCaseSensitive(json, "action");
    const char *action = cJSON_IsString(act) ? act->valuestring : NULL;

    if (action && strcmp(action, "get") == 0) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        cJSON *root = wslog_filter_to_json();
        xSemaphoreGive(s_mutex);
        cJSON_AddStringToObject(root, "type", "log_filter_response");
        ws_json_echo_req_id(root, json);
        char *str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (str) {
            websocket_broadcast_json_transient(str);
            free(str);
        }
        return true;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    wslog_apply_filter_from_json(json);
    xSemaphoreGive(s_mutex);

    cJSON *req_id = cJSON_GetObjectItemCaseSensitive(json, "req_id");
    if (cJSON_IsNumber(req_id) || cJSON_IsString(req_id)) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        cJSON *root = wslog_filter_to_json();
        xSemaphoreGive(s_mutex);
        cJSON_AddStringToObject(root, "type", "log_filter_response");
        ws_json_echo_req_id(root, json);
        char *str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (str) {
            websocket_broadcast_json_transient(str);
            free(str);
        }
    } else {
        ESP_LOGI(TAG, "Filter updated via WS");
    }
    return true;
}


// ── Register handlers ────────────────────────────────────────────────── 

uint8_t register_wslog_handlers_in_web_server(httpd_handle_t *server)
{
    uint8_t handler_count = 0;

    ws_register_message_handler(wslog_ws_handler);

    httpd_uri_t get_filter_uri = {
        .uri = "/api/log/filter",
        .method = HTTP_GET,
        .handler = wslog_get_filter_handler,
        .user_ctx = NULL,
        .is_websocket = false,
    };
    httpd_register_uri_handler(*server, &get_filter_uri);
    handler_count++;

    httpd_uri_t set_filter_uri = {
        .uri = "/api/log/filter",
        .method = HTTP_POST,
        .handler = wslog_set_filter_handler,
        .user_ctx = NULL,
        .is_websocket = false,
    };
    httpd_register_uri_handler(*server, &set_filter_uri);
    handler_count++;

    return handler_count;
}