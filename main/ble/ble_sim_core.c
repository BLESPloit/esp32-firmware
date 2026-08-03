#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"


#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "ble/ble_discovery.h"
#include "ble/ble_scan.h"
#include "ble/device_parser.h"
#include "graphics/graphics.h"
#include "interface/interface_sim.h"
#include "lua/lua_hook.h"
#include "common/storage.h"
#include "common/utils.h"

#include "api/web_server.h"
#include "ble/device_manifest.h"
#include "ble/ble_sim.h"
#include "ble/ble_sim_internal.h"

static const char *TAG = "BLE sim - core";

#ifndef CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES
#define CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES 4
#endif

static bool s_ble_sim_teardown = false;

bool ble_sim_teardown_active(void) { return s_ble_sim_teardown; }
void ble_sim_set_teardown(bool active) { s_ble_sim_teardown = active; }

void ble_sim_stop_all_ext_adv(void)
{
    for (int i = 0; i < CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES; i++) {
        int rc = ble_gap_ext_adv_stop(i);
        if (rc != 0 && rc != BLE_HS_EALREADY && rc != BLE_HS_EINVAL && rc != BLE_HS_ENOENT) {
            WS_LOGW(TAG, "ble_gap_ext_adv_stop(%d) rc=%d", i, rc);
        }
    }
}

void ble_sim_clear_ext_adv_sets(void)
{
    int rc = ble_gap_ext_adv_clear();
    if (rc == BLE_HS_EBUSY) {
        ble_sim_stop_all_ext_adv();
        vTaskDelay(pdMS_TO_TICKS(200));
        rc = ble_gap_ext_adv_clear();
    }
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        WS_LOGW(TAG, "ble_gap_ext_adv_clear rc=%d — trying per-instance remove", rc);
        for (int i = 0; i < CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES; i++) {
            int rm = ble_gap_ext_adv_remove(i);
            if (rm != 0 && rm != BLE_HS_EALREADY && rm != BLE_HS_ENOENT) {
                WS_LOGW(TAG, "ble_gap_ext_adv_remove(%d) rc=%d", i, rm);
            }
        }
    }
}

// forward declarations
void ble_store_config_init(void);
esp_err_t stop_ble_simulation(void);

// globals (to be accessed from other files)
ble_server_t *ble_server;

// ── BLE simulation ────────────────────────────────────────────────── 

