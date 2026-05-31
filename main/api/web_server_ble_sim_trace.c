#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "cJSON.h"

#include "api/web_server.h"
#include "api/web_server_internal.h"

static const char *TAG = "web server - ble sim trace";


static SemaphoreHandle_t s_mutex;
static bool              s_enabled = true;


static bool trace_is_enabled(void) {
    if (!s_mutex) return s_enabled;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool on = s_enabled;
    xSemaphoreGive(s_mutex);
    return on;
}

void web_ble_sim_trace_set_enabled(bool enabled) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_enabled = enabled;
    xSemaphoreGive(s_mutex);
}

bool web_ble_sim_trace_get_enabled(void) {
    return trace_is_enabled();
}

// Max raw write payload to trace; 64 bytes = 128 hex chars in the JSON.
// Writes larger than this are silently truncated in the hex field.
// In order to increase it adjust NimBLE stack size in sdkconfig 
#define BLE_SIM_TRACE_MAX_DATA_BYTES  64u

// Buffer: fixed JSON overhead ~80 chars + 512 hex + null 
#define TRACE_CHR_BUF_SIZE  (80u + BLE_SIM_TRACE_MAX_DATA_BYTES * 2u + 4u)


// don't use cJSON here as it's costly
void web_ble_sim_trace_emit_chr(uint16_t conn_handle, const char *svc_uuid,
                                const char *chr_uuid, const char *action,
                                const uint8_t *data, size_t data_len) {
    if (!trace_is_enabled()) return;

    const char *act = action ? action : "unknown";
    char buf[TRACE_CHR_BUF_SIZE];

    int pos = snprintf(buf, sizeof(buf),
        "{\"type\":\"ble_sim_trace\",\"action\":\"%s\","
        "\"conn\":%u,\"svc\":\"%s\",\"chr\":\"%s\"",
        act,
        (unsigned)conn_handle,
        svc_uuid ? svc_uuid : "",
        chr_uuid ? chr_uuid : "");

    if (pos <= 0 || pos >= (int)sizeof(buf)) return;

    bool has_data = data_len > 0 && data != NULL
                    && (strcmp(act, "write") == 0
                        || strcmp(act, "write_noresp") == 0);

    if (has_data) {
        int w = snprintf(buf + pos, sizeof(buf) - pos, ",\"data\":\"");
        if (w > 0) pos += w;

        size_t capped_len = data_len > BLE_SIM_TRACE_MAX_DATA_BYTES
                            ? BLE_SIM_TRACE_MAX_DATA_BYTES : data_len;
        for (size_t i = 0; i < capped_len && (pos + 3) < (int)sizeof(buf) - 2; i++) {
            pos += snprintf(buf + pos, 3, "%02x", data[i]);
        }

        w = snprintf(buf + pos, sizeof(buf) - pos, "\"");
        if (w > 0) pos += w;
    }

    snprintf(buf + pos, sizeof(buf) - pos, ",\"src\":\"%s\"}", ws_get_node_id());
    websocket_broadcast_json_presrc_safe(buf);

}

void web_ble_sim_trace_emit_subscribe(
        uint16_t conn_handle, uint16_t attr_handle,
        const char *svc_uuid, const char *chr_uuid, const char *action,
        uint8_t reason,
        uint8_t prev_notify,  uint8_t cur_notify,
        uint8_t prev_indicate, uint8_t cur_indicate) {

    if (!trace_is_enabled()) return;

    char buf[256];

    snprintf(buf, sizeof(buf),
        "{\"type\":\"ble_sim_trace\",\"action\":\"%s\","
        "\"conn\":%u,\"attr_handle\":%u,"
        "\"svc\":\"%s\",\"chr\":\"%s\","
        "\"reason\":%u,"
        "\"prev_notify\":%s,\"prev_indicate\":%s,"
        "\"notify\":%s,\"indicate\":%s,"
        "\"src\":\"%s\"}",
        action ? action : "subscribe",
        (unsigned)conn_handle,
        (unsigned)attr_handle,
        svc_uuid ? svc_uuid : "",
        chr_uuid ? chr_uuid : "",
        (unsigned)reason,
        prev_notify   ? "true" : "false",
        prev_indicate ? "true" : "false",
        cur_notify    ? "true" : "false",
        cur_indicate  ? "true" : "false",
        ws_get_node_id());

    websocket_broadcast_json_presrc_safe(buf);
}

static bool ble_sim_trace_ws_handler(const char *type, cJSON *json) {
    if (strcmp(type, "ble_sim_trace") != 0) return false;

    cJSON *en = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    if (cJSON_IsBool(en) && s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_enabled = cJSON_IsTrue(en);
        ESP_LOGI(TAG, "trace %s", s_enabled ? "enabled" : "disabled");
        xSemaphoreGive(s_mutex);
    }
    return true;
}

uint8_t register_ble_sim_trace_handlers_in_web_server(httpd_handle_t *server) {
    (void)server;
    s_mutex = xSemaphoreCreateMutex();
    ws_register_message_handler(ble_sim_trace_ws_handler);
    return 0;
}