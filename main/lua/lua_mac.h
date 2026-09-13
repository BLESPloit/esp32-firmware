#pragma once

#include <lua.h>

// Install `mac.format` / `mac.reverse_octets` / `mac.from_reversed` (KMP LuaMacGlobals parity). 
void lua_mac_register_functions(lua_State *L);
