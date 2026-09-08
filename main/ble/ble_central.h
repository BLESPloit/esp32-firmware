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

typedef enum {
    CENTRAL_PIN_POLICY_PROMPT = 0,
    CENTRAL_PIN_POLICY_CONFIGURED_ELSE_PROMPT = 1,
} central_pin_policy_t;

bool ble_central_is_active(void);
uint16_t ble_central_conn_handle(void);
void send_update_central_status_to_ws(const char *status);
esp_err_t ble_central_load_services_and_connect(const char *device_id);
void unload_ble_device_for_central(bool keep_services);
bool ble_central_cache_matches_addr(const ble_addr_t *addr);
bool ble_central_cache_resolve_addr_str(const char *addr_str, ble_addr_t *out);
void ble_central_request_discovery_broadcast_on_ready(void);
void ble_central_relay_op(const char* svc_uuid, const char* chr_uuid, const char* data, ble_relay_op_t op);
stored_service_t *ble_central_get_services(void);
void ble_central_attach_from_discovery(uint16_t conn_handle, discovery_context_t *ctx, void *arg);
int ble_central_connect(ble_addr_t *addr);
esp_err_t ble_central_reattach(uint16_t conn_handle);
void ble_central_set_pending_seq(uint32_t seq);
void ble_central_set_pending_requester(const char *node_id);

void ble_central_smp_reset(void);
void ble_central_smp_claim_pairing_rsp_cb(void);
void ble_central_smp_apply_json(const cJSON *json);
int ble_central_smp_initiate(uint16_t conn_handle);
int ble_central_smp_handle_gap_event(struct ble_gap_event *event, void *arg);
int ble_central_smp_pair_io(uint16_t conn_handle, bool has_passkey, uint32_t passkey,
                            bool has_accept, bool accept, bool cancel);
void ble_central_smp_on_auth_error(uint16_t conn_handle, int host_status,
                                   const char *svc, const char *chr, uint32_t seq);
void ble_central_smp_on_disconnect(uint16_t conn_handle, int reason);