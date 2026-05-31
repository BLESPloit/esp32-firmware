#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "esp_event.h"
#include "cJSON.h"

#include "ble/ble_discovery.h"
#include "ble/ble_discovery_internal.h"
#include "ble/ble_scan.h"
#include "common/storage.h"
#include "common/utils.h"

static const char *TAG = "BLE discovery - JSON";

// external info from ble_scanner
extern ble_scanned_device_t scanned_devices[MAX_SCANNED_DEVICES];
extern pairing_attempt_context_t g_pairing_attempt;

// ── Json building ────────────────────────────────────────────────── 

// Helper function to decode auth_req bits
static cJSON* decode_auth_req(uint8_t auth_req) {
    cJSON *flags = cJSON_CreateObject();
    cJSON_AddBoolToObject(flags, "bonding", auth_req & 0x01);
    cJSON_AddBoolToObject(flags, "mitm", auth_req & 0x04);
    cJSON_AddBoolToObject(flags, "secure_connections", auth_req & 0x08);
    cJSON_AddBoolToObject(flags, "keypress", auth_req & 0x10);
    cJSON_AddBoolToObject(flags, "ct2", auth_req & 0x20);
    return flags;
}

void store_pairing_attempt_info(discovery_context_t *ctx)
{
    if (!ctx || !ctx->json_root) return;
    
    // Check if already stored (avoid duplicates)
    cJSON *attempts_array = cJSON_GetObjectItem(ctx->json_root, "pairing_attempts");
    if (attempts_array) {
        int array_size = cJSON_GetArraySize(attempts_array);
        if (array_size > 0) {
            cJSON *last_attempt = cJSON_GetArrayItem(attempts_array, array_size - 1);
            cJSON *last_strategy = cJSON_GetObjectItem(last_attempt, "strategy_index");
            if (last_strategy && last_strategy->valueint == ctx->pairing_strategy) {
                ESP_LOGD(TAG, "Pairing attempt already stored for this strategy");
                return;
            }
        }
    } else {
        attempts_array = cJSON_CreateArray();
        cJSON_AddItemToObject(ctx->json_root, "pairing_attempts", attempts_array);
    }
    
    cJSON *attempt = cJSON_CreateObject();
    cJSON_AddStringToObject(attempt, "strategy", get_pairing_strategy_name(ctx->pairing_strategy));
    cJSON_AddNumberToObject(attempt, "strategy_index", ctx->pairing_strategy);
    
    // Add mode info
    if (ctx->auto_retry_strategies) {
        cJSON_AddStringToObject(attempt, "mode", "AUTO");
        cJSON_AddNumberToObject(attempt, "attempt_number", g_pairing_attempt.current_strategy + 1);
    } else {
        cJSON_AddStringToObject(attempt, "mode", "MANUAL");
    }
    
    // Add timestamp
    cJSON_AddNumberToObject(attempt, "timestamp_ms", xTaskGetTickCount() * portTICK_PERIOD_MS);
    
    // Request parameters
    cJSON *request = cJSON_CreateObject();
    cJSON_AddNumberToObject(request, "io_capability", ctx->pairing_info.request.io_capability);
    cJSON_AddNumberToObject(request, "oob_data_flag", ctx->pairing_info.request.oob_data_flag);
    cJSON_AddNumberToObject(request, "auth_req", ctx->pairing_info.request.auth_req);
    cJSON_AddNumberToObject(request, "max_key_size", ctx->pairing_info.request.max_key_size);
    cJSON_AddNumberToObject(request, "init_key_dist", ctx->pairing_info.request.init_key_dist);
    cJSON_AddNumberToObject(request, "resp_key_dist", ctx->pairing_info.request.resp_key_dist);
    cJSON_AddItemToObject(attempt, "request", request);
    
    // Response parameters (if received)
    if (ctx->pairing_info.response_received) {
        cJSON *response = cJSON_CreateObject();
        cJSON_AddNumberToObject(response, "io_capability", ctx->pairing_info.response.io_capability);
        cJSON_AddNumberToObject(response, "oob_data_flag", ctx->pairing_info.response.oob_data_flag);
        cJSON_AddNumberToObject(response, "auth_req", ctx->pairing_info.response.auth_req);
        cJSON_AddNumberToObject(response, "max_key_size", ctx->pairing_info.response.max_key_size);
        cJSON_AddNumberToObject(response, "init_key_dist", ctx->pairing_info.response.init_key_dist);
        cJSON_AddNumberToObject(response, "resp_key_dist", ctx->pairing_info.response.resp_key_dist);
        cJSON_AddItemToObject(attempt, "response", response);
    }
    
    // Pairing outcome
    cJSON_AddBoolToObject(attempt, "pairing_attempted", ctx->pairing_info.pairing_attempted);
    cJSON_AddBoolToObject(attempt, "response_received", ctx->pairing_info.response_received);
    cJSON_AddBoolToObject(attempt, "pairing_successful", ctx->pairing_info.pairing_successful);
    cJSON_AddBoolToObject(attempt, "encrypted", ctx->pairing_info.encrypted);
    cJSON_AddBoolToObject(attempt, "pairing_completed", ctx->pairing_completed);
    
    if (ctx->pairing_info.pairing_method != 0xFF) {
        cJSON_AddNumberToObject(attempt, "pairing_method", ctx->pairing_info.pairing_method);
        const char *method_name = 
            ctx->pairing_info.pairing_method == 0 ? "Just Works" :
            ctx->pairing_info.pairing_method == 1 ? "Passkey Entry" :
            ctx->pairing_info.pairing_method == 2 ? "Numeric Comparison" :
            ctx->pairing_info.pairing_method == 3 ? "OOB" : "Unknown";
        cJSON_AddStringToObject(attempt, "pairing_method_name", method_name);
    }
    
    // Secured read outcome
    if (g_pairing_attempt.secured_read_attempted) {
        // Use global tracking (works for both AUTO and manual if we update it)
        cJSON_AddBoolToObject(attempt, "secured_read_attempted", true);
        cJSON_AddNumberToObject(attempt, "secured_read_status", g_pairing_attempt.secured_read_status);
        cJSON_AddBoolToObject(attempt, "secured_read_successful", 
                              g_pairing_attempt.secured_read_status == 0);
    } else if (ctx->phase >= DISC_PHASE_READING_SECURED) {
        // We entered secured reads phase but no attempt was recorded yet
        cJSON_AddBoolToObject(attempt, "secured_read_attempted", true);
        cJSON_AddNumberToObject(attempt, "secured_read_status", -1); // Unknown/interrupted
        cJSON_AddBoolToObject(attempt, "secured_read_successful", false);
    } else {
        cJSON_AddBoolToObject(attempt, "secured_read_attempted", false);
    }
    
    cJSON_AddItemToArray(attempts_array, attempt);
    
    ESP_LOGI(TAG, "Stored pairing attempt info in JSON (strategy: %s)", 
             get_pairing_strategy_name(ctx->pairing_strategy));
}




