#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "cJSON.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ble/ble_discovery.h"
#include "ble/ble_sim.h" // ble_adv_set_free
#include "common/utils.h"
#include "ble/device_parser.h"

static const char *TAG = "json parser";

// Read JSON file to buffer
char *read_json_file(const char *filepath, size_t *out_size) {
    if (!filepath) {
        ESP_LOGE(TAG, "read_json_file: filepath is NULL");
        return NULL;
    }
    FILE *f = fopen(filepath, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Unable to open file: %s", filepath);
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buffer = malloc(file_size + 1);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %ld bytes", file_size);
        fclose(f);
        return NULL;
    }
    
    size_t read_bytes = fread(buffer, 1, file_size, f);
    buffer[read_bytes] = '\0';
    fclose(f);
    
    if (out_size) *out_size = read_bytes;
    return buffer;
}


// Convert JSON properties to NimBLE flags
ble_gatt_chr_flags properties_to_flags(uint8_t properties) {
    ble_gatt_chr_flags flags = 0;
    
    if (properties & 0x01) flags |= BLE_GATT_CHR_F_BROADCAST;
    if (properties & 0x02) flags |= BLE_GATT_CHR_F_READ;
    if (properties & 0x04) flags |= BLE_GATT_CHR_F_WRITE_NO_RSP;
    if (properties & 0x08) flags |= BLE_GATT_CHR_F_WRITE;
    if (properties & 0x10) flags |= BLE_GATT_CHR_F_NOTIFY;
    if (properties & 0x20) flags |= BLE_GATT_CHR_F_INDICATE;
    if (properties & 0x40) flags |= BLE_GATT_CHR_F_AUTH_SIGN_WRITE;
    if (properties & 0x80) flags |= BLE_GATT_CHR_F_RELIABLE_WRITE;
    
    return flags;
}


// Parse UUID string and create ble_uuid_any_t
ble_uuid_any_t* create_uuid_from_string(const char *uuid_str) {
    ble_uuid_any_t *uuid = malloc(sizeof(ble_uuid_any_t));
    if (uuid == NULL) return NULL;
    
    int uuid_len = strlen(uuid_str);
 
    // TBD: handle also 32-bit UUID (not very common)

    if (uuid_len == 4) {
        // 16-bit UUID
        uint16_t uuid16 = (uint16_t)strtol(uuid_str, NULL, 16);
        ble_uuid16_t *u16 = (ble_uuid16_t *)uuid;
        u16->u.type = BLE_UUID_TYPE_16;
        u16->value = uuid16;
    } else if (uuid_len == 32 || uuid_len == 36) {
        // 128-bit UUID
        ble_uuid128_t *u128 = (ble_uuid128_t *)uuid;
        u128->u.type = BLE_UUID_TYPE_128;
        
        // Parse 128-bit UUID (with or without dashes)
        char uuid_no_dash[33];
        int j = 0;
        for (int i = 0; i < uuid_len && j < 32; i++) {
            if (uuid_str[i] != '-') {
                uuid_no_dash[j++] = uuid_str[i];
            }
        }
        uuid_no_dash[32] = '\0';
        
        // Convert to bytes (reverse order for BLE)
        for (int i = 0; i < 16; i++) {
            sscanf(uuid_no_dash + (30 - i * 2), "%2hhx", &u128->value[i]);
        }
    } else {
        free(uuid);
        return NULL;
    }
    
    return uuid;
}



