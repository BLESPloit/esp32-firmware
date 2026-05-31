#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "nvs.h"
#include "esp_littlefs.h"
#include "ble/ble_sim.h" // ADV_INT...
#include "graphics/graphics.h"
#include "common/storage.h"
#include "api/wifi.h"

static const char *TAG = "storage";

device_config_t config = {
        .net_enabled = {
            .value.u8 = true,
            .type = CONFIG_TYPE_BOOL,
            .nvs_name = "net"
        },
        .wifi_ssid = {
            .value.str = NULL,
            .type = CONFIG_TYPE_STR,
            .nvs_name = "ssid"
        },
        .wifi_psk = {
            .value.str = NULL,
            .type = CONFIG_TYPE_STR,
            .nvs_name = "psk"
        },
        .wifi_ap_ssid = {
            .value.str = NULL,
            .type = CONFIG_TYPE_STR,
            .nvs_name = "apssid"
        },
        .wifi_ap_psk = {
            .value.str = NULL,
            .type = CONFIG_TYPE_STR,
            .nvs_name = "appsk"
        },
        .wifi_mode_pref = {
            .value.u8 = WIFI_MODE_PREF_STA_FIRST,
            .type = CONFIG_TYPE_UINT8,
            .nvs_name = "wifimode"
        }
    };

// ── LittleFS operations ────────────────────────────────────────────────── 

esp_err_t initialize_littlefs(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/" LITTLEFS_LABEL,
        .partition_label = LITTLEFS_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS (%s)", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    // Log filesystem info
    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS: %d KB total, %d KB used", total / 1024, used / 1024);
    }

    return ESP_OK;
}

// create a new empty device manifest
esp_err_t create_manifest_json(const char *directory, const char *name) {
    // Create JSON object
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_ERR_NO_MEM;
    }

    // Add fields to JSON
    cJSON_AddStringToObject(root, "name", name ? name : "");
    cJSON_AddStringToObject(root, "description", "");

    // Convert JSON to string
    char *json_str = cJSON_Print(root);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to convert JSON to string");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    // Create file path
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/manifest.json", directory);

    // Write to file
    FILE *f = fopen(filepath, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        cJSON_free(json_str);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    fprintf(f, "%s", json_str);
    fclose(f);

    ESP_LOGI(TAG, "Created manifest.json at: %s", filepath);

    // Clean up
    cJSON_free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

// create a folder (device BDADDR) and save JSON to file (ble.json)
void save_json_to_file(const ble_addr_t *addr, cJSON *json)
{
    // Create filename from address
    char path[64], json_path[80], old_json_path[84];

    snprintf(path, sizeof(path), "/"LITTLEFS_LABEL"/devices/%02x%02x%02x%02x%02x%02x",
             addr->val[5], addr->val[4], addr->val[3],
             addr->val[2], addr->val[1], addr->val[0]);
    
    // create a folder if not exists
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0755);
    }
    
    snprintf(json_path, sizeof(json_path), "%s/ble.json", path);
    snprintf(old_json_path, sizeof(old_json_path), "%s/ble.json.old", path);
    
    if (stat(json_path, &st) == 0) {
        ESP_LOGI(TAG, "Moving old scan to %s", old_json_path);
        rename(json_path, old_json_path);
    }
    
    ESP_LOGI(TAG,"Saving JSON to file: %s", json_path);
    
    // Convert JSON to string
    char *json_string = cJSON_Print(json);
    if (!json_string) {
        ESP_LOGE(TAG,"Failed to serialize JSON");
        return;
    }
    
    // Open file for writing
    FILE *f = fopen(json_path, "w");
    if (!f) {
        ESP_LOGE(TAG,"Failed to open file for writing: %s", json_path);
        free(json_string);
        return;
    }
    
    // Write JSON string to file
    size_t written = fwrite(json_string, 1, strlen(json_string), f);
    fclose(f);
    
    if (written == strlen(json_string)) {
        ESP_LOGI(TAG, "Successfully saved JSON (%zu bytes) to %s", written, json_path);
    } else {
        ESP_LOGE(TAG, "Failed to write complete JSON to file");
    }
    
    free(json_string);

    // create new template info.json
    create_manifest_json(path, "Scanned");

}

// ── NVS operations ────────────────────────────────────────────────── 

esp_err_t initialize_nvs(void)
{
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t open_nvs_handle(nvs_handle_t *handle)
{
    ESP_LOGI(TAG, "Opening Non-Volatile Storage (NVS) handle... ");
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,"Error (%s) opening NVS handle!", esp_err_to_name(err));
        return ESP_FAIL;
    }
    else return ESP_OK;
}

