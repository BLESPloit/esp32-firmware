#include "sdkconfig.h"
#include "driver/gpio.h"
#include "hw/board.h"
#include "hw/button.h"
#include "hw/named_gpio.h"
#include "esp_log.h"

static const char *TAG = "board";

esp_err_t board_register_buttons(void)
{
#if CONFIG_BLESPLOIT_BUTTON_MAIN_GPIO >= 0
    ESP_ERROR_CHECK(button_add(BUTTON_ID_MAIN, (gpio_num_t)CONFIG_BLESPLOIT_BUTTON_MAIN_GPIO, false));
#endif
#if CONFIG_BLESPLOIT_BUTTON_SECOND_GPIO >= 0
    ESP_ERROR_CHECK(button_add(BUTTON_ID_SECOND, (gpio_num_t)CONFIG_BLESPLOIT_BUTTON_SECOND_GPIO, false));
#endif
    ESP_LOGI(TAG, "Buttons: main=%d second=%d", CONFIG_BLESPLOIT_BUTTON_MAIN_GPIO,
             CONFIG_BLESPLOIT_BUTTON_SECOND_GPIO);
    return ESP_OK;
}

esp_err_t board_register_named_gpios(void)
{
#if CONFIG_BLESPLOIT_GPIO_A >= 0
    ESP_ERROR_CHECK(named_gpio_add(NAMED_GPIO_A, CONFIG_BLESPLOIT_GPIO_A));
#endif
#if CONFIG_BLESPLOIT_GPIO_B >= 0
    ESP_ERROR_CHECK(named_gpio_add(NAMED_GPIO_B, CONFIG_BLESPLOIT_GPIO_B));
#endif
    ESP_LOGI(TAG, "Named GPIOs: gpio_a=%d gpio_b=%d",
             CONFIG_BLESPLOIT_GPIO_A, CONFIG_BLESPLOIT_GPIO_B);
    return ESP_OK;
}
