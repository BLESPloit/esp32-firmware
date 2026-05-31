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
#include "ble/ble_central.h"
#include "ble/ble_scan.h"
#include "graphics/graphics.h"
#include "interface/interface_central.h"
#include "common/storage.h"
#include "common/utils.h"
#include "api/web_server.h"
#include "api/web_server_internal.h"


static const char *TAG = "web server - device central";

extern char current_central_device[128];
extern SemaphoreHandle_t device_central_mutex;


const char *web_central_get_device(void) {
    return strlen(current_central_device) ? current_central_device : NULL;
}

void web_central_set_device(const char *device_id) {
    xSemaphoreTake(device_central_mutex, portMAX_DELAY);
    if (device_id && device_id[0]) {
        strncpy(current_central_device, device_id, sizeof(current_central_device) - 1);
        current_central_device[sizeof(current_central_device) - 1] = '\0';
    } else {
        current_central_device[0] = '\0';
    }
    xSemaphoreGive(device_central_mutex);
}


// Get current "central" status
static esp_err_t central_status_handler(httpd_req_t *req) {
    xSemaphoreTake(device_central_mutex, portMAX_DELAY);
    
    cJSON *status = cJSON_CreateObject();
    if (strlen(current_central_device) > 0) {
        cJSON_AddStringToObject(status, "connected", current_central_device);
    } else {
        cJSON_AddNullToObject(status, "connected");
    }
    
    char *json_str = cJSON_PrintUnformatted(status);
    cJSON_Delete(status);
    xSemaphoreGive(device_central_mutex);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ESP_OK;
}



// called from websocket dispatcher (web_server_websocket.c)
void web_central_start(const char *device_folder) {
    xSemaphoreTake(device_central_mutex, portMAX_DELAY);
    strncpy(current_central_device, device_folder, sizeof(current_central_device) - 1);
    current_central_device[sizeof(current_central_device) - 1] = '\0';
    ble_central_load_services_and_connect(current_central_device);
    xSemaphoreGive(device_central_mutex);

    ESP_LOGI(TAG, "Started central for: %s", current_central_device);
    web_central_broadcast_status();
}

void web_central_stop(void) {
    xSemaphoreTake(device_central_mutex, portMAX_DELAY);
    current_central_device[0] = '\0';
    xSemaphoreGive(device_central_mutex);

    if (!ble_central_is_active()) { 
        ESP_LOGW(TAG, "central already stopped, ignoring duplicate stop");
        return;
    }

    unload_ble_device_for_central();

    ESP_LOGI(TAG, "Stopped central");
    web_central_broadcast_status();
}

void web_central_broadcast_status(void) {
    xSemaphoreTake(device_central_mutex, portMAX_DELAY);

    cJSON *status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "type", "central_status");
    if (strlen(current_central_device) > 0) {
        cJSON_AddStringToObject(status, "status", "started");
        cJSON_AddStringToObject(status, "device", current_central_device);
    } else {
        cJSON_AddStringToObject(status, "status", "stopped");
        cJSON_AddNullToObject(status, "device");
    }

    char *json_str = cJSON_PrintUnformatted(status);
    cJSON_Delete(status);
    xSemaphoreGive(device_central_mutex);

    if (json_str) {
        websocket_broadcast_json(json_str);  // saved to state, replayed on reconnect
        free(json_str);
    }
}


// websocket messages handler
static bool central_ws_handler(const char *type, cJSON *json) {
    if (strcmp(type, "central") != 0) return false;

    cJSON *act = cJSON_GetObjectItemCaseSensitive(json, "action");
    const char *action = cJSON_IsString(act) ? act->valuestring : NULL;

    if      (action && strcmp(action, "start")  == 0) {
        cJSON *dev = cJSON_GetObjectItemCaseSensitive(json, "device");
        if (cJSON_IsString(dev)) web_central_start(dev->valuestring);
    }
    else if (action && strcmp(action, "stop")   == 0) web_central_stop();
    else if (action && strcmp(action, "status") == 0) web_central_broadcast_status();
    else if (action && strcmp(action, "menu_select") == 0) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
        if (cJSON_IsString(id)) interface_central_web_select(id->valuestring);
    }
    else ESP_LOGW(TAG, "central: unknown action '%s'", action ?: "null");

    return true;
}

// HTML page with device "central"
esp_err_t httpd_central_page(httpd_req_t *req) {
    const char *fragments[] = {
        "head.html",        // <html><head>...</head><body>
        "central_body.html",// page-specific content
        "footer.html"       // </body></html> + shared scripts
    };
    return serve_html_fragments(req, fragments, 3);

}


// ── Register handlers ────────────────────────────────────────────────── 

uint8_t register_device_central_handlers_in_web_server(httpd_handle_t *server)
{
    ws_register_message_handler(central_ws_handler);

    httpd_uri_t central_uri = {
        .uri = "/central/*",
        .method = HTTP_GET,
        .handler = httpd_central_page,
        .user_ctx = NULL,
        .is_websocket = false,
    };

    httpd_register_uri_handler(*server, &central_uri);

    return 1;
}