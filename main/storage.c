#include "storage.h"

#include <stdio.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage";

static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_lock;

// Survives deep sleep, so timelapse wakeups don't have to rescan the card
static RTC_DATA_ATTR int s_next_index;

esp_err_t storage_mount(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    // 1-bit mode: uses GPIO 14 (CLK), 15 (CMD), 2 (D0) and leaves GPIO 4
    // (flash LED) and GPIO 13 (button) free.
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(STORAGE_MOUNT_POINT, &host, &slot, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s (is the card inserted and FAT32?)", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }
    ESP_LOGI(TAG, "SD card mounted: %s, %llu MB", s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024));
    return ESP_OK;
}

void storage_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT_POINT, s_card);
        s_card = NULL;
    }
}

bool storage_is_mounted(void)
{
    return s_card != NULL;
}

bool storage_ensure_mounted(void)
{
    static int64_t s_last_try_us = INT64_MIN / 2;
    static portMUX_TYPE s_try_mux = portMUX_INITIALIZER_UNLOCKED;

    if (s_card) {
        return true;
    }
    // A failed mount blocks for a while, so don't retry on every request
    int64_t now = esp_timer_get_time();
    bool try_now = false;
    portENTER_CRITICAL(&s_try_mux);
    if (now - s_last_try_us >= 3 * 1000000LL) {
        s_last_try_us = now;
        try_now = true;
    }
    portEXIT_CRITICAL(&s_try_mux);

    if (try_now) {
        storage_mount();
    }
    return s_card != NULL;
}

// Continue numbering after whatever IMG_NNNN.jpg files are already on the card
static int scan_next_index(void)
{
    int max_idx = 0;
    DIR *dir = opendir(STORAGE_MOUNT_POINT);
    if (!dir) {
        return 1;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int idx;
        if (sscanf(ent->d_name, "IMG_%d.", &idx) == 1 && idx > max_idx) {
            max_idx = idx;
        }
    }
    closedir(dir);
    return max_idx + 1;
}

esp_err_t storage_save_jpeg(const uint8_t *data, size_t len, char *name_out, size_t name_len)
{
    if (!s_card) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_next_index <= 0) {
        s_next_index = scan_next_index();
    }

    char path[40];
    snprintf(name_out, name_len, "IMG_%04d.jpg", s_next_index);
    snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/%s", name_out);

    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = ESP_FAIL;
    FILE *f = fopen(path, "wb");
    if (f) {
        size_t written = fwrite(data, 1, len, f);
        fclose(f);
        if (written == len) {
            s_next_index++;
            ret = ESP_OK;
            ESP_LOGI(TAG, "Saved %s (%u bytes, %lld ms)", path, (unsigned)len,
                     (esp_timer_get_time() - t0) / 1000);
        } else {
            ESP_LOGE(TAG, "Short write to %s (%u of %u bytes)", path, (unsigned)written, (unsigned)len);
        }
    } else {
        ESP_LOGE(TAG, "Failed to open %s", path);
    }
    xSemaphoreGive(s_lock);
    return ret;
}
