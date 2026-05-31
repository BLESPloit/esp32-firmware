#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "ble/ble_discovery.h"
#include "ble/ble_scan.h"
#include "ble/ble_sim_internal.h"

#include "ble/device_parser.h"
#include "graphics/graphics.h"
#include "interface/interface_sim.h"
#include "lua/lua_hook.h"
#include "common/storage.h"
#include "common/utils.h"

static const char *TAG = "BLE sim - SMP";

extern device_config_t config; // storage.c
extern ble_server_t *ble_server;

// helper to get text description of pairing errors
// shared with ble_discovery
const char* get_pairing_error_string(int status) {
    // Security Manager (peer) errors - base 0x500
    if (status >= 0x500 && status < 0x600) {
        switch (status - 0x500) {
            case 0x01: return "SM: Passkey entry failed";
            case 0x02: return "SM: OOB not available";
            case 0x03: return "SM: Authentication requirements";
            case 0x04: return "SM: Confirm value failed";
            case 0x05: return "SM: Pairing not supported";
            case 0x06: return "SM: Encryption key size";
            case 0x07: return "SM: Command not supported";
            case 0x08: return "SM: Unspecified reason";
            case 0x09: return "SM: Repeated attempts";
            case 0x0A: return "SM: Invalid parameters";
            case 0x0B: return "SM: DHKey check failed";
            case 0x0C: return "SM: Numeric comparison failed";
            case 0x0D: return "SM: BR/EDR pairing in progress";
            case 0x0E: return "SM: Cross-transport key derivation not allowed";
            default: return "SM: Unknown error";
        }
    }
    
    // Core BLE host errors
    switch (status) {
        case 0: return "Success";
        case 1: return "Retry later";
        case 2: return "Already in progress";
        case 3: return "Invalid parameter";
        case 8: return "No memory";
        case 9: return "Not connected";
        case 13: return "Timeout";
        default: return "Unknown error";
    }
}

// set the peripheral security parameters based on pairing_info parsed from ble.json
void configure_nimble_security_for_peripheral(ble_server_t *server) {
    if (server == NULL || server->pairing_info == NULL) {
        ESP_LOGW(TAG, "No pairing info available");
        return;
    }
  
    ble_pairing_config_t *resp = &server->pairing_info->response;
    
    ble_hs_cfg.sm_io_cap = resp->io_capability;
    ble_hs_cfg.sm_oob_data_flag = resp->oob_data_flag;

    // Apply pairing configuration to NimBLE
    ble_hs_cfg.sm_bonding = (resp->auth_req & BLE_SM_AUTH_REQ_BONDING) ? 1 : 0;
    ble_hs_cfg.sm_mitm = (resp->auth_req & BLE_SM_AUTH_REQ_MITM) ? 1 : 0;
    ble_hs_cfg.sm_sc = (resp->auth_req & BLE_SM_AUTH_REQ_SC) ? 1 : 0;
    ble_hs_cfg.sm_io_cap = resp->io_capability;
    ble_hs_cfg.sm_our_key_dist = resp->resp_key_dist;
    ble_hs_cfg.sm_their_key_dist = resp->init_key_dist;

    
    ESP_LOGI(TAG, "NimBLE security configured: io_cap=%d, bonding=%d, mitm=%d, sc=%d",
             resp->io_capability, ble_hs_cfg.sm_bonding,
             ble_hs_cfg.sm_mitm, ble_hs_cfg.sm_sc);
}



// 
// EVENT HANDLER (redirected from main gap event handler)
// 

