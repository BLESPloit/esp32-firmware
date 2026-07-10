#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h> 

#include "services/gap/ble_svc_gap.h" // BLE_ADDR_PUBLIC ...
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

#include "ble/ble_sim.h" // load device for simulation
#include "ble/ble_discovery.h"
#include "ble/ble_scan.h"
#include "graphics/graphics.h"
#include "interface/interface_sim.h"
#include "common/storage.h"
#include "common/utils.h"
#include "api/web_server.h"
#include "api/web_server_internal.h"



static const char *TAG = "web server";

httpd_handle_t server = NULL;

// Global state for currently simulated device
char current_simulated_device[128] = "";
SemaphoreHandle_t simulation_mutex = NULL;

char current_central_device[128] = "";
SemaphoreHandle_t device_central_mutex = NULL;

// Initialize simulation state mutex
void init_simulation_state(void) {
    simulation_mutex = xSemaphoreCreateMutex();
    device_central_mutex = xSemaphoreCreateMutex();
    init_websocket_mutex();
}

esp_err_t httpd_index_page(httpd_req_t *req) {
    return serve_static_html(req, "index.html");
}

// ── HTTPD start ────────────────────────────────────────────────── 

bool web_server_is_running(void)
{
    return server != NULL;
}

esp_err_t start_web_server(void)
{
    if (server != NULL) {
        return ESP_OK;
    }

    int rc;
    static int handler_count = 0;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 30; 
//    config.stack_size = 4096; 
    config.stack_size = 8192;  // Increase stack size for file operations
    config.uri_match_fn = httpd_uri_match_wildcard; // set wildcard matching function
    config.max_open_sockets = 10;  // Increase from default 7 to support list of devices with multiple pictures
    config.lru_purge_enable = true; // Enable LRU socket purging
    config.recv_wait_timeout = 3;   // reduce timeouts
    config.send_wait_timeout = 1;
   
    rc = httpd_start(&server, &config);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %d", rc);
        log_memory_usage("After unsuccessful HTTP server start");
        return ESP_FAIL;
    }

    // initialize device simulation mutex
    init_simulation_state();
    // initialize ws client for relay mutex
    ws_relay_client_init();
    ws_node_init();
    // Initialize WebSocket mutex and state
    init_websocket_mutex();
    smart_state_init();

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = httpd_index_page,
        .user_ctx = NULL,
        .is_websocket = false,
    };  
    httpd_register_uri_handler(server, &index_uri);
    handler_count++;

    handler_count += register_websocket_handler_in_web_server(&server);
    handler_count += register_file_handlers_in_web_server(&server);
    handler_count += register_ble_scanner_handlers_in_web_server(&server);
    handler_count += register_device_central_handlers_in_web_server(&server);
    handler_count += register_device_editor_handlers_in_web_server(&server);
    handler_count += register_device_sim_handlers_in_web_server(&server);
    handler_count += register_devices_list_handlers_in_web_server(&server);
    handler_count += register_relay_handlers_in_web_server(&server);
    handler_count += register_ble_sim_trace_handlers_in_web_server(&server);
    handler_count += register_static_handlers_in_web_server(&server);
    handler_count += register_system_handlers_in_web_server(&server);
    handler_count += register_wslog_handlers_in_web_server(&server);
    handler_count += register_wifi_config_handlers_in_webserver(&server);


    wslog_init(); // start sending logs via websocket if enabled

    ESP_LOGI(TAG, "HTTP started on port %d with %d handlers of %d max", config.server_port, handler_count, config.max_uri_handlers);
    return ESP_OK;
    
}
