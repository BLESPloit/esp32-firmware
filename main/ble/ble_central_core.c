#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "ble/ble_discovery.h" 
#include "ble/ble_scan.h"
#include "device_parser.h"
#include "graphics/graphics.h"
#include "interface/interface_central.h"
#include "lua/lua_hook.h"
#include "lua/lua_ble_bridge.h"
#include "common/storage.h"
#include "common/utils.h"
#include "api/web_server.h" 
#include "ble/device_manifest.h"

#include "ble/ble_central.h"

static const char *TAG = "BLE central - core";

static discovery_context_t *central_ctx;
static uint16_t central_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static char cached_device_id[64] = {0};
static relay_pending_t g_relay_pending = {0};
static TimerHandle_t s_central_post_connect_timer = NULL;

static int ble_central_gap_event_handler(struct ble_gap_event *event, void *arg);
void send_update_central_status_to_ws(const char *status);
static relay_send_fn_t g_relay_send = websocket_broadcast_json_transient;

static void central_on_ready(uint16_t conn_handle)
{
    send_update_central_status_to_ws("connected");
    ble_lua_bridge_set_conn_handle(conn_handle);
    lua_call_handler_async("on_connected", NULL);
}

static int central_mtu_exchange_cb(uint16_t conn_handle,
                                   const struct ble_gatt_error *error,
                                   uint16_t mtu, void *arg)
{
    uint16_t negotiated = ble_att_mtu(conn_handle);

    if (error) {
        ESP_LOGW(TAG, "MTU exchange failed: status=%d att_handle=0x%04x; continuing with mtu=%d",
                 error->status, error->att_handle, negotiated);
    } else {
        ESP_LOGI(TAG, "MTU exchange complete; conn=0x%04x mtu=%d (cb mtu=%d)",
                 conn_handle, negotiated, mtu);
    }

    if (conn_handle == central_conn_handle) {
        central_on_ready(conn_handle);
    }
    return 0;
}

static void central_ensure_mtu(uint16_t conn_handle)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    uint16_t cur_mtu = ble_att_mtu(conn_handle);
    if (cur_mtu > 23) {
        ESP_LOGI(TAG, "MTU already %d on conn=0x%04x; skipping exchange", cur_mtu, conn_handle);
        central_on_ready(conn_handle);
        return;
    }

    ESP_LOGI(TAG, "Requesting ATT MTU exchange on conn=0x%04x (current=%d)", conn_handle, cur_mtu);
    int rc = ble_gattc_exchange_mtu(conn_handle, central_mtu_exchange_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gattc_exchange_mtu failed: %d; continuing with default MTU", rc);
        central_on_ready(conn_handle);
    }
}

static void central_cancel_post_connect(void)
{
    if (s_central_post_connect_timer) {
        xTimerStop(s_central_post_connect_timer, 0);
    }
}

static void central_post_connect_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    uint16_t conn = central_conn_handle;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "Central post-connect timer fired but no connection");
        return;
    }
    ESP_LOGI(TAG, "Post-connect settle done; starting MTU on conn=0x%04x", conn);
    central_ensure_mtu(conn);
}

// Defer ATT/MTU so the GAP callback can return and answer L2CAP updates. 
static void central_schedule_post_connect(uint16_t conn_handle, uint32_t delay_ms)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    uint32_t ticks = pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 1);
    if (ticks == 0) {
        ticks = 1;
    }

    if (s_central_post_connect_timer == NULL) {
        s_central_post_connect_timer = xTimerCreate("cent_pc", ticks, pdFALSE, NULL,
                                                    central_post_connect_timer_cb);
        if (s_central_post_connect_timer == NULL) {
            ESP_LOGE(TAG, "Failed to create post-connect timer; starting MTU immediately");
            central_ensure_mtu(conn_handle);
            return;
        }
    } else {
        xTimerStop(s_central_post_connect_timer, 0);
    }

    if (xTimerChangePeriod(s_central_post_connect_timer, ticks, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to arm post-connect timer; starting MTU immediately");
        central_ensure_mtu(conn_handle);
        return;
    }

    ESP_LOGI(TAG, "Scheduled post-connect MTU in %lu ms", (unsigned long)delay_ms);
}

static int central_handle_conn_update_req(struct ble_gap_event *event)
{
    const struct ble_gap_upd_params *peer = event->conn_update_req.peer_params;
    struct ble_gap_upd_params *self = event->conn_update_req.self_params;
    const char *kind = (event->type == BLE_GAP_EVENT_L2CAP_UPDATE_REQ)
                           ? "L2CAP" : "LL";

    if (peer) {
        ESP_LOGI(TAG, "%s conn update req: itvl=%u-%u lat=%u tmo=%u",
                 kind, peer->itvl_min, peer->itvl_max,
                 peer->latency, peer->supervision_timeout);
    } else {
        ESP_LOGI(TAG, "%s conn update req", kind);
    }

    if (self && peer) {
        *self = *peer;
        if (self->supervision_timeout < BLE_CENTRAL_CONN_SUPERVISION_TIMEOUT) {
            self->supervision_timeout = BLE_CENTRAL_CONN_SUPERVISION_TIMEOUT;
        }
    }

    return 0; //accept immediately
}

// ── HELPERS ────────────────────────────────────────────────── 

bool ble_central_is_active(void) { return central_ctx != NULL; }

stored_service_t *ble_central_get_services(void) {
    return central_ctx ? central_ctx->services : NULL;
}

