#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#include "common/utils.h"
#include "lua/lua_bits_hex.h"

static const char HEX_DIGITS[] = "0123456789abcdef";

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** Lua: hex_string = bin_to_hex(binary_data). Lowercase; non-string → "". */
static int lua_bin_to_hex(lua_State *L)
{
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushliteral(L, "");
        return 1;
    }

    size_t len;
    const unsigned char *data = (const unsigned char *)lua_tolstring(L, 1, &len);
    if (len == 0) {
        lua_pushliteral(L, "");
        return 1;
    }

    char *hex = malloc(len * 2 + 1);
    if (!hex) {
        return luaL_error(L, "Memory allocation failed");
    }

    bin_to_hex_string(data, len, hex);
    lua_pushlstring(L, hex, len * 2);
    free(hex);
    return 1;
}

/** Lua: binary_data = hex_to_bin(hex_string). Whitespace-stripped; invalid → "". */
static int lua_hex_to_bin(lua_State *L)
{
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushliteral(L, "");
        return 1;
    }

    size_t raw_len;
    const char *raw = lua_tolstring(L, 1, &raw_len);

    char *compact = malloc(raw_len + 1);
    if (!compact) {
        return luaL_error(L, "Memory allocation failed");
    }

    size_t n = 0;
    for (size_t i = 0; i < raw_len; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (!isspace(c)) {
            compact[n++] = (char)c;
        }
    }
    compact[n] = '\0';

    if (n == 0 || (n % 2) != 0) {
        free(compact);
        lua_pushliteral(L, "");
        return 1;
    }

    size_t bin_len = n / 2;
    unsigned char *bin = malloc(bin_len);
    if (!bin) {
        free(compact);
        return luaL_error(L, "Memory allocation failed");
    }

    for (size_t i = 0; i < bin_len; i++) {
        int hi = hex_nibble(compact[i * 2]);
        int lo = hex_nibble(compact[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            free(compact);
            free(bin);
            lua_pushliteral(L, "");
            return 1;
        }
        bin[i] = (unsigned char)((hi << 4) | lo);
    }

    free(compact);
    lua_pushlstring(L, (const char *)bin, bin_len);
    free(bin);
    return 1;
}

/** Luaj checkint: error on non-number, truncate toward zero, 32-bit. */
static int32_t checkint32(lua_State *L, int idx)
{
    return (int32_t)luaL_checknumber(L, idx);
}

static void push_int32(lua_State *L, int32_t v)
{
    lua_pushinteger(L, (lua_Integer)v);
}

static int clamp_shift(int32_t n)
{
    if (n < 0) return 0;
    if (n > 31) return 31;
    return (int)n;
}

static void append_hex_byte(char *out, unsigned b)
{
    out[0] = HEX_DIGITS[(b >> 4) & 0xF];
    out[1] = HEX_DIGITS[b & 0xF];
}

static void pack_int(char *out, int32_t n, int width_bytes, int little_endian)
{
    uint32_t u;
    if (width_bytes == 1) {
        u = (uint32_t)n & 0xFFu;
    } else if (width_bytes == 2) {
        u = (uint32_t)n & 0xFFFFu;
    } else {
        u = (uint32_t)n;
    }

    if (little_endian) {
        for (int i = 0; i < width_bytes; i++) {
            append_hex_byte(out + i * 2, (u >> (8 * i)) & 0xFFu);
        }
    } else {
        for (int i = 0; i < width_bytes; i++) {
            append_hex_byte(out + i * 2, (u >> (8 * (width_bytes - 1 - i))) & 0xFFu);
        }
    }
    out[width_bytes * 2] = '\0';
}

/**
 * Strip all whitespace and lowercase. Non-string → NULL, *out_len = 0.
 * Caller frees a non-NULL return.
 */
