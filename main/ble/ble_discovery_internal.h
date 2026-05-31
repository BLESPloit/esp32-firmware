#pragma once
#include "ble/ble_discovery.h"

// Called from core/read/smp files
void discovery_complete(discovery_context_t *ctx, int rc);
void start_service_discovery(discovery_context_t *ctx);
void start_value_reading(discovery_context_t *ctx);
void start_pairing_phase(discovery_context_t *ctx);
void start_secured_reads(discovery_context_t *ctx);
void retry_failed_read(discovery_context_t *ctx);
void reconnect_and_resume(discovery_context_t *ctx);

void pairing_response_callback(uint16_t conn_handle, 
                                     const struct ble_sm_pairing_params *rsp, 
                                     void *arg);


// Serialises ctx->services linked list into ctx->json_root — no file I/O
void build_services_json(discovery_context_t *ctx);

// Calls build_services_json() then writes to LittleFS
void build_json_and_finish(discovery_context_t *ctx);

int ble_smp_handle_gap_event(struct ble_gap_event *event, void *arg);

const char* get_pairing_strategy_name(pairing_strategy_t strategy);
void store_pairing_attempt_info(discovery_context_t *ctx);


// Shared state
extern discovery_context_t *g_disc_ctx;

// Error helpers (used by read, smp files)
const char* get_att_error_string(int error);
bool is_authentication_required_error(int error);