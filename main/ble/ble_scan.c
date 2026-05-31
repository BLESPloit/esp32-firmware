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
#include "common/storage.h"
#include "common/utils.h"
#include "api/web_server.h" // web_scan_push_device, web_scan_broadcast_status
#include "ble/ble_scan.h"


static const char *TAG = "BLE scan";

// ── Global state: scanning ────────────────────────────────────────────────── 

volatile bool filter_connectable_only = false;
ble_scanned_device_t scanned_devices[MAX_SCANNED_DEVICES];
int scanned_device_count = 0;

static bool ble_scanning = false;
static SemaphoreHandle_t scanning_status_mutex = NULL; // for thread-safe update of scanning status

// used in web_server_ble_scanner.c
SemaphoreHandle_t scan_device_mutex = NULL; // for thread-safe update of scanning results

static int ble_scanner_gap_event_handler(struct ble_gap_event *event, void *arg);


// ── GET/SET scanning status ────────────────────────────────────────────────── 

// Thread-safe getter
bool is_ble_scanning(void) {
    bool value = false;
    if (xSemaphoreTake(scanning_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        value = ble_scanning;
        xSemaphoreGive(scanning_status_mutex);
    }
    return value;
}

// Thread-safe setter
void set_ble_scanning(bool value) {
    if (xSemaphoreTake(scanning_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ble_scanning = value;
        xSemaphoreGive(scanning_status_mutex);
    }
}


// ── Scan event handler ────────────────────────────────────────────────── 

static int ble_scanner_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;
    
    switch (event->type) {
        
    case BLE_GAP_EVENT_EXT_DISC:
        if (is_ble_scanning()) {
            process_scanned_device(event->ext_disc.addr.val,
                                 event->ext_disc.addr.type,
                                 event->ext_disc.rssi,
                                 event->ext_disc.data,
                                 event->ext_disc.length_data,
                                 event->ext_disc.props,
                                 event->ext_disc.prim_phy);
        }
        break;
        
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Scan complete");
        set_ble_scanning(false);
        web_scan_broadcast_status();   // ← notify clients scan ended
        break;
                
    default:
        ESP_LOGW(TAG, "Unhandled GAP event: %d", event->type);
        break;
    }
    
    return 0;
}

// ── Advertisement scanning helpers ────────────────────────────────────────────────── 

typedef enum {
    RSSI_BAND_STRONG   = 0,   // >= -60
    RSSI_BAND_MEDIUM   = 1,   // -60 .. -75
    RSSI_BAND_WEAK     = 2,   // -75 .. -90
    RSSI_BAND_CRITICAL = 3,   // < -90
} rssi_band_t;

static rssi_band_t rssi_get_band(int8_t rssi) {
    if (rssi >= -60) return RSSI_BAND_STRONG;
    if (rssi >= -75) return RSSI_BAND_MEDIUM;
    if (rssi >= -90) return RSSI_BAND_WEAK;
    return RSSI_BAND_CRITICAL;
}


/**
 * Derives the BLE PDU type from the advertisement properties bitmask.
 * NimBLE's props field is a combination of BLE_HCI_ADV_* bitmasks from
 * nimble/hci_common.h — legacy bit, connectable bit, scan-rsp bit, etc.
 */

static const char *get_adv_pdu_type_str(uint16_t props)
{
    // Scan response — check first, it can co-exist with other bits
    if (props & BLE_HCI_ADV_SCAN_RSP_MASK) {
        return "SCAN_RSP";
    }
    // Non-legacy (extended) advertising
    if (!(props & BLE_HCI_ADV_LEGACY_MASK)) {
        return "ADV_EXT_IND";
    }
    // Legacy PDU: derive from connectable + directed bits
    bool connectable = (props & BLE_HCI_ADV_CONN_MASK) != 0;
    bool directed    = (props & BLE_HCI_ADV_DIRECT_MASK) != 0;
    bool scannable   = (props & BLE_HCI_ADV_SCAN_MASK) != 0;

    if (connectable && directed)  return "ADV_DIRECT_IND";
    if (connectable)              return "ADV_IND";
    if (scannable)                return "ADV_SCAN_IND";
    return "ADV_NONCONN_IND";
}



const char* get_phy_name(uint8_t phy)
{
    switch (phy) {
    case BLE_HCI_LE_PHY_1M: return "1M";
        case BLE_HCI_LE_PHY_2M: return "2M";
    case BLE_HCI_LE_PHY_CODED: return "Coded";
    default: return "Unknown";
    }
}

bool is_legacy_adv(uint16_t props)
{
    return (props & BLE_HCI_ADV_LEGACY_MASK) != 0;
}

bool is_connectable_adv(uint16_t props)
{
    return (props & BLE_HCI_ADV_CONN_MASK) != 0;
}

static bool parse_device_name(uint8_t *data, uint8_t data_len, char *name, size_t name_size)
{
    if (!data || !name || data_len == 0 || name_size == 0) return false;
    
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    
    int rc = ble_hs_adv_parse_fields(&fields, data, data_len);
    
    if (rc == 0 && fields.name != NULL && fields.name_len > 0 && fields.name_len < 128) {
        int len = (fields.name_len < name_size - 1) ? fields.name_len : name_size - 1;
        
        // Validate printable characters
        bool is_valid = true;
        for (int i = 0; i < len; i++) {
            unsigned char c = fields.name[i];
            if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) {
                is_valid = false;
                break;
            }
        }
        
        if (is_valid) {
            memcpy(name, fields.name, len);
            name[len] = '\0';
            return true;
        }
    }
    
    return false;
}