// Parse ble.json directly into NimBLE structures with metadata 
// Used for peripheral (simulation) mode
ble_server_t* parse_json_to_nimble(const char *json_string, 
                                    ble_gatt_access_fn callback) {
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            ESP_LOGE(TAG, "Parse error: %s", error_ptr);
        }
        return NULL;
    }
    
    // Allocate main structure
    ble_server_t *server = calloc(1, sizeof(ble_server_t));
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to allocate BLE server structure");
        cJSON_Delete(root);
        return NULL;
    }
    
    // Parse services
    cJSON *services_json = cJSON_GetObjectItem(root, "services");
    if (cJSON_IsArray(services_json)) {
        server->service_count = cJSON_GetArraySize(services_json);
        
        // Allocate NimBLE service definitions
        server->services = calloc(server->service_count + 1, 
                                  sizeof(struct ble_gatt_svc_def));
        
        // Count total characteristics for metadata array
        int total_chars = 0;
        cJSON *svc_json = NULL;
        cJSON_ArrayForEach(svc_json, services_json) {
            cJSON *chars = cJSON_GetObjectItem(svc_json, "characteristics");
            if (cJSON_IsArray(chars)) {
                total_chars += cJSON_GetArraySize(chars);
            }
        }
              
        // Parse each service
        int svc_idx = 0;
        cJSON_ArrayForEach(svc_json, services_json) {
            if (!parse_service_direct(svc_json, 
                                     &server->services[svc_idx],
                                     svc_idx,
                                     callback)) {
                ESP_LOGE(TAG, "Failed to parse service %d", svc_idx);
                continue;
            }
            svc_idx++;
        }
        
        // Null terminator for NimBLE
        server->services[server->service_count].type = 0;
    }
    
    // Parse devinfo
    cJSON *devinfo = cJSON_GetObjectItem(root, "devinfo");
    if (cJSON_IsObject(devinfo)) {
        parse_devinfo_direct(devinfo, server);
    }

    // Parse pairing info
    cJSON *pairing_info = cJSON_GetObjectItem(root, "pairing_info");
    if (cJSON_IsObject(pairing_info)) {
        parse_pairing_info(pairing_info, server);
    }
    

    cJSON_Delete(root);
    return server;
}

// Parse single service directly
bool parse_service_direct(cJSON *svc_json, 
                         struct ble_gatt_svc_def *svc_def,
                         int svc_idx,
                         ble_gatt_access_fn callback) {
    // Parse service UUID
    cJSON *uuid = cJSON_GetObjectItem(svc_json, "uuid");
    if (cJSON_IsString(uuid)) {
        svc_def->uuid = &create_uuid_from_string(uuid->valuestring)->u;
    } else {
        return false;
    }
    
    svc_def->type = BLE_GATT_SVC_TYPE_PRIMARY;
    
    // Store metadata
    cJSON *start_handle = cJSON_GetObjectItem(svc_json, "start_handle");
    cJSON *end_handle = cJSON_GetObjectItem(svc_json, "end_handle");
       
    // Parse characteristics
    cJSON *chars_json = cJSON_GetObjectItem(svc_json, "characteristics");
    if (cJSON_IsArray(chars_json)) {
        int char_count = cJSON_GetArraySize(chars_json);
        
        // Allocate NimBLE characteristic definitions
        struct ble_gatt_chr_def *chars = calloc(char_count + 1, 
                                                sizeof(struct ble_gatt_chr_def));
        
        int char_idx = 0;
        cJSON *char_json = NULL;
        cJSON_ArrayForEach(char_json, chars_json) {
            if (parse_characteristic_direct(char_json, &chars[char_idx], svc_idx, char_idx, callback)) {
                char_idx++;
            }
        }
        
        // Null terminator
        chars[char_idx].uuid = NULL;
        svc_def->characteristics = chars;
    }
    
    return true;
}

