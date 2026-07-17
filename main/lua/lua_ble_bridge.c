#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/ble_uuid.h"
#include <lua.h>
#include <lauxlib.h>

#include "ble/ble_discovery.h"   // stored_service_t, stored_characteristic_t
#include "ble/ble_central.h"     // ble_central_get_services()
#include "ble/ble_sim.h"
#include "ble/device_parser.h"       // create_uuid_from_string()
#include "common/utils.h"
#include "lua/lua_hook.h"            // lua_call_handler_async()
#include "lua/lua_ble_bridge.h"

#define TAG "LUA BLE"

// ble_sim_gap.c, needed for LUA peripheral
extern ble_server_t *ble_server; 
extern uint16_t sim_conn_handle; 

static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// ── UUID HELPERS ────────────────────────────────────────────────── 
// TBD: merge with utils.c

// Normalise any UUID string to uppercase, no dashes, 32 chars (128-bit)
// or 4 chars (16-bit).  Used for comparison only.
static void uuid_normalise(const char *in, char *out, size_t out_len) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j < out_len - 1; i++) {
        if (in[i] != '-') out[j++] = (char)toupper((unsigned char)in[i]);
    }
    out[j] = '\0';
}

// Convert a NimBLE ble_uuid_any_t back to a canonical string.
// 16-bit  → "XXXX"
// 128-bit → "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" (32 hex chars, no dashes)
static void uuid_to_str(const ble_uuid_any_t *uuid, char *buf, size_t buf_len) {
    if (uuid->u.type == BLE_UUID_TYPE_16) {
        snprintf(buf, buf_len, "%04X", uuid->u16.value);
    } else if (uuid->u.type == BLE_UUID_TYPE_128) {
        // NimBLE stores 128-bit UUIDs in little-endian byte order
        for (int i = 15; i >= 0; i--) {
            snprintf(buf + (15 - i) * 2, 3, "%02X", uuid->u128.value[i]);
        }
        buf[32] = '\0';
    } else {
        snprintf(buf, buf_len, "????");
    }
}

// Walk the stored_service_t list and find a characteristic by
// (svc_uuid_str, chr_uuid_str). Both strings are accepted in any case/dash format.
// Returns pointer to matching stored_characteristic_t, or NULL.
static stored_characteristic_t *find_char_by_uuid(const char *svc_uuid_str,
                                                    const char *chr_uuid_str) {
    stored_service_t *services = ble_central_get_services();
    if (!services) return NULL;

    char svc_norm[33], chr_norm[33];
    uuid_normalise(svc_uuid_str, svc_norm, sizeof(svc_norm));
    uuid_normalise(chr_uuid_str, chr_norm, sizeof(chr_norm));

    for (stored_service_t *svc = services; svc; svc = svc->next) {
        char svc_str[33];
        uuid_to_str(&svc->uuid, svc_str, sizeof(svc_str));
        if (strcasecmp(svc_str, svc_norm) != 0) continue;

        for (stored_characteristic_t *chr = svc->characteristics; chr; chr = chr->next) {
            char chr_str[33];
            uuid_to_str(&chr->uuid, chr_str, sizeof(chr_str));
            if (strcasecmp(chr_str, chr_norm) == 0) return chr;
        }
    }
    return NULL;
}

// Reverse lookup: attr_handle → (svc_uuid_str, chr_uuid_str)
// Fills svc_buf and chr_buf (each at least 33 bytes).
// Returns true if found.
static bool handle_to_uuid_strs(uint16_t attr_handle,
                                 char *svc_buf, size_t svc_buf_len,
                                 char *chr_buf, size_t chr_buf_len) {
    stored_service_t *services = ble_central_get_services();
    if (!services) return false;

    for (stored_service_t *svc = services; svc; svc = svc->next) {
        for (stored_characteristic_t *chr = svc->characteristics; chr; chr = chr->next) {
            if (chr->val_handle == attr_handle) {
                uuid_to_str(&svc->uuid, svc_buf, svc_buf_len);
                uuid_to_str(&chr->uuid, chr_buf, chr_buf_len);
                return true;
            }
        }
    }
    return false;
}