// ── Multiple advertisements from the same address ───────────────────────── 

static bool adv_payload_already_seen(const ble_scanned_device_t *dev,
                                     const uint8_t *data, uint8_t len)
{
    if (!dev->seen_payloads) return false;
    for (int i = 0; i < dev->seen_count; i++) {
        if (dev->seen_payloads[i].len == len &&
            memcmp(dev->seen_payloads[i].data, data, len) == 0)
            return true;
    }
    return false;
}

// Returns false if allocation failed or cap reached
static bool adv_payload_mark_seen(ble_scanned_device_t *dev,
                                  const uint8_t *data, uint8_t len)
{
    if (dev->seen_count >= MAX_SEEN_ADV_PAYLOADS) return false;

    if (!dev->seen_payloads) {
        dev->seen_payloads = malloc(MAX_SEEN_ADV_PAYLOADS * sizeof(seen_adv_entry_t));
        if (!dev->seen_payloads) return false;   // OOM — skip silently
    }

    memcpy(dev->seen_payloads[dev->seen_count].data, data, len);
    dev->seen_payloads[dev->seen_count].len = len;
    dev->seen_count++;
    return true;
}

// Free any seen-payload sets from previous scan
static void free_adv_payloads(void)
{
    for (int i = 0; i < scanned_device_count; i++) {
        if (scanned_devices[i].seen_payloads) {
            free(scanned_devices[i].seen_payloads);
            scanned_devices[i].seen_payloads = NULL;
        }
    }
}



// ── Advertisement processing ────────────────────────────────────────────────── 

