#include "secret.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

static const char *TAG = "secret";

#define GENERATED_LEN 12

esp_err_t secret_get(const char *key, const char *configured, char *out, size_t len)
{
    if (strlen(configured) >= SECRET_MIN_LEN) {
        strlcpy(out, configured, len);
        return ESP_OK;
    }
    if (strlen(configured) > 0) {
        ESP_LOGW(TAG, "Configured \"%s\" is under %d chars - using a random one instead",
                 key, SECRET_MIN_LEN);
    }
    if (len <= GENERATED_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("secrets", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    size_t stored_len = len;
    if (nvs_get_str(nvs, key, out, &stored_len) != ESP_OK) {
        // No look-alike characters (0/O, 1/l/I) so it's easy to type
        static const char charset[] = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        for (size_t i = 0; i < GENERATED_LEN; i++) {
            out[i] = charset[esp_random() % (sizeof(charset) - 1)];
        }
        out[GENERATED_LEN] = '\0';
        err = nvs_set_str(nvs, key, out);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
    }
    nvs_close(nvs);
    return err;
}
