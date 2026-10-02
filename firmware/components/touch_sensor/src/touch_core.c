/*
 * M capacitive touch sensor - core algorithm.
 * Mirrored by pc_prototype/touch_detector.py (see tools/parity_touch.py).
 *
 * Per pad:  delta = baseline - reading, debounced press/release with hysteresis,
 *           baseline tracks only while the pad is untouched, and a pad held
 *           longer than stuck_ms re-baselines itself (wet shell, leaning object).
 * Per session: pad mask, weighted angle around the sphere, travel, strength,
 *           and a gesture (tap / double tap / hold / stroke / hug).
 */
#include "touch_core.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* How far the pressed pads spread around the shell: 0 for one pad, ~180 for two
 * opposite sides. One broad hand covers neighbouring pads; a hug does not. */
static float pad_span(const float *angles, uint8_t n)
{
    if (n < 2) {
        return 0.0f;
    }
    float a[TOUCH_MAX_PADS];
    memcpy(a, angles, n * sizeof(float));
    for (uint8_t i = 1; i < n; i++) {           /* insertion sort, n <= 12 */
        const float key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
    float max_gap = a[0] + 360.0f - a[n - 1];
    for (uint8_t i = 1; i < n; i++) {
        const float gap = a[i] - a[i - 1];
        if (gap > max_gap) max_gap = gap;
    }
    return 360.0f - max_gap;
}

static float wrap180(float d)
{
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

void touch_core_init(touch_core_t *t, const touch_config_t *cfg)
{
    memset(t, 0, sizeof(*t));
    t->cfg = *cfg;
    if (t->cfg.pads > TOUCH_MAX_PADS) {
        t->cfg.pads = TOUCH_MAX_PADS;
    }
}

void touch_core_reset(touch_core_t *t)
{
    const bool init = t->initialized;
    int32_t saved[TOUCH_MAX_PADS];
    memcpy(saved, t->baseline, sizeof(saved));
    touch_config_t cfg = t->cfg;
    memset(t, 0, sizeof(*t));
    t->cfg = cfg;
    t->initialized = init;
    memcpy(t->baseline, saved, sizeof(saved));
}

void touch_core_recalibrate(touch_core_t *t)
{
    touch_core_reset(t);
    t->initialized = false;
}

void touch_core_process(touch_core_t *t, const uint16_t *values, uint32_t t_ms, touch_result_t *out)
{
    const touch_config_t *c = &t->cfg;
    const uint8_t n = c->pads;

    memset(out, 0, sizeof(*out));
    out->active = t->active;

    if (!t->initialized) {
        for (uint8_t i = 0; i < n; i++) {
            t->baseline[i] = (int32_t)values[i] << 8;
        }
        t->initialized = true;
        return;
    }

    bool recalibrated = false;
    uint16_t mask = 0, zone_mask = 0;
    uint8_t count = 0;
    uint16_t max_delta = 0;
    touch_zone_t zone = TOUCH_ZONE_NONE;
    float sx = 0.0f, sy = 0.0f, wsum = 0.0f;
    float pressed_angles[TOUCH_MAX_PADS];
    uint8_t angle_count = 0;
    bool crown = false;

    for (uint8_t i = 0; i < n; i++) {
        int32_t d = (t->baseline[i] >> 8) - (int32_t)values[i];
        if (d < 0) {
            d = 0;
        }
        t->delta[i] = (uint16_t)(d > 65535 ? 65535 : d);

        if (!t->pressed[i]) {
            if (t->delta[i] > c->touch_threshold) {
                if (++t->press_cnt[i] >= c->press_frames) {
                    t->pressed[i] = true;
                    t->press_since[i] = t_ms;
                    t->release_cnt[i] = 0;
                }
            } else {
                t->press_cnt[i] = 0;
            }
            /* baseline only tracks untouched pads */
            t->baseline[i] += (((int32_t)values[i] << 8) - t->baseline[i]) >> c->baseline_shift;
        } else {
            if (t->delta[i] < c->release_threshold) {
                if (++t->release_cnt[i] >= c->release_frames) {
                    t->pressed[i] = false;
                    t->press_cnt[i] = 0;
                }
            } else {
                t->release_cnt[i] = 0;
            }
            if (t->pressed[i] && t_ms - t->press_since[i] >= c->stuck_ms) {
                t->baseline[i] = (int32_t)values[i] << 8;   /* stuck pad: re-baseline */
                t->pressed[i] = false;
                t->press_cnt[i] = t->release_cnt[i] = 0;
                t->delta[i] = 0;
                recalibrated = true;
            }
        }

        if (t->pressed[i]) {
            mask |= (uint16_t)(1u << i);
            count++;
            if (t->delta[i] > max_delta) {
                max_delta = t->delta[i];
                zone = (touch_zone_t)c->pad_zone[i];
            }
            zone_mask |= (uint16_t)(1u << c->pad_zone[i]);
            if (c->pad_angle[i] == TOUCH_NO_ANGLE) {
                crown = true;
            } else {
                const float w = (float)t->delta[i];
                pressed_angles[angle_count++] = c->pad_angle[i];
                const float rad = c->pad_angle[i] * (float)M_PI / 180.0f;
                sx += w * cosf(rad);
                sy += w * sinf(rad);
                wsum += w;
            }
        }
    }

    /* Every pad at once for a while is not a hug - the device is being picked up
     * or carried. Re-learn the baseline instead of holding a fake touch forever. */
    bool handled = false;
    if (count >= n && n > 1) {
        if (t->all_pads_since == 0) {
            t->all_pads_since = t_ms;
        } else if (t_ms - t->all_pads_since >= c->all_pads_ms) {
            for (uint8_t i = 0; i < n; i++) {
                t->baseline[i] = (int32_t)values[i] << 8;
                t->pressed[i] = false;
                t->press_cnt[i] = t->release_cnt[i] = 0;
                t->delta[i] = 0;
            }
            mask = 0; count = 0; zone_mask = 0; zone = TOUCH_ZONE_NONE; max_delta = 0;
            sx = 0.0f; sy = 0.0f; wsum = 0.0f; angle_count = 0;
            t->all_pads_since = 0;
            recalibrated = true;
            handled = true;
        }
    } else {
        t->all_pads_since = 0;
    }

    const bool has_angle = wsum > 0.0f;
    float angle = t->angle;
    if (has_angle) {
        angle = atan2f(sy, sx) * 180.0f / (float)M_PI;
        if (angle < 0.0f) {
            angle += 360.0f;
        }
    }

    /* session state */
    touch_event_t event = recalibrated ? TOUCH_EVT_RECALIBRATED : TOUCH_EVT_NONE;
    if (!t->active && count > 0) {
        t->active = true;
        t->start_ms = t_ms;
        t->travel = 0.0f;
        t->gesture = TOUCH_GESTURE_NONE;
        t->had_angle = false;
        event = TOUCH_EVT_START;
    } else if (t->active && count > 0) {
        if (event != TOUCH_EVT_RECALIBRATED) {
            event = TOUCH_EVT_UPDATE;
        }
    }

    if (t->active && has_angle) {
        if (t->had_angle) {
            t->travel += wrap180(angle - t->prev_angle);
        }
        t->prev_angle = angle;
        t->had_angle = true;
    }
    t->angle = angle;
    t->has_angle = has_angle;
    t->crown = crown;

    const uint32_t duration = t->active ? t_ms - t->start_ms : 0;

    const float span = pad_span(pressed_angles, angle_count);
    if (t->active && count > 0) {
        if (count >= c->hug_pads && span >= c->hug_span_deg && duration >= c->hug_min_ms) {
            t->gesture = TOUCH_GESTURE_HUG;
        } else if (fabsf(t->travel) >= c->stroke_min_deg && t->gesture != TOUCH_GESTURE_HUG) {
            t->gesture = TOUCH_GESTURE_STROKE;
        } else if (duration >= c->hold_min_ms &&
                   (t->gesture == TOUCH_GESTURE_NONE || t->gesture == TOUCH_GESTURE_HOLD)) {
            t->gesture = TOUCH_GESTURE_HOLD;
        }
    }

    if (t->active && count == 0) {
        t->active = false;
        event = TOUCH_EVT_END;
        if (duration <= c->tap_max_ms && fabsf(t->travel) < c->stroke_min_deg &&
            t->gesture == TOUCH_GESTURE_NONE) {
            /* keep a short history of taps so repeated taps read as patting */
            if (t->tap_count > 0 && t_ms - t->tap_times[t->tap_count - 1] > c->double_tap_gap_ms) {
                t->tap_count = 0;
            }
            if (t->tap_count >= (uint8_t)(sizeof(t->tap_times) / sizeof(t->tap_times[0]))) {
                t->tap_count = 0;
            }
            t->tap_times[t->tap_count++] = t_ms;
            const bool in_window = t->tap_count >= c->pat_taps &&
                                   t_ms - t->tap_times[t->tap_count - c->pat_taps] <= c->pat_window_ms;
            if (in_window) {
                t->gesture = TOUCH_GESTURE_PAT;
            } else if (t->tap_count >= 2 && t_ms - t->last_tap_end_ms <= c->double_tap_gap_ms) {
                t->gesture = TOUCH_GESTURE_DOUBLE_TAP;
            } else {
                t->gesture = TOUCH_GESTURE_TAP;
            }
            t->had_tap = true;
            t->last_tap_end_ms = t_ms;
        } else {
            t->had_tap = false;
            t->tap_count = 0;
        }
    }

    out->event = event;
    out->gesture = t->gesture;
    out->active = t->active;
    out->pad_mask = mask;
    out->pad_count = count;
    out->has_angle = has_angle;
    out->angle = angle;
    out->travel = t->travel;
    out->span = span;
    out->crown = crown;
    out->duration_ms = duration;
    out->zone = zone;
    out->zone_mask = zone_mask;
    out->handled = handled;
    out->sustained = t->active && duration >= c->sustained_ms &&
                     (t->gesture == TOUCH_GESTURE_HOLD || t->gesture == TOUCH_GESTURE_HUG);
    out->recalibrated = recalibrated;
    float s = (float)max_delta / (float)(c->strength_full ? c->strength_full : 1);
    out->strength = s > 1.0f ? 1.0f : s;
    if (!t->active && event != TOUCH_EVT_END) {
        out->gesture = TOUCH_GESTURE_NONE;
    }
}

const char *touch_event_name(touch_event_t e)
{
    static const char *const n[] = {"NONE", "START", "UPDATE", "END", "RECALIBRATED"};
    return (unsigned)e < 5 ? n[e] : "?";
}

const char *touch_gesture_name(touch_gesture_t g)
{
    static const char *const n[] = {"none", "tap", "double tap", "hold", "stroke", "hug", "pat"};
    return (unsigned)g < 7 ? n[g] : "?";
}

const char *touch_zone_name(touch_zone_t z)
{
    static const char *const n[] = {"none", "front", "right", "back", "left", "crown", "base"};
    return (unsigned)z < 7 ? n[z] : "?";
}
