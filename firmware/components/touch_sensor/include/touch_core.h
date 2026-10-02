/*
 * M capacitive touch sensor - platform-independent core.
 *
 * Turns raw per-pad capacitance readings (MPR121 filtered data, or any other
 * source) into touch sessions and gestures: tap, double tap, hold, stroke and hug.
 *
 * Pure C99, no ESP-IDF dependency, so it is unit-tested on a PC
 * (firmware/host_test) and verified against the Python prototype
 * (tools/parity_touch.py).
 *
 * Keep TOUCH_CONFIG_DEFAULT() in sync with pc_prototype/config.py TouchConfig.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_MAX_PADS 12
#define TOUCH_NO_ANGLE (-1.0f)   /* pad with no position on the ring (crown pad) */

typedef enum {
    TOUCH_EVT_NONE = 0,
    TOUCH_EVT_START,        /* first pad pressed                                  */
    TOUCH_EVT_UPDATE,       /* still being touched                                */
    TOUCH_EVT_END,          /* last pad released; result carries the gesture      */
    TOUCH_EVT_RECALIBRATED, /* a pad was stuck (wet shell, leaning object) - reset */
} touch_event_t;

/* Where on the body a pad sits, so the rest of the system can say "patted on the
 * head" without knowing pad numbers. */
typedef enum {
    TOUCH_ZONE_NONE = 0,
    TOUCH_ZONE_FRONT,
    TOUCH_ZONE_RIGHT,
    TOUCH_ZONE_BACK,
    TOUCH_ZONE_LEFT,
    TOUCH_ZONE_CROWN,
    TOUCH_ZONE_BASE,
} touch_zone_t;

typedef enum {
    TOUCH_GESTURE_NONE = 0,
    TOUCH_GESTURE_TAP,
    TOUCH_GESTURE_DOUBLE_TAP,
    TOUCH_GESTURE_HOLD,     /* hand resting on the shell   */
    TOUCH_GESTURE_STROKE,   /* hand sliding around the ring */
    TOUCH_GESTURE_HUG,      /* several pads held at once    */
    TOUCH_GESTURE_PAT,      /* rhythmic repeated taps       */
} touch_gesture_t;

typedef struct {
    uint8_t  pads;                        /* number of electrodes in use          */
    float    pad_angle[TOUCH_MAX_PADS];   /* degrees around the sphere, or TOUCH_NO_ANGLE */
    uint8_t  pad_zone[TOUCH_MAX_PADS];    /* touch_zone_t per pad                 */
    uint16_t touch_threshold;             /* counts below baseline to press       */
    uint16_t release_threshold;           /* counts to release (hysteresis)       */
    uint8_t  press_frames;                /* debounce frames to press             */
    uint8_t  release_frames;              /* debounce frames to release           */
    uint8_t  baseline_shift;              /* baseline EMA 1/2^n while untouched   */
    uint32_t stuck_ms;                    /* held this long -> recalibrate that pad */
    uint32_t tap_max_ms;                  /* shorter than this is a tap           */
    uint32_t double_tap_gap_ms;           /* second tap within this = double tap  */
    uint32_t hold_min_ms;                 /* longer than this is a hold           */
    float    stroke_min_deg;              /* travel around the ring to be a stroke */
    uint8_t  pat_taps;                    /* taps in a row that mean patting      */
    uint32_t pat_window_ms;               /* ...within this much time             */
    uint32_t sustained_ms;                /* held this long = settled, calmer still */
    uint32_t all_pads_ms;                 /* every pad at once this long = handling */
    uint8_t  hug_pads;                    /* pads at once to count as a hug       */
    float    hug_span_deg;                /* ...and spread this far around the shell */
    uint32_t hug_min_ms;
    uint16_t strength_full;               /* delta counts mapping to strength 1.0 */
} touch_config_t;

