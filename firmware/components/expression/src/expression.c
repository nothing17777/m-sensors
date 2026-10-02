#include "expression.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TWO_PI ((float)(2.0 * M_PI))

/* ======================= response map ======================= */

#define NIGHT_BRIGHTNESS_CAP 40
#define TOUCH_FIRM_THRESHOLD 0.45f
#define NIGHT_SPEED_SCALE    0.6f

/* DRAFT Emotion Mapping Table (Phase 1 deliverable) - confirm with IPMD.
 * Angry/fearful get soothing responses instead of being mirrored. */
static const expression_t EMOTION_TABLE[EMOTION_COUNT] = {
    /*                       pattern               R    G    B    W   bri  speed dir  music        */
    [EMOTION_NEUTRAL]   = {LED_PATTERN_BREATHE,     0,   0,   0, 160,  90, 0.25f, 0, "ambient_01", HAPTIC_NONE},
    [EMOTION_HAPPY]     = {LED_PATTERN_SPARKLE,   255, 150,   0,  40, 170, 0.80f, 0, "bright_01", HAPTIC_NONE},
    [EMOTION_SAD]       = {LED_PATTERN_BREATHE,    40,  80, 255,  20,  70, 0.15f, 0, "comfort_01", HAPTIC_NONE},
    [EMOTION_ANGRY]     = {LED_PATTERN_WAVE,      140,  60, 255,   0,  80, 0.20f, 0, "calm_01", HAPTIC_NONE},
    [EMOTION_SURPRISED] = {LED_PATTERN_PULSE,     120, 200, 255,  60, 170, 1.20f, 0, "chime_01", HAPTIC_NONE},
    [EMOTION_FEARFUL]   = {LED_PATTERN_BREATHE,   255, 110,  30,  60,  70, 0.15f, 0, "comfort_01", HAPTIC_NONE},
    [EMOTION_DISGUSTED] = {LED_PATTERN_WAVE,       60, 200, 120,   0,  90, 0.30f, 0, "calm_01", HAPTIC_NONE},
};

/* Idle: a soft blink every four seconds, so the sphere reads as awake but quiet. */
static const expression_t AMBIENT = {LED_PATTERN_BLINK, 0, 0, 0, 170, 60, 0.25f, 0, NULL, HAPTIC_NONE};

/* Stage 1 - calm: a gentle, cool "I noticed you" that does not startle anyone. */
static const expression_t AUTO_CALM[3] = {
    [MOTION_AROUSAL_CALM]    = {LED_PATTERN_GLOW_TOWARD, 120, 180, 255, 70,  80, 0.30f, 0, NULL, HAPTIC_NONE},
    [MOTION_AROUSAL_LIVELY]  = {LED_PATTERN_GLOW_TOWARD, 110, 190, 255, 50, 100, 0.45f, 0, NULL, HAPTIC_NONE},
    [MOTION_AROUSAL_INTENSE] = {LED_PATTERN_RIPPLE,      120, 170, 255, 40, 120, 0.70f, 0, NULL, HAPTIC_NONE},
};

/* Stage 2 - warm: once the person stays, the light warms up and opens out. */
static const expression_t AUTO_WARM[3] = {
    [MOTION_AROUSAL_CALM]    = {LED_PATTERN_GLOW_TOWARD, 255, 140,  40, 90, 120, 0.30f, 0, "warm_01", HAPTIC_NONE},
    [MOTION_AROUSAL_LIVELY]  = {LED_PATTERN_GLOW_TOWARD, 255, 120,  20, 60, 150, 0.60f, 0, "warm_01", HAPTIC_NONE},
    [MOTION_AROUSAL_INTENSE] = {LED_PATTERN_RIPPLE,      255,  95,  10, 30, 180, 1.00f, 0, "warm_01", HAPTIC_NONE},
};

static const char *const EMOTION_NAMES[EMOTION_COUNT] = {
    "neutral", "happy", "sad", "angry", "surprised", "fearful", "disgusted",
};