esp_err_t start_ble_simulation(void) {
    int rc;

    // Stop any active advertising/connections first
    ble_sim_stop_all_ext_adv();

    struct ble_gap_conn_desc desc;
    for (int i = 0; i < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++) {
        if (ble_gap_conn_find(i, &desc) == 0) {
            ble_gap_terminate(desc.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    configure_nimble_security_for_peripheral(ble_server);

    if (ble_server->services != NULL) {
        // Full connectable simulation — reset GATT and register services
        rc = ble_gatts_reset();
        if (rc == BLE_HS_EBUSY) {
            WS_LOGW(TAG, "GATT busy on start — forcing stop first");
            stop_ble_simulation();
            vTaskDelay(pdMS_TO_TICKS(300));
            rc = ble_gatts_reset();
        }
        if (rc != 0) {
            WS_LOGE(TAG, "Failed to reset GATT: %d", rc);
            return ESP_FAIL;
        }

        if (gatt_svr_init(ble_server->services) != 0) {
            WS_LOGE(TAG, "GATT init error");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "GATT services added successfully");
    } else {
        ESP_LOGI(TAG, "Adv-only simulation — skipping GATT init");
    }

    rc = ble_start_advertising();
    if (rc != 0) {
        WS_LOGE(TAG, "Failed to start advertising: %d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}


esp_err_t load_ble_device_for_simulation(const char *device_id) {
    size_t filesize;

    ESP_LOGI(TAG, "Loading for simulation device: %s", device_id);

    // Read manifest.json and resolve all file paths from it
    device_paths_t *paths = manifest_resolve(device_id);
    if (!paths || !paths->peripheral) {
        WS_LOGE(TAG, "Device '%s' has no peripheral role", device_id);
        device_paths_free(paths);
        free(paths);
        return ESP_FAIL;
    }

    // Load and parse BLE services (optional — adv-only devices have no profile)
    if (paths->profile) {
        char *json_buffer = read_json_file(paths->profile, &filesize);
        if (!json_buffer) {
            WS_LOGE(TAG, "Failed to read profile: %s", paths->profile);
            device_paths_free(paths);
            free(paths);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Read %d bytes", filesize);
        ble_server = parse_json_to_nimble(json_buffer, ble_sim_gatt_access_callback);
        free(json_buffer);

        if (ble_server == NULL) {
            WS_LOGE(TAG, "Failed to parse JSON");
            device_paths_free(paths);
            free(paths);
            return ESP_FAIL;
        }
    } else {
        ESP_LOGI(TAG, "No ble.json profile — adv-only simulation");
        ble_server = calloc(1, sizeof(ble_server_t));
        if (!ble_server) {
            WS_LOGE(TAG, "Failed to allocate ble_server");
            device_paths_free(paths);
            free(paths);
            return ESP_FAIL;
        }
    }

    // Load advertising profiles if available
    esp_err_t adv_rc = ble_adv_resolve(device_id);
    if (adv_rc != ESP_OK) {
        WS_LOGE(TAG, "ble_adv_resolve failed: %d — no adv.json and no ble.json devinfo", adv_rc);
        device_paths_free(paths);
        free(paths);
        free_ble_server(&ble_server);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Successfully parsed BLE device");
    ESP_LOGI(TAG, "Number of services: %d", ble_server->service_count);

    // Initialize Lua, graphics, and interface

    bool lua_available = false;
    if (paths->peripheral->entry != NULL) {
        FILE *f = fopen(paths->peripheral->entry, "r");
        if (f) {
            fclose(f);
            lua_available = true;
            esp_err_t lua_load_result = lua_init_persistent_minimal(
                paths->peripheral->entry, false, paths->vars, paths->uuids);

            if (lua_load_result != ESP_OK) {
                WS_LOGE(TAG, "Lua script load error, check the syntax");
            }
        } else {
            WS_LOGW(TAG, "No peripheral script, running static-only simulation");
        }
    }

    if (paths->graphics) graphics_init(paths->graphics);

    if (paths->peripheral->interface) interface_init(paths->peripheral->interface);

    // Only enforce Lua task if we actually tried to start it
    if (lua_available && !lua_is_task_running()) {
        WS_LOGE(TAG, "Lua task failed to start — aborting simulation");
        graphics_cleanup();
        interface_cleanup();
        return ESP_FAIL;
    }

    // show addres also as a fallback if there's no lua (so no graphic)
    bool show_adv_address = paths->peripheral->show_adv_address || !lua_available;

    // paths no longer needed after this point
    device_paths_free(paths);
    free(paths);

    // Start simulation
    esp_err_t ret = start_ble_simulation();
    if (ret != ESP_OK) {
        WS_LOGE(TAG, "BLE simulation failed to start: %d", ret);
        lua_cleanup();
        graphics_cleanup();
        interface_cleanup();
        free_ble_server(&ble_server);
        return ESP_FAIL;
    }

    // show advertising address
    if (show_adv_address && ble_server && ble_server->adv_set.count > 0) {
        ble_adv_params_t *p = &ble_server->adv_set.instances[0].params;

        char addr_text[32] = {0};
        if (p->addr_type == BLE_ADV_ADDR_PUBLIC_HARDWARE) {
            uint8_t hw_addr[6];
            if (ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, hw_addr, NULL) == 0) {
                snprintf(addr_text, sizeof(addr_text),
                        "%02X:%02X:%02X:%02X:%02X:%02X",
                        hw_addr[5], hw_addr[4], hw_addr[3],
                        hw_addr[2], hw_addr[1], hw_addr[0]);
            }
        } else if (p->bd_addr[0] != '\0') {
            strlcpy(addr_text, p->bd_addr, sizeof(addr_text));
        }

        if (addr_text[0] != '\0') {
            char label_text[100] = {0};
            if (p->id[0] != '\0') {
                snprintf(label_text, sizeof(label_text), "%s:%s %s", device_id, p->id, addr_text);
            } else {
                snprintf(label_text, sizeof(label_text), "%s", addr_text);
            }

            ESP_LOGI(TAG, "Showing adv address on display: %s", label_text);
            if ( gfx_add_text_element("_adv_addr", label_text,
                                    GFX_ALIGN_TOP_CENTER, 0, 0,
                                    0xFFFFFF, 5)) 
                gfx_render_text("_adv_addr", NULL, 0xFFFFFF);
        }
    }


    lua_call_handler_async("on_startup", NULL);
    log_memory_usage("BLE sim ready");
    return ESP_OK;
}

esp_err_t stop_ble_simulation(void) {
    int rc = 0;

    ble_sim_set_teardown(true);
    
    ESP_LOGI(TAG, "Stopping BLE simulation...");
    
    // First stop advertising all profiles
    if (ble_server && ble_server->adv_set.instances) {
        for (uint8_t i = 0; i < ble_server->adv_set.count; i++) {
            rc = ble_gap_ext_adv_stop(ble_server->adv_set.instances[i].instance);
            if (rc != 0 && rc != BLE_HS_EALREADY)
                WS_LOGW(TAG, "Failed to stop adv instance %d: %d", i, rc);
        }
    }
    ble_sim_stop_all_ext_adv();

    if (rc == BLE_HS_EALREADY) {
        ESP_LOGI(TAG, "Advertising already stopped");
    } else if (rc != 0) {
        WS_LOGW(TAG, "Failed to stop advertising: %d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising stopped successfully");
    }
    
    // Wait for advertising to fully stop
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Disconnect all connected peers - iterate through all possible handles
    int conn_count = 0;
    struct ble_gap_conn_desc desc;
    
    for (uint16_t handle = 0; handle < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; handle++) {
        if (ble_gap_conn_find(handle, &desc) == 0) {
            ESP_LOGI(TAG, "Found active connection on handle: %d", handle);
            rc = ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
            if (rc == 0) {
                ESP_LOGI(TAG, "Successfully initiated termination for connection %d", handle);
                conn_count++;
            } else {
                WS_LOGW(TAG, "Failed to terminate connection %d: %d", handle, rc);
            }
        }
    }
    
    if (conn_count > 0) {
        ESP_LOGI(TAG, "Terminated %d connections, waiting for cleanup...", conn_count);
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        ESP_LOGI(TAG, "No active connections to terminate");
    }
    
    // Verify all connections are closed
    bool connections_active = false;
    for (uint16_t handle = 0; handle < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; handle++) {
        if (ble_gap_conn_find(handle, &desc) == 0) {
            WS_LOGW(TAG, "Connection %d still active after termination", handle);
            connections_active = true;
        }
    }
    
    if (connections_active) {
        WS_LOGW(TAG, "Some connections still active, waiting additional time...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Check for any active GAP procedures (important after callbacks may restart advertising)
    if (ble_gap_adv_active()) {
        ESP_LOGI(TAG, "Advertising restarted, stopping it...");
        ble_gap_adv_stop();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ble_sim_clear_ext_adv_sets();
    
    if (ble_gap_disc_active()) {
        ESP_LOGI(TAG, "Discovery active, cancelling...");
        ble_gap_disc_cancel();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
       
    // Reset GATT database
    rc = ble_gatts_reset();
    if (rc != 0) {
        WS_LOGE(TAG, "Failed to reset GATT server: %d (%s)", rc, 
                 rc == BLE_HS_EBUSY ? "EBUSY - connections still active" : "other error");
        
        if (rc == BLE_HS_EBUSY) {
            // Last resort: force sync and try one more time
            WS_LOGW(TAG, "Forcing host stack sync and retrying...");
            ble_hs_sched_reset(0);
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            rc = ble_gatts_reset();
            if (rc != 0) {
                WS_LOGE(TAG, "GATT reset still failed after retry: %d", rc);
                ble_sim_set_teardown(false);
                return ESP_ERR_INVALID_STATE;
            }
        } else {
            ble_sim_set_teardown(false);
            return ESP_FAIL;
        }
    }
    
    ESP_LOGI(TAG, "GATT database reset successful");

    free_ble_server(&ble_server);

    ble_sim_set_teardown(false);
    
    ESP_LOGI(TAG, "BLE simulation stopped and memory cleared");
    
    return ESP_OK;
}


// free resources, unload graphics ...
void unload_ble_device_for_simulation(void) {
    ESP_LOGI(TAG, "Unloading BLE device simulation...");

    // Stop BLE simulation and clear GATT structures
    esp_err_t ret = stop_ble_simulation();
    if (ret != ESP_OK) {
        WS_LOGE(TAG, "Failed to stop BLE simulation: %d", ret);
    }

    // Cleanup interface
    interface_cleanup();

    // Cleanup graphics
    graphics_cleanup();

    // Clear vars path before Lua teardown — prevents stale path if a new
    // device loads without vars.json before g_vars_path is re-set.
    lua_vars_set_path("");

    // Cleanup Lua state
    lua_cleanup();

    ESP_LOGI(TAG, "BLE device unloaded successfully");
    log_memory_usage("After BLE sim unload");
}
