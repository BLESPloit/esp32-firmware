#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_event.h"
#include "cJSON.h"

#include "api/web_server.h"
#include "common/storage.h"
#include "common/utils.h"
#include "ble/ble_discovery.h"
#include "ble/ble_discovery_internal.h"

static const char *TAG = "BLE discovery - SMP";

// Pairing attempt tracking for AUTO mode
pairing_attempt_context_t g_pairing_attempt = {0};

#define PAIRING_RESPONSE_TIMEOUT_MS 5000
#define PAIRING_ENCRYPTION_TIMEOUT_MS 10000
#define PAIRING_SECURED_READ_TIMEOUT_MS 3000
#define DEFAULT_PIN 123456


const char* get_pairing_error_string(int status); // ble_sim_smp.c


// simple broadcast status helper (no heavy cJSON)
static void web_broadcast_smp_progress(
    const char *event,
    uint16_t conn_handle,
    int status,
    const char *detail
)
{
    char detail_escaped[96] = {0};
    char json[256] = {0};

    if (!detail) {
        detail = "";
    }

    snprintf(
        detail_escaped,
        sizeof(detail_escaped),
        "%s",
        detail
    );

    snprintf(
        json,
        sizeof(json),
        "{\"type\":\"smp\",\"event\":\"%s\",\"conn_handle\":%u,\"status\":%d,\"detail\":\"%s\"}",
        event ? event : "unknown",
        (unsigned int)conn_handle,
        status,
        detail_escaped
    );

    websocket_broadcast_json_transient(json);
}


// ── Pairing strategy helpers ────────────────────────────────────────────────── 

const char* get_pairing_strategy_name(pairing_strategy_t strategy)
{
    switch (strategy) {
        case PAIRING_STRATEGY_LEGACY_JUST_WORKS:
            return "Legacy Just Works (NoIO, SC disabled)";
        case PAIRING_STRATEGY_SC_JUST_WORKS:
            return "SC Just Works (NoIO, SC enabled)";
        case PAIRING_STRATEGY_LEGACY_PIN:
            return "Legacy PIN (KeyboardOnly, SC disabled)";
        case PAIRING_STRATEGY_SC_PIN:
            return "SC PIN (KeyboardOnly, SC enabled)";
        case PAIRING_STRATEGY_AUTO:
            return "AUTO (Try all strategies)";
        default:
            return "Unknown";
    }
}

static void configure_security_for_strategy(pairing_strategy_t strategy, uint32_t pin)
{
    ESP_LOGI(TAG, "Configuring security for strategy: %s", get_pairing_strategy_name(strategy));
    
    switch (strategy) {
        case PAIRING_STRATEGY_LEGACY_JUST_WORKS:
            ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
            ble_hs_cfg.sm_bonding = 1;
            ble_hs_cfg.sm_mitm = 0;
            ble_hs_cfg.sm_sc = 0;  // Disable Secure Connections
            ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
            ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
            break;
            
        case PAIRING_STRATEGY_SC_JUST_WORKS:
            ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
            ble_hs_cfg.sm_bonding = 1;
            ble_hs_cfg.sm_mitm = 0;
            ble_hs_cfg.sm_sc = 1;  // Enable Secure Connections
            ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
            ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
            break;
            
        case PAIRING_STRATEGY_LEGACY_PIN:
            ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
            ble_hs_cfg.sm_bonding = 1;
            ble_hs_cfg.sm_mitm = 1;  // MITM required for PIN
            ble_hs_cfg.sm_sc = 0;    // Disable Secure Connections
            ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
            ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
            break;
            
        case PAIRING_STRATEGY_SC_PIN:
            ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
            ble_hs_cfg.sm_bonding = 1;
            ble_hs_cfg.sm_mitm = 1;  // MITM required for PIN
            ble_hs_cfg.sm_sc = 1;    // Enable Secure Connections
            ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
            ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown strategy, using defaults");
            break;
    }
}

