#include <string.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "math.h"

#include "lvgl.h"
#include "api/web_server.h"
#include "graphics/graphics.h"

static const char *TAG = "LVGL SVG";

// Render from pre-loaded buffer, called from disp_render_svg() when buf available.
// Still deferred via lv_timer so it runs in the LVGL task context — only the
// flash READ is skipped, everything else is identical.
typedef struct {
    char        id[32];
    const char *raw;        // points into gfx_element_t->svg_buf — NOT freed by timer
    size_t      raw_len;
    int         x, y;
    int32_t     target_w, target_h;
    bool        color_provided;
    uint32_t    color;
    lv_obj_t   *target_obj;
} svg_buf_render_req_t;


// ── Cleanup callback ──────────────────────────────────────────────────────────

static void svg_img_delete_cb(lv_event_t *e)
{
    lv_obj_t *img = lv_event_get_target(e);
    lv_image_dsc_t *dsc = (lv_image_dsc_t *)lv_obj_get_user_data(img);
    if (dsc) {
//        ESP_LOGI(TAG, "DELETE dsc=%p data=%p", (void*)dsc, (void*)dsc->data);
        free((void *)dsc->data);
        free(dsc);
    }
}

// ── Parse viewBox or width/height ─────────────────────────────────────

static void svg_parse_viewbox(const char *data, size_t len,
                               float *vb_w, float *vb_h)
{
    *vb_w = 100; *vb_h = 100;

    const char *p = memmem(data, len, "viewBox", 7);
    if (p) {
        p += 7;
        while (*p == ' ' || *p == '=') p++;
        if (*p == '"') p++;
        float x, y, w, h;
        if (sscanf(p, "%f %f %f %f", &x, &y, &w, &h) == 4) {
            *vb_w = w; *vb_h = h;
            ESP_LOGI(TAG, "viewBox: %.2fx%.2f", w, h);
            return;
        }
    }

    float w = 0, h = 0;
    const char *wp = memmem(data, len, "width=\"", 7);
    if (wp) sscanf(wp + 7, "%f", &w);
    const char *hp = memmem(data, len, "height=\"", 8);
    if (hp) sscanf(hp + 8, "%f", &h);
    if (w > 0 && h > 0) { *vb_w = w; *vb_h = h; }

    ESP_LOGI(TAG, "viewBox fallback: %.2fx%.2f", *vb_w, *vb_h);
}


// ── Resolve render size ───────────────────────────────────────────────

static void svg_resolve_size(int32_t req_w, int32_t req_h,
                              float vb_w, float vb_h,
                              int32_t *out_w, int32_t *out_h)
{
    int32_t tw = req_w, th = req_h;

    if      (tw > 0 && th == 0) { th = (int32_t)(tw * vb_h / vb_w); }
    else if (th > 0 && tw == 0) { tw = (int32_t)(th * vb_w / vb_h); }
    else if (tw > 0 && th > 0)  {
        float scale = fminf((float)tw / vb_w, (float)th / vb_h);
        tw = (int32_t)(vb_w * scale);
        th = (int32_t)(vb_h * scale);
    }
    if (tw == 0) tw = (int32_t)vb_w;
    if (th == 0) th = (int32_t)vb_h;

    ESP_LOGI(TAG, "Render size: %ldx%ld", (long)tw, (long)th);
    *out_w = tw; *out_h = th;
}


// ── Step 4: build final SVG string ───────────────────────────────────────────
// Replaces the original <svg> tag, rewrites fill colors if color != 0.
// Returns heap buffer (caller must free), sets *out_len. NULL on failure.

static char *svg_build(const char *raw, size_t raw_len,
                       int32_t tw, int32_t th,
                       float vb_w, float vb_h, bool color_provided,
                       uint32_t color, size_t *out_len)
{
    // Locate body = everything after the original <svg ...> closing '>'
    const char *svg_tag = memmem(raw, raw_len, "<svg", 4);
    const char *close   = svg_tag
                          ? memchr(svg_tag, '>', raw_len - (svg_tag - raw))
                          : NULL;
    const char *body    = close ? close + 1 : raw;
    size_t body_len     = raw_len - (size_t)(body - raw);

    // Build new <svg> header
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "<svg xmlns=\"http://www.w3.org/2000/svg\" "
        "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
        "width=\"%ld\" height=\"%ld\" viewBox=\"0 0 %.4f %.4f\">",
        (long)tw, (long)th, vb_w, vb_h);

    // Allocate output: header + body + generous headroom for color rewrites
    size_t alloc = (size_t)hlen + body_len * 2 + 64;
    char *out = malloc(alloc);
    if (!out) { ESP_LOGE(TAG, "OOM svg_build %u bytes", (unsigned)alloc); return NULL; }

    // Copy header then body into output
    memcpy(out, header, hlen);
    memcpy(out + hlen, body, body_len);
    out[hlen + body_len] = '\0';

    if (color_provided == false) {
        *out_len = (size_t)hlen + body_len;
        ESP_LOGI(TAG, "SVG built: %u bytes (no color)", (unsigned)*out_len);
        return out;
    }

    ESP_LOGI(TAG, "Setting SVG color: #%06lX", (unsigned long)(color & 0xFFFFFF));
    // Rewrite fill="..." values using a second pass into a new buffer
    char hex[8];
    snprintf(hex, sizeof(hex), "#%06lX", (unsigned long)(color & 0xFFFFFF));

    char *dst_buf = malloc(alloc);  // same size — body*2 headroom covers all expansions
    if (!dst_buf) {
        ESP_LOGE(TAG, "OOM color rewrite buffer");
        free(out);
        return NULL;
    }

    const char *src = out;
    char *dst = dst_buf;

    while (*src) {
        if (strncmp(src, "fill=\"", 6) != 0) {
            *dst++ = *src++;
            continue;
        }
        const char *val = src + 6;
        const char *end = strchr(val, '"');
        if (!end) { *dst++ = *src++; continue; }

        size_t vlen = (size_t)(end - val);
        bool keep = (vlen == 4 && memcmp(val, "none", 4) == 0);

        if (keep) {
            // copy fill="none" verbatim
            size_t chunk = 6 + vlen + 1;
            memcpy(dst, src, chunk);
            dst += chunk;
            src  = end + 1;
        } else {
            // replace with target color
            memcpy(dst, "fill=\"", 6); dst += 6;
            memcpy(dst, hex, 7);       dst += 7;
            *dst++ = '"';
            src = end + 1;
        }
    }
    *dst = '\0';

    free(out);
    *out_len = (size_t)(dst - dst_buf);
    ESP_LOGI(TAG, "SVG built: %u bytes (color #%s)", (unsigned)*out_len, hex + 1);
    return dst_buf;
}