// TBD: merge with utils.c

// Convert os_mbuf chain to hex string (caller must free)
static char *relay_mbuf_to_hex(struct os_mbuf *om) {
    if (!om) return NULL;
    uint16_t total = OS_MBUF_PKTLEN(om);  // total across all fragments
    if (total == 0) return strdup("");
    char *hex = malloc(total * 2 + 1);
    if (!hex) return NULL;

    uint16_t offset = 0;
    struct os_mbuf *cur = om;
    while (cur) {
        uint8_t *data = OS_MBUF_DATA(cur, uint8_t *);
        uint16_t frag_len = cur->om_len;  // length of THIS fragment
        for (uint16_t i = 0; i < frag_len; i++) {
            sprintf(hex + (offset + i) * 2, "%02x", (unsigned int)data[i]);  // cast to uint
        }
        offset += frag_len;
        cur = SLIST_NEXT(cur, om_next);
    }
    hex[total * 2] = '\0';
    return hex;
}

// BLE_UUID_TYPE_32 from ble_uuid_from_str for inputs like "00001800" (len > 6) holds a 16-bit
// assigned UUID in the numeric value; coerce so relay UUID matching works with ATT discovery.
static void relay_normalize_uuid(ble_uuid_any_t *uuid) {
    if (uuid->u.type != BLE_UUID_TYPE_32 || uuid->u32.value > 0xFFFF) {
        return;
    }
    uint16_t v = (uint16_t)uuid->u32.value;
    uuid->u16.u.type = BLE_UUID_TYPE_16;
    uuid->u16.value = v;
}

// Bluetooth SIG base UUID prefix (little-endian wire order, same as NimBLE ble_uuid_base).
static const uint8_t relay_uuid_sig_base[12] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00,
};

static bool relay_is_sig_prefix(const uint8_t flat[16]) {
    return memcmp(flat, relay_uuid_sig_base, 12) == 0;
}

static uint16_t relay_sig_short(const uint8_t flat[16]) {
    if (!relay_is_sig_prefix(flat)) {
        return 0;
    }
    return (uint16_t)(flat[12] | (flat[13] << 8));
}

// Expand any UUID to 128-bit wire form so TYPE_16/32/128 compare equal when semantically equal.
static bool relay_uuid_flat128(const ble_uuid_t *uuid, uint8_t out[16]) {
    switch (uuid->type) {
    case BLE_UUID_TYPE_128:
        memcpy(out, BLE_UUID128(uuid)->value, 16);
        return true;
    case BLE_UUID_TYPE_32:
        memcpy(out, relay_uuid_sig_base, 12);
        out[12] = (uint8_t)(BLE_UUID32(uuid)->value & 0xff);
        out[13] = (uint8_t)((BLE_UUID32(uuid)->value >> 8) & 0xff);
        out[14] = (uint8_t)((BLE_UUID32(uuid)->value >> 16) & 0xff);
        out[15] = (uint8_t)((BLE_UUID32(uuid)->value >> 24) & 0xff);
        return true;
    case BLE_UUID_TYPE_16:
        memcpy(out, relay_uuid_sig_base, 12);
        out[12] = (uint8_t)(BLE_UUID16(uuid)->value & 0xff);
        out[13] = (uint8_t)(BLE_UUID16(uuid)->value >> 8);
        out[14] = 0;
        out[15] = 0;
        return true;
    default:
        return false;
    }
}

static bool relay_uuid_equal(const ble_uuid_t *a, const ble_uuid_t *b) {
    uint8_t flat_a[16], flat_b[16];
    if (!relay_uuid_flat128(a, flat_a) || !relay_uuid_flat128(b, flat_b)) {
        return false;
    }
    if (memcmp(flat_a, flat_b, 16) == 0) {
        return true;
    }
    uint16_t short_a = relay_sig_short(flat_a);
    uint16_t short_b = relay_sig_short(flat_b);
    return short_a != 0 && short_a == short_b;
}

// Parse svc/chr UUID for relay lookup: NimBLE string rules + SIG UUID collapse,
// plus TYPE_32-to-TYPE_16 coercion for padded hex. Fallback: short plain hex when NimBLE rejects (e.g. len < 4).
static bool relay_parse_uuid(const char *str, ble_uuid_any_t *out) {
    if (!str || !out) return false;

    if (ble_uuid_from_str(out, str) == 0) {
        relay_normalize_uuid(out);
        return true;
    }

    size_t len = strlen(str);
    if (len > 8) {
        return false;
    }

    uint32_t val = (uint32_t)strtoul(str, NULL, 16);
    if (val <= 0xFFFF) {
        out->u16.u.type = BLE_UUID_TYPE_16;
        out->u16.value = (uint16_t)val;
    } else {
        out->u32.u.type = BLE_UUID_TYPE_32;
        out->u32.value = val;
    }
    relay_normalize_uuid(out);
    return true;
}


