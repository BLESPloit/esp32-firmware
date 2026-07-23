#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include <errno.h>
#include "esp_wifi.h"
#include <sys/param.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble/ble_init.h"
#include "ble/ble_scan.h"
#include "hw/button.h"
#include "hw/board.h"
#include "hw/named_gpio.h"
#include "hw/display.h"
#include "api/console.h"
#include "ble/device_parser.h"
#include "lua/lua_hook.h"
#include "graphics/graphics.h"
#include "common/storage.h"
#include "common/utils.h"
#include "api/web_server.h"
#include "api/wifi.h"
#include "api/usb_net.h"

static const char *TAG = "main";

extern device_config_t config;

void print_time(void) {
    char strftime_buf[64];

    timestamp_to_string(time(NULL), strftime_buf, sizeof(strftime_buf));
    ESP_LOGI(TAG, "Current time: %s", strftime_buf);

}

void app_main(void)
{
    esp_err_t ret;

    log_memory_usage("main start");

    // Initialize storage
    ret = initialize_nvs();
    ESP_ERROR_CHECK( ret );

    ret = initialize_littlefs();
    ESP_ERROR_CHECK( ret );

    read_config_nvs();
   
    display_init();
    lvgl_init();

    // Initialize button manager and named GPIOs
    ESP_ERROR_CHECK(button_init());
    ESP_ERROR_CHECK(board_register_buttons());
    ESP_ERROR_CHECK(named_gpio_init());
    ESP_ERROR_CHECK(board_register_named_gpios());

    log_memory_usage("after button");

    initialize_bluetooth();
    log_memory_usage("after initialize bluetooth");

   // Create default event loop (must be before http server)
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_netif_init());

    // WiFi before TinyUSB: esp_wifi_init() hangs if USB OTG stack is up first 
    if (config.net_enabled.value.u8) {
        wifi_init_with_fallback();
    }

    if (!config.usb_jtag_console.value.u8) {
        ESP_ERROR_CHECK(usb_net_init());
    }
    initialize_console();

    // initialize BLE scanning device mutex 
    ble_scanner_init();

    log_memory_usage("main end");

}