// pmic.h - M5PM1 PMIC Core Driver
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// I2C Configuration
#define PMIC_I2C_PORT       0
#define PMIC_I2C_ADDR       0x6E
#define PMIC_I2C_SDA_PIN    47
#define PMIC_I2C_SCL_PIN    48
#define PMIC_I2C_FREQ_HZ    100000

// PMIC Register Map (from M5PM1 datasheet)
#define PMIC_REG_ID             0x00    // Chip ID
#define PMIC_REG_GPIO_MODE      0x01    // GPIO mode control
#define PMIC_REG_GPIO_CTRL      0x02    // GPIO control
#define PMIC_REG_GPIO_IN        0x03    // GPIO input
#define PMIC_REG_GPIO_OUT       0x04    // GPIO output

// Power control registers
#define PMIC_REG_LDO_CTRL       0x10    // LDO control
#define PMIC_REG_DCDC_CTRL      0x11    // DCDC control
#define PMIC_REG_BOOST_CTRL     0x12    // Boost control
#define PMIC_REG_CHARGE_CTRL    0x13    // Charge control

// Power switch control
#define PMIC_REG_L2_CTRL        0x16    // L2 power switch
#define PMIC_REG_L3A_CTRL       0x17    // L3A power switch (LCD)
#define PMIC_REG_L3B_CTRL       0x18    // L3B power switch (Audio)

// ADC registers
#define PMIC_REG_VREF_L         0x20    // Internal Vref (Low byte)
#define PMIC_REG_VREF_H         0x21    // Internal Vref (High byte)
#define PMIC_REG_VBAT_L         0x22    // Battery voltage (Low byte)
#define PMIC_REG_VBAT_H         0x23    // Battery voltage (High byte)
#define PMIC_REG_VIN_L          0x24    // 5VIN voltage (Low byte)
#define PMIC_REG_VIN_H          0x25    // 5VIN voltage (High byte)
#define PMIC_REG_5VOUT_L        0x26    // 5VOUT voltage (Low byte)
#define PMIC_REG_5VOUT_H        0x27    // 5VOUT voltage (High byte)

// Power domain bits (from schematic)
#define PMIC_POWER_L2           (1 << 2)  // Main 3V3_L2 rail
#define PMIC_POWER_L3A_LCD      (1 << 2)  // LCD power (3V3_L3A)
#define PMIC_POWER_L3B_AUDIO    (1 << 2)  // Audio power (3V3_L3B)

// GPIO mapping (from your table and schematic)
typedef enum {
    PMIC_GPIO0_CHG_STAT = 0,    // PYG0_CHG_STAT - Charge status
    PMIC_GPIO1_IRQ = 1,          // PYG1_IRQ - General IRQ
    PMIC_GPIO2_L3B_EN = 2,       // PYG2_L3B_EN - Audio power enable
    PMIC_GPIO3_SPK_PULSE = 3,    // PYG3_SPK_Pulse - Speaker control
    PMIC_GPIO4_IMU_INT = 4,      // PYG4_IMU_INT - IMU interrupt
} pmic_gpio_t;

/**
 * @brief Initialize PMIC I2C communication
 * @return ESP_OK on success
 */
esp_err_t pmic_init(void);

/**
 * @brief Deinitialize PMIC and I2C
 */
void pmic_deinit(void);

/**
 * @brief Read single register from PMIC
 * @param reg Register address
 * @param val Pointer to store value
 * @return ESP_OK on success
 */
esp_err_t pmic_read_reg(uint8_t reg, uint8_t *val);

/**
 * @brief Write single register to PMIC
 * @param reg Register address
 * @param val Value to write
 * @return ESP_OK on success
 */
esp_err_t pmic_write_reg(uint8_t reg, uint8_t val);

/**
 * @brief Read 16-bit value from PMIC (little-endian)
 * @param reg_low Low byte register address
 * @param value Pointer to store 16-bit value
 * @return ESP_OK on success
 */
esp_err_t pmic_read_u16(uint8_t reg_low, uint16_t *value);

/**
 * @brief Update specific bits in register
 * @param reg Register address
 * @param mask Bit mask
 * @param set true to set bits, false to clear
 * @return ESP_OK on success
 */
esp_err_t pmic_update_bits(uint8_t reg, uint8_t mask, bool set);

/**
 * @brief Enable/disable LCD power domain (L3A)
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t pmic_lcd_power(bool enable);

/**
 * @brief Enable/disable Audio power domain (L3B)
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t pmic_audio_power(bool enable);

/**
 * @brief Enable/disable IMU power domain (L1)
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t pmic_imu_power(bool enable);

/**
 * @brief Read battery voltage in millivolts
 * @param voltage_mv Pointer to store voltage
 * @return ESP_OK on success
 */
esp_err_t pmic_read_battery_voltage(uint16_t *voltage_mv);

/**
 * @brief Read 5VIN voltage in millivolts
 * @param voltage_mv Pointer to store voltage
 * @return ESP_OK on success
 */
esp_err_t pmic_read_vin_voltage(uint16_t *voltage_mv);

/**
 * @brief Check if charging
 * @param is_charging Pointer to store charging status
 * @return ESP_OK on success
 */

/**
 * @brief Get PMIC chip ID
 * @param id Pointer to store chip ID
 * @return ESP_OK on success
 */
esp_err_t pmic_get_chip_id(uint8_t *id);
void pmic_dump_registers(void);


/**
 * @brief Estimate charging current in milliamps
 * @return Positive = charging, Negative = discharging, 0 = idle/full
 * @note This is an estimate based on voltage, not actual measurement
 */
int16_t pmic_estimate_charge_current_ma(void);

/**
 * @brief Check if battery is actively charging
 * @param is_charging Pointer to store charging status
 * @return ESP_OK on success
 * @note Returns false when charge is complete (even if USB connected)
 */
esp_err_t pmic_is_charging(bool *is_charging);

/**
 * @brief Check if USB/charger is connected
 * @param is_connected Pointer to store connection status
 * @return ESP_OK on success
 */
esp_err_t pmic_is_usb_connected(bool *is_connected);

/**
 * @brief Charge status enum
 */
typedef enum {
    CHARGE_STATUS_DISCHARGING,     // Running on battery
    CHARGE_STATUS_CHARGING,        // Actively charging
    CHARGE_STATUS_CHARGE_COMPLETE, // USB connected, charge complete
    CHARGE_STATUS_ERROR            // Cannot determine
} pmic_charge_status_t;

/**
 * @brief Get detailed charge status
 * @param status Pointer to store charge status
 * @return ESP_OK on success
 */
esp_err_t pmic_get_charge_status(pmic_charge_status_t *status);



esp_err_t pmic_is_charging(bool *is_charging);
void pmic_debug_charging_status(void);
