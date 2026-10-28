#include "ble/ble_observe.h"

#include <stdio.h>
#include <string.h>
#include <lua.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gap.h"

#include "ble/ble_scan.h"
#include "ble/device_parser.h"
#include "lua/lua_hook.h"

static const char *TAG = "BLE observe";

#define OBS_SLOTS 8
#define OBS_LEGACY_MAX 31

typedef struct {
    bool active;
    uint8_t company_count;
    uint16_t company_id[PERIPH_OBSERVE_COMPANIES];
    uint8_t mfg_prefix[20];
    uint8_t mfg_prefix_len;
    bool has_svc16;
    uint16_t svc16;
    char name[PERIPH_OBSERVE_NAME_LEN];
    bool has_addr;
    uint8_t addr[6];
    uint8_t addr_type;
    bool has_addr_type;
} observe_seed_t;

typedef struct {
    bool used;
    bool queued;
    bool delivered;
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    bool is_scan_rsp;
    uint8_t len;
    uint8_t data[OBS_LEGACY_MAX];
    uint32_t seen;
    int64_t last_deliver_us;
    int64_t last_rx_us;
} observe_slot_t;

static SemaphoreHandle_t s_mu;
static observe_seed_t s_seed;
static ble_observe_lua_filter_t s_lua;
static bool s_lua_active;
static bool s_autostart;
static bool s_central_mode;
static bool s_wake_posted;
static bool s_logged_missing_adv;
static observe_slot_t s_slots[OBS_SLOTS];

extern ble_server_t *ble_server;

static void ensure_mu(void) {
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
}

static int64_t now_us(void) {
    return esp_timer_get_time();
}