// ── Advertisements ────────────────────────────────────────────────── 

// adv_set_data(profile_id, adv_hex [, scan_rsp_hex])
// adv_hex and scan_rsp_hex are hex strings, scan_rsp_hex is optional (nil = don't update)
static int lua_adv_set_data(lua_State *L)
{
    const char *profile_id  = luaL_checkstring(L, 1);
    const char *adv_hex     = luaL_checkstring(L, 2);
    const char *rsp_hex     = lua_isstring(L, 3) ? lua_tostring(L, 3) : NULL;

    if (!ble_server || !ble_server->adv_set.instances) {
        return luaL_error(L, "adv_set_data: no adv set loaded");
    }

    // Find instance by id
    ble_adv_instance_t *inst = NULL;
    for (uint8_t i = 0; i < ble_server->adv_set.count; i++) {
        if (strcmp(ble_server->adv_set.instances[i].params.id, profile_id) == 0) {
            inst = &ble_server->adv_set.instances[i];
            break;
        }
    }
    if (!inst) {
        return luaL_error(L, "adv_set_data: profile '%s' not found", profile_id);
    }

    // --- adv data ---
    if (adv_hex && adv_hex[0] != '\0') {
        uint16_t adv_len = (uint16_t)(strlen(adv_hex) / 2);
        uint8_t *adv_buf = malloc(adv_len);
        if (!adv_buf) return luaL_error(L, "adv_set_data: OOM adv");

        adv_len = (uint16_t)hex_string_to_bytes(adv_hex, adv_buf, adv_len);

        struct os_mbuf *om = os_msys_get_pkthdr(adv_len, 0);
        if (!om) { free(adv_buf); return luaL_error(L, "adv_set_data: OOM mbuf"); }
        if (os_mbuf_append(om, adv_buf, adv_len) != 0) {
            free(adv_buf);
            os_mbuf_free_chain(om);
            return luaL_error(L, "adv_set_data: mbuf append failed");
        }
        free(adv_buf);

        int rc = ble_gap_ext_adv_set_data(inst->instance, om);
        // NimBLE takes ownership of om even on failure
        if (rc != 0) {
            return luaL_error(L, "adv_set_data: ble_gap_ext_adv_set_data failed %d", rc);
        }
        ESP_LOGI(TAG, "adv_set_data: updated adv on '%s' (%d bytes)", profile_id, adv_len);
    }

    // --- scan response (optional) ---
    if (rsp_hex && rsp_hex[0] != '\0') {
        uint16_t rsp_len = (uint16_t)(strlen(rsp_hex) / 2);
        uint8_t *rsp_buf = malloc(rsp_len);
        if (!rsp_buf) return luaL_error(L, "adv_set_data: OOM rsp");

        rsp_len = (uint16_t)hex_string_to_bytes(rsp_hex, rsp_buf, rsp_len);

        struct os_mbuf *om = os_msys_get_pkthdr(rsp_len, 0);
        if (!om) { free(rsp_buf); return luaL_error(L, "adv_set_data: OOM rsp mbuf"); }
        if (os_mbuf_append(om, rsp_buf, rsp_len) != 0) {
            free(rsp_buf);
            os_mbuf_free_chain(om);
            return luaL_error(L, "adv_set_data: rsp mbuf append failed");
        }
        free(rsp_buf);

        int rc = ble_gap_ext_adv_rsp_set_data(inst->instance, om);
        if (rc != 0) {
            return luaL_error(L, "adv_set_data: ble_gap_ext_adv_rsp_set_data failed %d", rc);
        }
        ESP_LOGI(TAG, "adv_set_data: updated scan_rsp on '%s' (%d bytes)", profile_id, rsp_len);
    }

    lua_pushboolean(L, 1);
    return 1;
}