static bool should_retry_with_next_strategy(discovery_context_t *ctx)
{
    if (!ctx->auto_retry_strategies) {
        return false; // Manual mode - no auto retry
    }
    
    // Check if we should move to next strategy
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Timeout waiting for pairing response
    if (g_pairing_attempt.waiting_for_response &&
        (now - g_pairing_attempt.attempt_timestamp) > PAIRING_RESPONSE_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Pairing response timeout");
        return true;
    }
    
    // Pairing error occurred
    if (ctx->pairing_info.pairing_attempted && 
        !ctx->pairing_info.pairing_successful &&
        ctx->pairing_info.response_received) {
        ESP_LOGW(TAG, "Pairing failed with error");
        return true;
    }
    
    // Encryption timeout
    if (g_pairing_attempt.waiting_for_encryption &&
        (now - g_pairing_attempt.attempt_timestamp) > PAIRING_ENCRYPTION_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Encryption timeout");
        return true;
    }
    
    // Secured read still failed after pairing
    if (g_pairing_attempt.secured_read_attempted &&
        g_pairing_attempt.secured_read_status != 0 &&
        ctx->pairing_info.pairing_successful) {
        ESP_LOGW(TAG, "Secured read failed despite successful pairing (status: %d)", 
                 g_pairing_attempt.secured_read_status);
        return true;
    }
    
    return false;
}

static void reset_pairing_state(discovery_context_t *ctx)
{
    if (!ctx) return;
    
    // Store previous attempt before resetting
    store_pairing_attempt_info(ctx);
    
    // Reset pairing info in context
    memset(&ctx->pairing_info, 0, sizeof(pairing_info_t));
    ctx->pairing_completed = false;
    ctx->any_auth_required = true;
    
    // Reset attempt tracking (but keep strategy index for AUTO mode)
    g_pairing_attempt.waiting_for_response = false;
    g_pairing_attempt.waiting_for_encryption = false;
    g_pairing_attempt.secured_read_attempted = false;
    g_pairing_attempt.secured_read_status = 0;
}

static void advance_to_next_strategy_with_reconnect(discovery_context_t *ctx);


// ── SMP functions ────────────────────────────────────────────────── 

void pairing_response_callback(uint16_t conn_handle, 
                                     const struct ble_sm_pairing_params *rsp, 
                                     void *arg)
{
    if (!g_disc_ctx || conn_handle != g_disc_ctx->conn_handle) {
        ESP_LOGW(TAG, "Pairing response for unknown connection (handle=%d)", conn_handle);
        return;
    }
    
    ESP_LOGI(TAG, "=== Pairing Response Received ===");
    ESP_LOGI(TAG, "  Strategy: %s", get_pairing_strategy_name(g_disc_ctx->pairing_strategy));
    ESP_LOGI(TAG, "  Connection Handle: 0x%04x", conn_handle);
    ESP_LOGI(TAG, "  IO Capability: %d", rsp->io_cap);
    ESP_LOGI(TAG, "  OOB Data Flag: %d", rsp->oob_data_flag);
    ESP_LOGI(TAG, "  Auth Req: 0x%02x", rsp->authreq);
    ESP_LOGI(TAG, "    - Bonding: %s", (rsp->authreq & 0x01) ? "Yes" : "No");
    ESP_LOGI(TAG, "    - MITM: %s", (rsp->authreq & 0x04) ? "Yes" : "No");
    ESP_LOGI(TAG, "    - SC: %s", (rsp->authreq & 0x08) ? "Yes" : "No");
    ESP_LOGI(TAG, "    - Keypress: %s", (rsp->authreq & 0x10) ? "Yes" : "No");
    ESP_LOGI(TAG, "    - CT2: %s", (rsp->authreq & 0x20) ? "Yes" : "No");
    ESP_LOGI(TAG, "  Max Key Size: %d", rsp->max_enc_key_size);
    ESP_LOGI(TAG, "  Init Key Dist: 0x%02x", rsp->init_key_dist);
    ESP_LOGI(TAG, "  Resp Key Dist: 0x%02x", rsp->resp_key_dist);
    ESP_LOGI(TAG, "================================");
    
    // Store response data
    g_disc_ctx->pairing_info.response_received = true;
    g_disc_ctx->pairing_info.response.io_capability = rsp->io_cap;
    g_disc_ctx->pairing_info.response.oob_data_flag = rsp->oob_data_flag;
    g_disc_ctx->pairing_info.response.auth_req = rsp->authreq;
    g_disc_ctx->pairing_info.response.max_key_size = rsp->max_enc_key_size;
    g_disc_ctx->pairing_info.response.init_key_dist = rsp->init_key_dist;
    g_disc_ctx->pairing_info.response.resp_key_dist = rsp->resp_key_dist;

    web_broadcast_smp_progress("pairing_response_received", conn_handle, 0,
                            get_pairing_strategy_name(g_disc_ctx->pairing_strategy));

    // Mark that we got response
    if (g_disc_ctx->auto_retry_strategies) {
        g_pairing_attempt.waiting_for_response = false;
        g_pairing_attempt.waiting_for_encryption = true;
    }
    
    // If PROBE_ONLY mode, abort pairing now
    if (g_disc_ctx->pairing_mode == PAIRING_MODE_PROBE_ONLY) {
        ESP_LOGI(TAG, "PROBE_ONLY mode: Got pairing response, will disconnect");
        g_disc_ctx->pairing_info.aborted_after_response = true;
        
        vTaskDelay(pdMS_TO_TICKS(10));
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        ESP_LOGI(TAG, "Continuing with pairing (mode: %d)", g_disc_ctx->pairing_mode);
    }
}

