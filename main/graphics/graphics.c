#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "sdkconfig.h"
#include "cJSON.h"
#include "ble/device_parser.h"
#include "common/utils.h"
#include "common/storage.h"
#include "api/web_server.h"
#include "graphics/graphics.h"

static const char *TAG = "graphics";

gfx_display_t g_display = {
    .w = CONFIG_BLESPLOIT_DISPLAY_WIDTH,
    .h = CONFIG_BLESPLOIT_DISPLAY_HEIGHT
};

static graphics_manager_t *g_manager = NULL;
extern char current_simulated_device[128];

// forward declarations
void gfx_clear_screen(void);


// ── HELPERS ────────────────────────────────────────────────── 

gfx_element_t *graphics_find_by_id(const char *id)
{
    if (!g_manager || !id) return NULL;
    for (int i = 0; i < g_manager->element_count; i++)
        if (!strcmp(g_manager->elements[i].id, id))
            return &g_manager->elements[i];
    ESP_LOGW(TAG, "Element '%s' not found", id);
    return NULL;
}

// Helper used in cleanup and remove of LVGL objects
static void gfx_delete_element_obj(gfx_element_t *e) {
    if (!e->lvgl_obj) return;
    if (e->type == ELEM_SVG)
        disp_delete_svg(e->lvgl_obj);
    else
        disp_delete_obj(e->lvgl_obj);
    e->lvgl_obj = NULL;
}

// Helper: map gfx_align_t → lv_align_t for LVGL
lv_align_t gfx_to_lv_align(gfx_align_t a)
{
    switch (a) {
        case GFX_ALIGN_TOP_LEFT:      return LV_ALIGN_TOP_LEFT;
        case GFX_ALIGN_TOP_CENTER:    return LV_ALIGN_TOP_MID;
        case GFX_ALIGN_TOP_RIGHT:     return LV_ALIGN_TOP_RIGHT;
        case GFX_ALIGN_MIDDLE_LEFT:   return LV_ALIGN_LEFT_MID;
        case GFX_ALIGN_MIDDLE_RIGHT:  return LV_ALIGN_RIGHT_MID;
        case GFX_ALIGN_BOTTOM_LEFT:   return LV_ALIGN_BOTTOM_LEFT;
        case GFX_ALIGN_BOTTOM_CENTER: return LV_ALIGN_BOTTOM_MID;
        case GFX_ALIGN_BOTTOM_RIGHT:  return LV_ALIGN_BOTTOM_RIGHT;
        default:                      return LV_ALIGN_CENTER;
    }
}


// ── Coordinate conversion ──────────────────────────────────────────────────
// Converts normalized gfx_coords_t to absolute pixel origin (top-left of element).
// ew/eh: element pixel size — pass 0 if unknown (anchor still applied, offset only)
static void coords_to_px(const gfx_coords_t *c,
                          int ew, int eh,
                          int *out_x, int *out_y)
{
    uint16_t sw = g_display.w, sh = g_display.h;
    int ax, ay;
    switch (c->align) {
        case GFX_ALIGN_TOP_LEFT:   ax = 0;           ay = 0;           break;
        case GFX_ALIGN_TOP_CENTER: ax = sw/2 - ew/2; ay = 0;           break;
        case GFX_ALIGN_TOP_RIGHT:  ax = sw   - ew;   ay = 0;           break;
        case GFX_ALIGN_MIDDLE_LEFT:   ax = 0;           ay = sh/2 - eh/2; break;
        case GFX_ALIGN_MIDDLE_RIGHT:  ax = sw   - ew;   ay = sh/2 - eh/2; break;
        case GFX_ALIGN_BOTTOM_LEFT:   ax = 0;           ay = sh   - eh;   break;
        case GFX_ALIGN_BOTTOM_CENTER: ax = sw/2 - ew/2; ay = sh   - eh;   break;
        case GFX_ALIGN_BOTTOM_RIGHT:  ax = sw   - ew;   ay = sh   - eh;   break;
        default: /* CENTER */      ax = sw/2 - ew/2; ay = sh/2 - eh/2; break;
    }
    *out_x = ax + (c->x * (int)sw) / 100;
    *out_y = ay + (c->y * (int)sh) / 100;
}

