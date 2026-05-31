#pragma once
#include "driver/gpio.h"

// Button IDs for easy reference
typedef enum {
    BUTTON_ID_MAIN = 0,
    BUTTON_ID_SECOND = 1,
    BUTTON_ID_MAX = 2
} button_id_t;

typedef enum {
    BUTTON_EVENT_NONE,
    BUTTON_EVENT_PRESS,
    BUTTON_EVENT_RELEASE,
    BUTTON_EVENT_SINGLE_CLICK,
    BUTTON_EVENT_DOUBLE_CLICK,
    BUTTON_EVENT_LONG_PRESS
} my_button_event_t;


esp_err_t button_init(void);
esp_err_t button_add(button_id_t button_id, gpio_num_t gpio_num, bool active_level);
