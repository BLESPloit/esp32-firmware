#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"

#include "interface/interface_sim.h"
#include "graphics/graphics.h"
#include "common/storage.h"

#include "hw/button.h"

static const char *TAG = "button";

#define MAX_BUTTONS         4
#define BUTTON_DEBOUNCE_MS  50
#define LONG_PRESS_MS       2000

typedef void (*button_callback_t)(button_id_t button_id, my_button_event_t event);

typedef struct {
    button_id_t button_id;
    my_button_event_t event;
} button_event_msg_t;

typedef struct {
    gpio_num_t gpio_num;
    bool active_level;          // 0 for active low, 1 for active high
    bool enabled;
    bool last_state;
    bool current_state;
    TickType_t press_time;
    TickType_t last_release_time;
    TimerHandle_t debounce_timer;
    bool long_press_triggered;
    uint8_t click_count;
    TimerHandle_t double_click_timer;
} button_t;

static struct {
    button_t buttons[MAX_BUTTONS];
    uint8_t button_count;
    QueueHandle_t event_queue;
    TaskHandle_t event_task;
} button_manager = {0};

static void send_button_event(button_id_t button_id, my_button_event_t event)
{
    button_event_msg_t msg = {
        .button_id = button_id,
        .event = event
    };
    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (xPortInIsrContext()) {
        xQueueSendFromISR(button_manager.event_queue, &msg, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    } else {
        xQueueSend(button_manager.event_queue, &msg, 0);
    }
}

// Here the function calls for the buttons: single/double/long press
static void button_event_task(void *param)
{
    button_event_msg_t msg;
    
    while (1) {
        if (xQueueReceive(button_manager.event_queue, &msg, portMAX_DELAY)) {
            // Handle logging and callbacks in task context (safe)
            switch (msg.event) {
                case BUTTON_EVENT_PRESS:
                    ESP_LOGI(TAG, "Button %d pressed", msg.button_id);
                    break;
                case BUTTON_EVENT_RELEASE:
                    // Don't log the release events (reduce spam)
                    break;
                case BUTTON_EVENT_SINGLE_CLICK:
                    ESP_LOGI(TAG, "Button %d: Single click", msg.button_id);
                    interface_handle_physical_button(msg.button_id, "single");
                    break;
                case BUTTON_EVENT_DOUBLE_CLICK:
                    ESP_LOGI(TAG, "Button %d: Double click", msg.button_id);
                    interface_handle_physical_button(msg.button_id, "double");
                    break;
                case BUTTON_EVENT_LONG_PRESS:
                    interface_handle_physical_button(msg.button_id, "long");
                    break;
                default:
                    break;
            }
            
        }
    }
}

static void button_double_click_timer_callback(TimerHandle_t xTimer)
{
    // Find which button this timer belongs to
    for (int i = 0; i < button_manager.button_count; i++) {
        if (button_manager.buttons[i].double_click_timer == xTimer) {
            button_t *btn = &button_manager.buttons[i];
            
            if (btn->click_count == 1) {
                // Single click confirmed
                send_button_event(i, BUTTON_EVENT_SINGLE_CLICK);
            } else if (btn->click_count >= 2) {
                // Double click confirmed
                send_button_event(i, BUTTON_EVENT_DOUBLE_CLICK);
            }
            
            btn->click_count = 0;
            break;
        }
    }
}

static void button_debounce_timer_callback(TimerHandle_t xTimer)
{
    // Find which button this timer belongs to
    for (int i = 0; i < button_manager.button_count; i++) {
        if (button_manager.buttons[i].debounce_timer == xTimer) {
            button_t *btn = &button_manager.buttons[i];
            
            if (!btn->enabled) continue;
            
            bool gpio_level = gpio_get_level(btn->gpio_num);
            bool button_pressed = (gpio_level == btn->active_level);
            
            if (button_pressed != btn->current_state) {
                btn->current_state = button_pressed;
                
                if (btn->current_state) {
                    // Button pressed
                    btn->press_time = xTaskGetTickCount();
                    btn->long_press_triggered = false;
                    send_button_event(i, BUTTON_EVENT_PRESS);
                } else {
                    // Button released
                    TickType_t current_time = xTaskGetTickCount();
                    TickType_t press_duration = current_time - btn->press_time;
                    
                    send_button_event(i, BUTTON_EVENT_RELEASE);
                    
                    // Check for long press or clicks
                    if (!btn->long_press_triggered) {
                        if (press_duration >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
                            send_button_event(i, BUTTON_EVENT_LONG_PRESS);
                            btn->click_count = 0; // Reset click count
                        } else {
                            // Handle click counting for double-click detection
                            btn->click_count++;
                            btn->last_release_time = current_time;
                            
                            // Start/restart double-click timer
                            xTimerReset(btn->double_click_timer, 0);
                        }
                    }
                }
            }
            break;
        }
    }
}

static void IRAM_ATTR button_isr_handler(void* arg)
{
    button_id_t button_id = (button_id_t)(uintptr_t)arg;
    if (button_id < button_manager.button_count) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xTimerStartFromISR(button_manager.buttons[button_id].debounce_timer, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

static void button_check_long_press_task(void *param)
{
    while (1) {
        for (int i = 0; i < button_manager.button_count; i++) {
            button_t *btn = &button_manager.buttons[i];
            
            if (btn->enabled && btn->current_state && !btn->long_press_triggered) {
                TickType_t current_time = xTaskGetTickCount();
                TickType_t press_duration = current_time - btn->press_time;
                
                if (press_duration >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
                    btn->long_press_triggered = true;
                    send_button_event(i, BUTTON_EVENT_LONG_PRESS);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Check every 100ms
    }
}

esp_err_t button_add(button_id_t button_id, gpio_num_t gpio_num, bool active_level)
{
    if (button_id >= MAX_BUTTONS) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (button_id >= button_manager.button_count) {
        button_manager.button_count = button_id + 1;
    }
    
    button_t *btn = &button_manager.buttons[button_id];
    
    // Configure GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << gpio_num),
        .pull_down_en = active_level ? 1 : 0,
        .pull_up_en = active_level ? 0 : 1,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Initialize button structure
    btn->gpio_num = gpio_num;
    btn->active_level = active_level;
    btn->enabled = true;
    btn->current_state = (gpio_get_level(gpio_num) == active_level);
    btn->last_state = btn->current_state;
    btn->click_count = 0;
    btn->long_press_triggered = false;
    
    // Create debounce timer
    char timer_name[20];
    snprintf(timer_name, sizeof(timer_name), "btn_debounce_%d", button_id);
    btn->debounce_timer = xTimerCreate(
        timer_name,
        pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS),
        pdFALSE,
        NULL,
        button_debounce_timer_callback
    );
    
    if (btn->debounce_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    // Create double-click timer
    snprintf(timer_name, sizeof(timer_name), "btn_dblclk_%d", button_id);
    btn->double_click_timer = xTimerCreate(
        timer_name,
        pdMS_TO_TICKS(300), // 300ms window for double-click
        pdFALSE,
        NULL,
        button_double_click_timer_callback
    );
    
    if (btn->double_click_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    // Install ISR handler
    ret = gpio_isr_handler_add(gpio_num, button_isr_handler, (void*)(uintptr_t)button_id);
    if (ret != ESP_OK) {
        return ret;
    }
    
    ESP_LOGI(TAG, "Button %d initialized on GPIO %d (active %s)", 
             button_id, gpio_num, active_level ? "HIGH" : "LOW");
    
    return ESP_OK;
}

esp_err_t button_init(void)
{
    button_manager.button_count = 0;
    
    // Create event queue
    button_manager.event_queue = xQueueCreate(20, sizeof(button_event_msg_t));
    if (button_manager.event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    // Create event handling task
    if (xTaskCreate(button_event_task, "button_events", 3072, NULL, 5, &button_manager.event_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    
    // Install ISR service
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    
    // Create task for long press detection
    xTaskCreate(button_check_long_press_task, "button_long_press", 2048, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "Button manager initialized");
    return ESP_OK;
}

void button_enable(button_id_t button_id, bool enable)
{
    if (button_id < button_manager.button_count) {
        button_manager.buttons[button_id].enabled = enable;
        ESP_LOGI(TAG, "Button %d %s", button_id, enable ? "enabled" : "disabled");
    }
}