// Find val_handle by service + char UUID strings (traverses central_ctx->services)
static uint16_t relay_find_char_handle(const char *svc_str, const char *chr_str) {
    if (!central_ctx || !central_ctx->services) return 0;
    ble_uuid_any_t svc_u, chr_u;
    if (!relay_parse_uuid(svc_str, &svc_u) || !relay_parse_uuid(chr_str, &chr_u)) return 0;

    stored_service_t *svc = central_ctx->services;
    while (svc) {
        if (relay_uuid_equal(&svc->uuid.u, &svc_u.u)) {
            stored_characteristic_t *chr = svc->characteristics;
            while (chr) {
                if (relay_uuid_equal(&chr->uuid.u, &chr_u.u)) {
                    return chr->val_handle;
                }
                chr = chr->next;
            }
        }
        svc = svc->next;
    }
    ESP_LOGW(TAG, "Relay: char not found svc=%s chr=%s", svc_str, chr_str);
    return 0;
}

// Find CCCD (or any descriptor) handle by service UUID + characteristic UUID + descriptor UUID
static uint16_t relay_find_desc_handle(const char *svc_str, const char *chr_str, uint16_t desc_uuid16) {
    if (!central_ctx || !central_ctx->services) return 0;

    ble_uuid_any_t svc_u, chr_u;
    if (!relay_parse_uuid(svc_str, &svc_u) || !relay_parse_uuid(chr_str, &chr_u)) return 0;

    ble_uuid16_t desc_u = {
        .u.type = BLE_UUID_TYPE_16,
        .value  = desc_uuid16
    };

    stored_service_t *svc = central_ctx->services;
    while (svc) {
        if (relay_uuid_equal(&svc->uuid.u, &svc_u.u)) {
            stored_characteristic_t *chr = svc->characteristics;
            while (chr) {
                if (relay_uuid_equal(&chr->uuid.u, &chr_u.u)) {
                    stored_descriptor_t *desc = chr->descriptors;
                    while (desc) {
                        if (relay_uuid_equal(&desc->uuid.u, &desc_u.u)) {
                            return desc->handle;
                        }
                        desc = desc->next;
                    }
                    // Characteristic matched but descriptor not found
                    ESP_LOGW(TAG, "Relay: desc 0x%04x not found on chr=%s svc=%s",
                             desc_uuid16, chr_str, svc_str);
                    return 0;
                }
                chr = chr->next;
            }
        }
        svc = svc->next;
    }
    ESP_LOGW(TAG, "Relay: svc=%s chr=%s not found (desc lookup)", svc_str, chr_str);
    return 0;
}



// Reverse lookup: handle → UUID strings (writes into fixed bufs, no alloc needed in cb)
static void relay_handle_to_uuids(uint16_t handle, char *svc_buf, size_t svc_len,
                                  char *chr_buf, size_t chr_len) {
    svc_buf[0] = chr_buf[0] = '\0';
    if (!central_ctx) return;
    stored_service_t *svc = central_ctx->services;
    while (svc) {
        stored_characteristic_t *chr = svc->characteristics;
        while (chr) {
            if (chr->val_handle == handle || chr->def_handle == handle) {
                ble_uuid_to_str(&svc->uuid.u, svc_buf);  // NimBLE fills buf[BLE_UUID_STR_LEN]
                ble_uuid_to_str(&chr->uuid.u, chr_buf);
                return;
            }
            chr = chr->next;
        }
        svc = svc->next;
    }
}

// hex string → binary, caller must free; returns byte count
static size_t relay_hex_to_bin(const char *hex, uint8_t **out) {
    size_t len = strlen(hex) / 2;
    *out = malloc(len);
    if (!*out) return 0;
    for (size_t i = 0; i < len; i++) {
        sscanf(hex + 2 * i, "%02hhx", &(*out)[i]);
    }
    return len;
}



// send status update to ws
void send_update_central_status_to_ws(const char *status)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"type\":\"central\",\"status\":\"%s\"}", status);
    websocket_broadcast_json(json);
}


void ble_central_set_relay_sender(relay_send_fn_t fn) {
    g_relay_send = fn ? fn : websocket_broadcast_json_transient;
}


void ble_central_set_pending_seq(uint32_t seq) {
    g_relay_pending.seq   = seq;
    g_relay_pending.valid = true;
}

void ble_central_set_pending_requester(const char *node_id) {
    if (!node_id) return;
    strncpy(g_relay_pending.requester, node_id, sizeof(g_relay_pending.requester) - 1);
    g_relay_pending.requester[sizeof(g_relay_pending.requester) - 1] = '\0';
}


// ── BLE central scanning ────────────────────────────────────────────────── 

// TBD: maybe reuse the BLE discovery?
esp_err_t ble_central_start_scanning(void)
{
    struct ble_gap_disc_params disc_params;

    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.filter_duplicates = 1; // get each advertisement only once
    disc_params.passive = 0; // we want also the scan responses
    // leave default for the rest parameters
    disc_params.itvl = 0; 
    disc_params.window = 0;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    ESP_LOGI(TAG, "Start scanning...");

    uint8_t own_addr_type;
    int rc = ble_central_infer_own_addr_type(&own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer own addr type: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER,
                      &disc_params, ble_central_gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start scan: %d", rc);
        return ESP_FAIL;
    }

    send_update_central_status_to_ws("scanning");

    set_ble_scanning(true);

    return ESP_OK;

}