// get_adv_bd_addr(profile_id)
// Resolved TX address for the profile (same as sim_status adv[].bd_addr).
// Valid after the stack has started that profile's advertising (e.g. from on_startup).
// Returns address string, or nil and an error message if missing or not found.
static int lua_get_adv_bd_addr(lua_State *L)
{
    const char *profile_id = luaL_checkstring(L, 1);

    if (!ble_server || !ble_server->adv_set.instances) {
        lua_pushnil(L);
        lua_pushstring(L, "get_adv_bd_addr: no adv set loaded");
        return 2;
    }

    for (uint8_t i = 0; i < ble_server->adv_set.count; i++) {
        ble_adv_params_t *p = &ble_server->adv_set.instances[i].params;
        if (strcmp(p->id, profile_id) != 0) {
            continue;
        }
        if (p->bd_addr[0] == '\0') {
            lua_pushnil(L);
            lua_pushstring(L, "get_adv_bd_addr: bd_addr not set for profile");
            return 2;
        }
        lua_pushstring(L, p->bd_addr);
        return 1;
    }

    lua_pushnil(L);
    lua_pushfstring(L, "get_adv_bd_addr: profile '%s' not found", profile_id);
    return 2;
}

// adv_enable(profile_id)
// Starts a specific advertising profile instance by ID.
// Returns true on success, or false + error string.
static int lua_adv_enable(lua_State *L) {
    const char *profile_id = luaL_checkstring(L, 1);

    if (!ble_server || !ble_server->adv_set.instances)
        return luaL_error(L, "adv_enable: no adv set loaded");

    ble_adv_instance_t *inst = NULL;
    for (uint8_t i = 0; i < ble_server->adv_set.count; i++) {
        if (strcmp(ble_server->adv_set.instances[i].params.id, profile_id) == 0) {
            inst = &ble_server->adv_set.instances[i];
            break;
        }
    }
    if (!inst) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "adv_enable: profile '%s' not found", profile_id);
        return 2;
    }

    if (ble_gap_ext_adv_active(inst->instance)) {
        ESP_LOGI(TAG, "adv_enable: instance %d (%s) already running", inst->instance, profile_id);
        lua_pushboolean(L, 1);
        return 1;
    }

    // Full configure+start (handles both first-time and restart)
    esp_err_t rc = ble_adv_instance_start(inst);
    if (rc != ESP_OK) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "advenable: ble_adv_instance_start failed %d", rc);
        return 2;
    }

    inst->running = true;

    // Resume rotation timer if profile uses rotation
    if (inst->rotation_timer) {
        xTimerStart(inst->rotation_timer, 0);
    }

    ESP_LOGI(TAG, "adv_enable: started profile '%s' on instance %d", profile_id, inst->instance);
    lua_pushboolean(L, 1);
    return 1;
}

// adv_disable(profile_id)
// Stops a specific advertising profile instance by ID.
// Returns true on success, or false + error string.
static int lua_adv_disable(lua_State *L) {
    const char *profile_id = luaL_checkstring(L, 1);

    if (!ble_server || !ble_server->adv_set.instances)
        return luaL_error(L, "adv_disable: no adv set loaded");

    ble_adv_instance_t *inst = NULL;
    for (uint8_t i = 0; i < ble_server->adv_set.count; i++) {
        if (strcmp(ble_server->adv_set.instances[i].params.id, profile_id) == 0) {
            inst = &ble_server->adv_set.instances[i];
            break;
        }
    }
    if (!inst) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "adv_disable: profile '%s' not found", profile_id);
        return 2;
    }

    // Stop rotation timer first to avoid a race condition
    if (inst->rotation_timer) {
        xTimerStop(inst->rotation_timer, 0);
    }

    if (!ble_gap_ext_adv_active(inst->instance)) {
        ESP_LOGI(TAG, "adv_disable: instance %d (%s) already stopped", inst->instance, profile_id);
        inst->running = false;
        lua_pushboolean(L, 1);
        return 1;
    }

    int rc = ble_gap_ext_adv_stop(inst->instance);
    if (rc != 0) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "adv_disable: ble_gap_ext_adv_stop failed %d", rc);
        return 2;
    }

    inst->running = false;
    ESP_LOGI(TAG, "adv_disable: stopped profile '%s' on instance %d", profile_id, inst->instance);
    lua_pushboolean(L, 1);
    return 1;
}


