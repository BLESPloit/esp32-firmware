#pragma once
#include "ble/device_parser.h" // ble_server_t, ble_adv_params_t, etc.

// NimBLE auth_req bit definitions
#define BLE_SM_AUTH_REQ_BONDING   (1 << 0)
#define BLE_SM_AUTH_REQ_MITM      (1 << 2)
#define BLE_SM_AUTH_REQ_SC        (1 << 3)
#define BLE_SM_AUTH_REQ_KEYPRESS  (1 << 6)

void        ble_print_conn_desc(struct ble_gap_conn_desc *desc);
esp_err_t   ble_start_advertising(void);

int         gatt_svr_init(struct ble_gatt_svc_def *services);

int         ble_sim_gatt_access_callback(uint16_t conn_handle, uint16_t attr_handle,
                                         struct ble_gatt_access_ctxt *ctxt, void *arg);

bool        ble_sim_lookup_chr_by_val_handle(uint16_t val_handle,
                                             char *svc_uuid_out, char *chr_uuid_out);

void        configure_nimble_security_for_peripheral(ble_server_t *server);
int         ble_sim_smp_event(struct ble_gap_event *event, void *arg);
esp_err_t ble_adv_resolve(const char *device_folder);