void ble_central_attach_from_discovery(uint16_t conn_handle,
                                       discovery_context_t *ctx,
                                       void *arg)
{
    ESP_LOGI(TAG, "Attaching from discovery, conn_handle=0x%04x", conn_handle);

    if (central_ctx) {
        // Terminate old connection first so its GAP events arrive before
        // we replace central_ctx — prevents stale handle lookups
        if (central_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
            central_conn_handle != conn_handle) {
            ESP_LOGI(TAG, "Terminating previous connection 0x%04x", central_conn_handle);
            ble_gap_terminate(central_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            vTaskDelay(pdMS_TO_TICKS(500));  // ← wait for NimBLE to complete teardown
            central_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }

        free_services(central_ctx->services);
        if (central_ctx->json_root) cJSON_Delete(central_ctx->json_root);
        free(central_ctx);
        central_ctx = NULL;  // ← clear before re-assign so any racing event sees NULL
    }

    central_ctx = ctx;
    central_conn_handle = conn_handle;
    central_ctx->phase = DISC_PHASE_COMPLETE;

    ble_gap_set_event_cb(conn_handle, ble_central_gap_event_handler, NULL);

    central_ensure_mtu(conn_handle);
}



// ── Load from json ────────────────────────────────────────────────── 

esp_err_t ble_central_load_services_and_connect(const char *device_id) {

    if (central_ctx && strncmp(cached_device_id, device_id, sizeof(cached_device_id)) == 0) {
        ESP_LOGI(TAG, "Using cached services for %s", device_id);
        ble_central_connect(&central_ctx->device_addr);
        return ESP_OK;
    }

    size_t file_size;

    ESP_LOGI(TAG, "Loading stored BLE services for connecting to: %s", device_id);

    // Read manifest.json and resolve all file paths from it

    device_paths_t *paths = manifest_resolve(device_id);
    if (!paths || !paths->central) {
        device_paths_free(paths); free(paths);
        return ESP_FAIL;
    }

    lua_init_persistent_minimal(paths->central->entry, true, paths->vars, paths->uuids);
    interface_central_init(paths->central->menu);

    // Load BLE services JSON 
    char *json_buffer = read_json_file(paths->profile, &file_size);
    if (!json_buffer) return ESP_FAIL;
    ESP_LOGI(TAG, "Read %d bytes from JSON services", file_size);

    central_ctx = create_discovery_context(0, 0, false, PAIRING_MODE_NONE, PAIRING_STRATEGY_AUTO, 0);
    if (!central_ctx) {
        ESP_LOGE(TAG, "Failed to create 'central' context");
        free(json_buffer);
        return ESP_FAIL;
    }

    stored_service_t *ble_services = NULL;
    if (parse_json_to_discovery(json_buffer, &ble_services, &central_ctx->device_addr)) {
        central_ctx->services = ble_services;
        strncpy(cached_device_id, device_id, sizeof(cached_device_id) - 1);
    } else {
        ESP_LOGE(TAG, "Error parsing JSON services");
    }
    free(json_buffer);

    if (central_ctx != NULL) {
        ESP_LOGI(TAG, "Successfully parsed BLE services for connection");
        lua_init_persistent_minimal(paths->central->entry, true, paths->vars, paths->uuids);
        interface_central_init(paths->central->menu);
        ble_central_start_scanning();
        device_paths_free(paths);
        free(paths);
        log_memory_usage("BLE central ready");
    } else {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return ESP_FAIL;
    }

    return ESP_OK;
}

// free resources, unload graphics, fonts, ...
void unload_ble_device_for_central(void) {
    ESP_LOGI(TAG, "Unloading BLE device central...");

    if (!central_ctx) {
        ESP_LOGW(TAG, "unload_ble_device_for_central: already unloaded, ignoring");
        return;
    }

    central_cancel_post_connect();

    // Cancel ongoing operations (pending connect + scan)
    if (ble_gap_conn_active()) {
        ble_gap_conn_cancel();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "Failed to cancel scan: %d", rc);
    }

    // terminate established connection
    if (central_conn_handle != BLE_HS_CONN_HANDLE_NONE) {  
        ESP_LOGI(TAG, "Terminating connection handle 0x%04x", central_conn_handle);
        rc = ble_gap_terminate(central_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to terminate: %d", rc);
        }
        vTaskDelay(pdMS_TO_TICKS(200));  // Allow disconnect event
        central_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
  
    // Cleanup interface
    interface_central_cleanup();
    
    // Cleanup graphics
    graphics_cleanup();
    
    // Cleanup Lua state
    lua_cleanup();

    // Cleanup services
    free_services(central_ctx->services);
    if (central_ctx->json_root) cJSON_Delete(central_ctx->json_root);
    free(central_ctx);
    central_ctx = NULL;

    interface_central_state_clear_all(); // or maybe leave it for the next connection?

    ESP_LOGI(TAG, "BLE device unloaded successfully");
    log_memory_usage("After BLE 'central' unload");
}



int ble_central_connect(ble_addr_t *addr)
{

    int rc;
    struct ble_gap_conn_params conn_params = {0};
    uint8_t own_addr_type;

    // Stop scan
    rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "Failed to cancel scan: %d", rc);
    }
    set_ble_scanning(false);
    
    vTaskDelay(pdMS_TO_TICKS(100));    

    // Cancel ongoing operations
    if (ble_gap_conn_active()) {
        ble_gap_conn_cancel();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    vTaskDelay(pdMS_TO_TICKS(100));     
    
    ble_central_fill_conn_params(&conn_params);

    
    rc = ble_central_infer_own_addr_type(&own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "error determining address type; rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Own address type: %d", own_addr_type);

    send_update_central_status_to_ws("connecting");

    ble_gap_connect(own_addr_type, addr, 10000, &conn_params, ble_central_gap_event_handler, NULL);

    central_ctx->phase = DISC_PHASE_COMPLETE; // skip discovery

    ESP_LOGI(TAG, "Connection initiated...");
    return 0;
}


