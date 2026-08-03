#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_event.h"

#include "common/storage.h"
#include "common/utils.h"

#include "api/web_server.h" // ws broadcast 

#include "ble/ble_central.h"
#include "ble/ble_discovery.h"
#include "ble/ble_discovery_internal.h"
#include "ble/ble_scan.h"


static const char *TAG = "BLE discovery - core";

// ── Global state: discovery ────────────────────────────────────────────────── 

discovery_context_t *g_disc_ctx = NULL;
static uint16_t discovery_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool g_cancelling_for_reconnect = false; 

// external info from ble_scanner
extern ble_scanned_device_t scanned_devices[MAX_SCANNED_DEVICES];
extern int scanned_device_count;

// forward declarations
static int ble_discovery_gap_event_handler(struct ble_gap_event *event, void *arg);

typedef enum {
    DISC_POST_CONNECT_DISCOVERY = 0,
    DISC_POST_CONNECT_PAIRING_RETRY,
    DISC_POST_CONNECT_READ_RETRY,
    DISC_POST_CONNECT_READ_RESUME_DISCOVERY,
} disc_post_connect_action_t;

static disc_post_connect_action_t s_disc_post_connect_action = DISC_POST_CONNECT_DISCOVERY;
static TimerHandle_t s_post_connect_timer = NULL;
static discovery_context_t *s_post_connect_ctx = NULL;

void ble_central_fill_conn_params(struct ble_gap_conn_params *params)
{
    if (!params) {
        return;
    }
    memset(params, 0, sizeof(*params));
    params->scan_itvl = 0x0010;
    params->scan_window = 0x0010;
    params->itvl_min = BLE_CENTRAL_CONN_ITVL_MIN;
    params->itvl_max = BLE_CENTRAL_CONN_ITVL_MAX;
    params->latency = 0;
    params->supervision_timeout = BLE_CENTRAL_CONN_SUPERVISION_TIMEOUT;
    params->min_ce_len = 0;
    params->max_ce_len = 0;
}

int ble_central_infer_own_addr_type(uint8_t *own_addr_type)
{
    if (!own_addr_type) {
        return BLE_HS_EINVAL;
    }
    return ble_hs_id_infer_auto(0, own_addr_type);
}

static void discovery_post_connect_continue(discovery_context_t *ctx)
{
    if (!ctx) {
        return;
    }

    switch (s_disc_post_connect_action) {
    case DISC_POST_CONNECT_PAIRING_RETRY:
        ctx->retrying_pairing_strategy = false;
        start_pairing_phase(ctx);
        break;
    case DISC_POST_CONNECT_READ_RETRY:
        retry_failed_read(ctx);
        break;
    case DISC_POST_CONNECT_READ_RESUME_DISCOVERY:
        start_service_discovery(ctx);
        break;
    case DISC_POST_CONNECT_DISCOVERY:
    default:
        start_service_discovery(ctx);
        break;
    }
}

static int discovery_mtu_exchange_cb(uint16_t conn_handle,
                                    const struct ble_gatt_error *error,
                                    uint16_t mtu, void *arg)
{
    discovery_context_t *ctx = (discovery_context_t *)arg;
    uint16_t negotiated = ble_att_mtu(conn_handle);

    if (error) {
        ESP_LOGW(TAG, "MTU exchange failed: status=%d att_handle=0x%04x; continuing with mtu=%d",
                 error->status, error->att_handle, negotiated);
    } else {
        ESP_LOGI(TAG, "MTU exchange complete; conn=0x%04x mtu=%d (cb mtu=%d)",
                 conn_handle, negotiated, mtu);
    }

    if (ctx && ctx == g_disc_ctx && conn_handle == ctx->conn_handle) {
        discovery_post_connect_continue(ctx);
    }
    return 0;
}

static void discovery_start_post_connect(discovery_context_t *ctx,
                                         disc_post_connect_action_t action)
{
    if (!ctx || ctx->conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    s_disc_post_connect_action = action;

    uint16_t conn = ctx->conn_handle;
    uint16_t cur_mtu = ble_att_mtu(conn);
    if (cur_mtu > 23) {
        ESP_LOGI(TAG, "MTU already %d on conn=0x%04x; skipping exchange", cur_mtu, conn);
        discovery_post_connect_continue(ctx);
        return;
    }

    ESP_LOGI(TAG, "Requesting ATT MTU exchange on conn=0x%04x (current=%d)", conn, cur_mtu);
    int rc = ble_gattc_exchange_mtu(conn, discovery_mtu_exchange_cb, ctx);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gattc_exchange_mtu failed: %d; continuing with default MTU", rc);
        discovery_post_connect_continue(ctx);
    }
}

