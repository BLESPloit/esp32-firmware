#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

#include "common/storage.h"
#include "common/utils.h"

#include "graphics/graphics.h"

static const char *TAG = "LVGL UI";

extern device_config_t config;

static lv_obj_t *temp_label = NULL;
static lv_timer_t *hide_timer = NULL;


// Forward declarations for the LodePNG functions we use directly:
extern unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h,
                                  const unsigned char *in, size_t insize);
extern const char *lodepng_error_text(unsigned code);


// ── BLINKING ──────────────────────────────────────────────────────────

static lv_color_t original_bg_color;
static int blink_count_remaining;
static lv_timer_t *blink_timer = NULL;

static void background_blink_timer_cb(lv_timer_t *timer) {
    lv_obj_t *scr = lv_scr_act();
    if (!lv_obj_is_valid(scr)) return;

    if (blink_count_remaining % 2 == 0) {
        // Even steps: blink color ON
        uint32_t blink_color_hex = (uint32_t)(uintptr_t)lv_timer_get_user_data(timer);
        lv_obj_set_style_bg_color(scr, lv_color_hex(blink_color_hex), 0);
    } else {
        // Odd steps: restore original
        lv_obj_set_style_bg_color(scr, original_bg_color, 0);
    }

    // Make sure LVGL redraws the screen
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_invalidate(scr);

    blink_count_remaining--;
    if (blink_count_remaining <= 0) {
        // Ensure original color is restored on final step
        lv_obj_set_style_bg_color(scr, original_bg_color, 0);
        lv_obj_invalidate(scr);
        blink_timer = NULL;  // Timer auto-deletes via repeat count
    }
}

void ui_blink_screen_background(lv_color_t blink_color, uint16_t times, uint16_t period_ms) {
    ESP_LOGI(TAG, "Blinking screen background 0x%06lx %d times, %dms/period",
             lv_color_to_u32(blink_color), times, period_ms);

    if (lvgl_lock(-1)) {
        lv_obj_t *scr = lv_scr_act();
        original_bg_color = lv_obj_get_style_bg_color(scr, 0);

        // Ensure bg is opaque so color is visible
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

        if (blink_timer) {
            lv_timer_del(blink_timer);
            blink_timer = NULL;
        }

        blink_count_remaining = times * 2;  // Reset counter fresh every call

        uint32_t blink_color_hex = lv_color_to_u32(blink_color) & 0x00FFFFFF;
        blink_timer = lv_timer_create(background_blink_timer_cb, period_ms, (void *)(uintptr_t)blink_color_hex);
        lv_timer_set_repeat_count(blink_timer, times * 2);

        lvgl_unlock();
    }
}


// ── TEMPORARY TEXT LABEL ──────────────────────────────────────────────────────────


void hide_temp_label_timer_cb(lv_timer_t *timer) {
    lv_obj_t *label = (lv_obj_t *)lv_timer_get_user_data(timer);
    
    if (label != NULL && lv_obj_is_valid(label)) {
        lv_obj_del(label);
    }
    
    if (label == temp_label) {
        temp_label = NULL;
    }
    
    hide_timer = NULL;
}