// ── GATT operation result ────────────────────────────────────────────────── 
// lives on the lua_task stack while blocking

typedef struct {
    SemaphoreHandle_t done_sem;
    int               rc;
    uint8_t           data[BLE_LUA_DATA_LEN];
    size_t            data_len;
} ble_op_result_t;

static int gattc_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg) {
    ble_op_result_t *res = (ble_op_result_t *)arg;
    res->rc = error->status;
    xSemaphoreGive(res->done_sem);
    return 0;
}

static int gattc_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg) {
    ble_op_result_t *res = (ble_op_result_t *)arg;
    res->rc = error->status;
    if (error->status == 0 && attr && attr->om) {
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len > BLE_LUA_DATA_LEN) len = BLE_LUA_DATA_LEN;
        ble_hs_mbuf_to_flat(attr->om, res->data, len, NULL);
        res->data_len = len;
    }
    xSemaphoreGive(res->done_sem);
    return 0;
}

// Shared helper: block until semaphore fires or 3s timeout.
// Deletes the semaphore and returns NimBLE rc, or -1 on timeout.
static int wait_op(ble_op_result_t *res) {
    if (xSemaphoreTake(res->done_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        vSemaphoreDelete(res->done_sem);
        return -1;  // timeout sentinel
    }
    vSemaphoreDelete(res->done_sem);
    return res->rc;
}

// ── MTU from LUA ──────────────────────────────────────────────────────────── 

// get_mtu()  ->  integer
// Returns the negotiated ATT MTU for the current connection, or 23 (BLE default) if not connected / MTU not yet exchanged.
// The usable notify payload is get_mtu() - 3.
static int lua_ble_get_mtu(lua_State *L) {
    uint16_t mtu = 23; // BLE spec default
    uint16_t handle = BLE_HS_CONN_HANDLE_NONE;

    if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        handle = g_conn_handle;
    } else if (sim_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        handle = sim_conn_handle;
    }

    if (handle != BLE_HS_CONN_HANDLE_NONE) {
        uint16_t negotiated = ble_att_mtu(handle);
        if (negotiated > 0) {
            mtu = negotiated;
        }
    }
    lua_pushinteger(L, mtu);
    return 1;
}

// set_preferred_mtu(mtu)  ->  true | false, errmsg
// Valid range: 23 .. 517 (BLE spec limits).
static int lua_ble_set_preferred_mtu(lua_State *L) {
    int mtu = (int)luaL_checkinteger(L, 1);
    if (mtu < 23 || mtu > 517) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "set_preferred_mtu: value %d out of range [23..517]", mtu);
        return 2;
    }
    int rc = ble_att_set_preferred_mtu((uint16_t)mtu);
    if (rc != 0) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "set_preferred_mtu: ble_att_set_preferred_mtu rc=%d", rc);
        return 2;
    }
    ESP_LOGI(TAG, "preferred ATT MTU set to %d", mtu);

    if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        rc = ble_gattc_exchange_mtu(g_conn_handle, NULL, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "set_preferred_mtu: ble_gattc_exchange_mtu rc=%d", rc);
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}

// ── Notification dispatch ──────────────────────────────────────────────────────────── 

void ble_lua_bridge_set_conn_handle(uint16_t conn_handle) {
    g_conn_handle = conn_handle;
}