// Runs outside the NimBLE host task so the GAP callback can return immediately
// and answer peripheral L2CAP Connection Parameter Update requests. 
static void discovery_post_connect_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    discovery_context_t *ctx = s_post_connect_ctx;
    if (ctx && ctx == g_disc_ctx &&
        ctx->conn_handle != BLE_HS_CONN_HANDLE_NONE &&
        ctx->conn_handle == discovery_conn_handle) {
        ESP_LOGI(TAG, "Post-connect settle done; starting ATT on conn=0x%04x",
                 ctx->conn_handle);
        discovery_start_post_connect(ctx, s_disc_post_connect_action);
    } else {
        ESP_LOGW(TAG, "Post-connect timer fired but connection no longer valid");
    }
}

static void discovery_cancel_post_connect(void)
{
    if (s_post_connect_timer) {
        xTimerStop(s_post_connect_timer, 0);
    }
    s_post_connect_ctx = NULL;
}

static void discovery_schedule_post_connect(discovery_context_t *ctx,
                                            disc_post_connect_action_t action,
                                            uint32_t delay_ms)
{
    if (!ctx) {
        return;
    }

    s_disc_post_connect_action = action;
    s_post_connect_ctx = ctx;

    uint32_t ticks = pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 1);
    if (ticks == 0) {
        ticks = 1;
    }

    if (s_post_connect_timer == NULL) {
        s_post_connect_timer = xTimerCreate("disc_pc", ticks, pdFALSE, NULL,
                                            discovery_post_connect_timer_cb);
        if (s_post_connect_timer == NULL) {
            ESP_LOGE(TAG, "Failed to create post-connect timer; starting ATT immediately");
            discovery_start_post_connect(ctx, action);
            return;
        }
    } else {
        xTimerStop(s_post_connect_timer, 0);
    }

    // ChangePeriod also starts a dormant timer. 
    if (xTimerChangePeriod(s_post_connect_timer, ticks, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to arm post-connect timer; starting ATT immediately");
        discovery_start_post_connect(ctx, action);
        return;
    }

    ESP_LOGI(TAG, "Scheduled post-connect ATT in %lu ms (action=%d)",
             (unsigned long)delay_ms, (int)action);
}

// Accept peer L2CAP/LL connection-parameter updates immediately.
// Return 0 = accept, non-zero HCI code = reject. 
static int discovery_handle_conn_update_req(struct ble_gap_event *event)
{
    const struct ble_gap_upd_params *peer = event->conn_update_req.peer_params;
    struct ble_gap_upd_params *self = event->conn_update_req.self_params;
    const char *kind = (event->type == BLE_GAP_EVENT_L2CAP_UPDATE_REQ)
                           ? "L2CAP" : "LL";

    if (peer) {
        ESP_LOGI(TAG, "%s conn update req: itvl=%u-%u lat=%u tmo=%u",
                 kind, peer->itvl_min, peer->itvl_max,
                 peer->latency, peer->supervision_timeout);
    } else {
        ESP_LOGI(TAG, "%s conn update req", kind);
    }

    // LL path exposes self_params for negotiation; L2CAP path may not. 
    if (self && peer) {
        *self = *peer;
        if (self->supervision_timeout < BLE_CENTRAL_CONN_SUPERVISION_TIMEOUT) {
            self->supervision_timeout = BLE_CENTRAL_CONN_SUPERVISION_TIMEOUT;
        }
    }

    return 0; // accept immediately 
}


// ── Helpers ────────────────────────────────────────────────── 

// Helper: find scanned device index by addr string
// Accepts: "AA:BB:CC:DD:EE:FF" or "AABBCCDDEEFF"
static int find_device_by_addr(const char *addr_str) {
    uint8_t target[6];

    int parsed = sscanf(addr_str,
                        "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
                        &target[5], &target[4], &target[3],
                        &target[2], &target[1], &target[0]);

    if (parsed != 6) {
        // Try without colons
        parsed = sscanf(addr_str,
                        "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
                        &target[5], &target[4], &target[3],
                        &target[2], &target[1], &target[0]);

        if (parsed != 6) {
            return -1;
        }
    }

    for (int i = 0; i < scanned_device_count && i < MAX_SCANNED_DEVICES; i++) {
        if (scanned_devices[i].valid &&
            memcmp(scanned_devices[i].addr, target, 6) == 0) {
            return i;
        }
    }

    return -1;
}

// Helper: find an existing GAP connection to a given address
// Returns the conn_handle if found, BLE_HS_CONN_HANDLE_NONE otherwise
static uint16_t find_existing_conn_to_addr(const ble_addr_t *target_addr)
{
    struct ble_gap_conn_desc desc;
    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;

    for (uint16_t h = 0; h < BLE_HS_CONN_HANDLE_NONE; h++) {
        if (ble_gap_conn_find(h, &desc) == 0) {
            if (desc.peer_id_addr.type == target_addr->type &&
                memcmp(desc.peer_id_addr.val, target_addr->val, 6) == 0) {
                conn_handle = h;
                break;
            }
        }
    }
    return conn_handle;
}

