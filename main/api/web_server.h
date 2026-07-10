#pragma once
#include <stdbool.h>
#include "ble/ble_scan.h" //ble_scanned_device_t
#include "ble/ble_discovery.h" // discovery_context_t
#include "graphics/graphics.h" // gfx_coords_t

#define HTTP_CHUNK_SIZE 512
#define MAX_DEVICEINFO_FILE_SIZE 1024
#define MAX_DEVICE_PATH_LEN 512

#define MAX_WS_CLIENTS 5
#define MAX_STATE_ELEMENTS 10


esp_err_t start_web_server(void);
bool web_server_is_running(void);
void init_simulation_state(void);


// Broadcast a JSON string to all WebSocket clients and save to smart state
// for replay on reconnect. Use this for any persistent UI element.
esp_err_t websocket_broadcast_json(const char *json_str);

// Broadcast a JSON string to all WebSocket clients WITHOUT saving to state.
// Use this for transient/one-shot messages (e.g. notifications, animations).
esp_err_t websocket_broadcast_json_transient(const char *json_str);

void web_scan_broadcast_status(void);


typedef enum {
    SCAN_PUSH_NEW       = 0,// first time this address is seen
    SCAN_PUSH_SCAN_RSP,     // scan response arrived for known device
    SCAN_PUSH_ADV_DATA,     // adv_data payload changed (rotation)
    SCAN_PUSH_DUMP,         // dump all scanned devices on demand
    SCAN_PUSH_RSSI,         // RSSI update
    SCAN_PUSH_LOST,         // device lost
} scan_push_reason_t;
void web_scan_push_device(const ble_scanned_device_t *dev, scan_push_reason_t reason);

// websocket client for relay

// Response callback: called when read response arrives from relay server
typedef void (*ws_relay_response_cb_t)(const char *chr_uuid, const char *data_hex, int status, void *arg);
void ws_relay_client_init(void);
esp_err_t ws_relay_client_connect(const char *target_ip);
esp_err_t ws_relay_client_send(const char *svc_uuid, const char *chr_uuid, const char *data_hex, const char *action);
// Register pending read: cb fired when response with matching chr_uuid arrives
esp_err_t ws_relay_client_read(const char *svc_uuid, const char *chr_uuid, ws_relay_response_cb_t cb, void *arg);
// Blocking read: waits up to timeout_ms for response
// Returns ESP_OK + fills out_hex (caller must free) on success
esp_err_t ws_relay_client_read_sync(const char *svc_uuid, const char *chr_uuid, char **out_hex, uint32_t timeout_ms);
void ws_relay_client_disconnect(void);


void web_scan_broadcast_discovery_result(discovery_context_t *ctx, int rc);

// broadcast connection/service discovery progress
void web_broadcast_connection_progress(const char *addr, const char *phase, const char *status, const char *detail);
void web_broadcast_connection_progress_disc_ctx(discovery_context_t *ctx, const char *phase, const char *status);
void web_broadcast_connection_progress_connection(discovery_context_t *ctx, const char *status);
void web_broadcast_connection_progress_discovery(discovery_context_t *ctx, const char *status);
void web_broadcast_connection_progress_reading(discovery_context_t *ctx, const char *status);


// Websocket logs - macros in place of ESP_LOGI/W/E 
#define WS_LOGI(tag, fmt, ...)  do { ESP_LOGI(tag, fmt, ##__VA_ARGS__); wslog_send('I', tag, fmt, ##__VA_ARGS__); } while(0)
#define WS_LOGW(tag, fmt, ...)  do { ESP_LOGW(tag, fmt, ##__VA_ARGS__); wslog_send('W', tag, fmt, ##__VA_ARGS__); } while(0)
#define WS_LOGE(tag, fmt, ...)  do { ESP_LOGE(tag, fmt, ##__VA_ARGS__); wslog_send('E', tag, fmt, ##__VA_ARGS__); } while(0)

// Called by the macros — do not call directly
void wslog_send(char level, const char *tag, const char *fmt, ...);
esp_err_t parse_websocket_message(const char *payload); // used by console proxy


// Called when a relay_rsp arrives on the WS server (sim role)
void ble_sim_relay_init(void);
void ble_sim_relay_deliver_rsp(uint32_t seq, const char *data_hex, int status);
// Pluggable relay response sender — set to route GATT responses back over
// the WS client socket when acting as Central for a Sim. Pass NULL to restore default.
typedef void (*relay_send_fn_t)(const char *json_str);
void ble_central_set_relay_sender(relay_send_fn_t fn);
void web_central_set_device(const char *device_id);

/** Sim peripheral: trace central-initiated GATT traffic over WebSocket (type ble_sim_trace). */
void web_ble_sim_trace_emit_chr(uint16_t conn_handle, const char *svc_uuid,
                                const char *chr_uuid, const char *action,
                                const uint8_t *data, size_t data_len);
void web_ble_sim_trace_emit_subscribe(
    uint16_t conn_handle, uint16_t attr_handle,
    const char *svc_uuid, const char *chr_uuid, const char *action,
    uint8_t reason, uint8_t prev_notify, uint8_t cur_notify,
    uint8_t prev_indicate, uint8_t cur_indicate);

bool web_ble_sim_trace_get_enabled(void);
void web_ble_sim_trace_set_enabled(bool enabled);
esp_err_t websocket_broadcast_json_presrc(const char *json_str);