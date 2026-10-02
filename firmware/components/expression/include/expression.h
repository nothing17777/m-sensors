/*
 * M expression layer: motion/emotion -> behaviour -> light.
 * Pure C99 (host-testable). Mirrors pc_prototype/response_map.py, behavior.py, led_sim.py.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "motion_core.h"
#include "touch_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- response map ---------- */

typedef enum {
    LED_PATTERN_SOLID = 0,
    LED_PATTERN_BLINK,        /* idle: one soft blink every few seconds */
    LED_PATTERN_BREATHE,
    LED_PATTERN_GLOW_TOWARD,  /* bright spot on the side the motion is */
    LED_PATTERN_RIPPLE,
    LED_PATTERN_SPARKLE,
    LED_PATTERN_WAVE,
    LED_PATTERN_PULSE,
} led_pattern_t;

typedef enum {
    EMOTION_NEUTRAL = 0,
    EMOTION_HAPPY,
    EMOTION_SAD,
    EMOTION_ANGRY,
    EMOTION_SURPRISED,
    EMOTION_FEARFUL,
    EMOTION_DISGUSTED,
    EMOTION_COUNT
} emotion_t;

typedef enum {
    HAPTIC_NONE = 0,
    HAPTIC_SOFT_PULSE,   /* one gentle bump, for a tap                         */
    HAPTIC_PURR,         /* slow low buzz while being held or hugged           */
} haptic_t;

typedef struct {
    led_pattern_t pattern;
    uint8_t r, g, b, w;      /* SK6812 RGBW */
    uint8_t brightness;      /* 0..255 */
    float speed;             /* cycles per second */
    float direction;         /* quarter-turns around the ring: -1 left, +1 right, +-2 = behind */
    const char *music;       /* track id for the music subsystem, may be NULL */
    haptic_t haptic;         /* optional actuator; ignored if no motor is fitted */
} expression_t;

emotion_t    emotion_from_label(const char *label);   /* case-insensitive, synonyms, unknown -> NEUTRAL */
const char  *emotion_name(emotion_t e);
/* Activation runs calm first, then warms up while the person stays. */
typedef enum { AUTO_STAGE_CALM = 0, AUTO_STAGE_WARM } auto_stage_t;

expression_t response_ambient(bool night);
expression_t response_auto(auto_stage_t stage, motion_arousal_t arousal, float direction, bool night);
expression_t response_emotion(emotion_t e, float intensity, bool night);
expression_t response_touch(touch_gesture_t g, float strength, float angle_deg, bool has_angle, float travel,
                            bool sustained, bool night);

/* ---------- behaviour state machine ---------- */

typedef enum { MODE_AMBIENT = 0, MODE_AUTO_RESPONSE, MODE_EMOTION, MODE_TOUCH } behavior_mode_t;

typedef struct {
    float emotion_hold_s;
    float min_confidence;
    float presence_grace_s;
    float calm_to_warm_s;      /* how long the calm stage lasts before warming up */
    float regreet_cooldown_s;  /* someone returning this soon skips the calm stage */
    float touch_afterglow_s;   /* warm glow lingers this long after the hand leaves */
    bool  flip_direction;
} behavior_config_t;

#define BEHAVIOR_CONFIG_DEFAULT() { .emotion_hold_s = 8.0f, .min_confidence = 0.4f, \
                                    .presence_grace_s = 3.0f, .calm_to_warm_s = 2.5f, .regreet_cooldown_s = 20.0f, \
                                    .touch_afterglow_s = 4.0f, .flip_direction = false }

typedef struct {
    behavior_config_t cfg;
    behavior_mode_t mode;
    bool night;
    motion_result_t motion;
    touch_result_t touch;
    float touch_until;
    float auto_since;
    auto_stage_t auto_stage;
    float direction;
    emotion_t emotion;
    float intensity;
    float emotion_until;
    float last_motion_end;
    float last_face_seen;
    bool scan_due;
} behavior_t;

void behavior_init(behavior_t *b, const behavior_config_t *cfg);
void behavior_on_motion(behavior_t *b, const motion_result_t *m, float now_s);
void behavior_on_touch(behavior_t *b, const touch_result_t *t, float now_s);
bool behavior_on_emotion(behavior_t *b, emotion_t e, float confidence, float intensity, float now_s);
void behavior_on_face(behavior_t *b, float now_s);
void behavior_tick(behavior_t *b, float now_s);
bool behavior_wants_faces(const behavior_t *b, float now_s);
bool behavior_take_scan_request(behavior_t *b);   /* true once per motion START */
expression_t behavior_expression(const behavior_t *b);
const char *behavior_mode_name(behavior_mode_t m);

/* ---------- LED ring renderer ---------- */

typedef struct {
    uint16_t n;
    uint16_t front_index;    /* LED closest to the camera */
    float fade_s;
    expression_t cur, prev;
    bool has_cur;
    float switch_t, last_t, dir;
} led_ring_t;

void led_ring_init(led_ring_t *ring, uint16_t n, uint16_t front_index, float fade_s);
/* Writes n * 4 bytes (R,G,B,W per LED). */
void led_ring_render(led_ring_t *ring, const expression_t *expr, float now_s, uint8_t *rgbw_out);

#ifdef __cplusplus
}
#endif
