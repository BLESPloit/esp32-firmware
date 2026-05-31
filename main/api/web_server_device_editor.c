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

#include "ble/ble_sim.h" // load device for simulation
#include "ble/ble_discovery.h"
#include "ble/ble_scan.h"
#include "graphics/graphics.h"
#include "interface/interface_sim.h"
#include "common/storage.h"
#include "common/utils.h"
#include "api/web_server.h"
#include "api/web_server_internal.h"
#include "ble/device_manifest.h"


static const char *TAG = "web server - device editor";


// Structure to hold parsed request data
typedef struct {
    char *buffer;
    cJSON *json;
} request_data_t;

// Structure to hold file data
typedef struct {
    char *buffer;
    cJSON *json;
} file_data_t;


// forward declarations
static esp_err_t web_server_read_json_file(const char *file_path, file_data_t *data);

// ── Helpers ────────────────────────────────────────────────── 

// Helper function to decode io_capability
static const char* get_io_capability_name(uint8_t io_cap) {
    switch(io_cap) {
        case 0: return "DisplayOnly";
        case 1: return "DisplayYesNo";
        case 2: return "KeyboardOnly";
        case 3: return "NoInputNoOutput";
        case 4: return "KeyboardDisplay";
        default: return "Unknown";
    }
}

// Helper function to decode auth_req flags into JSON object
static cJSON* decode_auth_req_to_json(uint8_t auth_req) {
    cJSON *flags = cJSON_CreateObject();
    cJSON_AddBoolToObject(flags, "bonding", (auth_req & 0x01) != 0);
    cJSON_AddBoolToObject(flags, "mitm", (auth_req & 0x04) != 0);
    cJSON_AddBoolToObject(flags, "secure_connections", (auth_req & 0x08) != 0);
    cJSON_AddBoolToObject(flags, "keypress", (auth_req & 0x10) != 0);
    return flags;
}

// Helper function to encode auth_req from flags
static uint8_t encode_auth_req_from_json(cJSON *flags_obj) {
    uint8_t auth_req = 0;

    cJSON *bonding = cJSON_GetObjectItem(flags_obj, "bonding");
    if (bonding && cJSON_IsTrue(bonding)) auth_req |= 0x01;

    cJSON *mitm = cJSON_GetObjectItem(flags_obj, "mitm");
    if (mitm && cJSON_IsTrue(mitm)) auth_req |= 0x04;

    cJSON *sc = cJSON_GetObjectItem(flags_obj, "secure_connections");
    if (sc && cJSON_IsTrue(sc)) auth_req |= 0x08;

    cJSON *keypress = cJSON_GetObjectItem(flags_obj, "keypress");
    if (keypress && cJSON_IsTrue(keypress)) auth_req |= 0x10;

    return auth_req;
}




// Helper function to validate device_id format
static bool is_valid_device_id(const char *device_id)
{
    if (device_id == NULL || strlen(device_id) == 0 || strlen(device_id) >= 64) {
        return false;
    }
    
    for (size_t i = 0; i < strlen(device_id); i++) {
        char c = device_id[i];
        if (!((c >= 'a' && c <= 'z') || 
              (c >= 'A' && c <= 'Z') || 
              (c >= '0' && c <= '9') || 
              c == '-' || c == '_')) {
            return false;
        }
    }
    
    return true;
}

// ── Rename device directory ────────────────────────────────────────────────── 


