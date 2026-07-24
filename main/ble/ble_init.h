#pragma once
#include "host/ble_hs.h"

void initialize_bluetooth(void);
bool ble_wait_for_sync(uint32_t timeout_ms);