// ── Discovery context management ────────────────────────────────────────────────── 
// shared with ble_central
discovery_context_t* create_discovery_context(uint16_t conn_handle, 
                                                     uint8_t device_id,
                                                     bool read_values,
                                                     pairing_mode_t pairing_mode,
                                                     pairing_strategy_t strategy,
                                                     uint32_t pin)
{
    discovery_context_t *ctx = calloc(1, sizeof(discovery_context_t));
    if (!ctx) return NULL;

    ctx->conn_handle = conn_handle;
    ctx->phase = DISC_PHASE_SERVICES;
    ctx->read_values = read_values;
    ctx->services = NULL;
    ctx->current_service = NULL;
    ctx->current_char = NULL;
    ctx->current_desc = NULL;
    ctx->device_id = device_id;
    
    ctx->save_json_on_complete = true;   // default: save
    ctx->handoff_cb  = NULL;
    ctx->handoff_arg = NULL;

    // Initialize retry fields
    ctx->retry_info.handle = 0;
    ctx->retry_info.retry_count = 0;
    ctx->retry_info.last_error = 0;
    ctx->retry_info.is_descriptor = false;
    ctx->retry_in_progress = false;
    ctx->disconnected_during_read = false;
    ctx->reconnect_attempt_count = 0;
    ctx->reconnect_delay_ms = RECONNECT_DELAY_MS;
    ctx->reconnect_attempted = false;
    ctx->retry_service = NULL;
    
    // Initialize pairing info
    memset(&ctx->pairing_info, 0, sizeof(pairing_info_t));
    ctx->pairing_info.pairing_method = 0xFF;
    ctx->pairing_mode = pairing_mode;
    ctx->pairing_strategy = strategy;
    ctx->pairing_strategy_initial = strategy;  // Store original selection
    ctx->pairing_pin = pin;
    ctx->auto_retry_strategies = (strategy == PAIRING_STRATEGY_AUTO);
    ctx->retrying_pairing_strategy = false;
    ctx->any_auth_required = false;
    ctx->pairing_completed = false;

    ctx->json_root = cJSON_CreateObject();
    ctx->json_services = cJSON_CreateArray();
    if (ctx->json_root && ctx->json_services) {
        cJSON_AddItemToObject(ctx->json_root, "services", ctx->json_services);
    }
    
    return ctx;
}



void free_descriptors(stored_descriptor_t *desc)
{
    while (desc) {
        stored_descriptor_t *next = desc->next;
        if (desc->value_hex) free(desc->value_hex);
        free(desc);
        desc = next;
    }
}

void free_characteristics(stored_characteristic_t *chr)
{
    while (chr) {
        stored_characteristic_t *next = chr->next;
        free_descriptors(chr->descriptors);
        if (chr->value_hex) free(chr->value_hex);
        free(chr);
        chr = next;
    }
}

void free_services(stored_service_t *svc)
{
    while (svc) {
        stored_service_t *next = svc->next;
        free_characteristics(svc->characteristics);
        free(svc);
        svc = next;
    }
}

void destroy_discovery_context(discovery_context_t *ctx)
{
    if (!ctx) return;
    
    free_services(ctx->services);
    
    if (ctx->json_root) {
        cJSON_Delete(ctx->json_root);
    }
    
    free(ctx);
}


// ── Discovery completion ────────────────────────────────────────────────── 

static const char* phase_to_string(disc_phase_t phase) {
    switch (phase) {
        case DISC_PHASE_IDLE: return "idle";
        case DISC_PHASE_SERVICES: return "service_discovery";
        case DISC_PHASE_CHARACTERISTICS: return "characteristic_discovery";
        case DISC_PHASE_DESCRIPTORS: return "descriptor_discovery";
        case DISC_PHASE_READING_VALUES: return "reading_values";
        case DISC_PHASE_PAIRING: return "pairing";
        case DISC_PHASE_READING_SECURED: return "reading_secured";
        case DISC_PHASE_COMPLETE: return "complete";
        default: return "unknown";
    }
}

// Is the connection still live and the service map usable?
static bool discovery_handoff_viable(discovery_context_t *ctx, int rc)
{
    // Connection must still be open
    if (ctx->conn_handle == BLE_HS_CONN_HANDLE_NONE)
        return false;

    // Must have gotten at least through characteristic discovery
    if (ctx->services == NULL)
        return false;

    if (ctx->phase < DISC_PHASE_DESCRIPTORS)
        return false;

    // rc == 0 is always fine; non-zero is fine if it's a post-handles error
    // (value reads, pairing, secured reads) — not a structural failure
    return true;
}



