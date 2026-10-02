idf_component_register(
    SRCS "src/touch_core.c" "src/touch_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_driver_i2c)
