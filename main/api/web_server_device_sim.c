#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

#include "ble/ble_sim.h"
#include "interface/interface_sim.h"
#include "api/web_server.h"
#include "api/web_server_internal.h"

static const char *TAG = "web server - device sim";

extern char current_simulated_device[128];
extern SemaphoreHandle_t simulation_mutex;

// forward declarations
void web_sim_stop(void);

const char *web_sim_get_device(void) {
    return strlen(current_simulated_device) ? current_simulated_device : NULL;
}


void web_sim_broadcast_status(void) {
    xSemaphoreTake(simulation_mutex, portMAX_DELAY);

    cJSON *status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "type", "sim_status");
    if (strlen(current_simulated_device) > 0) {
        cJSON_AddStringToObject(status, "status", "started");
        cJSON_AddStringToObject(status, "device", current_simulated_device);
        cJSON *adv = ble_adv_set_to_json();
        cJSON_AddItemToObject(status, "adv", adv ? adv : cJSON_CreateArray());
    } else {
        cJSON_AddStringToObject(status, "status", "stopped");
        cJSON_AddNullToObject(status, "device");
    }

    char *json_str = cJSON_PrintUnformatted(status);
    cJSON_Delete(status);
    xSemaphoreGive(simulation_mutex);

    if (json_str) {
        websocket_broadcast_json(json_str);   // saved to state, replayed on reconnect
        free(json_str);
    }
}


void web_sim_start(const char *device_folder) {
    xSemaphoreTake(simulation_mutex, portMAX_DELAY);

    // If the same device is already running, do nothing
    if (strlen(current_simulated_device) > 0) {
        bool same = (strcmp(current_simulated_device, device_folder) == 0);
        xSemaphoreGive(simulation_mutex);
        if (same) {
            WS_LOGW(TAG, "Simulation for '%s' already running, ignoring start", device_folder);
            web_sim_broadcast_status(); // let client know current state
            return;
        }
        // Different device requested — stop the current one first
        xSemaphoreGive(simulation_mutex);
        web_sim_stop();
        xSemaphoreTake(simulation_mutex, portMAX_DELAY);
    }

    strncpy(current_simulated_device, device_folder, sizeof(current_simulated_device) - 1);
    current_simulated_device[sizeof(current_simulated_device) - 1] = '\0';
    esp_err_t ret = load_ble_device_for_simulation(current_simulated_device);
    xSemaphoreGive(simulation_mutex);
    if (ret != ESP_OK) {
        WS_LOGE(TAG, "Simulation failed for: %s", device_folder);
        current_simulated_device[0] = '\0';
    } else {
        ESP_LOGI(TAG, "Started simulation for: %s", current_simulated_device);
    }
    web_sim_broadcast_status();
}


void web_sim_stop(void) {
    xSemaphoreTake(simulation_mutex, portMAX_DELAY);
    current_simulated_device[0] = '\0';
    xSemaphoreGive(simulation_mutex);

    unload_ble_device_for_simulation();

    ESP_LOGI(TAG, "Stopped simulation");
    web_sim_broadcast_status();
}

// websocket messages handler
static bool sim_ws_handler(const char *type, cJSON *json) {
    if (strcmp(type, "sim") != 0) return false;

    cJSON *act = cJSON_GetObjectItemCaseSensitive(json, "action");
    const char *action = cJSON_IsString(act) ? act->valuestring : NULL;

    if      (action && strcmp(action, "start")  == 0) {
        cJSON *dev = cJSON_GetObjectItemCaseSensitive(json, "device");
        if (cJSON_IsString(dev)) web_sim_start(dev->valuestring);
    }
    else if (action && strcmp(action, "stop")   == 0) web_sim_stop();
    else if (action && strcmp(action, "status") == 0) web_sim_broadcast_status();
    
    else WS_LOGW(TAG, "sim: unknown action '%s'", action ?: "null");

    return true;
}

// user pressed a button on simulation
static bool sim_button_ws_handler(const char *type, cJSON *json) {
    if (strcmp(type, "sim_button") != 0) return false;
    cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
    if (cJSON_IsString(id)) interface_call_lua(id->valuestring);
    return true;
}

// ── HTML page ──

esp_err_t httpd_simulate_page(httpd_req_t *req) {
    const char *fragments[] = {
        "head.html",
        "sim_body.html",
        "footer.html"
    };
    return serve_html_fragments(req, fragments, 3);
}


// register handlers
uint8_t register_device_sim_handlers_in_web_server(httpd_handle_t *server)
{
    ws_register_message_handler(sim_ws_handler);
    ws_register_message_handler(sim_button_ws_handler);

    httpd_uri_t simulation_uri = {
        .uri = "/sim/*",
        .method = HTTP_GET,
        .handler = httpd_simulate_page,
        .user_ctx = NULL,
        .is_websocket = false,
    };
    httpd_register_uri_handler(*server, &simulation_uri);

    return 1;
}