// Resolve size % to pixels. Returns 0 if pct==0 (caller uses intrinsic size).
static int size_to_px(uint8_t pct)
{
    if (pct == 0) return 0;
    int base = g_display.w < g_display.h ? g_display.w : g_display.h;
    return (pct * base) / 100;
}

// Resolve coords: use override if non-NULL, else element defaults
static const gfx_coords_t *resolve(const gfx_element_t *e,
                                    const gfx_coords_t *override)
{
    return override ? override : &e->coords;
}

// ── JSON parsing helpers ───────────────────────────────────────────────────
gfx_align_t gfx_parse_align(const char *s)
{
    if (!s)                          return GFX_ALIGN_CENTER;
    if (!strcmp(s, "top_left"))      return GFX_ALIGN_TOP_LEFT;
    if (!strcmp(s, "top_center"))    return GFX_ALIGN_TOP_CENTER;
    if (!strcmp(s, "top_right"))     return GFX_ALIGN_TOP_RIGHT;
    if (!strcmp(s, "middle_left"))      return GFX_ALIGN_MIDDLE_LEFT;
    if (!strcmp(s, "middle_right"))     return GFX_ALIGN_MIDDLE_RIGHT;
    if (!strcmp(s, "bottom_left"))      return GFX_ALIGN_BOTTOM_LEFT;
    if (!strcmp(s, "bottom_center"))    return GFX_ALIGN_BOTTOM_CENTER;
    if (!strcmp(s, "bottom_right"))     return GFX_ALIGN_BOTTOM_RIGHT;
    return GFX_ALIGN_CENTER;
}

static void parse_coords(cJSON *j, gfx_coords_t *c)
{
    c->align = GFX_ALIGN_CENTER;
    c->x = c->y = 0;
    c->w = c->h = 0;

    cJSON *align = cJSON_GetObjectItem(j, "align");
    if (cJSON_IsString(align)) c->align = gfx_parse_align(align->valuestring);

    cJSON *x = cJSON_GetObjectItem(j, "x");
    if (cJSON_IsNumber(x)) c->x = (int8_t)x->valueint;

    cJSON *y = cJSON_GetObjectItem(j, "y");
    if (cJSON_IsNumber(y)) c->y = (int8_t)y->valueint;

    cJSON *w = cJSON_GetObjectItem(j, "w");
    if (cJSON_IsNumber(w)) c->w = (uint8_t)w->valueint;

    cJSON *h = cJSON_GetObjectItem(j, "h");
    if (cJSON_IsNumber(h)) c->h = (uint8_t)h->valueint;
}

