#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"  // Use old API
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "hw/display.h"
#include "hw/m5stack_pmic.h"

#include "lvgl.h"

// Panel wiring — M5StickS3 ST7789P3
#define LCD_MOSI_PIN         GPIO_NUM_39
#define LCD_SCLK_PIN         GPIO_NUM_40
#define LCD_DC_PIN           GPIO_NUM_45
#define LCD_CS_PIN           GPIO_NUM_41
#define LCD_RST_PIN          GPIO_NUM_21
#define LCD_BL_PIN           GPIO_NUM_38
#define LCD_HOST             SPI3_HOST
#define LCD_PIXEL_CLOCK_HZ   (40 * 1000 * 1000)
#define LCD_OFFSET_X         52
#define LCD_OFFSET_Y         40
#define DISPLAY_W            CONFIG_BLESPLOIT_DISPLAY_WIDTH
#define DISPLAY_H            CONFIG_BLESPLOIT_DISPLAY_HEIGHT

static const char *TAG = "M5StickS3_Display";
static spi_device_handle_t lcd_spi = NULL;

static esp_pm_lock_handle_t display_pm_lock = NULL;

// LVGL callback
static void (*flush_ready_callback)(void) = NULL;


void display_set_flush_ready_cb(void (*callback)(void))
{
    flush_ready_callback = callback;
}

// ── ST7789P3 Display Functions ────────────────────────────────────────────────── 

#define ST7789_CMD_SWRESET  0x01
#define ST7789_CMD_SLPOUT   0x11
#define ST7789_CMD_COLMOD   0x3A
#define ST7789_CMD_MADCTL   0x36
#define ST7789_CMD_CASET    0x2A
#define ST7789_CMD_RASET    0x2B
#define ST7789_CMD_RAMWR    0x2C
#define ST7789_CMD_DISPON   0x29
#define ST7789_CMD_INVON    0x21

static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(LCD_DC_PIN, 0);
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 8,
    };
    t.tx_data[0] = cmd;
    ESP_ERROR_CHECK(spi_device_transmit(lcd_spi, &t));
}

static void lcd_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(LCD_DC_PIN, 1);
    spi_transaction_t t = {
        .tx_buffer = data,
        .length = len * 8,
    };
    ESP_ERROR_CHECK(spi_device_transmit(lcd_spi, &t));
}

static void IRAM_ATTR spi_post_transfer_callback(spi_transaction_t *trans)
{
    if (flush_ready_callback) {
        flush_ready_callback();
    }
}

static void lcd_hw_reset(void)
{
    gpio_set_level(LCD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void lcd_set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint16_t xs = x0 + LCD_OFFSET_X;
    uint16_t xe = x1 + LCD_OFFSET_X;
    uint16_t ys = y0 + LCD_OFFSET_Y;
    uint16_t ye = y1 + LCD_OFFSET_Y;

    uint8_t data[4];

    lcd_cmd(ST7789_CMD_CASET);
    data[0] = xs >> 8;
    data[1] = xs & 0xFF;
    data[2] = xe >> 8;
    data[3] = xe & 0xFF;
    lcd_data(data, 4);

    lcd_cmd(ST7789_CMD_RASET);
    data[0] = ys >> 8;
    data[1] = ys & 0xFF;
    data[2] = ye >> 8;
    data[3] = ye & 0xFF;
    lcd_data(data, 4);

    lcd_cmd(ST7789_CMD_RAMWR);
}

void display_fill_color(uint16_t color)
{
    lcd_set_addr_window(0, 0, DISPLAY_W - 1, DISPLAY_H - 1);

    size_t pixels = DISPLAY_W * DISPLAY_H;
    const size_t buf_size = 512; // Larger buffer
    uint16_t buf[buf_size];
    
    uint16_t swapped_color = __builtin_bswap16(color);
    for (int i = 0; i < buf_size; ++i) buf[i] = swapped_color;

    gpio_set_level(LCD_DC_PIN, 1);
    while (pixels > 0) {
        size_t chunk = pixels > buf_size ? buf_size : pixels;
        spi_transaction_t t = {
            .tx_buffer = buf,
            .length = chunk * 16,
        };
        ESP_ERROR_CHECK(spi_device_transmit(lcd_spi, &t));
        pixels -= chunk;
    }
}

void display_push_colors(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t *data)
{
    lcd_set_addr_window(x, y, x + width - 1, y + height - 1);

    gpio_set_level(LCD_DC_PIN, 1);

    size_t total_pixels = width * height;
    const size_t chunk_pixels = 2048;
    static uint16_t chunk_buf[2048];

    lv_color32_t *src = (lv_color32_t *)data;  // treat input as ARGB8888

    size_t offset = 0;
    while (offset < total_pixels) {
        size_t pixels_to_send = (total_pixels - offset) < chunk_pixels ?
                                 (total_pixels - offset) : chunk_pixels;

        for (size_t i = 0; i < pixels_to_send; i++) {
            // Convert ARGB8888 → RGB565 big-endian (ST7789 expects big-endian)
            uint16_t r = src[offset + i].red   >> 3;
            uint16_t g = src[offset + i].green >> 2;
            uint16_t b = src[offset + i].blue  >> 3;
            uint16_t rgb565 = (r << 11) | (g << 5) | b;
            chunk_buf[i] = __builtin_bswap16(rgb565);
        }

        spi_transaction_t t = {
            .tx_buffer = chunk_buf,
            .length = pixels_to_send * 16,
        };
        ESP_ERROR_CHECK(spi_device_transmit(lcd_spi, &t));

        offset += pixels_to_send;
    }

    if (flush_ready_callback) {
        flush_ready_callback();
    }
}


static void lcd_init_panel(void)
{
    lcd_hw_reset();

    lcd_cmd(ST7789_CMD_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(ST7789_CMD_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));

    // 16-bit color
    uint8_t colmod = 0x55;
    lcd_cmd(ST7789_CMD_COLMOD);
    lcd_data(&colmod, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Memory access control (portrait, USB-C at bottom)
    uint8_t madctl = 0x00;
    lcd_cmd(ST7789_CMD_MADCTL);
    lcd_data(&madctl, 1);

    // Inversion ON
    lcd_cmd(ST7789_CMD_INVON);
    vTaskDelay(pdMS_TO_TICKS(10));

    lcd_cmd(ST7789_CMD_DISPON);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "ST7789P3 panel initialized");
}

static void lcd_spi_init(void)
{
    // Configure control pins
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    io_conf.pin_bit_mask = 1ULL << LCD_DC_PIN;
    gpio_config(&io_conf);
    io_conf.pin_bit_mask = 1ULL << LCD_RST_PIN;
    gpio_config(&io_conf);
    io_conf.pin_bit_mask = 1ULL << LCD_CS_PIN;
    gpio_config(&io_conf);

    gpio_set_level(LCD_CS_PIN, 1);
    gpio_set_level(LCD_DC_PIN, 0);

    // SPI bus config
    spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = LCD_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_W * DISPLAY_H * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = LCD_PIXEL_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = LCD_CS_PIN,
        .queue_size = 7,
        .flags = SPI_DEVICE_NO_DUMMY,
        .pre_cb = NULL,
        .post_cb = NULL,
//        .post_cb = spi_post_transfer_callback,  // Add callback
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &devcfg, &lcd_spi));
    
    ESP_LOGI(TAG, "SPI initialized at %d MHz", LCD_PIXEL_CLOCK_HZ / 1000000);
}


