/*
 * ESP32-CAM (AI-Thinker) starter
 *
 * Stream mode:    WiFi live stream + web UI, photos from the page or the button.
 * Timelapse mode: wake from deep sleep on a timer or the button, save one
 *                 photo to the SD card, sleep again.
 *
 * Pick the mode (and WiFi credentials) in `idf.py menuconfig` -> ESP32-CAM App.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "sdkconfig.h"

#include "camera.h"
#include "flash_led.h"
#include "storage.h"

#if CONFIG_APP_MODE_STREAM
#include "net.h"
#include "web.h"
#endif

static const char *TAG = "app";

#define BUTTON_GPIO     CONFIG_BUTTON_GPIO
#define BUTTON_POLL_MS  20
#define BUTTON_DEBOUNCE 3   // consecutive polls that must agree

#ifdef CONFIG_PHOTO_USE_FLASH
#define PHOTO_USE_FLASH true
#else
#define PHOTO_USE_FLASH false
#endif

#if CONFIG_APP_MODE_STREAM

// Short, dim blink so a no-flash photo still gives some feedback
static void ack_blink(void)
{
    int prev = flash_led_get();
    if (prev == 0) {
        flash_led_set(2);
        vTaskDelay(pdMS_TO_TICKS(80));
        flash_led_set(0);
    }
}

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    int stable_count = 0;
    bool pressed = false;
    while (true) {
        bool down = gpio_get_level(BUTTON_GPIO) == 0;
        stable_count = (down != pressed) ? stable_count + 1 : 0;

        if (stable_count >= BUTTON_DEBOUNCE) {
            pressed = down;
            stable_count = 0;
            if (pressed) {
                char name[24];
                ESP_LOGI(TAG, "Button pressed - taking photo");
                if (camera_take_photo(PHOTO_USE_FLASH, name, sizeof(name)) == ESP_OK
                    && !PHOTO_USE_FLASH) {
                    ack_blink();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

static void run_stream_mode(void)
{
    // Stream at SVGA for a smooth frame rate; photos are still taken at UXGA
    if (camera_init(FRAMESIZE_SVGA) != ESP_OK) {
        return;
    }
    if (storage_mount() != ESP_OK) {
        ESP_LOGW(TAG, "No SD card - streaming only, photos can't be saved");
    }

    net_start();
    web_start();
    xTaskCreate(button_task, "button", 4096, NULL, 5, NULL);
}

#else  // CONFIG_APP_MODE_TIMELAPSE

static const char *wakeup_reason(void)
{
    uint32_t causes = esp_sleep_get_wakeup_causes();
    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT0)) {
        return "button";
    }
    if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        return "timer";
    }
    return "power-on";
}

static void go_to_sleep(void)
{
    // If the button is still held, ext0 would wake us straight back up
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);
    for (int i = 0; i < 250 && gpio_get_level(BUTTON_GPIO) == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }

    flash_led_hold_off_for_sleep();

    rtc_gpio_pullup_en(BUTTON_GPIO);
    rtc_gpio_pulldown_dis(BUTTON_GPIO);
    esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, 0);
    esp_sleep_enable_timer_wakeup((uint64_t)CONFIG_TIMELAPSE_INTERVAL_SEC * 1000000);

    ESP_LOGI(TAG, "Sleeping for %d s (or until the button is pressed)", CONFIG_TIMELAPSE_INTERVAL_SEC);
    esp_deep_sleep_start();
}

static void run_timelapse_mode(void)
{
    ESP_LOGI(TAG, "Timelapse wakeup (%s)", wakeup_reason());

    if (camera_init(FRAMESIZE_UXGA) == ESP_OK) {
        // A freshly powered sensor needs a few frames for exposure to settle
        camera_warmup(5);
        if (storage_mount() == ESP_OK) {
            char name[24];
            camera_take_photo(PHOTO_USE_FLASH, name, sizeof(name));
            storage_unmount();
        }
        camera_deinit_for_sleep();
    }
    go_to_sleep();
}

#endif

void app_main(void)
{
    flash_led_init();

#if CONFIG_APP_MODE_STREAM
    run_stream_mode();
#else
    run_timelapse_mode();
#endif
}