void disp_show_temp_label(const char *text, uint16_t milliseconds) {
    ESP_LOGI("UI", "Showing temp label: %s", text);
    

    if (lvgl_lock(-1)) {
        // Cancel previous timer
        if (hide_timer != NULL) {
            lv_timer_del(hide_timer);
            hide_timer = NULL;
        }
        // Delete old label
        if (temp_label != NULL && lv_obj_is_valid(temp_label)) {
            lv_obj_del(temp_label);
            temp_label = NULL;
        }
        
       // Create semi-transparent background container
        lv_obj_t *container = lv_obj_create(lv_layer_top()); // ensure it floats above other objects
        lv_obj_set_size(container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(container, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(container, LV_OPA_70, 0);  // 70% opacity
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 5, 0);
        lv_obj_set_style_radius(container, 10, 0);
//        lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);  // Disable scrolling
//        lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);
//        lv_obj_set_style_width(container, 0, LV_PART_SCROLLBAR);  // Set scrollbar width to 0

        // Create label inside container
        lv_obj_t *label = lv_label_create(container);
        lv_label_set_text(label, text);
        
        // Enable text wrapping
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        
        // Set maximum width (e.g., 80% of screen width)
        int32_t max_width = (int32_t)(lv_obj_get_width(lv_scr_act()) * 0.8);
        lv_obj_set_width(label, max_width);
        
        // Style the label
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        
        lv_obj_center(label);
        
        // Center container on screen
        lv_obj_center(container);
        
        temp_label = container;

        // if timeout provided, create one-shot timer to hide label
        if (milliseconds > 0 ) {        
            hide_timer = lv_timer_create(hide_temp_label_timer_cb, milliseconds, container);
            lv_timer_set_repeat_count(hide_timer, 1);
        }

        lvgl_unlock();
    }
    
}


// ── ANIMATIONS ──────────────────────────────────────────────────────────

typedef struct {
    int32_t base_x;
    int32_t base_y;
} anim_ctx_t;

// Maps speed 1..10 → duration_ms 1000..300 (linear inverse)
static uint32_t speed_to_duration(gfx_anim_speed_t speed)
{
    if (speed < ANIM_SPEED_MIN) speed = ANIM_SPEED_MIN;
    if (speed > ANIM_SPEED_MAX) speed = ANIM_SPEED_MAX;
    /* speed=1  → 1000ms, speed=10 → 300ms */
    return 1000 - (speed - 1) * (1000 - 300) / (ANIM_SPEED_MAX - ANIM_SPEED_MIN);
}

static void anim_bounce_exec_cb(void *var, int32_t val)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    if (!lv_obj_is_valid(obj)) return;
    anim_ctx_t *ctx = (anim_ctx_t *)lv_obj_get_user_data(obj);
    if (!ctx) return;

    // val is always the animated axis; the other axis stays fixed at base
    lv_obj_set_pos(obj, ctx->base_x, val);
}

static void anim_bounce_x_exec_cb(void *var, int32_t val)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    if (!lv_obj_is_valid(obj)) return;
    anim_ctx_t *ctx = (anim_ctx_t *)lv_obj_get_user_data(obj);
    if (!ctx) return;
    lv_obj_set_pos(obj, val, ctx->base_y);
}

static void anim_bounce_ready_cb(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)a->var;
    anim_ctx_t *ctx = (anim_ctx_t *)lv_obj_get_user_data(obj);
    if (lv_obj_is_valid(obj) && ctx)
        lv_obj_set_pos(obj, ctx->base_x, ctx->base_y);
    free(ctx);
    lv_obj_set_user_data(obj, NULL);
}

