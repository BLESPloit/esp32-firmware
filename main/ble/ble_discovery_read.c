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

#include "common/storage.h"
#include "common/utils.h"
#include "api/web_server.h"
#include "ble/ble_discovery.h"
#include "ble/ble_discovery_internal.h"

static const char *TAG = "BLE discovery - read";

// external info from ble_scanner
extern int scanned_device_count;

static int desc_read_callback(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr,
                             void *arg);
static int char_read_callback(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr,
                             void *arg);

// ── Helpers ────────────────────────────────────────────────── 

const char* get_att_error_string(int error)
{
    switch (error) {
    case BLE_ATT_ERR_INSUFFICIENT_AUTHEN:
        return "Insufficient Authentication (Pairing Required)";
    case BLE_ATT_ERR_INSUFFICIENT_AUTHOR:
        return "Insufficient Authorization";
    case BLE_ATT_ERR_INSUFFICIENT_ENC:
        return "Insufficient Encryption";
    case BLE_ATT_ERR_INSUFFICIENT_KEY_SZ:
        return "Insufficient Key Size";
    case BLE_ATT_ERR_READ_NOT_PERMITTED:
        return "Read Not Permitted";
    case BLE_ATT_ERR_WRITE_NOT_PERMITTED:
        return "Write Not Permitted";
    case BLE_HS_ENOTCONN:
        return "Connection Lost";
    case BLE_HS_ETIMEOUT:
        return "Timeout";
    default:
        return "Unknown Error";
    }
}

// Convert NimBLE host error to raw ATT error code by removing the internally added 0x100
static int get_host_error_to_att(int host_error) {
    if (host_error >= 0x100 && host_error <= 0x1FF) {
        return host_error - 0x100;  // Remove BLE_HS_ERR_ATT_BASE
    }
    return host_error;  // Return as-is if not an ATT error
}

bool is_authentication_required_error(int error)
{
    return (error == BLE_ATT_ERR_INSUFFICIENT_AUTHEN ||
            error == BLE_ATT_ERR_INSUFFICIENT_ENC ||
            error == BLE_ATT_ERR_INSUFFICIENT_KEY_SZ);
}

static bool is_recoverable_error(int error)
{
    // Errors that might be resolved by reconnection
    return (error == BLE_HS_ENOTCONN || 
            error == BLE_HS_ETIMEOUT ||
            error == BLE_ATT_ERR_INSUFFICIENT_AUTHEN ||
            error == BLE_ATT_ERR_INSUFFICIENT_AUTHOR ||
            error == BLE_ATT_ERR_INSUFFICIENT_ENC);
}

static bool is_permanent_error(int error)
{
    // Errors that won't be fixed by retrying
    return (error == BLE_ATT_ERR_READ_NOT_PERMITTED ||
            error == BLE_ATT_ERR_WRITE_NOT_PERMITTED ||
            error == BLE_ATT_ERR_INVALID_HANDLE);
}

// ── Retry logic ────────────────────────────────────────────────── 

static void mark_read_as_failed(discovery_context_t *ctx, uint16_t handle, 
                               int error, bool is_descriptor)
{
    if (!ctx) return;
    
    if (is_descriptor && ctx->current_desc) {
        ctx->current_desc->last_read_error = error;
        ctx->current_desc->read_retry_count++;
        WS_LOGW(TAG, "Descriptor read failed (handle=0x%04x, error=%d, attempt=%d): %s",
                handle, error, ctx->current_desc->read_retry_count, 
                get_att_error_string(error));
    } else if (!is_descriptor && ctx->current_char) {
        ctx->current_char->last_read_error = error;
        ctx->current_char->read_retry_count++;
        WS_LOGW(TAG, "Characteristic read failed (handle=0x%04x, error=%d, attempt=%d): %s",
                handle, error, ctx->current_char->read_retry_count,
                get_att_error_string(error));
    }
}

