#include <sdkconfig.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "driver/spi_master.h"

#include "esp_lcd_gc9a01.h"
#include "hw/display.h"
#include "lvgl.h"

// Lilygo T-QT-PRO wiring
#define LCD_PIXEL_CLOCK_HZ     (20 * 1000 * 1000)
#define LCD_HOST               SPI2_HOST
#define BOARD_SPI_MISO         (-1)
#define BOARD_SPI_MOSI         (2)
#define BOARD_SPI_SCK          (3)
#define BOARD_TFT_CS           (5)
#define BOARD_TFT_RST          (1)
#define BOARD_TFT_DC           (6)
#define BOARD_TFT_BL           (10)

#define DISPLAY_W              CONFIG_BLESPLOIT_DISPLAY_WIDTH
#define DISPLAY_H              CONFIG_BLESPLOIT_DISPLAY_HEIGHT

static const char *TAG = "Display";

static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
static void (*flush_ready_callback)(void) = NULL;


static bool display_on_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                  esp_lcd_panel_io_event_data_t *edata,
                                  void *user_ctx)
{
    if (flush_ready_callback) {
        flush_ready_callback();  // calls lv_display_flush_ready() in lvgl_ui.c
    }
    return false;
}

void display_set_flush_ready_cb(void (*callback)(void))
{
    flush_ready_callback = callback;
}

void display_push_colors(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t *data)
{
    size_t total_pixels = width * height;
    uint16_t *chunk_buf = heap_caps_malloc(total_pixels * sizeof(uint16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(chunk_buf);

    // LVGL 9 XRGB8888 in memory (little-endian): byte0=B, byte1=G, byte2=R, byte3=X
    // GC9A01 panel is configured BGR, so we feed R5G6B5 with R in high bits.
    uint8_t *src = data;
    for (size_t i = 0; i < total_pixels; i++) {
        uint8_t b = src[i*4 + 0];
        uint8_t g = src[i*4 + 1];
        uint8_t r = src[i*4 + 2];
        // RGB565, big-endian (bswap for SPI)
        chunk_buf[i] = __builtin_bswap16(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + width, y + height, chunk_buf);
    heap_caps_free(chunk_buf);
}


void display_init(void)
{
    ESP_LOGI(TAG, "============T-QT-PRO-S3============");

    spi_bus_config_t buscfg = {
        .sclk_io_num = BOARD_SPI_SCK,
        .mosi_io_num = BOARD_SPI_MOSI,
        .miso_io_num = BOARD_SPI_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_W * DISPLAY_H * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BOARD_TFT_DC,
        .cs_gpio_num = BOARD_TFT_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = display_on_trans_done,  // ← Fix #2: correct type
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                              &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_TFT_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    esp_lcd_panel_swap_xy(panel_handle, false);
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true));
    esp_lcd_panel_set_gap(panel_handle, 2, 1);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << BOARD_TFT_BL,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(BOARD_TFT_BL, 0);
}

void lcd_backlight_set(bool on)
{
    gpio_set_level(BOARD_TFT_BL, on ? 1 : 0);
}