#include "storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

#define STORAGE_NAMESPACE "app_storage"
#define CONFIG_KEY "app_config"

static const char *TAG = "STORAGE";

esp_err_t storage_save_config(const app_config_t *config) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs_handle, CONFIG_KEY, config, sizeof(app_config_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) setting blob!", esp_err_to_name(err));
    } else {
        err = nvs_commit(nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error (%s) committing updates!", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Configuration saved successfully");
        }
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t storage_load_config(app_config_t *config) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        // Set default values if NVS cannot be opened
        strcpy(config->wifi_ssid, "");
        strcpy(config->wifi_password, "");
        config->brightness = 128;
        return err;
    }

    size_t required_size = sizeof(app_config_t);
    err = nvs_get_blob(nvs_handle, CONFIG_KEY, config, &required_size);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Configuration not found in NVS. Loading default values.");
        strcpy(config->wifi_ssid, "");
        strcpy(config->wifi_password, "");
        config->brightness = 128;
        err = ESP_OK; // It's not an error if the config is not found, we just use defaults
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) reading configuration from NVS!", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Configuration loaded successfully");
    }

    nvs_close(nvs_handle);
    return err;
}
