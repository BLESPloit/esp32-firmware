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


#include "ble/ble_discovery.h"
#include "ble/ble_scan.h"
#include "common/utils.h"
#include "api/web_server.h"
#include "api/web_server_internal.h"

static const char *TAG = "web server - BLE scanner";

extern volatile bool filter_connectable_only;
extern ble_scanned_device_t scanned_devices[MAX_SCANNED_DEVICES];
extern int scanned_device_count;
extern SemaphoreHandle_t scan_device_mutex;


// ── Connection progress ────────────────────────────────────────────────── 

void web_broadcast_connection_progress(const char *addr, const char *phase, const char *status, const char *detail)
{
    cJSON *msg = cJSON_CreateObject();
    if (!msg) return;

    cJSON_AddStringToObject(msg, "type", "connection_progress");

    if (addr && addr[0] != '\0') {
        cJSON_AddStringToObject(msg, "addr", addr);
    }
    if (phase && phase[0] != '\0') {
        cJSON_AddStringToObject(msg, "phase", phase);
    }
    if (status && status[0] != '\0') {
        cJSON_AddStringToObject(msg, "status", status);
    }
    if (detail && detail[0] != '\0') {
        cJSON_AddStringToObject(msg, "detail", detail);
    }

    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!json) return;

    websocket_broadcast_json_transient(json);
    free(json);
}

// add the address automatically from discovery context
void web_broadcast_connection_progress_disc_ctx(discovery_context_t *ctx, const char *phase, const char *status)
{
    char addr_str[18] = {0};
    if (ctx) {
        format_ble_addr(&ctx->device_addr.val, addr_str, sizeof(addr_str));
    }

    web_broadcast_connection_progress(addr_str[0] ? addr_str : NULL, phase, status, NULL);
}

void web_broadcast_connection_progress_connection(discovery_context_t *ctx, const char *status)
{
    web_broadcast_connection_progress_disc_ctx(ctx, "connection", status);
}

void web_broadcast_connection_progress_discovery(discovery_context_t *ctx, const char *status)
{
    web_broadcast_connection_progress_disc_ctx(ctx, "discovery", status);
}

void web_broadcast_connection_progress_reading(discovery_context_t *ctx, const char *status)
{
    web_broadcast_connection_progress_disc_ctx(ctx, "reading", status);
}


// ── BLE scanning ────────────────────────────────────────────────── 

void web_scan_broadcast_status(void) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type",               "scan_status");
    cJSON_AddBoolToObject  (r, "scanning",           is_ble_scanning());
    cJSON_AddBoolToObject  (r, "connectable_filter", filter_connectable_only);
    cJSON_AddNumberToObject(r, "count",              scanned_device_count);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (s) { websocket_broadcast_json(s); free(s); }  // ← saved to state, NOT transient
}



void web_scan_start(bool connectable_only) {
    esp_err_t ret = start_ble_scan(connectable_only);
    web_scan_broadcast_status();
    
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "scan_status");
    cJSON_AddBoolToObject(r, "scanning", ret == ESP_OK);
    cJSON_AddStringToObject(r, "result", ret == ESP_OK ? "started" :
                            ret == ESP_ERR_INVALID_STATE ? "already_running" : "error");
    cJSON_AddBoolToObject(r, "connectable_filter", connectable_only);
    char *s = cJSON_PrintUnformatted(r); cJSON_Delete(r);
    if (s) { websocket_broadcast_json_transient(s); free(s); }
}

void web_scan_stop(void) {
    esp_err_t ret = stop_ble_scan();
    web_scan_broadcast_status();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "scan_status");
    cJSON_AddBoolToObject(r, "scanning", false);
    cJSON_AddStringToObject(r, "result", ret == ESP_OK ? "stopped" : "not_running");
    char *s = cJSON_PrintUnformatted(r); cJSON_Delete(r);
    if (s) { websocket_broadcast_json_transient(s); free(s); }
}

