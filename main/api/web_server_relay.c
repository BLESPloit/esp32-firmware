#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h> 

#include "services/gap/ble_svc_gap.h" // BLE_ADDR_PUBLIC ...
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "esp_websocket_client.h"

#include "ble/ble_central.h"
#include "ble/ble_discovery.h"
#include "ble/ble_scan.h"
//#include "ble/ble_sim.h" // load device for simulation
//#include "graphics.h"
//#include "interface_sim.h"
#include "common/storage.h"
#include "common/utils.h"
#include "api/web_server.h"
#include "api/web_server_internal.h"

static const char *TAG = "web server - relay";


// ── WS relay client ────────────────────────────────────────────────── 
// Used by esp32 "central" for connecting to "sim"

#define MAX_PENDING_READS 4

// Shared response slot (one at a time — GATT reads are sequential)
static struct {
    SemaphoreHandle_t sem;
    uint32_t          seq;
    char              data_hex[512];
    int               status;
    bool              active;
} s_sync_read = {0};


// Static client state

// static ws_pending_read_t s_pending_reads[MAX_PENDING_READS];
static SemaphoreHandle_t s_pending_mutex = NULL;
static esp_websocket_client_handle_t s_ws_client = NULL;
static SemaphoreHandle_t s_ws_mutex = NULL;

// Init mutex (call once at startup)
void ws_relay_client_init(void) {
    s_ws_mutex = xSemaphoreCreateMutex();
    s_pending_mutex = xSemaphoreCreateMutex();
    s_sync_read.sem = xSemaphoreCreateBinary();
//    memset(s_pending_reads, 0, sizeof(s_pending_reads));
}

static void relay_client_send_json(const char *json_str) {
    if (!s_ws_client || !esp_websocket_client_is_connected(s_ws_client)) {
        ESP_LOGW(TAG, "WS client not connected");
        return;
    }
    ESP_LOGI(TAG, "TX: %s", json_str);
    esp_websocket_client_send_text(s_ws_client, json_str, strlen(json_str), portMAX_DELAY);
}

// Plugged into ble_central_core as the response sender
static void relay_client_response_sender(const char *json_str) {
    relay_client_send_json(json_str);
}

// Blocking version — called from GATT server callback
esp_err_t ws_relay_client_read_sync(const char *svc_uuid, const char *chr_uuid,
                                    char **out_hex, uint32_t timeout_ms) {
    if (!chr_uuid || !out_hex) return ESP_ERR_INVALID_ARG;
    *out_hex = NULL;

    // Arm sync slot with a new seq
    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    static uint32_t s_seq = 0;
    uint32_t seq = ++s_seq;
    s_sync_read.seq          = seq;
    s_sync_read.data_hex[0]  = '\0';
    s_sync_read.status       = -1;
    s_sync_read.active       = true;
    xSemaphoreGive(s_pending_mutex);

    esp_err_t rc = ws_relay_client_send(svc_uuid, chr_uuid, NULL, "read");
    if (rc != ESP_OK) { s_sync_read.active = false; return rc; }

    if (xSemaphoreTake(s_sync_read.sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "Relay read timeout");
        s_sync_read.active = false;
        return ESP_ERR_TIMEOUT;
    }

    if (s_sync_read.status != 0) return ESP_FAIL;
    *out_hex = strdup(s_sync_read.data_hex);
    return ESP_OK;
}