// Parse and create combined context
bool parse_characteristic_direct(cJSON *char_json,
                                struct ble_gatt_chr_def *chr_def,
                                int svc_idx,
                                int char_idx,
                                ble_gatt_access_fn callback) {
    // Allocate context with metadata
    char_context_t *ctx = calloc(1, sizeof(char_context_t));
    if (ctx == NULL) return false;
    
    // Store indices
    ctx->service_index = svc_idx;
    ctx->char_index = char_idx;
    
    // Get handles
    cJSON *handle = cJSON_GetObjectItem(char_json, "handle");
    if (cJSON_IsNumber(handle)) {
        ctx->original_handle = handle->valueint;
    }
    
    // Get properties
    cJSON *properties = cJSON_GetObjectItem(char_json, "properties");
    if (cJSON_IsNumber(properties)) {
        ctx->original_properties = properties->valueint;
        chr_def->flags = properties_to_flags(properties->valueint);
    }
    
    // Parse value
    cJSON *value = cJSON_GetObjectItem(char_json, "value");
    if (!cJSON_IsObject(value)) {
        free(ctx);
        return false;
    }
    
    cJSON *val_handle = cJSON_GetObjectItem(value, "handle");
    if (cJSON_IsNumber(val_handle)) {
        ctx->original_value_handle = val_handle->valueint;
    }
    
    cJSON *val_uuid = cJSON_GetObjectItem(value, "uuid");
    if (cJSON_IsString(val_uuid)) {
        chr_def->uuid = &create_uuid_from_string(val_uuid->valuestring)->u;
    } else {
        free(ctx);
        return false;
    }
    
    // Parse data
    cJSON *val_data = cJSON_GetObjectItem(value, "data");
    if (cJSON_IsString(val_data)) {
        int data_len = strlen(val_data->valuestring) / 2;
        ctx->data = malloc(data_len);
        if (ctx->data != NULL) {
            ctx->data_len = hex_string_to_bytes(val_data->valuestring,
                                                ctx->data, data_len);
        }
    }

    // Parse encryption_required flag
    cJSON *enc_required = cJSON_GetObjectItem(value, "encryption_required");
    if (cJSON_IsBool(enc_required) && cJSON_IsTrue(enc_required)) {
        ctx->encryption_required = true;
        if (ctx->original_properties & 0x02) {  // Read property
        //      BLE_GATT_CHR_F_READ_ENC     = 0x0400  // Requires encryption for reads
            chr_def->flags |= BLE_GATT_CHR_F_READ_ENC;
        }
        if (ctx->original_properties & 0x08) {  // Write property
        //      BLE_GATT_CHR_F_WRITE_ENC    = 0x0800  // Requires encryption for writes
            chr_def->flags |= BLE_GATT_CHR_F_WRITE_ENC;
        }
        if (ctx->original_properties & 0x04) {  // Write without response
            chr_def->flags |= BLE_GATT_CHR_F_WRITE_ENC;
        }
        ESP_LOGI(TAG, "Characteristic requires encryption (handle=%d, uuid=%s)", 
                ctx->original_value_handle, val_uuid->valuestring);
    } else {
        ctx->encryption_required = false;
    }

    // Parse authentication_required flag
    cJSON *auth_required = cJSON_GetObjectItem(value, "authentication_required");
    if (cJSON_IsBool(auth_required) && cJSON_IsTrue(auth_required)) {
        ctx->encryption_required = true;
        if (ctx->original_properties & 0x02) {  // Read property
        //      BLE_GATT_CHR_F_READ_AUTHEN  = 0x0100  // Requires authentication for reads
            chr_def->flags |= BLE_GATT_CHR_F_READ_AUTHEN;
        }
        if (ctx->original_properties & 0x08) {  // Write property
        //      BLE_GATT_CHR_F_WRITE_AUTHEN = 0x0200  // Requires authentication for writes
            chr_def->flags |= BLE_GATT_CHR_F_WRITE_AUTHEN;
        }
        if (ctx->original_properties & 0x04) {  // Write without response
            chr_def->flags |= BLE_GATT_CHR_F_WRITE_AUTHEN;
        }
        ESP_LOGI(TAG, "Characteristic requires authentication (handle=%d, uuid=%s)", 
                ctx->original_value_handle, val_uuid->valuestring);
    } else {
        ctx->encryption_required = false;
    }

// Optionally log authentication error info for debugging
//    cJSON *read_error = cJSON_GetObjectItem(value, "read_error");
//    cJSON *error_code = cJSON_GetObjectItem(value, "error_code");
//    if (cJSON_IsString(read_error) && cJSON_IsNumber(error_code)) {
//        ESP_LOGD(TAG, "  Error info: '%s' (code=%d)", 
//                read_error->valuestring, error_code->valueint);
//    }


    // Store combined context in arg
    chr_def->arg = ctx;
    chr_def->access_cb = callback;

    // Parse dynamic configuration 
    cJSON *dynamic = cJSON_GetObjectItem(char_json, "dynamic");
    if (cJSON_IsObject(dynamic)) {
        ESP_LOGI(TAG, "Parsing dynamic config for characteristic index %d", ctx->char_index);
        ctx->has_dynamic = parse_dynamic_config(dynamic, &ctx->dynamic);
        
        // Also store in metadata for lookups
//        char_meta->has_dynamic = chr_ctx->has_dynamic;
//        if (chr_ctx->has_dynamic) {
//            memcpy(&char_meta->dynamic, &chr_ctx->dynamic, 
//                   sizeof(ble_dynamic_config_t));
//        }
    } else {
//        ESP_LOGI(TAG, "No dynamic!");
        ctx->has_dynamic = false;
    }


    // Parse descriptors...
    cJSON *descs_json = cJSON_GetObjectItem(char_json, "descriptors");
    if (cJSON_IsArray(descs_json)) {
        int desc_count = cJSON_GetArraySize(descs_json);
        
        // Filter out CCCD descriptors - NimBLE adds them automatically
//        bool has_notify_indicate = (char_meta->original_properties & 0x30) != 0;  // Notify(0x10) | Indicate(0x20)
        
        // Count non-CCCD descriptors
        int non_cccd_count = 0;
        cJSON *desc_json = NULL;
        cJSON_ArrayForEach(desc_json, descs_json) {
            cJSON *desc_uuid = cJSON_GetObjectItem(desc_json, "uuid");
            if (cJSON_IsString(desc_uuid)) {
                // Skip CCCD (0x2902) if characteristic has notify/indicate
                if (strcasecmp(desc_uuid->valuestring, "2902") == 0 ) { //} && has_notify_indicate) {
                    ESP_LOGI(TAG, "Skipping CCCD descriptor - NimBLE adds automatically");
                    continue;
                }
                non_cccd_count++;
            }
        }
        
        if (non_cccd_count > 0) {
            // Allocate descriptors array (+ 1 for terminator)
            struct ble_gatt_dsc_def *dscs = calloc(non_cccd_count + 1,
                                                    sizeof(struct ble_gatt_dsc_def));
            if (dscs == NULL) {
                ESP_LOGE(TAG, "Failed to allocate descriptors");
                free(ctx->data);
                free(ctx);
                return false;
            }
            
            int dsc_idx = 0;
            cJSON_ArrayForEach(desc_json, descs_json) {
                cJSON *desc_uuid = cJSON_GetObjectItem(desc_json, "uuid");
                if (!cJSON_IsString(desc_uuid)) continue;
                
                // Skip CCCD if notify/indicate present
                if (strcasecmp(desc_uuid->valuestring, "2902") == 0 ) { // && has_notify_indicate) {
                    continue;
                }
                
                if (parse_descriptor_direct(desc_json, &dscs[dsc_idx], callback)) {
                    dsc_idx++;
                }
            }
            
            // Null terminator
            dscs[dsc_idx].uuid = NULL;
            chr_def->descriptors = dscs;
            
            ESP_LOGI(TAG, "Added %d custom descriptor(s)", dsc_idx); // , char_meta->uuid_str);
        } else { //if (has_notify_indicate) {
//            ESP_LOGI(TAG, "No custom descriptors - NimBLE will add CCCD automatically ");
            chr_def->descriptors = NULL;
        }
    }


    return true;
}