void web_scan_connect(const char *addr, bool save_result, bool open_central,
                      bool read_values, pairing_mode_t mode,
                      pairing_strategy_t strategy, uint32_t pin)
{
    int rc = ble_connect_and_discover(addr, save_result, open_central,
                                      read_values, mode, strategy, pin);


    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "scan_status");
    cJSON_AddStringToObject(r, "result", rc == 0 ? "connecting" :
                            rc == BLE_HS_EALREADY ? "already_connecting" :
                            rc == BLE_HS_EBUSY    ? "busy" :
                            rc == -1              ? "invalid_id" : "error");
    cJSON_AddStringToObject(r, "addr", addr);
    char *s = cJSON_PrintUnformatted(r); cJSON_Delete(r);
    if (s) { websocket_broadcast_json_transient(s); free(s); }
}


static const char *scan_push_reason_str(scan_push_reason_t reason) {
    switch (reason) {
        case SCAN_PUSH_SCAN_RSP:  return "scan_rsp";
        case SCAN_PUSH_ADV_DATA:  return "adv_data";
        case SCAN_PUSH_DUMP:         return "dump";
        case SCAN_PUSH_RSSI:      return "rssi";
        case SCAN_PUSH_LOST:      return "lost";
        default:                  return "new";
    }
}

void web_scan_push_device(const ble_scanned_device_t *dev, scan_push_reason_t reason) {
    char addr_str[18];
    snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             dev->addr[5], dev->addr[4], dev->addr[3],
             dev->addr[2], dev->addr[1], dev->addr[0]);

    if (reason == SCAN_PUSH_RSSI) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "type",   "scan_device");
        cJSON_AddStringToObject(d, "update", "rssi");
        cJSON_AddStringToObject(d, "addr",   addr_str);
        cJSON_AddNumberToObject(d, "rssi",   dev->rssi);
        char *s = cJSON_PrintUnformatted(d);
        cJSON_Delete(d);
        if (s) {
            websocket_broadcast_json_transient(s);
            free(s);
        }
        return;
    }

    char escaped_name[64];
    json_escape_string(dev->name, escaped_name, sizeof(escaped_name));

    char adv_hex[MAX_SCANNED_ADV_DATA_LEN * 2], scan_rsp_hex[MAX_SCANNED_SCAN_RESP_DATA_LEN * 2];
    bytes_to_hex_string(dev->adv_data, dev->adv_data_len, dev->full_adv_len,
                        adv_hex, sizeof(adv_hex));
    if (dev->has_scan_rsp)
        bytes_to_hex_string(dev->scan_rsp, dev->scan_rsp_len, dev->full_scan_rsp_len,
                            scan_rsp_hex, sizeof(scan_rsp_hex));
    else
        strcpy(scan_rsp_hex, "");

    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "type",           "scan_device");
    cJSON_AddStringToObject(d, "update", scan_push_reason_str(reason));
    cJSON_AddStringToObject(d, "addr",           addr_str);
    cJSON_AddStringToObject(d, "addr_type",      dev->addr_type == BLE_ADDR_PUBLIC ? "public" : "random");
    cJSON_AddNumberToObject(d, "rssi",           dev->rssi);
    cJSON_AddStringToObject(d, "name",           escaped_name);
    cJSON_AddStringToObject(d, "adv_data",       adv_hex);
    cJSON_AddNumberToObject(d, "adv_len",        dev->adv_data_len);
    cJSON_AddNumberToObject(d, "full_adv_len",   dev->full_adv_len);
    cJSON_AddStringToObject(d, "pdu_type",       dev->pdu_type);
    if (dev)
    cJSON_AddStringToObject(d, "scan_rsp",       scan_rsp_hex);
    cJSON_AddBoolToObject  (d, "has_scan_rsp",   dev->has_scan_rsp);
    cJSON_AddBoolToObject  (d, "is_extended",    dev->is_extended);
    cJSON_AddBoolToObject  (d, "is_connectable", dev->is_connectable);
    cJSON_AddStringToObject(d, "phy",            get_phy_name(dev->phy));

    char *s = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    if (s) {
        websocket_broadcast_json_transient(s);
        free(s);
    }
}