static const struct { const char *alias; emotion_t e; } SYNONYMS[] = {
    {"happiness", EMOTION_HAPPY}, {"joy", EMOTION_HAPPY},       {"smile", EMOTION_HAPPY},
    {"sadness", EMOTION_SAD},     {"anger", EMOTION_ANGRY},     {"surprise", EMOTION_SURPRISED},
    {"fear", EMOTION_FEARFUL},    {"scared", EMOTION_FEARFUL},  {"disgust", EMOTION_DISGUSTED},
    {"contempt", EMOTION_DISGUSTED}, {"calm", EMOTION_NEUTRAL}, {"none", EMOTION_NEUTRAL},
};

emotion_t emotion_from_label(const char *label)
{
    char buf[24];
    size_t n = 0;
    if (!label) {
        return EMOTION_NEUTRAL;
    }
    while (*label && isspace((unsigned char)*label)) {
        label++;
    }
    while (label[n] && n < sizeof(buf) - 1) {
        buf[n] = (char)tolower((unsigned char)label[n]);
        n++;
    }
    while (n > 0 && isspace((unsigned char)buf[n - 1])) {
        n--;
    }
    buf[n] = '\0';
    for (int i = 0; i < EMOTION_COUNT; i++) {
        if (strcmp(buf, EMOTION_NAMES[i]) == 0) {
            return (emotion_t)i;
        }
    }
    for (size_t i = 0; i < sizeof(SYNONYMS) / sizeof(SYNONYMS[0]); i++) {
        if (strcmp(buf, SYNONYMS[i].alias) == 0) {
            return SYNONYMS[i].e;
        }
    }
    return EMOTION_NEUTRAL;
}

const char *emotion_name(emotion_t e)
{
    return (unsigned)e < EMOTION_COUNT ? EMOTION_NAMES[e] : "?";
}

static expression_t apply_night(expression_t x, bool night)
{
    if (night) {
        if (x.brightness > NIGHT_BRIGHTNESS_CAP) {
            x.brightness = NIGHT_BRIGHTNESS_CAP;
        }
        x.speed *= NIGHT_SPEED_SCALE;
    }
    return x;
}

expression_t response_ambient(bool night)
{
    return apply_night(AMBIENT, night);
}

expression_t response_auto(auto_stage_t stage, motion_arousal_t arousal, float direction, bool night)
{
    const expression_t *table = stage == AUTO_STAGE_WARM ? AUTO_WARM : AUTO_CALM;
    expression_t x = table[(unsigned)arousal < 3 ? arousal : 0];
    x.direction = direction;
    return apply_night(x, night);
}

expression_t response_emotion(emotion_t e, float intensity, bool night)
{
    expression_t x = EMOTION_TABLE[(unsigned)e < EMOTION_COUNT ? e : EMOTION_NEUTRAL];
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    x.brightness = (uint8_t)((float)x.brightness * (0.6f + 0.4f * intensity));
    return apply_night(x, night);
}

/* Touch responses. The sketch's "turn soft / warm" - every touch answers warm.
 * haptic is a proposal: the BOM has no motor yet. */
