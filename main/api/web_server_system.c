#include <stdbool.h>

#include "cJSON.h"

#include "esp_err.h"

#include "esp_heap_caps.h"

#include "esp_app_desc.h"

#include "esp_log.h"

#include "api/web_server.h"

#include "api/web_server_internal.h"



static const char *TAG = "web server system";



static void broadcast_memory_status(const char *label) {

    uint32_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    uint32_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    uint32_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);



    ESP_LOGI("WEBSERVER", "=== Memory %s - PSRAM: %" PRIu32, label, psram);

    ESP_LOGI("WEBSERVER", "INTERNAL: %" PRIu32 ", largest: %" PRIu32 ", min: %" PRIu32, internal, largest, min_free);



    cJSON *mem = cJSON_CreateObject();

    cJSON_AddStringToObject(mem, "type", "memory_status");

    cJSON_AddStringToObject(mem, "label", label);

    cJSON_AddNumberToObject(mem, "psram_free_bytes", psram);

    cJSON_AddNumberToObject(mem, "internal_free_bytes", internal);

    cJSON_AddNumberToObject(mem, "largest_free_block", largest);

    cJSON_AddNumberToObject(mem, "min_free_ever", min_free);



    char *jsonstr = cJSON_PrintUnformatted(mem);

    websocket_broadcast_json_transient(jsonstr);

    free(jsonstr);

    cJSON_Delete(mem);

}



static void broadcast_version(void) {

    const esp_app_desc_t *desc = esp_app_get_description();



    cJSON *obj = cJSON_CreateObject();

    cJSON_AddStringToObject(obj, "type",         "version");

    cJSON_AddStringToObject(obj, "version",      desc->version);

    cJSON_AddStringToObject(obj, "idf_version",  desc->idf_ver);

    char build_datetime[64];

    snprintf(build_datetime, sizeof(build_datetime), "%s %s", desc->date, desc->time);

    cJSON_AddStringToObject(obj, "build_datetime", build_datetime);



    char *jsonstr = cJSON_PrintUnformatted(obj);

    websocket_broadcast_json_transient(jsonstr);

    free(jsonstr);

    cJSON_Delete(obj);

}



static bool system_ws_handler(const char *type, cJSON *json) {

    if (strcmp(type, "system") != 0) return false;



    cJSON *act = cJSON_GetObjectItemCaseSensitive(json, "action");

    const char *action = cJSON_IsString(act) ? act->valuestring : NULL;



    if (!action) {

        ESP_LOGW("WEBSERVER", "system: missing action");

        return true;

    }



    if ( (strcmp(action, "reboot") == 0) || (strcmp(action, "restart") == 0))  {

        ESP_LOGW("WEBSERVER", "Reboot requested via WS");

        broadcast_memory_status("before reboot");

        vTaskDelay(pdMS_TO_TICKS(500));

        esp_restart();



    } else if (strcmp(action, "memory") == 0) {

        broadcast_memory_status("requested");



    } else if (strcmp(action, "version") == 0) {

        broadcast_version();



    } else {

        ESP_LOGW("WEBSERVER", "system: unknown action %s", action);

    }

    return true;

}



uint8_t register_system_handlers_in_web_server(httpd_handle_t *server) {

    (void)server;

    ws_register_message_handler(system_ws_handler);

    return 0;

}