void ble_lua_bridge_clear_conn_handle(void) {
    g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

// Called from ble_central_core.c — NimBLE task context.
// Calls the single Lua handler:  on_notify(svc_uuid, chr_uuid, hex_data)
// We encode as: func="on_notify"  param="SVC_UUID|CHR_UUID|HEXDATA"
// lua_task splits on '|' before calling Lua.
void ble_lua_bridge_on_notify(uint16_t attr_handle,
                               const uint8_t *data, size_t data_len) {
    char svc_str[33] = {0};
    char chr_str[33] = {0};

    if (!handle_to_uuid_strs(attr_handle, svc_str, sizeof(svc_str),
                                           chr_str, sizeof(chr_str))) {
        ESP_LOGW(TAG, "on_notify: unknown handle 0x%04x", attr_handle);
        return;
    }

    // Hex-encode payload (max 128 raw bytes)
    if (data_len > 128) data_len = 128;
    char hex[257] = {0};
    for (size_t i = 0; i < data_len; i++)
        snprintf(hex + i * 2, 3, "%02x", data[i]);

    // Pack into single param string: "SVC_UUID|CHR_UUID|HEXDATA"
    // fits comfortably in lua_event_t.data[256]:
    //   32 + 1 + 32 + 1 + 256 = 322 — exceeds the 256 byte buffer!
    // Use 16-bit UUIDs where possible (4 chars), 128-bit worst case 32.
    // Worst case: 32 + 1 + 32 + 1 + 128*2 = 322 — too large.
    // Solution: limit hex payload to 95 bytes (190 hex chars) leaving room.
    // For full payloads use ble_read() instead of relying on notify data.
    char param[256];
    int written = snprintf(param, sizeof(param), "%s|%s|%s", svc_str, chr_str, hex);
    if (written >= (int)sizeof(param)) {
        // Truncate hex to fit — notify payload was too long
        ESP_LOGW(TAG, "on_notify: param truncated for handle 0x%04x", attr_handle);
        param[sizeof(param) - 1] = '\0';
    }

    ESP_LOGI(TAG, "on_notify: svc=%s chr=%s len=%zu", svc_str, chr_str, data_len);
    lua_call_handler_async("on_notify", param);
}



// ── LUA C bindings - BLE "Central" ──────────────────────────────────────────────────────────── 


// Internal: resolve svc+chr UUIDs from Lua stack positions 1,2 → val_handle.
// Returns val_handle, or pushes error and returns 0.
static uint16_t resolve_handle_from_lua(lua_State *L, const char *caller) {
    const char *svc_uuid = luaL_checkstring(L, 1);
    const char *chr_uuid = luaL_checkstring(L, 2);

    stored_characteristic_t *chr = find_char_by_uuid(svc_uuid, chr_uuid);
    if (!chr) {
        luaL_error(L, "%s: characteristic not found (svc=%s chr=%s)",
                   caller, svc_uuid, chr_uuid);
        return 0;  // unreachable, luaL_error longjmps
    }
    return chr->val_handle;
}

// ble_write(svc_uuid, chr_uuid, data_binary)
// Returns true, or false + error string.
static int lua_ble_write(lua_State *L) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE)
        return luaL_error(L, "ble_write: not connected");

    uint16_t handle = resolve_handle_from_lua(L, "ble_write");

    size_t data_len;
    const char *data = luaL_checklstring(L, 3, &data_len);

    ble_op_result_t res = {0};
    res.done_sem = xSemaphoreCreateBinary();
    if (!res.done_sem) return luaL_error(L, "ble_write: OOM");

    int rc = ble_gattc_write_flat(g_conn_handle, handle,
                                   data, data_len, gattc_write_cb, &res);
    if (rc != 0) {
        vSemaphoreDelete(res.done_sem);
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "ble_write: gattc error %d", rc);
        return 2;
    }

    int result = wait_op(&res);
    if (result == -1) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "ble_write: timeout");
        return 2;
    }
    if (result != 0) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "ble_write: ATT error %d", result);
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

// ble_read(svc_uuid, chr_uuid)
// Returns binary string, or nil + error string.
static int lua_ble_read(lua_State *L) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE)
        return luaL_error(L, "ble_read: not connected");

    uint16_t handle = resolve_handle_from_lua(L, "ble_read");

    ble_op_result_t res = {0};
    res.done_sem = xSemaphoreCreateBinary();
    if (!res.done_sem) return luaL_error(L, "ble_read: OOM");

    int rc = ble_gattc_read(g_conn_handle, handle, gattc_read_cb, &res);
    if (rc != 0) {
        vSemaphoreDelete(res.done_sem);
        lua_pushnil(L);
        lua_pushfstring(L, "ble_read: gattc error %d", rc);
        return 2;
    }

    int result = wait_op(&res);
    if (result == -1) {
        lua_pushnil(L);
        lua_pushstring(L, "ble_read: timeout");
        return 2;
    }
    if (result != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "ble_read: ATT error %d", result);
        return 2;
    }

    lua_pushlstring(L, (const char *)res.data, res.data_len);
    return 1;
}

