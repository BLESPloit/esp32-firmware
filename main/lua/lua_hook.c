#include <stdio.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include "graphics/graphics.h"
#include "interface/interface_central.h"
#include "ble/device_parser.h"
#include "common/storage.h"
#include "common/utils.h"
#include "lua/lua_ble_bridge.h"
#include "lua/lua_crypto.h"
#include "lua/lua_gfx.h"
#include "lua/lua_hook.h"

#define TAG "LUA"

// LUA state (preserve between calls)
static lua_State *g_lua_state = NULL;

// queue
static QueueHandle_t lua_event_queue = NULL;
static TaskHandle_t lua_task_handle = NULL;


bool lua_is_task_running(void) {
    return (lua_task_handle != NULL &&
            lua_event_queue != NULL &&
            g_lua_state     != NULL);
}

// optional persistent variables (vars.json)
static char g_vars_path[128] = "";


// ── HELPERS ────────────────────────────────────────────────── 

lua_State *lua_get_state(void) {
    return g_lua_state;
}

/**
 * Convert binary data to hex string
 * Lua: hex_string = bin_to_hex(binary_data)
 */
 static int lua_bin_to_hex(lua_State* L) {
    size_t len;
    const unsigned char *data = (const unsigned char *)luaL_checklstring(L, 1, &len);
    
    char *hex = malloc(len * 2 + 1);
    if (!hex) {
        return luaL_error(L, "Memory allocation failed");
    }
    
    for (size_t i = 0; i < len; i++) {
        sprintf(hex + i * 2, "%02X", data[i]);
    }
    hex[len * 2] = '\0';
    
    lua_pushstring(L, hex);
    free(hex);
    
    return 1;
}

/**
 * Convert hex string to binary data
 * Lua: binary_data = hex_to_bin(hex_string)
 */
static int lua_hex_to_bin(lua_State* L) {
    const char *hex = luaL_checkstring(L, 1);
    size_t hex_len = strlen(hex);
    
    if (hex_len % 2 != 0) {
        return luaL_error(L, "Hex string must have even length");
    }
    
    size_t bin_len = hex_len / 2;
    unsigned char *bin = malloc(bin_len);
    if (!bin) {
        return luaL_error(L, "Memory allocation failed");
    }
    
    for (size_t i = 0; i < bin_len; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
            free(bin);
            return luaL_error(L, "Invalid hex character at position %d", i * 2);
        }
        bin[i] = (unsigned char)byte;
    }
    
    lua_pushlstring(L, (const char *)bin, bin_len);
    free(bin);
    
    return 1;
}

/**
 * Get current Unix timestamp
 * Lua: timestamp = get_time()
 */
static int lua_get_time(lua_State* L) {
    time_t now = time(NULL);
    lua_pushinteger(L, (lua_Integer)now);
    return 1;
}


// ── Delayed execution ────────────────────────────────────────────────── 

// Timer callback — fires from esp_timer task (NOT lua_task)
// Must not touch lua_State; only posts to the queue
static void lua_delay_timer_cb(void *arg) {
    lua_event_t event = {0};
    event.type = LUA_EVENT_DELAYED_CALL;
    // arg is a heap-allocated function name string
    strncpy(event.func_name, (const char *)arg, sizeof(event.func_name) - 1);
    free(arg);  // safe to free here, funcname was copied

    if (xQueueSend(lua_event_queue, &event, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "Lua delay queue full, dropping callback: %s", event.func_name);
    }
}

// Lua: delay(seconds, "callback_func_name")
static int lua_delay(lua_State *L) {
    lua_Number seconds = luaL_checknumber(L, 1);
    const char *func_name = luaL_checkstring(L, 2);

    // Heap-allocate funcname so the timer callback can safely use it
    char *name_copy = malloc(strlen(func_name) + 1);
    if (!name_copy) return luaL_error(L, "OOM in delay()");
    strcpy(name_copy, func_name);

    esp_timer_handle_t timer;
    esp_timer_create_args_t args = {
        .callback = lua_delay_timer_cb,
        .arg      = name_copy,
        .dispatch_method = ESP_TIMER_TASK,
        .name     = "lua_delay",
    };

    if (esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_once(timer, (uint64_t)(seconds * 1e6)) != ESP_OK) {
        free(name_copy);
        return luaL_error(L, "Failed to start delay timer");
    }

    // Timer auto-deletes is NOT supported by default — store handle if you
    // need to cancel, or just let it fire and leak the handle (acceptable for
    // one-shot fire-and-forget on ESP32 for small counts)
    return 0;
}

