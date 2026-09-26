#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

#include "graphics/battery_ui.h"
#include "graphics/graphics.h"
#include "hw/m5stack_pmic.h"

static const char *TAG = "battery_ui";

#define BATTERY_UI_PERIOD_MS 10000

static lv_obj_t *bat_bar   = NULL;
static lv_obj_t *bat_label = NULL;
static lv_obj_t *bat_bolt  = NULL;

static lv_color_t bat_color_for_pct(int pct)
{
    if (pct >= 50) return lv_color_hex(0x22CC44);
    if (pct >= 20) return lv_color_hex(0xFFAA00);
    return lv_color_hex(0xFF2222);
}

static void bat_clear_clickable(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

// Caller holds the LVGL lock. pct/charging come from one PMIC poll.
static void bat_apply(int pct, bool charging)
{
    if (bat_bar != NULL && lv_obj_is_valid(bat_bar)) {
        lv_bar_set_value(bat_bar, pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bat_bar, bat_color_for_pct(pct), LV_PART_INDICATOR);
    }

    if (bat_label != NULL && lv_obj_is_valid(bat_label)) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(bat_label, buf);
    }

    if (bat_bolt != NULL && lv_obj_is_valid(bat_bolt)) {
        if (charging) {
            lv_obj_clear_flag(bat_bolt, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(bat_bolt, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Compact chip on lv_layer_top(), top-right. Survives disp_clear_screen(),
// which only cleans the active screen.
//   42% [==  ]|
static void bat_create(void)
{
    lv_obj_t *chip = lv_obj_create(lv_layer_top());
    lv_obj_set_size(chip, 56, 16);
    lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_set_style_bg_color(chip, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_radius(chip, 2, 0);
    lv_obj_set_style_pad_all(chip, 1, 0);
    lv_obj_set_scrollbar_mode(chip, LV_SCROLLBAR_MODE_OFF);
    bat_clear_clickable(chip);

    bat_label = lv_label_create(chip);
    lv_obj_set_style_text_font(bat_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(bat_label, lv_color_hex(0x000000), 0);
    lv_obj_set_width(bat_label, 28);
    lv_obj_set_style_text_align(bat_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_scrollbar_mode(bat_label, LV_SCROLLBAR_MODE_OFF);
    lv_label_set_text(bat_label, "?%");
    lv_obj_align(bat_label, LV_ALIGN_LEFT_MID, 0, 0);
    bat_clear_clickable(bat_label);

    lv_obj_t *bat_body = lv_obj_create(chip);
    lv_obj_set_size(bat_body, 22, 10);
    lv_obj_set_style_bg_opa(bat_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(bat_body, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_border_width(bat_body, 1, 0);
    lv_obj_set_style_radius(bat_body, 2, 0);
    lv_obj_set_style_pad_all(bat_body, 0, 0);
    lv_obj_align_to(bat_body, bat_label, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    bat_clear_clickable(bat_body);

    bat_bar = lv_bar_create(bat_body);
    lv_obj_set_size(bat_bar, 20, 8);
    lv_obj_align(bat_bar, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(bat_bar, 0, 100);
    lv_bar_set_value(bat_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(bat_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bat_bar, 0, 0);
    lv_obj_set_style_radius(bat_bar, 1, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bat_bar, bat_color_for_pct(0), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bat_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    bat_clear_clickable(bat_bar);

    lv_obj_t *bat_cap = lv_obj_create(chip);
    lv_obj_set_size(bat_cap, 2, 5);
    lv_obj_set_style_bg_color(bat_cap, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_bg_opa(bat_cap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bat_cap, 0, 0);
    lv_obj_set_style_radius(bat_cap, 1, 0);
    lv_obj_set_style_pad_all(bat_cap, 0, 0);
    lv_obj_align_to(bat_cap, bat_body, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    bat_clear_clickable(bat_cap);

    bat_bolt = lv_label_create(chip);
    lv_label_set_text(bat_bolt, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(bat_bolt, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(bat_bolt, lv_color_hex(0xFFAA00), 0);
    lv_obj_align_to(bat_bolt, bat_body, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bat_bolt, LV_OBJ_FLAG_HIDDEN);
    bat_clear_clickable(bat_bolt);
}

// Runs from the LVGL task, which already holds lvgl_mux around lv_timer_handler. 
static void battery_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    int pct = 0;
    bool charging = false;
    if (pmic_read_battery_status(&pct, &charging) != ESP_OK) {
        ESP_LOGW(TAG, "PMIC battery read failed");
        return;
    }
    bat_apply(pct, charging);
}

void battery_ui_start(void)
{
    static bool started = false;
    if (started) return;

    int pct = 0;
    bool charging = false;
    bool have = (pmic_read_battery_status(&pct, &charging) == ESP_OK);

    if (!lvgl_lock(-1)) {
        ESP_LOGE(TAG, "LVGL lock failed");
        return;
    }

    bat_create();
    if (have) {
        bat_apply(pct, charging);
    }
    lv_timer_create(battery_timer_cb, BATTERY_UI_PERIOD_MS, NULL);
    started = true;
    lvgl_unlock();
}
