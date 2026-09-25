#pragma once

#include <stdbool.h>

#define FLASH_LED_GPIO 4

void flash_led_init(void);
// 0 = off, 1-100 = brightness in percent
void flash_led_set(int percent);
int flash_led_get(void);
// Drive the LED pin low and hold it there through deep sleep
void flash_led_hold_off_for_sleep(void);