static esp_err_t rename_device_directory(const char *old_device_id, const char *new_device_id)
{
    char old_dir[MAX_DEVICE_PATH_LEN];
    char new_dir[MAX_DEVICE_PATH_LEN];
    DIR *dir = NULL;
    struct dirent *entry;
    
    // Validate new device_id
    if (!is_valid_device_id(new_device_id)) {
        ESP_LOGE(TAG, "Invalid device_id format: %s", new_device_id);
        return ESP_FAIL;
    }
    
    int ret = snprintf(old_dir, sizeof(old_dir), "/"LITTLEFS_LABEL"/devices/%s", old_device_id);
    if (ret >= sizeof(old_dir)) {
        ESP_LOGE(TAG, "Old directory path too long");
        return ESP_FAIL;
    }
    
    ret = snprintf(new_dir, sizeof(new_dir), "/"LITTLEFS_LABEL"/devices/%s", new_device_id);
    if (ret >= sizeof(new_dir)) {
        ESP_LOGE(TAG, "New directory path too long");
        return ESP_FAIL;
    }
    
    // Check if new directory already exists
    struct stat st;
    if (stat(new_dir, &st) == 0) {
        ESP_LOGE(TAG, "Target directory already exists: %s", new_dir);
        return ESP_FAIL;
    }
    
    // Create new directory
    if (mkdir(new_dir, 0755) != 0) {
        ESP_LOGE(TAG, "Failed to create new directory: %s", new_dir);
        return ESP_FAIL;
    }
    
    // Open old directory
    dir = opendir(old_dir);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory: %s", old_dir);
        rmdir(new_dir);
        return ESP_FAIL;
    }
    
    // Move all files from old to new directory
    int files_moved = 0;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and .. entries
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Build paths with size checks
        char old_path[512];
        char new_path[512];
        
        ret = snprintf(old_path, sizeof(old_path), "%s/%s", old_dir, entry->d_name);
        if (ret >= sizeof(old_path)) {
            ESP_LOGE(TAG, "Old file path too long for: %s", entry->d_name);
            continue;
        }
        
        ret = snprintf(new_path, sizeof(new_path), "%s/%s", new_dir, entry->d_name);
        if (ret >= sizeof(new_path)) {
            ESP_LOGE(TAG, "New file path too long for: %s", entry->d_name);
            continue;
        }
        
        // Check if it's a regular file
        struct stat file_stat;
        if (stat(old_path, &file_stat) != 0) {
            ESP_LOGW(TAG, "Failed to stat file: %s", old_path);
            continue;
        }
        
        if (!S_ISREG(file_stat.st_mode)) {
            ESP_LOGW(TAG, "Skipping non-file entry: %s", entry->d_name);
            continue;
        }
        
        // Move the file
        if (rename(old_path, new_path) != 0) {
            ESP_LOGE(TAG, "Failed to move file: %s", entry->d_name);
            closedir(dir);
            
            // Rollback: try to move files back
            DIR *rollback_dir = opendir(new_dir);
            if (rollback_dir != NULL) {
                struct dirent *rb_entry;
                while ((rb_entry = readdir(rollback_dir)) != NULL) {
                    if (strcmp(rb_entry->d_name, ".") == 0 || strcmp(rb_entry->d_name, "..") == 0) {
                        continue;
                    }
                    char rb_new[512], rb_old[512];
                    if (snprintf(rb_new, sizeof(rb_new), "%s/%s", new_dir, rb_entry->d_name) < sizeof(rb_new) &&
                        snprintf(rb_old, sizeof(rb_old), "%s/%s", old_dir, rb_entry->d_name) < sizeof(rb_old)) {
                        rename(rb_new, rb_old);
                    }
                }
                closedir(rollback_dir);
            }
            rmdir(new_dir);
            return ESP_FAIL;
        }
        
        files_moved++;
        ESP_LOGI(TAG, "Moved file: %s", entry->d_name);
    }
    
    closedir(dir);
    
    // Remove old directory
    if (rmdir(old_dir) != 0) {
        ESP_LOGW(TAG, "Failed to remove old directory: %s", old_dir);
    }
    
    ESP_LOGI(TAG, "Successfully renamed device from '%s' to '%s' (%d files moved)", 
             old_device_id, new_device_id, files_moved);
    
    return ESP_OK;
}


// ── Handle PATCH requests ────────────────────────────────────────────────── 

