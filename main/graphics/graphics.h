#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#define MAX_GRAPHICS_ELEMENTS 32
#define COLOR_DEBOUNCE_MS  100   // apply color after 100ms of quiet (limits color updates to 10 times per second)

// Animation movement
typedef enum {
    ANIM_DIR_UP,
    ANIM_DIR_DOWN,
    ANIM_DIR_LEFT,
    ANIM_DIR_RIGHT,
} gfx_anim_dir_t;

// Animation speed: 1=slowest (1000ms), 10=fastest (300ms) 
typedef uint8_t gfx_anim_speed_t;
#define ANIM_SPEED_MIN   1
#define ANIM_SPEED_MAX   10

// Alignment anchor — where on the screen the element is pinned
typedef enum {
    GFX_ALIGN_CENTER = 0,
    GFX_ALIGN_TOP_LEFT,
    GFX_ALIGN_TOP_CENTER,
    GFX_ALIGN_TOP_RIGHT,
    GFX_ALIGN_MIDDLE_LEFT,
    GFX_ALIGN_MIDDLE_RIGHT,
    GFX_ALIGN_BOTTOM_LEFT,
    GFX_ALIGN_BOTTOM_CENTER,
    GFX_ALIGN_BOTTOM_RIGHT,
} gfx_align_t;

// Normalized position/size
// align : anchor point on screen
// x, y  : offset from anchor as % of screen_w / screen_h  (-100..100)
// w, h  : element size as % of min(screen_w, screen_h)     (0 = intrinsic)
typedef struct {
    gfx_align_t align;
    int8_t  x;
    int8_t  y;
    uint8_t w;
    uint8_t h;
} gfx_coords_t;

// Physical display size — set once during graphics_init() from JSON
typedef struct {
    uint16_t w;
    uint16_t h;
} gfx_display_t;

extern gfx_display_t g_display;

// Element types
typedef enum {
    ELEM_SVG,
    ELEM_PNG,
    ELEM_TEXT,
} gfx_elem_type_t;

typedef struct {
    char            id[32];
    gfx_elem_type_t type;
    gfx_coords_t    coords;
    bool            color_provided;
    uint32_t        color;
    uint8_t         size;
    char            data[128];
    lv_obj_t       *lvgl_obj;
    lv_timer_t     *pending_render_timer;  // debounce timer for color updates

    // SVG: pre-loaded raw bytes
    char           *svg_buf;
    size_t          svg_len;
    // PNG: pre-loaded raw bytes
    uint8_t         *png_buf;
    size_t          png_len;

} gfx_element_t;

typedef struct {
    gfx_element_t *elements;
    int            element_count;
} graphics_manager_t;

// Init / cleanup
bool           graphics_init(const char *filepath);
void           graphics_cleanup(void);
gfx_element_t *graphics_find_by_id(const char *id);
void           graphics_print_all(graphics_manager_t *manager);

// helper
gfx_align_t gfx_parse_align(const char *s);

// Render API
// coords: pass NULL to use defaults stored in gfx_element_t from graphics.json

void gfx_render_svg(const char *id);

void gfx_render_element(const char *id);
void gfx_remove_element(const char *id);
void gfx_set_background(uint32_t color);
void gfx_set_element_color(const char *id, uint32_t color);
void gfx_set_element_position(const char *id, const gfx_coords_t *coords);
void gfx_clear_screen(void);
void gfx_print_notification(const char *text,
                            gfx_align_t align,
                            int8_t x, int8_t y,
                            uint32_t color,
                            uint8_t size,          // % of min(w,h)
                            uint16_t duration_ms);

void gfx_print_notification_center(const char *text); // default values

bool gfx_add_text_element(const char *id, const char *text,
                                gfx_align_t align, int8_t x, int8_t y,
                                uint32_t color, uint8_t size);
void gfx_render_text(const char *id, const gfx_coords_t *coords, uint32_t color);
void gfx_update_text(const char *id, const char *text, uint32_t color);


// websocket rendering functions (called from interface layer)
void websocket_render_png(const char *id, const char *path, const gfx_coords_t *c);
//void websocket_render_svg(const char *id, const char *svg, const gfx_coords_t *c, uint32_t color);
void websocket_render_svg(const char *id, const char *svg, const gfx_coords_t *c, bool color_provided, uint32_t color);

void websocket_element_remove(const char *element_id);
void websocket_set_background(uint32_t color);
void websocket_set_element_color(const char *element_id, uint32_t color);
void websocket_print_text(const char *id, const char *text, gfx_align_t align,
                          int8_t x, int8_t y, uint32_t color,
                          uint8_t size_pct);
void websocket_print_notification(const char *text,
                                  gfx_align_t align,
                                  int8_t x, int8_t y,
                                  uint32_t color,
                                  uint8_t size,
                                  uint16_t duration);
void websocket_clear_screen(void);


// ── LVGL lifecycle ─────────────────────────────────────────────────────────
void lvgl_init(void);
bool lvgl_lock(int timeout_ms);
void lvgl_unlock(void);

void disp_delete_obj(lv_obj_t *obj);

// ── Screen-level ops ──────────────────────────────────────────────────────
void disp_set_background(uint32_t color);
void disp_clear_screen(void);



// ── Transient / notification ──────────────────────────────────────────────
void disp_show_temp_label(const char *text, uint16_t milliseconds);

lv_obj_t *disp_render_text(const char *text, int x, int y,
                            lv_align_t lvalign, uint8_t sizepct, uint32_t color);
bool      disp_update_text(lv_obj_t *bg, const char *text, uint32_t color);


// PNG
lv_obj_t *disp_render_png(const uint8_t *png_buf, size_t png_len, int x, int y, int w, int h);


void disp_animate(lv_obj_t *obj, gfx_anim_dir_t dir,
                  gfx_anim_speed_t speed, uint16_t repeat_count);

// lvgl_svg.c
lv_obj_t *disp_render_svg(const char *filepath, const char *id,
                           int x, int y,
                           int32_t target_w, int32_t target_h,
                           bool color_provided, uint32_t color);
                           

void disp_delete_svg(lv_obj_t *obj);
void disp_delete_svg_delayed(lv_obj_t *obj, uint32_t delay_ms);