void web_scan_dump_all(void) {
    // Optional: still useful for initial load or debug
    // Broadcast each device individually as scan_device messages (transient)
    if (xSemaphoreTake(scan_device_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    for (int i = 0; i < scanned_device_count && i < MAX_SCANNED_DEVICES; i++) {
        if (scanned_devices[i].valid)
            web_scan_push_device(&scanned_devices[i], SCAN_PUSH_DUMP);  // these save to state anyway
    }
    xSemaphoreGive(scan_device_mutex);
    web_scan_broadcast_status();

}

// Call this from discovery_complete() after JSON metadata is annotated,
// before the save/handoff branch.
void web_scan_broadcast_discovery_result(discovery_context_t *ctx, int rc)
{
    if (!ctx) return;

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "scan_discovery_result");

    char addr_str[18];
    snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             ctx->device_addr.val[5], ctx->device_addr.val[4],
             ctx->device_addr.val[3], ctx->device_addr.val[2],
             ctx->device_addr.val[1], ctx->device_addr.val[0]);
    cJSON_AddStringToObject(msg, "addr", addr_str);
    cJSON_AddNumberToObject(msg, "rc", rc);
    cJSON_AddBoolToObject(msg, "viable",
        ctx->services != NULL && ctx->phase >= DISC_PHASE_DESCRIPTORS);

    // Always called after build_services_json() — json_services is populated
    if (ctx->json_services)
        cJSON_AddItemToObject(msg, "services", cJSON_Duplicate(ctx->json_services, true));

    char *s = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (s) { websocket_broadcast_json_transient(s); free(s); }
}

static bool scanner_ws_handler(const char *type, cJSON *json) {
    if (strcmp(type, "scanner") != 0) return false;

    cJSON *act = cJSON_GetObjectItemCaseSensitive(json, "action");
    const char *action = cJSON_IsString(act) ? act->valuestring : NULL;

    if (!action) return true;

    if (strcmp(action, "start") == 0) {
        cJSON *c = cJSON_GetObjectItemCaseSensitive(json, "connectable");
        web_scan_start(cJSON_IsTrue(c));
    }
    else if (strcmp(action, "stop")   == 0) web_scan_stop();
    else if (strcmp(action, "status") == 0) web_scan_dump_all();
    else if (strcmp(action, "connect") == 0) {
        cJSON *j_addr   = cJSON_GetObjectItemCaseSensitive(json, "addr");
        cJSON *j_read   = cJSON_GetObjectItemCaseSensitive(json, "read_values");
        cJSON *j_mode   = cJSON_GetObjectItemCaseSensitive(json, "pairing_mode");
        cJSON *j_strat  = cJSON_GetObjectItemCaseSensitive(json, "strategy");
        cJSON *j_pin    = cJSON_GetObjectItemCaseSensitive(json, "pin");
        cJSON *j_save   = cJSON_GetObjectItem(json, "save_result");
        cJSON *j_central = cJSON_GetObjectItem(json, "open_central");

        if (cJSON_IsString(j_addr)) {
            web_scan_connect(
                j_addr->valuestring,
                cJSON_IsBool(j_save)    ? cJSON_IsTrue(j_save)    : false,
                cJSON_IsBool(j_central) ? cJSON_IsTrue(j_central) : false,
                cJSON_IsBool(j_read)    ? cJSON_IsTrue(j_read)    : false,
                cJSON_IsNumber(j_mode)  ? (pairing_mode_t)j_mode->valueint          : PAIRING_MODE_NONE,
                cJSON_IsNumber(j_strat) ? (pairing_strategy_t)j_strat->valueint     : PAIRING_STRATEGY_LEGACY_JUST_WORKS,
                cJSON_IsNumber(j_pin)   ? (uint32_t)j_pin->valueint                 : 123456
            );
        }
    }
    return true;
}

// BLE scan root handler
esp_err_t httpd_scan_page(httpd_req_t *req) {
    const char *fragments[] = {
        "head.html",
        "scan_body.html",
        "footer.html" 
    };
    return serve_html_fragments(req, fragments, 3);

}


// ── Register handlers ────────────────────────────────────────────────── 

uint8_t register_ble_scanner_handlers_in_web_server(httpd_handle_t *server) {
    ws_register_message_handler(scanner_ws_handler);

    httpd_uri_t uri_ble_scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = httpd_scan_page,
        .user_ctx = NULL,
        .is_websocket = false,
    };

    httpd_register_uri_handler(*server, &uri_ble_scan);
    return 1;
}