void configure_ble_security(void)
{
    if (!g_disc_ctx) {
        ESP_LOGW(TAG, "No discovery context, using defaults");
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
        ble_hs_cfg.sm_bonding = 0;
        ble_hs_cfg.sm_mitm = 0;
        ble_hs_cfg.sm_sc = 0;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
        return;
    }
    
    configure_security_for_strategy(g_disc_ctx->pairing_strategy, g_disc_ctx->pairing_pin);
}

void start_pairing_phase(discovery_context_t *ctx)
{
    if (!ctx) return;
    
    ESP_LOGI(TAG, "=== Starting Pairing Phase ===");
    
    // Check if AUTO mode
    if (ctx->pairing_strategy_initial == PAIRING_STRATEGY_AUTO) {
        ctx->auto_retry_strategies = true;
        
        // Initialize or resume AUTO mode
        if (!ctx->retrying_pairing_strategy) {
            // First attempt - start from beginning
            memset(&g_pairing_attempt, 0, sizeof(pairing_attempt_context_t));
            g_pairing_attempt.current_strategy = PAIRING_STRATEGY_LEGACY_JUST_WORKS;
            ctx->pairing_strategy = PAIRING_STRATEGY_LEGACY_JUST_WORKS;
            ESP_LOGI(TAG, "AUTO mode: Starting with strategy 1/4");
        } else {
            // Resuming after disconnect
            ctx->pairing_strategy = g_pairing_attempt.current_strategy;
            ESP_LOGI(TAG, "AUTO mode: Resuming with strategy %d/4", 
                     ctx->pairing_strategy + 1);
        }
    } else {
        // Manual strategy selection
        ctx->auto_retry_strategies = false;
        ESP_LOGI(TAG, "Manual strategy: %s", get_pairing_strategy_name(ctx->pairing_strategy));
        
        if (ctx->pairing_strategy == PAIRING_STRATEGY_LEGACY_PIN || 
            ctx->pairing_strategy == PAIRING_STRATEGY_SC_PIN) {
            ESP_LOGI(TAG, "PIN: %06lu", (unsigned long)ctx->pairing_pin);
        }
    }
    
    ctx->phase = DISC_PHASE_PAIRING;
    
    configure_ble_security();
    
    // Store our pairing request parameters
    ctx->pairing_info.request.io_capability = ble_hs_cfg.sm_io_cap;
    ctx->pairing_info.request.oob_data_flag = ble_hs_cfg.sm_oob_data_flag;
    ctx->pairing_info.request.auth_req = 
        (ble_hs_cfg.sm_bonding ? 0x01 : 0) |
        (ble_hs_cfg.sm_mitm ? 0x04 : 0) |
        (ble_hs_cfg.sm_sc ? 0x08 : 0);
    ctx->pairing_info.request.max_key_size = 16;
    ctx->pairing_info.request.init_key_dist = ble_hs_cfg.sm_our_key_dist;
    ctx->pairing_info.request.resp_key_dist = ble_hs_cfg.sm_their_key_dist;
    
    // FIXED: Add initial pairing configuration to JSON
    if (ctx->json_root) {
        cJSON_AddStringToObject(ctx->json_root, "pairing_mode", 
                               ctx->pairing_mode == PAIRING_MODE_NONE ? "none" :
                               ctx->pairing_mode == PAIRING_MODE_PROBE_ONLY ? "probe_only" :
                               "complete");
        cJSON_AddStringToObject(ctx->json_root, "pairing_strategy_selected", 
                               get_pairing_strategy_name(ctx->pairing_strategy_initial));
        cJSON_AddBoolToObject(ctx->json_root, "pairing_auto_retry", ctx->auto_retry_strategies);
        
        if (ctx->pairing_strategy == PAIRING_STRATEGY_LEGACY_PIN || 
            ctx->pairing_strategy == PAIRING_STRATEGY_SC_PIN) {
            cJSON_AddNumberToObject(ctx->json_root, "pairing_pin_configured", ctx->pairing_pin);
        }
    }
    
    // Set timeout tracking for AUTO mode
    if (ctx->auto_retry_strategies) {
        g_pairing_attempt.attempt_timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS;
        g_pairing_attempt.waiting_for_response = true;
    }

//    web_broadcast_smp_progress("pairing_starting", ctx->conn_handle, 0,
//                               get_pairing_strategy_name(ctx->pairing_strategy));

    int rc = ble_gap_security_initiate(ctx->conn_handle);
    if (rc == 0) {
        ctx->pairing_info.pairing_attempted = true;
        ESP_LOGI(TAG, "Pairing initiated successfully");
        web_broadcast_smp_progress("pairing_initiated", ctx->conn_handle, 0,
                                   get_pairing_strategy_name(ctx->pairing_strategy));        
    } else {
        ESP_LOGW(TAG, "Failed to initiate pairing: %d", rc);
        web_broadcast_smp_progress("pairing_initiate_failed", ctx->conn_handle, rc,
                            get_pairing_strategy_name(ctx->pairing_strategy));
        
        if (ctx->auto_retry_strategies) {
            // Try next strategy
            reset_pairing_state(ctx);
            g_pairing_attempt.current_strategy++;
            ctx->pairing_strategy = g_pairing_attempt.current_strategy;
            
            if (g_pairing_attempt.current_strategy < PAIRING_STRATEGY_AUTO) {
                vTaskDelay(pdMS_TO_TICKS(500));
                advance_to_next_strategy_with_reconnect(ctx);
            } else {
                ctx->phase = DISC_PHASE_COMPLETE;
                discovery_complete(ctx, 0);
            }
        } else {
            // Manual mode - store failed attempt and complete
            store_pairing_attempt_info(ctx);
            ctx->phase = DISC_PHASE_COMPLETE;
            discovery_complete(ctx, 0);
        }
    }
}