// Read and parse the HTTP request body
static esp_err_t read_request_body(httpd_req_t *req, request_data_t *data)
{
    data->buffer = malloc(req->content_len + 1);
    if (data->buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for request");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    int ret = httpd_req_recv(req, data->buffer, req->content_len);
    if (ret <= 0) {
        free(data->buffer);
        data->buffer = NULL;
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    data->buffer[ret] = '\0';

    ESP_LOGI(TAG, "Patch JSON: %s", data->buffer); 
    
    data->json = cJSON_Parse(data->buffer);
    free(data->buffer);
    data->buffer = NULL; // so cleanup doesn't double-free
    if (data->json == NULL) {
        ESP_LOGE(TAG, "Failed to parse patch JSON");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid JSON\"}");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// Read and parse existing JSON file
// TBD: reuse from device_parser?
static esp_err_t web_server_read_json_file(const char *file_path, file_data_t *data)
{
    FILE *f = fopen(file_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "File not found: %s", file_path);
        return ESP_ERR_NOT_FOUND;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    data->buffer = malloc(file_size + 1);
    if (data->buffer == NULL) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to allocate memory for file");
        return ESP_ERR_NO_MEM;
    }
    
    fread(data->buffer, 1, file_size, f);
    data->buffer[file_size] = '\0';
    fclose(f);

    data->json = cJSON_Parse(data->buffer);
    free(data->buffer);    // free immediately, cJSON tree is independent
    data->buffer = NULL;   // so callers don't double-free

    if (data->json == NULL) {
        ESP_LOGE(TAG, "Failed to parse existing JSON");
        free(data->buffer);
        data->buffer = NULL;
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// Write JSON to file
static esp_err_t write_json_file(const char *filepath, cJSON *json) {
    // Serialize FIRST
    char *json_string = cJSON_PrintUnformatted(json); // Unformatted produces smaller string (no indentation/newlines)
    if (json_string == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return ESP_FAIL;
    }
    
    // Only open file after successful serialization
    FILE *f = fopen(filepath, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        cJSON_free(json_string);
        return ESP_FAIL;
    }
    
    fputs(json_string, f);
    fclose(f);
    cJSON_free(json_string);
    
    ESP_LOGI(TAG, "Successfully wrote JSON to file");
    return ESP_OK;
}


// Handle device_id rename operation
static esp_err_t handle_device_id_change(httpd_req_t *req, cJSON *patch_json, const char *device_id)
{
    cJSON *new_device_id_item = cJSON_GetObjectItem(patch_json, "device_id");
    if (new_device_id_item == NULL || !cJSON_IsString(new_device_id_item)) {
        return ESP_ERR_NOT_FOUND;  // Not a device_id change
    }
    
    const char *new_device_id = new_device_id_item->valuestring;
    
    // Validate new device ID
    if (!is_valid_device_id(new_device_id)) {
        ESP_LOGE(TAG, "Invalid device_id format: %s", new_device_id);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid device_id. Use only alphanumerics, hyphens, and underscores\"}");
        return ESP_FAIL;
    }
    
    // Check if new device_id is the same as current
    if (strcmp(device_id, new_device_id) == 0) {
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"No change\"}");
        return ESP_OK;
    }
    
    // Rename the device directory
    if (rename_device_directory(device_id, new_device_id) != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"error\":\"Failed to rename device directory. Target may already exist.\"}");
        return ESP_FAIL;
    }
    
    // Send success response
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    char response[128];
    snprintf(response, sizeof(response), "{\"status\":\"success\",\"new_device_id\":\"%s\"}", new_device_id);
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

// Update pairing_info fields in existing JSON
static void update_pairing_info_fields(cJSON *existing_pairing, cJSON *pairing_info_patch)
{
    // Update passkey if provided
    cJSON *passkey = cJSON_GetObjectItem(pairing_info_patch, "passkey");
    if (passkey && cJSON_IsString(passkey)) {
        cJSON_DeleteItemFromObject(existing_pairing, "passkey");
        cJSON_AddStringToObject(existing_pairing, "passkey", passkey->valuestring);
        ESP_LOGI(TAG, "Updated passkey: %s", passkey->valuestring);
    }

    // Handle initiate_pairing_on_connection flag
    cJSON *initiate_pairing_on_connection = cJSON_GetObjectItem(pairing_info_patch, "initiate_pairing_on_connection");
    if (initiate_pairing_on_connection && cJSON_IsBool(initiate_pairing_on_connection)) {
        cJSON_DeleteItemFromObject(existing_pairing, "initiate_pairing_on_connection");
        cJSON_AddBoolToObject(existing_pairing, "initiate_pairing_on_connection", cJSON_IsTrue(initiate_pairing_on_connection));
        ESP_LOGI(TAG, "Updated initiate_pairing_on_connection: %s", cJSON_IsTrue(initiate_pairing_on_connection) ? "true" : "false");
    }
 
 
    // Update response fields if provided
    cJSON *response = cJSON_GetObjectItem(pairing_info_patch, "response");
    if (!response || !cJSON_IsObject(response)) {
        return;
    }
    
    cJSON *existing_response = cJSON_GetObjectItem(existing_pairing, "response");
    if (!existing_response) {
        existing_response = cJSON_CreateObject();
        cJSON_AddItemToObject(existing_pairing, "response", existing_response);
        ESP_LOGI(TAG, "Created new response object");
    }
    
    // Handle io_capability
    cJSON *io_cap = cJSON_GetObjectItem(response, "io_capability");
    if (io_cap && cJSON_IsNumber(io_cap)) {
        if (io_cap->valueint >= 0 && io_cap->valueint <= 4) {
            cJSON_DeleteItemFromObject(existing_response, "io_capability");
            cJSON_AddNumberToObject(existing_response, "io_capability", io_cap->valueint);
            ESP_LOGI(TAG, "Updated io_capability: %d (%s)", 
                    io_cap->valueint, get_io_capability_name(io_cap->valueint));
        } else {
            ESP_LOGW(TAG, "Invalid io_capability value: %d (must be 0-4)", io_cap->valueint);
        }
    }
    
    // Handle auth_req - accepts number or flags object
    cJSON *auth_req = cJSON_GetObjectItem(response, "auth_req");
    if (auth_req) {
        uint8_t auth_req_value;
        if (cJSON_IsNumber(auth_req)) {
            auth_req_value = auth_req->valueint;
            ESP_LOGI(TAG, "auth_req provided as number: %d", auth_req_value);
        } else if (cJSON_IsObject(auth_req)) {
            auth_req_value = encode_auth_req_from_json(auth_req);
            ESP_LOGI(TAG, "auth_req provided as flags, encoded to: %d", auth_req_value);
        } else {
            ESP_LOGW(TAG, "Invalid auth_req format");
            goto skip_auth_req;
        }
        
        cJSON_DeleteItemFromObject(existing_response, "auth_req");
        cJSON_AddNumberToObject(existing_response, "auth_req", auth_req_value);
        ESP_LOGI(TAG, "Updated auth_req: 0x%02X (bonding:%d mitm:%d sc:%d keypress:%d)", 
                auth_req_value,
                (auth_req_value & 0x01) != 0,
                (auth_req_value & 0x04) != 0,
                (auth_req_value & 0x08) != 0,
                (auth_req_value & 0x10) != 0);
    }
    skip_auth_req:
    
    // Handle other response fields
    cJSON *oob_flag = cJSON_GetObjectItem(response, "oob_data_flag");
    if (oob_flag && cJSON_IsNumber(oob_flag)) {
        cJSON_DeleteItemFromObject(existing_response, "oob_data_flag");
        cJSON_AddNumberToObject(existing_response, "oob_data_flag", oob_flag->valueint);
        ESP_LOGI(TAG, "Updated oob_data_flag: %d", oob_flag->valueint);
    }
    
    cJSON *max_key = cJSON_GetObjectItem(response, "max_key_size");
    if (max_key && cJSON_IsNumber(max_key)) {
        if (max_key->valueint >= 7 && max_key->valueint <= 16) {
            cJSON_DeleteItemFromObject(existing_response, "max_key_size");
            cJSON_AddNumberToObject(existing_response, "max_key_size", max_key->valueint);
            ESP_LOGI(TAG, "Updated max_key_size: %d", max_key->valueint);
        } else {
            ESP_LOGW(TAG, "Invalid max_key_size: %d (must be 7-16)", max_key->valueint);
        }
    }
    
    cJSON *init_key = cJSON_GetObjectItem(response, "init_key_dist");
    if (init_key && cJSON_IsNumber(init_key)) {
        cJSON_DeleteItemFromObject(existing_response, "init_key_dist");
        cJSON_AddNumberToObject(existing_response, "init_key_dist", init_key->valueint);
        ESP_LOGI(TAG, "Updated init_key_dist: %d", init_key->valueint);
    }
    
    cJSON *resp_key = cJSON_GetObjectItem(response, "resp_key_dist");
    if (resp_key && cJSON_IsNumber(resp_key)) {
        cJSON_DeleteItemFromObject(existing_response, "resp_key_dist");
        cJSON_AddNumberToObject(existing_response, "resp_key_dist", resp_key->valueint);
        ESP_LOGI(TAG, "Updated resp_key_dist: %d", resp_key->valueint);
    }
}

// Handle pairing_info update operation
static esp_err_t handle_pairing_info_update(httpd_req_t *req, cJSON *patch_json, const char *file_path)
{
    cJSON *pairing_info_patch = cJSON_GetObjectItem(patch_json, "pairing_info");
    if (!pairing_info_patch || !cJSON_IsObject(pairing_info_patch)) {
        return ESP_ERR_NOT_FOUND;  // Not a pairing_info update
    }
    
    ESP_LOGI(TAG, "Detected pairing_info update");
    
    // Read existing file
    file_data_t file_data = {0};
    esp_err_t err = web_server_read_json_file(file_path, &file_data);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    } else if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    // Get or create pairing_info object
    cJSON *existing_pairing = cJSON_GetObjectItem(file_data.json, "pairing_info");
    if (!existing_pairing) {
        existing_pairing = cJSON_CreateObject();
        cJSON_AddItemToObject(file_data.json, "pairing_info", existing_pairing);
        ESP_LOGI(TAG, "Created new pairing_info object in ble.json");
    }
    
    // Update fields
    update_pairing_info_fields(existing_pairing, pairing_info_patch);
    
    // Write back to file
    esp_err_t result = write_json_file(file_path, file_data.json);
    
    // Cleanup
    free(file_data.buffer);
    cJSON_Delete(file_data.json);
    
    // Send response
    if (result == ESP_OK) {
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    } else {
        httpd_resp_send_500(req);
    }
    
    return result;
}

// Update BLE characteristic fields
static esp_err_t update_characteristic_fields(httpd_req_t *req, cJSON *characteristic, cJSON *patch_json)
{
    cJSON *properties_item = cJSON_GetObjectItem(patch_json, "properties");
    cJSON *value_item = cJSON_GetObjectItem(patch_json, "value");
    cJSON *dynamic_item = cJSON_GetObjectItem(patch_json, "dynamic");
    
    // Update properties if present
    if (properties_item) {
        if (cJSON_IsNumber(properties_item)) {
            cJSON_DeleteItemFromObject(characteristic, "properties");
            cJSON_AddNumberToObject(characteristic, "properties", properties_item->valueint);
            ESP_LOGI(TAG, "Updated properties to: %d", properties_item->valueint);
        } else {
            ESP_LOGE(TAG, "Properties value is not a number");
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"Properties value must be a number\"}");
            return ESP_FAIL;
        }
    }
    
    // Update value if present
    if (value_item) {
        cJSON *valobj = cJSON_GetObjectItem(characteristic, "value");
        if (!valobj) {
            valobj = cJSON_CreateObject();
            cJSON_AddItemToObject(characteristic, "value", valobj);
        }

        if (cJSON_IsString(value_item)) {
            // existing behavior: update value.data as hex string
            cJSON_DeleteItemFromObject(valobj, "data");
            if (strlen(value_item->valuestring) > 0) {
                cJSON_AddStringToObject(valobj, "data", value_item->valuestring);
                ESP_LOGI(TAG, "Updated value.data to %s", value_item->valuestring);
            } else {
                ESP_LOGI(TAG, "Cleared value.data");
            }
        } else if (cJSON_IsObject(value_item)) {
            // NEW: merge security flags etc. into value object
            cJSON *enc = cJSON_GetObjectItem(value_item, "encryption_required");
            if (enc && cJSON_IsBool(enc)) {
                cJSON_DeleteItemFromObject(valobj, "encryption_required");
                cJSON_AddBoolToObject(valobj, "encryption_required",
                                    cJSON_IsTrue(enc));
                ESP_LOGI(TAG, "Updated value.encryption_required to %s",
                        cJSON_IsTrue(enc) ? "true" : "false");
            }

            cJSON *auth = cJSON_GetObjectItem(value_item, "authentication_required");
            if (auth && cJSON_IsBool(auth)) {
                cJSON_DeleteItemFromObject(valobj, "authentication_required");
                cJSON_AddBoolToObject(valobj, "authentication_required",
                                    cJSON_IsTrue(auth));
                ESP_LOGI(TAG, "Updated value.authentication_required to %s",
                        cJSON_IsTrue(auth) ? "true" : "false");
            }

            // optionally also allow updating "data" from the same object
            cJSON *data = cJSON_GetObjectItem(value_item, "data");
            if (data && cJSON_IsString(data)) {
                cJSON_DeleteItemFromObject(valobj, "data");
                if (strlen(data->valuestring) > 0) {
                    cJSON_AddStringToObject(valobj, "data", data->valuestring);
                    ESP_LOGI(TAG, "Updated value.data to %s", data->valuestring);
                } else {
                    ESP_LOGI(TAG, "Cleared value.data");
                }
            }
        } else {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "error: value must be string or object");
            return ESP_FAIL;
        }
    }

    
    // Update dynamic hooks if present
    if (dynamic_item) {
        cJSON_DeleteItemFromObject(characteristic, "dynamic");
        
        if (cJSON_IsObject(dynamic_item)) {
            bool has_hooks = false;
            if (cJSON_GetObjectItem(dynamic_item, "on_read")) has_hooks = true;
            if (cJSON_GetObjectItem(dynamic_item, "on_write")) has_hooks = true;
            if (cJSON_GetObjectItem(dynamic_item, "on_notify")) has_hooks = true;
            if (cJSON_GetObjectItem(dynamic_item, "on_indicate")) has_hooks = true;
            
            if (has_hooks) {
                cJSON_AddItemToObject(characteristic, "dynamic", cJSON_Duplicate(dynamic_item, 1));
                ESP_LOGI(TAG, "Updated dynamic hooks");
            } else {
                ESP_LOGI(TAG, "Removed dynamic (no hooks)");
            }
        }
    }
    
    return ESP_OK;
}

