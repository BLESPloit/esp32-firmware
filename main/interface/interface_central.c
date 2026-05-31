#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "esp_log.h"

#include "ble/device_parser.h"          // read_json_file()
#include "lua/lua_hook.h"               // lua_call_handler_async(), lua_register_c_function()
#include "api/web_server.h" 
#include "graphics/graphics.h"                
#include "interface/interface_central.h"

static const char *TAG = "interface_central";


static central_menu_def_t   *g_def   = NULL;
static central_menu_state_t  g_state = {0};
static SemaphoreHandle_t     g_mutex = NULL;


// states
typedef struct {
    char key[CENTRAL_STATE_KEY_LEN]; 
    char value[CENTRAL_STATE_VAL_LEN];  // ""       — empty until Lua sets it
    bool registered;
    bool initialized;              // false until Lua calls ui_state_set()
} interface_central_state_entry_t;

static interface_central_state_entry_t state_entries[CENTRAL_STATE_MAX_ENTRIES];
static void interface_central_state_register(const char *key);

// Point current view at a static node's items (no copy, no alloc)
static void current_point_to_node(central_menu_node_t *node) {
    strncpy(g_state.current_title, node->title, CENTRAL_MENU_LABEL_LEN - 1);
    g_state.current_title[CENTRAL_MENU_LABEL_LEN - 1] = '\0';
    g_state.current_items      = node->items;
    g_state.current_item_count = node->item_count;
    g_state.selected_index     = 0;
}

// ── Navigation stack ────────────────────────────────────────────────── 

static bool stack_push(const char *id) {
    if (g_state.stack_depth >= CENTRAL_MENU_STACK_DEPTH) {
        ESP_LOGW(TAG, "Navigation stack full, cannot push '%s'", id);
        return false;
    }
    strncpy(g_state.stack[g_state.stack_depth], id, CENTRAL_MENU_ID_LEN - 1);
    g_state.stack[g_state.stack_depth][CENTRAL_MENU_ID_LEN - 1] = '\0';
    g_state.stack_depth++;
    return true;
}

// Returns id of the level popped back TO, or NULL if already at root
static const char *stack_pop(void) {
    if (g_state.stack_depth <= 1) {
        ESP_LOGI(TAG, "Already at root");
        return NULL;
    }
    g_state.stack_depth--;
    return g_state.stack[g_state.stack_depth - 1];
}

static const char *stack_top(void) {
    if (g_state.stack_depth == 0) return NULL;
    return g_state.stack[g_state.stack_depth - 1];
}

static central_menu_node_t *find_node(const char *id) {
    if (!g_def || !id) return NULL;
    for (int i = 0; i < g_def->node_count; i++) {
        if (strcmp(g_def->nodes[i].id, id) == 0)
            return &g_def->nodes[i];
    }
    ESP_LOGW(TAG, "Static menu node '%s' not found", id);
    return NULL;
}

