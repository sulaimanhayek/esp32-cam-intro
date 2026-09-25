#pragma once

#include "esp_err.h"

// Join the configured router; if none is set (or it fails), start our own
// access point instead. Also advertises <hostname>.local over mDNS.
esp_err_t net_start(void);
