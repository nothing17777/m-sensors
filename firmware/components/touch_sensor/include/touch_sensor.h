/*
 * M touch sensor - ESP-IDF wrapper around touch_core.
 *
 * Reads MPR121 filtered capacitance over I2C (the controller chosen in the build
 * plan) at a fixed rate and reports touch sessions and gestures through a callback.
 *
 * Two ways to use it, same as the motion sensor:
 *  A) touch_sensor_start(): owns the I2C device and a FreeRTOS task.
 *  B) touch_sensor_init_feed() + touch_sensor_feed(): you supply the readings
 *     (another driver, ESP32-S3 native touch pads, or a test harness).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "touch_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*touch_sensor_cb_t)(const touch_result_t *result, void *user_ctx);

typedef struct {
    touch_config_t core;
    int      i2c_port;          /* I2C port number (0 on most boards)             */
    int      sda_gpio;
    int      scl_gpio;
    uint8_t  address;           /* MPR121: 0x5A with ADDR tied to GND             */
    uint32_t scl_hz;
    bool     internal_pullups;  /* false when the module already has pull-ups     */
    uint8_t  sample_hz;
    bool     notify_while_active;
    touch_sensor_cb_t callback;
    void    *callback_ctx;
    uint32_t task_stack;
    uint8_t  task_priority;
    int8_t   task_core;         /* -1 = no affinity                                */
} touch_sensor_config_t;

#define TOUCH_SENSOR_CONFIG_DEFAULT() {   \
    .core = TOUCH_CONFIG_DEFAULT(),       \
    .i2c_port = 0,                        \
    .sda_gpio = 39,                       \
    .scl_gpio = 38,                       \
    .address = 0x5A,                      \
    .scl_hz = 400000,                     \
    .internal_pullups = true,             \
    .sample_hz = 50,                      \
    .notify_while_active = true,          \
    .callback = NULL,                     \
    .callback_ctx = NULL,                 \
    .task_stack = 4096,                   \
    .task_priority = 5,                   \
    .task_core = 1,                       \
}

esp_err_t touch_sensor_start(const touch_sensor_config_t *cfg);
esp_err_t touch_sensor_stop(void);
bool      touch_sensor_is_running(void);
/* Forces the next reading to become the baseline (e.g. after the shell is wiped). */
void      touch_sensor_recalibrate(void);
/* Raw per-pad deltas below the baseline, for tuning. Returns the pad count. */
uint8_t   touch_sensor_deltas(uint16_t *out, uint8_t max_pads);

esp_err_t touch_sensor_init_feed(const touch_sensor_config_t *cfg);
esp_err_t touch_sensor_feed(const uint16_t *values, uint8_t pads, uint32_t t_ms, touch_result_t *out);

#ifdef __cplusplus
}
#endif
