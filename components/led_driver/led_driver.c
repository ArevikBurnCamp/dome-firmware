#include "led_driver.h"
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "LED_DRIVER";
static led_strip_handle_t led_strip;
static uint16_t s_led_count = 0;

esp_err_t led_driver_init(int gpio, uint16_t led_count) {
    s_led_count = led_count;
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio,
        .max_leds = led_count,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_LOGI(TAG, "LED strip initialized");
    return ESP_OK;
}

esp_err_t led_driver_set_pixel(uint16_t index, uint8_t r, uint8_t g, uint8_t b) {
    return led_strip_set_pixel(led_strip, index, r, g, b);
}

esp_err_t led_driver_refresh() {
    return led_strip_refresh(led_strip);
}

esp_err_t led_driver_clear() {
    return led_strip_clear(led_strip);
}

esp_err_t led_driver_set_brightness(uint8_t brightness) {
    return led_strip_set_brightness(led_strip, brightness);
}

void led_driver_show_frame(const uint8_t *rgb_buffer, size_t led_count) {
    for (size_t i = 0; i < led_count; i++) {
        led_driver_set_pixel(i, rgb_buffer[i * 3], rgb_buffer[i * 3 + 1], rgb_buffer[i * 3 + 2]);
    }
    led_driver_refresh();
}