void discovery_complete(discovery_context_t *ctx, int rc)
{
    if (!ctx) return;

    // Prevent double completion — unchanged
    static bool completing = false;
    if (completing) {
        ESP_LOGW(TAG, "discovery_complete already in progress, ignoring");
        return;
    }
    completing = true;

    if (rc != 0) {
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "error: %d (phase=%d)", rc, ctx->phase);
        ESP_LOGW(TAG, "Discovery completed with %s", err_msg);
        web_broadcast_connection_progress_discovery(ctx, err_msg);
    } else {
        ESP_LOGI(TAG, "Discovery complete successfully");
    }

    // ── SAVE PATH ────────────────────────────────────────────────────────────
    // Pairing flush + JSON metadata are cheap; always do them if there's data,
    // regardless of whether we save — keeps the context consistent.
    bool has_data = (ctx->services != NULL || ctx->pairing_info.pairing_attempted);

    if (has_data) {
        if (ctx->pairing_info.pairing_attempted) {
            cJSON *attempts_array = cJSON_GetObjectItem(ctx->json_root, "pairing_attempts");
            if (!attempts_array || cJSON_GetArraySize(attempts_array) == 0) {
                ESP_LOGI(TAG, "Storing final pairing attempt before completion");
                store_pairing_attempt_info(ctx);
            }
        }

        if (ctx->json_root) {
            cJSON_AddNumberToObject(ctx->json_root, "error_code", rc);
            cJSON_AddBoolToObject(ctx->json_root, "partial_discovery", rc != 0);
            cJSON_AddStringToObject(ctx->json_root, "completion_phase",
                                    phase_to_string(ctx->phase));
            if (ctx->phase != DISC_PHASE_COMPLETE)
                cJSON_AddBoolToObject(ctx->json_root, "warning_incomplete", true);
        }

        // only write to LittleFS if caller asked for it (default: true)
        if (ctx->save_json_on_complete) {
            ESP_LOGI(TAG, "Saving %s discovery results", rc != 0 ? "PARTIAL" : "complete");
            build_json_and_finish(ctx); 
        } else {
            // Handoff path: not saving to LittleFS, but still need json_services
            // populated for the broadcast. build_json_and_finish() does both —
            // call it, but skip the actual file write by... 
            // Actually simpler: just build the JSON here without saving.
            build_services_json(ctx);  
            ESP_LOGI(TAG, "Skipping JSON save (handoff mode)");
        }
        web_scan_broadcast_discovery_result(ctx, rc);
    } else {
        ESP_LOGW(TAG, "No data discovered, skipping save");
    }

    // ── HANDOFF PATH ─────────────────────────────────────────────────────────
    if (ctx->handoff_cb && discovery_handoff_viable(ctx, rc)) {
        discovery_handoff_cb_t cb = ctx->handoff_cb;
        void *arg                 = ctx->handoff_arg;
        uint16_t conn_handle      = ctx->conn_handle;

        // Relinquish global ownership BEFORE the callback takes over —
        // callback may immediately start using the connection.
        g_disc_ctx              = NULL;
        discovery_conn_handle   = BLE_HS_CONN_HANDLE_NONE;

        // ctx ownership transfers to callee; do NOT destroy or disconnect.
        completing = false;
        cb(conn_handle, ctx, arg);
        return;
    } else if (ctx->handoff_cb && !discovery_handoff_viable(ctx, rc)) {
        ESP_LOGW(TAG, "Handoff requested but not viable (phase=%d, rc=%d), "
                    "falling through to cleanup", ctx->phase, rc);
        // fall through to destroy+disconnect
    }

    // ── DEFAULT CLEANUP ───────────────────────────────────────────────────────
    // Destroy first, then disconnect — so the GAP disconnect event
    // that follows cannot reach a dangling context pointer.
    destroy_discovery_context(ctx);
    g_disc_ctx = NULL;

    if (discovery_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "Disconnecting...");
        ble_gap_terminate(discovery_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    completing = false;
}

// ── Connectiona and discovery API ────────────────────────────────────────────────── 

