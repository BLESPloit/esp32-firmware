#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <lua.h>

#define BLE_LUA_DATA_LEN 256

// Called from ble_central_core.c on connect/disconnect
void ble_lua_bridge_set_conn_handle(uint16_t conn_handle);
void ble_lua_bridge_clear_conn_handle(void);

// Called from ble_central_core.c GAP handler on BLE_GAP_EVENT_NOTIFY_RX
void ble_lua_bridge_on_notify(uint16_t attr_handle, const uint8_t *data, size_t data_len);

// Register all BLE Lua bindings — called from lua_hook.c
void lua_ble_central_register_functions(lua_State *L);
void lua_ble_peripheral_register_functions(lua_State *L);
