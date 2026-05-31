#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_event.h"
#include "cJSON.h"

// #include "ble.h"
// #include "ble_scanner.h"
#include "common/storage.h"
#include "common/utils.h"
#include "api/web_server.h"
#include "ble/ble_discovery.h"
#include "ble/ble_discovery_internal.h"

static const char *TAG = "BLE discovery - GATT";

static void start_characteristic_discovery(discovery_context_t *ctx);
static void start_descriptor_discovery(discovery_context_t *ctx);

// ── Memory management ────────────────────────────────────────────────── 

static stored_descriptor_t* create_descriptor(uint16_t handle, const ble_uuid_t *uuid)
{
    stored_descriptor_t *desc = calloc(1, sizeof(stored_descriptor_t));
    if (desc) {
        desc->handle = handle;
        memcpy(&desc->uuid, uuid, sizeof(ble_uuid_any_t));
        desc->value_hex = NULL;
        desc->encryption_required = false;
        desc->read_retry_count = 0;
        desc->last_read_error = 0;
        desc->next = NULL;
    }
    return desc;
}

static stored_characteristic_t* create_characteristic(const struct ble_gatt_chr *chr)
{
    stored_characteristic_t *c = calloc(1, sizeof(stored_characteristic_t));
    if (c) {
        c->def_handle = chr->def_handle;
        c->val_handle = chr->val_handle;
        memcpy(&c->uuid, &chr->uuid, sizeof(ble_uuid_any_t));
        c->properties = chr->properties;
        c->value_hex = NULL;
        c->descriptors = NULL;
        c->encryption_required = false;
        c->read_retry_count = 0;
        c->last_read_error = 0;
        c->next = NULL;
    }
    return c;
}

static stored_service_t* create_service(const struct ble_gatt_svc *svc)
{
    stored_service_t *s = calloc(1, sizeof(stored_service_t));
    if (s) {
        s->start_handle = svc->start_handle;
        s->end_handle = svc->end_handle;
        memcpy(&s->uuid, &svc->uuid, sizeof(ble_uuid_any_t));
        s->characteristics = NULL;
        s->next = NULL;
    }
    return s;
}

static void add_descriptor_to_char(stored_characteristic_t *chr, stored_descriptor_t *desc)
{
    if (!chr || !desc) return;
    
    if (!chr->descriptors) {
        chr->descriptors = desc;
    } else {
        stored_descriptor_t *last = chr->descriptors;
        while (last->next) last = last->next;
        last->next = desc;
    }
}

static void add_char_to_service(stored_service_t *svc, stored_characteristic_t *chr)
{
    if (!svc || !chr) return;
    
    if (!svc->characteristics) {
        svc->characteristics = chr;
    } else {
        stored_characteristic_t *last = svc->characteristics;
        while (last->next) last = last->next;
        last->next = chr;
    }
}

static void add_service_to_context(discovery_context_t *ctx, stored_service_t *svc)
{
    if (!ctx || !svc) return;
    
    if (!ctx->services) {
        ctx->services = svc;
    } else {
        stored_service_t *last = ctx->services;
        while (last->next) last = last->next;
        last->next = svc;
    }
}

// ── Service discovery ────────────────────────────────────────────────── 

static int service_disc_callback(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                const struct ble_gatt_svc *service,
                                void *arg)
{
    discovery_context_t *ctx = (discovery_context_t *)arg;
    char uuid_str[37];
    int rc;
    
    switch (error->status) {
    case 0:
        if (service) {
            stored_service_t *svc = create_service(service);
            if (svc) {
                add_service_to_context(ctx, svc);
                ble_uuid_to_str_no_0x_prefix(&service->uuid, uuid_str);
                ESP_LOGI(TAG, "Service: uuid=%s start=0x%04x end=0x%04x",
                        uuid_str, service->start_handle, service->end_handle);
            } else {
                WS_LOGW(TAG, "Not enough memory to create service...");
                rc = BLE_HS_ENOMEM;
                goto error;
            }
        }
        break;
        
    case BLE_HS_EDONE:
        ESP_LOGI(TAG, "Service discovery complete");
        ctx->phase = DISC_PHASE_CHARACTERISTICS;
        web_broadcast_connection_progress_discovery(ctx, "services discovered");        
        start_characteristic_discovery(ctx);
        break;

    case BLE_HS_ENOTCONN:
        // Transient connection loss during service discovery —
        // NimBLE is auto-reattempting (reason 0x3e). Hold the context;
        // the GAP CONNECT event will call start_service_discovery() again.
        WS_LOGW(TAG, "Service discovery: connection lost (ENOTCONN), deferring to GAP handler");
        return 0;
        
    default:
        rc = error->status;
        goto error;
    }
    
    return 0;
    
error:
    discovery_complete(ctx, rc);
    return rc;
}

