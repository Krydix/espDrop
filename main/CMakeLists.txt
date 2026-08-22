idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES core tapdrop esp_driver_gpio esp_driver_rmt
                                esp_system esp_wifi nvs_flash spi_flash)
