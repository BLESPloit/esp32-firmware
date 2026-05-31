#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/bas/ble_svc_bas.h"
#include "freertos/semphr.h"

#include "ble/device_parser.h"
#include "lua/lua_hook.h"
#include "common/storage.h"
#include "common/utils.h"
#include "api/web_server.h"

#define TAG "BLE sim - GATT"

extern ble_server_t *ble_server;

// Pending relay response — filled by incoming ws relay_rsp message
typedef struct {
    uint32_t          seq;
    SemaphoreHandle_t sem;
    char             *data_hex;   // heap-alloc'd, set by ws handler, freed by caller
    int               status;
    bool              valid;
} sim_relay_pending_t;

static sim_relay_pending_t g_sim_relay_pending = {0};
static SemaphoreHandle_t   g_sim_relay_mutex   = NULL;
static uint32_t            g_sim_relay_seq      = 0;

void ble_sim_relay_init(void) {
    g_sim_relay_mutex            = xSemaphoreCreateMutex();
    g_sim_relay_pending.sem      = xSemaphoreCreateBinary();
    g_sim_relay_pending.valid    = false;
}

// Called by parse_websocket_message when a relay_rsp arrives
void ble_sim_relay_deliver_rsp(uint32_t seq, const char *data_hex, int status) {
    xSemaphoreTake(g_sim_relay_mutex, portMAX_DELAY);
    if (g_sim_relay_pending.valid && g_sim_relay_pending.seq == seq) {
        g_sim_relay_pending.status   = status;
        g_sim_relay_pending.data_hex = data_hex ? strdup(data_hex) : NULL;
        xSemaphoreGive(g_sim_relay_pending.sem);   // unblock GATT callback
    }
    xSemaphoreGive(g_sim_relay_mutex);
}


// Logs information about a connection to the console.
void ble_print_conn_desc(struct ble_gap_conn_desc *desc)
{
	// just to make it shorter
    const uint8_t *ota = desc->our_ota_addr.val;
    const uint8_t *id = desc->our_id_addr.val;
    const uint8_t *p_ota = desc->peer_ota_addr.val;
    const uint8_t *p_id = desc->peer_id_addr.val;

	ESP_LOGI(TAG, "Connection: handle=%d", desc->conn_handle);
	ESP_LOGI(TAG, "  our_ota_addr_type=%d our_ota_addr=%02x:%02x:%02x:%02x:%02x:%02x, our_id_addr_type=%d our_id_addr=%02x:%02x:%02x:%02x:%02x:%02x",
                desc->our_ota_addr.type, ota[5], ota[4], ota[3], ota[2], ota[1], ota[0],
                desc->our_id_addr.type, id[5], id[4], id[3], id[2], id[1], id[0]);
	ESP_LOGI(TAG, "  peer_ota_addr_type=%d peer_ota_addr=%02x:%02x:%02x:%02x:%02x:%02x, peer_id_addr_type=%d peer_id_addr=%02x:%02x:%02x:%02x:%02x:%02x",
                desc->peer_ota_addr.type, p_ota[5], p_ota[4], p_ota[3], p_ota[2], p_ota[1], p_ota[0],
                desc->peer_id_addr.type, p_id[5], p_id[4], p_id[3], p_id[2], p_id[1], p_id[0]);

    ESP_LOGI(TAG, "  conn_itvl=%d conn_latency=%d supervision_timeout=%d encrypted=%d authenticated=%d bonded=%d\n",
                desc->conn_itvl, desc->conn_latency, desc->supervision_timeout, desc->sec_state.encrypted, 
                desc->sec_state.authenticated, desc->sec_state.bonded);

}