int ble_connect_and_discover(const char *addr_str, bool save_result, bool open_central, bool read_values, 
                            pairing_mode_t pairing_mode, pairing_strategy_t strategy, uint32_t pin)
{
    int rc;
    struct ble_gap_conn_params conn_params = {0};

    int id = find_device_by_addr(addr_str);
    
    // Check device ID
    if (id >= scanned_device_count) {
        ESP_LOGE(TAG, "Device ID %d out of range (have %d devices)", id, scanned_device_count);
        return ESP_FAIL;
    }
    
    // Validate strategy
    if (strategy >= PAIRING_STRATEGY_AUTO) {
        ESP_LOGW(TAG, "Invalid strategy %d, using AUTO", strategy);
        strategy = PAIRING_STRATEGY_LEGACY_JUST_WORKS;
    }
    
    // Validate PIN (must be 6 digits)
    if ((strategy == PAIRING_STRATEGY_LEGACY_PIN || strategy == PAIRING_STRATEGY_SC_PIN) &&
        (pin > 999999)) {
        ESP_LOGW(TAG, "Invalid PIN %lu, using default 123456", (unsigned long)pin);
        pin = 123456;
    }

    // Build target address early so we can query GAP
    ble_addr_t target_addr;
    target_addr.type = scanned_devices[id].addr_type;
    memcpy(target_addr.val, scanned_devices[id].addr, 6);

    // Check if already connected to the same device (even if handle was handed off)
    uint16_t existing_handle = find_existing_conn_to_addr(&target_addr);
    if (existing_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "Already connected to target device (conn_handle=0x%04x), reusing",
                existing_handle);

        if (open_central) {
            ESP_LOGI(TAG, "open_central=1, connection alive — reattaching central");

            // Broadcast cached services to the client
            discovery_context_t *temp_ctx = create_discovery_context(
                existing_handle, id, false, PAIRING_MODE_NONE, PAIRING_STRATEGY_AUTO, 0);
            if (temp_ctx) {
                temp_ctx->services = ble_central_get_services();  // borrow
                temp_ctx->save_json_on_complete = false;
                memcpy(&temp_ctx->device_addr, &target_addr, sizeof(ble_addr_t));
                build_services_json(temp_ctx);
                web_scan_broadcast_discovery_result(temp_ctx, 0);
                temp_ctx->services = NULL;  // detach before destroy
                destroy_discovery_context(temp_ctx);
            }

            return ble_central_reattach(existing_handle) == ESP_OK ? 0 : ESP_FAIL;
        }


        // For non-central reuse: redo discovery on the live connection
        discovery_conn_handle = existing_handle;

        if (g_disc_ctx) {
            destroy_discovery_context(g_disc_ctx);
            g_disc_ctx = NULL;
        }
        g_disc_ctx = create_discovery_context(existing_handle, id, read_values,
                                            pairing_mode, strategy, pin);
        if (!g_disc_ctx) {
            ESP_LOGE(TAG, "Failed to create discovery context");
            return ESP_FAIL;
        }
        g_disc_ctx->save_json_on_complete = save_result;

        memcpy(&g_disc_ctx->device_addr, &target_addr, sizeof(ble_addr_t));

        ble_sm_set_pairing_rsp_cb(pairing_response_callback, NULL);
        ESP_LOGI(TAG, "Reusing connection, starting service discovery");
        start_service_discovery(g_disc_ctx);
        return 0;
    }

    // Not connected to this device — check for a different active connection and disconnect it first
    // TBD: handle multiple connections simultaneously?
    if (discovery_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "Disconnecting existing connection to different device...");
        rc = ble_gap_terminate(discovery_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc == 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        discovery_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }

    // Cancel ongoing connection attempt
    if (ble_gap_conn_active()) {
        ESP_LOGW(TAG, "GAP connection already active, cancelling...");
        g_cancelling_for_reconnect = true;
        rc = ble_gap_conn_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY && rc != BLE_HS_EDONE) {
            ESP_LOGE(TAG, "Failed to cancel active connection: %d", rc);
            g_cancelling_for_reconnect = false; 
        } else {
            ESP_LOGI(TAG, "Active connection cancelled (rc=%d)", rc);
            vTaskDelay(pdMS_TO_TICKS(300)); // give GAP time to deliver the event (GAP_EVENT_CONNECT with status = 0x09)
        }
        vTaskDelay(pdMS_TO_TICKS(200)); // give more time for GAP state to clear
    }

    // Stop scan — treat EDONE and EALREADY as non-errors
    rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY && rc != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Failed to cancel scan: rc=%d", rc);
    } else if (rc != 0) {
        ESP_LOGD(TAG, "Scan cancel returned rc=%d (not scanning, ok)", rc);
    }
    set_ble_scanning(false);

    vTaskDelay(pdMS_TO_TICKS(150));
    
    ble_central_fill_conn_params(&conn_params);
    
    // Prepare target address
    ble_addr_t addr;
    addr.type = scanned_devices[id].addr_type;
    memcpy(addr.val, scanned_devices[id].addr, 6);
    
    if (g_disc_ctx) {
        destroy_discovery_context(g_disc_ctx);
        g_disc_ctx = NULL;
    }
    
    // Create discovery context with strategy and PIN
    g_disc_ctx = create_discovery_context(BLE_HS_CONN_HANDLE_NONE, id, read_values, 
                                         pairing_mode, strategy, pin);
    if (!g_disc_ctx) {
        ESP_LOGE(TAG, "Failed to create discovery context");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connection parameters: read_values=%d, save_result=%d, open_central=%d, pairing_mode=%d, strategy=%d, pin=%06lu",
             read_values, save_result, open_central, pairing_mode, strategy, (unsigned long)pin);

    g_disc_ctx->save_json_on_complete = save_result;
    if (open_central) {
        g_disc_ctx->handoff_cb  = ble_central_attach_from_discovery;
        g_disc_ctx->handoff_arg = NULL;   // central doesn't need extra arg
    }

    // Register pairing response callback (NimBLE patch)
    // This allows us to have access to the internal pairing details
    ble_sm_set_pairing_rsp_cb(pairing_response_callback, NULL);

    // Schedule a host reset to flush it, then wait for the NimBLE task to process it.
    ble_hs_sched_reset(0);
    vTaskDelay(pdMS_TO_TICKS(200));    

    uint8_t own_addr_type;
    rc = ble_central_infer_own_addr_type(&own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer own addr type: %d", rc);
        destroy_discovery_context(g_disc_ctx);
        g_disc_ctx = NULL;
        return rc;
    }

    // Initiate connection
    rc = ble_gap_connect(own_addr_type, &addr, 30000, &conn_params,
                        ble_discovery_gap_event_handler, NULL);
    
    if (rc != 0) {
        // Detailed error diagnosis
        const char *err_hint = "";
        if (rc == BLE_HS_EALREADY)
            err_hint = " (BLE_HS_EALREADY: GAP procedure already in progress)";
        else if (rc == BLE_HS_EDONE)
            err_hint = " (BLE_HS_EDONE: GAP already in done/active state - "
                    "previous connection or scan not fully cancelled)";
        else if (rc == BLE_HS_EBUSY)
            err_hint = " (BLE_HS_EBUSY: another procedure still pending)";
        else if (rc == BLE_HS_EINVAL)
            err_hint = " (BLE_HS_EINVAL: invalid address or parameters)";
        else if (rc == BLE_HS_ENOTSYNCED)
            err_hint = " (BLE_HS_ENOTSYNCED: host not yet synced with controller)";

        ESP_LOGE(TAG, "ble_gap_connect failed: rc=%d%s", rc, err_hint);
        ESP_LOGE(TAG, "  -> peer addr type=%d, own=%d, conn_active=%d, disc_active=%d",
                addr.type, own_addr_type, ble_gap_conn_active(), ble_gap_disc_active());
        destroy_discovery_context(g_disc_ctx);
        g_disc_ctx = NULL;
        return rc;
    }
   
    ESP_LOGI(TAG, "Connection initiated...");

    return 0;
}