// ── Menu functions (interface central) ────────────────────────────────────────────────── 

// interface_push_menu(id)
static int lua_interface_push_menu(lua_State *L) 
{
    const char *id = luaL_checkstring(L, 1);
    interface_central_push_menu(id);
    return 0;
}

// interface_pop_menu()
static int lua_interface_pop_menu(lua_State *L) 
{
    (void)L;
    interface_central_pop_menu();
    return 0;
}

// interface_set_title(text)
static int lua_interface_set_title(lua_State *L) 
{
    const char *title = luaL_checkstring(L, 1);
    interface_central_set_title(title);
    return 0;
}

static int lua_interface_set_state(lua_State *L) 
{
    const char *key = luaL_checkstring(L, 1);
    const char *value = luaL_checkstring(L, 2);
    interface_central_state_set(key, value);
    return 0;
}

// ── Lua Task (executes all Lua code safely) ────────────────────────────────────────────────── 

static void lua_task(void* arg) {
    lua_event_t event;
    
    ESP_LOGI(TAG, "Lua execution task started");
    
    while (1) {
        if (xQueueReceive(lua_event_queue, &event, portMAX_DELAY)) {
            
            if (!g_lua_state) {
                ESP_LOGE(TAG, "Lua state not initialized");
                if (event.result) *event.result = ESP_FAIL;
                if (event.done_sem) xSemaphoreGive(event.done_sem);
                continue;
            }
            
            switch (event.type) {
                case LUA_EVENT_CALL_HANDLER: {
                    ESP_LOGD(TAG, "Calling handler: %s", event.func_name);
                    
                    lua_getglobal(g_lua_state, event.func_name);
                    
                    if (lua_isfunction(g_lua_state, -1)) {

                        // for BLE notify handle 3 params: "SVC_UUID|CHR_UUID|HEXDATA"
                        if (strcmp(event.func_name, "on_notify") == 0 && event.data_len > 0) {
                            char *param = (char *)event.data;
                            char *p1 = strchr(param, '|');
                            char *p2 = p1 ? strchr(p1 + 1, '|') : NULL;

                            if (p1 && p2) {
                                lua_pushlstring(g_lua_state, param, p1 - param);          // svc_uuid
                                lua_pushlstring(g_lua_state, p1 + 1, p2 - (p1 + 1));     // chr_uuid
                                lua_pushstring(g_lua_state, p2 + 1);                      // hex_data
                                lua_pcall(g_lua_state, 3, 0, 0);
                            } else {
                                lua_pop(g_lua_state, 1);  // malformed, discard
                            }
                        } 
                        // all the others - single param
                        else {
                            if (event.data_len > 0) {
                                lua_pushstring(g_lua_state, (const char*)event.data);
                                
                                if (lua_pcall(g_lua_state, 1, 0, 0) != LUA_OK) {
                                    const char* err = lua_tostring(g_lua_state, -1);
                                    ESP_LOGE(TAG, "Lua error in %s: %s", event.func_name, err);
                                    lua_pop(g_lua_state, 1);
                                    if (event.result) *event.result = ESP_FAIL;
                                } else {
                                    if (event.result) *event.result = ESP_OK;
                                }
                            } else {
                                if (lua_pcall(g_lua_state, 0, 0, 0) != LUA_OK) {
                                    const char* err = lua_tostring(g_lua_state, -1);
                                    ESP_LOGE(TAG, "Lua error in %s: %s", event.func_name, err);
                                    lua_pop(g_lua_state, 1);
                                    if (event.result) *event.result = ESP_FAIL;
                                } else {
                                    if (event.result) *event.result = ESP_OK;
                                }
                            }
                        }
                    } else {
                        ESP_LOGW(TAG, "Function %s not found", event.func_name);
                        lua_pop(g_lua_state, 1);
                        if (event.result) *event.result = ESP_FAIL;
                    }
                    
                    if (event.done_sem) xSemaphoreGive(event.done_sem);
                    break;
                }
                
                case LUA_EVENT_CALL_FUNCTION: {
                    lua_getglobal(g_lua_state, event.func_name);
                    
                    if (!lua_isfunction(g_lua_state, -1)) {
                        ESP_LOGE(TAG, "Function '%s' not found", event.func_name);
                        lua_pop(g_lua_state, 1);
                        if (event.result) *event.result = ESP_FAIL;
                        if (event.done_sem) xSemaphoreGive(event.done_sem);
                        break;
                    }
                    
                    lua_pushlstring(g_lua_state, (const char*)event.data, event.data_len);
                    
                    if (lua_pcall(g_lua_state, 1, 1, 0) != LUA_OK) {
                        ESP_LOGE(TAG, "Call error: %s", lua_tostring(g_lua_state, -1));
                        lua_pop(g_lua_state, 1);
                        if (event.result) *event.result = ESP_FAIL;
                    } else {
                        size_t len;
                        const uint8_t *result = (const uint8_t*)lua_tolstring(g_lua_state, -1, &len);
                        
                        if (result && len <= event.max_len && event.output) {
                            memcpy(event.output, result, len);
                            if (event.output_len) *event.output_len = len;
                            if (event.result) *event.result = ESP_OK;
                        } else {
                            ESP_LOGW(TAG, "Output buffer too small or invalid");
                            if (event.result) *event.result = ESP_FAIL;
                        }
                        
                        lua_pop(g_lua_state, 1);
                    }
                    
                    if (event.done_sem) xSemaphoreGive(event.done_sem);
                    break;
                }
                case LUA_EVENT_DELAYED_CALL: {
                    lua_getglobal(g_lua_state, event.func_name);
                    if (lua_isfunction(g_lua_state, -1)) {
                        if (lua_pcall(g_lua_state, 0, 0, 0) != LUA_OK) {
                            ESP_LOGE(TAG, "Lua delay callback error in '%s': %s",
                                    event.func_name,
                                    lua_tostring(g_lua_state, -1));
                            lua_pop(g_lua_state, 1);
                        }
                    } else {
                        ESP_LOGW(TAG, "Delay callback '%s' not found", event.func_name);
                        lua_pop(g_lua_state, 1);
                    }
                    break;
                }

                case LUA_EVENT_SHUTDOWN: {
                    ESP_LOGI(TAG, "Lua shutdown requested");
                    if (g_lua_state) {
                        lua_close(g_lua_state);
                        g_lua_state = NULL;
                    }
                    if (event.done_sem) {
                        xSemaphoreGive(event.done_sem);
                    }
                    vTaskDelete(NULL);
                    break;
                }
            }
        }
    }
}