// Handle BLE characteristic update operation
static esp_err_t handle_characteristic_update(httpd_req_t *req, cJSON *patch_json, const char *file_path)
{
    cJSON *svc_idx = cJSON_GetObjectItem(patch_json, "service_index");
    cJSON *chr_idx = cJSON_GetObjectItem(patch_json, "characteristic_index");
    
    if (!svc_idx || !chr_idx) {
        return ESP_ERR_NOT_FOUND;  // Not a characteristic update
    }
    
    // Read existing file
    file_data_t file_data = {0};
    esp_err_t err = web_server_read_json_file(file_path, &file_data);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    } else if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    // Navigate to the characteristic
    cJSON *services = cJSON_GetObjectItem(file_data.json, "services");
    if (!services || svc_idx->valueint >= cJSON_GetArraySize(services)) {
        ESP_LOGE(TAG, "Invalid service index: %d", svc_idx->valueint);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid service index\"}");
        free(file_data.buffer);
        cJSON_Delete(file_data.json);
        return ESP_FAIL;
    }
    
    cJSON *service = cJSON_GetArrayItem(services, svc_idx->valueint);
    cJSON *characteristics = cJSON_GetObjectItem(service, "characteristics");
    if (!characteristics || chr_idx->valueint >= cJSON_GetArraySize(characteristics)) {
        ESP_LOGE(TAG, "Invalid characteristic index: %d", chr_idx->valueint);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid characteristic index\"}");
        free(file_data.buffer);
        cJSON_Delete(file_data.json);
        return ESP_FAIL;
    }
    
    cJSON *characteristic = cJSON_GetArrayItem(characteristics, chr_idx->valueint);
    ESP_LOGI(TAG, "Updating characteristic for service %d, characteristic %d", svc_idx->valueint, chr_idx->valueint);
    
    // Update characteristic fields
    esp_err_t result = update_characteristic_fields(req, characteristic, patch_json);
    if (result != ESP_OK) {
        free(file_data.buffer);
        cJSON_Delete(file_data.json);
        return result;
    }
    
    // Write back to file
    result = write_json_file(file_path, file_data.json);
    
    // Cleanup
    free(file_data.buffer);
    cJSON_Delete(file_data.json);
    
    // Send response
    if (result == ESP_OK) {
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    } else {
        httpd_resp_send_500(req);
    }
    
    return result;
}