// ── Render — push current state ────────────────────────────────────── 
// Must be called with g_mutex held.
static void render_current_menu(void) {
    ESP_LOGI(TAG, "Render menu: '%s' %d items  sel=%d",
             g_state.current_title,
             g_state.current_item_count,
             g_state.selected_index);

    if (!g_def || g_state.stack_depth == 0) return;

    // Build combined title for UI, if needed
    char title[CENTRAL_MENU_LABEL_LEN];
    strncpy(title, g_state.current_title, sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';

    // Send minimal JSON (you can extend with 'title' if UI needs it)
    char json[128];
    snprintf(json, sizeof(json),
             "{\"type\":\"menu\",\"id\":\"%s\",\"title\":\"%s\"}",
             g_state.stack[g_state.stack_depth - 1], title);

    websocket_broadcast_json(json);
}

// ── Json parsing ────────────────────────────────────────────────────── 

static central_menu_def_t *parse_json(const char *filepath) {
    size_t json_size;
    char  *buf = read_json_file(filepath, &json_size);
    if (!buf) {
        ESP_LOGE(TAG, "Cannot read %s", filepath);
        return NULL;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse error near: %s", cJSON_GetErrorPtr());
        return NULL;
    }

    central_menu_def_t *def = calloc(1, sizeof(central_menu_def_t));
    if (!def) { cJSON_Delete(root); return NULL; }

    // optional states
    cJSON *states = cJSON_GetObjectItem(root, "states");
    if (cJSON_IsArray(states)) {
        cJSON *sj = NULL;
        cJSON_ArrayForEach(sj, states) {
            cJSON *key = cJSON_GetObjectItem(sj, "key");
            if (cJSON_IsString(key))
                interface_central_state_register(key->valuestring);
        }
    }

    // optional initial_menu override
    cJSON *initial = cJSON_GetObjectItem(root, "initial_menu");
    if (cJSON_IsString(initial))
        strncpy(def->initial_menu_id, initial->valuestring, CENTRAL_MENU_ID_LEN - 1);

    cJSON *menus = cJSON_GetObjectItem(root, "menus");
    if (!cJSON_IsArray(menus)) {
        ESP_LOGE(TAG, "No 'menus' array in %s", filepath);
        free(def); cJSON_Delete(root); return NULL;
    }

    int count = cJSON_GetArraySize(menus);
    def->nodes = calloc(count, sizeof(central_menu_node_t));
    if (!def->nodes) { free(def); cJSON_Delete(root); return NULL; }


    int valid = 0;
    cJSON *mj = NULL;
    cJSON_ArrayForEach(mj, menus) {
        central_menu_node_t *node = &def->nodes[valid];

        cJSON *id = cJSON_GetObjectItem(mj, "id");
        if (!cJSON_IsString(id)) {
            ESP_LOGW(TAG, "Menu node missing 'id', skipping");
            continue;
        }
        strncpy(node->id, id->valuestring, CENTRAL_MENU_ID_LEN - 1);

        cJSON *title = cJSON_GetObjectItem(mj, "title");
        if (cJSON_IsString(title))
            strncpy(node->title, title->valuestring, CENTRAL_MENU_LABEL_LEN - 1);
        else
            strncpy(node->title, node->id, CENTRAL_MENU_LABEL_LEN - 1);

        cJSON *on_enter = cJSON_GetObjectItem(mj, "on_enter");
        if (cJSON_IsString(on_enter))
            strncpy(node->on_enter, on_enter->valuestring, CENTRAL_MENU_LUA_LEN - 1);

        // Parse items — allocate only what is needed
        cJSON *items_json = cJSON_GetObjectItem(mj, "items");
        int item_count = 0;
        if (cJSON_IsArray(items_json)) {
            int raw = cJSON_GetArraySize(items_json);
            if (raw > CENTRAL_MENU_MAX_ITEMS) {
                ESP_LOGW(TAG, "Node '%s': clamping %d items to %d",
                         node->id, raw, CENTRAL_MENU_MAX_ITEMS);
                raw = CENTRAL_MENU_MAX_ITEMS;
            }
            if (raw > 0) {
                node->items = calloc(raw, sizeof(central_menu_item_t));
                if (!node->items) {
                    ESP_LOGE(TAG, "OOM allocating items for '%s'", node->id);
                    // leave item_count = 0, continue
                } else {
                    cJSON *ij = NULL;
                    cJSON_ArrayForEach(ij, items_json) {
                        if (item_count >= raw) break;
                        cJSON *iid = cJSON_GetObjectItem(ij, "id");
                        cJSON *lbl = cJSON_GetObjectItem(ij, "label");
                        if (!cJSON_IsString(iid) || !cJSON_IsString(lbl)) {
                            ESP_LOGW(TAG, "Item in '%s' missing id/label, skipping",
                                     node->id);
                            continue;
                        }
                        strncpy(node->items[item_count].id,
                                iid->valuestring, CENTRAL_MENU_ID_LEN - 1);
                        strncpy(node->items[item_count].label,
                                lbl->valuestring, CENTRAL_MENU_LABEL_LEN - 1);
                        item_count++;
                    }
                }
            }
        }
        node->item_count = item_count;

        // First node becomes initial_menu if not explicitly set
        if (valid == 0 && def->initial_menu_id[0] == '\0')
            strncpy(def->initial_menu_id, node->id, CENTRAL_MENU_ID_LEN - 1);

        ESP_LOGI(TAG, "Loaded node '%s' ('%s')  %d items  on_enter='%s'",
                 node->id, node->title, node->item_count, node->on_enter);
        valid++;
    }

    def->node_count = valid;
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Parsed %d nodes, initial='%s'", valid, def->initial_menu_id);
    return def;
}

static void free_def(central_menu_def_t *def) {
    if (!def) return;
    if (def->nodes) {
        for (int i = 0; i < def->node_count; i++) {
            if (def->nodes[i].items)
                free(def->nodes[i].items);
        }
        free(def->nodes);
    }
    free(def);
}

// ── LUA C-function bindings ────────────────────────────────────────────────────── 

void interface_central_push_menu(const char *id) {
    central_menu_node_t *node = find_node(id);
    if (!node) {
        ESP_LOGW(TAG, "push_menu: unknown node '%s'", id);
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    stack_push(id);
    current_point_to_node(node);
    render_current_menu();
    xSemaphoreGive(g_mutex);

    if (node->on_enter[0] != '\0')
        lua_call_from_field(node->on_enter);
}

void interface_central_pop_menu(void) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const char *parent_id = stack_pop();
    if (!parent_id) {
        xSemaphoreGive(g_mutex);
        return;
    }

    central_menu_node_t *node = find_node(parent_id);
    if (node) {
        current_point_to_node(node);
        render_current_menu();
        xSemaphoreGive(g_mutex);

        if (node->on_enter[0] != '\0')
            lua_call_from_field(node->on_enter);
    } else {
        // In simplified version, this should not happen (no dynamic menus)
        xSemaphoreGive(g_mutex);
        ESP_LOGW(TAG, "pop: parent '%s' not found", parent_id);
    }
}


void interface_central_set_title(const char *title) {
    if (!g_def) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (title && title[0]) {
        strncpy(g_state.current_title, title, CENTRAL_MENU_LABEL_LEN - 1);
        g_state.current_title[CENTRAL_MENU_LABEL_LEN - 1] = '\0';
    } else {
        g_state.current_title[0] = '\0';
    }
    render_current_menu();
    xSemaphoreGive(g_mutex);
}


// ── Lifecycle ────────────────────────────────────────────────────── 

bool interface_central_init(const char *filepath) {
    if (g_def) {
        ESP_LOGW(TAG, "Already initialized");
        return false;
    }

    g_mutex = xSemaphoreCreateMutex();
    if (!g_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return false;
    }

    g_def = parse_json(filepath);
    if (!g_def) {
        vSemaphoreDelete(g_mutex);
        g_mutex = NULL;
        return false;
    }

    central_menu_node_t *initial = find_node(g_def->initial_menu_id);
    if (!initial) {
        ESP_LOGE(TAG, "Initial node '%s' not found", g_def->initial_menu_id);
        free_def(g_def); g_def = NULL;
        vSemaphoreDelete(g_mutex); g_mutex = NULL;
        return false;
    }

    memset(&g_state, 0, sizeof(g_state));
    stack_push(g_def->initial_menu_id);
    current_point_to_node(initial);

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    render_current_menu();
    xSemaphoreGive(g_mutex);

    if (initial->on_enter[0] != '\0')
        lua_call_from_field(initial->on_enter);

    ESP_LOGI(TAG, "Initialized, showing '%s'", g_def->initial_menu_id);
    return true;
}

void interface_central_cleanup(void) {
    if (g_mutex) xSemaphoreTake(g_mutex, portMAX_DELAY);

    free_def(g_def);
    g_def = NULL;
    memset(&g_state, 0, sizeof(g_state));

    if (g_mutex) {
        xSemaphoreGive(g_mutex);
        vSemaphoreDelete(g_mutex);
        g_mutex = NULL;
    }
    ESP_LOGI(TAG, "Cleaned up");
}

// ── Physical button input ────────────────────────────────────────────────────── 

void interface_central_input_next(void) {
    if (!g_def) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (g_state.current_item_count > 0) {
        g_state.selected_index =
            (g_state.selected_index + 1) % g_state.current_item_count;
        render_current_menu();
    }
    xSemaphoreGive(g_mutex);
}

void interface_central_input_back(void) {
    // Reuse Lua binding directly — keeps back behaviour consistent with
    // Lua calling interface_pop_menu() explicitly
//    lua_cf_pop_menu(NULL);
}

void interface_central_input_select(void) {
    if (!g_def) return;

    char item_id[CENTRAL_MENU_ID_LEN] = {0};

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (g_state.current_item_count > 0 &&
        g_state.selected_index < g_state.current_item_count) {
        strncpy(item_id,
                g_state.current_items[g_state.selected_index].id,
                CENTRAL_MENU_ID_LEN - 1);
    }
    xSemaphoreGive(g_mutex);

    if (item_id[0] == '\0') {
        ESP_LOGW(TAG, "select: nothing selected");
        return;
    }

    ESP_LOGI(TAG, "Select: '%s'", item_id);
    lua_call_from_field(item_id);
}


// ── Web button input ────────────────────────────────────────────────────── 
// Called from parse_websocket_message() for type="menu_select"

void interface_central_web_select(const char *item_id) {
    if (!item_id || item_id[0] == '\0') return;
    ESP_LOGI(TAG, "Web select: '%s'", item_id);
    lua_call_from_field(item_id);
}


// ── States ────────────────────────────────────────────────────── 
static interface_central_state_entry_t *find_state_entry(const char *key) {
    if (!key || !key[0]) return NULL;
    for (int i = 0; i < CENTRAL_STATE_MAX_ENTRIES; i++) {
        if (state_entries[i].registered &&
            strcmp(state_entries[i].key, key) == 0)
            return &state_entries[i];
    }
    return NULL;
}

static void interface_central_broadcast_state(const char *key, const char *value) {
    char json[72];
    snprintf(json, sizeof(json),
             "{\"type\":\"central_state\",\"key\":\"%s\",\"value\":\"%s\"}",
             key, value ? value : "");
    websocket_broadcast_json(json);
}

static void interface_central_state_register(const char *key) {
    if (!key || !key[0]) return;

    if (find_state_entry(key)) {
        ESP_LOGW(TAG, "State key '%s' already registered", key);
        return;
    }

    for (int i = 0; i < CENTRAL_STATE_MAX_ENTRIES; i++) {
        if (!state_entries[i].registered) {
            strncpy(state_entries[i].key, key, CENTRAL_STATE_KEY_LEN - 1);
            state_entries[i].key[CENTRAL_STATE_KEY_LEN - 1] = '\0';
            state_entries[i].value[0]   = '\0';
            state_entries[i].registered  = true;
            state_entries[i].initialized = false;
            ESP_LOGI(TAG, "Registered state key '%s'", key);
            return;
        }
    }
    ESP_LOGE(TAG, "No free slots for state '%s'", key);
}

void interface_central_state_set(const char *key, const char *value) {
    if (!key || !key[0]) return;

    interface_central_state_entry_t *e = find_state_entry(key);
    if (!e) {
        ESP_LOGW(TAG, "Unknown key '%s'", key);
        return;
    }

    const char *v = (value && value[0]) ? value : "";
    if (e->initialized && strcmp(e->value, v) == 0)
        return;  // no change, skip broadcast

    strncpy(e->value, v, CENTRAL_STATE_VAL_LEN - 1);
    e->value[CENTRAL_STATE_VAL_LEN - 1] = '\0';
    e->initialized = (v[0] != '\0');

    interface_central_broadcast_state(e->key, e->initialized ? e->value : NULL);
    ESP_LOGI(TAG, "'%s' = '%s'", e->key, e->value);
}

const char *interface_central_state_get(const char *key) {
    const interface_central_state_entry_t *e = find_state_entry(key);
    return (e && e->initialized) ? e->value : NULL;
}

void interface_central_state_clear_all(void) {
    for (int i = 0; i < CENTRAL_STATE_MAX_ENTRIES; i++) {
        if (state_entries[i].registered && state_entries[i].initialized) {
            state_entries[i].value[0]    = '\0';
            state_entries[i].initialized = false;
            interface_central_broadcast_state(state_entries[i].key, NULL);
        }
    }
    ESP_LOGI(TAG, "All state values cleared");
}