static bool should_retry_read(discovery_context_t *ctx, uint16_t handle, 
                             int error, bool is_descriptor)
{
    if (!ctx) return false;
    
    uint8_t retry_count = 0;
    
    if (is_descriptor && ctx->current_desc) {
        retry_count = ctx->current_desc->read_retry_count;
    } else if (!is_descriptor && ctx->current_char) {
        retry_count = ctx->current_char->read_retry_count;
    }
    
    // Skip retry for permanent errors
    if (is_permanent_error(error)) {
        WS_LOGW(TAG, "Permanent error, skipping retries for handle 0x%04x", handle);
        return false;
    }
    
    // Retry if under limit and error is recoverable
    if (retry_count < MAX_READ_RETRIES && is_recoverable_error(error)) {
        WS_LOGI(TAG, "Will retry read for handle 0x%04x (attempt %d/%d)",
                handle, retry_count + 1, MAX_READ_RETRIES);
        return true;
    }
    
    WS_LOGW(TAG, "Max retries reached for handle 0x%04x, skipping", handle);
    return false;
}

static void setup_retry(discovery_context_t *ctx, uint16_t handle, 
                       int error, bool is_descriptor)
{
    if (!ctx) return;
    
    ctx->retry_info.handle = handle;
    ctx->retry_info.last_error = error;
    ctx->retry_info.is_descriptor = is_descriptor;
    ctx->retry_in_progress = true;
    
    // IMPORTANT: Save current service so we can restore position after reconnection
    ctx->retry_service = ctx->current_service;
    
    WS_LOGI(TAG, "Setting up retry for %s handle 0x%04x",
            is_descriptor ? "descriptor" : "characteristic", handle);
}


void retry_failed_read(discovery_context_t *ctx)
{
    if (!ctx || !ctx->retry_in_progress) {
        WS_LOGW(TAG, "retry_failed_read called with invalid state");
        return;
    }
    
    uint16_t handle = ctx->retry_info.handle;
    bool is_desc = ctx->retry_info.is_descriptor;
    int rc;
    
    WS_LOGI(TAG, "Retrying read for %s handle 0x%04x...",
            is_desc ? "descriptor" : "characteristic", handle);
    
    ctx->retry_in_progress = false;
    
    if (is_desc) {
        if (!ctx->current_desc) {
            WS_LOGW(TAG, "Current descriptor is NULL, cannot retry");
            desc_read_callback(ctx->conn_handle,
                             &(struct ble_gatt_error){.status = BLE_ATT_ERR_READ_NOT_PERMITTED},
                             NULL, ctx);
            return;
        }
        
        rc = ble_gattc_read(ctx->conn_handle, handle, desc_read_callback, ctx);
        if (rc != 0) {
            WS_LOGE(TAG, "Failed to retry descriptor read: %d", rc);
            desc_read_callback(ctx->conn_handle,
                             &(struct ble_gatt_error){.status = BLE_ATT_ERR_READ_NOT_PERMITTED},
                             NULL, ctx);
        }
    } else {
        // For characteristics, we need to restore position
        
        // First, restore the service context
        if (ctx->retry_service) {
            ctx->current_service = ctx->retry_service;
            
            // Find the characteristic with the retry handle
            ctx->current_char = ctx->current_service->characteristics;
            while (ctx->current_char) {
                if (ctx->current_char->val_handle == handle) {
                    break;
                }
                ctx->current_char = ctx->current_char->next;
            }
        }
        
        if (!ctx->current_char) {
            WS_LOGE(TAG, "Could not find characteristic handle 0x%04x after reconnection", handle);
            // Start from beginning of current service
            if (ctx->current_service && ctx->current_service->characteristics) {
                ctx->current_char = ctx->current_service->characteristics;
            } else {
                WS_LOGE(TAG, "Cannot recover position, skipping to next service");
                char_read_callback(ctx->conn_handle,
                                 &(struct ble_gatt_error){.status = 0},
                                 NULL, ctx);
                return;
            }
        }
        
        // Check if we've exceeded retries
        if (ctx->current_char->read_retry_count >= MAX_READ_RETRIES) {
            WS_LOGW(TAG, "Max retries already reached for handle 0x%04x, skipping to next", handle);
            
            // Mark as failed and move to next characteristic
            mark_read_as_failed(ctx, ctx->current_char->val_handle, 
                              BLE_ATT_ERR_READ_NOT_PERMITTED, false);
            
            // Move to next readable characteristic
            ctx->current_char = ctx->current_char->next;
            
            // Manually trigger the "find next readable" logic
            while (ctx->current_char) {
                if (ctx->current_char->properties & BLE_GATT_CHR_PROP_READ) {
                    rc = ble_gattc_read(ctx->conn_handle, ctx->current_char->val_handle,
                                      char_read_callback, ctx);
                    if (rc != 0) {
                        ESP_LOGE(TAG, "Failed to read next characteristic: %d", rc);
                        discovery_complete(ctx, rc);
                    }
                    return;
                }
                ctx->current_char = ctx->current_char->next;
            }
            
            // No more readable chars in this service - continue with next service
            ESP_LOGI(TAG, "No more readable characteristics in current service after skip");
            char_read_callback(ctx->conn_handle,
                             &(struct ble_gatt_error){.status = 0},
                             NULL, ctx);
            return;
        }
        
        // Try the read again
        rc = ble_gattc_read(ctx->conn_handle, handle, char_read_callback, ctx);
        if (rc != 0) {
            WS_LOGE(TAG, "Failed to retry characteristic read: %d", rc);
            char_read_callback(ctx->conn_handle,
                             &(struct ble_gatt_error){.status = BLE_ATT_ERR_READ_NOT_PERMITTED},
                             NULL, ctx);
        }
    }
}