// ── Parse graphics.json ────────────────────────────────────────────────────
static graphics_manager_t *parse_graphics_json_file(const char *filepath)
{
    size_t json_size;
    char *json_str = read_json_file(filepath, &json_size);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to read graphics file: %s", filepath);
        return NULL;
    }

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Graphics JSON parse error: %s", cJSON_GetErrorPtr() ?: "unknown");
        return NULL;
    }

    graphics_manager_t *m = calloc(1, sizeof(graphics_manager_t));
    if (!m) { cJSON_Delete(root); return NULL; }

    cJSON *elems = cJSON_GetObjectItem(root, "elements");
    if (!cJSON_IsArray(elems)) {
        ESP_LOGE(TAG, "No elements array in graphics JSON");
        free(m);
        cJSON_Delete(root);
        return NULL;
    }

    int count = cJSON_GetArraySize(elems);
    if (count == 0) {
        ESP_LOGI(TAG, "Elements array is empty");
        m->element_count = 0;
        m->elements = NULL;
        cJSON_Delete(root);
        return m;
    }

    m->elements = calloc(count, sizeof(gfx_element_t));
    if (!m->elements) { free(m); cJSON_Delete(root); return NULL; }

    int valid = 0;
    cJSON *ej;
    cJSON_ArrayForEach(ej, elems) {
        gfx_element_t *e = &m->elements[valid];

        cJSON *id   = cJSON_GetObjectItem(ej, "id");
        cJSON *type = cJSON_GetObjectItem(ej, "type");
        if (!cJSON_IsString(id) || !id->valuestring) {
            ESP_LOGW(TAG, "Element missing id, skipping");
            continue;
        }
        if (!cJSON_IsString(type) || !type->valuestring) {
            ESP_LOGW(TAG, "Element '%s' missing type, skipping", id->valuestring);
            continue;
        }

        // ── id ────────────────────────────────────────────────────────────
        strncpy(e->id, id->valuestring, sizeof(e->id) - 1);

        // ── type ──────────────────────────────────────────────────────────
        const char *t = type->valuestring;
        if      (!strcmp(t, "svg"))  e->type = ELEM_SVG;
        else if (!strcmp(t, "png"))  e->type = ELEM_PNG;
        else if (!strcmp(t, "text")) e->type = ELEM_TEXT;
        else {
            ESP_LOGW(TAG, "Unknown type '%s' for '%s', skipping", t, e->id);
            continue;
        }

        // ── coords (align, x, y, w, h) ────────────────────────────────────
        parse_coords(ej, &e->coords);

        // ── color (hex string "0xRRGGBB" or integer) ──────────────────────
        cJSON *color_j = cJSON_GetObjectItem(ej, "color");
        e->color_provided = true;
        if (cJSON_IsString(color_j)) 
            e->color = (uint32_t)strtoul(color_j->valuestring, NULL, 16);
        else if (cJSON_IsNumber(color_j))
            e->color = (uint32_t)color_j->valueint;
        else  
            e->color_provided = false; // will not override the default color

        // ── size (% of min screen dimension) ──────────────────────────────
        cJSON *sz = cJSON_GetObjectItem(ej, "size");
        e->size = cJSON_IsNumber(sz) ? (uint8_t)sz->valueint : 0;

        // ── data: type-specific field name, all land in e->data[128] ──────
        const char *field;
        if (e->type == ELEM_TEXT) field = "text";
        else field = "file"; // ELEM_SVG, ELEM_PNG

        cJSON *data_j = cJSON_GetObjectItem(ej, field);
        if (cJSON_IsString(data_j))
            strncpy(e->data, data_j->valuestring, sizeof(e->data) - 1);

        ESP_LOGI(TAG, "Loaded %s '%s' data='%s' size=%d color=#%06lX "
                      "align=%d x=%d y=%d w=%d h=%d",
                 t, e->id, e->data, e->size, (unsigned long)e->color,
                 e->coords.align, e->coords.x, e->coords.y,
                 e->coords.w, e->coords.h);
        valid++;
    }

    m->element_count = valid;
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Graphics manager: %d elements, display %dx%d",
             valid, g_display.w, g_display.h);
    return m;
}


//
// ── Init / cleanup ─────────────────────────────────────────────────────────
//

void graphics_print_all(graphics_manager_t *manager)
{
    if (!manager) return;
    ESP_LOGI(TAG, "Graphics manager: %d elements", manager->element_count);
    for (int i = 0; i < manager->element_count; i++) {
        gfx_element_t *e = &manager->elements[i];
        switch (e->type) {
            case ELEM_SVG:
                ESP_LOGI(TAG, "  [%d] '%s' SVG svg=%s size=%d x=%d y=%d color #%06lX", 
                         i, e->id, e->data, e->size, e->coords.x, e->coords.y, (unsigned long)(e->color & 0xFFFFFF));
                break;
            case ELEM_PNG:
                ESP_LOGI(TAG, "  [%d] '%s' PNG file=%s size=%d w=%d h=%d",
                         i, e->id, e->data, e->size, e->coords.w, e->coords.h);
                break;
            case ELEM_TEXT:
                ESP_LOGI(TAG, "  [%d] '%s' Text text=%s size=%d color #%06lX",
                         i, e->id, e->data, e->size, (unsigned long)(e->color & 0xFFFFFF));
                break;
        }
    }
}

