#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>

#include "hw/named_gpio.h"

static const char *TAG = "named_gpio";

typedef struct {
    int gpio_num;
    bool configured;
} named_gpio_entry_t;

static named_gpio_entry_t s_gpios[NAMED_GPIO_MAX];

static const char *named_gpio_id_str(named_gpio_id_t id)
{
    return id == NAMED_GPIO_A ? "gpio_a" : "gpio_b";
}

esp_err_t named_gpio_init(void)
{
    for (int i = 0; i < NAMED_GPIO_MAX; i++) {
        s_gpios[i].gpio_num = -1;
        s_gpios[i].configured = false;
    }
    return ESP_OK;
}

esp_err_t named_gpio_add(named_gpio_id_t id, int gpio_num)
{
    if (id >= NAMED_GPIO_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (gpio_num < 0) {
        return ESP_OK;
    }

    s_gpios[id].gpio_num = gpio_num;
    s_gpios[id].configured = false;
    ESP_LOGI(TAG, "Registered %s on GPIO %d", id == NAMED_GPIO_A ? "gpio_a" : "gpio_b", gpio_num);
    return ESP_OK;
}

static esp_err_t named_gpio_ensure_output(named_gpio_id_t id)
{
    named_gpio_entry_t *entry = &s_gpios[id];

    if (entry->gpio_num < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (entry->configured) {
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << entry->gpio_num),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        return ret;
    }

    entry->configured = true;
    return ESP_OK;
}

esp_err_t named_gpio_set(named_gpio_id_t id, int level)
{
    if (id >= NAMED_GPIO_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = named_gpio_ensure_output(id);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_level((gpio_num_t)s_gpios[id].gpio_num, level ? 1 : 0);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "set %s (GPIO %d) -> %d", named_gpio_id_str(id),
                 s_gpios[id].gpio_num, level ? 1 : 0);
    }
    return ret;
}

esp_err_t named_gpio_get(named_gpio_id_t id, int *level)
{
    if (id >= NAMED_GPIO_MAX || level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_gpios[id].gpio_num < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    *level = gpio_get_level((gpio_num_t)s_gpios[id].gpio_num);
    ESP_LOGI(TAG, "get %s (GPIO %d) -> %d", named_gpio_id_str(id),
             s_gpios[id].gpio_num, *level);
    return ESP_OK;
}

esp_err_t named_gpio_from_name(const char *name, named_gpio_id_t *out)
{
    if (name == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(name, "gpio_a") == 0) {
        *out = NAMED_GPIO_A;
        return ESP_OK;
    }
    if (strcmp(name, "gpio_b") == 0) {
        *out = NAMED_GPIO_B;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