// Parse dynamic configuration from JSON
bool parse_dynamic_config(cJSON *dynamic_json, ble_dynamic_config_t *config) {
    if (dynamic_json == NULL || config == NULL) {
        return false;
    }
    
    memset(config, 0, sizeof(ble_dynamic_config_t));
    
    cJSON *on_read = cJSON_GetObjectItem(dynamic_json, "on_read");
    if (cJSON_IsString(on_read) && on_read->valuestring != NULL) {
        strncpy(config->on_read, on_read->valuestring, 
                sizeof(config->on_read) - 1);
        ESP_LOGI(TAG, "  on_read: %s", config->on_read);
    }
    
    cJSON *on_write = cJSON_GetObjectItem(dynamic_json, "on_write");
    if (cJSON_IsString(on_write) && on_write->valuestring != NULL) {
        strncpy(config->on_write, on_write->valuestring, 
                sizeof(config->on_write) - 1);
        ESP_LOGI(TAG, "  on_write: %s", config->on_write);
    }
    
    return true;
}


// Parse descriptor directly
bool parse_descriptor_direct(cJSON *desc_json,
                            struct ble_gatt_dsc_def *dsc_def,
                            ble_gatt_access_fn callback) {
    cJSON *uuid = cJSON_GetObjectItem(desc_json, "uuid");
    if (cJSON_IsString(uuid)) {
        dsc_def->uuid = &create_uuid_from_string(uuid->valuestring)->u;
    } else {
        return false;
    }
    
    dsc_def->att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE;

    // Parse value
    cJSON *value = cJSON_GetObjectItem(desc_json, "data");
    if (cJSON_IsString(value)) {
        int data_len = strlen(value->valuestring) / 2;
        uint8_t *data_storage = malloc(sizeof(uint16_t) + data_len);        
        if (data_storage != NULL) {
            *(uint16_t *)data_storage = data_len;
            hex_string_to_bytes(value->valuestring,
                               data_storage + sizeof(uint16_t), data_len);
            dsc_def->arg = data_storage;
        }
    }
    
    dsc_def->access_cb = callback;
    return true;
}