void start_service_discovery(discovery_context_t *ctx)
{
    if (!ctx) return;

    // Guard against double-call (e.g. fresh connect + reuse path both firing)
    if (ctx->phase != DISC_PHASE_SERVICES) {
        ESP_LOGW(TAG, "start_service_discovery called in wrong phase (%d), ignoring", ctx->phase);
        return;
    }
    
    // Clear any partial results from a previous interrupted attempt
    // TBD: implement the followup from the interrrupted attempt so that we don't need to start from scratch
    if (ctx->services) {
        free_services(ctx->services);
        ctx->services = NULL;
        ctx->current_service = NULL;
        ctx->current_char = NULL;
        ctx->current_desc = NULL;
    }

    ESP_LOGI(TAG, "Starting service discovery...");
    ctx->phase = DISC_PHASE_SERVICES;
    
    int rc = ble_gattc_disc_all_svcs(ctx->conn_handle, service_disc_callback, ctx);

    if (rc != 0) {
        WS_LOGE(TAG, "Failed to start service discovery: %d", rc);
//        web_broadcast_connection_progress_discovery(ctx, "error: failed to start discovery, disconnecting!");
        discovery_complete(ctx, rc);
    }
}

// ── Characteristic discovery ────────────────────────────────────────────────── 

static int char_disc_callback(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr,
                             void *arg)
{
    discovery_context_t *ctx = (discovery_context_t *)arg;
    char uuid_str[37];
    int rc;
    
    switch (error->status) {
    case 0:
        if (chr && ctx->current_service) {
            stored_characteristic_t *c = create_characteristic(chr);
            if (c) {
                add_char_to_service(ctx->current_service, c);
                ble_uuid_to_str_no_0x_prefix(&chr->uuid, uuid_str);
                ESP_LOGI(TAG, "  Char: handle=0x%04x val=0x%04x uuid=%s props=0x%02x",
                        chr->def_handle, chr->val_handle, uuid_str, chr->properties);
            } else {
                rc = BLE_HS_ENOMEM;
                goto error;
            }
        }
        break;
        
    case BLE_HS_EDONE:
        // Move to next service
        if (ctx->current_service) {
            ctx->current_service = ctx->current_service->next;
        }
        
        if (ctx->current_service) {
            // Discover characteristics for next service
            rc = ble_gattc_disc_all_chrs(conn_handle,
                                        ctx->current_service->start_handle,
                                        ctx->current_service->end_handle,
                                        char_disc_callback, ctx);
            if (rc != 0) goto error;
        } else {
            // All characteristics discovered
            ESP_LOGI(TAG, "All characteristics discovered");
            ctx->phase = DISC_PHASE_DESCRIPTORS;
            web_broadcast_connection_progress_discovery(ctx, "characteristics discovered");
            start_descriptor_discovery(ctx);
        }
        break;

    case BLE_HS_ENOTCONN:
        ESP_LOGW(TAG, "Char discovery: connection lost, deferring to GAP handler");
        return 0;
        
    default:
        rc = error->status;
        goto error;
    }
    
    return 0;
    
error:
    discovery_complete(ctx, rc);
    return rc;
}

static void start_characteristic_discovery(discovery_context_t *ctx)
{
    if (!ctx || !ctx->services) {
        discovery_complete(ctx, BLE_HS_EUNKNOWN);
        return;
    }
    
    ESP_LOGI(TAG, "Starting characteristic discovery...");
    ctx->phase = DISC_PHASE_CHARACTERISTICS;
    ctx->current_service = ctx->services;
    
    int rc = ble_gattc_disc_all_chrs(ctx->conn_handle,
                                    ctx->current_service->start_handle,
                                    ctx->current_service->end_handle,
                                    char_disc_callback, ctx);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start characteristic discovery: %d", rc);
        discovery_complete(ctx, rc);
    }
}

// ── Descriptor discovery ────────────────────────────────────────────────── 

static uint16_t get_char_end_handle(stored_service_t *svc, stored_characteristic_t *chr)
{
    if (!svc || !chr) return 0;
    
    if (chr->next) {
        return chr->next->def_handle - 1;
    }
    return svc->end_handle;
}

static bool char_has_descriptor_space(stored_service_t *svc, stored_characteristic_t *chr)
{
    if (!svc || !chr) return false;
    uint16_t end_handle = get_char_end_handle(svc, chr);
    return end_handle > chr->val_handle;
}

