#pragma once
#include <stdbool.h>
#include "host/ble_uuid.h"


#define ANSI_RED     "\x1b[31m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_RESET   "\x1b[0m"

void log_memory_usage(const char *message);
void format_ble_addr(uint8_t *addr, char *output, size_t output_size);
void log_heap_details(const char *label);
int hex_string_to_bytes(const char *hex_str, uint8_t *bytes, int max_len);
int bytes_to_hex_string(uint8_t *data, uint8_t len, uint16_t full_len, char *output, size_t output_size);
void bin_to_hex_string(const uint8_t *data, size_t len, char *hex_str);
char* mbuf_to_hex_string(struct os_mbuf *om);
void log_hex_data(const char *label, const uint8_t *data, uint16_t len);
void print_uuid(const ble_uuid_t *uuid);
bool uuid_str_matches(const char *uuid1, const char *uuid2);
char * ble_uuid_to_str_no_0x_prefix(const ble_uuid_t *uuid, char *dst);
void json_escape_string(const char *src, char *dst, size_t dst_size);
int unicode_to_utf8(uint32_t codepoint, char *out, size_t out_size);