static void lcd_backlight_pwm_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .gpio_num = LCD_BL_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    
    ESP_LOGI(TAG, "Backlight PWM initialized");
}

// ── Public API ────────────────────────────────────────────────── 

void lcd_backlight_set(bool on)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 128 : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void lcd_backlight_brightness(uint8_t brightness)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_init(void)
{
    ESP_LOGI(TAG, "========== M5StickS3 Display Init ==========");
    
    // Initialize PMIC (safe to call multiple times)
    esp_err_t ret = pmic_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMIC init failed: %d", ret);
        return;
    }
    
    // Enable LCD power via PMIC
    ret = pmic_lcd_power(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PMIC LCD power enable failed: %d, trying anyway...", ret);
        // Continue anyway - LCD might still work
    }
    
    // Init SPI & panel
    lcd_spi_init();
    lcd_init_panel();
    
    // Init backlight
    lcd_backlight_pwm_init();
    lcd_backlight_brightness(128);
    
    display_fill_color(0x0000);
    
    ESP_LOGI(TAG, "Display ready: %dx%d", DISPLAY_W, DISPLAY_H);
}


void display_enable(void)
{
    // Acquire PM lock BEFORE any display operations
    if (display_pm_lock != NULL) {
        esp_pm_lock_acquire(display_pm_lock);
        ESP_LOGI("Display", "PM lock acquired - light sleep disabled");
    }
    
    // Turn on backlight
    lcd_backlight_set(true);
    
    // Wake up display controller
    // lcd_wake_command();
    
    // Small delay to ensure display is ready
    vTaskDelay(pdMS_TO_TICKS(10));
    
    ESP_LOGI("Display", "Display enabled");
}

void display_disable(void)
{
    // Turn off backlight
    lcd_backlight_set(false);
    
    // Put display to sleep
    // lcd_sleep_command();
    
    // Small delay before releasing lock
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Release PM lock to allow light sleep
    if (display_pm_lock != NULL) {
        esp_pm_lock_release(display_pm_lock);
        ESP_LOGI("Display", "PM lock released - light sleep allowed");
    }
    
    ESP_LOGI("Display", "Display disabled");
}


void display_test_pattern(void)
{
    ESP_LOGI(TAG, "Drawing test pattern...");
    
    // Test 1: Full screen colors
    display_fill_color(0xF800); // Red
    vTaskDelay(pdMS_TO_TICKS(1000));
    display_fill_color(0x07E0); // Green
    vTaskDelay(pdMS_TO_TICKS(1000));
    display_fill_color(0x001F); // Blue
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Test 2: Color bars
    uint16_t colors[] = {
        0xF800, // Red
        0xFFE0, // Yellow
        0x07E0, // Green
        0x07FF, // Cyan
        0x001F, // Blue
        0xF81F, // Magenta
        0xFFFF, // White
        0x0000  // Black
    };
    
    int bar_height = DISPLAY_H / 8;
    for (int i = 0; i < 8; i++) {
        lcd_set_addr_window(0, i * bar_height, DISPLAY_W - 1, (i + 1) * bar_height - 1);
        
        size_t pixels = DISPLAY_W * bar_height;
        uint16_t buf[64];
        for (int j = 0; j < 64; j++) buf[j] = __builtin_bswap16(colors[i]);
        
        gpio_set_level(LCD_DC_PIN, 1);
        while (pixels > 0) {
            size_t chunk = pixels > 64 ? 64 : pixels;
            spi_transaction_t t = {
                .tx_buffer = buf,
                .length = chunk * 16,
            };
            ESP_ERROR_CHECK(spi_device_transmit(lcd_spi, &t));
            pixels -= chunk;
        }
    }
    
    vTaskDelay(pdMS_TO_TICKS(2000));
    display_fill_color(0x0000); // Clear to black
    ESP_LOGI(TAG, "Test pattern complete");
}

void display_blink_color(uint16_t color) {
    display_enable();
    display_fill_color(color);
    vTaskDelay(pdMS_TO_TICKS(500));
    display_disable();
}
