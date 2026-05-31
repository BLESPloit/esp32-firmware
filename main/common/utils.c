#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "os/os_mbuf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h" 
#include "utils.h"

static const char *TAG = "utils";

// Function to log memory usage with the message at the end

void log_memory_usage(const char *label) {
    uint32_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    uint32_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    
    ESP_LOGI(TAG, "=== Memory %s - PSRAM: %" PRIu32, label, psram);
    ESP_LOGI(TAG, "INTERNAL: %" PRIu32 ", largest block: %" PRIu32 ", min ever: %" PRIu32, 
             internal, largest, min_free);
}

// Format BLE address for display (reverse byte order)
void format_ble_addr(uint8_t *addr, char *output, size_t output_size) {
    snprintf(output, output_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}


// Helper to convert single hex digit to value
static inline int hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;  // Invalid hex character
}

int hex_string_to_bytes(const char *hex_str, uint8_t *bytes, int max_len) {
    if (hex_str == NULL || bytes == NULL || max_len <= 0) {
        return -1;
    }
    
    int len = strlen(hex_str);
    
    if (len % 2 != 0) {
        ESP_LOGE(TAG, "Invalid hex string length: %d (must be even)", len);
        return -1;
    }
    
    int byte_len = len / 2;
    if (byte_len > max_len) {
        ESP_LOGW(TAG, "Hex string too long: %d bytes > max %d", byte_len, max_len);
        return -1;
    }
    
    for (int i = 0; i < byte_len; i++) {
        int high = hex_char_to_int(hex_str[i * 2]);
        int low = hex_char_to_int(hex_str[i * 2 + 1]);
        
        if (high < 0 || low < 0) {
            ESP_LOGE(TAG, "Invalid hex character at position %d: '%c%c'", 
                     i * 2, hex_str[i * 2], hex_str[i * 2 + 1]);
            return -1;
        }
        
        bytes[i] = (uint8_t)((high << 4) | low);
    }
    
    return byte_len;
}


// Convert raw bytes to hex string with truncation (JSON-safe)
int bytes_to_hex_string(uint8_t *data, uint8_t len, uint16_t full_len, 
                                char *output, size_t output_size) {
    if (output_size < 4) return 0;
    
    size_t offset = 0;
    for (int i = 0; i < len && offset < output_size - 6; i++) {
        offset += snprintf(output + offset, output_size - offset, "%02X", data[i]);
        // Add space only between bytes, not after last byte
//        if (i < len - 1 && offset < output_size - 6) {
//            output[offset++] = ' ';
//        }
    }
    if (full_len > len && offset < output_size - 4) {
        offset += snprintf(output + offset, output_size - offset, "...");
    }
    if (offset < output_size) {
        output[offset] = '\0';
    }
    return offset;
}

// this one is used in ble_scanner
// TBD: merge with others 
/**
 * @brief Convert binary data to hexadecimal string
 * 
 * @param data Pointer to binary data
 * @param len Length of binary data
 * @param hex_str Output buffer for hex string (must be at least len*2 + 1 bytes)
 */
void bin_to_hex_string(const uint8_t *data, size_t len, char *hex_str) {
    const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        hex_str[i * 2] = hex_chars[(data[i] >> 4) & 0x0F];
        hex_str[i * 2 + 1] = hex_chars[data[i] & 0x0F];
    }
    hex_str[len * 2] = '\0';
}


// Helper to convert mbuf data to hex string
char* mbuf_to_hex_string(struct os_mbuf *om)
{
    if (!om) return strdup("");
    
    uint16_t len = OS_MBUF_PKTLEN(om);
    char *hex_str = malloc(len * 2 + 1);
    if (!hex_str) return strdup("");
    
    uint8_t byte;
    for (int i = 0; i < len; i++) {
        os_mbuf_copydata(om, i, 1, &byte);
        sprintf(&hex_str[i * 2], "%02x", byte);
    }
    hex_str[len * 2] = '\0';
    
    return hex_str;
}


void log_hex_data(const char *label, const uint8_t *data, uint16_t len) {
    if (data == NULL || len == 0) {
        ESP_LOGI(TAG, "%s: (empty)", label);
        return;
    }
    
    // Allocate temporary buffer on stack
    char hex_str[len * 3 + 1];  // "XX " per byte + null terminator
    int pos = 0;
    
    for (int i = 0; i < len; i++) {
        pos += sprintf(hex_str + pos, "%02x", data[i]);
    }
    
    // Remove trailing space
    if (pos > 0) {
        hex_str[pos - 1] = '\0';
    }
    
    ESP_LOGI(TAG, "%s (%d bytes): %s", label, len, hex_str);
}

// Helper function to print UUID
void print_uuid(const ble_uuid_t *uuid) {
    char buf[BLE_UUID_STR_LEN];
    ble_uuid_to_str(uuid, buf);
    printf("%s", buf);
}