void disp_animate(lv_obj_t *obj, gfx_anim_dir_t dir,
                  gfx_anim_speed_t speed, uint16_t repeat_count)
{
    if (!obj || !lv_obj_is_valid(obj)) return;

    // Cancel any running animation on this object 
    lv_anim_delete(obj, anim_bounce_exec_cb);
    lv_anim_delete(obj, anim_bounce_x_exec_cb);
    anim_ctx_t *old_ctx = (anim_ctx_t *)lv_obj_get_user_data(obj);
    if (old_ctx) { free(old_ctx); }

    anim_ctx_t *ctx = calloc(1, sizeof(anim_ctx_t));
    if (!ctx) return;

    lv_obj_update_layout(obj);
    ctx->base_x = lv_obj_get_x(obj);
    ctx->base_y = lv_obj_get_y(obj);
    lv_obj_set_user_data(obj, ctx);

    const uint32_t duration_ms = speed_to_duration(speed);
    const int32_t  lift        = 8;   

    int32_t val_start, val_end;
    lv_anim_exec_xcb_t exec_cb;

    switch (dir) {
        case ANIM_DIR_UP:
            val_start = ctx->base_y - lift;
            val_end   = ctx->base_y;
            exec_cb   = anim_bounce_exec_cb;
            break;
        case ANIM_DIR_DOWN:
            val_start = ctx->base_y + lift;
            val_end   = ctx->base_y;
            exec_cb   = anim_bounce_exec_cb;
            break;
        case ANIM_DIR_LEFT:
            val_start = ctx->base_x - lift;
            val_end   = ctx->base_x;
            exec_cb   = anim_bounce_x_exec_cb;
            break;
        case ANIM_DIR_RIGHT:
            val_start = ctx->base_x + lift;
            val_end   = ctx->base_x;
            exec_cb   = anim_bounce_x_exec_cb;
            break;
        default:
            free(ctx);
            lv_obj_set_user_data(obj, NULL);
            return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_time(&anim, duration_ms);
    lv_anim_set_repeat_count(&anim, repeat_count);
    lv_anim_set_exec_cb(&anim, exec_cb);
    lv_anim_set_values(&anim, val_start, val_end);
    lv_anim_set_path_cb(&anim, lv_anim_path_bounce);
    lv_anim_set_ready_cb(&anim, anim_bounce_ready_cb);

    lv_anim_start(&anim);
}


// ── PNG ──────────────────────────────────────────────────────────

lv_obj_t *disp_render_png(const uint8_t *png_buf, size_t png_len, int x, int y, int w, int h)
{
    ESP_LOGI(TAG, "Loading PNG from buf at %d,%d size %dx%d", x, y, w, h);
    if (!png_buf || png_len == 0) {
        ESP_LOGE(TAG, "disp_render_png: no preloaded buffer");
        return NULL;
    }

    if (!lvgl_lock(-1)) return NULL;

    // Wrap raw PNG bytes — LVGL's built-in decoder handles the rest
    static lv_image_dsc_t dsc; // confirm it works with multiple PNGs
    memset(&dsc, 0, sizeof(dsc));
    dsc.header.magic   = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf      = LV_COLOR_FORMAT_RAW;
    dsc.header.w       = 1;      // placeholder — decoder fills real size
    dsc.header.h       = 1;
    dsc.data_size      = png_len;
    dsc.data           = png_buf;  // points into PSRAM preload buffer — not freed here

    lv_obj_t *img = lv_image_create(lv_screen_active());
    lv_obj_set_style_pad_all(img, 0, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(img, LV_SCROLLBAR_MODE_OFF);

    lv_image_set_src(img, &dsc);

    if (w > 0) {
        lv_image_header_t hdr = {0};
        if (lv_image_decoder_get_info(&dsc, &hdr) == LV_RESULT_OK
            && hdr.w > 0 && hdr.h > 0)
        {
            int th     = (h > 0) ? h : w;
            int zoom_w = (w  * 256) / (int)hdr.w;
            int zoom_h = (th * 256) / (int)hdr.h;
            int zoom   = zoom_w < zoom_h ? zoom_w : zoom_h;
            lv_image_set_scale(img, (uint32_t)zoom);

            // Clamp object size to the scaled visual size so LVGL
            // doesn't see an oversized bounding box and add scrollbars
            int scaled_w = ((int)hdr.w * zoom) / 256;
            int scaled_h = ((int)hdr.h * zoom) / 256;
            lv_obj_set_size(img, scaled_w, scaled_h);
        }
    }

    lv_obj_align(img, LV_ALIGN_TOP_LEFT, x, y);

// Debug positioning after resize
//    lv_obj_update_layout(img);   // force layout recalc before reading back
//    ESP_LOGI(TAG, "PNG pos after align: x=%ld y=%ld w=%ld h=%ld",
//        (long)lv_obj_get_x(img),
//        (long)lv_obj_get_y(img),
//        (long)lv_obj_get_width(img),
//        (long)lv_obj_get_height(img));

    lv_obj_invalidate(img);

    lvgl_unlock();
    return img;

}


// ── TEXT ──────────────────────────────────────────────────────────

lv_obj_t *disp_render_text(const char *text, int x, int y, lv_align_t lvalign, uint8_t size_pct, uint32_t color)
{
    if (!lvgl_lock(-1)) return NULL;

    int off_x = x * (int)g_display.w / 100;
    int off_y = y * (int)g_display.h / 100;

    // ── background container ─────────────────────────────────────────── 
    lv_obj_t *bg = lv_obj_create(lv_scr_act());
    lv_obj_set_style_bg_color(bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_40, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 2, 0); 
    lv_obj_align(bg, lvalign, off_x, off_y);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    int32_t max_width = (int32_t)(g_display.w * 0.8f);
    lv_obj_set_width(bg,  max_width);
    lv_obj_set_height(bg, LV_SIZE_CONTENT);

    // ── label ───────────────────────────────────────────────────────── 
    lv_obj_t *label = lv_label_create(bg);
    if (label) {
        static lv_style_t style;
        lv_style_init(&style);

        const lv_font_t *font;
        switch (size_pct) {
            case 0  ... 5:  font = &lv_font_montserrat_8;  break;
            case 6  ... 10: font = &lv_font_montserrat_12; break;
            case 11 ... 20: font = &lv_font_montserrat_16; break;
            default:        font = &lv_font_montserrat_22; break;
        }
        if (!font) font = LV_FONT_DEFAULT;

        lv_style_set_text_font(&style, font);
        lv_style_set_text_font(&style, &lv_font_montserrat_14);
        lv_style_set_text_color(&style, lv_color_hex(color));
        lv_obj_add_style(label, &style, 0);

        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, LV_PCT(100)); 

        lv_label_set_text(label, text);
        lv_obj_update_layout(label);

        lv_obj_center(label);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    }

    lvgl_unlock();
    return bg;
}

// Update existing text element
 bool disp_update_text(lv_obj_t *bg, const char *text, uint32_t color)
{
    if (!bg || !lv_obj_is_valid(bg)) return false;
    if (!lvgl_lock(-1))              return false;

    // First child of bg is the label
    lv_obj_t *label = lv_obj_get_child(bg, 0);
    if (label && lv_obj_is_valid(label)) {
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);

        lv_obj_update_layout(label);
        int pad = 2;
        lv_obj_set_size(bg,
                        lv_obj_get_width(label)  + pad * 2,
                        lv_obj_get_height(label) + pad * 2);
        lv_obj_center(label);
        lv_obj_invalidate(bg);
    }

    lvgl_unlock();
    return true;
}