static void graphics_preload_assets(const char *device_name) {
    if (!g_manager) return;

    for (int i = 0; i < g_manager->element_count; i++) {
        gfx_element_t *e = &g_manager->elements[i];
        if (e->type != ELEM_SVG && e->type != ELEM_PNG) continue;

        char fullpath[MAX_DEVICE_PATH_LEN];
        snprintf(fullpath, sizeof(fullpath),
                 "/" LITTLEFS_LABEL "/devices/%s/assets/%s", device_name, e->data);

        struct stat st;
        if (stat(fullpath, &st) != 0) {
            ESP_LOGW(TAG, "Asset not found: %s", fullpath);
            continue;
        }

        uint8_t *buf = heap_caps_malloc(st.st_size + 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            ESP_LOGW(TAG, "PSRAM alloc failed for %s", e->data);
            continue;
        }

        FILE *f = fopen(fullpath, "rb");
        if (!f) { heap_caps_free(buf); continue; }

        size_t n = fread(buf, 1, st.st_size, f);
        fclose(f);

        if (e->type == ELEM_SVG) {
            buf[n] = '\0';   // SVG is text, null-terminate
            e->svg_buf = (char *)buf;
            e->svg_len = n;
        } else {
            e->png_buf = buf;
            e->png_len = n;
        }
        ESP_LOGI(TAG, "Pre-loaded %s (%s) → %u bytes PSRAM",
                 e->data, e->type == ELEM_SVG ? "SVG" : "PNG", (unsigned)n);
    }
}

bool graphics_init(const char *filepath) {
    if (g_manager) { ESP_LOGW(TAG, "Graphics already initialized"); return false; }
    g_manager = parse_graphics_json_file(filepath);
    if (!g_manager) return false;
    graphics_preload_assets(current_simulated_device); 
    graphics_print_all(g_manager);
    return true;
}



void graphics_cleanup(void) {
    if (!g_manager) return;

    // Must hold LVGL mutex since we're touching LVGL objects from outside LVGL task
    if (lvgl_lock(-1))
    {
        for (int i = 0; i < g_manager->element_count; i++)
            g_manager->elements[i].lvgl_obj = NULL;
        lv_obj_clean(lv_screen_active());   // synchronous, fires delete callbacks
        lvgl_unlock();
    }

    // Now no LVGL timers pending, safe to free everything
    for (int i = 0; i < g_manager->element_count; i++) {
        gfx_element_t *e = &g_manager->elements[i];
        if (e->pending_render_timer) {
#if !CONFIG_BLESPLOIT_BOARD_BARE
            lv_timer_del(e->pending_render_timer);
#endif
            e->pending_render_timer = NULL;
        }
        if (e->svg_buf) { heap_caps_free(e->svg_buf); e->svg_buf = NULL; }
        if (e->png_buf) { heap_caps_free(e->png_buf); e->png_buf = NULL; }
        websocket_element_remove(e->id);
    }
    free(g_manager->elements);
    free(g_manager);
    g_manager = NULL;
    // 
    gfx_clear_screen();

}

//
// ── Render API ─────────────────────────────────────────────────────────────
//

bool gfx_add_text_element(const char *id, const char *text,
                                gfx_align_t align, int8_t x, int8_t y,
                                uint32_t color, uint8_t size)
{
    if (!g_manager) {
        g_manager = calloc(1, sizeof(graphics_manager_t));
        if (!g_manager) return false;
    }

    int new_count = g_manager->element_count + 1;
    gfx_element_t *new_elements = realloc(g_manager->elements,
                                          new_count * sizeof(gfx_element_t));
    if (!new_elements) return false;
    g_manager->elements = new_elements;

    gfx_element_t *e = &g_manager->elements[g_manager->element_count];
    memset(e, 0, sizeof(gfx_element_t));
    strncpy(e->id,   id,   sizeof(e->id)   - 1);
    strncpy(e->data, text, sizeof(e->data) - 1);
    e->type           = ELEM_TEXT;
    e->coords.align   = align;
    e->coords.x       = x;
    e->coords.y       = y;
    e->color          = color;
    e->color_provided = true;
    e->size           = size;

    g_manager->element_count = new_count;
    return true;
}