// Sends a relay read request over the server WS and blocks until response (or timeout)
// Returns ESP_OK + fills *out_hex (caller must free) on success
static esp_err_t sim_relay_read_sync(const char *svc_uuid, const char *chr_uuid,
                                     char **out_hex, uint32_t timeout_ms) {
    *out_hex = NULL;
    if (!g_sim_relay_mutex) return ESP_FAIL;

    xSemaphoreTake(g_sim_relay_mutex, portMAX_DELAY);
    uint32_t seq = ++g_sim_relay_seq;
    g_sim_relay_pending.seq   = seq;
    g_sim_relay_pending.valid = true;
    g_sim_relay_pending.data_hex = NULL;
    xSemaphoreGive(g_sim_relay_mutex);

    // Send request outward via the ESP32's own WS server broadcast
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type",   "relay");
    cJSON_AddStringToObject(j, "action", "read");
    cJSON_AddStringToObject(j, "svc",    svc_uuid);
    cJSON_AddStringToObject(j, "chr",    chr_uuid);
    cJSON_AddNumberToObject(j, "seq",    (double)seq);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (s) { websocket_broadcast_json_transient(s); free(s); }

    // Block until relay_rsp arrives or timeout
    bool got = xSemaphoreTake(g_sim_relay_pending.sem,
                              pdMS_TO_TICKS(timeout_ms)) == pdTRUE;

    xSemaphoreTake(g_sim_relay_mutex, portMAX_DELAY);
    g_sim_relay_pending.valid = false;
    if (got && g_sim_relay_pending.status == 0) {
        *out_hex = g_sim_relay_pending.data_hex;   // transfer ownership
        g_sim_relay_pending.data_hex = NULL;
    } else {
        free(g_sim_relay_pending.data_hex);
        g_sim_relay_pending.data_hex = NULL;
    }
    xSemaphoreGive(g_sim_relay_mutex);

    return (got && *out_hex) ? ESP_OK : ESP_FAIL;
}

bool ble_sim_lookup_chr_by_val_handle(uint16_t val_handle, char *svc_uuid_out,
                                      char *chr_uuid_out)
{
    if (!ble_server || !svc_uuid_out || !chr_uuid_out || ble_server->service_count <= 0) {
        return false;
    }

    svc_uuid_out[0] = '\0';
    chr_uuid_out[0] = '\0';

    for (int s = 0; s < ble_server->service_count; s++) {
        struct ble_gatt_svc_def *svc = &ble_server->services[s];
        if (!svc->uuid || !svc->characteristics) {
            continue;
        }

        const struct ble_gatt_chr_def *ch;
        for (ch = svc->characteristics; ch->uuid != NULL; ch++) {
            char_context_t *ctx = (char_context_t *)ch->arg;
            if (ctx && ctx->val_handle == val_handle) {
                ble_uuid_to_str_no_0x_prefix(svc->uuid, svc_uuid_out);
                ble_uuid_to_str_no_0x_prefix(ch->uuid, chr_uuid_out);
                return true;
            }
        }
    }

    return false;
}

int ble_sim_chr_update_value_and_notify(uint16_t val_handle,
                                        const uint8_t *data,
                                        uint16_t data_len)
{
    if (data_len > 0 && data == NULL) {
        return BLE_HS_EINVAL;
    }

    if (!ble_server || ble_server->service_count <= 0) {
        return BLE_HS_ENOENT;
    }

    char_context_t *found = NULL;
    for (int s = 0; s < ble_server->service_count; s++) {
        struct ble_gatt_svc_def *svc = &ble_server->services[s];
        if (!svc->uuid || !svc->characteristics) {
            continue;
        }

        const struct ble_gatt_chr_def *ch;
        for (ch = svc->characteristics; ch->uuid != NULL; ch++) {
            char_context_t *ctx = (char_context_t *)ch->arg;
            if (ctx && ctx->val_handle == val_handle) {
                found = ctx;
                break;
            }
        }
        if (found) {
            break;
        }
    }

    if (!found) {
        return BLE_HS_ENOENT;
    }

    if (data_len > found->data_len) {
        uint8_t *new_data = realloc(found->data, data_len);
        if (!new_data) {
            return BLE_HS_ENOMEM;
        }
        found->data = new_data;
    }

    if (data_len > 0) {
        memcpy(found->data, data, data_len);
    }
    found->data_len = data_len;

    ble_gatts_chr_updated(val_handle);
    return 0;
}

