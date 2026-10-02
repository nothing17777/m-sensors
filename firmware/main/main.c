/*
 * M motion sensor system - ESP32-S3 app.
 *
 *   OV5640 (QQVGA gray, 10 fps) -> motion_sensor -.
 *   MPR121 touch pads (50 Hz)    -> touch_sensor  -+-> behavior -> LED ring (SK6812 RGBW)
 *                                        |
 *                                        +-> app_request_face_scan()  --> EchoAi integration (other module)
 *                                                                         calls m_on_emotion()
 *
 * Until the EchoAi integration exists, DEMO_FAKE_EMOTIONS answers each face-scan
 * request with a fake emotion so the whole motion -> emotion -> light flow can be
 * tested on the hardware.
 */
#include <stdio.h>

#include "board_pins.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "expression.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "motion_sensor.h"
#include "touch_sensor.h"

#define DEMO_FAKE_EMOTIONS   1      /* set 0 once the real EchoAi module calls m_on_emotion() */
#define RESCAN_INTERVAL_S    4.0f   /* re-scan faces this often while someone is present */
#define LED_FPS              50

static const char *TAG = "m";

static SemaphoreHandle_t s_lock;
static behavior_t s_behavior;
static led_strip_handle_t s_strip;
static float s_last_scan_s = -1e9f;

static float now_s(void)
{
    return (float)((double)esp_timer_get_time() / 1e6);
}

/* ---------------- public hooks for other modules ---------------- */

/* Called by the EchoAi integration module with the API result. Thread-safe. */
void m_on_emotion(const char *label, float confidence, float intensity)
{
    const emotion_t e = emotion_from_label(label);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool accepted = behavior_on_emotion(&s_behavior, e, confidence, intensity, now_s());
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "EMOTION %s (\"%s\") conf=%.2f %s", emotion_name(e), label ? label : "", confidence,
             accepted ? "" : "(ignored: low confidence)");
}

/* Called by the face detection module whenever it sees a face (keeps presence alive). */
void m_on_face(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    behavior_on_face(&s_behavior, now_s());
    xSemaphoreGive(s_lock);
}

#if DEMO_FAKE_EMOTIONS
static void fake_emotion_cb(void *arg)
{
    static const char *const labels[] = {"happy", "surprised", "neutral", "sad", "angry"};
    static int i = 0;
    (void)arg;
    m_on_emotion(labels[i++ % 5], 0.9f, 0.7f);
}
#endif

/* Override this (non-weak definition in the EchoAi module) to run real face detection + upload. */
__attribute__((weak)) void app_request_face_scan(const motion_result_t *m)
{
    ESP_LOGI(TAG, "face scan requested (motion at x=%.2f) - no EchoAi module linked yet", m->centroid_x);
#if DEMO_FAKE_EMOTIONS
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t args = {.callback = fake_emotion_cb, .name = "fake_emotion"};
        esp_timer_create(&args, &t);
    }
    esp_timer_stop(t);
    esp_timer_start_once(t, 400 * 1000);  /* simulate ~400 ms API latency */
#endif
}

/* ---------------- motion callback (runs in motion task) ---------------- */

static void on_motion(const motion_result_t *m, void *ctx)
{
    (void)ctx;
    const float t = now_s();
    bool scan = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const behavior_mode_t before = s_behavior.mode;
    behavior_on_motion(&s_behavior, m, t);
    if (behavior_take_scan_request(&s_behavior) ||
        (behavior_wants_faces(&s_behavior, t) && m->active && t - s_last_scan_s >= RESCAN_INTERVAL_S)) {
        s_last_scan_s = t;
        scan = true;
    }
    const behavior_mode_t after = s_behavior.mode;
    xSemaphoreGive(s_lock);

    if (m->event == MOTION_EVT_START) {
        ESP_LOGI(TAG, "MOTION START level=%s area=%.1f%% x=%.2f", motion_level_name(m->level),
                 m->area_ratio * 100.0f, m->centroid_x);
    } else if (m->event == MOTION_EVT_END) {
        ESP_LOGI(TAG, "MOTION END duration=%.1fs", m->duration_ms / 1000.0f);
    } else if (m->event == MOTION_EVT_LIGHTING) {
        ESP_LOGI(TAG, "scene changed (lights or the device moved) - re-learning the view");
    }
    if (m->event == MOTION_EVT_UPDATE && m->trend != MOTION_TREND_STEADY) {
        ESP_LOGD(TAG, "person %s", motion_trend_name(m->trend));
    }
    if (before != after) {
        ESP_LOGI(TAG, "mode %s -> %s", behavior_mode_name(before), behavior_mode_name(after));
    }
    if (scan) {
        app_request_face_scan(m);
    }
}