// ble_subscribe(svc_uuid, chr_uuid)
// Writes 0x0001 to the CCCD. The single on_notify() handler receives all.
// Returns true, or false + error string.
static int lua_ble_subscribe(lua_State *L) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE)
        return luaL_error(L, "ble_subscribe: not connected");

    uint16_t val_handle = resolve_handle_from_lua(L, "ble_subscribe");
    uint16_t cccd_handle = val_handle + 1;
    uint8_t  cccd_val[2] = {0x01, 0x00};

    ble_op_result_t res = {0};
    res.done_sem = xSemaphoreCreateBinary();
    if (!res.done_sem) return luaL_error(L, "ble_subscribe: OOM");

    int rc = ble_gattc_write_flat(g_conn_handle, cccd_handle,
                                   cccd_val, sizeof(cccd_val), gattc_write_cb, &res);
    if (rc != 0) {
        vSemaphoreDelete(res.done_sem);
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "ble_subscribe: gattc error %d", rc);
        return 2;
    }

    int result = wait_op(&res);
    if (result == -1) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "ble_subscribe: timeout");
        return 2;
    }
    if (result != 0) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "ble_subscribe: ATT error %d", result);
        return 2;
    }

    ESP_LOGI(TAG, "Subscribed svc=%s chr=%s (CCCD handle 0x%04x)",
             lua_tostring(L, 1), lua_tostring(L, 2), cccd_handle);
    lua_pushboolean(L, 1);
    return 1;
}

// ble_unsubscribe(svc_uuid, chr_uuid)
static int lua_ble_unsubscribe(lua_State *L) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        lua_pushboolean(L, 0); lua_pushstring(L, "not connected"); return 2;
    }

    uint16_t val_handle = resolve_handle_from_lua(L, "ble_unsubscribe");
    uint16_t cccd_handle = val_handle + 1;
    uint8_t  cccd_val[2] = {0x00, 0x00};

    ble_op_result_t res = {0};
    res.done_sem = xSemaphoreCreateBinary();
    if (!res.done_sem) return luaL_error(L, "ble_unsubscribe: OOM");

    ble_gattc_write_flat(g_conn_handle, cccd_handle,
                         cccd_val, sizeof(cccd_val), gattc_write_cb, &res);
    wait_op(&res);  // best-effort, ignore result

    lua_pushboolean(L, 1);
    return 1;
}

// ble_connected()
static int lua_ble_connected(lua_State *L) {
    lua_pushboolean(L, g_conn_handle != BLE_HS_CONN_HANDLE_NONE);
    return 1;
}

void lua_ble_central_register_functions(lua_State *L) {
    lua_register(L, "ble_write",       lua_ble_write);
    lua_register(L, "ble_read",        lua_ble_read);
    lua_register(L, "ble_subscribe",   lua_ble_subscribe);
    lua_register(L, "ble_unsubscribe", lua_ble_unsubscribe);
    lua_register(L, "ble_connected",   lua_ble_connected);

    // TBD: on_notify
    // optionally: start_notify_wait / finish_notify_wait (available on mobile)

    lua_register(L, "get_mtu",             lua_ble_get_mtu); 
    lua_register(L, "set_preferred_mtu",   lua_ble_set_preferred_mtu);

    ESP_LOGI(TAG, "BLE central Lua functions registered");
}


// ── LUA C bindings - BLE "Peripheral" ──────────────────────────────────────────────────────────── 

// Bluetooth SIG base UUID (little-endian wire order) for 16/32-bit UUID expansion.
static const uint8_t sim_uuid_sig_base[12] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00,
};