void close_nvs_handle(nvs_handle_t *handle)
{
    ESP_LOGI(TAG, "Closing Non-Volatile Storage (NVS) handle... ");
    nvs_close(*handle);
}


// Write a single config value to NVS storage
esp_err_t save_param_to_nvs(nvs_handle_t handle, const config_param_t *param, bool commit)
{
    esp_err_t err;
    if (!handle || !param) return ESP_ERR_INVALID_ARG;

    switch (param->type) {
        case CONFIG_TYPE_UINT8:
            err = nvs_set_u8(handle, param->nvs_name, param->value.u8);
            break;
        case CONFIG_TYPE_BOOL:
            err = nvs_set_u8(handle, param->nvs_name, param->value.u8);
            break;
        case CONFIG_TYPE_UINT16:
            err = nvs_set_u16(handle, param->nvs_name, param->value.u16);
            break;
        case CONFIG_TYPE_UINT32:
            err = nvs_set_u32(handle, param->nvs_name, param->value.u32);
            break;
        case CONFIG_TYPE_STR:
            if (param->value.str) {
                err = nvs_set_str(handle, param->nvs_name, param->value.str);
            } else {
                err = nvs_erase_key(handle, param->nvs_name);
                if (err == ESP_ERR_NVS_NOT_FOUND)
                    err = ESP_OK;
            }
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error writing '%s' to nvs!", param->nvs_name);
    } else {
        switch(param->type) {
            case CONFIG_TYPE_UINT8:
                ESP_LOGI(TAG, "Write '%s' (u8): %u success", param->nvs_name, param->value.u8);
                break;
            case CONFIG_TYPE_BOOL:
                ESP_LOGI(TAG, "Write '%s' (bool): %s success", param->nvs_name, param->value.u8 ? "true" : "false");
                break;
            case CONFIG_TYPE_UINT16:
                ESP_LOGI(TAG, "Write '%s' (u16): %u success", param->nvs_name, param->value.u16);
                break;
            case CONFIG_TYPE_UINT32:
                ESP_LOGI(TAG, "Write '%s' (u32): %lu success", param->nvs_name, param->value.u32);
                break;
            case CONFIG_TYPE_STR:
                if (param->value.str)
                    ESP_LOGI(TAG, "Write '%s' (str): %s success", param->nvs_name, param->value.str);
                else
                    ESP_LOGI(TAG, "Erase '%s' success", param->nvs_name);
                break;
        }
    }

    if ((err == ESP_OK) && commit)
        err = nvs_commit(handle);

    return err;
}

// Write all config values. As of now take the config values from globals
esp_err_t write_config_nvs(void)
{
    esp_err_t err;
    nvs_handle_t handle;

    err = open_nvs_handle(&handle);
    if (err != ESP_OK)
        return err;

    // write all values, do not commit yet
    err = save_param_to_nvs(handle, &config.net_enabled, false);
    if (err != ESP_OK) goto error;
    err = save_param_to_nvs(handle, &config.wifi_ssid, false);
    if (err != ESP_OK) goto error;
    err = save_param_to_nvs(handle, &config.wifi_psk, false);
    if (err != ESP_OK) goto error;
    err = save_param_to_nvs(handle, &config.wifi_ap_ssid, false);
    if (err != ESP_OK) goto error;
    err = save_param_to_nvs(handle, &config.wifi_ap_psk, false);
    if (err != ESP_OK) goto error;
    err = save_param_to_nvs(handle, &config.wifi_mode_pref, false);
    if (err != ESP_OK) goto error;

    // Commit written value.
    // After setting any values, nvs_commit() must be called to ensure changes are written to flash storage. 
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,"Error commiting config to nvs!");        
    } else {
        ESP_LOGI(TAG, "Commit config success");
    }

error:
    close_nvs_handle(&handle);
    return err;

}

