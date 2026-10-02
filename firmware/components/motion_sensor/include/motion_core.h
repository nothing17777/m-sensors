/*
 * M camera motion sensor - platform-independent core.
 *
 * Pure C99, no ESP-IDF dependency, so it is unit-tested on a PC
 * (firmware/host_test) and verified bit-exact against the Python prototype
 * (tools/parity_check.py).
 *
 * Keep MOTION_CONFIG_DEFAULT() in sync with pc_prototype/config.py MotionConfig.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOTION_EVT_NONE = 0,
    MOTION_EVT_START,     /* debounced: someone started moving in view      */
    MOTION_EVT_UPDATE,    /* session active and this frame has motion        */
    MOTION_EVT_END,       /* no motion for quiet_ms                          */
    MOTION_EVT_LIGHTING,  /* whole-scene change (lights on/off) - ignored    */
} motion_event_t;

typedef enum {
    MOTION_LEVEL_NONE = 0,
    MOTION_LEVEL_PRESENCE, /* small area changed */
    MOTION_LEVEL_ACTIVE,   /* large area changed */
} motion_level_t;

/* Is the person coming closer or moving away? */
typedef enum {
    MOTION_TREND_STEADY = 0,
    MOTION_TREND_APPROACHING,
    MOTION_TREND_RETREATING,
} motion_trend_t;

/* Motion energy bucket. NOT an emotion - EchoAi provides emotion from faces. */
typedef enum {
    MOTION_AROUSAL_CALM = 0,
    MOTION_AROUSAL_LIVELY,
    MOTION_AROUSAL_INTENSE,
} motion_arousal_t;

typedef struct {
    uint8_t  block;             /* downscale block size in px                     */
    uint8_t  pixel_threshold;   /* luma diff that counts as changed               */
    uint8_t  bg_shift;          /* bg learning rate 1/2^n for non-moving cells    */
    uint8_t  fg_shift;          /* bg learning rate 1/2^n for moving cells        */
    uint8_t  stable_threshold;  /* |cell - prev frame| <= this = not moving       */
    uint8_t  min_neighbors;     /* noise filter: changed neighbours required      */
    uint8_t  frames_to_trigger; /* consecutive moving frames before START         */
    uint8_t  warmup_frames;     /* auto-exposure settle frames                    */
    uint8_t  noise_gain_q4;     /* threshold floor = noise x this/16 (dim rooms)  */
    uint8_t  settle_frames;     /* frames ignored after the whole scene changes   */
    float    trend_fast_alpha;  /* area EMAs behind approach/retreat              */
    float    trend_slow_alpha;
    float    trend_deadband;    /* area difference that counts as moving closer   */
    uint32_t quiet_ms;          /* no motion this long -> END                     */
    float    min_area_ratio;    /* fraction of cells = motion                     */
    float    active_area_ratio; /* fraction of cells = ACTIVE                     */
    float    lighting_ratio;    /* fraction of raw-changed cells = lighting event */
    float    area_full_ratio;   /* area mapping to energy 1.0                     */
    float    speed_full;        /* frame-widths/s mapping to energy 1.0           */
    float    speed_alpha;       /* speed EMA                                      */
    float    energy_alpha;      /* energy EMA                                     */
} motion_config_t;

#define MOTION_CONFIG_DEFAULT() {   \
    .block = 2,                     \
    .pixel_threshold = 22,          \
    .bg_shift = 4,                  \
    .fg_shift = 7,                  \
    .stable_threshold = 10,         \
    .min_neighbors = 2,             \
    .frames_to_trigger = 2,         \
    .warmup_frames = 10,            \
    .noise_gain_q4 = 64,            \
    .settle_frames = 8,             \
    .trend_fast_alpha = 0.35f,      \
    .trend_slow_alpha = 0.07f,      \
    .trend_deadband = 0.004f,       \
    .quiet_ms = 1500,               \
    .min_area_ratio = 0.01f,        \
    .active_area_ratio = 0.08f,     \
    .lighting_ratio = 0.65f,        \
    .area_full_ratio = 0.25f,       \
    .speed_full = 1.5f,             \
    .speed_alpha = 0.3f,            \
    .energy_alpha = 0.2f,           \
}

typedef struct {
    motion_event_t   event;
    motion_level_t   level;
    motion_arousal_t arousal;
    motion_trend_t   trend;
    bool     settling;        /* scene was upheaved; ignoring frames until settled */
    uint16_t threshold;       /* pixel threshold actually used this frame          */
    bool     active;          /* inside a motion session                        */
    float    area_ratio;      /* changed cells / total cells                    */
    float    centroid_x;      /* 0 = image left .. 1 = right (valid if level)   */
    float    centroid_y;      /* 0 = top .. 1 = bottom                          */
    uint16_t bbox_x0, bbox_y0, bbox_x1, bbox_y1; /* cell coords, inclusive      */
    float    speed;           /* smoothed frame-widths per second               */
    float    energy;          /* smoothed 0..1                                  */
    uint32_t duration_ms;     /* session duration (START..now / END)            */
    uint32_t changed_cells;
} motion_result_t;

typedef struct {
    motion_config_t cfg;
    uint16_t in_w, in_h, cw, ch;
    uint32_t cells, min_cells, active_cells, lighting_cells;
    uint16_t *bg;    /* Q8 background   */
    uint8_t  *cur;   /* downscaled frame */
    uint8_t  *prev;  /* previous downscaled frame */
    uint8_t  *raw;   /* raw change mask  */
    uint8_t  *mask;  /* filtered mask    */
    bool     initialized;
    uint16_t frame_count;
    uint8_t  consec;
    bool     active;
    uint32_t start_ms, last_motion_ms, prev_t;
    bool     has_prev_t, prev_valid;
    float    prev_cx, prev_cy, speed, energy;
    float    area_fast, area_slow;
    uint32_t noise_q8;
    uint8_t  settle;
    motion_arousal_t arousal;
    motion_trend_t trend;
} motion_core_t;

/* Allocates buffers for a width x height 8-bit grayscale input. 0 on success. */
int  motion_core_init(motion_core_t *m, const motion_config_t *cfg, uint16_t width, uint16_t height);
void motion_core_deinit(motion_core_t *m);
void motion_core_reset(motion_core_t *m);
/* Call when the device or camera is moved: the scene is re-learned from scratch. */
void motion_core_recalibrate(motion_core_t *m);
void motion_core_process(motion_core_t *m, const uint8_t *gray, uint32_t t_ms, motion_result_t *out);
/* Filtered change mask (cw x ch, 1 = motion) from the last processed frame. */
const uint8_t *motion_core_mask(const motion_core_t *m);

const char *motion_event_name(motion_event_t e);
const char *motion_level_name(motion_level_t l);
const char *motion_arousal_name(motion_arousal_t a);
const char *motion_trend_name(motion_trend_t t);

#ifdef __cplusplus
}
#endif