static bool sim_uuid_flat128(const ble_uuid_t *uuid, uint8_t out[16]) {
    switch (uuid->type) {
    case BLE_UUID_TYPE_128:
        memcpy(out, BLE_UUID128(uuid)->value, 16);
        return true;
    case BLE_UUID_TYPE_32:
        memcpy(out, sim_uuid_sig_base, 12);
        out[12] = (uint8_t)(BLE_UUID32(uuid)->value & 0xff);
        out[13] = (uint8_t)((BLE_UUID32(uuid)->value >> 8) & 0xff);
        out[14] = (uint8_t)((BLE_UUID32(uuid)->value >> 16) & 0xff);
        out[15] = (uint8_t)((BLE_UUID32(uuid)->value >> 24) & 0xff);
        return true;
    case BLE_UUID_TYPE_16:
        memcpy(out, sim_uuid_sig_base, 12);
        out[12] = (uint8_t)(BLE_UUID16(uuid)->value & 0xff);
        out[13] = (uint8_t)(BLE_UUID16(uuid)->value >> 8);
        out[14] = 0;
        out[15] = 0;
        return true;
    default:
        return false;
    }
}

static bool sim_uuid_equal(const ble_uuid_t *a, const ble_uuid_t *b) {
    uint8_t flat_a[16], flat_b[16];
    if (!sim_uuid_flat128(a, flat_a) || !sim_uuid_flat128(b, flat_b)) {
        return false;
    }
    if (memcmp(flat_a, flat_b, 16) == 0) {
        return true;
    }
    if (memcmp(flat_a, sim_uuid_sig_base, 12) != 0 ||
        memcmp(flat_b, sim_uuid_sig_base, 12) != 0) {
        return false;
    }
    uint16_t short_a = (uint16_t)(flat_a[12] | (flat_a[13] << 8));
    uint16_t short_b = (uint16_t)(flat_b[12] | (flat_b[13] << 8));
    return short_a != 0 && short_a == short_b;
}

static uint16_t sim_find_chr_handle_in_server(const ble_uuid_t *svc_uuid,
                                              const ble_uuid_t *chr_uuid) {
    if (!ble_server || ble_server->service_count <= 0) {
        return 0;
    }

    for (int s = 0; s < ble_server->service_count; s++) {
        struct ble_gatt_svc_def *svc = &ble_server->services[s];
        if (!svc->uuid || !svc->characteristics ||
            !sim_uuid_equal(svc->uuid, svc_uuid)) {
            continue;
        }

        const struct ble_gatt_chr_def *ch;
        for (ch = svc->characteristics; ch->uuid != NULL; ch++) {
            if (!sim_uuid_equal(ch->uuid, chr_uuid)) {
                continue;
            }
            char_context_t *ctx = (char_context_t *)ch->arg;
            if (ctx && ctx->val_handle != 0) {
                return ctx->val_handle;
            }
        }
    }
    return 0;
}

static uint16_t sim_find_chr_handle(const char *svc_uuid_str,
                                     const char *chr_uuid_str) {
    ble_uuid_any_t *svc_uuid = create_uuid_from_string(svc_uuid_str);
    if (!svc_uuid) {
        ESP_LOGE(TAG, "sim_find_chr_handle: bad svc UUID: %s", svc_uuid_str);
        return 0;
    }

    ble_uuid_any_t *chr_uuid = create_uuid_from_string(chr_uuid_str);
    if (!chr_uuid) {
        ESP_LOGE(TAG, "sim_find_chr_handle: bad chr UUID: %s", chr_uuid_str);
        free(svc_uuid);
        return 0;
    }

    uint16_t def_handle = 0;
    uint16_t val_handle = 0;
    int rc = ble_gatts_find_chr(&svc_uuid->u, &chr_uuid->u,
                                 &def_handle, &val_handle);

    if (rc == 0) {
        free(svc_uuid);
        free(chr_uuid);
        return val_handle;
    }

    val_handle = sim_find_chr_handle_in_server(&svc_uuid->u, &chr_uuid->u);
    free(svc_uuid);
    free(chr_uuid);

    if (val_handle == 0) {
        ESP_LOGW(TAG, "sim_find_chr_handle: not found svc=%s chr=%s rc=%d",
                 svc_uuid_str, chr_uuid_str, rc);
        return 0;
    }
    return val_handle;
}