// simple matching by address + type for starters
bool ble_central_is_adv_matching(const struct ble_gap_ext_disc_desc *disc)
{

   if (!central_ctx || !disc) return false;

   // Compare address type
   if (central_ctx->device_addr.type != disc->addr.type) {
//        ESP_LOGI(TAG, "Addr type mismatch: expected %d, got %d", central_ctx->device_addr.type, disc->addr.type);
        return false;
   }
   
   // compare address
   return memcmp(central_ctx->device_addr.val, disc->addr.val, sizeof(disc->addr.val)) == 0;

}



// Returns heap-allocated ",\"seq\":N,\"dst\":\"ESP_XXXX\"" suffix, or "" if none pending.
// Clears the pending slot — call exactly once per response.
static char *relay_routing_suffix(void) {
    if (!g_relay_pending.valid) return strdup("");
    char *out = malloc(48);
    if (!out) return strdup("");
    snprintf(out, 48, ",\"seq\":%lu,\"dst\":\"%s\"",
             (unsigned long)g_relay_pending.seq,
             g_relay_pending.requester);
    g_relay_pending.valid = false;
    return out;
}

// ── Callbacks ────────────────────────────────────────────────── 

static int relay_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg) {
    char svc_buf[BLE_UUID_STR_LEN] = {0};
    char chr_buf[BLE_UUID_STR_LEN] = {0};
    char resp[560];

    relay_handle_to_uuids(attr ? attr->handle : 0, svc_buf, sizeof(svc_buf),
                          chr_buf, sizeof(chr_buf));

    char *routing = relay_routing_suffix();
    if (error->status == 0 && attr && attr->om) {
        char *hex = relay_mbuf_to_hex(attr->om);
        snprintf(resp, sizeof(resp),
                 "{\"type\":\"relay\",\"action\":\"read_rsp\",\"svc\":\"%s\",\"chr\":\"%s\","
                 "\"data\":\"%s\",\"status\":0%s}",
                 svc_buf, chr_buf, hex ? hex : "", routing);
        free(hex);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"type\":\"relay\",\"action\":\"read_rsp\",\"svc\":\"%s\",\"chr\":\"%s\","
                 "\"status\":%d%s}",
                 svc_buf, chr_buf, error->status, routing);
    }
    free(routing);
    websocket_broadcast_json(resp);
    return 0;
}


static int relay_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg) {
    char svc_buf[BLE_UUID_STR_LEN] = {0};
    char chr_buf[BLE_UUID_STR_LEN] = {0};
    char resp[300];

    relay_handle_to_uuids(attr ? attr->handle : 0, svc_buf, sizeof(svc_buf),
                          chr_buf, sizeof(chr_buf));

    char *routing = relay_routing_suffix();
    snprintf(resp, sizeof(resp),
             "{\"type\":\"relay\",\"action\":\"write_rsp\",\"svc\":\"%s\",\"chr\":\"%s\","
             "\"status\":%d%s}",
             svc_buf, chr_buf, error->status, routing);
    free(routing);
    websocket_broadcast_json(resp);
    return 0;
}


// read descriptor (for now mostly 0x2901)
static int relay_desc_read_cb(uint16_t conn_handle,
                               const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr,
                               void *arg)
{
    relay_desc_read_ctx_t *ctx = (relay_desc_read_ctx_t *)arg;
    char resp[580];
    char *routing = relay_routing_suffix();

    if (error->status == 0 && attr && attr->om) {
        char *hex = relay_mbuf_to_hex(attr->om);
        snprintf(resp, sizeof(resp),
            "{\"type\":\"relay\",\"action\":\"read_desc_rsp\","
            "\"svc\":\"%s\",\"chr\":\"%s\",\"desc\":\"%s\","
            "\"data\":\"%s\",\"status\":0%s}",
            ctx ? ctx->svc : "",
            ctx ? ctx->chr : "",
            ctx ? ctx->desc : "",
            hex ? hex : "",
            routing);
        free(hex);
    } else {
        snprintf(resp, sizeof(resp),
            "{\"type\":\"relay\",\"action\":\"read_desc_rsp\","
            "\"svc\":\"%s\",\"chr\":\"%s\",\"desc\":\"%s\","
            "\"status\":%d%s}",
            ctx ? ctx->svc : "",
            ctx ? ctx->chr : "",
            ctx ? ctx->desc : "",
            error->status,
            routing);
    }
    free(routing);
    free(ctx);
    websocket_broadcast_json(resp);
    return 0;
}

// subscribe callback
static int relay_subscribe_cb(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr,
                              void *arg)
{
    relay_subscribe_ctx_t *ctx = (relay_subscribe_ctx_t *)arg;
    char resp[300];
    char *routing = relay_routing_suffix();  // consume seq/dst here

    snprintf(resp, sizeof(resp),
        "{\"type\":\"relay\",\"action\":\"subscribe_rsp\","
        "\"svc\":\"%s\",\"chr\":\"%s\",\"status\":%d%s}",
        ctx ? ctx->svc : "",
        ctx ? ctx->chr : "",
        error->status,
        routing);
    free(routing);
    free(ctx);  // free the heap-allocated context
    websocket_broadcast_json(resp);
    return 0;
}


