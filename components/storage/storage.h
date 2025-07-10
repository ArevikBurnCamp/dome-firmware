#pragma once

#include "app_config.h"
#include "esp_err.h"

esp_err_t storage_load_config(app_config_t *config);
esp_err_t storage_save_config(const app_config_t *config);