void gfx_render_text(const char *id, const gfx_coords_t *coords, uint32_t color)
{
    gfx_element_t *e = graphics_find_by_id(id);
    if (!e || e->type != ELEM_TEXT) {
        ESP_LOGW(TAG, "gfx_render_text: element %s not found", id);
        return;
    }

    const gfx_coords_t *c  = resolve(e, coords);
    lv_align_t         lv_align = gfx_to_lv_align(c->align);

    if (e->lvgl_obj && lv_obj_is_valid(e->lvgl_obj)) {
        // --- REUSE: only update text + color, keep position/font ---
        disp_update_text(e->lvgl_obj, e->data, color);
    } else {
        // --- FIRST RENDER: create the object and store the pointer ---
        e->lvgl_obj = disp_render_text(e->data, c->x, c->y,
                                       lv_align, e->size, color);
    }

    websocket_print_text(id, e->data, c->align, c->x, c->y, color, e->size);
    ESP_LOGI(TAG, "Rendered Text %s", id);
}


void gfx_update_text(const char *id, const char *text, uint32_t color)
{
    gfx_element_t *e = graphics_find_by_id(id);
    if (!e || e->type != ELEM_TEXT) {
        ESP_LOGW(TAG, "gfx_update_text: text element '%s' not found", id);
        return;
    }
    if (text) {
        strncpy(e->data, text, sizeof(e->data) - 1);
        e->data[sizeof(e->data) - 1] = '\0';
    }

    // Re-render with stored coords
    gfx_render_text(id, NULL, color);

}


void gfx_render_png(const char *id)
{
    gfx_element_t *e = graphics_find_by_id(id);
    if (!e || e->type != ELEM_PNG) return;

    const gfx_coords_t *c = &e->coords;
    gfx_coords_t c_with_size;
    if (c->w == 0 && e->size > 0) {
        c_with_size   = *c;
        c_with_size.w = e->size;
        c_with_size.h = e->size;
        c             = &c_with_size;
    }

    int pw = size_to_px(c->w);
    int ph = size_to_px(c->h);
    int px, py;
    coords_to_px(c, pw, ph, &px, &py);

    // Physical display: only possible when the buffer is preloaded 
    if (e->png_buf && e->png_len > 0) {
        e->lvgl_obj = disp_render_png(e->png_buf, e->png_len, px, py, pw, ph);
    } else {
        ESP_LOGW(TAG, "PNG '%s' not preloaded, skipping LVGL render", id);
    }

    // WebSocket render: device-relative asset path (browser adds /static/, mobile uses fs read)
    char ws_path[MAX_DEVICE_PATH_LEN];
    snprintf(ws_path, sizeof(ws_path), "%s/%s",
             current_simulated_device, e->data);
    websocket_render_png(id, ws_path, c);

    ESP_LOGI(TAG, "Rendered PNG '%s' align=%d x=%d y=%d w=%d h=%d",
             id, c->align, c->x, c->y, c->w, c->h);
}


void gfx_render_svg(const char *id)
{
    gfx_element_t *e = graphics_find_by_id(id);
    if (!e || e->type != ELEM_SVG) return;

    const gfx_coords_t *c = &e->coords;
    gfx_coords_t c_with_size;
    if (c->w == 0 && e->size > 0) {
        c_with_size = *c;
        c_with_size.w = e->size;
        c = &c_with_size;
    }
    int sz_px = size_to_px(c->w);

    int px, py;
    coords_to_px(c, sz_px, sz_px, &px, &py);

    char lvgl_path[MAX_DEVICE_PATH_LEN];
    snprintf(lvgl_path, sizeof(lvgl_path),
             "A:/" LITTLEFS_LABEL "/devices/%s/assets/%s",
             current_simulated_device, e->data);

   // LVGL SVG assumes x,y coordinates from top left. Just normalized width (height=0) 
    e->lvgl_obj = disp_render_svg(lvgl_path, id, px, py, sz_px, 0, e->color_provided, e->color);

    char ws_path[MAX_DEVICE_PATH_LEN];
    snprintf(ws_path, sizeof(ws_path), "%s/%s",
             current_simulated_device, e->data);
    
    websocket_render_svg(id, ws_path, c, e->color_provided, e->color);

    ESP_LOGI(TAG, "Rendered SVG '%s' align=%d x=%d y=%d sz_px=%d color_provided=%d color=%06lX",
             id, c->align, c->x, c->y, sz_px, e->color_provided, (unsigned long)e->color);
}


