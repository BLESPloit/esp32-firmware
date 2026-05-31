#pragma once

#include <stddef.h>
#include "esp_err.h"

#define MAX_WIFI_SSID_LENGTH 64
#define MAX_WIFI_PSK_LENGTH 64

/** IEEE / ESP soft-AP SSID max octets (excludes NUL). */
#define MAX_WIFI_AP_SSID_LEN 32

#define WIFI_AP_SSID        "BLESPLO.it"
#define WIFI_AP_PASS        "BLESPlo.it"

/** Stored in NVS `wifi_mode_pref` as uint8. Default STA-first preserves legacy boot logic. */
#define WIFI_MODE_PREF_STA_FIRST 0
#define WIFI_MODE_PREF_AP_ONLY   1

void wifi_build_default_ap_ssid(char *out, size_t out_sz);

/** Empty/null SSID string returns ESP_OK (caller treats as default). Non-empty must be 1..MAX_WIFI_AP_SSID_LEN. */
esp_err_t wifi_validate_custom_ap_ssid(const char *ssid);

/** Null or empty: valid (use default password). Otherwise WPA2 passphrase length 8..63. */
esp_err_t wifi_validate_custom_ap_psk(const char *psk);

/** Resolved SSID string that AP mode would broadcast (custom NVS or MAC-based default). */
void wifi_get_ap_ssid_effective(char *out, size_t out_sz);

void wifi_get_current_ip(char *out, size_t out_sz);

void wifi_init_with_fallback(void);