static bool hex_nibble(char c, uint8_t *out) {
    if (c >= '0' && c <= '9') { *out = (uint8_t)(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { *out = (uint8_t)(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { *out = (uint8_t)(c - 'A' + 10); return true; }
    return false;
}

static uint8_t parse_hex(const char *s, uint8_t *out, uint8_t max_len) {
    uint8_t n = 0;
    uint8_t hi = 0;
    bool have_hi = false;
    if (!s) return 0;
    for (const char *p = s; *p; p++) {
        if (*p == ' ' || *p == ':' || *p == '-') continue;
        uint8_t nib;
        if (!hex_nibble(*p, &nib)) return 0;
        if (!have_hi) {
            hi = nib;
            have_hi = true;
        } else {
            if (n >= max_len) return 0;
            out[n++] = (uint8_t)((hi << 4) | nib);
            have_hi = false;
        }
    }
    if (have_hi) return 0;
    return n;
}

static bool parse_u16_hex(const char *s, uint16_t *out) {
    uint8_t b[2];
    uint8_t n = parse_hex(s, b, 2);
    if (n == 0 || n > 2) return false;
    if (n == 1) *out = b[0];
    else *out = (uint16_t)((b[0] << 8) | b[1]);
    return true;
}

static bool mac_equal_str(const char *addr_str, const uint8_t val[6]) {
    uint8_t parsed[6];
    unsigned b[6];
    if (!addr_str || !addr_str[0]) return false;
    if (sscanf(addr_str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) parsed[i] = (uint8_t)b[i];
    return memcmp(parsed, val, 6) == 0;
}

static bool is_own_addr(const uint8_t val[6]) {
    if (!ble_server || !ble_server->adv_set.instances) return false;
    for (uint8_t i = 0; i < ble_server->adv_set.count; i++) {
        const char *s = ble_server->adv_set.instances[i].params.bd_addr;
        if (mac_equal_str(s, val)) return true;
    }
    return false;
}

static bool ad_walk_ok(const uint8_t *d, uint8_t len, const observe_seed_t *seed) {
    bool company_ok = seed->company_count == 0;
    bool mfg_ok = seed->mfg_prefix_len == 0;
    bool svc_ok = !seed->has_svc16;
    bool name_ok = seed->name[0] == '\0';
    if (company_ok && mfg_ok && svc_ok && name_ok && !seed->has_addr) return true;

    uint8_t i = 0;
    while (i + 1 < len) {
        uint8_t field_len = d[i];
        if (field_len == 0) break;
        if ((uint16_t)i + 1u + field_len > len) break;
        uint8_t type = d[i + 1];
        const uint8_t *val = d + i + 2;
        uint8_t val_len = (uint8_t)(field_len - 1);

        if (type == 0xFF && val_len >= 2) {
            uint16_t cid = (uint16_t)(val[0] | (val[1] << 8));
            for (uint8_t c = 0; c < seed->company_count; c++) {
                if (seed->company_id[c] == cid) company_ok = true;
            }
            if (seed->mfg_prefix_len > 0 && val_len >= (uint8_t)(2 + seed->mfg_prefix_len) &&
                memcmp(val + 2, seed->mfg_prefix, seed->mfg_prefix_len) == 0) {
                mfg_ok = true;
            }
        }
        if (type == 0x16 && val_len >= 2 && seed->has_svc16) {
            uint16_t uuid = (uint16_t)(val[0] | (val[1] << 8));
            if (uuid == seed->svc16) svc_ok = true;
        }
        if ((type == 0x08 || type == 0x09) && seed->name[0] && val_len > 0) {
            char tmp[32];
            uint8_t n = val_len < sizeof(tmp) - 1 ? val_len : (uint8_t)(sizeof(tmp) - 1);
            memcpy(tmp, val, n);
            tmp[n] = '\0';
            if (strstr(tmp, seed->name)) name_ok = true;
        }
        i = (uint8_t)(i + 1 + field_len);
    }
    return company_ok && mfg_ok && svc_ok && name_ok;
}

static bool seed_matches(const observe_seed_t *seed, const uint8_t *addr, uint8_t addr_type,
                         const uint8_t *data, uint8_t len) {
    if (seed->has_addr) {
        if (memcmp(seed->addr, addr, 6) != 0) return false;
        if (seed->has_addr_type && seed->addr_type != addr_type) return false;
    }
    return ad_walk_ok(data, len, seed);
}

static bool lua_matches(const ble_observe_lua_filter_t *f, const uint8_t *addr,
                        int8_t rssi, const uint8_t *data, uint8_t len, bool is_scan_rsp) {
    if (f->has_addr && memcmp(f->addr, addr, 6) != 0) return false;
    if (f->has_rssi_min && rssi < f->rssi_min) return false;
    if (f->has_scan_rsp && is_scan_rsp != f->scan_rsp) return false;
    for (uint8_t r = 0; r < f->nrules; r++) {
        const uint8_t off = f->rules[r].offset;
        const uint8_t n = f->rules[r].len;
        if ((uint16_t)off + n > len) return false;
        for (uint8_t i = 0; i < n; i++) {
            uint8_t mask = f->rules[r].mask[i];
            if ((data[off + i] & mask) != (f->rules[r].hex[i] & mask)) return false;
        }
    }
    return true;
}

static bool packet_matches(const uint8_t *addr, uint8_t addr_type, int8_t rssi,
                           const uint8_t *data, uint8_t len, bool is_scan_rsp) {
    if (s_lua_active) return lua_matches(&s_lua, addr, rssi, data, len, is_scan_rsp);
    return seed_matches(&s_seed, addr, addr_type, data, len);
}

static uint32_t active_interval_ms(void) {
    if (!s_lua_active) return 0;
    return s_lua.interval_ms;
}

static observe_slot_t *slot_claim(observe_slot_t *slot, const uint8_t *addr, bool is_scan_rsp) {
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    memcpy(slot->addr, addr, 6);
    slot->is_scan_rsp = is_scan_rsp;
    return slot;
}

static observe_slot_t *slot_for(const uint8_t *addr, bool is_scan_rsp) {
    observe_slot_t *free_slot = NULL;
    observe_slot_t *oldest = NULL;
    for (int i = 0; i < OBS_SLOTS; i++) {
        if (!s_slots[i].used) {
            if (!free_slot) free_slot = &s_slots[i];
            continue;
        }
        if (s_slots[i].is_scan_rsp == is_scan_rsp &&
            memcmp(s_slots[i].addr, addr, 6) == 0) {
            return &s_slots[i];
        }
        if (!oldest || s_slots[i].last_rx_us < oldest->last_rx_us) {
            oldest = &s_slots[i];
        }
    }
    if (free_slot) return slot_claim(free_slot, addr, is_scan_rsp);
    if (!oldest) return NULL;
    return slot_claim(oldest, addr, is_scan_rsp);
}

static void post_wake_after_unlock(bool post) {
    if (!post) return;
    if (lua_post_observe(0) != ESP_OK) {
        if (s_mu && xSemaphoreTake(s_mu, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_wake_posted = false;
            xSemaphoreGive(s_mu);
        }
    }
}

void ble_observe_on_legacy(const uint8_t *addr, uint8_t addr_type, int8_t rssi,
                           const uint8_t *data, uint8_t data_len, bool is_scan_rsp) {
    if (!addr || !data || data_len == 0 || data_len > OBS_LEGACY_MAX) return;
    if (s_central_mode) return;
    ensure_mu();
    if (!s_mu || xSemaphoreTake(s_mu, pdMS_TO_TICKS(5)) != pdTRUE) return;

    if (is_own_addr(addr) || !packet_matches(addr, addr_type, rssi, data, data_len, is_scan_rsp)) {
        xSemaphoreGive(s_mu);
        return;
    }

    observe_slot_t *slot = slot_for(addr, is_scan_rsp);
    if (!slot) {
        xSemaphoreGive(s_mu);
        return;
    }

    bool same = slot->len == data_len &&
                memcmp(slot->data, data, data_len) == 0;
    uint32_t interval = active_interval_ms();
    bool post = false;
    int64_t now = now_us();
    slot->last_rx_us = now;

    if (same) {
        if (slot->seen < UINT32_MAX) slot->seen++;
        slot->rssi = rssi;
        slot->addr_type = addr_type;
        if (slot->queued) {
            if (!s_wake_posted) {
                s_wake_posted = true;
                post = true;
            }
        } else if (interval > 0 &&
                   (now - slot->last_deliver_us) >= (int64_t)interval * 1000) {
            slot->queued = true;
            if (!s_wake_posted) {
                s_wake_posted = true;
                post = true;
            }
        }
    } else {
        memcpy(slot->data, data, data_len);
        slot->len = data_len;
        slot->rssi = rssi;
        slot->addr_type = addr_type;
        slot->seen = 1;
        slot->queued = true;
        if (!s_wake_posted) {
            s_wake_posted = true;
            post = true;
        }
    }

    xSemaphoreGive(s_mu);
    post_wake_after_unlock(post);
}

bool ble_observe_central_match(const uint8_t *addr, uint8_t addr_type, int8_t rssi,
                               const uint8_t *data, uint8_t data_len, bool is_scan_rsp) {
    if (!addr || !data || data_len == 0 || data_len > OBS_LEGACY_MAX) return false;
    ensure_mu();
    if (!s_mu || xSemaphoreTake(s_mu, pdMS_TO_TICKS(5)) != pdTRUE) return false;
    bool ok = s_central_mode && packet_matches(addr, addr_type, rssi, data, data_len, is_scan_rsp);
    xSemaphoreGive(s_mu);
    return ok;
}

static void clear_slots(void) {
    memset(s_slots, 0, sizeof(s_slots));
    s_wake_posted = false;
}

void ble_observe_begin_peripheral(const device_role_peripheral_t *role) {
    ensure_mu();
    if (s_mu) xSemaphoreTake(s_mu, portMAX_DELAY);
    memset(&s_seed, 0, sizeof(s_seed));
    memset(&s_lua, 0, sizeof(s_lua));
    s_lua_active = false;
    s_central_mode = false;
    s_logged_missing_adv = false;
    s_autostart = role && role->observe;
    if (role && role->observe) {
        s_seed.company_count = role->observe_company_count;
        if (s_seed.company_count > PERIPH_OBSERVE_COMPANIES) {
            s_seed.company_count = PERIPH_OBSERVE_COMPANIES;
        }
        memcpy(s_seed.company_id, role->observe_company_id,
               s_seed.company_count * sizeof(uint16_t));
        s_seed.mfg_prefix_len = parse_hex(role->observe_mfg_prefix, s_seed.mfg_prefix,
                                          (uint8_t)sizeof(s_seed.mfg_prefix));
        s_seed.has_svc16 = parse_u16_hex(role->observe_svc16, &s_seed.svc16);
        snprintf(s_seed.name, sizeof(s_seed.name), "%s", role->observe_name);
    }
    clear_slots();
    if (s_mu) xSemaphoreGive(s_mu);
}

void ble_observe_begin_central(const ble_addr_t *addr) {
    ensure_mu();
    if (s_mu) xSemaphoreTake(s_mu, portMAX_DELAY);
    memset(&s_seed, 0, sizeof(s_seed));
    memset(&s_lua, 0, sizeof(s_lua));
    s_lua_active = false;
    s_central_mode = true;
    s_autostart = false;
    s_logged_missing_adv = false;
    if (addr) {
        s_seed.has_addr = true;
        s_seed.has_addr_type = true;
        s_seed.addr_type = addr->type;
        memcpy(s_seed.addr, addr->val, 6);
    }
    clear_slots();
    if (s_mu) xSemaphoreGive(s_mu);
}

void ble_observe_end(void) {
    ensure_mu();
    if (s_mu) xSemaphoreTake(s_mu, portMAX_DELAY);
    memset(&s_seed, 0, sizeof(s_seed));
    memset(&s_lua, 0, sizeof(s_lua));
    s_lua_active = false;
    s_central_mode = false;
    s_autostart = false;
    clear_slots();
    if (s_mu) xSemaphoreGive(s_mu);
    ble_scan_unsubscribe(BLE_SCAN_CONSUMER_SIM);
}

bool ble_observe_autostart_pending(void) {
    return s_autostart;
}

esp_err_t ble_observe_scan_enable(void) {
    s_autostart = true;
    return ble_scan_subscribe(BLE_SCAN_CONSUMER_SIM);
}

esp_err_t ble_observe_scan_disable(void) {
    s_autostart = false;
    return ble_scan_unsubscribe(BLE_SCAN_CONSUMER_SIM);
}

bool ble_observe_set_lua_filter(const ble_observe_lua_filter_t *filter) {
    if (!filter) return false;
    ensure_mu();
    if (!s_mu || xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    s_lua = *filter;
    s_lua_active = true;
    clear_slots();
    xSemaphoreGive(s_mu);
    return true;
}

void ble_observe_clear_lua_filter(void) {
    ensure_mu();
    if (!s_mu || xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) return;
    memset(&s_lua, 0, sizeof(s_lua));
    s_lua_active = false;
    clear_slots();
    xSemaphoreGive(s_mu);
}

void ble_observe_on_forced_stop(void) {
    s_autostart = false;
    lua_post_observe(1);
}

static void hex_encode(const uint8_t *b, uint8_t n, char *out) {
    static const char *H = "0123456789abcdef";
    for (uint8_t i = 0; i < n; i++) {
        out[i * 2] = H[b[i] >> 4];
        out[i * 2 + 1] = H[b[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

void ble_observe_deliver(struct lua_State *L) {
    observe_slot_t snaps[OBS_SLOTS];
    int n = 0;
    ensure_mu();
    if (!s_mu || xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) return;
    s_wake_posted = false;
    int64_t now = now_us();
    for (int i = 0; i < OBS_SLOTS; i++) {
        if (!s_slots[i].used || !s_slots[i].queued) continue;
        snaps[n++] = s_slots[i];
        s_slots[i].queued = false;
        s_slots[i].seen = 0;
        s_slots[i].delivered = true;
        s_slots[i].last_deliver_us = now;
    }
    xSemaphoreGive(s_mu);

    for (int i = 0; i < n; i++) {
        lua_getglobal(L, "on_adv");
        if (!lua_isfunction(L, -1)) {
            if (!s_logged_missing_adv) {
                ESP_LOGW(TAG, "on_adv is not defined");
                s_logged_missing_adv = true;
            }
            lua_pop(L, 1);
            break;
        }
        char addr[18];
        char hex[OBS_LEGACY_MAX * 2 + 1];
        const uint8_t *v = snaps[i].addr;
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 v[5], v[4], v[3], v[2], v[1], v[0]);
        hex_encode(snaps[i].data, snaps[i].len, hex);
        lua_pushstring(L, addr);
        lua_pushinteger(L, snaps[i].addr_type);
        lua_pushinteger(L, snaps[i].rssi);
        lua_pushstring(L, hex);
        lua_pushboolean(L, snaps[i].is_scan_rsp ? 1 : 0);
        lua_pushinteger(L, (lua_Integer)snaps[i].seen);
        if (lua_pcall(L, 6, 0, 0) != LUA_OK) {
            ESP_LOGE(TAG, "on_adv error: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}