static char *normalize_hex_arg(lua_State *L, int idx, size_t *out_len)
{
    if (lua_type(L, idx) != LUA_TSTRING) {
        *out_len = 0;
        return NULL;
    }

    size_t raw_len;
    const char *raw = lua_tolstring(L, idx, &raw_len);
    char *out = malloc(raw_len + 1);
    if (!out) {
        luaL_error(L, "Memory allocation failed");
        return NULL;
    }

    size_t n = 0;
    for (size_t i = 0; i < raw_len; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (isspace(c)) {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
    *out_len = n;
    return out;
}

// ── bits bitwise ────────────────────────────────────────────────────────────

static int lua_bits_band(lua_State *L)
{
    int32_t a = checkint32(L, 1);
    int32_t b = checkint32(L, 2);
    push_int32(L, a & b);
    return 1;
}

static int lua_bits_bor(lua_State *L)
{
    int32_t a = checkint32(L, 1);
    int32_t b = checkint32(L, 2);
    push_int32(L, a | b);
    return 1;
}

static int lua_bits_bxor(lua_State *L)
{
    int32_t a = checkint32(L, 1);
    int32_t b = checkint32(L, 2);
    push_int32(L, a ^ b);
    return 1;
}

static int lua_bits_bnot(lua_State *L)
{
    int32_t a = checkint32(L, 1);
    push_int32(L, (int32_t)(~(uint32_t)a));
    return 1;
}

static int lua_bits_rshift(lua_State *L)
{
    uint32_t a = (uint32_t)checkint32(L, 1);
    int n = clamp_shift(checkint32(L, 2));
    push_int32(L, (int32_t)(a >> n));
    return 1;
}

static int lua_bits_lshift(lua_State *L)
{
    int32_t a = checkint32(L, 1);
    int n = clamp_shift(checkint32(L, 2));
    push_int32(L, (int32_t)((uint32_t)a << n));
    return 1;
}

static int lua_bits_arshift(lua_State *L)
{
    int32_t a = checkint32(L, 1);
    int n = clamp_shift(checkint32(L, 2));
    push_int32(L, a >> n);
    return 1;
}

/** 1-based byte index into hex as given (not norm'd). Short/invalid → 0. */
static int lua_bits_byte_at(lua_State *L)
{
    size_t len;
    const char *hex = luaL_checklstring(L, 1, &len);
    int32_t i = checkint32(L, 2);
    if (i < 1 || len < (size_t)i * 2) {
        push_int32(L, 0);
        return 1;
    }
    size_t start = (size_t)(i - 1) * 2;
    int hi = hex_nibble((unsigned char)hex[start]);
    int lo = hex_nibble((unsigned char)hex[start + 1]);
    if (hi < 0 || lo < 0) {
        push_int32(L, 0);
        return 1;
    }
    push_int32(L, (hi << 4) | lo);
    return 1;
}

static int lua_bits_tohex(lua_State *L)
{
    int32_t n = checkint32(L, 1);
    int32_t width = 2;
    if (lua_gettop(L) >= 2) {
        width = checkint32(L, 2);
        if (width < 1) {
            width = 1;
        }
    }
    if (width > 64) {
        width = 64;
    }

    uint32_t u = (uint32_t)n;
    char num[9];
    int nlen = 0;
    if (u == 0) {
        num[nlen++] = '0';
    } else {
        char tmp[8];
        int tlen = 0;
        while (u != 0 && tlen < 8) {
            tmp[tlen++] = HEX_DIGITS[u & 0xF];
            u >>= 4;
        }
        while (tlen > 0) {
            num[nlen++] = tmp[--tlen];
        }
    }
    num[nlen] = '\0';

    int pad = (width > nlen) ? (int)width - nlen : 0;
    size_t out_len = (size_t)pad + (size_t)nlen;
    char *out = malloc(out_len + 1);
    if (!out) {
        return luaL_error(L, "Memory allocation failed");
    }
    memset(out, '0', (size_t)pad);
    memcpy(out + pad, num, (size_t)nlen);
    out[out_len] = '\0';
    lua_pushlstring(L, out, out_len);
    free(out);
    return 1;
}

/** Strip spaces only (KMP fromhex), then pair digits. Invalid → 0. */
static int lua_bits_fromhex(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);

    uint32_t v = 0;
    int have_hi = 0;
    int hi = 0;
    int any = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == ' ') {
            continue;
        }
        int nib = hex_nibble((unsigned char)s[i]);
        if (nib < 0) {
            push_int32(L, 0);
            return 1;
        }
        any = 1;
        if (!have_hi) {
            hi = nib;
            have_hi = 1;
        } else {
            v = ((v << 8) | (uint32_t)((hi << 4) | nib)) & 0xFFFFFFFFu;
            have_hi = 0;
        }
    }
    if (!any) {
        push_int32(L, 0);
        return 1;
    }
    push_int32(L, (int32_t)v);
    return 1;
}

