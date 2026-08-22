idf_component_register(
    SRCS
        "src/correlation.c"
        "src/tapdrop.c"
    INCLUDE_DIRS "include"
    REQUIRES core esp_driver_gpio esp_timer
)