// ── Persistent variables ────────────────────────────────────────────────── 

void lua_vars_inject(const char *vars_path) {
    lua_State *L = lua_get_state();
    if (!L) return;

    lua_newtable(L);

    if (!vars_path || vars_path[0] == '\0') {
        lua_setglobal(L, "vars");
        return;
    }

    size_t sz;
    char *buf = read_json_file(vars_path, &sz);
    if (!buf) {
        ESP_LOGW(TAG, "vars json not found: %s", vars_path);
        lua_setglobal(L, "vars");
        return;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "vars json parse error");
        lua_setglobal(L, "vars");
        return;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (!item->string) continue;
        if      (cJSON_IsString(item))  lua_pushstring(L, item->valuestring);
        else if (cJSON_IsNumber(item))  lua_pushnumber(L, item->valuedouble);
        else if (cJSON_IsBool(item))    lua_pushboolean(L, cJSON_IsTrue(item));
        else continue;
        lua_setfield(L, -2, item->string);
    }
    lua_setglobal(L, "vars");
    cJSON_Delete(root);

    ESP_LOGI(TAG, "vars table injected from %s", vars_path);
}


// set the g_vars_path global
void lua_vars_set_path(const char *path) {
    if (path && path[0] != '\0') {
        strlcpy(g_vars_path, path, sizeof(g_vars_path));
    } else {
        g_vars_path[0] = '\0';
    }
}