// Parse devinfo directly
void parse_devinfo_direct(cJSON *devinfo, ble_server_t *server) {
    cJSON *adv_data = cJSON_GetObjectItem(devinfo, "adv_data");
    if (cJSON_IsString(adv_data) && adv_data->valuestring != NULL) {
        //server->devinfo = malloc();
        server->devinfo = malloc(sizeof(ble_devinfo_t));
        server->devinfo->adv_data_len = hex_string_to_bytes(adv_data->valuestring,
                                                    server->devinfo->adv_data,
                                                    sizeof(server->devinfo->adv_data));
    }
    
    cJSON *bd_addr = cJSON_GetObjectItem(devinfo, "bd_addr");
    if (cJSON_IsString(bd_addr) && bd_addr->valuestring != NULL) {
        strncpy(server->devinfo->bd_addr, bd_addr->valuestring, sizeof(server->devinfo->bd_addr) - 1);
    }
    
    cJSON *addr_type = cJSON_GetObjectItem(devinfo, "addr_type");
    if (cJSON_IsNumber(addr_type)) {
        server->devinfo->addr_type = addr_type->valueint;
    }
    
    cJSON *scan_rsp = cJSON_GetObjectItem(devinfo, "scan_rsp");
    if (cJSON_IsString(scan_rsp) && scan_rsp->valuestring != NULL) {
        server->devinfo->scan_rsp_len = hex_string_to_bytes(scan_rsp->valuestring,
                                                    server->devinfo->scan_rsp,
                                                    sizeof(server->devinfo->scan_rsp));
    }
    cJSON *pdu = cJSON_GetObjectItem(devinfo, "pdu_type");
    if (cJSON_IsString(pdu))
        strlcpy(server->devinfo->pdu_type, pdu->valuestring, sizeof(server->devinfo->pdu_type));
    else
        strlcpy(server->devinfo->pdu_type, "ADV_IND", sizeof(server->devinfo->pdu_type));  // safe default

}


// Parse pairing configuration (request / response)
bool parse_pairing_config(cJSON *config_json, ble_pairing_config_t *config) {
    if (config == NULL) {
        return false;
    }
    
    // Set default values first
    config->io_capability = 3;
    config->auth_req = 1;
    config->oob_data_flag = 0;
    config->max_key_size = 16;
    config->init_key_dist = 1;
    config->resp_key_dist = 1;
    
    // If no JSON provided, return with defaults
    if (config_json == NULL) {
        return true;
    }
    
    // Override with JSON values if present
    cJSON *io_cap = cJSON_GetObjectItem(config_json, "io_capability");
    if (cJSON_IsNumber(io_cap)) {
        config->io_capability = io_cap->valueint;
    }
    
    cJSON *auth_req = cJSON_GetObjectItem(config_json, "auth_req");
    if (cJSON_IsNumber(auth_req)) {
        config->auth_req = auth_req->valueint;
    }
    
    cJSON *oob_flag = cJSON_GetObjectItem(config_json, "oob_data_flag");
    if (cJSON_IsNumber(oob_flag)) {
        config->oob_data_flag = oob_flag->valueint;
    }
    
    cJSON *max_key = cJSON_GetObjectItem(config_json, "max_key_size");
    if (cJSON_IsNumber(max_key)) {
        config->max_key_size = max_key->valueint;
    }
    
    cJSON *init_key = cJSON_GetObjectItem(config_json, "init_key_dist");
    if (cJSON_IsNumber(init_key)) {
        config->init_key_dist = init_key->valueint;
    }
    
    cJSON *resp_key = cJSON_GetObjectItem(config_json, "resp_key_dist");
    if (cJSON_IsNumber(resp_key)) {
        config->resp_key_dist = resp_key->valueint;
    }
    
    return true;
}



