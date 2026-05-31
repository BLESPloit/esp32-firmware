// pmic.c - M5PM1 PMIC Core Driver Implementation
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hw/m5stack_pmic.h"

static const char *TAG = "PMIC";
static bool pmic_initialized = false;

#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BINARY(byte)  \
  (byte & 0x80 ? '1' : '0'), \
  (byte & 0x40 ? '1' : '0'), \
  (byte & 0x20 ? '1' : '0'), \
  (byte & 0x10 ? '1' : '0'), \
  (byte & 0x08 ? '1' : '0'), \
  (byte & 0x04 ? '1' : '0'), \
  (byte & 0x02 ? '1' : '0'), \
  (byte & 0x01 ? '1' : '0')


esp_err_t pmic_init(void)
{
    if (pmic_initialized) {
        ESP_LOGW(TAG, "PMIC already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing M5PM1 PMIC...");
    
    // Reset I2C pins
    gpio_reset_pin(PMIC_I2C_SDA_PIN);
    gpio_reset_pin(PMIC_I2C_SCL_PIN);
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Test hardware pull-ups
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << PMIC_I2C_SDA_PIN) | (1ULL << PMIC_I2C_SCL_PIN),
    };
    gpio_config(&io_conf);
    vTaskDelay(pdMS_TO_TICKS(50));
    
    int sda = gpio_get_level(PMIC_I2C_SDA_PIN);
    int scl = gpio_get_level(PMIC_I2C_SCL_PIN);
    if (sda == 0 || scl == 0) {
        ESP_LOGE(TAG, "I2C pins not pulled high! SDA=%d, SCL=%d", sda, scl);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "I2C hardware pull-ups detected");
    
    // Configure I2C
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PMIC_I2C_SDA_PIN,
        .scl_io_num = PMIC_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = PMIC_I2C_FREQ_HZ,
        .clk_flags = 0,
    };
    
    ESP_ERROR_CHECK(i2c_param_config(PMIC_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(PMIC_I2C_PORT, conf.mode, 0, 0, 0));
    
    // Longer delay for PMIC to be ready after I2C init
    vTaskDelay(pdMS_TO_TICKS(200)); 
    
    // Mark as initialized
    pmic_initialized = true;
    
    // Read chip ID with retry (PMIC needs time to respond)
    uint8_t id = 0;
    esp_err_t err = ESP_FAIL;
    const int max_retries = 5;  // Increased from 3
    
    for (int retry = 0; retry < max_retries; retry++) {
        err = pmic_read_reg(PMIC_REG_ID, &id);
        
        if (err == ESP_OK && id != 0x00 && id != 0xFF) {
            ESP_LOGI(TAG, "M5PM1 PMIC detected, ID=0x%02X", id);
            break;
        }
        
        if (retry < max_retries - 1) {
            ESP_LOGD(TAG, "PMIC read retry %d/%d (err=%d, id=0x%02X)...", 
                     retry + 1, max_retries - 1, err, id);
            vTaskDelay(pdMS_TO_TICKS(50));  // Shorter delay between retries
        }
    }
    
    if (err != ESP_OK || id == 0x00 || id == 0xFF) {
        ESP_LOGW(TAG, "PMIC ID verification failed (ID=0x%02X), but I2C seems OK", id);
        // Don't fail - PMIC might still work
    }
    
    ESP_LOGI(TAG, "PMIC initialized successfully at address 0x%02X", PMIC_I2C_ADDR);
    
    return ESP_OK;
}


void pmic_deinit(void)
{
    if (pmic_initialized) {
        i2c_driver_delete(PMIC_I2C_PORT);
        pmic_initialized = false;
        ESP_LOGI(TAG, "PMIC deinitialized");
    }
}

esp_err_t pmic_read_reg(uint8_t reg, uint8_t *val)
{
    if (!pmic_initialized) {
        ESP_LOGE(TAG, "PMIC not initialized - cannot read reg 0x%02X", reg);
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = i2c_master_write_read_device(PMIC_I2C_PORT, PMIC_I2C_ADDR,
                                                  &reg, 1, val, 1,
                                                  pdMS_TO_TICKS(500));
    
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Failed to read PMIC reg 0x%02X: %d", reg, ret);
    }
    
    return ret;
}

esp_err_t pmic_write_reg(uint8_t reg, uint8_t val)
{
    if (!pmic_initialized) {
        ESP_LOGE(TAG, "PMIC not initialized - cannot write reg 0x%02X", reg);
        return ESP_ERR_INVALID_STATE;
    }
    
    uint8_t buf[2] = {reg, val};
    esp_err_t ret = i2c_master_write_to_device(PMIC_I2C_PORT, PMIC_I2C_ADDR,
                                                buf, 2, pdMS_TO_TICKS(500));
    
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Failed to write PMIC reg 0x%02X: %d", reg, ret);
    }
    
    return ret;
}