static int unpack_le(lua_State *L, int width_bytes)
{
    size_t len;
    const char *hex = luaL_checklstring(L, 1, &len);
    int32_t offset = checkint32(L, 2);
    if (offset < 1) {
        push_int32(L, 0);
        return 1;
    }
    size_t need = (size_t)(offset - 1) * 2 + (size_t)width_bytes * 2;
    if (len < need) {
        push_int32(L, 0);
        return 1;
    }
    size_t start = (size_t)(offset - 1) * 2;
    int32_t acc = 0;
    for (int b = 0; b < width_bytes; b++) {
        int hi = hex_nibble((unsigned char)hex[start + (size_t)b * 2]);
        int lo = hex_nibble((unsigned char)hex[start + (size_t)b * 2 + 1]);
        if (hi < 0 || lo < 0) {
            push_int32(L, 0);
            return 1;
        }
        acc |= ((hi << 4) | lo) << (8 * b);
    }
    push_int32(L, acc);
    return 1;
}

static int unpack_be(lua_State *L, int width_bytes)
{
    size_t len;
    const char *hex = luaL_checklstring(L, 1, &len);
    int32_t offset = checkint32(L, 2);
    if (offset < 1) {
        push_int32(L, 0);
        return 1;
    }
    size_t need = (size_t)(offset - 1) * 2 + (size_t)width_bytes * 2;
    if (len < need) {
        push_int32(L, 0);
        return 1;
    }
    size_t start = (size_t)(offset - 1) * 2;
    uint32_t acc = 0;
    for (int b = 0; b < width_bytes; b++) {
        int hi = hex_nibble((unsigned char)hex[start + (size_t)b * 2]);
        int lo = hex_nibble((unsigned char)hex[start + (size_t)b * 2 + 1]);
        if (hi < 0 || lo < 0) {
            push_int32(L, 0);
            return 1;
        }
        acc = (acc << 8) | (uint32_t)((hi << 4) | lo);
    }
    push_int32(L, (int32_t)acc);
    return 1;
}

static int lua_bits_le16(lua_State *L) { return unpack_le(L, 2); }
static int lua_bits_le32(lua_State *L) { return unpack_le(L, 4); }
static int lua_bits_be16(lua_State *L) { return unpack_be(L, 2); }
static int lua_bits_be32(lua_State *L) { return unpack_be(L, 4); }

// ── hex string helpers / packers ────────────────────────────────────────────

static int lua_hex_norm(lua_State *L)
{
    size_t n;
    char *compact = normalize_hex_arg(L, 1, &n);
    if (!compact) {
        lua_pushliteral(L, "");
        return 1;
    }
    lua_pushlstring(L, compact, n);
    free(compact);
    return 1;
}

static int lua_hex_len(lua_State *L)
{
    size_t n;
    char *compact = normalize_hex_arg(L, 1, &n);
    if (compact) {
        free(compact);
    }
    lua_pushinteger(L, (lua_Integer)(n / 2));
    return 1;
}

static int lua_hex_slice(lua_State *L)
{
    int32_t from = checkint32(L, 2);
    int32_t count = checkint32(L, 3);
    size_t n;
    char *compact = normalize_hex_arg(L, 1, &n);
    if (!compact) {
        lua_pushliteral(L, "");
        return 1;
    }
    if (from < 1 || count < 1) {
        free(compact);
        lua_pushliteral(L, "");
        return 1;
    }
    size_t start = (size_t)(from - 1) * 2;
    size_t need = (size_t)count * 2;
    if (start + need > n) {
        free(compact);
        lua_pushliteral(L, "");
        return 1;
    }
    lua_pushlstring(L, compact + start, need);
    free(compact);
    return 1;
}

static int lua_hex_to_ascii(lua_State *L)
{
    size_t n;
    char *compact = normalize_hex_arg(L, 1, &n);
    if (!compact) {
        lua_pushliteral(L, "");
        return 1;
    }

    char *out = malloc(n / 2 + 1);
    if (!out) {
        free(compact);
        return luaL_error(L, "Memory allocation failed");
    }

    size_t o = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        int hi = hex_nibble((unsigned char)compact[i]);
        int lo = hex_nibble((unsigned char)compact[i + 1]);
        if (hi < 0 || lo < 0) {
            continue;
        }
        int b = (hi << 4) | lo;
        if (b >= 0x20 && b <= 0x7E) {
            out[o++] = (char)b;
        }
    }
    lua_pushlstring(L, out, o);
    free(out);
    free(compact);
    return 1;
}