// ── Discovery event handler ────────────────────────────────────────────────── 

static int ble_discovery_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;
    
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:

        ESP_LOGI(TAG, "Connection %s; status=%d",
                event->connect.status == 0 ? "established" : "failed",
                event->connect.status);
        
        if (event->connect.status == 0) {
            discovery_conn_handle = event->connect.conn_handle;
            
            struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(discovery_conn_handle, &desc) == 0 && g_disc_ctx) {
                memcpy(&g_disc_ctx->device_addr, &desc.peer_id_addr, sizeof(ble_addr_t));
                g_disc_ctx->conn_handle = discovery_conn_handle;

                web_broadcast_connection_progress_connection(g_disc_ctx, "connected");

                // Do not block the NimBLE host task here — peripherals often
                // send L2CAP Connection Parameter Update immediately. 
                if (g_disc_ctx->retrying_pairing_strategy && g_disc_ctx->auto_retry_strategies) {
                    ESP_LOGI(TAG, "AUTO mode: Reconnected for pairing strategy retry");
                    discovery_schedule_post_connect(g_disc_ctx,
                                                    DISC_POST_CONNECT_PAIRING_RETRY,
                                                    BLE_CENTRAL_POST_CONNECT_SETTLE_MS);
                } else if (g_disc_ctx->disconnected_during_read) {
                    ESP_LOGI(TAG, "Reconnected successfully! Resuming value reading...");
                    g_disc_ctx->disconnected_during_read = false;

                    if (g_disc_ctx->retry_in_progress) {
                        discovery_schedule_post_connect(g_disc_ctx,
                                                        DISC_POST_CONNECT_READ_RETRY,
                                                        BLE_CENTRAL_POST_CONNECT_SETTLE_MS);
                    } else {
                        ESP_LOGW(TAG, "Reconnected but no retry pending, resuming discovery");
                        discovery_schedule_post_connect(g_disc_ctx,
                                                        DISC_POST_CONNECT_READ_RESUME_DISCOVERY,
                                                        BLE_CENTRAL_POST_CONNECT_SETTLE_MS);
                    }
                } else {
                    discovery_schedule_post_connect(g_disc_ctx,
                                                    DISC_POST_CONNECT_DISCOVERY,
                                                    BLE_CENTRAL_POST_CONNECT_SETTLE_MS);
                }
            }
        } else {
            // Connection failed
            ESP_LOGE(TAG, "BLE_GAP_EVENT_CONNECT failed: status=%d (HCI error: 0x%02x - %s)",
                    event->connect.status,
                    event->connect.status,
                    event->connect.status == 0x3e ? "Connection Failed to be Established" :
                    event->connect.status == 0x08 ? "Connection Timeout" :
                    event->connect.status == 0x09 ? "Connection limit exceeded (expected result of cancel)" :
                    event->connect.status == 0x02 ? "Unknown Connection ID" :
                    "see BLE HCI error codes");
            discovery_conn_handle = BLE_HS_CONN_HANDLE_NONE;

            if (g_cancelling_for_reconnect) {
                ESP_LOGI(TAG, "Ignoring failed connect: was cancelled for reconnect");
                g_cancelling_for_reconnect = false;
                discovery_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                // Do NOT call discovery_complete() — let the new connection proceed
                return 0;
            }

            web_broadcast_connection_progress_connection(g_disc_ctx, "failed"); // here we don't have the address in the ctx (will be 00:00...)

            if (g_disc_ctx) {
                if (g_disc_ctx->retrying_pairing_strategy || g_disc_ctx->disconnected_during_read) {
                    ESP_LOGI(TAG, "Reconnection failed, will retry");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    
                    if (g_disc_ctx->reconnect_attempt_count < MAX_RECONNECT_ATTEMPTS) {
                        reconnect_and_resume(g_disc_ctx);
                    } else {
                        discovery_complete(g_disc_ctx, event->connect.status);
                    }
                } else {
                    discovery_complete(g_disc_ctx, event->connect.status);

                }
            }
        }
        break;

    
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnect; reason=0x%04x (%d)", event->disconnect.reason, event->disconnect.reason);
        discovery_cancel_post_connect();
        discovery_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        
        if (g_disc_ctx) {
            g_disc_ctx->conn_handle = BLE_HS_CONN_HANDLE_NONE;

            web_broadcast_connection_progress_connection(g_disc_ctx, "disconnected"); 

            // 0x216 = BLE_HS_HCI_ERR(0x3e) = CONNECTION_FAILED_TO_BE_ESTABLISHED
            // NimBLE will auto-reattempt — hold the context and let it reconnect
            if (event->disconnect.reason == 0x0216) {
                ESP_LOGI(TAG, "Transient connection failure (0x3e), NimBLE reattempting — holding context");
                return 0;  // ← do NOT call discovery_complete(), do NOT destroy context
            }

            // In case disconnect was during pairing/reading secured chars then store the pairing attempt (if not yet stored)
            if ((g_disc_ctx->phase == DISC_PHASE_PAIRING || 
                g_disc_ctx->phase == DISC_PHASE_READING_SECURED) &&
                g_disc_ctx->pairing_info.pairing_attempted) {
                
                // Check if not already stored
                cJSON *attempts = cJSON_GetObjectItem(g_disc_ctx->json_root, "pairing_attempts");
                if (!attempts || cJSON_GetArraySize(attempts) == 0) {
                    ESP_LOGI(TAG, "Storing pairing info due to disconnect during phase %d", 
                            g_disc_ctx->phase);
                    store_pairing_attempt_info(g_disc_ctx);
                }
            }

            // Check if disconnect was due to PROBE_ONLY abort
            if (g_disc_ctx->pairing_info.aborted_after_response) {
                ESP_LOGI(TAG, "Disconnect after pairing response probe - discovery complete");
                discovery_complete(g_disc_ctx, 0);
            }
            // Check if we're retrying pairing strategies (AUTO mode)
            else if (g_disc_ctx->retrying_pairing_strategy && g_disc_ctx->auto_retry_strategies) {
                ESP_LOGI(TAG, "AUTO mode: Disconnect for pairing strategy retry - reconnecting...");
                
                vTaskDelay(pdMS_TO_TICKS(1000));
                
                // Reconnect to try next strategy
                struct ble_gap_conn_params conn_params = {0};
                ble_central_fill_conn_params(&conn_params);

                uint8_t own_addr_type;
                int rc = ble_central_infer_own_addr_type(&own_addr_type);
                if (rc == 0) {
                    rc = ble_gap_connect(own_addr_type, &g_disc_ctx->device_addr, 30000,
                                         &conn_params, ble_discovery_gap_event_handler, NULL);
                }
                
                if (rc != 0) {
                    ESP_LOGE(TAG, "Reconnection for pairing retry failed: %d", rc);
                    g_disc_ctx->retrying_pairing_strategy = false;
                    discovery_complete(g_disc_ctx, rc);
                } else {
                    ESP_LOGI(TAG, "Reconnecting to try next pairing strategy...");
                }
            }
            // Check if we need to reconnect for read retry
            else if (g_disc_ctx->disconnected_during_read && 
                    g_disc_ctx->reconnect_attempt_count < MAX_RECONNECT_ATTEMPTS) {
                ESP_LOGI(TAG, "Disconnect during read - initiating reconnection");
                vTaskDelay(pdMS_TO_TICKS(2000));
                reconnect_and_resume(g_disc_ctx);
            }
            else if (g_disc_ctx->disconnected_during_read && 
                    g_disc_ctx->reconnect_attempt_count >= MAX_RECONNECT_ATTEMPTS) {
                ESP_LOGE(TAG, "Max reconnection attempts reached");
                discovery_complete(g_disc_ctx, event->disconnect.reason);
            }
            else {
                // Premature or normal disconnect
                if (g_disc_ctx->phase != DISC_PHASE_COMPLETE) {
                    ESP_LOGW(TAG, "Premature disconnect at phase %d (%s), saving partial results", 
                            g_disc_ctx->phase, phase_to_string(g_disc_ctx->phase));
                } else {
                    ESP_LOGI(TAG, "Normal disconnect after completion");
                }
                discovery_complete(g_disc_ctx, event->disconnect.reason);
            }
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "Connection updated; status=%d", event->conn_update.status);
        break;

    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        return discovery_handle_conn_update_req(event);
        
    case BLE_GAP_EVENT_REATTEMPT_COUNT:
        ESP_LOGI(TAG, "Connection reattempt count: %d", event->reattempt_cnt.count);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "GAP MTU update; conn=0x%04x mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        ESP_LOGI(TAG, "Notification; handle=0x%04x", event->notify_rx.attr_handle);
        break;

    // dispatch SMP/security related events to _smp.c 
    case BLE_GAP_EVENT_ENC_CHANGE:  
    case BLE_GAP_EVENT_PASSKEY_ACTION:
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:       
    case BLE_GAP_EVENT_REPEAT_PAIRING:
    case BLE_GAP_EVENT_PARING_COMPLETE:
        return ble_smp_handle_gap_event(event, arg);

    default:
        ESP_LOGW(TAG, "Unhandled GAP event: %d", event->type);
        break;
    }
    
    return 0;
}


