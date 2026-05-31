#pragma once
#include "ble/ble_discovery.h" // stored_service_t

typedef enum {
    BLE_RELAY_OP_READ        = 0,
    BLE_RELAY_OP_WRITE       = 1,  // with response
    BLE_RELAY_OP_WRITE_NORESP = 2,  // no response
    BLE_RELAY_OP_SUBSCRIBE = 3,
    BLE_RELAY_OP_UNSUBSCRIBE = 4,
    BLE_RELAY_OP_READ_DESC = 5,
    BLE_RELAY_OP_UNKNOWN     = -1,
} ble_relay_op_t;

// needed to pass context to the subscribe callback
typedef struct {
    char svc[BLE_UUID_STR_LEN];
    char chr[BLE_UUID_STR_LEN];
} relay_subscribe_ctx_t;

typedef struct {
    char svc[BLE_UUID_STR_LEN];
    char chr[BLE_UUID_STR_LEN];
    char desc[12];   // "2901", "2902", etc. — short UUID string
} relay_desc_read_ctx_t;


// Pending relay request context — carries seq + requester through async GATT callbacks
typedef struct {
    uint32_t seq;
    char     requester[12];   // "ESP_AABBCC\0"
    bool     valid;
} relay_pending_t;

bool ble_central_is_active(void);
esp_err_t ble_central_load_services_and_connect(const char *device_id);
void unload_ble_device_for_central(void);
void ble_central_relay_op(const char* svc_uuid, const char* chr_uuid, const char* data, ble_relay_op_t op);
stored_service_t *ble_central_get_services(void);
void ble_central_attach_from_discovery(uint16_t conn_handle, discovery_context_t *ctx, void *arg);
int ble_central_connect(ble_addr_t *addr);
esp_err_t ble_central_reattach(uint16_t conn_handle);
void ble_central_set_pending_seq(uint32_t seq);
void ble_central_set_pending_requester(const char *node_id);