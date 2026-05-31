#pragma once
#include "nvs_flash.h"
#include "time.h"
#include "cJSON.h"
#include "host/ble_gap.h"

#define LITTLEFS_LABEL "storage"
#define NVS_NAMESPACE "config"

// available config value types
typedef enum {
    CONFIG_TYPE_UINT8,
    CONFIG_TYPE_UINT16,
    CONFIG_TYPE_UINT32,
    CONFIG_TYPE_STR,
    CONFIG_TYPE_BOOL
} config_value_type_t;

// Union holding different possible value types
typedef union {
    uint8_t     u8;
    uint16_t    u16;
    uint32_t    u32;
    char *str;
} config_value_t;

// Struct representing a single config parameter
typedef struct {
    config_value_t      value;
    config_value_type_t type;
    const char          *nvs_name;  // nvs storage label
} config_param_t;

// Main device configuration struct
typedef struct {
    config_param_t net_enabled;

    // wifi STA credentials
    config_param_t wifi_ssid;
    config_param_t wifi_psk;

    // soft-AP overrides (optional NVS); mode pref WIFI_MODE_PREF_* in wifi.h
    config_param_t wifi_ap_ssid;
    config_param_t wifi_ap_psk;
    config_param_t wifi_mode_pref;
} device_config_t;


esp_err_t initialize_littlefs(void);
void save_json_to_file(const ble_addr_t *addr, cJSON *json);

esp_err_t initialize_nvs(void);
esp_err_t open_nvs_handle(nvs_handle_t *handle);
void close_nvs_handle(nvs_handle_t *handle);
esp_err_t save_param_to_nvs(nvs_handle_t handle, const config_param_t *param, bool commit);
esp_err_t write_config_nvs(void);
esp_err_t load_param_from_nvs(nvs_handle_t handle, config_param_t *param);
esp_err_t read_config_nvs(void);
esp_err_t generate_new_badge_id(bool restart_advertising);
esp_err_t update_nvs_points_and_energy(void);

void timestamp_to_string(uint32_t timestamp, char *strftime_buf, size_t bufsize);