// vars_save() — serializes the current vars global back to vars.json
static int lua_vars_save(lua_State *L) {
    lua_getglobal(L, "vars");
    if (!lua_istable(L, -1)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    cJSON *root = cJSON_CreateObject();
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        const char *key = lua_tostring(L, -2);
        if (lua_isstring(L, -1))       cJSON_AddStringToObject(root, key, lua_tostring(L, -1));
        else if (lua_isnumber(L, -1))  cJSON_AddNumberToObject(root, key, lua_tonumber(L, -1));
        else if (lua_isboolean(L, -1)) cJSON_AddBoolToObject(root, key, lua_toboolean(L, -1));
        lua_pop(L, 1);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    FILE *f = fopen(g_vars_path, "w");
    if (f) { fputs(json_str, f); fclose(f); }
    cJSON_free(json_str);

    lua_pushboolean(L, f != NULL);
    return 1;
}


void lua_uuids_inject(const char *path) {
    lua_State *L = lua_get_state();
    if (!L) return;

    lua_newtable(L);

    if (!path || !path[0]) {
        lua_setglobal(L, "uuids");
        return;
    }

    size_t filesize;
    char *json_buf = read_json_file(path, &filesize);
    if (!json_buf) {
        ESP_LOGW(TAG, "lua_uuids_inject: cannot read %s", path);
        lua_setglobal(L, "uuids");
        return;
    }

    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);
    if (!root) {
        ESP_LOGE(TAG, "lua_uuids_inject: JSON parse error");
        lua_setglobal(L, "uuids");
        return;
    }

    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, root) {
        if (!entry->string) continue;                      // key = symbol name
        cJSON *uuid_val = cJSON_GetObjectItem(entry, "uuid");
        if (!cJSON_IsString(uuid_val)) continue;
        lua_pushstring(L, uuid_val->valuestring);
        lua_setfield(L, -2, entry->string);                // uuids[key] = uuid
    }

    lua_setglobal(L, "uuids");
    cJSON_Delete(root);
    ESP_LOGI(TAG, "lua_uuids_inject: loaded %s", path);
}



// ── Initialization ────────────────────────────────────────────────── 