// ── Value reading with retry ────────────────────────────────────────────────── 

static int char_read_callback(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr,
                             void *arg)
{
    discovery_context_t *ctx = (discovery_context_t *)arg;
    int rc = 0;
    
    if (!ctx) {
        WS_LOGE(TAG, "char_read_callback: NULL context!");
        return BLE_HS_EINVAL;
    }

    // Convert NimBLE host error to ATT protocol error for cleaner handling
    int att_error = get_host_error_to_att(error->status);
    
    // Check for authentication errors
    if (is_authentication_required_error(att_error)) {
        WS_LOGW(TAG, "Characteristic requires authentication: handle=0x%04x, error=0x%02x (%s)", 
                ctx->current_char ? ctx->current_char->val_handle : 0,
                att_error, get_att_error_string(att_error));

        if (ctx->current_char) {
            ctx->current_char->encryption_required = true;
            ctx->current_char->last_read_error = att_error;
            ctx->any_auth_required = true;  // Mark that we need pairing
            
            WS_LOGI(TAG, "Marking characteristic as requiring auth, will attempt pairing later");
        }
        goto move_to_next;
    }

    // Check for disconnection
    if (error->status == BLE_HS_ENOTCONN) {
        WS_LOGW(TAG, "Connection lost during characteristic read");
        
        if (!ctx->current_char) {
            WS_LOGW(TAG, "Connection lost but no current characteristic");
            goto error;
        }
        
        if (should_retry_read(ctx, ctx->current_char->val_handle, error->status, false)) {
            mark_read_as_failed(ctx, ctx->current_char->val_handle, error->status, false);
            ctx->disconnected_during_read = true;
            setup_retry(ctx, ctx->current_char->val_handle, error->status, false);
            
            ESP_LOGI(TAG, "Will attempt reconnection and retry...");
            vTaskDelay(pdMS_TO_TICKS(ctx->reconnect_delay_ms));
            reconnect_and_resume(ctx);
            return 0;
        } else {
            WS_LOGE(TAG, "Connection lost and max retries reached");
            goto error;
        }
    }
    
    // Handle other read errors
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        WS_LOGW(TAG, "Characteristic read error: %d - %s", 
                error->status, get_att_error_string(error->status));
        
        if (!ctx->current_char) {
            WS_LOGW(TAG, "Read error but current_char is NULL, cannot retry");
            goto move_to_next;
        }
        
        if (should_retry_read(ctx, ctx->current_char->val_handle, error->status, false)) {
            mark_read_as_failed(ctx, ctx->current_char->val_handle, error->status, false);
            
            if (is_recoverable_error(error->status)) {
                setup_retry(ctx, ctx->current_char->val_handle, error->status, false);
                vTaskDelay(pdMS_TO_TICKS(100));
                retry_failed_read(ctx);
                return 0;
            }
        } else {
            mark_read_as_failed(ctx, ctx->current_char->val_handle, error->status, false);
            WS_LOGW(TAG, "Skipping characteristic handle 0x%04x after %d attempts",
                    ctx->current_char->val_handle, ctx->current_char->read_retry_count);
        }
    }
    
    // Successful read
    if (error->status == 0 && attr && ctx->current_char) {
        ctx->current_char->value_hex = mbuf_to_hex_string(attr->om);
        if (ctx->current_char->value_hex) {
            ESP_LOGI(TAG, "  Read value: %s", ctx->current_char->value_hex);
        }
        ctx->current_char->read_retry_count = 0;
        ctx->current_char->last_read_error = 0;
    }
    
