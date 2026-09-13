#include <lauxlib.h>
#include <lua.h>

#include "lua/lua_mac.h"

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Collect exactly 6 octets from hex digits (colons/dashes/spaces ignored). Returns 0 or -1. 
static int collect_octets(lua_State *L, int idx, unsigned char out[6])
{
    if (lua_type(L, idx) != LUA_TSTRING) {
        return -1;
    }
    size_t len = 0;
    const char *s = lua_tolstring(L, idx, &len);
    unsigned char digits[12];
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        int v = hex_nibble((unsigned char)s[i]);
        if (v < 0) {
            continue;
        }
        if (n >= 12) {
            return -1;
        }
        digits[n++] = (unsigned char)v;
    }
    if (n != 12) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (unsigned char)((digits[i * 2] << 4) | digits[i * 2 + 1]);
    }
    return 0;
}

static void format_mac(const unsigned char octets[6], char out[18])
{
    static const char HEX[] = "0123456789ABCDEF";
    int p = 0;
    for (int i = 0; i < 6; i++) {
        if (i) {
            out[p++] = ':';
        }
        out[p++] = HEX[(octets[i] >> 4) & 0xF];
        out[p++] = HEX[octets[i] & 0xF];
    }
    out[p] = '\0';
}

static int lua_mac_format(lua_State *L)
{
    unsigned char octets[6];
    if (collect_octets(L, 1, octets) != 0) {
        lua_pushliteral(L, "");
        return 1;
    }
    char buf[18];
    format_mac(octets, buf);
    lua_pushlstring(L, buf, 17);
    return 1;
}

static int lua_mac_reverse_octets(lua_State *L)
{
    unsigned char octets[6];
    if (collect_octets(L, 1, octets) != 0) {
        lua_pushliteral(L, "");
        return 1;
    }
    static const char HEX[] = "0123456789abcdef";
    char buf[13];
    int p = 0;
    for (int i = 5; i >= 0; i--) {
        buf[p++] = HEX[(octets[i] >> 4) & 0xF];
        buf[p++] = HEX[octets[i] & 0xF];
    }
    buf[p] = '\0';
    lua_pushlstring(L, buf, 12);
    return 1;
}

static int lua_mac_from_reversed(lua_State *L)
{
    unsigned char octets[6];
    if (collect_octets(L, 1, octets) != 0) {
        lua_pushliteral(L, "");
        return 1;
    }
    unsigned char rev[6];
    for (int i = 0; i < 6; i++) {
        rev[i] = octets[5 - i];
    }
    char buf[18];
    format_mac(rev, buf);
    lua_pushlstring(L, buf, 17);
    return 1;
}

void lua_mac_register_functions(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_mac_format);
    lua_setfield(L, -2, "format");
    lua_pushcfunction(L, lua_mac_reverse_octets);
    lua_setfield(L, -2, "reverse_octets");
    lua_pushcfunction(L, lua_mac_from_reversed);
    lua_setfield(L, -2, "from_reversed");
    lua_setglobal(L, "mac");
}
