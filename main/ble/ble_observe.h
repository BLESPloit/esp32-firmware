#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ble/device_manifest.h"
#include "host/ble_hs.h"

#define BLE_OBSERVE_BYTE_RULES 4
#define BLE_OBSERVE_RULE_LEN   16

// Lua scan_set_filter() table, already decoded. Replaces the seed until cleared.
typedef struct {
    bool has_addr;
    uint8_t addr[6];          // NimBLE val order (addr[0] is the low octet)
    bool has_rssi_min;
    int8_t rssi_min;
    bool has_scan_rsp;
    bool scan_rsp;            // required is_scan_rsp value when has_scan_rsp
    uint32_t interval_ms;     // 0 = deliver a payload only when it changes
    uint8_t nrules;
    struct {
        uint8_t offset;
        uint8_t len;
        uint8_t hex[BLE_OBSERVE_RULE_LEN];
        uint8_t mask[BLE_OBSERVE_RULE_LEN];
    } rules[BLE_OBSERVE_BYTE_RULES];
} ble_observe_lua_filter_t;

// Seed from the peripheral manifest, or from the central's ble.json address.
void ble_observe_begin_peripheral(const device_role_peripheral_t *role);
void ble_observe_begin_central(const ble_addr_t *addr);
void ble_observe_end(void);

// Manifest asked to subscribe after advertising starts, and the script has not
// called scan_disable(). Cleared by a force-stop.
bool ble_observe_autostart_pending(void);

esp_err_t ble_observe_scan_enable(void);
esp_err_t ble_observe_scan_disable(void);

// true on success. false leaves the previous filter in place.
bool ble_observe_set_lua_filter(const ble_observe_lua_filter_t *filter);
void ble_observe_clear_lua_filter(void);

// Host-task entry for one legacy PDU (raw AD, length byte first, <= 31 bytes).
void ble_observe_on_legacy(const uint8_t *addr, uint8_t addr_type, int8_t rssi,
                           const uint8_t *data, uint8_t data_len, bool is_scan_rsp);

// Central connect decision. Same matcher; no on_adv and interval_ms is ignored.
bool ble_observe_central_match(const uint8_t *addr, uint8_t addr_type, int8_t rssi,
                               const uint8_t *data, uint8_t data_len, bool is_scan_rsp);

// Remote Scan stop or central connect dropped the SIM consumer.
void ble_observe_on_forced_stop(void);

// Lua task: deliver queued on_adv calls.
struct lua_State;
void ble_observe_deliver(struct lua_State *L);