// Read a single parameter
esp_err_t load_param_from_nvs(nvs_handle_t handle, config_param_t *param)
{
    esp_err_t err;
    if (!handle || !param) return ESP_ERR_INVALID_ARG;

    switch (param->type) {
        case CONFIG_TYPE_UINT8:
            err = nvs_get_u8(handle, param->nvs_name, &param->value.u8);
            break;
        case CONFIG_TYPE_BOOL:
            err = nvs_get_u8(handle, param->nvs_name, &param->value.u8);
            break;
        case CONFIG_TYPE_UINT16:
            err = nvs_get_u16(handle, param->nvs_name, &param->value.u16);
            break;
        case CONFIG_TYPE_UINT32:
            err = nvs_get_u32(handle, param->nvs_name, &param->value.u32);
            break;
        case CONFIG_TYPE_STR: {
            size_t size;
            // check the size first to allocate buffer
            err = nvs_get_str(handle, param->nvs_name, NULL, &size);
            if (err == ESP_OK && size > 0) {
                char *buf = malloc(size);
                if (!buf) {
                    err = ESP_ERR_NO_MEM;
                    break;
                }
                err = nvs_get_str(handle, param->nvs_name, buf, &size);
                if (err == ESP_OK) {
                    param->value.str = buf;
                } else {
                    free(buf);
                }
            }
            break;
        }
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error reading '%s' from nvs!", param->nvs_name);
    } else {
        switch(param->type) {
            case CONFIG_TYPE_UINT8:
                ESP_LOGI(TAG, "Read '%s' (u8): %u success", param->nvs_name, param->value.u8);
                break;
            case CONFIG_TYPE_BOOL:
                ESP_LOGI(TAG, "Read '%s' (bool): %s success", param->nvs_name, param->value.u8 ? "true" : "false");
                break;
            case CONFIG_TYPE_UINT16:
                ESP_LOGI(TAG, "Read '%s' (u16): %u success", param->nvs_name, param->value.u16);
                break;
            case CONFIG_TYPE_UINT32:
                ESP_LOGI(TAG, "Read '%s' (u32): %lu (0x%08"PRIx32") success", param->nvs_name, param->value.u32, param->value.u32);
                break;
            case CONFIG_TYPE_STR:
                ESP_LOGI(TAG, "Read '%s' (str): %s success", param->nvs_name, param->value.str);
                break;
        }
    }

    return err;
}


// Read all config values and save into global "config". Store in nvs if generated
esp_err_t read_config_nvs(void)
{
    esp_err_t err;
    nvs_handle_t handle;
    bool write_commit_needed = false;

    err = open_nvs_handle(&handle);
    if (err != ESP_OK)
        return err;

    err = load_param_from_nvs(handle, &config.net_enabled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Setting initial value for %s: %d", config.net_enabled.nvs_name, true);
        config.net_enabled.value.u8 = true;
        err = save_param_to_nvs(handle, &config.net_enabled, false);
        if (err != ESP_OK) return err;
        write_commit_needed = true;
    }

    err = load_param_from_nvs(handle, &config.wifi_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,"Wifi STA ssid not set, will run in AP mode");
    }

    err = load_param_from_nvs(handle, &config.wifi_psk);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,"Wifi STA psk not set, will run in AP mode");
    }

    err = load_param_from_nvs(handle, &config.wifi_ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wifi AP ssid not set (use default MAC-based)");
    }

    err = load_param_from_nvs(handle, &config.wifi_ap_psk);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wifi AP psk not set (use default)");
    }

    err = load_param_from_nvs(handle, &config.wifi_mode_pref);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wifi mode pref not set, default STA-first");
        config.wifi_mode_pref.value.u8 = WIFI_MODE_PREF_STA_FIRST;
        err = save_param_to_nvs(handle, &config.wifi_mode_pref, false);
        if (err != ESP_OK)
            return err;
        write_commit_needed = true;
    } else if (config.wifi_mode_pref.value.u8 > WIFI_MODE_PREF_AP_ONLY) {
        ESP_LOGW(TAG, "Invalid wifi mode %u, resetting to STA-first", config.wifi_mode_pref.value.u8);
        config.wifi_mode_pref.value.u8 = WIFI_MODE_PREF_STA_FIRST;
        err = save_param_to_nvs(handle, &config.wifi_mode_pref, false);
        if (err != ESP_OK)
            return err;
        write_commit_needed = true;
    }

    // commit writes if needed
    if (write_commit_needed) {
        err = nvs_commit(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG,"Error commiting config to nvs!");        
        } else {
            ESP_LOGI(TAG, "Commit config success");
        }
    }

    close_nvs_handle(&handle);
    return err;
}


void timestamp_to_string(uint32_t timestamp, char *strftime_buf, size_t bufsize) {
    time_t time = (time_t)timestamp;
    struct tm timeinfo;

    gmtime_r(&time, &timeinfo);
    strftime(strftime_buf, bufsize, "%Y-%m-%d %H:%M:%S", &timeinfo);
//    ESP_LOGI(TAG, "UTC time: %s", strftime_buf);
}
