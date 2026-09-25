#include "camera.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "driver/rtc_io.h"
#include "sdkconfig.h"

#include "flash_led.h"
#include "storage.h"

static const char *TAG = "camera";

// AI-Thinker ESP32-CAM pin map
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD     26
#define CAM_PIN_SIOC     27
#define CAM_PIN_D7       35
#define CAM_PIN_D6       34
#define CAM_PIN_D5       39
#define CAM_PIN_D4       36
#define CAM_PIN_D3       21
#define CAM_PIN_D2       19
#define CAM_PIN_D1       18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC    25
#define CAM_PIN_HREF     23
#define CAM_PIN_PCLK     22

#define PHOTO_SIZE       FRAMESIZE_UXGA
#define MAX_SIZE_WAIT_FRAMES 10

static SemaphoreHandle_t s_lock;
static framesize_t s_stream_size;

esp_err_t camera_init(framesize_t stream_size)
{
    // Release the power-down hold from a previous deep sleep, if any
    rtc_gpio_hold_dis(CAM_PIN_PWDN);
    rtc_gpio_deinit(CAM_PIN_PWDN);

    if (esp_psram_get_size() == 0) {
        ESP_LOGE(TAG, "No PSRAM found - UXGA frame buffers need it");
        return ESP_ERR_NO_MEM;
    }

    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = PHOTO_SIZE,        // sizes the frame buffers for full-res photos
        .jpeg_quality = 10,              // 0-63, lower = better
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    s_stream_size = stream_size;
    if (stream_size != PHOTO_SIZE) {
        esp_camera_sensor_get()->set_framesize(esp_camera_sensor_get(), stream_size);
    }
    ESP_LOGI(TAG, "Camera ready (sensor PID 0x%02x)", esp_camera_sensor_get()->id.PID);
    return ESP_OK;
}

void camera_deinit_for_sleep(void)
{
    esp_camera_deinit();
    // Keep the sensor powered down while the ESP32 sleeps
    rtc_gpio_init(CAM_PIN_PWDN);
    rtc_gpio_set_direction(CAM_PIN_PWDN, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(CAM_PIN_PWDN, 1);
    rtc_gpio_hold_en(CAM_PIN_PWDN);
}

void camera_warmup(int frames)
{
    for (int i = 0; i < frames; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
    }
}

camera_fb_t *camera_grab(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        xSemaphoreGive(s_lock);
    }
    return fb;
}

void camera_release(camera_fb_t *fb)
{
    esp_camera_fb_return(fb);
    xSemaphoreGive(s_lock);
}

// Wait until frames come out at the requested width (a size switch takes a
// frame or two to reach the output), then drop `extra` more for exposure.
static void wait_for_width(int width, int extra)
{
    for (int i = 0; i < MAX_SIZE_WAIT_FRAMES; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            continue;
        }
        bool ok = fb->width == width;
        esp_camera_fb_return(fb);
        if (ok) {
            break;
        }
    }
    camera_warmup(extra);
}

esp_err_t camera_take_photo(bool use_flash, char *name_out, size_t name_len)
{
    sensor_t *s = esp_camera_sensor_get();
    bool switch_size = s_stream_size != PHOTO_SIZE;
    int prev_flash = flash_led_get();

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (switch_size) {
        s->set_framesize(s, PHOTO_SIZE);
    }
    if (use_flash) {
        flash_led_set(CONFIG_FLASH_BRIGHTNESS);
    }
    // With flash on, give auto exposure a few frames to adapt to the light
    wait_for_width(resolution[PHOTO_SIZE].width, use_flash ? 3 : 1);

    camera_fb_t *fb = esp_camera_fb_get();

    if (use_flash) {
        flash_led_set(prev_flash);
    }

    esp_err_t err = ESP_FAIL;
    if (fb) {
        ESP_LOGI(TAG, "Captured %ux%u, %u bytes%s", fb->width, fb->height,
                 (unsigned)fb->len, use_flash ? " (flash)" : "");
        err = storage_save_jpeg(fb->buf, fb->len, name_out, name_len);
        esp_camera_fb_return(fb);
    } else {
        ESP_LOGE(TAG, "Capture failed");
    }

    if (switch_size) {
        s->set_framesize(s, s_stream_size);
    }
    xSemaphoreGive(s_lock);
    return err;
}