void process_scanned_device(uint8_t *addr, uint8_t addr_type, int8_t rssi,
                          uint8_t *data, uint8_t data_len, uint16_t props,
                          uint8_t phy)
{
    if (!addr || !data) return;
        
    bool is_scan_rsp = (props & BLE_HCI_ADV_SCAN_RSP_MASK) != 0;
    bool is_legacy = is_legacy_adv(props);
    bool is_connectable = is_connectable_adv(props);
    bool should_push = false;
    scan_push_reason_t push_reason = SCAN_PUSH_NEW;

    if (filter_connectable_only && !is_connectable && !is_scan_rsp) {
        return;
    }
    
    if (xSemaphoreTake(scan_device_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    
    // Find existing device
    int device_idx = -1;
    for (int i = 0; i < scanned_device_count && i < MAX_SCANNED_DEVICES; i++) {
        if (scanned_devices[i].valid && memcmp(scanned_devices[i].addr, addr, 6) == 0) {
            device_idx = i;
            break;
        }
    }
    
    if (device_idx == -1) {
        // New device
        if (scanned_device_count < MAX_SCANNED_DEVICES && !is_scan_rsp) {
            device_idx = scanned_device_count;
            memset(&scanned_devices[device_idx], 0, sizeof(ble_scanned_device_t));
            
            memcpy(scanned_devices[device_idx].addr, addr, 6);
            scanned_devices[device_idx].addr_type = addr_type;
            scanned_devices[device_idx].rssi = rssi;
            scanned_devices[device_idx].last_pushed_rssi = rssi;
            scanned_devices[device_idx].is_extended = !is_legacy;
            scanned_devices[device_idx].is_connectable = is_connectable;
            scanned_devices[device_idx].phy = phy;
            scanned_devices[device_idx].has_scan_rsp = false;
            
            scanned_devices[device_idx].full_adv_len = data_len;
            scanned_devices[device_idx].adv_data_len = (data_len < MAX_SCANNED_ADV_DATA_LEN) ?
                                                       data_len : MAX_SCANNED_ADV_DATA_LEN;
            
            if (scanned_devices[device_idx].adv_data_len > 0) {
                memcpy(scanned_devices[device_idx].adv_data, data,
                      scanned_devices[device_idx].adv_data_len);
            }
            
            if (!parse_device_name(data, data_len, scanned_devices[device_idx].name,
                                 sizeof(scanned_devices[device_idx].name))) {
                strcpy(scanned_devices[device_idx].name, "Unknown");
            }

            scanned_devices[device_idx].pdu_type = get_adv_pdu_type_str(props);
            
            scanned_devices[device_idx].valid = true;
            scanned_device_count++;
            should_push = true;   // new device → always push
            
            char addr_str[18];
            format_ble_addr(scanned_devices[device_idx].addr, addr_str, sizeof(addr_str));
            ESP_LOGI(TAG, "%s %s: %s RSSI:%d %s",
                    is_legacy ? "LEG" : "EXT",
                    is_connectable ? "CONN" : "NON-CONN",
                    addr_str, rssi, scanned_devices[device_idx].name);
        }
    } else {
        // Existing device
        if (device_idx >= 0 && device_idx < MAX_SCANNED_DEVICES) {
            scanned_devices[device_idx].rssi = rssi;

            if (!is_scan_rsp) {
                uint8_t new_len = (data_len < MAX_SCANNED_ADV_DATA_LEN)
                                  ? data_len : MAX_SCANNED_ADV_DATA_LEN;

                bool payload_changed =
                    (new_len != scanned_devices[device_idx].adv_data_len) ||
                    (memcmp(scanned_devices[device_idx].adv_data, data, new_len) != 0);

                if (payload_changed) {
                    // Always keep stored adv_data current regardless of push decision
                    scanned_devices[device_idx].adv_data_len = new_len;
                    scanned_devices[device_idx].full_adv_len = data_len;
                    memcpy(scanned_devices[device_idx].adv_data, data, new_len);

                    // Update pdu_type and is_extended current for this packet
                    // this might be a different advertising profile (so different PDU and connectable status)
                    scanned_devices[device_idx].is_extended = !is_legacy;
                    scanned_devices[device_idx].is_connectable = is_connectable;
                    scanned_devices[device_idx].phy = phy;
                    scanned_devices[device_idx].pdu_type = get_adv_pdu_type_str(props);


                    // Update name if new payload carries a longer one
                    char temp_name[32];
                    if (parse_device_name(data, new_len, temp_name, sizeof(temp_name))) {
                        if (strlen(temp_name) > strlen(scanned_devices[device_idx].name)) {
                            strncpy(scanned_devices[device_idx].name, temp_name,
                                    sizeof(scanned_devices[device_idx].name) - 1);
                        }
                    }

                    // Push only if this exact payload was never pushed before
                    if (!adv_payload_already_seen(&scanned_devices[device_idx], data, new_len)) {
                        if (adv_payload_mark_seen(&scanned_devices[device_idx], data, new_len)) {
                            should_push = true;
                            push_reason = SCAN_PUSH_ADV_DATA;
                        }
                        // if mark_seen returns false (OOM or cap reached) → silently skip
                    }
                }
                // RSSI update
                if (!should_push) {  // only check RSSI if no higher-priority push is already queued
                    if (rssi_get_band(rssi) != rssi_get_band(scanned_devices[device_idx].last_pushed_rssi)) {
                        should_push  = true;
                        push_reason  = SCAN_PUSH_RSSI;
                        scanned_devices[device_idx].last_pushed_rssi = rssi;
                    }
                }


            } else if (!scanned_devices[device_idx].has_scan_rsp) {
                // First scan response for this device
                scanned_devices[device_idx].full_scan_rsp_len = data_len;
                scanned_devices[device_idx].scan_rsp_len = (data_len < MAX_SCANNED_ADV_DATA_LEN)
                                                           ? data_len : MAX_SCANNED_ADV_DATA_LEN;
                if (scanned_devices[device_idx].scan_rsp_len > 0) {
                    memcpy(scanned_devices[device_idx].scan_rsp, data,
                           scanned_devices[device_idx].scan_rsp_len);
                }
                scanned_devices[device_idx].has_scan_rsp = true;
                should_push = true;
                push_reason = SCAN_PUSH_SCAN_RSP;

                // Try to extract name from scan response
                if (strcmp(scanned_devices[device_idx].name, "Unknown") == 0) {
                    parse_device_name(data, scanned_devices[device_idx].scan_rsp_len,
                                      scanned_devices[device_idx].name,
                                      sizeof(scanned_devices[device_idx].name));
                } else {
                    char temp_name[32];
                    if (parse_device_name(data, scanned_devices[device_idx].scan_rsp_len,
                                          temp_name, sizeof(temp_name))) {
                        if (strlen(temp_name) > strlen(scanned_devices[device_idx].name)) {
                            strncpy(scanned_devices[device_idx].name, temp_name,
                                    sizeof(scanned_devices[device_idx].name) - 1);
                        }
                    }
                }

                char addr_str[18];
                format_ble_addr(scanned_devices[device_idx].addr, addr_str, sizeof(addr_str));
                ESP_LOGI(TAG, "Scan response from: %s", addr_str);
            }

        }
    }            
 

    if (should_push) {
        ble_scanned_device_t device_copy = scanned_devices[device_idx];
        xSemaphoreGive(scan_device_mutex);
        web_scan_push_device(&device_copy, push_reason);
        return;
    }

    xSemaphoreGive(scan_device_mutex);
}


// ── Scanning control ────────────────────────────────────────────────── 

// Initialize mutexes, need to call it from main at startup
esp_err_t ble_scanner_init(void)
{

    // mutex for scanning status  
    scanning_status_mutex = xSemaphoreCreateMutex();
    if (scanning_status_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create scanning status mutex");
        return ESP_FAIL;
    }

    // mutex for device scan 
    scan_device_mutex = xSemaphoreCreateMutex();
    if (scan_device_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create scan mutex");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Scanner initialized");
    return ESP_OK;
}

esp_err_t start_ble_scan(bool connectable_only)
{

    if (scanning_status_mutex == NULL || scan_device_mutex == NULL) {
        ESP_LOGE(TAG, "System not initialized");
        return ESP_FAIL;
    }

    if (is_ble_scanning()) {
        ESP_LOGW(TAG, "Scan already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (ble_gap_conn_active()) {
        int rc_cancel = ble_gap_conn_cancel();
        if (rc_cancel != 0 && rc_cancel != BLE_HS_EALREADY && rc_cancel != BLE_HS_EDONE) {
            ESP_LOGW(TAG, "ble_gap_conn_cancel before scan failed: %d", rc_cancel);
        } else if (rc_cancel == 0) {
            ESP_LOGI(TAG, "Cancelled pending connection before starting scan");
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    struct ble_gap_disc_params disc_params;
    filter_connectable_only = connectable_only;
    
    if (xSemaphoreTake(scan_device_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_FAIL;
    }

    free_adv_payloads();

    scanned_device_count = 0;
    memset(scanned_devices, 0, sizeof(ble_scanned_device_t) * MAX_SCANNED_DEVICES);
    xSemaphoreGive(scan_device_mutex);
    
    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.filter_duplicates = 0;
    disc_params.passive = 0;
    disc_params.itvl = 0;
    disc_params.window = 0;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;
    
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_SCAN_DURATION_MS,
                         &disc_params, ble_scanner_gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start scan: %d", rc);
        return ESP_FAIL;
    }
    
    set_ble_scanning(true);
    ESP_LOGI(TAG, "Scan started (%ds) - %s", BLE_SCAN_DURATION_MS / 1000,
            connectable_only ? "Connectable only" : "All devices");
    web_scan_broadcast_status();   // ← notify clients scan started + clear old state
    return ESP_OK;
}

esp_err_t stop_ble_scan(void)
{
    if (!is_ble_scanning()) return ESP_ERR_INVALID_STATE;
    
    int rc = ble_gap_disc_cancel();
    if (rc != 0) return ESP_FAIL;
    
    set_ble_scanning(false);
    ESP_LOGI(TAG, "Scan stopped");
    return ESP_OK;
}

bool ble_scanned_device_info_add_to_json(cJSON *root, const ble_scanned_device_t *device)
{
    if (root == NULL || device == NULL) {
        return false;
    }
    
    cJSON *devinfo = cJSON_CreateObject();
    if (devinfo == NULL) {
        return false;
    }
    
    char bd_addr_str[18];
    snprintf(bd_addr_str, sizeof(bd_addr_str),
            "%02x:%02x:%02x:%02x:%02x:%02x",
            device->addr[5], device->addr[4], device->addr[3],
            device->addr[2], device->addr[1], device->addr[0]);
    
    char *adv_data_hex = NULL;
    if (device->adv_data_len > 0) {
        adv_data_hex = (char*)malloc(device->adv_data_len * 2 + 1);
        if (adv_data_hex != NULL) {
            bin_to_hex_string(device->adv_data, device->adv_data_len, adv_data_hex);
        }
    }
    
    char *scan_rsp_hex = NULL;
    if (device->scan_rsp_len > 0) {
        scan_rsp_hex = (char*)malloc(device->scan_rsp_len * 2 + 1);
        if (scan_rsp_hex != NULL) {
            bin_to_hex_string(device->scan_rsp, device->scan_rsp_len, scan_rsp_hex);
        }
    }
    
    cJSON_AddStringToObject(devinfo, "adv_data", adv_data_hex ? adv_data_hex : "");
    cJSON_AddStringToObject(devinfo, "bd_addr", bd_addr_str);
    cJSON_AddNumberToObject(devinfo, "addr_type", device->addr_type);
    cJSON_AddStringToObject(devinfo, "scan_rsp", scan_rsp_hex ? scan_rsp_hex : "");
    cJSON_AddStringToObject(devinfo, "pdu_type", device->pdu_type);
    
    cJSON_AddItemToObject(root, "devinfo", devinfo);
    
    if (adv_data_hex) free(adv_data_hex);
    if (scan_rsp_hex) free(scan_rsp_hex);
    
    return true;
}


esp_err_t ble_scanner_cleanup(void) {
    int rc;
    
    ESP_LOGI(TAG, "Deinitializing BLE scanner...");
    
    // Stop scanning if active
    if (is_ble_scanning()) {
        rc = ble_gap_disc_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "Failed to cancel scanning: %d", rc);
            // Continue with cleanup anyway
        } else {
            ESP_LOGI(TAG, "Scanning stopped");
            set_ble_scanning(false);
        }
    }
    
    // Clear scanned devices list
    if (scan_device_mutex != NULL) {
        if (xSemaphoreTake(scan_device_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            free_adv_payloads();
            scanned_device_count = 0;
            memset(scanned_devices, 0, sizeof(ble_scanned_device_t) * MAX_SCANNED_DEVICES);
            xSemaphoreGive(scan_device_mutex);
            ESP_LOGI(TAG, "Cleared scanned devices list");
        } else {
            ESP_LOGW(TAG, "Failed to take mutex for cleanup");
        }
        
        // Delete the mutex
        vSemaphoreDelete(scan_device_mutex);
        scan_device_mutex = NULL;
    }
    
    // Reset state variables
    filter_connectable_only = false;
    
    ESP_LOGI(TAG, "BLE scanner deinitialized successfully");
    
    return ESP_OK;
}