// WS client event handler
static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WS connected to Sim");
            // Send hello so Sim knows our node ID
            relay_client_send_json("{\"type\":\"hello\"}");
            // Route GATT responses back over this socket instead of WS server broadcast
            ble_central_set_relay_sender(relay_client_response_sender);
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WS disconnected from Sim");
            // Restore default sender (back to WS server broadcast)
            ble_central_set_relay_sender(NULL);
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code != WS_TRANSPORT_OPCODES_TEXT || data->data_len == 0) break;

            char buf[512] = {0};
            int len = data->data_len < (int)sizeof(buf)-1 ? data->data_len : (int)sizeof(buf)-1;
            memcpy(buf, data->data_ptr, len);

            cJSON *json = cJSON_Parse(buf);
            if (!json) break;

            cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
            cJSON *act  = cJSON_GetObjectItemCaseSensitive(json, "action");

            if (cJSON_IsString(type) && strcmp(type->valuestring, "relay") == 0
                && cJSON_IsString(act)) {

                const char *action = act->valuestring;

                // ── [we act as "central"] Incoming relay REQUEST from Sim: execute GATT op ──
                if (strcmp(action, "read")  == 0 || strcmp(action, "write") == 0 ||
                    strcmp(action, "write_noresp") == 0 ||
                    strcmp(action, "subscribe")    == 0 ||
                    strcmp(action, "unsubscribe")  == 0) {

                    cJSON *svc   = cJSON_GetObjectItemCaseSensitive(json, "svc");
                    cJSON *chr   = cJSON_GetObjectItemCaseSensitive(json, "chr");
                    cJSON *dat   = cJSON_GetObjectItemCaseSensitive(json, "data");
                    cJSON *j_seq = cJSON_GetObjectItemCaseSensitive(json, "seq");

                    uint32_t seq = cJSON_IsNumber(j_seq) ? (uint32_t)j_seq->valueint : 0;
                    ble_central_set_pending_seq(seq);
                    // no requester needed — response goes back over this socket directly

                    ble_relay_op_t op = BLE_RELAY_OP_UNKNOWN;
                    const char *data_str = cJSON_IsString(dat) ? dat->valuestring : NULL;
                    if      (strcmp(action, "read")         == 0) op = BLE_RELAY_OP_READ;
                    else if (strcmp(action, "write")        == 0) op = BLE_RELAY_OP_WRITE;
                    else if (strcmp(action, "write_noresp") == 0) op = BLE_RELAY_OP_WRITE_NORESP;
                    else if (strcmp(action, "subscribe")    == 0) { 
                        op = BLE_RELAY_OP_SUBSCRIBE;
                        cJSON *j_ind = cJSON_GetObjectItemCaseSensitive(json, "indicate");
                        data_str = (cJSON_IsBool(j_ind) && cJSON_IsTrue(j_ind)) ? "1" : "0";
                    }

                    else if (strcmp(action, "unsubscribe")  == 0) { op = BLE_RELAY_OP_UNSUBSCRIBE; data_str = "0"; }

                    if (op != BLE_RELAY_OP_UNKNOWN)
                        ble_central_relay_op(
                            cJSON_IsString(svc) ? svc->valuestring : NULL,
                            cJSON_IsString(chr) ? chr->valuestring : NULL,
                            data_str, op);

                // ── Incoming relay RESPONSE (read_rsp) matching a sync read ──
                } else if (strcmp(action, "read_rsp") == 0) {
                    cJSON *j_seq  = cJSON_GetObjectItemCaseSensitive(json, "seq");
                    cJSON *j_data = cJSON_GetObjectItemCaseSensitive(json, "data");
                    cJSON *j_sta  = cJSON_GetObjectItemCaseSensitive(json, "status");
                    uint32_t seq  = cJSON_IsNumber(j_seq) ? (uint32_t)j_seq->valueint : 0;

                    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
                    if (s_sync_read.active && s_sync_read.seq == seq) {
                        strncpy(s_sync_read.data_hex,
                                cJSON_IsString(j_data) ? j_data->valuestring : "",
                                sizeof(s_sync_read.data_hex) - 1);
                        s_sync_read.status = cJSON_IsNumber(j_sta) ? (int)j_sta->valuedouble : -1;
                        s_sync_read.active = false;
                        xSemaphoreGive(s_pending_mutex);
                        xSemaphoreGive(s_sync_read.sem);
                    } else {
                        xSemaphoreGive(s_pending_mutex);
                    }
                }
            }
            cJSON_Delete(json);
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WS error");
            break;
        default:
            break;
    }
}


// Connect to target relay server
esp_err_t ws_relay_client_connect(const char *target_ip) {
    if (!target_ip) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);

    // Disconnect existing
    if (s_ws_client) {
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }

    char uri[64];
    snprintf(uri, sizeof(uri), "ws://%s/ws", target_ip);

    esp_websocket_client_config_t config = {
        .uri = uri,
        .buffer_size = 512,
        .task_stack = 4096, 
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
    };

    s_ws_client = esp_websocket_client_init(&config);
    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);

    esp_err_t rc = esp_websocket_client_start(s_ws_client);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WS client: %d", rc);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }

    xSemaphoreGive(s_ws_mutex);
    return rc;
}

// Send relay JSON text frame
esp_err_t ws_relay_client_send(const char *svc_uuid, const char *chr_uuid,
                               const char *data_hex, const char *action) {
    if (!s_ws_client || !esp_websocket_client_is_connected(s_ws_client)) {
        ESP_LOGW(TAG, "WS not connected");
        return ESP_ERR_INVALID_STATE;
    }


    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type",   "relay");
    cJSON_AddStringToObject(root, "action", action);
    if (svc_uuid) cJSON_AddStringToObject(root, "svc",  svc_uuid);
    if (chr_uuid) cJSON_AddStringToObject(root, "chr",  chr_uuid);
    if (data_hex) cJSON_AddStringToObject(root, "data", data_hex);
    cJSON_AddNumberToObject(root, "seq", (double)s_sync_read.seq);  // echo current seq
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "TX: %s", json_str);

    relay_client_send_json(json_str);
    free(json_str);
    return ESP_OK;
}


// Disconnect
void ws_relay_client_disconnect(void) {
    if (s_ws_client) {
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }
}


// HTTP handler: /api/relay/connect
static esp_err_t ws_relay_connect_handler(httpd_req_t *req) {
    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    content[ret] = '\0';

    cJSON *json = cJSON_Parse(content);
    cJSON *target = cJSON_GetObjectItemCaseSensitive(json, "target");
    if (!cJSON_IsString(target)) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing target");
    }

    esp_err_t rc = ws_relay_client_connect(target->valuestring);
    cJSON_Delete(json);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", rc == ESP_OK ? "connected" : "failed");
    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, strlen(str));
    free(str);
    return ESP_OK;
}

