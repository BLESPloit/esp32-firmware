#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

#include "hw/display.h"
#include "common/storage.h"
#include "common/utils.h"

#include "graphics/graphics.h"

static const char *TAG = "LVGL core";

extern device_config_t config;


// #define LVGL_MEM_SIZE (256 * 1024)  // 256KB from PSRAM
#define LVGL_TICK_PERIOD_MS 2
//#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MAX_DELAY_MS 33 // 30 fps
#define LVGL_TASK_MIN_DELAY_MS 5
// #define LVGL_TASK_STACK_SIZE (4 * 1024)
#define LVGL_TASK_STACK_SIZE (60 * 1024)
#define LVGL_TASK_PRIORITY 2
//#define LVGL_TASK_PRIORITY tskIDLE_PRIORITY

#define DISPLAY_FULLRESH false

#define BACKGROUND_COLOR 0xFFFFFF // start with white

#define DISPLAY_PIXELS (CONFIG_BLESPLOIT_DISPLAY_WIDTH * CONFIG_BLESPLOIT_DISPLAY_HEIGHT)

static SemaphoreHandle_t lvgl_mux = NULL;
static lv_display_t *disp = NULL;

// move the whole LVGL task to PSRAM
EXT_RAM_BSS_ATTR static StackType_t  lvgl_task_stack[60 * 1024 / sizeof(StackType_t)];
static StaticTask_t lvgl_task_tcb;

void lv_mem_init(void) {
    // Nothing needed - PSRAM heap is already initialized by ESP-IDF
}

void *lv_malloc_core(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *ptr, size_t size) {
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_free_core(void *ptr) {
    heap_caps_free(ptr);
}

void lvgl_flush_ready_callback(void) {
    lv_display_flush_ready(disp);  
}

static void lvgl_flush_cb(lv_display_t *disp_handle, const lv_area_t *area, uint8_t *px_map) {
    uint16_t w = area->x2 - area->x1 + 1;
    uint16_t h = area->y2 - area->y1 + 1;
    display_push_colors(area->x1, area->y1, w, h, px_map); 
}

static void increase_lvgl_tick(void *arg)
{
    // Tell LVGL how many milliseconds has elapsed 
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

bool lvgl_lock(int timeout_ms)
{
    // Convert timeout in milliseconds to FreeRTOS ticks
    // If `timeout_ms` is set to -1, the program will block until the condition is met
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks) == pdTRUE;
}

void lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(lvgl_mux);
}

static void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    while (1) {
        if (lvgl_lock(-1)) {
            uint32_t task_delay_ms = lv_timer_handler();
            lvgl_unlock();

            // Clamp delay so IDLE task always gets CPU time 
            if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS)
                task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
            if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS)
                task_delay_ms = LVGL_TASK_MIN_DELAY_MS;

            // Yield OUTSIDE the mutex — critical for IDLE watchdog 
            vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}



void lvgl_init(void)
{
    ESP_LOGI("LVGL", "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    ESP_LOGI(TAG, "Initialize LVGL library");

    lv_init();

    lv_lodepng_init();

    display_set_flush_ready_cb(lvgl_flush_ready_callback);

    lv_color_t *buf1 = heap_caps_malloc(DISPLAY_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_color_t *buf2 = heap_caps_malloc(DISPLAY_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(buf1 && "LVGL draw buf1 PSRAM alloc failed");
    assert(buf2 && "LVGL draw buf2 PSRAM alloc failed");

    ESP_LOGI("LVGL", "buf1=%p buf2=%p each=%d bytes",
             buf1, buf2, (int)(DISPLAY_PIXELS * sizeof(lv_color_t)));

    disp = lv_display_create(CONFIG_BLESPLOIT_DISPLAY_WIDTH, CONFIG_BLESPLOIT_DISPLAY_HEIGHT);
    lv_timer_set_period(lv_display_get_refr_timer(disp), 33); // 33 fps
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2,
        DISPLAY_PIXELS * sizeof(lv_color_t),
        LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick",
        .skip_unhandled_events = false
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    assert(lvgl_mux);

    ESP_LOGI(TAG, "Display LVGL");
    ESP_LOGI(TAG, "Create LVGL task");

    xTaskCreateStaticPinnedToCore(
        lvgl_port_task,
        "LVGL",
        60 * 1024 / sizeof(StackType_t),  // stack depth in words
        NULL,
        LVGL_TASK_PRIORITY,
        lvgl_task_stack,   // stack buffer — put this in PSRAM
        &lvgl_task_tcb,
        1                  // core 1, keep core 0 for WiFi/BLE
    );

}
