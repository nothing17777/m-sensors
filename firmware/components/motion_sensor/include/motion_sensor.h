/*
 * M motion sensor - ESP-IDF wrapper around motion_core.
 *
 * Two ways to use it:
 *  A) motion_sensor_start(): owns a FreeRTOS task that grabs camera frames
 *     (esp32-camera must already be initialised) and calls your callback.
 *  B) motion_sensor_feed(): if another task owns the camera (e.g. the face
 *     detection pipeline), hand each frame buffer to the motion sensor.
 *
 * Accepted pixel formats: GRAYSCALE, YUV422 (YUYV), RGB565 (big-endian, as esp32-camera outputs).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "motion_core.h"
#include "sensor.h"   /* pixformat_t from esp32-camera */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*motion_sensor_cb_t)(const motion_result_t *result, void *user_ctx);

typedef struct {
    motion_config_t    core;
    uint8_t            fps;                  /* processing rate (task mode)                 */
    bool               notify_while_active;  /* callback every frame during a session       */
    motion_sensor_cb_t callback;
    void              *callback_ctx;
    uint32_t           task_stack;
    uint8_t            task_priority;
    int8_t             task_core;            /* -1 = no affinity                            */
} motion_sensor_config_t;

#define MOTION_SENSOR_CONFIG_DEFAULT() {         \
    .core = MOTION_CONFIG_DEFAULT(),             \
    .fps = 10,                                   \
    .notify_while_active = true,                 \
    .callback = NULL,                            \
    .callback_ctx = NULL,                        \
    .task_stack = 6144,                          \
    .task_priority = 5,                          \
    .task_core = 1,                              \
}

/* Mode A: start the camera-grabbing task. */
esp_err_t motion_sensor_start(const motion_sensor_config_t *cfg);
esp_err_t motion_sensor_stop(void);
/* Pause frame grabbing (e.g. while another module reconfigures the camera). */
void      motion_sensor_pause(bool paused);
bool      motion_sensor_is_running(void);

/* Mode B: configure once (callback, core params), then feed frames. fps/task fields are ignored. */
esp_err_t motion_sensor_init_feed(const motion_sensor_config_t *cfg);
esp_err_t motion_sensor_feed(const uint8_t *buf, size_t len, uint16_t width, uint16_t height,
                             pixformat_t format, motion_result_t *out);

#ifdef __cplusplus
}
#endif