// Peripheral-side notify: ble_notify(svc_uuid, chr_uuid, hex_string)
// data argument is always a hex-encoded string, e.g. "0011aabbcc"
// Updates the simulated characteristic value (char_context_t) then calls
// ble_gatts_chr_updated so only subscribed centrals receive an ATT notify.
// Returns true, or false + error_string
static int lua_ble_notify(lua_State *L) {
    const char *svc_uuid = luaL_checkstring(L, 1);
    const char *chr_uuid = luaL_checkstring(L, 2);
    size_t hex_len;
    const char *hex_str = luaL_checklstring(L, 3, &hex_len);

    ESP_LOGI(TAG, "About to push notification to svc: %s chr: %s", svc_uuid, chr_uuid);

    uint16_t attr_handle = sim_find_chr_handle(svc_uuid, chr_uuid);
    if (attr_handle == 0) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "ble_notify: characteristic not found %s/%s", svc_uuid, chr_uuid);
        return 2;
    }

    // Decode hex string → binary
    uint16_t data_len = (uint16_t)(hex_len / 2);
    uint8_t *data_buf = NULL;
    if (data_len > 0) {
        data_buf = malloc(data_len);
        if (!data_buf) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "ble_notify: OOM");
            return 2;
        }
        data_len = (uint16_t)hex_string_to_bytes(hex_str, data_buf, data_len);
    }

    int rc = ble_sim_chr_update_value_and_notify(attr_handle, data_buf, data_len);
    free(data_buf);
    if (rc != 0) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "ble_notify: rc=%d", rc);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// Like ble_notify, but sends the PDU directly via ble_gatts_notify_custom (legacy path).
// Does not update char_context_t. Does not check CCCD — may transmit even if the central
// has not subscribed. Use for fragmented / opaque payloads where multiple notifies must
// not overwrite the same backing buffer before transmission.
static int lua_ble_notify_raw(lua_State *L) {
    const char *svc_uuid = luaL_checkstring(L, 1);
    const char *chr_uuid = luaL_checkstring(L, 2);
    size_t hex_len;
    const char *hex_str = luaL_checklstring(L, 3, &hex_len);

    if (sim_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "ble_notify_raw: not connected");
        return 2;
    }

    ESP_LOGI(TAG, "ble_notify_raw: svc=%s chr=%s", svc_uuid, chr_uuid);

    uint16_t attr_handle = sim_find_chr_handle(svc_uuid, chr_uuid);
    if (attr_handle == 0) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "ble_notify_raw: characteristic not found %s/%s", svc_uuid,
                        chr_uuid);
        return 2;
    }

    uint16_t data_len = (uint16_t)(hex_len / 2);
    uint8_t *data_buf = NULL;
    if (data_len > 0) {
        data_buf = malloc(data_len);
        if (!data_buf) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "ble_notify_raw: OOM");
            return 2;
        }
        data_len = (uint16_t)hex_string_to_bytes(hex_str, data_buf, data_len);
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data_buf, data_len);
    free(data_buf);  // mbuf holds its own copy
    if (!om) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "ble_notify_raw: OOM mbuf");
        return 2;
    }

    int rc = ble_gatts_notify_custom(sim_conn_handle, attr_handle, om);
    if (rc != 0) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "ble_notify_raw: rc=%d", rc);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}



void lua_ble_peripheral_register_functions(lua_State *L) {
    lua_register(L, "ble_notify", lua_ble_notify);
    lua_register(L, "ble_notify_raw", lua_ble_notify_raw);
    lua_register(L, "adv_set_data", lua_adv_set_data);
    lua_register(L, "get_adv_bd_addr", lua_get_adv_bd_addr);
    lua_register(L, "adv_enable",   lua_adv_enable);
    lua_register(L, "adv_disable",  lua_adv_disable);

    lua_register(L, "get_mtu",             lua_ble_get_mtu); 
    lua_register(L, "set_preferred_mtu",   lua_ble_set_preferred_mtu);

    ESP_LOGI(TAG, "BLE peripheral Lua functions registered");
}

