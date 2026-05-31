#include "esp_log.h"
#include "nvs_flash.h"


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
#include "lua/lua_hook.h"
#include "common/storage.h"
#include "common/utils.h"

static const char *TAG = "BLE init";

struct ble_hs_cfg;

void ble_store_config_init(void);

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "Resetting state; reason=%d\n", reason);
}

static void ble_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    // This function will return only when nimble_port_stop() is executed 
    nimble_port_run();
    nimble_port_freertos_deinit();
    ESP_LOGI(TAG, "BLE Host Task finished");
}

void initialize_bluetooth(void)
{
    esp_err_t ret;

    // assume NVS is already initialized in main (NimBLE neeeds it to store PHY calibration data)

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE %d ", ret);
        return;
    }
    // Initialize the NimBLE host configuration. 
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL; // don't initialize GATT yet

    // GATT will be initialized on demand later, depending on scenario

    // Need to have template for store 
    ble_store_config_init();

    nimble_port_freertos_init(ble_host_task);
    
}
