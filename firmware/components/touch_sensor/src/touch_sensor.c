#include "touch_sensor.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch_sensor";

/* MPR121 registers */
#define MPR121_FILTDATA_L   0x04
#define MPR121_TOUCHTH_0    0x41
#define MPR121_RELEASETH_0  0x42
#define MPR121_DEBOUNCE     0x5B
#define MPR121_CONFIG1      0x5C
#define MPR121_CONFIG2      0x5D
#define MPR121_ECR          0x5E
#define MPR121_SOFTRESET    0x80

typedef struct {
    touch_sensor_config_t cfg;
    touch_core_t core;
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    bool configured;
    TaskHandle_t task;
    volatile bool run;
    uint16_t values[TOUCH_MAX_PADS];
} ts_ctx_t;

static ts_ctx_t s_ctx;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_ctx.dev, buf, sizeof(buf), 100);
}

static esp_err_t mpr121_init(uint8_t pads)
{
    esp_err_t err = reg_write(MPR121_SOFTRESET, 0x63);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MPR121 not responding at 0x%02x: %s", s_ctx.cfg.address, esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK_WITHOUT_ABORT(reg_write(MPR121_ECR, 0x00));   /* stop mode while configuring */

    /* baseline filter (Freescale/NXP application-note defaults) */
    const uint8_t filt[][2] = {
        {0x2B, 0x01}, {0x2C, 0x01}, {0x2D, 0x0E}, {0x2E, 0x00},   /* rising  */
        {0x2F, 0x01}, {0x30, 0x05}, {0x31, 0x01}, {0x32, 0x00},   /* falling */
        {0x33, 0x00}, {0x34, 0x00}, {0x35, 0x00}, {0x36, 0x00},   /* touched */
    };
    for (size_t i = 0; i < sizeof(filt) / sizeof(filt[0]); i++) {
        err = reg_write(filt[i][0], filt[i][1]);
        if (err != ESP_OK) return err;
    }
    /* The chip's own thresholds are only a coarse pre-filter; touch_core does the real work. */
    for (uint8_t i = 0; i < TOUCH_MAX_PADS; i++) {
        reg_write((uint8_t)(MPR121_TOUCHTH_0 + 2 * i), (uint8_t)s_ctx.cfg.core.touch_threshold);
        reg_write((uint8_t)(MPR121_RELEASETH_0 + 2 * i), (uint8_t)s_ctx.cfg.core.release_threshold);
    }
    reg_write(MPR121_DEBOUNCE, 0x00);
    reg_write(MPR121_CONFIG1, 0x10);   /* 16 uA charge current                       */
    reg_write(MPR121_CONFIG2, 0x20);   /* 0.5 us encoding, 1 ms sample interval      */
    return reg_write(MPR121_ECR, (uint8_t)(0x80 | (pads > 12 ? 12 : pads)));  /* run */
}

static esp_err_t read_values(uint8_t pads, uint16_t *values)
{
    uint8_t raw[TOUCH_MAX_PADS * 2];
    const uint8_t reg = MPR121_FILTDATA_L;
    const esp_err_t err = i2c_master_transmit_receive(s_ctx.dev, &reg, 1, raw, (size_t)pads * 2, 100);
    if (err != ESP_OK) {
        return err;
    }
    for (uint8_t i = 0; i < pads; i++) {
        values[i] = (uint16_t)(raw[2 * i] | ((uint16_t)(raw[2 * i + 1] & 0x03) << 8));
    }
    return ESP_OK;
}

esp_err_t touch_sensor_init_feed(const touch_sensor_config_t *cfg)
{
    if (!cfg || s_ctx.task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.cfg = *cfg;
    touch_core_init(&s_ctx.core, &s_ctx.cfg.core);
    s_ctx.configured = true;
    return ESP_OK;
}

esp_err_t touch_sensor_feed(const uint16_t *values, uint8_t pads, uint32_t t_ms, touch_result_t *out)
{
    if (!s_ctx.configured || !values) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pads != s_ctx.cfg.core.pads) {
        return ESP_ERR_INVALID_SIZE;
    }
    touch_result_t r;
    touch_core_process(&s_ctx.core, values, t_ms, &r);
    memcpy(s_ctx.values, values, (size_t)pads * sizeof(uint16_t));
    if (s_ctx.cfg.callback &&
        (r.event != TOUCH_EVT_NONE || (s_ctx.cfg.notify_while_active && r.active))) {
        s_ctx.cfg.callback(&r, s_ctx.cfg.callback_ctx);
    }
    if (out) {
        *out = r;
    }
    return ESP_OK;
}