// Called from BLE_GAP_EVENT_NOTIFY_RX in ble_central_gap_event_handler
static void relay_notify_rx(uint16_t attr_handle, struct os_mbuf *om, bool is_indication) {
    char svc_buf[BLE_UUID_STR_LEN] = {0};
    char chr_buf[BLE_UUID_STR_LEN] = {0};
    char resp[512];

    relay_handle_to_uuids(attr_handle, svc_buf, sizeof(svc_buf), chr_buf, sizeof(chr_buf));
    char *hex = relay_mbuf_to_hex(om);
    snprintf(resp, sizeof(resp),
             "{\"type\":\"relay\",\"action\":\"%s\",\"svc\":\"%s\",\"chr\":\"%s\","
             "\"data\":\"%s\"}",
             is_indication ? "indicate_rx" : "notify_rx",
             svc_buf, chr_buf, hex ? hex : "");
    free(hex);
    websocket_broadcast_json(resp);
}


// Immediate relay reply matching async *_rsp actions; consumes pending seq/dst.
static void relay_send_immediate_rsp(ble_relay_op_t op, int status, const char *err,
                                     const char *svc_uuid_str, const char *chr_uuid_str,
                                     const char *desc_str)
{
    char resp[768];
    char *routing = relay_routing_suffix();
    const char *svc = svc_uuid_str ? svc_uuid_str : "";
    const char *chr = chr_uuid_str ? chr_uuid_str : "";
    const char *desc = desc_str ? desc_str : "";

    switch (op) {
    case BLE_RELAY_OP_READ:
        snprintf(resp, sizeof(resp),
                 "{\"type\":\"relay\",\"action\":\"read_rsp\",\"svc\":\"%s\",\"chr\":\"%s\","
                 "\"status\":%d,\"error\":\"%s\"%s}",
                 svc, chr, status, err ? err : "", routing);
        break;
    case BLE_RELAY_OP_WRITE:
        snprintf(resp, sizeof(resp),
                 "{\"type\":\"relay\",\"action\":\"write_rsp\",\"svc\":\"%s\",\"chr\":\"%s\","
                 "\"status\":%d,\"error\":\"%s\"%s}",
                 svc, chr, status, err ? err : "", routing);
        break;
    case BLE_RELAY_OP_WRITE_NORESP:
        if (err && err[0]) {
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"relay\",\"action\":\"write_noresp_rsp\","
                     "\"characteristicUuid\":\"%s\",\"status\":%d,\"error\":\"%s\"%s}",
                     chr, status, err, routing);
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"relay\",\"action\":\"write_noresp_rsp\","
                     "\"characteristicUuid\":\"%s\",\"status\":%d%s}",
                     chr, status, routing);
        }
        break;
    case BLE_RELAY_OP_READ_DESC:
        snprintf(resp, sizeof(resp),
                 "{\"type\":\"relay\",\"action\":\"read_desc_rsp\","
                 "\"svc\":\"%s\",\"chr\":\"%s\",\"desc\":\"%s\","
                 "\"status\":%d,\"error\":\"%s\"%s}",
                 svc, chr, desc, status, err ? err : "", routing);
        break;
    case BLE_RELAY_OP_SUBSCRIBE:
    case BLE_RELAY_OP_UNSUBSCRIBE:
        snprintf(resp, sizeof(resp),
                 "{\"type\":\"relay\",\"action\":\"subscribe_rsp\","
                 "\"svc\":\"%s\",\"chr\":\"%s\",\"status\":%d,\"error\":\"%s\"%s}",
                 svc, chr, status, err ? err : "", routing);
        break;
    default:
        snprintf(resp, sizeof(resp),
                 "{\"type\":\"relay\",\"action\":\"response\",\"status\":%d,\"error\":\"%s\"%s}",
                 status, err ? err : "", routing);
        break;
    }
    free(routing);
    websocket_broadcast_json(resp);
}

// ── RELAY ────────────────────────────────────────────────── 

