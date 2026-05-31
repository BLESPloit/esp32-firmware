#include "host/ble_store.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

static const char *TAG = "BLE bond";


static void print_sec_hex_data(const char* label, const uint8_t* data, size_t len)
{
    char hex_str[len * 3 + 1];
    char* ptr = hex_str;
    
    for (size_t i = 0; i < len; i++) {
        ptr += sprintf(ptr, "%02x ", data[i]);
    }
    if (len > 0) {
        hex_str[len * 3 - 1] = '\0';  // Remove last space
    } else {
        hex_str[0] = '\0';
    }
    ESP_LOGI(TAG, "  %s: %s", label, hex_str);
}


void list_bonded_devices_with_keys(void)
{
    ble_addr_t bonded_addrs[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int num_bonded;
    int rc;

    // Get all bonded peer addresses
    rc = ble_store_util_bonded_peers(bonded_addrs, &num_bonded, 
                                    MYNEWT_VAL(BLE_STORE_MAX_BONDS));
    if (rc != 0) {
        ESP_LOGE("BLE", "Failed to retrieve bonded peers: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "=== Bonded Devices with Keys ===");
    ESP_LOGI(TAG, "Number of bonded devices: %d", num_bonded);

    for (int i = 0; i < num_bonded; i++) {
        char addr_str[18];
        sprintf(addr_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                bonded_addrs[i].val[5], bonded_addrs[i].val[4],
                bonded_addrs[i].val[3], bonded_addrs[i].val[2],
                bonded_addrs[i].val[1], bonded_addrs[i].val[0]);
        
        ESP_LOGI(TAG, "\n--- Device %d: %s ---", i + 1, addr_str);
        ESP_LOGI(TAG, "Address type: %s", 
                 bonded_addrs[i].type == BLE_ADDR_PUBLIC ? "Public" : "Random");

        // Get security information (LTK, etc.)
        struct ble_store_key_sec key_sec = {0};
        struct ble_store_value_sec value_sec = {0};
        
        key_sec.peer_addr = bonded_addrs[i];
        rc = ble_store_read_peer_sec(&key_sec, &value_sec);
        
        if (rc == 0) {
            ESP_LOGI(TAG, "Security Information:");
            ESP_LOGI(TAG, "  Authenticated: %s", value_sec.authenticated ? "Yes" : "No");
            ESP_LOGI(TAG, "  Secure Connections: %s", value_sec.sc ? "Yes" : "No");
            ESP_LOGI(TAG, "  Key Size: %d", value_sec.key_size);
            
            // Print LTK (Long Term Key)
            if (value_sec.ltk_present) {
                print_sec_hex_data("LTK", value_sec.ltk, 16);
                ESP_LOGI(TAG, "  LTK Random: %llu", (unsigned long long)value_sec.rand_num);
                ESP_LOGI(TAG, "  LTK EDIV: %u", value_sec.ediv);
            } else {
                ESP_LOGI(TAG, "  LTK: Not present");
            }

            // Print IRK (Identity Resolving Key)
            if (value_sec.irk_present) {
                print_sec_hex_data("IRK", value_sec.irk, 16);
            } else {
                ESP_LOGI(TAG, "  IRK: Not present");
            }

            // Print CSRK (Connection Signature Resolving Key)
            if (value_sec.csrk_present) {
                print_sec_hex_data("CSRK", value_sec.csrk, 16);
            } else {
                ESP_LOGI(TAG, "  CSRK: Not present");
            }
        } else {
            ESP_LOGI(TAG, "Failed to read security info: %d", rc);
        }

        // Get our own keys for this peer
        struct ble_store_key_sec our_key_sec = {0};
        struct ble_store_value_sec our_value_sec = {0};
        
        our_key_sec.peer_addr = bonded_addrs[i];
        rc = ble_store_read_our_sec(&our_key_sec, &our_value_sec);
        
        if (rc == 0) {
            ESP_LOGI(TAG, "Our Keys for this peer:");
            
            // Print our LTK
            if (our_value_sec.ltk_present) {
                print_sec_hex_data("Our LTK", our_value_sec.ltk, 16);
                ESP_LOGI(TAG, "  Our LTK Random: %llu", (unsigned long long)our_value_sec.rand_num);
                ESP_LOGI(TAG, "  Our LTK EDIV: %u", our_value_sec.ediv);
            }

            // Print our IRK
            if (our_value_sec.irk_present) {
                print_sec_hex_data("Our IRK", our_value_sec.irk, 16);
            }

            // Print our CSRK
            if (our_value_sec.csrk_present) {
                print_sec_hex_data("Our CSRK", our_value_sec.csrk, 16);
            }
        }

        // Get CCCD (Client Characteristic Configuration Descriptor) values
        struct ble_store_key_cccd cccd_key = {0};
        struct ble_store_value_cccd cccd_value = {0};
        
        cccd_key.peer_addr = bonded_addrs[i];
        
        ESP_LOGI(TAG, "CCCD Values:");
        int cccd_count = 0;
        
        // Try a few common handle ranges
        uint16_t test_handles[] = {0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 
                                  0x0010, 0x0020, 0x0030, 0x0040, 0x0050};
        int num_test_handles = sizeof(test_handles) / sizeof(test_handles[0]);
        
        for (int j = 0; j < num_test_handles; j++) {
            cccd_key.chr_val_handle = test_handles[j];
            rc = ble_store_read_cccd(&cccd_key, &cccd_value);
            
            if (rc == 0) {
                const char* notify_str = "";
                const char* indicate_str = "";
                
                // Check for notification flag (bit 0)
                if (cccd_value.flags & 0x0001) {
                    notify_str = "NOTIFY ";
                }
                // Check for indication flag (bit 1)
                if (cccd_value.flags & 0x0002) {
                    indicate_str = "INDICATE";
                }
                
                ESP_LOGI(TAG, "  Handle 0x%04x: Flags 0x%04x %s%s", 
                         cccd_value.chr_val_handle,
                         cccd_value.flags,
                         notify_str,
                         indicate_str);
                cccd_count++;
            }
        }
        
        if (cccd_count == 0) {
            ESP_LOGI(TAG, "  No CCCD values found in tested handles");
        }
    }
}


// Clear the entire bond database 
int clear_bond_database(void)
{
    int rc;
    
    ESP_LOGW(TAG, "Clearing entire bond database");
    
    rc = ble_store_clear();
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to clear bond database: %d", rc);
        return rc;
    }
    
    ESP_LOGI(TAG, "Bond database cleared successfully");
    return 0;
}

