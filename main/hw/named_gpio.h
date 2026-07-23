#pragma once

#include "esp_err.h"

typedef enum {
    NAMED_GPIO_A = 0,
    NAMED_GPIO_B = 1,
    NAMED_GPIO_MAX = 2
} named_gpio_id_t;

esp_err_t named_gpio_init(void);
esp_err_t named_gpio_add(named_gpio_id_t id, int gpio_num);
esp_err_t named_gpio_set(named_gpio_id_t id, int level);
esp_err_t named_gpio_get(named_gpio_id_t id, int *level);
esp_err_t named_gpio_from_name(const char *name, named_gpio_id_t *out);
