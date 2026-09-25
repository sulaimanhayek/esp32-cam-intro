#include "flash_led.h"

#include "driver/ledc.h"
#include "driver/rtc_io.h"

// The camera's XCLK uses LEDC timer 0 / channel 0, so take the next ones
#define FLASH_LEDC_TIMER   LEDC_TIMER_1
#define FLASH_LEDC_CHANNEL LEDC_CHANNEL_1
#define FLASH_LEDC_RES     LEDC_TIMER_8_BIT

static int s_percent;

void flash_led_init(void)
{
    // Release the hold from a previous deep sleep, if any
    rtc_gpio_hold_dis(FLASH_LED_GPIO);
    rtc_gpio_deinit(FLASH_LED_GPIO);

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = FLASH_LEDC_RES,
        .timer_num = FLASH_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num = FLASH_LED_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = FLASH_LEDC_CHANNEL,
        .timer_sel = FLASH_LEDC_TIMER,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    s_percent = 0;
}

void flash_led_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (percent * ((1 << FLASH_LEDC_RES) - 1)) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, FLASH_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, FLASH_LEDC_CHANNEL);
    s_percent = percent;
}

int flash_led_get(void)
{
    return s_percent;
}

void flash_led_hold_off_for_sleep(void)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, FLASH_LEDC_CHANNEL, 0);
    rtc_gpio_init(FLASH_LED_GPIO);
    rtc_gpio_set_direction(FLASH_LED_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(FLASH_LED_GPIO, 0);
    rtc_gpio_hold_en(FLASH_LED_GPIO);
}
