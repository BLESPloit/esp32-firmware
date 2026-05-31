#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "api/web_server.h"
#include "api/web_server_internal.h"
#include "graphics/graphics.h"

static const char *TAG = "websocket gfx";

// ── Broadcast helper ───────────────────────────────────────────────────────
static void gfx_broadcast(cJSON *j, bool stateful)
{
    char *s = cJSON_PrintUnformatted(j);
    if (!s) return;
    stateful ? websocket_broadcast_json(s)
             : websocket_broadcast_json_transient(s);
    ESP_LOGI(TAG, "gfx: %s", s);
    free(s);
}

// ── Shared: add normalized coords fields to a JSON object ─────────────────
static void add_coords(cJSON *j, const gfx_coords_t *c)
{
    // align as string — web client resolves to pixel anchor
    const char *align_str;
    switch (c->align) {
        case GFX_ALIGN_TOP_LEFT:   align_str = "top_left";   break;
        case GFX_ALIGN_TOP_CENTER: align_str = "top_center"; break;
        case GFX_ALIGN_TOP_RIGHT:  align_str = "top_right";  break;
        case GFX_ALIGN_MIDDLE_LEFT:   align_str = "middle_left";   break;
        case GFX_ALIGN_MIDDLE_RIGHT:  align_str = "middle_right";  break;
        case GFX_ALIGN_BOTTOM_LEFT:   align_str = "bottom_left";   break;
        case GFX_ALIGN_BOTTOM_CENTER: align_str = "bottom_center"; break;
        case GFX_ALIGN_BOTTOM_RIGHT:  align_str = "bottom_right";  break;
        default:                   align_str = "center";     break;
    }
    cJSON_AddStringToObject(j, "align", align_str);
    cJSON_AddNumberToObject(j, "x", c->x);
    cJSON_AddNumberToObject(j, "y", c->y);
    if (c->w) cJSON_AddNumberToObject(j, "w", c->w);
    if (c->h) cJSON_AddNumberToObject(j, "h", c->h);
}

// ── Render commands ────────────────────────────────────────────────────────
void websocket_render_png(const char *id, const char *path,
                           const gfx_coords_t *c)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "gfx");
    cJSON_AddStringToObject(j, "cmd",  "png");
    cJSON_AddStringToObject(j, "id",   id);
    cJSON_AddStringToObject(j, "path", path);
    add_coords(j, c);
    gfx_broadcast(j, true);
    cJSON_Delete(j);
}

void websocket_render_svg(const char *id, const char *svg,
                           const gfx_coords_t *c, bool color_provided, uint32_t color)
{
    char hex[8];
    snprintf(hex, sizeof(hex), "%06lX", color);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type",  "gfx");
    cJSON_AddStringToObject(j, "cmd",   "svg");
    cJSON_AddStringToObject(j, "id",    id);
    cJSON_AddStringToObject(j, "svg",   svg);
    if (color_provided == true)
        cJSON_AddStringToObject(j, "color", hex);
    add_coords(j, c);
    // w (== size_pct) is the SVG render size; h is unused for SVGs
    gfx_broadcast(j, true);
    cJSON_Delete(j);
}

// ── Element state commands ─────────────────────────────────────────────────
void websocket_element_remove(const char *id)
{
    smart_state_remove_element(id);   // remove from replay state first
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "gfx");
    cJSON_AddStringToObject(j, "cmd",  "remove");
    cJSON_AddStringToObject(j, "id",   id);
    gfx_broadcast(j, false);          // don't save remove cmd to state
    cJSON_Delete(j);
}

void websocket_set_background(uint32_t color)
{
    char hex[8];
    snprintf(hex, sizeof(hex), "%06lX", color);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type",  "gfx");
    cJSON_AddStringToObject(j, "cmd",   "background");
    cJSON_AddStringToObject(j, "color", hex);
    gfx_broadcast(j, true);
    cJSON_Delete(j);
}

void websocket_set_element_color(const char *id, uint32_t color)
{
    char hex[8];
    snprintf(hex, sizeof(hex), "%06lX", color);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type",  "gfx");
    cJSON_AddStringToObject(j, "cmd",   "color");
    cJSON_AddStringToObject(j, "id",    id);
    cJSON_AddStringToObject(j, "color", hex);
    gfx_broadcast(j, true);
    cJSON_Delete(j);
}

void websocket_clear_screen(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "gfx");
    cJSON_AddStringToObject(j, "cmd",  "clear");
    gfx_broadcast(j, true);
    cJSON_Delete(j);
}

void websocket_print_notification(const char *text,
                                  gfx_align_t align, int8_t x, int8_t y,
                                  uint32_t color, uint8_t size, uint16_t duration)
{
    char hex[8];
    snprintf(hex, sizeof(hex), "%06lX", color);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "gfx");
    cJSON_AddStringToObject(j, "cmd", "notification");
    cJSON_AddStringToObject(j, "text", text);
    cJSON_AddStringToObject(j, "color", hex);
    cJSON_AddNumberToObject(j, "size", size);
    cJSON_AddNumberToObject(j, "duration", duration);
    add_coords(j, &(gfx_coords_t){.align=align, .x=x, .y=y, .w=size, .h=0});
    gfx_broadcast(j, true);
    cJSON_Delete(j);
}


// print_text keeps align+offset rather than raw pixels too
void websocket_print_text(const char *id, const char *text,
                          gfx_align_t align, int8_t x, int8_t y,
                          uint32_t color, uint8_t size)
{
    char hex[8];
    snprintf(hex, sizeof(hex), "%06lX", color);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "gfx");
    cJSON_AddStringToObject(j, "cmd", "text");
    cJSON_AddStringToObject(j, "id", id);
    cJSON_AddStringToObject(j, "text", text);
    cJSON_AddStringToObject(j, "color", hex);
    cJSON_AddNumberToObject(j, "size", size);
    add_coords(j, &(gfx_coords_t){.align=align, .x=x, .y=y, .w=size, .h=0});
    gfx_broadcast(j, true);
    cJSON_Delete(j);
}


void register_gfx_handlers_in_webserver(void)
{
    // no inbound gfx messages yet — registration point ready for future use
}
