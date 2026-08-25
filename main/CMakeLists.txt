idf_component_register(SRCS "main.c"
                            "improv_serial_codec.c"
                            "maintenance_serial.c"
                            "ota_update.c"
                            "relay_spool.c"
                            "relay_stream.c"
                            "wifi_provision.c"
                       INCLUDE_DIRS "."
                       REQUIRES app_update core tapdrop esp_driver_gpio
                                esp_driver_rmt esp_driver_usb_serial_jtag
                                esp_event esp_http_client esp_https_ota
                                esp_netif esp_system esp_wifi lwip mbedtls
                                nvs_flash spi_flash)
