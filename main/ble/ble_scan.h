#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#include "host/ble_gatt.h"

#define MAX_SCANNED_DEVICES 200
#define MAX_SCANNED_ADV_DATA_LEN 64
#define MAX_SCANNED_SCAN_RESP_DATA_LEN 31
#define BLE_SCAN_DURATION_MS 30000
#define MAX_SEEN_ADV_PAYLOADS  4   // cap per device, allocated on first rotation
#define SCAN_RSSI_PUSH_MIN_MS       2000
#define SCAN_RSSI_PUSH_MIN_DB_DELTA 20

typedef struct {
    uint8_t  data[MAX_SCANNED_ADV_DATA_LEN];
    uint8_t  len;
} seen_adv_entry_t;

// Scanned device structure
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    int8_t last_pushed_rssi;
    uint32_t last_pushed_rssi_ms;
    char name[32];
    uint8_t adv_data[MAX_SCANNED_ADV_DATA_LEN];
    uint8_t adv_data_len;
    uint16_t full_adv_len;
    uint8_t scan_rsp[MAX_SCANNED_SCAN_RESP_DATA_LEN];
    uint8_t scan_rsp_len;
    uint16_t full_scan_rsp_len;
    const char *pdu_type;
    bool valid;
    bool has_scan_rsp;
    bool is_extended;
    bool is_connectable;
    uint8_t phy;
    seen_adv_entry_t *seen_payloads;   // malloc'd array, NULL = not yet rotating
    uint8_t           seen_count;      // entries used
} __attribute__((packed, aligned(4))) ble_scanned_device_t;

// One shared discovery procedure. Consumers share it; nobody starts a second scan.
#define BLE_SCAN_CONSUMER_REMOTE  0x01
#define BLE_SCAN_CONSUMER_SIM     0x02
#define BLE_SCAN_CONSUMER_CENTRAL 0x04

// Legacy advertising report from the shared scanner. data is the raw AD
// (length byte first), at most 31 bytes. Called on the NimBLE host task.
typedef void (*ble_scan_legacy_fn)(const uint8_t *addr, uint8_t addr_type,
                                   int8_t rssi, const uint8_t *data, uint8_t data_len,
                                   bool is_scan_rsp);

esp_err_t ble_scanner_init(void);
esp_err_t start_ble_scan(bool connectable_only);
esp_err_t stop_ble_scan(void);

// Add or drop a consumer. The radio stops only when the last consumer leaves.
// SIM and CENTRAL keep discovery running without a timeout; REMOTE alone uses
// BLE_SCAN_DURATION_MS. stop_ble_scan() force-stops every consumer.
esp_err_t ble_scan_subscribe(uint8_t consumer);
esp_err_t ble_scan_unsubscribe(uint8_t consumer);
void ble_scan_force_stop(void);

// Central match runs beside the remote scan list while BLE_SCAN_CONSUMER_CENTRAL is set.
void ble_scan_set_central_fn(ble_scan_legacy_fn fn);

// get/set scanning status
bool is_ble_scanning(void);
void set_ble_scanning(bool value);

// Utility functions
const char* get_phy_name(uint8_t phy);
bool is_legacy_adv(uint16_t props);
bool is_connectable_adv(uint16_t props);
void process_scanned_device(uint8_t *addr, uint8_t addr_type, int8_t rssi, 
                          uint8_t *data, uint8_t data_len, uint16_t props,
                          uint8_t phy);
bool ble_scanned_device_info_add_to_json(cJSON *root, const ble_scanned_device_t *device);