static bool relay_ws_handler(const char *type, cJSON *json) {
    if (strcmp(type, "relay") != 0) return false;

    cJSON *act = cJSON_GetObjectItemCaseSensitive(json, "action");
    const char *action = cJSON_IsString(act) ? act->valuestring : NULL;

    // Incoming responses from a central
    if (action && (
        strcmp(action, "read_rsp")       == 0 ||
        strcmp(action, "write_rsp")      == 0 ||
        strcmp(action, "subscribe_rsp")  == 0 ||
        strcmp(action, "read_desc_rsp")  == 0 ||
        strcmp(action, "notify_rx")      == 0 ||
        strcmp(action, "indicate_rx")    == 0
    )) {
        // Only deliver to sim for request-response pairs, not unsolicited events
        if (strcmp(action, "notify_rx") != 0 && strcmp(action, "indicate_rx") != 0) {
            cJSON *j_seq    = cJSON_GetObjectItemCaseSensitive(json, "seq");
            cJSON *j_data   = cJSON_GetObjectItemCaseSensitive(json, "data");
            cJSON *j_status = cJSON_GetObjectItemCaseSensitive(json, "status");
            ble_sim_relay_deliver_rsp(
                cJSON_IsNumber(j_seq)    ? (uint32_t)j_seq->valueint : 0,
                cJSON_IsString(j_data)   ? j_data->valuestring       : NULL,
                cJSON_IsNumber(j_status) ? j_status->valueint        : -1
            );
        }
        return true;
    }

    // Outgoing relay requests to a connected central
    cJSON *svc  = cJSON_GetObjectItemCaseSensitive(json, "svc");
    cJSON *chr  = cJSON_GetObjectItemCaseSensitive(json, "chr");
    cJSON *dat  = cJSON_GetObjectItemCaseSensitive(json, "data");
    cJSON *seq  = cJSON_GetObjectItemCaseSensitive(json, "seq");
    cJSON *src  = cJSON_GetObjectItemCaseSensitive(json, "src");

    if (cJSON_IsNumber(seq)) ble_central_set_pending_seq((uint32_t)seq->valueint);
    if (cJSON_IsString(src)) ble_central_set_pending_requester(src->valuestring);

    const char *svc_str = cJSON_IsString(svc) ? svc->valuestring : NULL;
    const char *chr_str = cJSON_IsString(chr) ? chr->valuestring : NULL;
    const char *data    = cJSON_IsString(dat) ? dat->valuestring : NULL;

    ble_relay_op_t op = BLE_RELAY_OP_UNKNOWN;
    if      (action && strcmp(action, "read")         == 0) op = BLE_RELAY_OP_READ;
    else if (action && strcmp(action, "write")        == 0) op = BLE_RELAY_OP_WRITE;
    else if (action && strcmp(action, "write_noresp") == 0) op = BLE_RELAY_OP_WRITE_NORESP;
    else if (action && strcmp(action, "read_desc") == 0) {
        op = BLE_RELAY_OP_READ_DESC;
        const char *desc_uuid = cJSON_GetStringValue(cJSON_GetObjectItem(json, "desc"));
        data = desc_uuid;
    }

    else if (action && strcmp(action, "subscribe")    == 0) {
        op = BLE_RELAY_OP_SUBSCRIBE;
        cJSON *j_ind = cJSON_GetObjectItemCaseSensitive(json, "indicate");
        data = (cJSON_IsBool(j_ind) && cJSON_IsTrue(j_ind)) ? "1" : "0";
    }
    else if (action && strcmp(action, "unsubscribe")  == 0) {
        op = BLE_RELAY_OP_UNSUBSCRIBE;
        data = "0";
    }

    if (op != BLE_RELAY_OP_UNKNOWN)
        ble_central_relay_op(svc_str, chr_str, data, op);
    else
        ESP_LOGW(TAG, "relay: unknown action '%s'", action ?: "null");

    return true;
}


// HTTP handler: /api/relay/disconnect
static esp_err_t ws_relay_disconnect_handler(httpd_req_t *req) {

    ws_relay_client_disconnect();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "disconnected");
    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, strlen(str));
    free(str);
    return ESP_OK;
}


// ── Register handlers ────────────────────────────────────────────────── 

uint8_t register_relay_handlers_in_web_server(httpd_handle_t *server)
{
    uint8_t handler_count = 0;

    ws_register_message_handler(relay_ws_handler);

    httpd_uri_t uri_ws_relay_connect = {
        .uri = "/api/relay/connect",
        .method = HTTP_POST,
        .handler = ws_relay_connect_handler,
        .user_ctx = NULL,
        .is_websocket = false,
    };
    httpd_register_uri_handler(*server, &uri_ws_relay_connect);
    handler_count++;

    httpd_uri_t uri_ws_relay_disconnect = {
        .uri = "/api/relay/disconnect",
        .method = HTTP_GET,
        .handler = ws_relay_disconnect_handler,
        .user_ctx = NULL,
        .is_websocket = false,
    };
    httpd_register_uri_handler(*server, &uri_ws_relay_disconnect);
    handler_count++;

    return handler_count;
}