#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_heap_caps.h"
#include "lvgl.h"


void display_init() {};

void *lv_malloc_core(size_t size)  { return heap_caps_malloc(size, MALLOC_CAP_DEFAULT); }
void  lv_free_core(void *ptr)      { heap_caps_free(ptr); }
void *lv_realloc_core(void *ptr, size_t size) { return heap_caps_realloc(ptr, size, MALLOC_CAP_DEFAULT); }

void lvgl_init(void) {};
bool lvgl_lock(int timeout_ms) { return false; };
void lvgl_unlock(void) {};

void disp_delete_obj(lv_obj_t *obj) {};

// ── Screen-level ops ──────────────────────────────────────────────────────
void disp_set_background(uint32_t color) {};
void disp_clear_screen(void) {};



// ── Transient / notification ──────────────────────────────────────────────
void disp_show_temp_label(const char *text, uint16_t milliseconds) {};

lv_obj_t *disp_render_text(const char *text, int x, int y,
                            lv_align_t lvalign, uint8_t sizepct, uint32_t color) { return NULL; };
bool      disp_update_text(lv_obj_t *bg, const char *text, uint32_t color) { return true;};


// PNG
lv_obj_t *disp_render_png(const uint8_t *png_buf, size_t png_len, int x, int y, int w, int h) { return NULL; };


// lvgl_svg.c
lv_obj_t *disp_render_svg(const char *filepath, const char *id,
                           int x, int y,
                           int32_t target_w, int32_t target_h,
                           bool color_provided, uint32_t color) { return NULL; };
                           

void disp_delete_svg(lv_obj_t *obj) {};
void disp_delete_svg_delayed(lv_obj_t *obj, uint32_t delay_ms) {};