#pragma once
#include "host/ble_hs.h"
#include "ble/device_parser.h" 

#define PROFILE_ADV_FILE    "adv.json"

esp_err_t load_ble_device_for_simulation(const char *device_id);
void unload_ble_device_for_simulation(void);

void ble_adv_set_free(ble_adv_set_t *set);

cJSON *ble_adv_set_to_json(void);

// needed by lua to enable/disable advertising profile
esp_err_t   ble_adv_instance_start(ble_adv_instance_t *inst);

/**
 * Update the simulated GATT characteristic value in RAM (char_context_t) and
 * schedule notifications only for centrals that have subscribed (NimBLE
 * ble_gatts_chr_updated). Does not re-run Lua on_write handlers.
 *
 * @param val_handle   Registered characteristic value handle
 * @param data         New value bytes (may be NULL if data_len == 0)
 * @param data_len     Length of data
 * @return             0 on success, BLE_HS_ENOENT if unknown handle / no server,
 *                     BLE_HS_ENOMEM on realloc failure, BLE_HS_EINVAL if
 *                     data_len > 0 but data is NULL
 */
int ble_sim_chr_update_value_and_notify(uint16_t val_handle,
                                        const uint8_t *data,
                                        uint16_t data_len);