esp_err_t lua_init_persistent_minimal(const char *script_path, bool central,
                                      const char *vars_path, const char *uuids_path) {
    // Initialize cryptographic subsystem
    if (crypto_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize crypto");
        return ESP_FAIL;
    }
    
    // Create Lua state
    g_lua_state = luaL_newstate();
    if (!g_lua_state) {
        ESP_LOGE(TAG, "Failed to create Lua state");
        return ESP_FAIL;
    }
    
    // Minimal standard libraries
    luaL_requiref(g_lua_state, "_G", luaopen_base, 1);
    luaL_requiref(g_lua_state, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(g_lua_state, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(g_lua_state, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(g_lua_state, 3);
    
    
    if (central == true) {
        // central menu
        lua_register(g_lua_state, "push_menu", lua_interface_push_menu);
        lua_register(g_lua_state, "pop_menu",  lua_interface_pop_menu);
        lua_register(g_lua_state, "set_title", lua_interface_set_title);
        lua_register(g_lua_state, "set_state", lua_interface_set_state);
        lua_register(g_lua_state, "gfx_print_notification", lua_gfx_print_notification);

        // BLE central bridge functions
        lua_ble_central_register_functions(g_lua_state);

        lua_register(g_lua_state, "delay", lua_delay);
    }
    else {
        // Register graphics functions for sim mode
        lua_register(g_lua_state, "gfx_show", lua_gfx_show);
        lua_register(g_lua_state, "gfx_set_color", lua_gfx_set_element_color );
        lua_register(g_lua_state, "gfx_set_position", lua_gfx_set_element_position);
        lua_register(g_lua_state, "gfx_remove", lua_gfx_remove_element);
        lua_register(g_lua_state, "gfx_set_background", lua_gfx_set_background);
        lua_register(g_lua_state, "gfx_print_notification", lua_gfx_print_notification);
        lua_register(g_lua_state, "gfx_render_text",  lua_gfx_render_text);
        lua_register(g_lua_state, "gfx_update_text",  lua_gfx_update_text);

        // BLE peripheral bridge functions
        lua_ble_peripheral_register_functions(g_lua_state);

        lua_register(g_lua_state, "delay", lua_delay);
    }
  
    // Register cryptographic functions
    lua_crypto_register_functions(g_lua_state);

    // Register utility functions
    lua_register(g_lua_state, "bin_to_hex", lua_bin_to_hex);
    lua_register(g_lua_state, "hex_to_bin", lua_hex_to_bin);
    lua_register(g_lua_state, "get_time", lua_get_time);

    lua_register(g_lua_state, "vars_save", lua_vars_save);

    // Inject manifest globals before script top-level code runs.
    lua_vars_set_path(vars_path ? vars_path : "");
    lua_vars_inject(vars_path);
    lua_uuids_inject(uuids_path);

    // Load and execute script
    if (luaL_loadfile(g_lua_state, script_path) != LUA_OK ||
        lua_pcall(g_lua_state, 0, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Lua init failed: %s", lua_tostring(g_lua_state, -1));
        lua_close(g_lua_state);
        g_lua_state = NULL;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Lua script %s loaded", script_path);
    
    // Create event queue
    lua_event_queue = xQueueCreate(LUA_QUEUE_SIZE, sizeof(lua_event_t));
    if (!lua_event_queue) {
        ESP_LOGE(TAG, "Failed to create Lua event queue");
        lua_close(g_lua_state);
        g_lua_state = NULL;
        return ESP_FAIL;
    }

    log_memory_usage("Before LUA task");
    
    // Create Lua execution task
    BaseType_t ret = xTaskCreate(
        lua_task,
        "lua_task",
        LUA_TASK_STACK_SIZE,
        NULL,
        LUA_TASK_PRIORITY,
        &lua_task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Lua task");
        vQueueDelete(lua_event_queue);
        lua_close(g_lua_state);
        g_lua_state = NULL;
        return ESP_FAIL;
    }
    
    log_memory_usage("After LUA task");
    
    ESP_LOGI(TAG, "Lua task queue initialized (stack: %d bytes)", LUA_TASK_STACK_SIZE);
//    ESP_LOGI(TAG, "Cryptographic functions: AES, SHA-256, ECDH secp256r1");
    return ESP_OK;
}

// ── Public API - Asynchronous ────────────────────────────────────────────────── 

esp_err_t lua_call_handler_async(const char *func_name, const char *param) {
    if (!lua_event_queue) {
        ESP_LOGW(TAG, "Lua queue not initialized");
        return ESP_FAIL;
    }
    
    lua_event_t event = {0};
    event.type = LUA_EVENT_CALL_HANDLER;
    strncpy(event.func_name, func_name, sizeof(event.func_name) - 1);
    
    if (param) {
        size_t param_len = strlen(param);
        if (param_len < sizeof(event.data)) {
            memcpy(event.data, param, param_len + 1);
            event.data_len = param_len;
        } else {
            ESP_LOGW(TAG, "Parameter too long for %s", func_name);
            return ESP_FAIL;
        }
    } else {
        event.data_len = 0;
    }
    
    if (xQueueSend(lua_event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Lua queue full, dropping: %s", func_name);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// ── Public API - Synchronous ────────────────────────────────────────────────── 

esp_err_t lua_call_stateful(const char *func_name, 
                            const uint8_t *input, 
                            size_t input_len,
                            uint8_t *output, 
                            size_t *output_len, 
                            size_t max_len) {
    if (!lua_event_queue) {
        ESP_LOGE(TAG, "Lua queue not initialized");
        return ESP_FAIL;
    }
    
    if (input_len > sizeof(((lua_event_t*)0)->data)) {
        ESP_LOGE(TAG, "Input data too large (%zu > %zu)", input_len, sizeof(((lua_event_t*)0)->data));
        return ESP_FAIL;
    }
    
    SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
    if (!done_sem) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return ESP_FAIL;
    }
    
    lua_event_t event = {0};
    event.type = LUA_EVENT_CALL_FUNCTION;
    strncpy(event.func_name, func_name, sizeof(event.func_name) - 1);
    memcpy(event.data, input, input_len);
    event.data_len = input_len;
    event.output = output;
    event.output_len = output_len;
    event.max_len = max_len;
    event.done_sem = done_sem;
    
    esp_err_t result = ESP_FAIL;
    event.result = &result;
    
    if (xQueueSend(lua_event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Lua queue full");
        vSemaphoreDelete(done_sem);
        return ESP_FAIL;
    }
    
    if (xSemaphoreTake(done_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Lua call timeout for: %s", func_name);
        vSemaphoreDelete(done_sem);
        return ESP_FAIL;
    }
    
    vSemaphoreDelete(done_sem);
    return result;
}


// ── Function optional arguments parse / split ────────────────────────────────────────────────── 

bool lua_call_parse(const char *src, char *buf, size_t buf_size, lua_call_t *out)
{
    if (!src || !src[0] || !buf || !out) return false;

    size_t src_len = strlen(src);
    if (src_len + 1 > buf_size) {
        ESP_LOGW(TAG, "buf too small for '%s'", src);
        return false;
    }

    memcpy(buf, src, src_len + 1);

    char *paren = strchr(buf, '(');
    if (!paren) {
        out->func = buf;
        out->args = NULL;
        return true;
    }

    char *close = strrchr(paren + 1, ')');
    if (!close) {
        ESP_LOGW(TAG, "missing ')' in lua field: '%s'", src);
        return false;
    }

    *paren = '\0';
    *close = '\0';

    out->func = buf;
    out->args = (close > paren + 1) ? paren + 1 : NULL;
    return true;
}

esp_err_t lua_call_from_field(const char *src)
{
    if (!src || !src[0]) return ESP_FAIL;

    size_t buf_size = strlen(src) + 1;
    char buf[buf_size];   // C99 VLA, stack only

    lua_call_t call;
    if (!lua_call_parse(src, buf, buf_size, &call)) {
        ESP_LOGE(TAG, "Failed to parse lua field: '%s'", src);
        return ESP_FAIL;
    }
    return lua_call_handler_async(call.func, call.args);
}

// ── Cleanup ────────────────────────────────────────────────── 

void lua_cleanup(void) {
    if (lua_task_handle && lua_event_queue) {
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        if (done) {
            lua_event_t event = {0};
            event.type = LUA_EVENT_SHUTDOWN;
            event.done_sem = done;
            if (xQueueSend(lua_event_queue, &event, portMAX_DELAY) == pdTRUE) {
                xSemaphoreTake(done, portMAX_DELAY);
            } else {
                ESP_LOGW(TAG, "Lua shutdown queue send failed, forcing task delete");
                vTaskDelete(lua_task_handle);
                if (g_lua_state) {
                    lua_close(g_lua_state);
                    g_lua_state = NULL;
                }
            }
            vSemaphoreDelete(done);
        }
        lua_task_handle = NULL;
    } else {
        if (lua_task_handle) {
            vTaskDelete(lua_task_handle);
            lua_task_handle = NULL;
        }
        if (g_lua_state) {
            lua_close(g_lua_state);
            g_lua_state = NULL;
        }
    }

    if (lua_event_queue) {
        vQueueDelete(lua_event_queue);
        lua_event_queue = NULL;
    }

    crypto_deinit();

    ESP_LOGI(TAG, "Lua cleanup complete");
}

void lua_print_stack_usage(void) {
    if (lua_task_handle) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(lua_task_handle);
        ESP_LOGI(TAG, "Lua task stack high-water mark: %u bytes free", hwm * sizeof(StackType_t));
    }
}



