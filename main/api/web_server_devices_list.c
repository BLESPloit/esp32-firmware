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

static const char *TAG = "web server - devices";

// list devices handler
static esp_err_t devices_list_handler(httpd_req_t *req) {
    const char *fragments[] = { "head.html", "devices_body.html", "footer.html" };
    return serve_html_fragments(req, fragments, 3);
}

// Scan all available devices and build JSON array
static char* scan_devices_folder(void) {
    cJSON *devices_array = cJSON_CreateArray();
    char *devicesdir = "/" LITTLEFS_LABEL "/devices";

    DIR *dir = opendir(devicesdir);
    if (dir == NULL) {
        // Directory doesn't exist yet — not an error, just no devices
        // Fall through and return an empty JSON array "[]"
        char *json_str = cJSON_PrintUnformatted(devices_array);
        cJSON_Delete(devices_array);
        return json_str;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR &&
            strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0)
        {
            device_meta_t *meta = manifest_load_meta(entry->d_name);
            if (!meta) continue;

            cJSON *device_obj = cJSON_CreateObject();
            if (meta->name)        cJSON_AddStringToObject(device_obj, "name",        meta->name);
            if (meta->description) cJSON_AddStringToObject(device_obj, "description", meta->description);
            if (meta->model_url)      cJSON_AddStringToObject(device_obj, "model_url",   meta->model_url);
            // if (meta->notes)      cJSON_AddStringToObject(device_obj, "notes",   meta->notes); // may be long
            if (meta->icon_url)    cJSON_AddStringToObject(device_obj, "icon",        meta->icon_url);
            cJSON_AddStringToObject(device_obj, "folder", entry->d_name);
            cJSON_AddItemToArray(devices_array, device_obj);

            device_meta_free(meta);
            free(meta);

        }
    }
    closedir(dir);

    char *json_str = cJSON_PrintUnformatted(devices_array);
    cJSON_Delete(devices_array);
    return json_str;
}



// Broadcast the device list to all WS clients
void web_devices_broadcast_list(void) {
    char *json_data = scan_devices_folder();
    if (!json_data) return;

    // Wrap the raw array in a typed message
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "devices_list");
    cJSON *arr = cJSON_Parse(json_data);
    free(json_data);
    if (arr) cJSON_AddItemToObject(msg, "devices", arr);

    char *out = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (out) {
        websocket_broadcast_json_transient(out);  // transient: list is rebuilt on each request
        free(out);
    }
}

static bool devices_ws_handler(const char *type, cJSON *json) {
    if (strcmp(type, "devices") != 0) return false;

    cJSON *act = cJSON_GetObjectItemCaseSensitive(json, "action");
    if (cJSON_IsString(act) && strcmp(act->valuestring, "list") == 0) {
        web_devices_broadcast_list();
    }
    return true;
}


// Handler for /api/devices endpoint
static esp_err_t devices_api_handler(httpd_req_t *req) {
    char *json_data = scan_devices_folder();
    
    if (json_data == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to scan devices");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_data, strlen(json_data));
    free(json_data);
    return ESP_OK;
}



// ── Register handlers ────────────────────────────────────────────────── 

uint8_t register_devices_list_handlers_in_web_server(httpd_handle_t *server)
{
    uint8_t handler_count = 0;

    ws_register_message_handler(devices_ws_handler);

    httpd_uri_t uri_devices_list = {
        .uri = "/devices",
        .method = HTTP_GET,
        .handler = devices_list_handler,
        .user_ctx = NULL,
        .is_websocket = false,
    };
    httpd_register_uri_handler(*server, &uri_devices_list);
    handler_count++;

    httpd_uri_t api_devices_uri = {
        .uri = "/api/devices",
        .method = HTTP_GET,
        .handler = devices_api_handler,
        .user_ctx = NULL,
        .is_websocket = false,
    };
    httpd_register_uri_handler(*server, &api_devices_uri);
    handler_count++;

    return handler_count;
}