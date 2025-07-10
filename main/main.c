#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "storage.h"
#include "led_driver.h"
#include "wifi_manager.h"
#include "udp_server.h"
#include "app_config.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    // 1. Initialize NVS (central point for NVS init)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Load configuration from storage first
    app_config_t app_config;
    storage_load_config(&app_config);

    // 3. Initialize LED driver with loaded or default config
    // Assuming these are compile-time constants from menuconfig for now
    led_driver_init(CONFIG_LED_GPIO, CONFIG_LED_COUNT);
    led_driver_set_brightness(app_config.brightness);

    // 4. Start WiFi manager with loaded config
    wifi_manager_start(&app_config);

    // 5. Wait for WiFi connection and start UDP server
    EventGroupHandle_t wifi_event_group = wifi_manager_get_event_group();
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected. Starting UDP server.");
        udp_server_start();
    } else {
        ESP_LOGE(TAG, "WiFi connection failed. UDP server not started.");
    }
}
