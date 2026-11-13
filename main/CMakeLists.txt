idf_component_register(SRCS "main.c" "ble_battery.c" "oled.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_lcd esp_driver_i2c esp_timer)
