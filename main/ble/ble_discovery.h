#pragma once
#include "cJSON.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

#define MAX_READ_RETRIES 2 // Retry failed reads once before skipping
#define MAX_RECONNECT_ATTEMPTS 3
#define RECONNECT_DELAY_MS 1000
#define RECONNECT_BACKOFF_MULTIPLIER 2

// forward declaration
struct discovery_context;

typedef void (*discovery_handoff_cb_t)(uint16_t conn_handle,
                                       struct discovery_context *ctx,
                                       void *arg);


// Discovery phase states
typedef enum {
    DISC_PHASE_IDLE = 0,
    DISC_PHASE_SERVICES,
    DISC_PHASE_CHARACTERISTICS,
    DISC_PHASE_DESCRIPTORS,
    DISC_PHASE_READING_VALUES,
    DISC_PHASE_PAIRING, 
    DISC_PHASE_READING_SECURED,
    DISC_PHASE_COMPLETE
} disc_phase_t;

// Read retry tracking
typedef struct {
    uint16_t handle;
    uint8_t retry_count;
    int last_error;
    bool is_descriptor;  // true if descriptor, false if characteristic
} read_retry_info_t;

// Stored structures for discovered GATT data
typedef struct stored_descriptor {
    uint16_t handle;
    ble_uuid_any_t uuid;
    char *value_hex;
    bool encryption_required;
    uint8_t read_retry_count;
    int last_read_error;
    struct stored_descriptor *next;
} stored_descriptor_t;

typedef struct stored_characteristic {
    uint16_t def_handle;
    uint16_t val_handle;
    ble_uuid_any_t uuid;
    uint8_t properties;
    char *value_hex;
    bool encryption_required;
    uint8_t read_retry_count;
    int last_read_error;
    stored_descriptor_t *descriptors;
    struct stored_characteristic *next;
} stored_characteristic_t;

typedef struct stored_service {
    uint16_t start_handle;
    uint16_t end_handle;
    ble_uuid_any_t uuid;
    stored_characteristic_t *characteristics;
    struct stored_service *next;
} stored_service_t;

// Pairing behavior mode
typedef enum {
    PAIRING_MODE_NONE = 0,           // Don't attempt pairing at all
    PAIRING_MODE_PROBE_ONLY,         // Send request, get response, then abort
    PAIRING_MODE_COMPLETE            // Complete full pairing process
} pairing_mode_t;


// Pairing/Security information captured from SMP packets
typedef struct {
    bool pairing_attempted;
    bool response_received;
    bool aborted_after_response;
    bool pairing_successful;
    
    // Pairing Request (from central/initiator - us)
    struct {
        uint8_t io_capability;      // 0=DisplayOnly, 1=DisplayYesNo, 2=KeyboardOnly, 3=NoInputNoOutput, 4=KeyboardDisplay
        uint8_t oob_data_flag;      // 0=not present, 1=present
        uint8_t auth_req;           // Bit0=Bonding, Bit2=MITM, Bit3=SC, Bit4=Keypress, Bit5=CT2
        uint8_t max_key_size;       // 7-16 bytes
        uint8_t init_key_dist;      // Bit0=EncKey, Bit1=IdKey, Bit2=Sign, Bit3=Link
        uint8_t resp_key_dist;      // Same as above
    } request;
    
    // Pairing Response (from peripheral/responder - peer device)
    struct {
        uint8_t io_capability;
        uint8_t oob_data_flag;
        uint8_t auth_req;
        uint8_t max_key_size;
        uint8_t init_key_dist;
        uint8_t resp_key_dist;
    } response;
    
    uint8_t pairing_method;         // 0=JustWorks, 1=Passkey, 2=NumericComparison, 3=OOB
    bool encrypted;                 // Link encrypted
} pairing_info_t;


// Pairing strategy progression
typedef enum {
    PAIRING_STRATEGY_LEGACY_JUST_WORKS = 0,  // Legacy, NoInput/NoOutput, SC disabled
    PAIRING_STRATEGY_SC_JUST_WORKS,          // Secure Connections, NoInput/NoOutput
    PAIRING_STRATEGY_LEGACY_PIN,             // Legacy, KeyboardOnly (PIN entry)
    PAIRING_STRATEGY_SC_PIN,                 // Secure Connections, KeyboardOnly (PIN entry)
    PAIRING_STRATEGY_AUTO                    // Try all strategies (not for manual selection)
} pairing_strategy_t;


// Pairing attempt tracking
typedef struct {
    pairing_strategy_t current_strategy;
    uint32_t attempt_timestamp;
    uint32_t pairing_timeout_ms;
    bool waiting_for_response;
    bool waiting_for_encryption;
    bool secured_read_attempted;
    int secured_read_status;
} pairing_attempt_context_t;

// Main discovery context
typedef struct {
    uint16_t conn_handle;
    disc_phase_t phase;
    bool read_values;
    
    // Discovery state
    stored_service_t *services;
    stored_service_t *current_service;
    stored_characteristic_t *current_char;
    stored_descriptor_t *current_desc;
    
    // Retry tracking
    read_retry_info_t retry_info;
    bool retry_in_progress;
    
    // JSON output
    cJSON *json_root;
    cJSON *json_services;
    
    // Device info
    ble_addr_t device_addr;
    uint8_t device_id;
    
    // Disconnection handling
    bool disconnected_during_read;
    uint8_t reconnect_attempt_count;
    uint32_t reconnect_delay_ms;
    bool reconnect_attempted;
    // Save service context for reconnection
    stored_service_t *retry_service;  // Which service was being read when exception happened

    pairing_info_t pairing_info;
    pairing_mode_t pairing_mode;     // How to handle pairing    

    // Pairing strategy configuration
    pairing_strategy_t pairing_strategy;      // Selected strategy (or AUTO)
    pairing_strategy_t pairing_strategy_initial; // Store initial selection
    uint32_t pairing_pin;                     // PIN for keyboard strategies
    bool auto_retry_strategies;               // True if using AUTO mode
    bool retrying_pairing_strategy;           // Flag for disconnect/reconnect cycle
    
    // tracking for secured reads
    bool any_auth_required;
    bool pairing_completed;    

    bool save_json_on_complete;    // write ble.json to LittleFS (for sim, cache)
    discovery_handoff_cb_t handoff_cb;
    void *handoff_arg;
} discovery_context_t;



// Public API
int ble_connect_and_discover(const char *addr_str, bool save_result, bool open_central, bool read_values, 
                            pairing_mode_t pairing_mode, pairing_strategy_t strategy, uint32_t pin);

// Error reason helpers
const char* get_att_error_string(int error);

// needed in device_parser and ble_central
void free_descriptors(stored_descriptor_t *desc);
void free_characteristics(stored_characteristic_t *chr);
void free_services(stored_service_t *svc);
// needed in ble_central
discovery_context_t* create_discovery_context(uint16_t conn_handle, 
                                                     uint8_t device_id,
                                                     bool read_values,
                                                     pairing_mode_t pairing_mode,
                                                     pairing_strategy_t strategy,
                                                     uint32_t pin);
void destroy_discovery_context(discovery_context_t *ctx);