idf_component_register(SRCS "esp32_freertos_advanced.c"
                    INCLUDE_DIRS "."
                    REQUIRES freertos driver esp_timer esp_system
                    )