// Handle generic field update operation
static esp_err_t handle_generic_update(httpd_req_t *req, cJSON *patch_json, const char *file_path)
{
    // Read existing file
    file_data_t file_data = {0};
    esp_err_t err = web_server_read_json_file(file_path, &file_data);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    } else if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "About to merge patch");
    
    // Merge patch into existing JSON
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, patch_json) {
        if (item->string) {
            cJSON *existing_item = cJSON_GetObjectItem(file_data.json, item->string);
            if (existing_item != NULL) {
                cJSON_ReplaceItemInObjectCaseSensitive(file_data.json, item->string, cJSON_Duplicate(item, true));
            } else {
                cJSON_AddItemToObject(file_data.json, item->string, cJSON_Duplicate(item, true));
            }
        }
    }
    
    ESP_LOGI(TAG, "About to write merged json");
    
    // Write back to file
    esp_err_t result = write_json_file(file_path, file_data.json);
    
    // Cleanup
    free(file_data.buffer);
    cJSON_Delete(file_data.json);
    
    // Send response
    if (result == ESP_OK) {
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    } else {
        httpd_resp_send_500(req);
    }
    
    return result;
}

// Main PATCH request handler
static esp_err_t handle_patch_request(httpd_req_t *req, const char *file_path, const char *device_id)
{
    request_data_t req_data = {0};
    esp_err_t result = ESP_FAIL;
    
    // Read and parse request body
    if (read_request_body(req, &req_data) != ESP_OK) {
        return ESP_FAIL;
    }
    
    // Try each handler in order of specificity
    result = handle_device_id_change(req, req_data.json, device_id);
    if (result != ESP_ERR_NOT_FOUND) {
        goto cleanup;
    }
    
    result = handle_pairing_info_update(req, req_data.json, file_path);
    if (result != ESP_ERR_NOT_FOUND) {
        goto cleanup;
    }
    
    result = handle_characteristic_update(req, req_data.json, file_path);
    if (result != ESP_ERR_NOT_FOUND) {
        goto cleanup;
    }
    
    // Fall back to generic update
    result = handle_generic_update(req, req_data.json, file_path);
    
cleanup:
    if (req_data.buffer) free(req_data.buffer);
    if (req_data.json) cJSON_Delete(req_data.json);
    
    return result;
}