static void advance_to_next_strategy_with_reconnect(discovery_context_t *ctx)
{
    if (!ctx || !ctx->auto_retry_strategies) return;
    
    // Check if we exhausted all strategies
    if (g_pairing_attempt.current_strategy >= PAIRING_STRATEGY_AUTO) {
        ESP_LOGW(TAG, "AUTO mode: All pairing strategies exhausted");
        ctx->phase = DISC_PHASE_COMPLETE;
        discovery_complete(ctx, 0);
        return;
    }
    
    ESP_LOGI(TAG, "=== AUTO Mode: Trying Strategy %d/4: %s ===", 
             g_pairing_attempt.current_strategy + 1,
             get_pairing_strategy_name(g_pairing_attempt.current_strategy));
    
    // Update context strategy
    ctx->pairing_strategy = g_pairing_attempt.current_strategy;
    ctx->retrying_pairing_strategy = true;
    ctx->phase = DISC_PHASE_PAIRING;
    
    // Disconnect to reset pairing state
    ESP_LOGI(TAG, "Disconnecting to reset for next pairing strategy...");
    ble_gap_terminate(ctx->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    // Disconnect event will handle reconnection
}

static int char_secured_read_callback(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     struct ble_gatt_attr *attr,
                                     void *arg)
{
    discovery_context_t *ctx = (discovery_context_t *)arg;
    
    if (!ctx || !ctx->current_char) {
        ESP_LOGE(TAG, "Invalid context in secured read callback");
        return BLE_HS_EINVAL;
    }
    
    // Track secured read result (for both AUTO and manual modes)
    g_pairing_attempt.secured_read_attempted = true;
    g_pairing_attempt.secured_read_status = error->status;

    if (!g_disc_ctx || ctx->conn_handle != conn_handle || 
        ctx->phase == DISC_PHASE_COMPLETE) {
        ESP_LOGW(TAG, "Connection terminated, aborting secured reads");
        return 0;
    }
    
    if (error->status == 0 && attr) {
        ctx->current_char->value_hex = mbuf_to_hex_string(attr->om);
        if (ctx->current_char->value_hex) {
            ESP_LOGI(TAG, "  Secured read successful: %s", ctx->current_char->value_hex);
        }
    } else {
        ESP_LOGW(TAG, "Secured read failed: handle=0x%04x, error=%d", 
                ctx->current_char->val_handle, error->status);
        
        if (should_retry_with_next_strategy(ctx)) {
            ESP_LOGI(TAG, "AUTO mode: Secured read failed, will retry with next strategy");
            reset_pairing_state(ctx);
            g_pairing_attempt.current_strategy++;
            advance_to_next_strategy_with_reconnect(ctx);
            return 0;
        }
    }
    
    start_secured_reads(ctx);
    return 0;
}

static int desc_secured_read_callback(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     struct ble_gatt_attr *attr,
                                     void *arg)
{
    discovery_context_t *ctx = (discovery_context_t *)arg;
    
    if (!ctx || !ctx->current_desc) {
        ESP_LOGE(TAG, "Invalid context in secured descriptor read callback");
        return BLE_HS_EINVAL;
    }
    
    // Track secured read result (for both AUTO and manual modes)
    g_pairing_attempt.secured_read_attempted = true;
    g_pairing_attempt.secured_read_status = error->status;
    
    if (!g_disc_ctx || ctx->conn_handle != conn_handle || 
        ctx->phase == DISC_PHASE_COMPLETE) {
        ESP_LOGW(TAG, "Connection terminated, aborting secured descriptor reads");
        return 0;
    }
    
    if (error->status == 0 && attr) {
        ctx->current_desc->value_hex = mbuf_to_hex_string(attr->om);
        if (ctx->current_desc->value_hex) {
            ESP_LOGI(TAG, "  Secured descriptor read successful: %s", ctx->current_desc->value_hex);
        }
    } else {
        ESP_LOGW(TAG, "Secured descriptor read failed: handle=0x%04x, error=%d", 
                ctx->current_desc->handle, error->status);
        
        if (should_retry_with_next_strategy(ctx)) {
            ESP_LOGI(TAG, "AUTO mode: Secured read failed, will retry with next strategy");
            reset_pairing_state(ctx);
            g_pairing_attempt.current_strategy++;
            advance_to_next_strategy_with_reconnect(ctx);
            return 0;
        }
    }
    
    start_secured_reads(ctx);
    return 0;
}


void start_secured_reads(discovery_context_t *ctx)
{
    if (!ctx) return;

    if (!g_disc_ctx || ctx->phase == DISC_PHASE_COMPLETE) {
        ESP_LOGW(TAG, "Context invalidated, aborting secured reads");
        return;
    }    

    ESP_LOGI(TAG, "=== Starting Secured Reads Phase ===");
    ctx->phase = DISC_PHASE_READING_SECURED;
    
    // Reset position to beginning
    ctx->current_service = ctx->services;
    ctx->current_char = NULL;
    ctx->current_desc = NULL;
    
    // Find first characteristic that requires authentication and hasn't been read
    while (ctx->current_service) {
        stored_characteristic_t *chr = ctx->current_service->characteristics;
        
        while (chr) {
            if (chr->encryption_required && !chr->value_hex) {
                ctx->current_char = chr;
                
                ESP_LOGI(TAG, "Retrying secured characteristic read: handle=0x%04x", chr->val_handle);
                int rc = ble_gattc_read(ctx->conn_handle, chr->val_handle,
                                       char_secured_read_callback, ctx);
                if (rc != 0) {
                    ESP_LOGE(TAG, "Failed to start secured read: %d", rc);
                    discovery_complete(ctx, rc);
                }
                return;
            }
            chr = chr->next;
        }
        
        ctx->current_service = ctx->current_service->next;
    }
    
    // Also check descriptors
    ctx->current_service = ctx->services;
    while (ctx->current_service) {
        stored_characteristic_t *chr = ctx->current_service->characteristics;
        
        while (chr) {
            stored_descriptor_t *desc = chr->descriptors;
            while (desc) {
                if (desc->encryption_required && !desc->value_hex) {
                    ctx->current_char = chr;
                    ctx->current_desc = desc;
                    
                    ESP_LOGI(TAG, "Retrying secured descriptor read: handle=0x%04x", desc->handle);
                    int rc = ble_gattc_read(ctx->conn_handle, desc->handle,
                                           desc_secured_read_callback, ctx);
                    if (rc != 0) {
                        ESP_LOGE(TAG, "Failed to start secured descriptor read: %d", rc);
                        discovery_complete(ctx, rc);
                    }
                    return;
                }
                desc = desc->next;
            }
            chr = chr->next;
        }
        
        ctx->current_service = ctx->current_service->next;
    }
    
    // No more secured reads needed
    ESP_LOGI(TAG, "All secured reads completed");
    
    // FIXED: Always store pairing attempt info (for both AUTO and manual modes)
    store_pairing_attempt_info(ctx);
    
    ctx->phase = DISC_PHASE_COMPLETE;
    discovery_complete(ctx, 0);
}


// ── GAP handler for SMP events ────────────────────────────────────────────────── 

int ble_smp_handle_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;
    
    if (!g_disc_ctx) return 0;
    
    switch (event->type) {

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption change event; status=%d", event->enc_change.status);
        
        if (g_disc_ctx) {
            if (g_disc_ctx->auto_retry_strategies) {
                g_pairing_attempt.waiting_for_encryption = false;
            }
            
            if (event->enc_change.status == 0) {
                ESP_LOGI(TAG, "Link encrypted successfully with strategy: %s", 
                        get_pairing_strategy_name(g_disc_ctx->pairing_strategy));
                g_disc_ctx->pairing_info.pairing_successful = true;
                g_disc_ctx->pairing_info.encrypted = true;
                g_disc_ctx->pairing_completed = true;

                web_broadcast_smp_progress("encryption_success",
                                           event->enc_change.conn_handle,
                                           0,
                                           get_pairing_strategy_name(g_disc_ctx->pairing_strategy));
                
                rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
                if (rc == 0) {
                    ESP_LOGI(TAG, "Security state: encrypted=%d, authenticated=%d, bonded=%d, key_size=%d",
                            desc.sec_state.encrypted,
                            desc.sec_state.authenticated,
                            desc.sec_state.bonded,
                            desc.sec_state.key_size);
                    
                    g_disc_ctx->pairing_info.request.max_key_size = desc.sec_state.key_size;
                    g_disc_ctx->pairing_info.response.max_key_size = desc.sec_state.key_size;
                }
                
                if (g_disc_ctx->phase == DISC_PHASE_PAIRING) {
                    ESP_LOGI(TAG, "Pairing complete, starting secured reads...");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    start_secured_reads(g_disc_ctx);
                }
            } else {
                ESP_LOGW(TAG, "Encryption failed: %d", event->enc_change.status);
                g_disc_ctx->pairing_info.pairing_successful = false;

                web_broadcast_smp_progress("encryption_failed",
                            event->enc_change.conn_handle,
                            event->enc_change.status,
                            get_pairing_error_string(event->enc_change.status));
                
                // Try next strategy if AUTO mode
                if (should_retry_with_next_strategy(g_disc_ctx)) {
                    ESP_LOGI(TAG, "AUTO mode: Encryption failed, will retry with next strategy");
                    reset_pairing_state(g_disc_ctx);
                    g_pairing_attempt.current_strategy++;
                    advance_to_next_strategy_with_reconnect(g_disc_ctx);
                } else {
                    // FIXED: Store failed pairing attempt in manual mode
                    if (!g_disc_ctx->auto_retry_strategies) {
                        store_pairing_attempt_info(g_disc_ctx);
                    }
                    
                    if (g_disc_ctx->phase == DISC_PHASE_PAIRING) {
                        ESP_LOGW(TAG, "Pairing failed, completing discovery without secured values");
                        g_disc_ctx->phase = DISC_PHASE_COMPLETE;
                        discovery_complete(g_disc_ctx, 0);
                    }
                }
            }
        }
        return 0;
    
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(TAG, "Passkey action event; action=%d", event->passkey.params.action);
        
        if (g_disc_ctx) {
            switch (event->passkey.params.action) {
            case BLE_SM_IOACT_NONE:
                g_disc_ctx->pairing_info.pairing_method = 0;
                ESP_LOGI(TAG, "Pairing method: Just Works");
                web_broadcast_smp_progress("passkey_action",
                                           event->passkey.conn_handle,
                                           0,
                                           "just_works");
                break;
                
            case BLE_SM_IOACT_DISP:
                g_disc_ctx->pairing_info.pairing_method = 1;
                ESP_LOGI(TAG, "Display passkey: %06lu", 
                        (unsigned long)event->passkey.params.numcmp);
                web_broadcast_smp_progress("passkey_action",
                            event->passkey.conn_handle,
                            0,
                            "display");
                break;
                
            case BLE_SM_IOACT_NUMCMP:
                g_disc_ctx->pairing_info.pairing_method = 2;
                ESP_LOGI(TAG, "Numeric comparison: %06lu", 
                        (unsigned long)event->passkey.params.numcmp);
                web_broadcast_smp_progress("passkey_action",
                                           event->passkey.conn_handle,
                                           0,
                                           "numeric_comparison");
                        
                if (g_disc_ctx->pairing_mode == PAIRING_MODE_COMPLETE) {
                    struct ble_sm_io io = {0};
                    io.action = event->passkey.params.action;
                    io.numcmp_accept = 1; 
                    rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
                    if (rc != 0) {
                        ESP_LOGW(TAG, "Failed to inject IO: %d", rc);
                        web_broadcast_smp_progress("numeric_comparison_confirm_failed",
                                                   event->passkey.conn_handle,
                                                   rc,
                                                   "inject_failed");                        
                    } else {
                        ESP_LOGI(TAG, "Auto-confirmed numeric comparison");
                        web_broadcast_smp_progress("numeric_comparison_confirmed",
                                                   event->passkey.conn_handle,
                                                   0,
                                                   "auto_confirmed");
                    }
                }
                break;
                
            case BLE_SM_IOACT_INPUT:
                g_disc_ctx->pairing_info.pairing_method = 1;
                ESP_LOGI(TAG, "Peer requires passkey input");
                web_broadcast_smp_progress("passkey_action",
                                           event->passkey.conn_handle,
                                           0,
                                           "input");
                
                // For PIN strategies, inject configured PIN
                if (g_disc_ctx->pairing_strategy == PAIRING_STRATEGY_LEGACY_PIN ||
                    g_disc_ctx->pairing_strategy == PAIRING_STRATEGY_SC_PIN) {
                    
                    struct ble_sm_io io = {0};
                    io.action = event->passkey.params.action;
                    io.passkey = g_disc_ctx->pairing_pin;
                    
                    rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
                    if (rc != 0) {
                        ESP_LOGW(TAG, "Failed to inject PIN: %d", rc);
                        ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                        web_broadcast_smp_progress("pin_inject_failed",
                                                   event->passkey.conn_handle,
                                                   rc,
                                                   "inject_failed");                        
                    } else {
                        ESP_LOGI(TAG, "Injected PIN: %06lu", (unsigned long)g_disc_ctx->pairing_pin);
                        web_broadcast_smp_progress("pin_injected",
                                                   event->passkey.conn_handle,
                                                   0,
                                                   get_pairing_strategy_name(g_disc_ctx->pairing_strategy));
                    }
                } else {
                    ESP_LOGW(TAG, "Passkey input unexpected for current strategy, disconnecting");
                    web_broadcast_smp_progress("pin_input_unexpected",
                                               event->passkey.conn_handle,
                                               -1,
                                               get_pairing_strategy_name(g_disc_ctx->pairing_strategy));
                    ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                }
                break;
                
            case BLE_SM_IOACT_OOB:
                g_disc_ctx->pairing_info.pairing_method = 3;
                ESP_LOGI(TAG, "OOB authentication required (not supported)");
                ESP_LOGW(TAG, "OOB not supported, disconnecting");
                web_broadcast_smp_progress("oob_not_supported",
                                           event->passkey.conn_handle,
                                           -1,
                                           "disconnecting");
                ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                break;
                
            default:
                ESP_LOGW(TAG, "Unknown passkey action: %d", event->passkey.params.action);
                web_broadcast_smp_progress("passkey_action_unknown",
                                           event->passkey.conn_handle,
                                           event->passkey.params.action,
                                           "unknown");
                break;
            }
        }
        break;
        
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
        ESP_LOGI(TAG, "Identity resolved");
        break;
        
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ESP_LOGI(TAG, "Repeat pairing request (device already bonded)");
        
        if (g_disc_ctx) {
            rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
            if (rc == 0) {
                rc = ble_store_util_delete_peer(&desc.peer_id_addr);
                if (rc == 0) {
                    ESP_LOGI(TAG, "Deleted old bond, will retry pairing");
                } else {
                    ESP_LOGW(TAG, "Failed to delete old bond: %d", rc);
                }
            }
        }
        
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PARING_COMPLETE:
    {
        ESP_LOGI(TAG, "Pairing complete; conn_handle=%d status=%d %s",
                event->pairing_complete.conn_handle,
                event->pairing_complete.status,
                event->pairing_complete.status == 0 ? "(SUCCESS)" : "(FAILED)");
        
        // Get connection descriptor to log security details
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->pairing_complete.conn_handle, &desc);
        
        if (rc == 0 && event->pairing_complete.status == 0) {
            web_broadcast_smp_progress("pairing_complete",
                                       event->pairing_complete.conn_handle,
                                       0,
                                       "success");
            // Pairing succeeded - log security state
            ESP_LOGI(TAG, "Security state: encrypted=%d authenticated=%d "
                        "bonded=%d key_size=%d",
                    desc.sec_state.encrypted,
                    desc.sec_state.authenticated,
                    desc.sec_state.bonded,
                    desc.sec_state.key_size);
            
            // Log peer identity address
            ESP_LOGI(TAG, "Peer identity: %02x:%02x:%02x:%02x:%02x:%02x (type=%d)",
                    desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                    desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                    desc.peer_id_addr.val[1], desc.peer_id_addr.val[0],
                    desc.peer_id_addr.type);                    
        } else if (event->pairing_complete.status != 0) {
            // Pairing failed - log error details with decoded error
            const char *error_str = get_pairing_error_string(event->pairing_complete.status);
            web_broadcast_smp_progress("pairing_complete",
                                       event->pairing_complete.conn_handle,
                                       event->pairing_complete.status,
                                       error_str);
            ESP_LOGE(TAG, "Pairing failed with status: 0x%02x (%s)",
                    event->pairing_complete.status, error_str);
        } else {
            ESP_LOGW(TAG, "Could not retrieve connection descriptor: %d", rc);
        }
        
        return 0;
    }


    default:
        break;
    }
    
    return 0;
}