static const char *skip_0x_prefix(const char *s)
{
    if (s == NULL) {
        return NULL;
    }

    if ((s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
        return s + 2;
    }

    return s;
}

bool uuid_str_matches(const char *uuid1, const char *uuid2)
{
    if (uuid1 == NULL || uuid2 == NULL) {
        return false;
    }

    uuid1 = skip_0x_prefix(uuid1);
    uuid2 = skip_0x_prefix(uuid2);

    return (strcasecmp(uuid1, uuid2) == 0);
}


char * ble_uuid_to_str_no_0x_prefix(const ble_uuid_t *uuid, char *dst)
{
    const uint8_t *u8p;

    switch (uuid->type) {
    case BLE_UUID_TYPE_16:
        sprintf(dst, "%04" PRIx16, BLE_UUID16(uuid)->value);
        break;
    case BLE_UUID_TYPE_32:
        sprintf(dst, "%08" PRIx32, BLE_UUID32(uuid)->value);
        break;
    case BLE_UUID_TYPE_128:
        u8p = BLE_UUID128(uuid)->value;

        sprintf(dst, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                     "%02x%02x%02x%02x%02x%02x",
                u8p[15], u8p[14], u8p[13], u8p[12],
                u8p[11], u8p[10],  u8p[9],  u8p[8],
                 u8p[7],  u8p[6],  u8p[5],  u8p[4],
                 u8p[3],  u8p[2],  u8p[1],  u8p[0]);
        break;
    default:
        dst[0] = '\0';
        break;
    }

    return dst;
}


// Helper function to escape string for JSON
void json_escape_string(const char *src, char *dst, size_t dst_size) {
    size_t dst_idx = 0;
    
    if (dst_size == 0) return;
    
    for (size_t i = 0; src[i] != '\0' && dst_idx < dst_size - 2; i++) {
        unsigned char c = src[i];
        
        // Escape special JSON characters
        if (c == '"') {
            if (dst_idx < dst_size - 3) {
                dst[dst_idx++] = '\\';
                dst[dst_idx++] = '"';
            } else break;
        } else if (c == '\\') {
            if (dst_idx < dst_size - 3) {
                dst[dst_idx++] = '\\';
                dst[dst_idx++] = '\\';
            } else break;
        } else if (c == '\b') {
            if (dst_idx < dst_size - 3) {
                dst[dst_idx++] = '\\';
                dst[dst_idx++] = 'b';
            } else break;
        } else if (c == '\f') {
            if (dst_idx < dst_size - 3) {
                dst[dst_idx++] = '\\';
                dst[dst_idx++] = 'f';
            } else break;
        } else if (c == '\n') {
            if (dst_idx < dst_size - 3) {
                dst[dst_idx++] = '\\';
                dst[dst_idx++] = 'n';
            } else break;
        } else if (c == '\r') {
            if (dst_idx < dst_size - 3) {
                dst[dst_idx++] = '\\';
                dst[dst_idx++] = 'r';
            } else break;
        } else if (c == '\t') {
            if (dst_idx < dst_size - 3) {
                dst[dst_idx++] = '\\';
                dst[dst_idx++] = 't';
            } else break;
        } else if (c < 0x20 || c == 0x7F) {
            // Control characters - escape as \uXXXX
            if (dst_idx < dst_size - 7) {
                int written = snprintf(dst + dst_idx, dst_size - dst_idx, "\\u%04x", c);
                if (written > 0) dst_idx += written;
            }
        } else if (c >= 0x80) {
            // High-bit characters - escape as \uXXXX for safety
            if (dst_idx < dst_size - 7) {
                int written = snprintf(dst + dst_idx, dst_size - dst_idx, "\\u%04x", c);
                if (written > 0) dst_idx += written;
            }
        } else {
            dst[dst_idx++] = c;
        }
    }
    
    dst[dst_idx] = '\0';
}



// Convert Unicode codepoint to UTF-8 byte sequence
// Returns number of bytes written (1-4 for valid UTF-8, 0 on error)
int unicode_to_utf8(uint32_t codepoint, char *out, size_t out_size) {
    if (out == NULL || out_size < 5) {  // Need space for max 4 bytes + null terminator
        return 0;
    }

    if (codepoint <= 0x7F) {
        // 1-byte UTF-8: 0xxxxxxx
        out[0] = (char)codepoint;
        out[1] = '\0';
        return 1;
    } 
    else if (codepoint <= 0x7FF) {
        // 2-byte UTF-8: 110xxxxx 10xxxxxx
        out[0] = (char)(0xC0 | ((codepoint >> 6) & 0x1F));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        out[2] = '\0';
        return 2;
    } 
    else if (codepoint <= 0xFFFF) {
        // 3-byte UTF-8: 1110xxxx 10xxxxxx 10xxxxxx
        out[0] = (char)(0xE0 | ((codepoint >> 12) & 0x0F));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        out[3] = '\0';
        return 3;
    } 
    else if (codepoint <= 0x10FFFF) {
        // 4-byte UTF-8: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        out[0] = (char)(0xF0 | ((codepoint >> 18) & 0x07));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (char)(0x80 | (codepoint & 0x3F));
        out[4] = '\0';
        return 4;
    }

    return 0;  // Invalid codepoint
}