expression_t response_touch(touch_gesture_t g, float strength, float angle_deg, bool has_angle, float travel,
                            bool sustained, bool night)
{
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;
    /* Gentle touch answers warm and slow; a firmer touch answers dynamically -
     * brighter, faster and more saturated. */
    const bool firm = strength > TOUCH_FIRM_THRESHOLD;
    expression_t x;
    switch (g) {
    case TOUCH_GESTURE_PAT:   /* rhythmic patting: a steady, soothing pulse in time with the hand */
        x = (expression_t){LED_PATTERN_PULSE, 255, 135, 35, 90, 140, 0.70f, 0, "warm_01", HAPTIC_SOFT_PULSE};
        break;
    case TOUCH_GESTURE_TAP:
    case TOUCH_GESTURE_DOUBLE_TAP:
        x = firm ? (expression_t){LED_PATTERN_RIPPLE, 255, 110, 10, 20, 190, 1.80f, 0, "touch_02", HAPTIC_SOFT_PULSE}
                 : (expression_t){LED_PATTERN_PULSE, 255, 150, 40, 100, 130, 1.20f, 0, "touch_01", HAPTIC_SOFT_PULSE};
        break;
    case TOUCH_GESTURE_HOLD:
        x = firm ? (expression_t){LED_PATTERN_PULSE, 255, 100, 10, 40, 170, 0.90f, 0, "warm_02", HAPTIC_PURR}
                 : (expression_t){LED_PATTERN_BREATHE, 255, 120, 30, 120, 110, 0.22f, 0, "warm_01", HAPTIC_PURR};
        break;
    case TOUCH_GESTURE_STROKE:
        x = firm ? (expression_t){LED_PATTERN_WAVE, 255, 100, 10, 30, 180, 1.10f, 0, "warm_02", HAPTIC_PURR}
                 : (expression_t){LED_PATTERN_WAVE, 255, 140, 40, 90, 130, 0.45f, 0, "warm_01", HAPTIC_PURR};
        if (travel < 0.0f) x.speed = -x.speed;   /* the wave follows the hand */
        break;
    case TOUCH_GESTURE_HUG:
        x = firm ? (expression_t){LED_PATTERN_BREATHE, 255, 110, 20, 60, 200, 0.35f, 0, "warm_02", HAPTIC_PURR}
                 : (expression_t){LED_PATTERN_BREATHE, 255, 130, 50, 160, 160, 0.12f, 0, "warm_01", HAPTIC_PURR};
        break;
    default:
        x = firm ? (expression_t){LED_PATTERN_GLOW_TOWARD, 255, 110, 20, 40, 170, 0.90f, 0, "touch_02", HAPTIC_SOFT_PULSE}
                 : (expression_t){LED_PATTERN_GLOW_TOWARD, 255, 140, 50, 100, 120, 0.50f, 0, "touch_01", HAPTIC_SOFT_PULSE};
        break;
    }
    x.brightness = (uint8_t)((float)x.brightness * (0.85f + 0.25f * strength));
    if (sustained) {           /* settled in: ease down into something quieter */
        x.brightness = (uint8_t)((float)x.brightness * 0.8f);
        x.speed *= 0.6f;
    }
    if (has_angle) {
        float a = angle_deg;
        while (a > 180.0f) a -= 360.0f;
        while (a < -180.0f) a += 360.0f;
        x.direction = a / 90.0f;    /* quarter-turns, so the glow meets the hand */
    }
    return apply_night(x, night);
}

/* ======================= behaviour ======================= */

void behavior_init(behavior_t *b, const behavior_config_t *cfg)
{
    memset(b, 0, sizeof(*b));
    b->cfg = *cfg;
    b->mode = MODE_AMBIENT;
    b->last_motion_end = -1e9f;
    b->last_face_seen = -1e9f;
    b->motion.centroid_x = b->motion.centroid_y = 0.5f;
    b->touch_until = -1e9f;
    b->auto_since = -1e9f;
    b->auto_stage = AUTO_STAGE_CALM;
}

void behavior_on_motion(behavior_t *b, const motion_result_t *m, float now_s)
{
    b->motion = *m;
    if (m->level != MOTION_LEVEL_NONE) {
        float d = m->centroid_x * 2.0f - 1.0f;
        b->direction = b->cfg.flip_direction ? -d : d;
    }
    if (m->event == MOTION_EVT_START) {
        b->scan_due = true;
        if (b->mode == MODE_AMBIENT) {
            b->mode = MODE_AUTO_RESPONSE;
            /* Someone who just stepped out and came back is already known, so skip
             * the calm greeting and pick up where it left off. */
            const bool returning = now_s - b->last_motion_end < b->cfg.regreet_cooldown_s;
            b->auto_since = returning ? now_s - b->cfg.calm_to_warm_s : now_s;
            b->auto_stage = returning ? AUTO_STAGE_WARM : AUTO_STAGE_CALM;
        }
    } else if (m->event == MOTION_EVT_END) {
        b->last_motion_end = now_s;
        if (b->mode == MODE_AUTO_RESPONSE) {
            b->mode = MODE_AMBIENT;
        }
    }
}

