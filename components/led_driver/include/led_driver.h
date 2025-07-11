#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t led_driver_init(int gpio, uint16_t led_count);
esp_err_t led_driver_set_pixel(uint16_t index, uint8_t r, uint8_t g, uint8_t b);
esp_err_t led_driver_refresh();
esp_err_t led_driver_clear();
esp_err_t led_driver_set_brightness(uint8_t brightness);
void led_driver_show_frame(const uint8_t *rgb_buffer, size_t led_count);

#endif // LED_DRIVER_H