static int lua_hex_from_ascii(lua_State *L)
{
    size_t n;
    const char *s = luaL_checklstring(L, 1, &n);
    if (n == 0) {
        lua_pushliteral(L, "");
        return 1;
    }
    char *out = malloc(n * 2 + 1);
    if (!out) {
        return luaL_error(L, "Memory allocation failed");
    }
    for (size_t i = 0; i < n; i++) {
        append_hex_byte(out + i * 2, (unsigned char)s[i]);
    }
    lua_pushlstring(L, out, n * 2);
    free(out);
    return 1;
}

static int lua_hex_u8(lua_State *L)
{
    char buf[3];
    pack_int(buf, checkint32(L, 1), 1, 1);
    lua_pushlstring(L, buf, 2);
    return 1;
}

static int lua_hex_le16(lua_State *L)
{
    char buf[5];
    pack_int(buf, checkint32(L, 1), 2, 1);
    lua_pushlstring(L, buf, 4);
    return 1;
}

static int lua_hex_be16(lua_State *L)
{
    char buf[5];
    pack_int(buf, checkint32(L, 1), 2, 0);
    lua_pushlstring(L, buf, 4);
    return 1;
}

static int lua_hex_le32(lua_State *L)
{
    char buf[9];
    pack_int(buf, checkint32(L, 1), 4, 1);
    lua_pushlstring(L, buf, 8);
    return 1;
}

static int lua_hex_be32(lua_State *L)
{
    char buf[9];
    pack_int(buf, checkint32(L, 1), 4, 0);
    lua_pushlstring(L, buf, 8);
    return 1;
}

void lua_bits_hex_register_functions(lua_State *L)
{
    lua_register(L, "bin_to_hex", lua_bin_to_hex);
    lua_register(L, "hex_to_bin", lua_hex_to_bin);

    lua_newtable(L);
    lua_pushcfunction(L, lua_bits_band);
    lua_setfield(L, -2, "band");
    lua_pushcfunction(L, lua_bits_bor);
    lua_setfield(L, -2, "bor");
    lua_pushcfunction(L, lua_bits_bxor);
    lua_setfield(L, -2, "bxor");
    lua_pushcfunction(L, lua_bits_bnot);
    lua_setfield(L, -2, "bnot");
    lua_pushcfunction(L, lua_bits_rshift);
    lua_setfield(L, -2, "rshift");
    lua_pushcfunction(L, lua_bits_lshift);
    lua_setfield(L, -2, "lshift");
    lua_pushcfunction(L, lua_bits_arshift);
    lua_setfield(L, -2, "arshift");
    lua_pushcfunction(L, lua_bits_byte_at);
    lua_setfield(L, -2, "byte_at");
    lua_pushcfunction(L, lua_bits_tohex);
    lua_setfield(L, -2, "tohex");
    lua_pushcfunction(L, lua_bits_fromhex);
    lua_setfield(L, -2, "fromhex");
    lua_pushcfunction(L, lua_bits_le16);
    lua_setfield(L, -2, "le16");
    lua_pushcfunction(L, lua_bits_be16);
    lua_setfield(L, -2, "be16");
    lua_pushcfunction(L, lua_bits_le32);
    lua_setfield(L, -2, "le32");
    lua_pushcfunction(L, lua_bits_be32);
    lua_setfield(L, -2, "be32");
    lua_setglobal(L, "bits");

    lua_newtable(L);
    lua_pushcfunction(L, lua_hex_norm);
    lua_setfield(L, -2, "norm");
    lua_pushcfunction(L, lua_bits_byte_at);
    lua_setfield(L, -2, "byte");
    lua_pushcfunction(L, lua_hex_len);
    lua_setfield(L, -2, "len");
    lua_pushcfunction(L, lua_hex_slice);
    lua_setfield(L, -2, "slice");
    lua_pushcfunction(L, lua_hex_to_ascii);
    lua_setfield(L, -2, "to_ascii");
    lua_pushcfunction(L, lua_hex_from_ascii);
    lua_setfield(L, -2, "from_ascii");
    lua_pushcfunction(L, lua_hex_u8);
    lua_setfield(L, -2, "u8");
    lua_pushcfunction(L, lua_hex_le16);
    lua_setfield(L, -2, "le16");
    lua_pushcfunction(L, lua_hex_be16);
    lua_setfield(L, -2, "be16");
    lua_pushcfunction(L, lua_hex_le32);
    lua_setfield(L, -2, "le32");
    lua_pushcfunction(L, lua_hex_be32);
    lua_setfield(L, -2, "be32");
    lua_setglobal(L, "hex");
}