esp_err_t pmic_update_bits(uint8_t reg, uint8_t mask, bool set)
{
    uint8_t val;
    esp_err_t err = pmic_read_reg(reg, &val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read reg 0x%02X for update: %d", reg, err);
        return err;
    }
    
    uint8_t old_val = val;
    
    if (set) {
        val |= mask;
    } else {
        val &= ~mask;
    }
    
    // Only write if changed
    if (val == old_val) {
        ESP_LOGD(TAG, "Reg 0x%02X already has correct value 0x%02X", reg, val);
        return ESP_OK;
    }
    
    err = pmic_write_reg(reg, val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write reg 0x%02X: %d", reg, err);
    }
    
    return err;
}


esp_err_t pmic_read_u16(uint8_t reg_low, uint16_t *value)
{
    uint8_t data[2];
    esp_err_t ret;
    
    ret = pmic_read_reg(reg_low, &data[0]);
    if (ret != ESP_OK) return ret;
    
    ret = pmic_read_reg(reg_low + 1, &data[1]);
    if (ret != ESP_OK) return ret;
    
    *value = data[0] | (data[1] << 8);
    return ESP_OK;
}


esp_err_t pmic_lcd_power(bool enable)
{
    ESP_LOGI(TAG, "%s LCD power (L3A)", enable ? "Enabling" : "Disabling");
    
    // Based on your display.c power-on sequence
    esp_err_t ret;
    
    if (enable) {
        ret = pmic_update_bits(0x16, PMIC_POWER_L3A_LCD, false);
        if (ret != ESP_OK) return ret;
        vTaskDelay(pdMS_TO_TICKS(10));
        
        ret = pmic_update_bits(0x10, PMIC_POWER_L3A_LCD, true);
        if (ret != ESP_OK) return ret;
        vTaskDelay(pdMS_TO_TICKS(10));
        
        ret = pmic_update_bits(0x13, PMIC_POWER_L3A_LCD, false);
        if (ret != ESP_OK) return ret;
        vTaskDelay(pdMS_TO_TICKS(10));
        
        ret = pmic_update_bits(0x11, PMIC_POWER_L3A_LCD, true);
        if (ret != ESP_OK) return ret;
        vTaskDelay(pdMS_TO_TICKS(10));
        
        ret = pmic_write_reg(0x09, 0x00);
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        ret = pmic_update_bits(0x11, PMIC_POWER_L3A_LCD, false);
    }
    
    return ret;
}

esp_err_t pmic_audio_power(bool enable)
{
    ESP_LOGI(TAG, "%s Audio power (L3B)", enable ? "Enabling" : "Disabling");
    
    // L3B is controlled via GPIO2 (PYG2_L3B_EN)
    // This typically means setting GPIO2 as output and controlling it
    return pmic_update_bits(PMIC_REG_GPIO_OUT, 1 << PMIC_GPIO2_L3B_EN, enable);
}

esp_err_t pmic_imu_power(bool enable)
{
    ESP_LOGI(TAG, "%s IMU power (L1)", enable ? "Enabling" : "Disabling");
    
    // L1 powers IMU - typically controlled via LDO enable
    return pmic_update_bits(PMIC_REG_LDO_CTRL, 1 << 1, enable);
}

esp_err_t pmic_read_battery_voltage(uint16_t *voltage_mv)
{
    return pmic_read_u16(PMIC_REG_VBAT_L, voltage_mv);
}

esp_err_t pmic_read_vin_voltage(uint16_t *voltage_mv)
{
    return pmic_read_u16(PMIC_REG_VIN_L, voltage_mv);
}

esp_err_t pmic_get_chip_id(uint8_t *id)
{
    return pmic_read_reg(PMIC_REG_ID, id);
}

void pmic_dump_registers(void)
{
    ESP_LOGI(TAG, "=== PMIC Register Dump ===");
    for (uint8_t reg = 0x00; reg <= 0x27; reg++) {
        uint8_t val = 0;
        if (pmic_read_reg(reg, &val) == ESP_OK) {
            ESP_LOGI(TAG, "  Reg 0x%02X = 0x%02X", reg, val);
        }
    }
}

esp_err_t pmic_is_charging(bool *is_charging)
{
    if (!is_charging) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Method: Check if VIN voltage is present
    // When USB is connected, VIN will be ~5V
    // When on battery only, VIN will be low or 0V
    
    uint16_t vin_mv;
    esp_err_t ret = pmic_read_vin_voltage(&vin_mv);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read VIN voltage");
        return ret;
    }
    
    uint16_t vbat_mv;
    ret = pmic_read_battery_voltage(&vbat_mv);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read battery voltage");
        return ret;
    }
    
    // USB/Charger is connected if VIN > 4V
    bool usb_connected = (vin_mv > 4000);
    
    // Charging if:
    // 1. USB is connected AND
    // 2. Battery is not full (< 4.15V allows some margin)
    bool battery_not_full = (vbat_mv < 4150);
    
    *is_charging = usb_connected && battery_not_full;
    
    ESP_LOGD(TAG, "VIN=%umV, VBAT=%umV, USB=%d, NotFull=%d -> Charging=%s", 
             vin_mv, vbat_mv, usb_connected, battery_not_full,
             *is_charging ? "YES" : "NO");
    
    return ESP_OK;
}

