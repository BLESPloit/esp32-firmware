#pragma once

#include <lua.h>

/**
 * Install `bin_to_hex` / `hex_to_bin` plus shared `bits` and `hex` tables
 * (KMP LuaBinaryHexGlobals + LuaBitsHexGlobals parity).
 */
void lua_bits_hex_register_functions(lua_State *L);
