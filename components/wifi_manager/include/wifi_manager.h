#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "freertos/event_groups.h"
#include "app_config.h"

extern const int WIFI_CONNECTED_BIT;

void wifi_manager_start(const app_config_t *config);
EventGroupHandle_t wifi_manager_get_event_group(void);

#endif // WIFI_MANAGER_H