bool behavior_on_emotion(behavior_t *b, emotion_t e, float confidence, float intensity, float now_s)
{
    if (confidence < b->cfg.min_confidence) {
        return false;
    }
    b->emotion = e;
    b->intensity = intensity;
    b->emotion_until = now_s + b->cfg.emotion_hold_s;
    if (b->mode != MODE_TOUCH) {     /* being touched wins; the emotion resumes afterwards */
        b->mode = MODE_EMOTION;
    }
    return true;
}

/* Touch has priority over everything else: it is the most direct contact. */
void behavior_on_touch(behavior_t *b, const touch_result_t *t, float now_s)
{
    b->touch = *t;
    if (t->active) {
        b->mode = MODE_TOUCH;
        b->touch_until = now_s + b->cfg.touch_afterglow_s;
    } else if (t->event == TOUCH_EVT_END) {
        b->touch_until = now_s + b->cfg.touch_afterglow_s;
        if (t->gesture == TOUCH_GESTURE_DOUBLE_TAP) {
            b->night = !b->night;    /* proposal: double tap switches night mode */
        }
    }
}

void behavior_on_face(behavior_t *b, float now_s)
{
    b->last_face_seen = now_s;
}

void behavior_tick(behavior_t *b, float now_s)
{
    b->auto_stage = (now_s - b->auto_since >= b->cfg.calm_to_warm_s) ? AUTO_STAGE_WARM : AUTO_STAGE_CALM;
    if (b->mode == MODE_TOUCH) {
        if (!b->touch.active && now_s >= b->touch_until) {
            if (b->emotion_until > now_s) b->mode = MODE_EMOTION;
            else if (b->motion.active) b->mode = MODE_AUTO_RESPONSE;
            else b->mode = MODE_AMBIENT;
        }
        return;
    }
    if (b->mode == MODE_EMOTION && now_s >= b->emotion_until) {
        b->mode = b->motion.active ? MODE_AUTO_RESPONSE : MODE_AMBIENT;
    } else if (b->mode == MODE_AMBIENT && b->motion.active) {
        b->mode = MODE_AUTO_RESPONSE;
    }
}

bool behavior_wants_faces(const behavior_t *b, float now_s)
{
    return b->motion.active || b->mode == MODE_EMOTION || b->touch.active ||
           now_s < b->touch_until ||          /* a hand on the shell means someone is here */
           now_s - b->last_motion_end < b->cfg.presence_grace_s ||
           now_s - b->last_face_seen < b->cfg.presence_grace_s;
}

bool behavior_take_scan_request(behavior_t *b)
{
    bool due = b->scan_due;
    b->scan_due = false;
    return due;
}

expression_t behavior_expression(const behavior_t *b)
{
    switch (b->mode) {
    case MODE_TOUCH:
        return response_touch(b->touch.gesture, b->touch.strength, b->touch.angle, b->touch.has_angle,
                              b->touch.travel, b->touch.sustained, b->night);
    case MODE_EMOTION:
        return response_emotion(b->emotion, b->intensity, b->night);
    case MODE_AUTO_RESPONSE:
        return response_auto(b->auto_stage, b->motion.arousal, b->direction, b->night);
    default:
        return response_ambient(b->night);
    }
}

const char *behavior_mode_name(behavior_mode_t m)
{
    static const char *const n[] = {"ambient", "auto_response", "emotion", "touch"};
    return (unsigned)m < 4 ? n[m] : "?";
}

/* ======================= LED renderer ======================= */

void led_ring_init(led_ring_t *ring, uint16_t n, uint16_t front_index, float fade_s)
{
    memset(ring, 0, sizeof(*ring));
    ring->n = n;
    ring->front_index = front_index;
    ring->fade_s = fade_s;
}

static bool same_look(const expression_t *a, const expression_t *b)
{
    return a->pattern == b->pattern && a->r == b->r && a->g == b->g && a->b == b->b && a->w == b->w &&
           a->brightness == b->brightness && a->speed == b->speed;
}