static void svg_color_apply_timer_cb(lv_timer_t *timer)
{
    gfx_element_t *e = (gfx_element_t *)lv_timer_get_user_data(timer);
    e->pending_render_timer = NULL;   // reusing the field as debounce timer slot

    lv_obj_t *old_obj = e->lvgl_obj;
    e->lvgl_obj = NULL;
    if (old_obj)
        disp_delete_svg(old_obj);
    gfx_render_svg(e->id);
    websocket_set_element_color(e->id, e->color);
}

void gfx_set_element_color(const char *id, uint32_t color)
{
    gfx_element_t *e = graphics_find_by_id(id);
    if (!e) return;
    e->color = color;
    e->color_provided = true;

    if (e->type == ELEM_SVG) {
        ESP_LOGI(TAG, "SVG %s update color (debounced)", id);

#if CONFIG_BLESPLOIT_BOARD_BARE
        gfx_render_svg(id);
        websocket_set_element_color(id, color);
        return;
#else
        if (e->pending_render_timer) {
            // Reset the countdown — a newer color arrived
            lv_timer_reset(e->pending_render_timer);
        } else {
            // First update in this burst — arm the timer
            e->pending_render_timer = lv_timer_create(
                svg_color_apply_timer_cb, COLOR_DEBOUNCE_MS, e);
            lv_timer_set_repeat_count(e->pending_render_timer, 1);
        }
        return;   // actual render deferred
#endif

    } else if (e->type == ELEM_TEXT) {
        lv_obj_t *old_obj = e->lvgl_obj;
        e->lvgl_obj = NULL;
        if (old_obj)
            disp_delete_obj(old_obj);   
        gfx_render_text(id, NULL, color);
    }
    // no set color for PNG

    websocket_set_element_color(id, color);
}


void gfx_set_element_position(const char *id, const gfx_coords_t *coords)
{
    gfx_element_t *e = graphics_find_by_id(id);
    if (!e) return;
    ESP_LOGW(TAG, "set element position not yet implemented!");
}


void gfx_set_background(uint32_t color)
{
    disp_set_background(color);
    websocket_set_background(color);
    ESP_LOGI(TAG, "Background set to #%06lX", color);
}



void gfx_print_notification(const char *text,
                            gfx_align_t align,
                            int8_t x, int8_t y,
                            uint32_t color,
                            uint8_t size,
                            uint16_t duration_ms)
{
    // LVGL: transient label
    disp_show_temp_label(text, duration_ms); 

    // Web
    websocket_print_notification(text, align, x, y, color, size, duration_ms);
}

void gfx_print_notification_center(const char *text)
{
    gfx_print_notification(text, GFX_ALIGN_CENTER, 0, 0,
                           0xFFFFFF, 10, 2000);
}


void gfx_render_element(const char *id)
{
    gfx_element_t *e = graphics_find_by_id(id);
    if (!e) return;

    switch (e->type) {
        case ELEM_SVG:
            gfx_render_svg(e->id);
            break;
        case ELEM_TEXT:
            gfx_render_text(e->id, &e->coords, e->color);
            break;
        case ELEM_PNG:
            gfx_render_png(e->id);
            break;
        default:
            break;
    }
}

void gfx_remove_element(const char *id) {
    gfx_element_t *e = graphics_find_by_id(id);
    if (!e) return;
    gfx_delete_element_obj(e);
    websocket_element_remove(id);
    ESP_LOGI(TAG, "Removed element '%s'", id);
}


void gfx_clear_screen(void)
{
    // Null obj pointers first — prevents svg_render_timer_cb from touching
    // objects that lv_obj_clean is about to synchronously destroy.
    // SVG memory is freed via svg_img_delete_cb (LV_EVENT_DELETE).
    // PNG and text objects have no custom cleanup — lv_obj_clean handles them.
    if (g_manager) {
        for (int i = 0; i < g_manager->element_count; i++)
            g_manager->elements[i].lvgl_obj = NULL;
    }

    disp_clear_screen();        // lv_obj_clean — synchronous
    websocket_clear_screen();
    ESP_LOGI(TAG, "Screen cleared");
}