/* 8 pads around the equator + 1 crown pad on top (see main/board_pins.h). */
#define TOUCH_CONFIG_DEFAULT() {                                                    \
    .pads = 9,                                                                      \
    .pad_angle = {0.0f, 45.0f, 90.0f, 135.0f, 180.0f, 225.0f, 270.0f, 315.0f,       \
                  TOUCH_NO_ANGLE, TOUCH_NO_ANGLE, TOUCH_NO_ANGLE, TOUCH_NO_ANGLE},  \
    .pad_zone = {TOUCH_ZONE_FRONT, TOUCH_ZONE_RIGHT, TOUCH_ZONE_RIGHT, TOUCH_ZONE_BACK,      \
                 TOUCH_ZONE_BACK, TOUCH_ZONE_BACK, TOUCH_ZONE_LEFT, TOUCH_ZONE_LEFT,         \
                 TOUCH_ZONE_CROWN, TOUCH_ZONE_NONE, TOUCH_ZONE_NONE, TOUCH_ZONE_NONE},       \
    .pat_taps = 3,                                                                  \
    .pat_window_ms = 2500,                                                          \
    .sustained_ms = 10000,                                                          \
    .all_pads_ms = 2000,                                                            \
    .touch_threshold = 12,                                                          \
    .release_threshold = 6,                                                         \
    .press_frames = 2,                                                              \
    .release_frames = 2,                                                            \
    .baseline_shift = 6,                                                            \
    .stuck_ms = 60000,                                                              \
    .tap_max_ms = 350,                                                              \
    .double_tap_gap_ms = 500,                                                       \
    .hold_min_ms = 700,                                                             \
    .stroke_min_deg = 60.0f,                                                        \
    .hug_pads = 3,                                                                  \
    .hug_span_deg = 100.0f,                                                         \
    .hug_min_ms = 500,                                                              \
    .strength_full = 120,                                                           \
}

typedef struct {
    touch_event_t   event;
    touch_gesture_t gesture;
    bool     active;
    uint16_t pad_mask;      /* bit per pressed pad                                */
    uint8_t  pad_count;
    bool     has_angle;
    float    angle;         /* degrees, where the hand is around the sphere       */
    float    travel;        /* signed degrees travelled this session (+ = clockwise) */
    float    span;          /* how far the pressed pads spread around the shell     */
    float    strength;      /* 0..1, how firmly it is being touched               */
    uint32_t duration_ms;
    touch_zone_t zone;      /* zone of the most firmly touched pad                */
    uint16_t zone_mask;     /* bit per touched zone                               */
    bool     sustained;     /* held long enough to count as settling in           */
    bool     handled;       /* every pad at once: the device is being carried     */
    bool     recalibrated;  /* a stuck pad re-baselined on this sample           */
    bool     crown;         /* a pad with no angle is touched ("head pat")        */
} touch_result_t;

typedef struct {
    touch_config_t cfg;
    int32_t  baseline[TOUCH_MAX_PADS];   /* Q8 */
    uint16_t delta[TOUCH_MAX_PADS];
    bool     pressed[TOUCH_MAX_PADS];
    uint8_t  press_cnt[TOUCH_MAX_PADS];
    uint8_t  release_cnt[TOUCH_MAX_PADS];
    uint32_t press_since[TOUCH_MAX_PADS];
    bool     initialized;
    bool     active;
    uint32_t start_ms;
    float    angle, prev_angle, travel;
    bool     has_angle, had_angle;
    bool     crown;
    uint32_t last_tap_end_ms;
    uint32_t tap_times[8];
    uint8_t  tap_count;
    uint32_t all_pads_since;
    bool     had_tap;
    touch_gesture_t gesture;
} touch_core_t;

void touch_core_init(touch_core_t *t, const touch_config_t *cfg);
void touch_core_reset(touch_core_t *t);       /* clears state, keeps calibration  */
void touch_core_recalibrate(touch_core_t *t); /* next reading becomes the baseline */
/* values: one filtered capacitance reading per pad (lower = touched, as on MPR121). */
void touch_core_process(touch_core_t *t, const uint16_t *values, uint32_t t_ms, touch_result_t *out);

const char *touch_event_name(touch_event_t e);
const char *touch_gesture_name(touch_gesture_t g);
const char *touch_zone_name(touch_zone_t z);

#ifdef __cplusplus
}
#endif