void ble_central_relay_op(const char *svc_uuid_str, const char *chr_uuid_str,
                          const char *data_hex, ble_relay_op_t op) {
    if (central_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGE(TAG, "Relay: no connection");
        relay_send_immediate_rsp(op, -1, "Not connected", svc_uuid_str, chr_uuid_str, NULL);
        return;
    }

    uint16_t handle = relay_find_char_handle(svc_uuid_str, chr_uuid_str);
    if (!handle) {
        relay_send_immediate_rsp(op, -2, "UUID not found", svc_uuid_str, chr_uuid_str,
                                 op == BLE_RELAY_OP_READ_DESC ? data_hex : NULL);
        return;
    }

    int rc = 0;
    switch (op) {
        case BLE_RELAY_OP_READ:
            rc = ble_gattc_read(central_conn_handle, handle, relay_read_cb, NULL);
            if (rc) {
                ESP_LOGE(TAG, "ble_gattc_read failed: %d", rc);
                relay_send_immediate_rsp(BLE_RELAY_OP_READ, rc, "GATT read failed",
                                         svc_uuid_str, chr_uuid_str, NULL);
            }
            break;

        case BLE_RELAY_OP_WRITE: {
            if (!data_hex || strlen(data_hex) % 2) {
                relay_send_immediate_rsp(BLE_RELAY_OP_WRITE, -3, "Invalid hex data",
                                         svc_uuid_str, chr_uuid_str, NULL);
                break;
            }
            uint8_t *bin = NULL;
            size_t bin_len = relay_hex_to_bin(data_hex, &bin);
            // ble_gattc_write_flat takes flat buffer directly — no mbuf[web:31]
            rc = ble_gattc_write_flat(central_conn_handle, handle,
                                    bin, (uint16_t)bin_len,
                                    relay_write_cb, NULL);
            free(bin);
            if (rc) {
                ESP_LOGE(TAG, "ble_gattc_write_flat failed: %d", rc);
                relay_send_immediate_rsp(BLE_RELAY_OP_WRITE, rc, "GATT write failed",
                                         svc_uuid_str, chr_uuid_str, NULL);
            }
            break;
        }

        case BLE_RELAY_OP_WRITE_NORESP: {
            if (!data_hex || strlen(data_hex) % 2) {
                relay_send_immediate_rsp(BLE_RELAY_OP_WRITE_NORESP, -3, "Invalid hex data",
                                         svc_uuid_str, chr_uuid_str, NULL);
                break;
            }
            uint8_t *bin = NULL;
            size_t bin_len = relay_hex_to_bin(data_hex, &bin);
            // ble_gattc_write_no_rsp_flat — flat buffer, no mbuf, no cb[web:31]
            rc = ble_gattc_write_no_rsp_flat(central_conn_handle, handle,
                                            bin, (uint16_t)bin_len);
            free(bin);
            relay_send_immediate_rsp(BLE_RELAY_OP_WRITE_NORESP, rc,
                                     rc ? "GATT write failed" : NULL,
                                     svc_uuid_str, chr_uuid_str, NULL);
            break;
        }

        case BLE_RELAY_OP_READ_DESC: {
            // datahex carries the descriptor UUID as a short hex string e.g. "2901"
            if (!data_hex || strlen(data_hex) < 4) {
                relay_send_immediate_rsp(BLE_RELAY_OP_READ_DESC, -3, "Missing desc UUID",
                                         svc_uuid_str, chr_uuid_str, "");
                return;
            }
            uint16_t desc_uuid16 = (uint16_t)strtoul(data_hex, NULL, 16);
            uint16_t desc_handle = relay_find_desc_handle(svc_uuid_str, chr_uuid_str, desc_uuid16);
            if (!desc_handle) {
                relay_send_immediate_rsp(BLE_RELAY_OP_READ_DESC, -2, "Descriptor not found",
                                         svc_uuid_str, chr_uuid_str, data_hex);
                return;
            }
            relay_desc_read_ctx_t *ctx = calloc(1, sizeof(relay_desc_read_ctx_t));
            if (ctx) {
                // Resolve svc/chr UUIDs via the char value handle for the response
                relay_handle_to_uuids(relay_find_char_handle(svc_uuid_str, chr_uuid_str),
                                    ctx->svc, sizeof(ctx->svc),
                                    ctx->chr, sizeof(ctx->chr));
                snprintf(ctx->desc, sizeof(ctx->desc), "%s", data_hex);
            }
            rc = ble_gattc_read(central_conn_handle, desc_handle, relay_desc_read_cb, ctx);
            if (rc) {
                ESP_LOGE(TAG, "ble_gattc_read desc failed: %d", rc);
                free(ctx);
                relay_send_immediate_rsp(BLE_RELAY_OP_READ_DESC, rc, "GATT read failed",
                                         svc_uuid_str, chr_uuid_str, data_hex);
            }
            return;
        }

        case BLE_RELAY_OP_SUBSCRIBE: {
            uint16_t cccd_handle = relay_find_desc_handle(svc_uuid_str, chr_uuid_str, 0x2902);
            if (!cccd_handle) {
                ESP_LOGW(TAG, "Relay: no CCCD for %s / %s", svc_uuid_str, chr_uuid_str);
                relay_send_immediate_rsp(BLE_RELAY_OP_SUBSCRIBE, -2, "CCCD not found",
                                         svc_uuid_str, chr_uuid_str, NULL);
                break;
            }
            // Allocate context so the callback knows which svc/chr this was for
            relay_subscribe_ctx_t *ctx = calloc(1, sizeof(relay_subscribe_ctx_t));
            if (ctx) {
                relay_handle_to_uuids(
                    relay_find_char_handle(svc_uuid_str, chr_uuid_str),  // use val_handle for lookup
                    ctx->svc, sizeof(ctx->svc),
                    ctx->chr, sizeof(ctx->chr));
            }
            bool indicate = data_hex && data_hex[0] == '1';
            uint16_t val = indicate ? 0x0002 : 0x0001;
            rc = ble_gattc_write_flat(central_conn_handle, cccd_handle,
                                &val, sizeof(val), relay_subscribe_cb, ctx);
            if (rc) {
                ESP_LOGE(TAG, "ble_gattc_write_flat subscribe failed: %d", rc);
                free(ctx);
                relay_send_immediate_rsp(BLE_RELAY_OP_SUBSCRIBE, rc, "GATT write failed",
                                         svc_uuid_str, chr_uuid_str, NULL);
            }
            break;
        }
        case BLE_RELAY_OP_UNSUBSCRIBE: {
            uint16_t cccd_handle = relay_find_desc_handle(svc_uuid_str, chr_uuid_str, 0x2902);
            if (!cccd_handle) {
                relay_send_immediate_rsp(BLE_RELAY_OP_UNSUBSCRIBE, -2, "CCCD not found",
                                         svc_uuid_str, chr_uuid_str, NULL);
                break;
            }
            relay_subscribe_ctx_t *ctx = calloc(1, sizeof(relay_subscribe_ctx_t));
            if (ctx) {
                relay_handle_to_uuids(
                    relay_find_char_handle(svc_uuid_str, chr_uuid_str),
                    ctx->svc, sizeof(ctx->svc),
                    ctx->chr, sizeof(ctx->chr));
            }
            uint16_t val = 0x0000;
            rc = ble_gattc_write_flat(central_conn_handle, cccd_handle,
                                &val, sizeof(val), relay_subscribe_cb, ctx);
            if (rc) {
                ESP_LOGE(TAG, "ble_gattc_write_flat unsubscribe failed: %d", rc);
                free(ctx);
                relay_send_immediate_rsp(BLE_RELAY_OP_UNSUBSCRIBE, rc, "GATT write failed",
                                         svc_uuid_str, chr_uuid_str, NULL);
            }
            break;
        }


        default:
            ESP_LOGW(TAG, "Relay: unknown op %d", op);
            relay_send_immediate_rsp(op, -4, "Unknown relay op", svc_uuid_str, chr_uuid_str, NULL);
            break;
    }
}