int ble_sim_smp_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
      
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(TAG, "PASSKEY_ACTION event; action=%d", 
                 event->passkey.params.action);
        
        struct ble_sm_io pkey = {0};
        
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            // Display passkey to user
            pkey.action = event->passkey.params.action;

            // Set the passkey from ble.json if available
            if (ble_server->pairing_info && ble_server->pairing_info->passkey[0] != '\0')
            {
                ESP_LOGI(TAG, "Our passkey: %s", ble_server->pairing_info->passkey);
                gfx_print_notification_center(ble_server->pairing_info->passkey);
                // convert to dec
                pkey.passkey = (uint32_t)atoi(ble_server->pairing_info->passkey);
            } else {
                ESP_LOGI(TAG, "Passkey not defined, using default '123456'");
                gfx_print_notification_center("PIN: 123456");
                pkey.passkey = 123456; 
            } 

            ESP_LOGI(TAG, "Enter passkey %06lu on the peer device", pkey.passkey);
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io result: %d", rc);
        }
        else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            // Numeric comparison - user should confirm the displayed number matches
            ESP_LOGI(TAG, "Confirm passkey matches on both devices: %06lu",
                     event->passkey.params.numcmp);
            // Accept comparison
            pkey.action = event->passkey.params.action;
            pkey.numcmp_accept = 1; // Set to 0 to reject
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io result: %d", rc);
        }
        else if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
            // Request user to input passkey displayed on peer device
            ESP_LOGI(TAG, "Enter passkey displayed on peer device");
            // static passkey for starters
            pkey.action = event->passkey.params.action;
            pkey.passkey = 123456; 
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io result: %d", rc);
        }
        else if (event->passkey.params.action == BLE_SM_IOACT_OOB) {
            // Out-of-band pairing
            ESP_LOGI(TAG, "OOB pairing requested");
            // Provide OOB data if available
        }
        else if (event->passkey.params.action == BLE_SM_IOACT_NONE) {
            // Just Works pairing - no user interaction needed
            ESP_LOGI(TAG, "Just Works pairing");
        }
        return 0;

        
    case BLE_GAP_EVENT_ENC_CHANGE:
        // Encryption state changed
        ESP_LOGI(TAG, "encryption change event; status=%d", 
                 event->enc_change.status);
        
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        if (rc == 0) {
            ble_print_conn_desc(&desc);
            ESP_LOGI(TAG, "encrypted=%d authenticated=%d bonded=%d",
                     desc.sec_state.encrypted,
                     desc.sec_state.authenticated,
                     desc.sec_state.bonded);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // Peer is attempting to pair but we already have a bond
        ESP_LOGI(TAG, "repeat pairing event; conn_handle=%d",
                 event->repeat_pairing.conn_handle);
        
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            ESP_LOGI(TAG, "deleting old bond and accepting new pairing");
            // Delete the old bond
            ble_gap_unpair(&desc.peer_id_addr);
        }
        // Return RETRY to continue with pairing after deleting bond
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
        // Peer's identity address was resolved
        ESP_LOGI(TAG, "identity resolved; conn_handle=%d",
                 event->identity_resolved.conn_handle);
        
        rc = ble_gap_conn_find(event->identity_resolved.conn_handle, &desc);
        if (rc == 0) {
            ble_print_conn_desc(&desc);
        }
        return 0;

    case BLE_GAP_EVENT_PARING_COMPLETE:
    {
        ESP_LOGI(TAG, "Pairing complete; conn_handle=%d status=%d %s",
                event->pairing_complete.conn_handle,
                event->pairing_complete.status,
                event->pairing_complete.status == 0 ? "(SUCCESS)" : "(FAILED)");
        
        // Get connection descriptor to log security details
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->pairing_complete.conn_handle, &desc);
        
        if (rc == 0 && event->pairing_complete.status == 0) {
            // Pairing succeeded - log security state
            ESP_LOGI(TAG, "Security state: encrypted=%d authenticated=%d "
                        "bonded=%d key_size=%d",
                    desc.sec_state.encrypted,
                    desc.sec_state.authenticated,
                    desc.sec_state.bonded,
                    desc.sec_state.key_size);
            
            // Log peer identity address
            ESP_LOGI(TAG, "Peer identity: %02x:%02x:%02x:%02x:%02x:%02x (type=%d)",
                    desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                    desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                    desc.peer_id_addr.val[1], desc.peer_id_addr.val[0],
                    desc.peer_id_addr.type);
        } else if (event->pairing_complete.status != 0) {
            // Pairing failed - log error details with decoded error
            const char *error_str = get_pairing_error_string(event->pairing_complete.status);
            ESP_LOGE(TAG, "Pairing failed with status: 0x%02x (%s)",
                    event->pairing_complete.status, error_str);
        } else {
            ESP_LOGW(TAG, "Could not retrieve connection descriptor: %d", rc);
        }
        
        return 0;
    }

    default:
        ESP_LOGW(TAG, "unhandled GAP event: %d", event->type);
    }

    return 0;
}