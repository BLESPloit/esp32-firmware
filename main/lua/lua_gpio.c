#include <lua.h>
#include <lauxlib.h>
#include "esp_err.h"

#include "hw/named_gpio.h"
#include "lua/lua_gpio.h"

static int lua_gpio_level_arg(lua_State *L, int index)
{
    if (lua_isboolean(L, index)) {
        return lua_toboolean(L, index);
    }
    return ((int)luaL_checkinteger(L, index)) != 0 ? 1 : 0;
}

static int lua_gpio_set(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int level = lua_gpio_level_arg(L, 2);

    named_gpio_id_t id;
    esp_err_t err = named_gpio_from_name(name, &id);
    if (err != ESP_OK) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "unknown gpio name");
        return 2;
    }

    err = named_gpio_set(id, level);
    if (err != ESP_OK) {
        lua_pushboolean(L, 0);
        if (err == ESP_ERR_NOT_FOUND) {
            lua_pushstring(L, "gpio not available on this board");
        } else {
            lua_pushstring(L, esp_err_to_name(err));
        }
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_gpio_get(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);

    named_gpio_id_t id;
    esp_err_t err = named_gpio_from_name(name, &id);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "unknown gpio name");
        return 2;
    }

    int level = 0;
    err = named_gpio_get(id, &level);
    if (err != ESP_OK) {
        lua_pushnil(L);
        if (err == ESP_ERR_NOT_FOUND) {
            lua_pushstring(L, "gpio not available on this board");
        } else {
            lua_pushstring(L, esp_err_to_name(err));
        }
        return 2;
    }

    lua_pushinteger(L, level);
    return 1;
}

void lua_gpio_register_functions(lua_State *L)
{
    lua_register(L, "gpio_set", lua_gpio_set);
    lua_register(L, "gpio_get", lua_gpio_get);
}