// ── REATTACH ────────────────────────────────────────────────── 
// connect to the device handed from discovery

esp_err_t ble_central_reattach(uint16_t conn_handle)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGE(TAG, "ble_central_reattach: invalid conn_handle");
        return ESP_FAIL;
    }

    if (!central_ctx || !central_ctx->services) {
        ESP_LOGW(TAG, "ble_central_reattach: no cached services, cannot reattach");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Reattaching central to existing conn_handle=0x%04x", conn_handle);

    // Re-sync the handle in case it drifted
    central_conn_handle = conn_handle;
    central_ctx->conn_handle = conn_handle;

    // Re-register central's GAP event handler (discovery may have taken it over)
    ble_gap_set_event_cb(conn_handle, ble_central_gap_event_handler, NULL);

    central_ensure_mtu(conn_handle);

    return ESP_OK;
}


// ── Central GAP event handler ────────────────────────────────────────────────── 
static int ble_central_gap_event_handler(struct ble_gap_event *event, void *arg)
{
   
    switch (event->type) {

    // scanner
    case BLE_GAP_EVENT_EXT_DISC:
//        ESP_LOGI(TAG, "Extended disc: addr type=%d rssi=%d", event->ext_disc.addr.type, event->ext_disc.rssi);
        if (ble_central_is_adv_matching(&event->ext_disc)) {
            WS_LOGI(TAG, "Matching device found, trying to connect");
            ble_central_connect(&event->ext_disc.addr);
        }
        break;
        
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Scan complete");
        set_ble_scanning(false);
        break;
                

    // connection API
    case BLE_GAP_EVENT_CONNECT:
        WS_LOGI(TAG, "Connection %s; status=%d",
                event->connect.status == 0 ? "established" : "failed",
                event->connect.status);

        if (event->connect.status == 0) {
            central_conn_handle = event->connect.conn_handle;

            // update addr, handle
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(central_conn_handle, &desc) == 0 && central_ctx) {
                memcpy(&central_ctx->device_addr, &desc.peer_id_addr, sizeof(ble_addr_t));
                central_ctx->conn_handle = central_conn_handle;
            }

            // Return quickly so L2CAP Connection Parameter Update can be answered.
            central_schedule_post_connect(event->connect.conn_handle,
                                          BLE_CENTRAL_POST_CONNECT_SETTLE_MS);

        }

        break;
    
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnect; reason=%d", event->disconnect.reason);
        if (event->disconnect.conn.conn_handle != central_conn_handle) {
            // Stale disconnect from previous session — ignore
            ESP_LOGW(TAG, "Ignoring disconnect for old handle 0x%04x (current=0x%04x)",
                    event->disconnect.conn.conn_handle, central_conn_handle);
            return 0;
        }
        central_cancel_post_connect();
        central_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_lua_bridge_clear_conn_handle();
        send_update_central_status_to_ws("disconnected");
        return 0;


    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "Connection updated; status=%d", event->conn_update.status);
        break;

    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        return central_handle_conn_update_req(event);

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "GAP MTU update; conn=0x%04x mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;
        
    case BLE_GAP_EVENT_NOTIFY_RX:
        ESP_LOGI(TAG, "Notify/indicate rx handle=0x%04x indication=%d",
                event->notify_rx.attr_handle, event->notify_rx.indication);
        relay_notify_rx(event->notify_rx.attr_handle,
                        event->notify_rx.om,
                        event->notify_rx.indication);

        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        uint8_t buf[BLE_LUA_DATA_LEN];
        if (len > sizeof(buf)) len = sizeof(buf);
        ble_hs_mbuf_to_flat(event->notify_rx.om, buf, len, NULL);
        ble_lua_bridge_on_notify(event->notify_rx.attr_handle, buf, len);
        
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:  
    case BLE_GAP_EVENT_PASSKEY_ACTION:
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:       
    case BLE_GAP_EVENT_REPEAT_PAIRING:
    case BLE_GAP_EVENT_PARING_COMPLETE:
        ESP_LOGI(TAG, "Central SMP: to be implemented");
        break;

    default:
        ESP_LOGW(TAG, "Unhandled GAP event: %d", event->type);
        break;
    }
    
    return 0;
}