// Sends a relay write request — fire-and-forget (no blocking wait)
static esp_err_t sim_relay_write(const char *svc_uuid, const char *chr_uuid,
                                 const char *data_hex) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type",   "relay");
    cJSON_AddStringToObject(j, "action", "write");
    cJSON_AddStringToObject(j, "svc",    svc_uuid);
    cJSON_AddStringToObject(j, "chr",    chr_uuid);
    cJSON_AddStringToObject(j, "data",   data_hex);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return ESP_ERR_NO_MEM;
    websocket_broadcast_json_transient(s);
    free(s);
    return ESP_OK;
}

// GATT access callback, every read/write/... will go through it
// it is set by device_parser for all characteristics
int ble_sim_gatt_access_callback(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {

    int rc, lua_rc;
    uint8_t lua_result[512]; 
    size_t lua_result_len = 0;
    char svc_uuid[37];
    char chr_uuid[37];
    char_context_t *ctx = NULL;

    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
            ctx = (char_context_t *)arg;
            // Get the service UUID via service_index
            ble_uuid_to_str_no_0x_prefix(ble_server->services[ctx->service_index].uuid, svc_uuid);
            ble_uuid_to_str_no_0x_prefix(ble_server->services[ctx->service_index].characteristics[ctx->char_index].uuid, chr_uuid);

            web_ble_sim_trace_emit_chr(conn_handle, svc_uuid, chr_uuid, "read", NULL, 0);

            if (ctx->has_dynamic && strlen(ctx->dynamic.on_read) > 0) {
                bool is_remote_relay = ctx->dynamic.on_read[0] == 'R';
                const char *lua_func = NULL;
                uint8_t *bin = NULL;
                size_t bin_len = 0;

                if (is_remote_relay) {
                    // Parse "R:optionalLuaFunc" format
                    char *colon_pos = strchr(ctx->dynamic.on_read + 1, ':');
                    if (colon_pos) lua_func = colon_pos + 1;

                    ESP_LOGI(TAG, "Read via relay svc: %s, chr: %s", svc_uuid, chr_uuid);

                    char *remote_hex = NULL;
                    esp_err_t relay_rc = sim_relay_read_sync(svc_uuid, chr_uuid, &remote_hex, 3000); // hardcoded timeout 3s for starters
                    if (relay_rc != ESP_OK || !remote_hex) {
                        ESP_LOGW(TAG, "Remote relay read failed: %d", relay_rc);
                        return BLE_ATT_ERR_INSUFFICIENT_RES;
                    }

                    // Hex → binary
                    bin_len = strlen(remote_hex) / 2;
                    bin = malloc(bin_len);
                    for (size_t i = 0; i < bin_len; i++)
                        sscanf(remote_hex + i * 2, "%02hhx", &bin[i]);
                    free(remote_hex);
                } else {
                    lua_func = ctx->dynamic.on_read;
                    bin = ctx->data;
                    bin_len = ctx->data_len;
                }

                // Unified Lua call (or direct data append if no Lua func)
                if (lua_func && strlen(lua_func) > 0) {
                    ESP_LOGI(TAG, "Calling Lua handler: %s", lua_func);
                    lua_result_len = sizeof(lua_result);
                    lua_rc = lua_call_stateful(lua_func, bin, bin_len, lua_result, &lua_result_len, sizeof(lua_result));

                    if (is_remote_relay) free(bin);  // only free heap-alloc'd relay buffer

                    if (lua_rc == 0 && lua_result_len > 0) {
                        rc = os_mbuf_append(ctxt->om, lua_result, lua_result_len);
                        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
                    } else {
                        if (is_remote_relay) return BLE_ATT_ERR_UNLIKELY;  // no fallback for relay
                        ESP_LOGW(TAG, "Lua handler failed, using static data");
                    }
                } else {
                    rc = os_mbuf_append(ctxt->om, bin, bin_len);
                    if (is_remote_relay) free(bin);  // only free heap-alloc'd relay buffer
                    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
                }
            }

            // Fallback to static data
            rc = os_mbuf_append(ctxt->om, ctx->data, ctx->data_len);
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            
        case BLE_GATT_ACCESS_OP_READ_DSC:
            ESP_LOGI(TAG, "Read desc handle=%d, conn=%d", attr_handle, conn_handle);
            // For descriptors: arg is uint8_t* (length prefix + data)
            // arg here is the raw uint8_t* descriptor storage, NOT char_context_t*
            if (arg != NULL) {
                uint8_t *data_storage = (uint8_t *)arg;
                uint16_t data_len = *(uint16_t *)data_storage;
                uint8_t *data = data_storage + sizeof(uint16_t);
                      
                int rc = os_mbuf_append(ctxt->om, data, data_len);
                return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            return BLE_ATT_ERR_UNLIKELY;

        case BLE_GATT_ACCESS_OP_WRITE_CHR:
            ctx = (char_context_t *)arg;
            // Get the service UUID via service_index
            ble_uuid_to_str_no_0x_prefix(ble_server->services[ctx->service_index].uuid, svc_uuid);
            ble_uuid_to_str_no_0x_prefix(ble_server->services[ctx->service_index].characteristics[ctx->char_index].uuid, chr_uuid);

            ESP_LOGI(TAG, "Write handle=%d, conn=%d", attr_handle, conn_handle);
            uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
            uint8_t write_data[512]; 
            // After flattening, declare the forwarding pointers:
            uint8_t *fwd_data = write_data;
            size_t   fwd_len  = om_len;
            
            rc = ble_hs_mbuf_to_flat(ctxt->om, write_data, om_len, NULL);
            if (rc != 0) {
                return rc;
            }

            web_ble_sim_trace_emit_chr(conn_handle, svc_uuid, chr_uuid, "write",
                                       write_data, om_len);

            // Check if dynamic on_write handler exists
            if (ctx->has_dynamic && strlen(ctx->dynamic.on_write) > 0) {
                bool is_remote_relay = ctx->dynamic.on_write[0] == 'R';
                const char *lua_func = NULL;

                if (is_remote_relay) {
                    // Parse "R:optionalLuaFunc" format
                    char *colon_pos = strchr(ctx->dynamic.on_write + 1, ':');
                    if (colon_pos) lua_func = colon_pos + 1;
                } else {
                   lua_func = ctx->dynamic.on_write; 
                }
            
                // Unified Lua pre-processing (transform before relay or local store)
                if (lua_func && strlen(lua_func) > 0) {
                    ESP_LOGI(TAG, "Calling Lua write handler: %s", lua_func);
                    lua_result_len = sizeof(lua_result);
                    lua_rc = lua_call_stateful(lua_func, write_data, om_len,
                                            lua_result, &lua_result_len, sizeof(lua_result));
                    if (lua_rc == 0 && lua_result_len > 0) {
                        fwd_data = lua_result;
                        fwd_len  = lua_result_len;
                    } else {
                        ESP_LOGW(TAG, "Lua write handler failed");
                        if (is_remote_relay) return BLE_ATT_ERR_UNLIKELY; // no fallback for relay
                        // non-relay: proceed with original write_buf
                    }
                }

                // optionally forward to relay
                if (is_remote_relay) {
                    ESP_LOGI(TAG, "Write via relay svc: %s, chr: %s", svc_uuid, chr_uuid);

                    char *hex = malloc(fwd_len * 2 + 1);
                    if (!hex) return BLE_ATT_ERR_INSUFFICIENT_RES;
                    for (size_t i = 0; i < fwd_len; i++)
                        sprintf(hex + i * 2, "%02x", fwd_data[i]);
                    hex[fwd_len * 2] = '\0';

                    esp_err_t relay_rc = sim_relay_write(svc_uuid, chr_uuid, hex);
                    free(hex);

                    if (relay_rc != ESP_OK) {
                        ESP_LOGW(TAG, "Remote relay write failed: %d", relay_rc);
                        return BLE_ATT_ERR_UNLIKELY;
                    }
                    return ESP_OK;
                }                

                // Local store with (possibly lua-transformed) data
                if (fwd_len > ctx->data_len) {
                    uint8_t *new_data = realloc(ctx->data, fwd_len);
                    if (!new_data) return BLE_ATT_ERR_INSUFFICIENT_RES;
                    ctx->data = new_data;
                }
                memcpy(ctx->data, fwd_data, fwd_len);
                ctx->data_len = fwd_len;

                return ESP_OK;   
            }

            // no dynamic handler - store raw write directly
            if (om_len > ctx->data_len) {
                uint8_t *new_data = realloc(ctx->data, om_len);
                if (new_data == NULL) {
                    return BLE_ATT_ERR_INSUFFICIENT_RES;
                }
                ctx->data = new_data;
            }
            
            memcpy(ctx->data, write_data, om_len);
            ctx->data_len = om_len;
            
            return ESP_OK;
            
        case BLE_GATT_ACCESS_OP_WRITE_DSC: 
            ESP_LOGI(TAG, "Descriptor write: to be implemented");
            return ESP_OK;

        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}



// callback on ATT register
void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];
    char matches[40]; 

    switch (ctxt->op) {
    // register service
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGI(TAG, "registered service %s with handle=%d",
                    ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                    ctxt->svc.handle);
        
        break;
    // register characteristic
    case BLE_GATT_REGISTER_OP_CHR:
        ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf);
        char_context_t *ctx = (char_context_t *)ctxt->chr.chr_def->arg;
        if (ctx != NULL) {
            ctx->val_handle = ctxt->chr.val_handle;
//            ESP_LOGI(TAG, "  Original handle: %d", ctx->original_handle);
//            ESP_LOGI(TAG, "  Original value_handle: %d", ctx->original_value_handle);
            if (ctx->original_value_handle == ctxt->chr.val_handle) {
                snprintf(matches, sizeof(matches), "✔  matches original");
            } else {
                snprintf(matches, sizeof(matches), ANSI_RED"✘  differs from original: %d", ctx->original_value_handle);
            }
        } else {
            ESP_LOGE(TAG, "Original handle numbers not retrieved, this should not happen...");
            snprintf(matches, sizeof(matches), "not sure if it matches original...");
        }
            
        ESP_LOGI(TAG, "registering characteristic %s with def_handle=%d val_handle=%d %s", buf, ctxt->chr.def_handle, ctxt->chr.val_handle, matches);
        break;

    case BLE_GATT_REGISTER_OP_DSC:
        ESP_LOGI(TAG, "registering descriptor %s with handle=%d", ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf), ctxt->dsc.handle);
        break;

    default:
        assert(0);
        break;
    }
}

int gatt_svr_init(struct ble_gatt_svc_def *services)
{
    int rc = 0;

    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;

    rc = ble_gatts_count_cfg(services);
//    SYSINIT_PANIC_ASSERT(rc == 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error count cfg services: %d", rc);
        log_memory_usage("Error gatts count");
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(services);
//    SYSINIT_PANIC_ASSERT(rc == 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error gatts add services: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_start(); // actually register the queued services;
    if (rc != 0) {
        ESP_LOGE(TAG, "Error gatts start: %d", rc);
        return ESP_FAIL;
    }

    return rc;
}
