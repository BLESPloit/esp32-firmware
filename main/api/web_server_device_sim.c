#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

#include "ble/ble_sim.h"
#include "ble/ble_init.h"
#include "interface/interface_sim.h"
#include "common/storage.h"
#include "api/web_server.h"
#include "api/web_server_internal.h"

static const char *TAG = "web server - device sim";

#define SIM_AUTOSTART_MAX_FAIL 3
#define SIM_AUTOSTART_BLE_SYNC_TIMEOUT_MS 10000

extern char current_simulated_device[128];
extern SemaphoreHandle_t simulation_mutex;
extern device_config_t config;

// forward declarations
void web_sim_broadcast_status(void);
static void web_sim_stop_internal(bool user_initiated);

const char *web_sim_get_device(void) {
    return strlen(current_simulated_device) ? current_simulated_device : NULL;
}

static void sim_autostart_add_status_fields(cJSON *status)
{
    cJSON_AddBoolToObject(status, "autostart", config.sim_autostart_enabled.value.u8 != 0);
    if (config.sim_autostart_device.value.str && config.sim_autostart_device.value.str[0]) {
        cJSON_AddStringToObject(status, "autostart_device", config.sim_autostart_device.value.str);
    } else {
        cJSON_AddNullToObject(status, "autostart_device");
    }
    cJSON_AddBoolToObject(status, "autostart_blocked",
                          config.sim_autostart_fail_count.value.u8 >= SIM_AUTOSTART_MAX_FAIL);
}

static void sim_autostart_clear_failures(void)
{
    if (config.sim_autostart_fail_count.value.u8 != 0) {
        config.sim_autostart_fail_count.value.u8 = 0;
        write_config_nvs();
    }
}

static void sim_autostart_disable_if_matches(const char *stopped_device)
{
    if (!config.sim_autostart_enabled.value.u8) {
        return;
    }
    const char *autostart_dev = config.sim_autostart_device.value.str;
    if (!stopped_device || stopped_device[0] == '\0' ||
        !autostart_dev || autostart_dev[0] == '\0') {
        return;
    }
    if (strcmp(stopped_device, autostart_dev) != 0) {
        return;
    }
    config.sim_autostart_enabled.value.u8 = 0;
    if (write_config_nvs() != ESP_OK) {
        WS_LOGE(TAG, "Failed to persist sim autostart disable on stop");
    } else {
        ESP_LOGI(TAG, "Sim autostart disabled (manual stop of %s)", stopped_device);
    }
}

void sim_autostart_set(bool enabled, const char *device_id)
{
    config.sim_autostart_enabled.value.u8 = enabled ? 1 : 0;

    if (enabled) {
        if (!device_id || device_id[0] == '\0') {
            WS_LOGW(TAG, "autostart enable requires device id");
            return;
        }
        char *copy = strdup(device_id);
        if (!copy) {
            WS_LOGE(TAG, "autostart enable: out of memory");
            return;
        }
        if (config.sim_autostart_device.value.str) {
            free(config.sim_autostart_device.value.str);
        }
        config.sim_autostart_device.value.str = copy;
        config.sim_autostart_fail_count.value.u8 = 0;
    }

    if (write_config_nvs() != ESP_OK) {
        WS_LOGE(TAG, "Failed to persist sim autostart settings");
    } else {
        ESP_LOGI(TAG, "Sim autostart %s%s%s",
                 enabled ? "enabled for " : "disabled",
                 enabled ? device_id : "",
                 enabled ? "" : "");
    }
    web_sim_broadcast_status();
}

static void sim_autostart_try_boot(void)
{
    if (!config.sim_autostart_enabled.value.u8) {
        ESP_LOGI(TAG, "Sim autostart disabled");
        return;
    }

    const char *device = config.sim_autostart_device.value.str;
    if (!device || device[0] == '\0') {
        ESP_LOGW(TAG, "Sim autostart enabled but no device configured");
        return;
    }

    if (config.sim_autostart_fail_count.value.u8 >= SIM_AUTOSTART_MAX_FAIL) {
        ESP_LOGW(TAG, "Sim autostart blocked after %u failures — disabling",
                 config.sim_autostart_fail_count.value.u8);
        config.sim_autostart_enabled.value.u8 = 0;
        write_config_nvs();
        return;
    }

    config.sim_autostart_fail_count.value.u8++;
    if (write_config_nvs() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist sim autostart fail counter");
        return;
    }

    ESP_LOGI(TAG, "Sim autostart attempt %u for device: %s",
             config.sim_autostart_fail_count.value.u8, device);

    web_sim_start(device);

    const char *running = web_sim_get_device();
    if (running && strcmp(running, device) == 0) {
        sim_autostart_clear_failures();
        ESP_LOGI(TAG, "Sim autostart succeeded");
        return;
    }

    ESP_LOGE(TAG, "Sim autostart failed for device: %s", device);
    if (config.sim_autostart_fail_count.value.u8 >= SIM_AUTOSTART_MAX_FAIL) {
        ESP_LOGW(TAG, "Sim autostart disabled after %u consecutive failures",
                 SIM_AUTOSTART_MAX_FAIL);
        config.sim_autostart_enabled.value.u8 = 0;
        write_config_nvs();
    }
}

static void sim_autostart_boot_task(void *arg)
{
    (void)arg;

    if (!ble_wait_for_sync(SIM_AUTOSTART_BLE_SYNC_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "BLE sync timeout — skipping sim autostart");
        vTaskDelete(NULL);
        return;
    }

    sim_autostart_try_boot();
    vTaskDelete(NULL);
}

void sim_autostart_start_boot_task(void)
{
    if (xTaskCreate(sim_autostart_boot_task, "sim_autostart", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sim autostart boot task");
    }
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
    sim_autostart_add_status_fields(status);

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
        web_sim_stop_internal(false);
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
        sim_autostart_clear_failures();
    }
    web_sim_broadcast_status();
}


static void web_sim_stop_internal(bool user_initiated) {
    char stopped_device[sizeof(current_simulated_device)] = "";

    xSemaphoreTake(simulation_mutex, portMAX_DELAY);
    if (strlen(current_simulated_device) > 0) {
        strncpy(stopped_device, current_simulated_device, sizeof(stopped_device) - 1);
        stopped_device[sizeof(stopped_device) - 1] = '\0';
    }
    current_simulated_device[0] = '\0';
    xSemaphoreGive(simulation_mutex);

    unload_ble_device_for_simulation();

    if (user_initiated) {
        sim_autostart_disable_if_matches(stopped_device);
    }

    ESP_LOGI(TAG, "Stopped simulation");
    web_sim_broadcast_status();
}

void web_sim_stop(void) {
    web_sim_stop_internal(true);
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
    else if (action && strcmp(action, "autostart") == 0) {
        cJSON *en = cJSON_GetObjectItemCaseSensitive(json, "enabled");
        if (!cJSON_IsBool(en)) {
            WS_LOGW(TAG, "sim autostart: enabled (bool) required");
            return true;
        }
        if (cJSON_IsTrue(en)) {
            cJSON *dev = cJSON_GetObjectItemCaseSensitive(json, "device");
            if (cJSON_IsString(dev) && dev->valuestring[0]) {
                sim_autostart_set(true, dev->valuestring);
            } else {
                WS_LOGW(TAG, "sim autostart enable requires device");
            }
        } else {
            sim_autostart_set(false, NULL);
        }
    }
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
