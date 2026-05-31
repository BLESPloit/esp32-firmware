#include <stdio.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include "graphics/graphics.h"
#include "interface/interface_central.h"
#include "common/storage.h"
#include "common/utils.h"
#include "lua/lua_hook.h"

#define TAG "LUA gfx"


// ── Graphics Lua Bindings ────────────────────────────────────────────────── 

// Helper: parse coords from Lua stack starting at index idx
// Expects: align (string), x (int), y (int), w (int), h (int)
// All optional — missing args default to center/0/0/0/0
static gfx_coords_t lua_parse_coords(lua_State *L, int idx)
{
    gfx_coords_t c = {
        .align = GFX_ALIGN_CENTER,
        .x = 0, .y = 0, .w = 0, .h = 0
    };
    if (lua_isstring(L, idx))  {
        const char *a = lua_tostring(L, idx);
        c.align = gfx_parse_align(a);
        idx++;
    }
    if (lua_isnumber(L, idx))   c.x = (int8_t)lua_tointeger(L, idx++);
    if (lua_isnumber(L, idx))   c.y = (int8_t)lua_tointeger(L, idx++);
    if (lua_isnumber(L, idx))   c.w = (uint8_t)lua_tointeger(L, idx++);
    if (lua_isnumber(L, idx))   c.h = (uint8_t)lua_tointeger(L, idx);
    return c;
}

int lua_gfx_render_text(lua_State *L)
{
    const char *id = luaL_checkstring(L, 1);
    gfx_coords_t c = { .align = GFX_ALIGN_CENTER, .x = 0, .y = 0, .w = 0, .h = 0 };
    uint32_t color = 0xFFFFFF;

    int idx = 2;
    if (lua_isstring(L, idx)) {
        const char *a = lua_tostring(L, idx++);
        c.align = gfx_parse_align(a);
    }
    if (lua_isnumber(L, idx))   c.x = (int8_t)lua_tointeger(L, idx++);
    if (lua_isnumber(L, idx))   c.y = (int8_t)lua_tointeger(L, idx++);
    if (lua_isnumber(L, idx))   color = (uint32_t)lua_tointeger(L, idx);

    gfx_render_text(id, &c, color);
    return 0;
}

int lua_gfx_update_text(lua_State *L)
{
    const char *id = luaL_checkstring(L, 1);
    const char *text = luaL_checkstring(L, 2);
    uint32_t color = 0xFFFFFF;
    if (lua_isnumber(L, 3)) color = (uint32_t)lua_tointeger(L, 3);
    gfx_update_text(id, text, color);
    return 0;
}

int lua_gfx_show(lua_State *L) 
{
    const char *id = luaL_checkstring(L, 1);
    gfx_element_t *e = graphics_find_by_id(id);
    if (!e) return 0;

    gfx_render_element(id);
    return 0;
}

// gfx_set_element_position(id [, align] [, x, y])
int lua_gfx_set_element_position(lua_State *L)
{
    const char *id = luaL_checkstring(L, 1);
    gfx_coords_t c = lua_parse_coords(L, 2);
    gfx_set_element_position(id, &c);
    return 0;
}

int lua_gfx_remove_element(lua_State *L)
{
    gfx_remove_element(luaL_checkstring(L, 1));
    return 0;
}

int lua_gfx_set_background(lua_State *L)
{
    gfx_set_background((uint32_t)luaL_checkinteger(L, 1));
    return 0;
}

int lua_gfx_set_element_color(lua_State *L)
{
    const char *id = luaL_checkstring(L, 1);
    uint32_t color = (uint32_t)luaL_checkinteger(L, 2);
    gfx_set_element_color(id, color);
    return 0;
}


int lua_gfx_print_notification(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);

    gfx_align_t align = GFX_ALIGN_CENTER;
    int8_t x = 0, y = 0;
    uint32_t color = 0xFFFFFF;
    uint8_t size = 10;
    uint16_t duration = 2000;

    int top = lua_gettop(L);
    if (top >= 2 && lua_isstring(L, 2)) {
        const char *a = lua_tostring(L, 2);
        align = gfx_parse_align(a);
    }
    if (top >= 3 && lua_isnumber(L, 3)) x = (int8_t)lua_tointeger(L, 3);
    if (top >= 4 && lua_isnumber(L, 4)) y = (int8_t)lua_tointeger(L, 4);
    if (top >= 5 && lua_isnumber(L, 5)) color = (uint32_t)lua_tointeger(L, 5);
    if (top >= 6 && lua_isnumber(L, 6)) size = (uint8_t)lua_tointeger(L, 6);
    if (top >= 7 && lua_isnumber(L, 7)) duration = (uint16_t)lua_tointeger(L, 7);

    gfx_print_notification(text, align, x, y, color, size, duration);
    return 0;
}