/* ---------------- touch callback (runs in touch task) ---------------- */

/* Override this in a haptics module if an actuator is fitted (see the sketch). */
__attribute__((weak)) void app_haptic(haptic_t h)
{
    static haptic_t last = HAPTIC_NONE;
    if (h != last) {
        ESP_LOGD(TAG, "haptic -> %d (no actuator fitted)", (int)h);
        last = h;
    }
}

static void on_touch(const touch_result_t *t, void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    behavior_on_touch(&s_behavior, t, now_s());
    xSemaphoreGive(s_lock);

    if (t->event == TOUCH_EVT_START) {
        ESP_LOGI(TAG, "TOUCH START on the %s (%.0f deg) strength=%.2f", touch_zone_name(t->zone),
                 t->has_angle ? t->angle : 0.0f, t->strength);
    } else if (t->event == TOUCH_EVT_END) {
        ESP_LOGI(TAG, "TOUCH END gesture=%s duration=%.1fs", touch_gesture_name(t->gesture),
                 t->duration_ms / 1000.0f);
    }
    if (t->handled) {
        ESP_LOGW(TAG, "every touch pad at once - the device is being handled, baselines reset");
    } else if (t->recalibrated) {
        ESP_LOGW(TAG, "touch pad was stuck - baseline reset");
    }
}

/* ---------------- LED task ---------------- */

static void led_task(void *arg)
{
    (void)arg;
    led_ring_t ring;
    uint8_t px[LED_RING_COUNT * 4];
    led_ring_init(&ring, LED_RING_COUNT, LED_RING_FRONT_IDX, 0.5f);
    TickType_t last_wake = xTaskGetTickCount();
    behavior_mode_t logged = MODE_AMBIENT;

    for (;;) {
        const float t = now_s();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        behavior_tick(&s_behavior, t);
        const expression_t x = behavior_expression(&s_behavior);
        const behavior_mode_t mode = s_behavior.mode;
        xSemaphoreGive(s_lock);

        if (mode != logged) {
            ESP_LOGI(TAG, "mode -> %s", behavior_mode_name(mode));
            logged = mode;
        }
        app_haptic(x.haptic);
        led_ring_render(&ring, &x, t, px);
        for (int i = 0; i < LED_RING_COUNT; i++) {
            led_strip_set_pixel_rgbw(s_strip, i, px[i * 4], px[i * 4 + 1], px[i * 4 + 2], px[i * 4 + 3]);
        }
        led_strip_refresh(s_strip);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000 / LED_FPS));
    }
}

/* ---------------- init ---------------- */

static esp_err_t camera_init(void)
{
    const camera_config_t cfg = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_GRAYSCALE,  /* if unsupported on your module, use PIXFORMAT_YUV422 */
        .frame_size = FRAMESIZE_QQVGA,        /* 160x120, same as the PC prototype */
        .jpeg_quality = 12,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&cfg);
}

static esp_err_t led_init(void)
{
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_RING_GPIO,
        .max_leds = LED_RING_COUNT,
        .led_model = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRBW,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    return led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
}

void app_main(void)
{
    s_lock = xSemaphoreCreateMutex();
    const behavior_config_t bcfg = BEHAVIOR_CONFIG_DEFAULT();
    behavior_init(&s_behavior, &bcfg);

    ESP_ERROR_CHECK(led_init());
    led_strip_clear(s_strip);
    xTaskCreatePinnedToCore(led_task, "leds", 4096, NULL, 4, NULL, 0);

    touch_sensor_config_t tcfg = TOUCH_SENSOR_CONFIG_DEFAULT();
    tcfg.sda_gpio = TOUCH_I2C_SDA;
    tcfg.scl_gpio = TOUCH_I2C_SCL;
    tcfg.address = TOUCH_I2C_ADDR;
    tcfg.core.pads = TOUCH_PADS;
    tcfg.callback = on_touch;
    esp_err_t terr = touch_sensor_start(&tcfg);
    if (terr != ESP_OK) {
        ESP_LOGE(TAG, "touch sensor init failed: %s - check the MPR121 wiring (SDA %d, SCL %d). "
                 "Motion and lights keep working.", esp_err_to_name(terr), TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    }

    esp_err_t err = camera_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: %s - check board_pins.h wiring. LEDs stay in ambient mode.",
                 esp_err_to_name(err));
        return;
    }

    motion_sensor_config_t mcfg = MOTION_SENSOR_CONFIG_DEFAULT();
    mcfg.callback = on_motion;
    ESP_ERROR_CHECK(motion_sensor_start(&mcfg));
    ESP_LOGI(TAG, "M motion sensor running");
}
