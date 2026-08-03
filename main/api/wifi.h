#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

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

/** WiFi IP only (STA then AP); returns "(not connected)" if neither has an address. */
void wifi_get_current_ip(char *out, size_t out_sz);

/** Heap-allocated wifi_status JSON message; caller must free. NULL on OOM. */
char *wifi_status_message_new(void);

/** Broadcast wifi_status to all WS clients if status changed. */
void wifi_broadcast_status(void);

/** Register IP_EVENT handler once (call before WiFi init). */
void wifi_status_register_events(void);

/** NVS-backed WiFi settings (passwords never included). Caller must cJSON_Delete(). */
cJSON *wifi_config_to_json(void);

/** Live WiFi/USB/runtime status. Caller must cJSON_Delete(). */
cJSON *wifi_status_to_json(void);

/** Partial update from JSON object; does not write NVS. err_buf optional (max 64). */
esp_err_t wifi_config_apply_json(cJSON *json, char *err_buf, size_t err_sz);

esp_err_t wifi_config_save(void);

void wifi_config_set_enabled(bool on);

void wifi_init_with_fallback(void);
