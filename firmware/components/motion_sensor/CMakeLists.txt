idf_component_register(
    SRCS "src/motion_core.c" "src/motion_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer)