move_to_next:
    // Move to next characteristic in current service
    if (ctx->current_char) {
        ctx->current_char = ctx->current_char->next;
    }
    
    // Find next readable characteristic in current service
    while (ctx->current_char) {
        if (ctx->current_char->properties & BLE_GATT_CHR_PROP_READ) {
            rc = ble_gattc_read(conn_handle, ctx->current_char->val_handle,
                              char_read_callback, ctx);
            if (rc != 0) {
                WS_LOGE(TAG, "Failed to read next characteristic: %d", rc);
                goto error;
            }
            return 0;
        }
        ctx->current_char = ctx->current_char->next;
    }
    
    // No more readable characteristics in current service - move to next service
    if (ctx->current_service) {
        ctx->current_service = ctx->current_service->next;
    }
    
    // Search through all remaining services for readable characteristics
    while (ctx->current_service) {
        ESP_LOGD(TAG, "Moving to next service for characteristic reading");
        
        if (ctx->current_service->characteristics) {
            ctx->current_char = ctx->current_service->characteristics;
            
            // Find first readable characteristic in this service
            while (ctx->current_char) {
                if (ctx->current_char->properties & BLE_GATT_CHR_PROP_READ) {
                    rc = ble_gattc_read(conn_handle, ctx->current_char->val_handle,
                                      char_read_callback, ctx);
                    if (rc != 0) {
                        WS_LOGE(TAG, "Failed to read characteristic in service: %d", rc);
                        goto error;
                    }
                    return 0;
                }
                ctx->current_char = ctx->current_char->next;
            }
        }
        
        ctx->current_service = ctx->current_service->next;
    }
    
    // All characteristics read - check if we need pairing
    WS_LOGI(TAG, "All characteristic values processed (first pass)");
    
    if (ctx->any_auth_required && ctx->pairing_mode != PAIRING_MODE_NONE && !ctx->pairing_info.pairing_attempted) {
        WS_LOGI(TAG, "Some values require authentication, initiating pairing phase...");
        web_broadcast_connection_progress_reading(ctx, "need pairing");
        ctx->phase = DISC_PHASE_PAIRING;
        start_pairing_phase(ctx);
    } else {
        WS_LOGI(TAG, "No authentication required or pairing disabled, completing discovery");
        web_broadcast_connection_progress_reading(ctx, "complete");
        ctx->phase = DISC_PHASE_COMPLETE;
        discovery_complete(ctx, 0);
    }
    
    return 0;
    
error:
    discovery_complete(ctx, rc);
    return rc;
}


// Read descriptor values (mostly 0x2901)
static int desc_read_callback(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr,
                             void *arg)
{
    discovery_context_t *ctx = (discovery_context_t *)arg;
    int rc;
    
    if (!ctx) {
        WS_LOGE(TAG, "desc_read_callback: NULL context!");
        return BLE_HS_EINVAL;
    }

    // Convert NimBLE host error to ATT protocol error for cleaner handling
    int att_error = get_host_error_to_att(error->status);
    
    // Check for authentication errors
    if (is_authentication_required_error(att_error)) {
        WS_LOGW(TAG, "Descriptor requires authentication: handle=0x%04x, error=0x%02x (%s)", 
                ctx->current_desc ? ctx->current_desc->handle : 0,
                att_error, get_att_error_string(att_error));

        web_broadcast_connection_progress_reading(ctx, "authentication required");

        if (ctx->current_desc) {
            ctx->current_desc->encryption_required = true;
            ctx->current_desc->last_read_error = att_error;
            ctx->any_auth_required = true;  // Mark that we need pairing
            
            ESP_LOGI(TAG, "Marking descriptor as requiring auth, will attempt pairing later");
        }
        goto move_to_next;
    }
    
    // Check for disconnection
    if (error->status == BLE_HS_ENOTCONN) {
        WS_LOGE(TAG, "Connection lost during descriptor read");
        web_broadcast_connection_progress_reading(ctx, "connection lost during read");
        
        if (ctx->current_desc && should_retry_read(ctx, ctx->current_desc->handle, error->status, true)) {
            mark_read_as_failed(ctx, ctx->current_desc->handle, error->status, true);
            setup_retry(ctx, ctx->current_desc->handle, error->status, true);
            ctx->disconnected_during_read = true;
            
            // Attempt reconnection
            reconnect_and_resume(ctx);
            return 0;
        } else {
            WS_LOGW(TAG, "Giving up on descriptor handle 0x%04x", 
                    ctx->current_desc ? ctx->current_desc->handle : 0);
        }
    }
    
    // Handle other read errors
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        WS_LOGW(TAG, "Descriptor read error: %d - %s", 
                error->status, get_att_error_string(error->status));
        
        if (ctx->current_desc && should_retry_read(ctx, ctx->current_desc->handle, error->status, true)) {
            mark_read_as_failed(ctx, ctx->current_desc->handle, error->status, true);
            
            if (is_recoverable_error(error->status)) {
                setup_retry(ctx, ctx->current_desc->handle, error->status, true);
                vTaskDelay(pdMS_TO_TICKS(100));
                retry_failed_read(ctx);
                return 0;
            }
        } else if (ctx->current_desc) {
            mark_read_as_failed(ctx, ctx->current_desc->handle, error->status, true);
            WS_LOGW(TAG, "Skipping descriptor handle 0x%04x after %d attempts",
                    ctx->current_desc->handle, ctx->current_desc->read_retry_count);
        }
    }
    
    // Successful read
    if (error->status == 0 && attr && ctx->current_desc) {
        ctx->current_desc->value_hex = mbuf_to_hex_string(attr->om);
        if (ctx->current_desc->value_hex) {
            ESP_LOGI(TAG, "  Read desc value: %s", ctx->current_desc->value_hex);
        }
        ctx->current_desc->read_retry_count = 0;
        ctx->current_desc->last_read_error = 0;
    }
    