static void delete_svg_timer_cb(lv_timer_t *timer)
{
    lv_obj_t *obj = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (obj && lv_obj_is_valid(obj)) {
        lv_image_set_src(obj, NULL);   // drop LVGL's internal ref first
        lv_obj_del(obj);               // fires svg_img_delete_cb → frees dsc+svg_buf
    }
}

void disp_delete_svg(lv_obj_t *obj)
{
    if (!obj) return;
    lv_timer_t *t = lv_timer_create(delete_svg_timer_cb, 0, obj);
    lv_timer_set_repeat_count(t, 1);
}

void disp_delete_svg_delayed(lv_obj_t *obj, uint32_t delay_ms)
{
    if (!obj) return;
    lv_timer_t *t = lv_timer_create(delete_svg_timer_cb, delay_ms, obj);
    lv_timer_set_repeat_count(t, 1);
}


static void svg_render_from_buf_timer_cb(lv_timer_t *timer) {
    svg_buf_render_req_t *req = lv_timer_get_user_data(timer);

    if (!req->target_obj || !lv_obj_is_valid(req->target_obj)) {
        free(req);
        return;
    }

    float vbw, vbh;
    svg_parse_viewbox(req->raw, req->raw_len, &vbw, &vbh);

    int32_t tw, th;
    svg_resolve_size(req->target_w, req->target_h, vbw, vbh, &tw, &th);

    size_t svg_len = 0;
    char *svg_buf = svg_build(req->raw, req->raw_len,
                              tw, th, vbw, vbh,
                              req->color_provided, req->color, &svg_len);
    if (!svg_buf) { free(req); return; }

    lv_image_dsc_t *dsc = calloc(1, sizeof(lv_image_dsc_t));
    if (!dsc) { free(svg_buf); free(req); return; }
    dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf     = LV_COLOR_FORMAT_RAW;
    dsc->header.w      = (uint32_t)tw;
    dsc->header.h      = (uint32_t)th;
    dsc->data_size     = svg_len;
    dsc->data          = (const uint8_t *)svg_buf;

    lv_obj_t *img = req->target_obj;
    lv_obj_set_user_data(img, dsc);
    lv_image_set_src(img, dsc);
    lv_obj_align(img, LV_ALIGN_TOP_LEFT, req->x, req->y);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);

    free(req);   // raw buffer NOT freed — owned by gfx_element_t
}



lv_obj_t *disp_render_svg(const char *filepath, const char *id,
                           int x, int y, int32_t target_w, int32_t target_h,
                           bool color_provided, uint32_t color)
{
    lv_obj_t *obj = lv_image_create(lv_screen_active());
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(obj, svg_img_delete_cb, LV_EVENT_DELETE, NULL);

    gfx_element_t *e = graphics_find_by_id(id);
    if (!e || !e->svg_buf) {
        // Pre-load failed at init — this is a hard error
        ESP_LOGE(TAG, "SVG %s not pre-loaded, cannot render", id);
        lv_obj_del(obj);
        return NULL;
    }

    svg_buf_render_req_t *req = malloc(sizeof(svg_buf_render_req_t));
    assert(req);
    strncpy(req->id, id, sizeof(req->id) - 1);
    req->raw            = e->svg_buf;
    req->raw_len        = e->svg_len;
    req->x              = x;  req->y = y;
    req->target_w       = target_w;  req->target_h = target_h;
    req->color_provided = color_provided;  req->color = color;
    req->target_obj     = obj;

    lv_timer_t *t = lv_timer_create(svg_render_from_buf_timer_cb, 0, req);
    lv_timer_set_repeat_count(t, 1);

    return obj;
}
