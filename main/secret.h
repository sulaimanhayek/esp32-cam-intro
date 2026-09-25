#pragma once

#include <stddef.h>
#include "esp_err.h"

#define SECRET_MIN_LEN 8

// Returns `configured` if it's at least SECRET_MIN_LEN chars. Otherwise returns
// a random per-device value, created on first use and kept in NVS under `key`,
// so no shared default password ever ships with the firmware.
// NVS must already be initialised.
esp_err_t secret_get(const char *key, const char *configured, char *out, size_t len);
