idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "led_driver" "storage" "wifi_manager" "udp_server" "nvs_flash")
