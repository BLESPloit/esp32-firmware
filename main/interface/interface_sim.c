#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_log.h"
#include "common/utils.h"

#include "ble/device_parser.h"
#include "lua/lua_hook.h"
#include "interface/interface_sim.h"

static const char *TAG = "interface";

static interface_manager_t *g_interface_manager;

// Initialize (call once at startup)
bool interface_init(const char *filepath) {
    if (g_interface_manager != NULL) {
        ESP_LOGW(TAG, "Interface already initialized");
        return false;
    }
    
    g_interface_manager = parse_interface_json_file(filepath);
    return (g_interface_manager != NULL);
}


// Parse buttons from interface.json file
interface_manager_t* parse_interface_json_file(const char *filepath) {
    // Read JSON file
    size_t json_size;
    char *json_string = read_json_file(filepath, &json_size);
    if (json_string == NULL) {
        ESP_LOGE(TAG, "Failed to read interface file: %s", filepath);
        return NULL;
    }

    // Parse JSON
    cJSON *root = cJSON_Parse(json_string);
    free(json_string);

    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            ESP_LOGE(TAG, "Interface JSON parse error: %s", error_ptr);
        }
        return NULL;
    }

    // Allocate manager structure
    interface_manager_t *manager = calloc(1, sizeof(interface_manager_t));
    if (manager == NULL) {
        ESP_LOGE(TAG, "Failed to allocate interface manager");
        cJSON_Delete(root);
        return NULL;
    }

    // Get buttons array
    cJSON *buttons_json = cJSON_GetObjectItem(root, "buttons");
    if (!cJSON_IsArray(buttons_json)) {
        ESP_LOGE(TAG, "No 'buttons' array found in JSON");
        free(manager);
        cJSON_Delete(root);
        return NULL;
    }

    int button_count = cJSON_GetArraySize(buttons_json);
    if (button_count == 0) {
        ESP_LOGI(TAG, "Buttons array is empty");
        manager->button_count = 0;
        manager->buttons = NULL;
        cJSON_Delete(root);
        return manager;
    }

    // Allocate buttons array
    manager->buttons = calloc(button_count, sizeof(interface_button_t));
    if (manager->buttons == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buttons array");
        free(manager);
        cJSON_Delete(root);
        return NULL;
    }

    // Parse each button
    int valid_count = 0;
    cJSON *btn_json = NULL;
    cJSON_ArrayForEach(btn_json, buttons_json) {
        interface_button_t *btn = &manager->buttons[valid_count];

        // Parse id (required)
        cJSON *id = cJSON_GetObjectItem(btn_json, "id");
        if (!cJSON_IsString(id) || id->valuestring == NULL) {
            ESP_LOGW(TAG, "Button missing id, skipping");
            continue;
        }
        strncpy(btn->id, id->valuestring, sizeof(btn->id) - 1);

        // Parse name (required)
        cJSON *name = cJSON_GetObjectItem(btn_json, "name");
        if (!cJSON_IsString(name) || name->valuestring == NULL) {
            ESP_LOGW(TAG, "Button '%s' missing name, skipping", btn->id);
            continue;
        }
        strncpy(btn->name, name->valuestring, sizeof(btn->name) - 1);

        // Parse lua callback (required)
        cJSON *lua = cJSON_GetObjectItem(btn_json, "lua");
        if (!cJSON_IsString(lua) || lua->valuestring == NULL) {
            ESP_LOGW(TAG, "Button '%s' missing lua callback, skipping", btn->id);
            continue;
        }
        strncpy(btn->lua, lua->valuestring, sizeof(btn->lua) - 1);

        // Parse physical_button_id (optional)
        cJSON *phys_id = cJSON_GetObjectItem(btn_json, "physical_button_id");
        if (cJSON_IsNumber(phys_id)) {
            btn->physical_button_id = phys_id->valueint;
        } else {
            btn->physical_button_id = -1;
        }

        // Parse physical_button_press (optional)
        cJSON *phys_press = cJSON_GetObjectItem(btn_json, "physical_button_press");
        if (cJSON_IsString(phys_press) && phys_press->valuestring != NULL) {
            strncpy(btn->physical_button_press, phys_press->valuestring, 
                   sizeof(btn->physical_button_press) - 1);
        } else {
            btn->physical_button_press[0] = '\0';
        }

        ESP_LOGI(TAG, "Loaded Interface Button: '%s' (%s), lua='%s', phys_id=%d, press='%s'", 
                 btn->id, btn->name, btn->lua, 
                 btn->physical_button_id, btn->physical_button_press);
        valid_count++;
    }

    manager->button_count = valid_count;
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Interface manager initialized with %d buttons", valid_count);
    return manager;
}

