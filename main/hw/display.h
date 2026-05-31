#pragma once

#include <stdint.h>
#include <stdbool.h>

void display_set_flush_ready_cb(void (*callback)(void));
void display_init(void);
void display_push_colors(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t *data);