move_to_next:
    // Move to next descriptor
    if (ctx->current_desc) {
        ctx->current_desc = ctx->current_desc->next;
    }
    
    // Find next 0x2901 descriptor to read in current characteristic
    while (ctx->current_desc) {
        if (ctx->current_desc->uuid.u16.u.type == BLE_UUID_TYPE_16 && 
            ctx->current_desc->uuid.u16.value == 0x2901) {
            rc = ble_gattc_read(conn_handle, ctx->current_desc->handle, desc_read_callback, ctx);
            if (rc != 0) {
                goto error;
            }
            return 0;
        }
        ctx->current_desc = ctx->current_desc->next;
    }
    
    // Move to next characteristic
    if (ctx->current_char) {
        ctx->current_char = ctx->current_char->next;
    }
    
    // Find next characteristic with descriptors
    while (ctx->current_char) {
        if (ctx->current_char->descriptors) {
            ctx->current_desc = ctx->current_char->descriptors;
            
            // Search for 0x2901 descriptors in this characteristic
            while (ctx->current_desc) {
                if (ctx->current_desc->uuid.u16.u.type == BLE_UUID_TYPE_16 && 
                    ctx->current_desc->uuid.u16.value == 0x2901) {
                    rc = ble_gattc_read(conn_handle, ctx->current_desc->handle, 
                                      desc_read_callback, ctx);
                    if (rc != 0) {
                        goto error;
                    }
                    return 0;
                }
                ctx->current_desc = ctx->current_desc->next;
            }
        }
        ctx->current_char = ctx->current_char->next;
    }
    
    // Move to next service
    if (ctx->current_service) {
        ctx->current_service = ctx->current_service->next;
    }
    
    // Search through remaining services
    while (ctx->current_service) {
        if (ctx->current_service->characteristics) {
            ctx->current_char = ctx->current_service->characteristics;
            
            // Search through characteristics in this service
            while (ctx->current_char) {
                if (ctx->current_char->descriptors) {
                    ctx->current_desc = ctx->current_char->descriptors;
                    
                    // Search for 0x2901 descriptors
                    while (ctx->current_desc) {
                        if (ctx->current_desc->uuid.u16.u.type == BLE_UUID_TYPE_16 && 
                            ctx->current_desc->uuid.u16.value == 0x2901) {
                            rc = ble_gattc_read(conn_handle, ctx->current_desc->handle, 
                                              desc_read_callback, ctx);
                            if (rc != 0) {
                                goto error;
                            }
                            return 0;
                        }
                        ctx->current_desc = ctx->current_desc->next;
                    }
                }
                ctx->current_char = ctx->current_char->next;
            }
        }
        ctx->current_service = ctx->current_service->next;
    }
    
    // All descriptor values read, now read characteristics
    ESP_LOGI(TAG, "All descriptor values processed, reading characteristics...");
    
    // Reset to beginning for characteristic reading
    ctx->current_service = ctx->services;
    ctx->current_char = ctx->current_service ? ctx->current_service->characteristics : NULL;
    
    // Find first readable characteristic
    while (ctx->current_service) {
        while (ctx->current_char) {
            if (ctx->current_char->properties & BLE_GATT_CHR_PROP_READ) {
                rc = ble_gattc_read(conn_handle, ctx->current_char->val_handle,
                                  char_read_callback, ctx);
                if (rc != 0) {
                    goto error;
                }
                return 0;
            }
            ctx->current_char = ctx->current_char->next;
        }
        
        ctx->current_service = ctx->current_service->next;
        if (ctx->current_service) {
            ctx->current_char = ctx->current_service->characteristics;
        }
    }
    
    // No readable characteristics found - check if pairing is needed
    ESP_LOGI(TAG, "All descriptor values processed, no readable characteristics");
    
    if (ctx->any_auth_required && ctx->pairing_mode != PAIRING_MODE_NONE && !ctx->pairing_info.pairing_attempted) {
        WS_LOGI(TAG, "Some values require authentication, initiating pairing phase...");
        ctx->phase = DISC_PHASE_PAIRING;
        web_broadcast_connection_progress_reading(ctx, "handing over to pairing");
        start_pairing_phase(ctx);
    } else {
        WS_LOGI(TAG, "No authentication required or pairing disabled, completing discovery");
        ctx->phase = DISC_PHASE_COMPLETE;
        discovery_complete(ctx, 0);
    }
    
    return 0;
    