static float pattern_level(const expression_t *x, uint16_t i, uint16_t n, float t, float center)
{
    const float ph = t * x->speed;
    float dist = fmodf((float)i - center + (float)n / 2.0f, (float)n);
    if (dist < 0) dist += (float)n;
    dist = fabsf(dist - (float)n / 2.0f);

    switch (x->pattern) {
    case LED_PATTERN_BLINK: {
        const float frac = ph - floorf(ph);
        const float a = frac / 0.06f, bq = (1.0f - frac) / 0.06f;
        const float g1 = expf(-0.5f * a * a), g2 = expf(-0.5f * bq * bq);
        return 0.14f + 0.86f * (g1 > g2 ? g1 : g2);
    }
    case LED_PATTERN_BREATHE:
        return 0.15f + 0.85f * (0.5f - 0.5f * cosf(TWO_PI * ph));
    case LED_PATTERN_GLOW_TOWARD: {
        const float sigma = (float)n / 8.0f;
        return 0.12f + 0.88f * expf(-(dist * dist) / (2.0f * sigma * sigma)) * (0.8f + 0.2f * sinf(TWO_PI * ph));
    }
    case LED_PATTERN_RIPPLE:
        return 0.25f + 0.75f * (0.5f + 0.5f * cosf(TWO_PI * (dist / ((float)n / 2.0f) * 1.5f - ph)));
    case LED_PATTERN_SPARKLE: {
        const uint32_t step = (uint32_t)(t * (x->speed > 0.01f ? x->speed : 0.01f) * 6.0f);
        const uint32_t h = (((uint32_t)i * 2654435761u) ^ (step * 40503u)) % 1000u;
        const float r = (float)h / 999.0f;
        return 0.4f + 0.6f * r * r * r;
    }
    case LED_PATTERN_WAVE:
        return 0.3f + 0.7f * (0.5f + 0.5f * sinf(TWO_PI * ((float)i / (float)n - ph)));
    case LED_PATTERN_PULSE: {
        const float frac = ph - floorf(ph);
        return 0.25f + 0.75f * expf(-5.0f * frac);
    }
    default:
        return 1.0f;
    }
}

static void render_one(const expression_t *x, uint16_t i, uint16_t n, float t, float center, float out[4])
{
    const float k = pattern_level(x, i, n, t, center) * ((float)x->brightness / 255.0f);
    out[0] = x->r * k;
    out[1] = x->g * k;
    out[2] = x->b * k;
    out[3] = x->w * k;
}

void led_ring_render(led_ring_t *ring, const expression_t *expr, float now_s, uint8_t *rgbw_out)
{
    if (!ring->has_cur) {
        ring->cur = ring->prev = *expr;
        ring->switch_t = now_s - ring->fade_s;
        ring->last_t = now_s;
        ring->dir = expr->direction;
        ring->has_cur = true;
    } else if (!same_look(expr, &ring->cur)) {
        ring->prev = ring->cur;
        ring->switch_t = now_s;
    }
    ring->cur = *expr;

    float dt = now_s - ring->last_t;
    if (dt < 0) dt = 0;
    ring->last_t = now_s;
    float a = dt * 4.0f;
    ring->dir += (expr->direction - ring->dir) * (a > 1.0f ? 1.0f : a);

    float k = ring->fade_s > 0 ? (now_s - ring->switch_t) / ring->fade_s : 1.0f;
    if (k > 1.0f) k = 1.0f;
    if (k < 0.0f) k = 0.0f;
    const float center = (float)ring->front_index + ring->dir * ((float)ring->n / 4.0f);

    for (uint16_t i = 0; i < ring->n; i++) {
        float nw[4], od[4];
        render_one(&ring->cur, i, ring->n, now_s, center, nw);
        if (k < 1.0f) {
            render_one(&ring->prev, i, ring->n, now_s, center, od);
        }
        for (int c = 0; c < 4; c++) {
            float v = k < 1.0f ? od[c] * (1.0f - k) + nw[c] * k : nw[c];
            rgbw_out[(uint32_t)i * 4 + c] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f));
        }
    }
}
