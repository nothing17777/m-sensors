/*
 * M camera motion sensor - core algorithm.
 * Step-for-step mirror of pc_prototype/motion_detector.py (see that file for notes).
 */
#include "motion_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef MOTION_CORE_MALLOC
#define MOTION_CORE_MALLOC(sz) malloc(sz)
#define MOTION_CORE_FREE(p)    free(p)
#endif

#define AROUSAL_T1   0.25f
#define AROUSAL_T2   0.60f
#define AROUSAL_HYST 0.05f

static uint32_t ratio_to_cells(float ratio, uint32_t cells)
{
    return (uint32_t)(ratio * (float)cells + 0.5f);
}

/* bg += (cur - bg) / 2^shift, truncating toward zero */
static inline int32_t adapt(int32_t bg, int32_t cur8, uint8_t shift)
{
    int32_t d = cur8 - bg;
    return d >= 0 ? bg + (d >> shift) : bg - ((-d) >> shift);
}

static motion_arousal_t next_arousal(motion_arousal_t lvl, float e)
{
    if (e >= AROUSAL_T2 + AROUSAL_HYST) {
        lvl = MOTION_AROUSAL_INTENSE;
    } else if (e >= AROUSAL_T1 + AROUSAL_HYST && lvl == MOTION_AROUSAL_CALM) {
        lvl = MOTION_AROUSAL_LIVELY;
    }
    if (e <= AROUSAL_T1 - AROUSAL_HYST) {
        lvl = MOTION_AROUSAL_CALM;
    } else if (e <= AROUSAL_T2 - AROUSAL_HYST && lvl == MOTION_AROUSAL_INTENSE) {
        lvl = MOTION_AROUSAL_LIVELY;
    }
    return lvl;
}

int motion_core_init(motion_core_t *m, const motion_config_t *cfg, uint16_t width, uint16_t height)
{
    if (!m || !cfg || cfg->block == 0) {
        return -1;
    }
    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;
    m->in_w = width;
    m->in_h = height;
    m->cw = width / cfg->block;
    m->ch = height / cfg->block;
    if (m->cw < 3 || m->ch < 3) {
        return -1;
    }
    m->cells = (uint32_t)m->cw * m->ch;
    m->bg   = MOTION_CORE_MALLOC(m->cells * sizeof(uint16_t));
    m->cur  = MOTION_CORE_MALLOC(m->cells);
    m->prev = MOTION_CORE_MALLOC(m->cells);
    m->raw  = MOTION_CORE_MALLOC(m->cells);
    m->mask = MOTION_CORE_MALLOC(m->cells);
    if (!m->bg || !m->cur || !m->prev || !m->raw || !m->mask) {
        motion_core_deinit(m);
        return -2;
    }
    m->min_cells = ratio_to_cells(cfg->min_area_ratio, m->cells);
    if (m->min_cells < 1) {
        m->min_cells = 1;
    }
    m->active_cells = ratio_to_cells(cfg->active_area_ratio, m->cells);
    m->lighting_cells = ratio_to_cells(cfg->lighting_ratio, m->cells);
    motion_core_reset(m);
    return 0;
}

void motion_core_deinit(motion_core_t *m)
{
    if (!m) {
        return;
    }
    MOTION_CORE_FREE(m->bg);
    MOTION_CORE_FREE(m->cur);
    MOTION_CORE_FREE(m->prev);
    MOTION_CORE_FREE(m->raw);
    MOTION_CORE_FREE(m->mask);
    m->bg = NULL;
    m->cur = m->prev = m->raw = m->mask = NULL;
}

void motion_core_reset(motion_core_t *m)
{
    m->initialized = false;
    m->frame_count = 0;
    m->consec = 0;
    m->active = false;
    m->start_ms = m->last_motion_ms = m->prev_t = 0;
    m->has_prev_t = m->prev_valid = false;
    m->prev_cx = m->prev_cy = m->speed = m->energy = 0.0f;
    m->area_fast = m->area_slow = 0.0f;
    m->noise_q8 = 0;
    m->settle = 0;
    m->arousal = MOTION_AROUSAL_CALM;
    m->trend = MOTION_TREND_STEADY;
    if (m->mask) {
        memset(m->mask, 0, m->cells);
    }
}

void motion_core_recalibrate(motion_core_t *m)
{
    motion_core_reset(m);
    m->initialized = false;      /* the next frame becomes the new background */
}

const uint8_t *motion_core_mask(const motion_core_t *m)
{
    return m->mask;
}