// ── HTTPD handler ────────────────────────────────────────────────── 
// /api/device/...

static esp_err_t httpd_device_handler(httpd_req_t *req)
{
    const char *prefix = "/api/device/";
    const char *path = req->uri + strlen(prefix);
    char file_path[256];
    char device_id[64] = {0};

    const char *slash = strchr(path, '/');

    if (slash == NULL) {
        // No sub-resource: /api/device/devicename 
        strncpy(device_id, path, sizeof(device_id) - 1);
        ESP_LOGI(TAG, "No sub-resoruce, serving full device: %s", device_id);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    size_t id_len = slash - path;
    strncpy(device_id, path, id_len);
    device_id[id_len] = '\0';

    // Strip trailing ".json" from sub-resource name for uniform matching
    char sub[64] = {0};
    strncpy(sub, slash + 1, sizeof(sub) - 1);
    char *dot = strstr(sub, ".json");
    if (dot) *dot = '\0';

    // "manifest" is self-describing — serve directly without reading itself
    if (strcmp(sub, "info") == 0 || strcmp(sub, "manifest") == 0) {
        snprintf(file_path, sizeof(file_path),
                "/"LITTLEFS_LABEL"/devices/%s/manifest.json", device_id);
        if (req->method == HTTP_GET)
            return serve_static_json(req, file_path);
        if (req->method == HTTP_PATCH)
            return handle_patch_request(req, file_path, device_id);
    }
    else if (strcmp(sub, "adv") == 0 || strcmp(sub, "advertisement") == 0 || strcmp(sub, "adv.json") == 0) {
        // Resolve via manifest so custom adv paths work
        char *adv_path = manifest_resolve_path(device_id, "advertisement");
        if (!adv_path) {
            // Fallback: hardcoded default path 
            snprintf(file_path, sizeof(file_path),
                       "/" LITTLEFS_LABEL "/devices/%s/peripheral/adv.json", device_id);
        }
        if (req->method == HTTP_GET) {
            ESP_LOGI(TAG, "Serving peripheral/adv.json for %s", device_id);
            return serve_static_json(req, adv_path);
        } else if (req->method == HTTP_PATCH) {
            ESP_LOGI(TAG, "Patching peripheral/adv.json for %s", device_id);
            return handle_patch_request(req, adv_path, device_id);
        }
    }

    else if (strcmp(sub, "adv") == 0 || strcmp(sub, "advertisement") == 0 || strcmp(sub, "adv.json") == 0) {
        char *adv_path = manifest_resolve_path(device_id, "advertisement");
        if (adv_path) {
            strncpy(file_path, adv_path, sizeof(file_path) - 1);
            file_path[sizeof(file_path) - 1] = '\0';
            free(adv_path);
        } else {
            // Fallback: advertisement key missing from manifest
            ESP_LOGW(TAG, "key 'advertisement' not found in manifest: %s, using default path", device_id);
            snprintf(file_path, sizeof(file_path),
                    LITTLEFS_LABEL "/devices/%s/peripheral/adv.json", device_id);
        }
        if (req->method == HTTP_GET) {
            ESP_LOGI(TAG, "Serving peripheral/adv.json for %s", device_id);
            return serve_static_json(req, file_path);
        } else if (req->method == HTTP_PATCH) {
            ESP_LOGI(TAG, "Patching peripheral/adv.json for %s", device_id);
            return handle_patch_request(req, file_path, device_id);
        }
    }

    // All other sub-resources: resolve path dynamically from manifest.json
    char *resolved = manifest_resolve_path(device_id, sub);
    if (!resolved) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    strncpy(file_path, resolved, sizeof(file_path) - 1);
    file_path[sizeof(file_path) - 1] = '\0';
    free(resolved);


    if (req->method == HTTP_GET)
        return serve_static_json(req, file_path);
    if (req->method == HTTP_PATCH)
        return handle_patch_request(req, file_path, device_id);

    httpd_resp_send_404(req);
    return ESP_FAIL;
}

// HTML page that fetches and displays the JSON
esp_err_t httpd_device_editor_page(httpd_req_t *req) {
    return serve_static_html(req, "editor.html");
}


// ── Register handlers ────────────────────────────────────────────────── 

uint8_t register_device_editor_handlers_in_web_server(httpd_handle_t *server)
{
    uint8_t handler_count = 0;

    // editor page
    httpd_uri_t device_editor_uri = {
        .uri = "/device/*",
        .method = HTTP_GET,
        .handler = httpd_device_editor_page,
        .user_ctx = NULL,
        .is_websocket = false,
    };
    httpd_register_uri_handler(*server, &device_editor_uri);
    handler_count++;

    // serve JSON file (/api/device/devicename/[info|ble|interface])
    httpd_uri_t get_device_uri = {
        .uri = "/api/device/*",
        .method = HTTP_GET,
        .handler = httpd_device_handler,
        .user_ctx = NULL,
        .is_websocket = false,
    };
    httpd_register_uri_handler(*server, &get_device_uri);
    handler_count++;

    // edit JSON file
    httpd_uri_t patch_device_uri = {
        .uri = "/api/device/*",
        .method = HTTP_PATCH,
        .handler = httpd_device_handler,
        .user_ctx = NULL,
        .is_websocket = false,
    };
    httpd_register_uri_handler(*server, &patch_device_uri);
    handler_count++;

    return handler_count;
}