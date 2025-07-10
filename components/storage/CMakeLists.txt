idf_component_register(SRCS "storage.c"
                    INCLUDE_DIRS "."
                    REQUIRES "nvs_flash"
                    PRIV_REQUIRES main)