static int desc_disc_callback(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             uint16_t chr_val_handle,
                             const struct ble_gatt_dsc *dsc,
                             void *arg)
{
    discovery_context_t *ctx = (discovery_context_t *)arg;
    char uuid_str[37];
    int rc;
    
    switch (error->status) {
    case 0:
        if (dsc && ctx->current_char) {
            stored_descriptor_t *d = create_descriptor(dsc->handle, &dsc->uuid.u);
            if (d) {
                add_descriptor_to_char(ctx->current_char, d);
                ble_uuid_to_str_no_0x_prefix(&dsc->uuid.u, uuid_str);
                ESP_LOGI(TAG, "    Desc: handle=0x%04x uuid=%s", dsc->handle, uuid_str);
            } else {
                rc = BLE_HS_ENOMEM;
                goto error;
            }
        }
        break;
        
    case BLE_HS_EDONE:
    case BLE_ATT_ERR_ATTR_NOT_FOUND:
        // Move to next characteristic
        if (ctx->current_char) {
            ctx->current_char = ctx->current_char->next;
        }
        
        // Find next characteristic with descriptor space
        while (ctx->current_char && 
               !char_has_descriptor_space(ctx->current_service, ctx->current_char)) {
            ctx->current_char = ctx->current_char->next;
        }
        
        if (ctx->current_char) {
            // Discover descriptors for this characteristic
            uint16_t end_handle = get_char_end_handle(ctx->current_service, ctx->current_char);
            rc = ble_gattc_disc_all_dscs(conn_handle,
                                        ctx->current_char->val_handle,
                                        end_handle,
                                        desc_disc_callback, ctx);
            if (rc != 0) goto error;
        } else {
            // Move to next service
            ctx->current_service = ctx->current_service->next;
            
            if (ctx->current_service) {
                // Start descriptor discovery for next service
                ctx->current_char = ctx->current_service->characteristics;
                
                // Find first characteristic with descriptor space
                while (ctx->current_char && 
                       !char_has_descriptor_space(ctx->current_service, ctx->current_char)) {
                    ctx->current_char = ctx->current_char->next;
                }
                
                if (ctx->current_char) {
                    uint16_t end_handle = get_char_end_handle(ctx->current_service, ctx->current_char);
                    rc = ble_gattc_disc_all_dscs(conn_handle,
                                                ctx->current_char->val_handle,
                                                end_handle,
                                                desc_disc_callback, ctx);
                    if (rc != 0) goto error;
                } else {
                    // No characteristics with descriptor space in this service
                    desc_disc_callback(conn_handle, 
                                     &(struct ble_gatt_error){.status = BLE_HS_EDONE},
                                     0, NULL, ctx);
                }
            } else {
                // All descriptors discovered
                ESP_LOGI(TAG, "All descriptors discovered");
                web_broadcast_connection_progress_discovery(ctx, "descriptors discovered");
                
                if (ctx->read_values) {
                    ctx->phase = DISC_PHASE_READING_VALUES;
                    start_value_reading(ctx);
                } else {
                    ctx->phase = DISC_PHASE_COMPLETE;
                    discovery_complete(ctx, 0);
                }
            }
        }
        break;

    case BLE_HS_ENOTCONN:
        ESP_LOGW(TAG, "Desc discovery: connection lost, deferring to GAP handler");
        return 0;

    default:
        rc = error->status;
        goto error;
    }
    
    return 0;
    
error:
    discovery_complete(ctx, rc);
    return rc;
}

static void start_descriptor_discovery(discovery_context_t *ctx)
{
    if (!ctx || !ctx->services) {
        discovery_complete(ctx, BLE_HS_EUNKNOWN);
        return;
    }
    
    ESP_LOGI(TAG, "Starting descriptor discovery...");
    ctx->phase = DISC_PHASE_DESCRIPTORS;
    ctx->current_service = ctx->services;
    
    if (!ctx->current_service->characteristics) {
        // No characteristics, move to next service
        desc_disc_callback(ctx->conn_handle,
                         &(struct ble_gatt_error){.status = BLE_HS_EDONE},
                         0, NULL, ctx);
        return;
    }
    
    ctx->current_char = ctx->current_service->characteristics;
    
    // Find first characteristic with descriptor space
    while (ctx->current_char && 
           !char_has_descriptor_space(ctx->current_service, ctx->current_char)) {
        ctx->current_char = ctx->current_char->next;
    }
    
    if (!ctx->current_char) {
        // No characteristics with descriptor space
        desc_disc_callback(ctx->conn_handle,
                         &(struct ble_gatt_error){.status = BLE_HS_EDONE},
                         0, NULL, ctx);
        return;
    }
    
    uint16_t end_handle = get_char_end_handle(ctx->current_service, ctx->current_char);
    int rc = ble_gattc_disc_all_dscs(ctx->conn_handle,
                                    ctx->current_char->val_handle,
                                    end_handle,
                                    desc_disc_callback, ctx);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start descriptor discovery: %d", rc);
        discovery_complete(ctx, rc);
    }
}