// we may need to build json and not save to file (e.g. just broadcast ws)
void build_services_json(discovery_context_t *ctx)
{
    if (!ctx || !ctx->json_root || !ctx->json_services) {
        ESP_LOGE(TAG, "Invalid JSON structure");
        return;
    }
    
    char uuid_str[37];
    
    // Build JSON from discovered data
    stored_service_t *svc = ctx->services;
    while (svc) {
        cJSON *svc_obj = cJSON_CreateObject();
        
        ble_uuid_to_str_no_0x_prefix(&svc->uuid.u, uuid_str);
        cJSON_AddStringToObject(svc_obj, "uuid", uuid_str);
        cJSON_AddStringToObject(svc_obj, "type_uuid", "2800");
        cJSON_AddNumberToObject(svc_obj, "start_handle", svc->start_handle);
        cJSON_AddNumberToObject(svc_obj, "end_handle", svc->end_handle);
        
        cJSON *chars_array = cJSON_CreateArray();
        
        stored_characteristic_t *chr = svc->characteristics;
        while (chr) {
            cJSON *chr_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(chr_obj, "handle", chr->def_handle);
            cJSON_AddStringToObject(chr_obj, "uuid", "2803");
            cJSON_AddNumberToObject(chr_obj, "properties", chr->properties);
            cJSON_AddNumberToObject(chr_obj, "security", 0);
            
            cJSON *value_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(value_obj, "handle", chr->val_handle);
            ble_uuid_to_str_no_0x_prefix(&chr->uuid.u, uuid_str);
            cJSON_AddStringToObject(value_obj, "uuid", uuid_str);
            cJSON_AddStringToObject(value_obj, "data", chr->value_hex ? chr->value_hex : "");

            // Add authentication info
            if (chr->encryption_required) {
                cJSON_AddBoolToObject(value_obj, "encryption_required", true);
                cJSON_AddStringToObject(value_obj, "read_error", 
                                    get_att_error_string(chr->last_read_error));
                cJSON_AddNumberToObject(value_obj, "error_code", chr->last_read_error);
            }
            cJSON_AddItemToObject(chr_obj, "value", value_obj);
            
            cJSON *descs_array = cJSON_CreateArray();
            
            stored_descriptor_t *desc = chr->descriptors;
            while (desc) {
                cJSON *desc_obj = cJSON_CreateObject();
                cJSON_AddNumberToObject(desc_obj, "handle", desc->handle);
                ble_uuid_to_str_no_0x_prefix(&desc->uuid.u, uuid_str);
                cJSON_AddStringToObject(desc_obj, "uuid", uuid_str);
                // Add authentication info for descriptors
                if (desc->encryption_required) {
                    cJSON_AddBoolToObject(desc_obj, "encryption_required", true);
                    cJSON_AddStringToObject(desc_obj, "read_error",
                                        get_att_error_string(desc->last_read_error));
                    cJSON_AddNumberToObject(desc_obj, "error_code", desc->last_read_error);
                }                
                cJSON_AddStringToObject(desc_obj, "data", desc->value_hex ? desc->value_hex : "");
                cJSON_AddItemToArray(descs_array, desc_obj);
                
                desc = desc->next;
            }
            
            cJSON_AddItemToObject(chr_obj, "descriptors", descs_array);
            cJSON_AddItemToArray(chars_array, chr_obj);
            
            chr = chr->next;
        }
       
        cJSON_AddItemToObject(svc_obj, "characteristics", chars_array);
        cJSON_AddItemToArray(ctx->json_services, svc_obj);
        
        svc = svc->next;
    }

    // Add pairing information to JSON root (outside service loop, after device info)
    if (ctx->pairing_info.pairing_attempted || ctx->pairing_info.response_received) {
        cJSON *pairing_obj = cJSON_CreateObject();

        // Pairing Response (peer device) - add even if pairing didn't complete
        if (ctx->pairing_info.response_received) {
            cJSON *resp_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(resp_obj, "io_capability", ctx->pairing_info.response.io_capability);
            cJSON_AddNumberToObject(resp_obj, "oob_data_flag", ctx->pairing_info.response.oob_data_flag);
            cJSON_AddNumberToObject(resp_obj, "auth_req", ctx->pairing_info.response.auth_req);
//            cJSON_AddItemToObject(resp_obj, "auth_req_flags", decode_auth_req(ctx->pairing_info.response.auth_req));
            cJSON_AddNumberToObject(resp_obj, "max_key_size", ctx->pairing_info.response.max_key_size);
            cJSON_AddNumberToObject(resp_obj, "init_key_dist", ctx->pairing_info.response.init_key_dist);
            cJSON_AddNumberToObject(resp_obj, "resp_key_dist", ctx->pairing_info.response.resp_key_dist);
            cJSON_AddItemToObject(pairing_obj, "response_original", resp_obj);
        }
        
        cJSON_AddItemToObject(ctx->json_root, "pairing_info", pairing_obj);
    }


    // Add device info
    ble_scanned_device_info_add_to_json(ctx->json_root, &scanned_devices[ctx->device_id]);
    
    // Print JSON
    char *json_string = cJSON_PrintUnformatted(ctx->json_root); // Unformatted produces smaller string (no indentation/newlines)
    if (json_string) {
        printf("\n=== Discovery Complete ===\n%s\n", json_string);
        free(json_string);
    }
    
}

void build_json_and_finish(discovery_context_t *ctx) {
    build_services_json(ctx);
    // Save to file
    save_json_to_file(&ctx->device_addr, ctx->json_root);
    log_memory_usage("After save json to file");
}