esp_err_t ble_discovery_cleanup(void) {
    int rc;
    
    ESP_LOGI(TAG, "Deinitializing BLE discovery...");

    discovery_cancel_post_connect();
       
    // Disconnect from any connected device (central role)
    if (discovery_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        rc = ble_gap_terminate(discovery_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) {
            ESP_LOGW(TAG, "Failed to terminate connection: %d", rc);
        }
        discovery_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    
    // Wait for operations to complete
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Clean up discovery context if active
    if (g_disc_ctx != NULL) {
        ESP_LOGI(TAG, "Cleaning up active discovery context...");
        destroy_discovery_context(g_disc_ctx);
        g_disc_ctx = NULL;
    }
        
    ESP_LOGI(TAG, "BLE discovery deinitialized successfully");
    
    return ESP_OK;
}



// ── Reconnection logic ────────────────────────────────────────────────── 

void reconnect_and_resume(discovery_context_t *ctx)
{
    if (!ctx) return;
    
    ctx->reconnect_attempt_count++;
    
    if (ctx->reconnect_attempt_count > MAX_RECONNECT_ATTEMPTS) {
        ESP_LOGE(TAG, "Max reconnection attempts (%d) reached", MAX_RECONNECT_ATTEMPTS);
        discovery_complete(ctx, BLE_HS_ENOTCONN);
        return;
    }
    
    ESP_LOGI(TAG, "Attempting reconnection %d/%d...",
             ctx->reconnect_attempt_count, MAX_RECONNECT_ATTEMPTS);
    
    web_broadcast_connection_progress_connection(g_disc_ctx, "reconnecting");

    struct ble_gap_conn_params conn_params = {0};
    ble_central_fill_conn_params(&conn_params);

    uint8_t own_addr_type;
    int rc = ble_central_infer_own_addr_type(&own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer own addr type: %d", rc);
        discovery_complete(ctx, rc);
        return;
    }
    
    // Initiate reconnection
    rc = ble_gap_connect(own_addr_type, &ctx->device_addr, 30000,
                         &conn_params, ble_discovery_gap_event_handler, NULL);
    
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed with error: %d", rc);
        
        // If it's still "already in progress", something is wrong with state
        if (rc == BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "BLE_HS_EALREADY - GAP state not clean, resetting");
            
            // Try to cancel any phantom connection
            ble_gap_conn_cancel();
            
            // Wait and retry
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            if (ctx->reconnect_attempt_count < MAX_RECONNECT_ATTEMPTS) {
                reconnect_and_resume(ctx);
            } else {
                discovery_complete(ctx, rc);
            }
        } else {
            // Other error - retry with backoff
            if (ctx->reconnect_attempt_count < MAX_RECONNECT_ATTEMPTS) {
                uint32_t delay = 1000 * (1 << ctx->reconnect_attempt_count);
                ESP_LOGI(TAG, "Retrying in %lu ms...", delay);
                vTaskDelay(pdMS_TO_TICKS(delay));
                reconnect_and_resume(ctx);
            } else {
                discovery_complete(ctx, rc);
            }
        }
    } else {
        ESP_LOGI(TAG, "Reconnection initiated successfully");
    }
}