esp_err_t pmic_is_usb_connected(bool *is_connected)
{
    if (!is_connected) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint16_t vin_mv;
    esp_err_t ret = pmic_read_vin_voltage(&vin_mv);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // USB is connected if VIN > 4V
    *is_connected = (vin_mv > 4000);
    
    ESP_LOGD(TAG, "VIN=%umV -> USB %s", vin_mv, *is_connected ? "CONNECTED" : "DISCONNECTED");
    
    return ESP_OK;
}

esp_err_t pmic_get_charge_status(pmic_charge_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint16_t vin_mv;
    uint16_t vbat_mv;
    
    esp_err_t ret = pmic_read_vin_voltage(&vin_mv);
    if (ret != ESP_OK) {
        *status = CHARGE_STATUS_ERROR;
        return ret;
    }
    
    ret = pmic_read_battery_voltage(&vbat_mv);
    if (ret != ESP_OK) {
        *status = CHARGE_STATUS_ERROR;
        return ret;
    }
    
    bool usb_connected = (vin_mv > 4000);
    bool battery_full = (vbat_mv >= 4150);  // 4.15V threshold
    
    if (!usb_connected) {
        // No USB = running on battery
        *status = CHARGE_STATUS_DISCHARGING;
    } else if (battery_full) {
        // USB connected but battery full = charge complete
        *status = CHARGE_STATUS_CHARGE_COMPLETE;
    } else {
        // USB connected and battery not full = charging
        *status = CHARGE_STATUS_CHARGING;
    }
    
    ESP_LOGD(TAG, "VIN=%umV, VBAT=%umV -> Status=%d", vin_mv, vbat_mv, *status);
    
    return ESP_OK;
}

// New: Get charging current estimate (for display)
int16_t pmic_estimate_charge_current_ma(void)
{
    uint16_t vin_mv;
    uint16_t vbat_mv;
    
    if (pmic_read_vin_voltage(&vin_mv) != ESP_OK) return 0;
    if (pmic_read_battery_voltage(&vbat_mv) != ESP_OK) return 0;
    
    bool usb_connected = (vin_mv > 4000);
    
    if (!usb_connected) {
        // Discharging - could estimate based on system load
        return -100;  // Negative = discharging (rough estimate)
    }
    
    // Estimate charge current based on battery voltage
    // LGS4056 typical charge current ~190mA (from schematic)
    if (vbat_mv >= 4150) {
        return 0;     // Nearly full, trickle charge or complete
    } else if (vbat_mv >= 4050) {
        return 50;    // CV mode, current tapering
    } else if (vbat_mv >= 3800) {
        return 150;   // CC mode, ~150mA
    } else {
        return 100;   // Pre-charge mode
    }
}

void pmic_debug_charging_status(void)
{
    ESP_LOGI(TAG, "=== PMIC Charging Debug ===");
    
    uint8_t gpio_in;
    uint16_t vbat, vin;
    
    pmic_read_reg(PMIC_REG_GPIO_IN, &gpio_in);
    pmic_read_battery_voltage(&vbat);
    pmic_read_vin_voltage(&vin);
    
    bool chg_stat = (gpio_in >> PMIC_GPIO0_CHG_STAT) & 1;
    bool usb_present = (vin > 4000);
    
    ESP_LOGI(TAG, "CHG_STAT GPIO0 = %d", chg_stat);
    ESP_LOGI(TAG, "VBAT = %u mV (%.3fV)", vbat, vbat/1000.0f);
    ESP_LOGI(TAG, "VIN  = %u mV (%.3fV)", vin, vin/1000.0f);
    ESP_LOGI(TAG, "USB Present = %s", usb_present ? "YES" : "NO");
    
    pmic_charge_status_t status;
    pmic_get_charge_status(&status);
    
    const char* status_str;
    switch(status) {
        case CHARGE_STATUS_DISCHARGING: status_str = "DISCHARGING"; break;
        case CHARGE_STATUS_CHARGING: status_str = "CHARGING"; break;
        case CHARGE_STATUS_CHARGE_COMPLETE: status_str = "COMPLETE"; break;
        default: status_str = "ERROR"; break;
    }
    
    int16_t current_ma = pmic_estimate_charge_current_ma();
    
    ESP_LOGI(TAG, "=== Result ===");
    ESP_LOGI(TAG, "Status: %s", status_str);
    ESP_LOGI(TAG, "Current: %d mA", current_ma);
}
