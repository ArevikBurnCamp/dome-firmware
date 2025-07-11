#pragma once

#include <stdint.h>
#include <stdbool.h>

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64

#define MAX_LEDS 300 // Maximum number of LEDs in the strip

typedef struct {
    char wifi_ssid[WIFI_SSID_MAX_LEN];
    char wifi_password[WIFI_PASSWORD_MAX_LEN];
    uint8_t brightness;
    bool power_state;
} app_config_t;