// ── GENERIC ──────────────────────────────────────────────────────────

void disp_set_background(uint32_t color)
{
    if (!lvgl_lock(-1)) return;
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
    lvgl_unlock();
}

void disp_set_element_color(const gfx_element_t *e, uint32_t color)
{
    // For now color change triggers a re-render from gfx layer
    (void)e; (void)color;
}


// ── disp_clear_screen ─────────────────────────────────────────────────────
// Wipes all objects AND the registry.
void disp_clear_screen(void)
{
    if (!lvgl_lock(-1)) return;
    lv_obj_clean(lv_scr_act());
    // Reset screen background to default (transparent/black)
    // so it doesn't persist into the next simulation
    lv_obj_remove_style_all(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
    lv_obj_invalidate(lv_scr_act());
    lvgl_unlock();
}


// ── delete LVGL object ────────────────────────────────────────────────────
static void generic_delete_timer_cb(lv_timer_t *timer) {
    lv_obj_t *obj = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (obj && lv_obj_is_valid(obj))
        lv_obj_del(obj);
}

void disp_delete_obj(lv_obj_t *obj) {
    if (!obj) return;
    lv_timer_t *t = lv_timer_create(generic_delete_timer_cb, 0, obj);
    lv_timer_set_repeat_count(t, 1);
}