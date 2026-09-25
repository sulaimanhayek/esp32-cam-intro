#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define STORAGE_MOUNT_POINT "/sdcard"

esp_err_t storage_mount(void);
void storage_unmount(void);
bool storage_is_mounted(void);
// Mount the card if it isn't mounted yet (e.g. inserted after boot).
// Retries at most every few seconds; returns true when a card is mounted.
bool storage_ensure_mounted(void);

// Save as the next IMG_NNNN.jpg; the file name (not path) is written to name_out
esp_err_t storage_save_jpeg(const uint8_t *data, size_t len, char *name_out, size_t name_len);
