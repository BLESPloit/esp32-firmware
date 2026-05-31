#pragma once
#include "cJSON.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble/ble_discovery.h" // stored_service_t

#define MAX_ADV_DATA_LEN 256
#define MAX_SCAN_RESP_LEN 256

// Dynamic callback configuration
typedef struct {
    char on_read[64];
    char on_write[64];
    char on_sub[64];
} ble_dynamic_config_t;

// Combined structure stored in chr->arg
typedef struct {
    uint16_t data_len;
    uint8_t *data;

    uint16_t val_handle;

    uint16_t original_handle;
    uint16_t original_value_handle;
    uint8_t  original_properties;
    uint8_t  service_index;
    uint8_t  char_index;

    bool encryption_required;
    bool authentication_required;
    ble_dynamic_config_t dynamic;
    bool has_dynamic;
} char_context_t;


// advertising profiles

typedef enum {
    BLE_ADV_ADDR_RANDOM_GENERATED = 0,  // fresh NRPA each start
    BLE_ADV_ADDR_RANDOM_SPECIFIC  = 1,  // use bd_addr string, type = RANDOM
    BLE_ADV_ADDR_PUBLIC           = 2,  // for now cannot spoof on ESP32, falls back to RANDOM
    BLE_ADV_ADDR_PUBLIC_HARDWARE  = 3,  // use actual ESP32 hardware address
} ble_adv_addr_type_t;

typedef enum {
    BLE_ADV_PHY_1M    = 0,
    BLE_ADV_PHY_2M    = 1,
    BLE_ADV_PHY_CODED = 2,
} ble_adv_phy_t;

// One payload entry in a rotation sequence
typedef struct {
    uint8_t *adv_data;
    uint16_t adv_data_len;
} ble_adv_payload_t;

// Optional payload rotation config
typedef struct {
    uint32_t          interval_ms;
    ble_adv_payload_t *payloads;     // malloc'd array
    uint8_t            count;
    uint8_t            current_idx;  // runtime: which payload is active
} ble_adv_rotation_t;

typedef enum {
    BLE_ADV_SOURCE_DEVINFO  = 0,
    BLE_ADV_SOURCE_PROFILE  = 1,
} ble_adv_source_t;

typedef struct {
    ble_adv_source_t    source;
    char                id[32];

    // Address
    ble_adv_addr_type_t addr_type;
    char                bd_addr[18];   // used when addr_type == RANDOM_SPECIFIC

    // GAP / ble_gap_ext_adv_params fields
    bool                legacy_pdu;
    bool                connectable;
    bool                scannable;
    uint8_t             channel_map;   // 0x07 = all three channels
    ble_adv_phy_t       primary_phy;
    ble_adv_phy_t       secondary_phy;
    int8_t              tx_power;      // 127 = controller default
    uint16_t            itvl;          // in 0.625 ms units (converted from adv_interval_ms)

    // Static payloads (used when rotation == NULL)
    uint8_t            *adv_data;
    uint16_t            adv_data_len;
    uint8_t            *scan_rsp;
    uint16_t            scan_rsp_len;

    // Optional rotation (NULL = static)
    ble_adv_rotation_t *rotation;

    bool                owns_buffers;  // true → free on teardown
} ble_adv_params_t;

typedef struct {
    ble_adv_params_t  params;
    uint8_t           instance;
    bool              running;
    bool              initially_enabled;
    TimerHandle_t     rotation_timer;  // NULL if no rotation
} ble_adv_instance_t;

typedef struct {
    ble_adv_instance_t *instances;
    uint8_t             count;
} ble_adv_set_t;


// ble.json devinfo

typedef struct {
    uint8_t  adv_data[MAX_ADV_DATA_LEN];
    uint16_t adv_data_len;

    char     bd_addr[32];
    uint8_t  addr_type;

    uint8_t  scan_rsp[MAX_SCAN_RESP_LEN];
    uint16_t scan_rsp_len;
    char     pdu_type[16];
} ble_devinfo_t;

// Pairing/security configuration
typedef struct {
    uint8_t io_capability;
    uint8_t oob_data_flag;
    uint8_t auth_req;
    uint8_t max_key_size;
    uint8_t init_key_dist;
    uint8_t resp_key_dist;
} ble_pairing_config_t;

typedef struct {
    bool                 initiate_pairing_on_connection;
    char                 passkey[7];
    ble_pairing_config_t response;
} ble_pairing_info_t;

// Main server structure
typedef struct {
    ble_devinfo_t      *devinfo;
    ble_adv_set_t       adv_set;
    ble_pairing_info_t *pairing_info;
    struct ble_gatt_svc_def *services;
    int                 service_count;
} ble_server_t;


char         *read_json_file(const char *filepath, size_t *out_size);
ble_gatt_chr_flags properties_to_flags(uint8_t properties);
ble_uuid_any_t    *create_uuid_from_string(const char *uuid_str);
ble_server_t      *parse_json_to_nimble(const char *json_string, ble_gatt_access_fn callback);

bool parse_service_direct(cJSON *svc_json, struct ble_gatt_svc_def *svc_def,
                          int svc_idx, ble_gatt_access_fn callback);

bool parse_characteristic_direct(cJSON *char_json, struct ble_gatt_chr_def *chr_def,
                                  int svc_idx, int char_idx, ble_gatt_access_fn callback);

bool parse_dynamic_config(cJSON *dynamic_json, ble_dynamic_config_t *config);

bool parse_descriptor_direct(cJSON *desc_json, struct ble_gatt_dsc_def *dsc_def,
                              ble_gatt_access_fn callback);

void parse_devinfo_direct(cJSON *devinfo, ble_server_t *server);

bool parse_pairing_info(cJSON *pairing_json, ble_server_t *server);

void free_ble_server(ble_server_t **server_ptr);

bool parse_json_to_discovery(const char *json_string,
                              stored_service_t **out_services,
                              ble_addr_t *out_device_addr);