static void touch_task(void *arg)
{
    (void)arg;
    const uint8_t pads = s_ctx.cfg.core.pads;
    const TickType_t period = pdMS_TO_TICKS(1000 / (s_ctx.cfg.sample_hz ? s_ctx.cfg.sample_hz : 50));
    TickType_t last_wake = xTaskGetTickCount();
    uint16_t values[TOUCH_MAX_PADS];
    int fails = 0;

    while (s_ctx.run) {
        vTaskDelayUntil(&last_wake, period > 0 ? period : 1);
        if (read_values(pads, values) != ESP_OK) {
            if (++fails % 50 == 1) {
                ESP_LOGW(TAG, "MPR121 read failed - check wiring and pull-ups");
            }
            continue;
        }
        fails = 0;
        touch_sensor_feed(values, pads, (uint32_t)(esp_timer_get_time() / 1000), NULL);
    }
    s_ctx.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t touch_sensor_start(const touch_sensor_config_t *cfg)
{
    if (!cfg || s_ctx.task) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = touch_sensor_init_feed(cfg);
    if (err != ESP_OK) {
        return err;
    }
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = cfg->i2c_port,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = cfg->internal_pullups,
    };
    err = i2c_new_master_bus(&bus_cfg, &s_ctx.bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->address,
        .scl_speed_hz = cfg->scl_hz,
    };
    err = i2c_master_bus_add_device(s_ctx.bus, &dev_cfg, &s_ctx.dev);
    if (err != ESP_OK) {
        i2c_del_master_bus(s_ctx.bus);
        s_ctx.bus = NULL;
        return err;
    }
    err = mpr121_init(cfg->core.pads);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(s_ctx.dev);
        i2c_del_master_bus(s_ctx.bus);
        s_ctx.dev = NULL; s_ctx.bus = NULL;
        return err;
    }

    s_ctx.run = true;
    BaseType_t ok;
    if (cfg->task_core < 0) {
        ok = xTaskCreate(touch_task, "touch", cfg->task_stack, NULL, cfg->task_priority, &s_ctx.task);
    } else {
        ok = xTaskCreatePinnedToCore(touch_task, "touch", cfg->task_stack, NULL, cfg->task_priority,
                                     &s_ctx.task, cfg->task_core);
    }
    if (ok != pdPASS) {
        s_ctx.run = false;
        s_ctx.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "MPR121 ready on SDA %d / SCL %d, %u pads at %u Hz",
             cfg->sda_gpio, cfg->scl_gpio, cfg->core.pads, cfg->sample_hz);
    return ESP_OK;
}

esp_err_t touch_sensor_stop(void)
{
    if (!s_ctx.task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.run = false;
    for (int i = 0; i < 100 && s_ctx.task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_ctx.dev) {
        reg_write(MPR121_ECR, 0x00);
        i2c_master_bus_rm_device(s_ctx.dev);
        s_ctx.dev = NULL;
    }
    if (s_ctx.bus) {
        i2c_del_master_bus(s_ctx.bus);
        s_ctx.bus = NULL;
    }
    return ESP_OK;
}

bool touch_sensor_is_running(void)
{
    return s_ctx.task != NULL;
}

void touch_sensor_recalibrate(void)
{
    touch_core_recalibrate(&s_ctx.core);
    ESP_LOGI(TAG, "touch baseline will be taken from the next reading");
}

uint8_t touch_sensor_deltas(uint16_t *out, uint8_t max_pads)
{
    const uint8_t n = s_ctx.cfg.core.pads < max_pads ? s_ctx.cfg.core.pads : max_pads;
    for (uint8_t i = 0; i < n; i++) {
        out[i] = s_ctx.core.delta[i];
    }
    return n;
}