// Parse pairing_info section
bool parse_pairing_info(cJSON *pairing_json, ble_server_t *server) {
    if (pairing_json == NULL || server == NULL) {
        return false;
    }
    
    server->pairing_info = malloc(sizeof(ble_pairing_info_t));
    if (server->pairing_info == NULL) {
        ESP_LOGE(TAG, "Failed to allocate pairing_info structure");
        return false;
    }
    
    memset(server->pairing_info, 0, sizeof(ble_pairing_info_t));

    // Parse initiate_pairing_on_connection flag
    cJSON *initiate_pairing_on_connection = cJSON_GetObjectItem(pairing_json, "initiate_pairing_on_connection");
    if (cJSON_IsBool(initiate_pairing_on_connection)) {
        server->pairing_info->initiate_pairing_on_connection = cJSON_IsTrue(initiate_pairing_on_connection);
    } else {
        // Default to false if not specified
        server->pairing_info->initiate_pairing_on_connection = false;
    } 

    // Parse passkey (6-digit numeric string)
    cJSON *passkey = cJSON_GetObjectItem(pairing_json, "passkey");
    if (cJSON_IsString(passkey)) {
        const char *passkey_str = cJSON_GetStringValue(passkey);
        if (passkey_str != NULL && strlen(passkey_str) == 6) {
            // Validate all digits
            bool valid = true;
            for (int i = 0; i < 6; i++) {
                if (!isdigit((unsigned char)passkey_str[i])) {
                    valid = false;
                    break;
                }
            }
            
            if (valid) {
                strncpy(server->pairing_info->passkey, passkey_str, 6);
                server->pairing_info->passkey[6] = '\0';
            } else {
                ESP_LOGW(TAG, "Invalid passkey format (must be 6 digits): %s", passkey_str);
                server->pairing_info->passkey[0] = '\0';
            }
        } else {
            server->pairing_info->passkey[0] = '\0';
        }
    } else {
        server->pairing_info->passkey[0] = '\0';
    }

    // Parse response configuration (for NimBLE config)
    // If response not present or not an object, parse_pairing_config will use defaults
    cJSON *response = cJSON_GetObjectItem(pairing_json, "response");
    if (cJSON_IsObject(response)) {
        parse_pairing_config(response, &server->pairing_info->response);
    } else {
        // No response object in JSON - use defaults
        parse_pairing_config(NULL, &server->pairing_info->response);
    }
    
    return true;
}

// Cleanup function to free ble_server_t
void free_ble_server(ble_server_t **server_ptr) {
    if (server_ptr == NULL || *server_ptr == NULL) return;

    ble_server_t *server = *server_ptr;

    ESP_LOGI(TAG, "Freeing BLE server structure");

    // Free services and their characteristics/descriptors
    if (server->services != NULL) {
        for (int svc_idx = 0; svc_idx < server->service_count; svc_idx++) {
            struct ble_gatt_svc_def *svc = &server->services[svc_idx];

            // Free service UUID
            if (svc->uuid != NULL) {
                free((void *)svc->uuid);
            }

            // Free characteristics
            if (svc->characteristics != NULL) {
                struct ble_gatt_chr_def *chars = (struct ble_gatt_chr_def *)svc->characteristics;

                for (int chr_idx = 0; chars[chr_idx].uuid != NULL; chr_idx++) {
                    struct ble_gatt_chr_def *chr = &chars[chr_idx];

                    // Free characteristic UUID
                    if (chr->uuid != NULL) {
                        free((void *)chr->uuid);
                    }

                    // Free characteristic context (includes data)
                    if (chr->arg != NULL) {
                        char_context_t *ctx = (char_context_t *)chr->arg;
                        if (ctx->data != NULL) {
                            free(ctx->data);
                        }
                        free(ctx);
                    }

                    // Free descriptors
                    if (chr->descriptors != NULL) {
                        struct ble_gatt_dsc_def *dscs = (struct ble_gatt_dsc_def *)chr->descriptors;

                        for (int dsc_idx = 0; dscs[dsc_idx].uuid != NULL; dsc_idx++) {
                            struct ble_gatt_dsc_def *dsc = &dscs[dsc_idx];

                            // Free descriptor UUID
                            if (dsc->uuid != NULL) {
                                free((void *)dsc->uuid);
                            }

                            // Free descriptor data
                            if (dsc->arg != NULL) {
                                free(dsc->arg);
                            }
                        }
                        free(dscs);
                    }
                }
                free(chars);
            }
        }
        free(server->services);
    }

    // Free device info
    if (server->devinfo != NULL) {
        free(server->devinfo);
    }

    // Free adv set (profile-owned buffers only; devinfo-borrowed ones skipped)
    ble_adv_set_free(&server->adv_set);

    // Free pairing info
    if (server->pairing_info != NULL) {
        free(server->pairing_info);
    }

    // Free server structure itself
    free(server);
    *server_ptr = NULL;

    ESP_LOGI(TAG, "BLE server structure freed");
}



