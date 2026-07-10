#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Install TinyUSB composite (CDC-ACM + CDC-NCM) and USB-side esp-netif. */
esp_err_t usb_net_init(void);

/** Human-readable USB NCM interface IP, or empty if not configured. */
void usb_net_get_ip(char *out, size_t out_sz);

/** True when USB host is mounted and NCM link is up. */
bool usb_net_link_up(void);