void motion_core_process(motion_core_t *m, const uint8_t *gray, uint32_t t_ms, motion_result_t *out)
{
    const motion_config_t *c = &m->cfg;
    const uint16_t cw = m->cw, ch = m->ch;
    const uint8_t b = c->block;
    const uint32_t block_area = (uint32_t)b * b;

    /* 1. block-average downscale */
    for (uint16_t cy = 0; cy < ch; cy++) {
        for (uint16_t cx = 0; cx < cw; cx++) {
            uint32_t sum = 0;
            for (uint8_t dy = 0; dy < b; dy++) {
                const uint8_t *row = gray + (uint32_t)(cy * b + dy) * m->in_w + (uint32_t)cx * b;
                for (uint8_t dx = 0; dx < b; dx++) {
                    sum += row[dx];
                }
            }
            m->cur[(uint32_t)cy * cw + cx] = (uint8_t)(sum / block_area);
        }
    }

    memset(out, 0, sizeof(*out));
    out->centroid_x = 0.5f;
    out->centroid_y = 0.5f;
    out->active = m->active;
    out->arousal = m->arousal;
    out->trend = m->trend;
    out->speed = m->speed;
    out->energy = m->energy;

    if (!m->initialized) {
        for (uint32_t i = 0; i < m->cells; i++) {
            m->bg[i] = (uint16_t)(m->cur[i] << 8);
        }
        memcpy(m->prev, m->cur, m->cells);
        m->initialized = true;
        m->frame_count = 1;
        return;
    }
    if (m->frame_count < c->warmup_frames) {
        /* measure the sensor noise while warming up, so the threshold floor is
         * already right by the time the first events can fire */
        uint32_t nsum = 0;
        for (uint32_t i = 0; i < m->cells; i++) {
            int32_t d = ((int32_t)m->cur[i] << 8) - m->bg[i];
            if (d < 0) {
                d = -d;
            }
            nsum += (uint32_t)(d >> 8);
            m->bg[i] = (uint16_t)adapt(m->bg[i], (int32_t)m->cur[i] << 8, 1);
        }
        const int32_t mean_q8 = (int32_t)((nsum << 8) / m->cells);
        m->noise_q8 = (uint32_t)((int32_t)m->noise_q8 + ((mean_q8 - (int32_t)m->noise_q8) >> 2));
        memcpy(m->prev, m->cur, m->cells);
        m->frame_count++;
        return;
    }

    /* 2-3. diff + selective background update.
     * The threshold has a floor that follows the measured sensor noise, so a dim,
     * lamp-lit room (where camera noise rises) does not trip false motion. */
    uint32_t thr = c->pixel_threshold;
    const uint32_t dyn = (m->noise_q8 * (uint32_t)c->noise_gain_q4) >> 12;
    if (dyn > thr) {
        thr = dyn;
    }
    out->threshold = (uint16_t)thr;

    uint32_t raw_count = 0, noise_sum = 0, noise_n = 0;
    for (uint32_t i = 0; i < m->cells; i++) {
        int32_t cur8 = (int32_t)m->cur[i] << 8;
        int32_t bg = m->bg[i];
        int32_t d = cur8 - bg;
        if (d < 0) {
            d = -d;
        }
        const uint32_t diff = (uint32_t)(d >> 8);
        uint8_t changed = diff > thr;
        if (!changed) {
            noise_sum += diff;
            noise_n++;
        }
        int16_t fd = (int16_t)m->cur[i] - (int16_t)m->prev[i];
        if (fd < 0) {
            fd = -fd;
        }
        uint8_t moving_cell = changed && fd > c->stable_threshold;
        m->raw[i] = changed;
        raw_count += changed;
        m->bg[i] = (uint16_t)adapt(bg, cur8, moving_cell ? c->fg_shift : c->bg_shift);
    }
    memcpy(m->prev, m->cur, m->cells);
    if (noise_n > 0) {
        const int32_t mean_q8 = (int32_t)((noise_sum << 8) / noise_n);
        m->noise_q8 = (uint32_t)((int32_t)m->noise_q8 + ((mean_q8 - (int32_t)m->noise_q8) >> 5));
    }

    /* 4. whole-scene change: lights switched, or the device itself was moved.
     * The background is re-taken and the next few frames are ignored while the
     * camera's exposure settles, instead of reporting a room full of motion. */
    const bool lighting = raw_count > m->lighting_cells;
    if (lighting) {
        m->settle = c->settle_frames;
    }
    const bool settling = m->settle > 0;
    out->settling = settling;

    /* 5. neighbour filter + geometry */
    uint32_t count = 0, sum_x = 0, sum_y = 0;
    uint16_t x0 = cw, y0 = ch, x1 = 0, y1 = 0;
    if (lighting || settling) {
        if (m->settle > 0) {
            m->settle--;
        }
        for (uint32_t i = 0; i < m->cells; i++) {
            m->bg[i] = (uint16_t)(lighting ? (m->cur[i] << 8) : adapt(m->bg[i], (int32_t)m->cur[i] << 8, 1));
        }
        memset(m->mask, 0, m->cells);
    } else {
        for (uint16_t y = 0; y < ch; y++) {
            for (uint16_t x = 0; x < cw; x++) {
                const uint32_t i = (uint32_t)y * cw + x;
                uint8_t keep = 0;
                if (m->raw[i]) {
                    uint8_t nb = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        int yy = (int)y + dy;
                        if (yy < 0 || yy >= ch) {
                            continue;
                        }
                        for (int dx = -1; dx <= 1; dx++) {
                            int xx = (int)x + dx;
                            if ((dx == 0 && dy == 0) || xx < 0 || xx >= cw) {
                                continue;
                            }
                            nb += m->raw[(uint32_t)yy * cw + (uint32_t)xx];
                        }
                    }
                    keep = nb >= c->min_neighbors;
                }
                m->mask[i] = keep;
                if (keep) {
                    count++;
                    sum_x += x;
                    sum_y += y;
                    if (x < x0) x0 = x;
                    if (y < y0) y0 = y;
                    if (x > x1) x1 = x;
                    if (y > y1) y1 = y;
                }
            }
        }
    }

    const bool moving = count >= m->min_cells;
    const float area = (float)count / (float)m->cells;
    out->changed_cells = count;
    out->area_ratio = area;

    float dt_s = 0.0f;
    if (m->has_prev_t && t_ms > m->prev_t) {
        dt_s = (float)(t_ms - m->prev_t) / 1000.0f;
    }
    m->prev_t = t_ms;
    m->has_prev_t = true;

    /* 6. centroid + speed */
    const float alpha_s = c->speed_alpha;
    if (moving) {
        const float cxc = (float)sum_x / (float)count;
        const float cyc = (float)sum_y / (float)count;
        out->centroid_x = (cxc + 0.5f) / (float)cw;
        out->centroid_y = (cyc + 0.5f) / (float)ch;
        out->bbox_x0 = x0;
        out->bbox_y0 = y0;
        out->bbox_x1 = x1;
        out->bbox_y1 = y1;
        out->level = count >= m->active_cells ? MOTION_LEVEL_ACTIVE : MOTION_LEVEL_PRESENCE;
        if (m->prev_valid && dt_s > 0.0f) {
            const float dx = cxc - m->prev_cx;
            const float dy = cyc - m->prev_cy;
            const float inst = sqrtf(dx * dx + dy * dy) / (float)cw / dt_s;
            m->speed = m->speed + alpha_s * (inst - m->speed);
        }
        m->prev_cx = cxc;
        m->prev_cy = cyc;
        m->prev_valid = true;
    } else {
        m->speed = m->speed + alpha_s * (0.0f - m->speed);
        m->prev_valid = false;
    }

    /* energy -> arousal */
    float target = 0.0f;
    if (moving) {
        float a = area / c->area_full_ratio;
        float s = m->speed / c->speed_full;
        if (a > 1.0f) a = 1.0f;
        if (s > 1.0f) s = 1.0f;
        target = 0.5f * a + 0.5f * s;
    }
    m->energy = m->energy + c->energy_alpha * (target - m->energy);
    m->arousal = next_arousal(m->arousal, m->energy);

    /* approach / retreat: a fast and a slow average of how much of the frame moves */
    m->area_fast = m->area_fast + c->trend_fast_alpha * (area - m->area_fast);
    m->area_slow = m->area_slow + c->trend_slow_alpha * (area - m->area_slow);
    const float trend_d = m->area_fast - m->area_slow;
    if (!m->active) {
        m->trend = MOTION_TREND_STEADY;
    } else if (trend_d > c->trend_deadband) {
        m->trend = MOTION_TREND_APPROACHING;
    } else if (trend_d < -c->trend_deadband) {
        m->trend = MOTION_TREND_RETREATING;
    } else {
        m->trend = MOTION_TREND_STEADY;
    }

    /* 7. session state */
    if (moving) {
        if (m->consec < 255) {
            m->consec++;
        }
        m->last_motion_ms = t_ms;
    } else {
        m->consec = 0;
    }

    motion_event_t event = lighting ? MOTION_EVT_LIGHTING : MOTION_EVT_NONE;
    if (settling && m->active) {          /* the scene changed under us: end the session */
        m->active = false;
        m->consec = 0;
        event = MOTION_EVT_END;
    } else if (!m->active) {
        if (m->consec >= c->frames_to_trigger) {
            m->active = true;
            m->start_ms = t_ms;
            event = MOTION_EVT_START;
        }
    } else if (moving) {
        event = MOTION_EVT_UPDATE;
    } else if (t_ms - m->last_motion_ms >= c->quiet_ms) {
        m->active = false;
        event = MOTION_EVT_END;
    }

    out->event = event;
    out->active = m->active;
    out->duration_ms = (m->active || event == MOTION_EVT_END) ? t_ms - m->start_ms : 0;
    out->speed = m->speed;
    out->energy = m->energy;
    out->arousal = m->arousal;
    out->trend = m->trend;
}

const char *motion_event_name(motion_event_t e)
{
    static const char *const n[] = {"NONE", "START", "UPDATE", "END", "LIGHTING"};
    return (unsigned)e < 5 ? n[e] : "?";
}

const char *motion_level_name(motion_level_t l)
{
    static const char *const n[] = {"NONE", "PRESENCE", "ACTIVE"};
    return (unsigned)l < 3 ? n[l] : "?";
}

const char *motion_trend_name(motion_trend_t t)
{
    static const char *const n[] = {"steady", "approaching", "retreating"};
    return (unsigned)t < 3 ? n[t] : "?";
}

const char *motion_arousal_name(motion_arousal_t a)
{
    static const char *const n[] = {"CALM", "LIVELY", "INTENSE"};
    return (unsigned)a < 3 ? n[a] : "?";
}