// Parse discovery JSON into stored_* list
// Used in "central" (connecting to device) mode
bool parse_json_to_discovery(const char *json_string, stored_service_t **out_services, ble_addr_t *out_device_addr) 
{
    cJSON *root = cJSON_Parse(json_string);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return false;
    }

    // Parse devinfo -> device_addr
    if (out_device_addr) {
        memset(out_device_addr, 0, sizeof(*out_device_addr));
        cJSON *devinfo = cJSON_GetObjectItem(root, "devinfo");
        if (cJSON_IsObject(devinfo)) {
            cJSON *bd_addr = cJSON_GetObjectItem(devinfo, "bd_addr");
            if (cJSON_IsString(bd_addr)) {
                // Remove colons → "64E8335E1462"
                char clean[13];
                int j = 0;
                for (int i = 0; bd_addr->valuestring[i] && j < 12; i++) {
                    if (bd_addr->valuestring[i] != ':') {
                        clean[j++] = toupper(bd_addr->valuestring[i]);
                    }
                }
                clean[12] = '\0';

                // Parse hex → temp bytes (big-endian parse)
                uint8_t temp[6];
                for (int i = 0; i < 6; i++) {
                    sscanf(clean + i*2, "%2hhx", &temp[i]);
                }
                
                // REVERSE to little-endian for BLE (val[0]=LSB)
                for (int i = 0; i < 6; i++) {
                    out_device_addr->val[i] = temp[5 - i];
                }
            }
            cJSON *addr_type = cJSON_GetObjectItem(devinfo, "addr_type");
            if (cJSON_IsNumber(addr_type)) {
                out_device_addr->type = addr_type->valueint;  // 0=PUBLIC, 1=RANDOM
            }
            
        }
//        ESP_LOGI(TAG, "Parsed addr: %s type=%d", 
//                ble_addr_to_str(out_device_addr, 1), out_device_addr->type);
    }


    cJSON *services_json = cJSON_GetObjectItem(root, "services");
    if (!cJSON_IsArray(services_json)) {
        ESP_LOGE(TAG, "No 'services' array in JSON");
        cJSON_Delete(root);
        return false;
    }

    stored_service_t *svc_head = NULL, *svc_tail = NULL;

    cJSON *svc_json = NULL;
    cJSON_ArrayForEach(svc_json, services_json) {
        stored_service_t *svc = calloc(1, sizeof(*svc));
        if (!svc) {
            ESP_LOGE(TAG, "Failed to alloc service");
            free_services(svc_head);
            cJSON_Delete(root);
            *out_services = NULL;
            return false;
        }

        // uuid
        cJSON *uuid = cJSON_GetObjectItem(svc_json, "uuid");
        if (cJSON_IsString(uuid)) {
            ble_uuid_any_t *u = create_uuid_from_string(uuid->valuestring);
            if (!u) {
                ESP_LOGE(TAG, "Failed to parse service UUID: %s", uuid->valuestring);
                free(svc);  // no children yet 
                free_services(svc_head);
                cJSON_Delete(root);
                *out_services = NULL;
                return false;
            }
            memcpy(&svc->uuid, u, sizeof(ble_uuid_any_t));
            free(u);
        } else {
            ESP_LOGE(TAG, "Missing/invalid service UUID");
            free(svc);
            free_services(svc_head);
            cJSON_Delete(root);
            *out_services = NULL;
            return false;
        }

        // handles
        cJSON *sh = cJSON_GetObjectItem(svc_json, "start_handle");
        cJSON *eh = cJSON_GetObjectItem(svc_json, "end_handle");
        if (cJSON_IsNumber(sh)) svc->start_handle = sh->valueint;
        if (cJSON_IsNumber(eh)) svc->end_handle = eh->valueint;

        // characteristics
        cJSON *chars_json = cJSON_GetObjectItem(svc_json, "characteristics");
        if (cJSON_IsArray(chars_json)) {
            stored_characteristic_t *chr_head = NULL, *chr_tail = NULL;
            cJSON *chr_json = NULL;
            cJSON_ArrayForEach(chr_json, chars_json) {
                stored_characteristic_t *chr = calloc(1, sizeof(*chr));
                if (!chr) {
                    ESP_LOGE(TAG, "Failed to alloc characteristic");
                    free_characteristics(chr_head);
                    free(svc);
                    free_services(svc_head);
                    cJSON_Delete(root);
                    *out_services = NULL;
                    return false;
                }

                cJSON *h = cJSON_GetObjectItem(chr_json, "handle");  // def_handle
                cJSON *props = cJSON_GetObjectItem(chr_json, "properties");
                if (cJSON_IsNumber(h))     chr->def_handle = h->valueint;
                if (cJSON_IsNumber(props)) chr->properties = props->valueint;

                cJSON *val = cJSON_GetObjectItem(chr_json, "value");
                if (cJSON_IsObject(val)) {
                    cJSON *vh = cJSON_GetObjectItem(val, "handle");
                    cJSON *vu = cJSON_GetObjectItem(val, "uuid");
                    cJSON *vd = cJSON_GetObjectItem(val, "data");
                    cJSON *enc = cJSON_GetObjectItem(val, "encryption_required");
                    if (cJSON_IsNumber(vh)) chr->val_handle = vh->valueint;
                    if (cJSON_IsString(vu)) {
                        ble_uuid_any_t *u = create_uuid_from_string(vu->valuestring);
                        if (!u) {
                            ESP_LOGE(TAG, "Failed to parse char UUID: %s", vu->valuestring);
                            free_characteristics(chr_head);  // cleanup this chr too 
                            free(chr);
                            free_descriptors(NULL);  // no descs yet 
                            free(svc);
                            free_services(svc_head);
                            cJSON_Delete(root);
                            *out_services = NULL;
                            return false;
                        }
                        memcpy(&chr->uuid, u, sizeof(ble_uuid_any_t));
                        free(u);
                    }
                    if (cJSON_IsString(vd) && vd->valuestring && vd->valuestring[0]) {
                        chr->value_hex = strdup(vd->valuestring);
                        if (!chr->value_hex) {
                            ESP_LOGE(TAG, "Failed to strdup value_hex");
                            free_characteristics(chr_head);
                            free(chr);
                            free(svc);
                            free_services(svc_head);
                            cJSON_Delete(root);
                            *out_services = NULL;
                            return false;
                        }
                    }
                    if (cJSON_IsBool(enc) && cJSON_IsTrue(enc)) {
                        chr->encryption_required = true;
                    }
                }

                // descriptors (similar pattern...)
                cJSON *descs_json = cJSON_GetObjectItem(chr_json, "descriptors");
                if (cJSON_IsArray(descs_json)) {
                    stored_descriptor_t *d_head = NULL, *d_tail = NULL;
                    cJSON *desc_json = NULL;
                    cJSON_ArrayForEach(desc_json, descs_json) {
                        stored_descriptor_t *desc = calloc(1, sizeof(*desc));
                        if (!desc) {
                            ESP_LOGE(TAG, "Failed to alloc descriptor");
                            free_descriptors(d_head);
                            free_characteristics(chr_head);
                            free(chr);
                            free(svc);
                            free_services(svc_head);
                            cJSON_Delete(root);
                            *out_services = NULL;
                            return false;
                        }

                        cJSON *dh = cJSON_GetObjectItem(desc_json, "handle");
                        cJSON *du = cJSON_GetObjectItem(desc_json, "uuid");
                        cJSON *dd = cJSON_GetObjectItem(desc_json, "data");
                        cJSON *enc = cJSON_GetObjectItem(desc_json, "encryption_required");
                        if (cJSON_IsNumber(dh)) desc->handle = dh->valueint;
                        if (cJSON_IsString(du)) {
                            ble_uuid_any_t *u = create_uuid_from_string(du->valuestring);
                            if (!u) {
                                ESP_LOGE(TAG, "Failed to parse desc UUID");
                                free(desc);
                                free_descriptors(d_head);
                                free_characteristics(chr_head);
                                free(chr);
                                free(svc);
                                free_services(svc_head);
                                cJSON_Delete(root);
                                *out_services = NULL;
                                return false;
                            }
                            memcpy(&desc->uuid, u, sizeof(ble_uuid_any_t));
                            free(u);
                        }
                        if (cJSON_IsString(dd) && dd->valuestring && dd->valuestring[0]) {
                            desc->value_hex = strdup(dd->valuestring);
                            if (!desc->value_hex) {
                                ESP_LOGE(TAG, "Failed to strdup desc value_hex");
                                free(desc);
                                free_descriptors(d_head);
                                free_characteristics(chr_head);
                                free(chr);
                                free(svc);
                                free_services(svc_head);
                                cJSON_Delete(root);
                                *out_services = NULL;
                                return false;
                            }
                        }
                        if (cJSON_IsBool(enc) && cJSON_IsTrue(enc)) {
                            desc->encryption_required = true;
                        }

                        if (!d_head) d_head = d_tail = desc;
                        else { d_tail->next = desc; d_tail = desc; }
                    }
                    chr->descriptors = d_head;
                }

                if (!chr_head) chr_head = chr_tail = chr;
                else { chr_tail->next = chr; chr_tail = chr; }
            }
            svc->characteristics = chr_head;
        }

        if (!svc_head) svc_head = svc_tail = svc;
        else { svc_tail->next = svc; svc_tail = svc; }
    }

    cJSON_Delete(root);
    *out_services = svc_head;
    return true;
}
