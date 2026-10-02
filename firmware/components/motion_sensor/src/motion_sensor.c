#include "motion_sensor.h"

#include <string.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "motion_sensor";

typedef struct {
    motion_sensor_config_t cfg;
    motion_core_t core;
    bool core_ready;
    bool configured;
    uint8_t *luma;
    size_t luma_cap;
    TaskHandle_t task;
    volatile bool run;
    volatile bool paused;
} ms_ctx_t;

static ms_ctx_t s_ctx;

static uint8_t *luma_buffer(size_t n)
{
    if (s_ctx.luma_cap < n) {
        heap_caps_free(s_ctx.luma);
        s_ctx.luma = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ctx.luma) {
            s_ctx.luma = heap_caps_malloc(n, MALLOC_CAP_8BIT);
        }
        s_ctx.luma_cap = s_ctx.luma ? n : 0;
    }
    return s_ctx.luma;
}

/* Returns a width*height 8-bit luma view of the frame, or NULL if unsupported. */
static const uint8_t *to_gray(const uint8_t *buf, size_t len, uint16_t w, uint16_t h, pixformat_t fmt)
{
    const size_t n = (size_t)w * h;
    switch (fmt) {
    case PIXFORMAT_GRAYSCALE:
        return len >= n ? buf : NULL;
    case PIXFORMAT_YUV422: { /* YUYV: Y at even bytes */
        uint8_t *y = len >= 2 * n ? luma_buffer(n) : NULL;
        if (y) {
            for (size_t i = 0; i < n; i++) {
                y[i] = buf[2 * i];
            }
        }
        return y;
    }
    case PIXFORMAT_RGB565: { /* big-endian RGB565 */
        uint8_t *y = len >= 2 * n ? luma_buffer(n) : NULL;
        if (y) {
            for (size_t i = 0; i < n; i++) {
                const uint8_t hi = buf[2 * i], lo = buf[2 * i + 1];
                const uint8_t r5 = hi >> 3, g6 = (uint8_t)(((hi & 0x07) << 3) | (lo >> 5)), b5 = lo & 0x1F;
                const uint32_t r = (uint32_t)(r5 << 3 | r5 >> 2);
                const uint32_t g = (uint32_t)(g6 << 2 | g6 >> 4);
                const uint32_t b = (uint32_t)(b5 << 3 | b5 >> 2);
                y[i] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
            }
        }
        return y;
    }
    default:
        return NULL;
    }
}

static esp_err_t ensure_core(uint16_t w, uint16_t h)
{
    if (s_ctx.core_ready && s_ctx.core.in_w == w && s_ctx.core.in_h == h) {
        return ESP_OK;
    }
    if (s_ctx.core_ready) {
        ESP_LOGW(TAG, "frame size changed to %ux%u, re-initialising", w, h);
        motion_core_deinit(&s_ctx.core);
        s_ctx.core_ready = false;
    }
    if (motion_core_init(&s_ctx.core, &s_ctx.cfg.core, w, h) != 0) {
        ESP_LOGE(TAG, "motion_core_init failed for %ux%u", w, h);
        return ESP_ERR_NO_MEM;
    }
    s_ctx.core_ready = true;
    ESP_LOGI(TAG, "motion core ready: %ux%u -> %ux%u cells", w, h, s_ctx.core.cw, s_ctx.core.ch);
    return ESP_OK;
}

static void notify(const motion_result_t *r)
{
    if (s_ctx.cfg.callback && (r->event != MOTION_EVT_NONE || (s_ctx.cfg.notify_while_active && r->active))) {
        s_ctx.cfg.callback(r, s_ctx.cfg.callback_ctx);
    }
}

esp_err_t motion_sensor_init_feed(const motion_sensor_config_t *cfg)
{
    if (!cfg || s_ctx.task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.cfg = *cfg;
    s_ctx.configured = true;
    return ESP_OK;
}

esp_err_t motion_sensor_feed(const uint8_t *buf, size_t len, uint16_t width, uint16_t height,
                             pixformat_t format, motion_result_t *out)
{
    if (!s_ctx.configured || !buf) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *gray = to_gray(buf, len, width, height, format);
    if (!gray) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = ensure_core(width, height);
    if (err != ESP_OK) {
        return err;
    }
    motion_result_t r;
    motion_core_process(&s_ctx.core, gray, (uint32_t)(esp_timer_get_time() / 1000), &r);
    notify(&r);
    if (out) {
        *out = r;
    }
    return ESP_OK;
}

static void motion_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / (s_ctx.cfg.fps ? s_ctx.cfg.fps : 10));
    TickType_t last_wake = xTaskGetTickCount();
    bool warned_format = false;

    while (s_ctx.run) {
        vTaskDelayUntil(&last_wake, period > 0 ? period : 1);
        if (s_ctx.paused) {
            continue;
        }
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "camera frame grab failed");
            continue;
        }
        const pixformat_t format = fb->format;
        esp_err_t err = motion_sensor_feed(fb->buf, fb->len, (uint16_t)fb->width, (uint16_t)fb->height,
                                           format, NULL);
        esp_camera_fb_return(fb);
        if (err == ESP_ERR_NOT_SUPPORTED && !warned_format) {
            ESP_LOGE(TAG, "unsupported pixel format %d (use GRAYSCALE, YUV422 or RGB565)", (int)format);
            warned_format = true;
        }
    }
    s_ctx.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t motion_sensor_start(const motion_sensor_config_t *cfg)
{
    if (!cfg || s_ctx.task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.cfg = *cfg;
    s_ctx.configured = true;
    s_ctx.paused = false;
    s_ctx.run = true;
    BaseType_t ok;
    if (cfg->task_core < 0) {
        ok = xTaskCreate(motion_task, "motion", cfg->task_stack, NULL, cfg->task_priority, &s_ctx.task);
    } else {
        ok = xTaskCreatePinnedToCore(motion_task, "motion", cfg->task_stack, NULL, cfg->task_priority,
                                     &s_ctx.task, cfg->task_core);
    }
    if (ok != pdPASS) {
        s_ctx.run = false;
        s_ctx.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "started at %u fps", cfg->fps);
    return ESP_OK;
}

esp_err_t motion_sensor_stop(void)
{
    if (!s_ctx.task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.run = false;
    for (int i = 0; i < 100 && s_ctx.task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_ctx.core_ready) {
        motion_core_deinit(&s_ctx.core);
        s_ctx.core_ready = false;
    }
    return ESP_OK;
}

void motion_sensor_pause(bool paused)
{
    s_ctx.paused = paused;
}

bool motion_sensor_is_running(void)
{
    return s_ctx.task != NULL;
}
