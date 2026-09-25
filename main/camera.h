#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_camera.h"

#define CAM_PIN_PWDN 32

// Buffers are always sized for full UXGA; `stream_size` is what the sensor runs
// at between photos (e.g. SVGA for a smooth live stream).
esp_err_t camera_init(framesize_t stream_size);
void camera_deinit_for_sleep(void);

// Throw away frames so auto exposure / white balance can settle
void camera_warmup(int frames);

// Grab a frame at the stream size. The camera is locked until camera_release().
camera_fb_t *camera_grab(void);
void camera_release(camera_fb_t *fb);

// Take a full-resolution photo (optionally with flash) and save it to the SD card
esp_err_t camera_take_photo(bool use_flash, char *name_out, size_t name_len);
