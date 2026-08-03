#pragma once
#include "esp_http_server.h"
#include "ble/ble_discovery.h" //pairing_mode_t, pairing_strategy_t
#include "cJSON.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))  // Add MIN macro
#endif

// websocket
void init_websocket_mutex(void);
void smart_state_init(void);
void ws_node_init(void);

esp_err_t serve_static_json(httpd_req_t *req, char *filepath);
esp_err_t serve_static_html(httpd_req_t *req, const char *filename);
esp_err_t serve_static_png(httpd_req_t *req, char *filepath);
esp_err_t serve_static_svg(httpd_req_t *req, char *filepath);
esp_err_t serve_html_fragments(httpd_req_t *req, const char **filenames, int count);

// re-used across device_sim and devices_list
esp_err_t device_gfx_handler(httpd_req_t *req);

uint8_t register_ble_scanner_handlers_in_web_server(httpd_handle_t *server);
uint8_t register_device_central_handlers_in_web_server(httpd_handle_t *server);
uint8_t register_device_editor_handlers_in_web_server(httpd_handle_t *server);
uint8_t register_device_sim_handlers_in_web_server(httpd_handle_t *server);
uint8_t register_devices_list_handlers_in_web_server(httpd_handle_t *server);
uint8_t register_file_handlers_in_web_server(httpd_handle_t *server);
uint8_t register_relay_handlers_in_web_server(httpd_handle_t *server);
uint8_t register_ble_sim_trace_handlers_in_web_server(httpd_handle_t *server);
uint8_t register_static_handlers_in_web_server(httpd_handle_t *server);
uint8_t register_system_handlers_in_web_server(httpd_handle_t *server);
uint8_t register_websocket_handler_in_web_server(httpd_handle_t *server);
uint8_t register_wifi_config_handlers_in_webserver(httpd_handle_t *server);

// called from websocket 

void web_central_start(const char *device_folder);
void web_central_stop(void);
void web_central_broadcast_status(void);

void web_scan_start(bool connectable_only);
void web_scan_stop(void);
void web_scan_dump_all(void);
void web_scan_connect(const char *addr, bool save_result, bool open_central, bool read_values, 
                      pairing_mode_t mode, pairing_strategy_t strategy, uint32_t pin);


// Call once after WebSocket server is ready
void wslog_init(void);
uint8_t register_wslog_handlers_in_web_server(httpd_handle_t *server);

const char *ws_get_node_id(void);

// A domain module registers a handler for incoming WS messages.
// Receives the already-parsed type string and the full cJSON root (do NOT free it).
// Return true if the message was handled (stops further dispatch).
typedef bool (*ws_message_handler_fn_t)(const char *type, cJSON *json);

void ws_register_message_handler(ws_message_handler_fn_t fn);

/** Copy req_id from request into response when present (number or string). */
void ws_json_echo_req_id(cJSON *response, const cJSON *request);

// getters
const char *web_central_get_device(void);
const char *web_sim_get_device(void);

void smart_state_remove_element(const char* element_id);

esp_err_t websocket_broadcast_json_presrc_safe(const char *json_str);