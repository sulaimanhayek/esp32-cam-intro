#pragma once

#include "esp_err.h"

// Control UI + API on port 80, MJPEG stream on port 81
esp_err_t web_start(void);