// Lookup by id
interface_button_t* interface_find_by_id(const char *id) {
    if (g_interface_manager == NULL || id == NULL) {
        return NULL;
    }
    
    for (int i = 0; i < g_interface_manager->button_count; i++) {
        if (strcmp(g_interface_manager->buttons[i].id, id) == 0) {
            return &g_interface_manager->buttons[i];
        }
    }
    
    ESP_LOGW(TAG, "Interface button '%s' not found", id);
    return NULL;
}

interface_button_t* interface_find_by_physical(int physical_id, const char *press_type) {
    if (g_interface_manager == NULL || press_type == NULL) {
        return NULL;
    }
    
    for (int i = 0; i < g_interface_manager->button_count; i++) {
        interface_button_t *btn = &g_interface_manager->buttons[i];
        if (btn->physical_button_id >= 0 && 
            btn->physical_button_id == physical_id &&
            strcmp(btn->physical_button_press, press_type) == 0) {
            return btn;
        }
    }
    
    return NULL;
}

// Free interface manager
void interface_manager_free(interface_manager_t *manager) {
    if (manager == NULL) {
        return;
    }

    if (manager->buttons != NULL) {
        free(manager->buttons);
    }

    free(manager);
    ESP_LOGI(TAG, "Interface manager freed");
}

void interface_cleanup(void) {
    if (g_interface_manager != NULL) {
        interface_manager_free(g_interface_manager);
        g_interface_manager = NULL;
    }
}

// Print all interface buttons (debugging)
void interface_print_all(interface_manager_t *manager) {
    if (manager == NULL) {
        ESP_LOGI(TAG, "Interface manager is NULL");
        return;
    }

    ESP_LOGI(TAG, "Interface Manager: %d buttons loaded", manager->button_count);

    for (int i = 0; i < manager->button_count; i++) {
        interface_button_t *btn = &manager->buttons[i];

        if (btn->physical_button_id >= 0) {
            ESP_LOGI(TAG, "  [%d] '%s': name='%s', lua='%s', phys_id=%d, press='%s'",
                     i, btn->id, btn->name, btn->lua, 
                     btn->physical_button_id, btn->physical_button_press);
        } else {
            ESP_LOGI(TAG, "  [%d] '%s': name='%s', lua='%s' (no physical button)",
                     i, btn->id, btn->name, btn->lua);
        }
    }
}

esp_err_t interface_call_lua_by_button_t(interface_button_t *interface_button) {
    // Call Lua function
    esp_err_t rc = lua_call_from_field(interface_button->lua);

    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "LUA function %s called successfully", interface_button->lua);
    } else {
        ESP_LOGW(TAG, "Lua function %s call failed", interface_button->lua);
    }
    return rc;
}


void interface_call_lua(const char* element_id) {
    interface_button_t* elem = interface_find_by_id(element_id);
    if (!elem) {
        ESP_LOGE(TAG, "Interface element %s not found", element_id);
        return;   
    }
    interface_call_lua_by_button_t(elem);
}

void interface_handle_physical_button(uint8_t physical_button_id, const char *press_type) {
    ESP_LOGI(TAG, "Physical button %d %s", physical_button_id, press_type);
    interface_button_t *interface_button = interface_find_by_physical(physical_button_id, press_type);
    if (interface_button == NULL) {
        ESP_LOGW(TAG, "Button id %d %s press not configured in interface!", physical_button_id, press_type);
        return;
    }
    interface_call_lua_by_button_t(interface_button);
}