error:
    discovery_complete(ctx, rc);
    return rc;
}


// Start the process of reading values (to be called after service discovery completion)
void start_value_reading(discovery_context_t *ctx)
{
    if (!ctx || !ctx->services) {
        discovery_complete(ctx, BLE_HS_EUNKNOWN);
        return;
    }
    
    ESP_LOGI(TAG, "Starting value reading phase...");
    ctx->phase = DISC_PHASE_READING_VALUES;
    
    // Start by reading descriptor values (0x2901 descriptors)
    // Search through ALL services and characteristics to find first 0x2901 descriptor
    ctx->current_service = ctx->services;
    
    while (ctx->current_service) {
        ctx->current_char = ctx->current_service->characteristics;
        
        while (ctx->current_char) {
            if (ctx->current_char->descriptors) {
                // Search for first 0x2901 descriptor in this characteristic
                stored_descriptor_t *desc = ctx->current_char->descriptors;
                
                while (desc) {
                    if (desc->uuid.u16.u.type == BLE_UUID_TYPE_16 &&
                        desc->uuid.u16.value == 0x2901) {
                        // Found first 0x2901 descriptor - start reading here
                        ctx->current_desc = desc;
                        int rc = ble_gattc_read(ctx->conn_handle, desc->handle,
                                               desc_read_callback, ctx);
                        if (rc != 0) {
                            ESP_LOGE(TAG, "Failed to start descriptor read: %d", rc);
                            discovery_complete(ctx, rc);
                        }
                        return;
                    }
                    desc = desc->next;
                }
                // This characteristic has descriptors but no 0x2901
                // Continue to next characteristic
            }
            ctx->current_char = ctx->current_char->next;
        }
        
        ctx->current_service = ctx->current_service->next;
    }
    
    // No 0x2901 descriptors found, skip to characteristic reading
    ESP_LOGI(TAG, "No 0x2901 descriptors found, proceeding to characteristic reading...");
    ctx->current_service = ctx->services;
    ctx->current_char = ctx->current_service ? ctx->current_service->characteristics : NULL;
    
    // Find first readable characteristic
    while (ctx->current_service) {
        while (ctx->current_char) {
            if (ctx->current_char->properties & BLE_GATT_CHR_PROP_READ) {
                int rc = ble_gattc_read(ctx->conn_handle, ctx->current_char->val_handle,
                                       char_read_callback, ctx);
                if (rc != 0) {
                    discovery_complete(ctx, rc);
                }
                return;
            }
            ctx->current_char = ctx->current_char->next;
        }
        
        ctx->current_service = ctx->current_service->next;
        if (ctx->current_service) {
            ctx->current_char = ctx->current_service->characteristics;
        }
    }
    
    // No readable data found
    ESP_LOGI(TAG, "No readable data found");
    ctx->phase = DISC_PHASE_COMPLETE;
    discovery_complete(ctx, 0);
}
