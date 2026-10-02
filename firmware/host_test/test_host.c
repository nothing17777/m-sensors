/* Host unit tests for motion_core + expression (no hardware needed). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "expression.h"
#include "motion_core.h"
#include "touch_core.h"

#define W 160
#define H 120
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t rng_state = 12345;
static int noise(int amp) { rng_state = rng_state * 1664525u + 1013904223u; return (int)((rng_state >> 16) % (2 * amp + 1)) - amp; }

static void make_frame(uint8_t *f, int sq_x, int sq_y, int sq, int val, int offset, int amp)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int v = 80 + (int)(30 * sin(x / 17.0) + 20 * cos(y / 11.0)) + offset + noise(amp);
            if (sq > 0 && x >= sq_x && x < sq_x + sq && y >= sq_y && y < sq_y + sq) v = val;
            f[y * W + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

typedef struct { int starts, ends, lighting, first_start, first_end, max_arousal; float min_cx, max_cx, max_energy; } stats_t;

static stats_t run(int n, void (*scene)(int i, uint8_t *f))
{
    motion_core_t m; motion_config_t cfg = MOTION_CONFIG_DEFAULT();
    uint8_t f[W * H]; motion_result_t r; stats_t s = {0, 0, 0, -1, -1, 0, 1, 0, 0};
    rng_state = 12345;
    if (motion_core_init(&m, &cfg, W, H) != 0) { printf("init failed\n"); exit(1); }
    for (int i = 0; i < n; i++) {
        scene(i, f);
        motion_core_process(&m, f, (uint32_t)i * 100, &r);
        if (r.event == MOTION_EVT_START) { s.starts++; if (s.first_start < 0) s.first_start = i; }
        if (r.event == MOTION_EVT_END) { s.ends++; if (s.first_end < 0) s.first_end = i; }
        if (r.event == MOTION_EVT_LIGHTING) s.lighting++;
        if (r.level != MOTION_LEVEL_NONE) { if (r.centroid_x < s.min_cx) s.min_cx = r.centroid_x; if (r.centroid_x > s.max_cx) s.max_cx = r.centroid_x; }
        if ((int)r.arousal > s.max_arousal) s.max_arousal = (int)r.arousal;
        if (r.energy > s.max_energy) s.max_energy = r.energy;
    }
    motion_core_deinit(&m);
    return s;
}

static void sc_static(int i, uint8_t *f)  { (void)i; make_frame(f, 0, 0, 0, 0, 0, 3); }
static void sc_noise(int i, uint8_t *f)   { (void)i; make_frame(f, 0, 0, 0, 0, 0, 14); }
static void sc_walk(int i, uint8_t *f)    { if (i >= 20 && i < 50) make_frame(f, 10 + (i - 20) * 3, 40, 28, 220, 0, 3); else make_frame(f, 0, 0, 0, 0, 0, 3); }
static void sc_lights(int i, uint8_t *f)  { make_frame(f, 0, 0, 0, 0, i >= 30 ? 80 : 0, 3); }
static void sc_left(int i, uint8_t *f)    { if (i >= 15) make_frame(f, 8 + i % 3, 45, 24, 230, 0, 3); else make_frame(f, 0, 0, 0, 0, 0, 3); }
static void sc_slow(int i, uint8_t *f)    { if (i >= 15) make_frame(f, 20 + (i - 15), 40, 24, 220, 0, 3); else make_frame(f, 0, 0, 0, 0, 0, 3); }
static void sc_fast(int i, uint8_t *f)    { if (i >= 15) make_frame(f, 60 + (int)(55 * sin(i * 1.3)), 20 + (int)(30 * fabs(cos(i * 0.9))), 40, 230, 0, 3); else make_frame(f, 0, 0, 0, 0, 0, 3); }
static void sc_still(int i, uint8_t *f)   { if (i >= 15 && i < 215) make_frame(f, 60, 40, 30, 230, 0, 3); else make_frame(f, 0, 0, 0, 0, 0, 3); }

static void test_motion(void)
{
    printf("motion_core\n");
    stats_t s;
    s = run(80, sc_static);  CHECK(s.starts == 0);
    s = run(80, sc_noise);   CHECK(s.starts == 0);
    s = run(90, sc_walk);    CHECK(s.starts == 1 && s.first_start >= 20 && s.first_start <= 23); CHECK(s.ends == 1 && s.first_end >= 64);
    s = run(70, sc_lights);  CHECK(s.lighting >= 1 && s.starts == 0);
    s = run(30, sc_left);    CHECK(s.max_cx < 0.3f);
    stats_t slow = run(60, sc_slow), fast = run(60, sc_fast);
    CHECK(fast.max_energy > slow.max_energy + 0.15f); CHECK(fast.max_arousal == MOTION_AROUSAL_INTENSE); CHECK(slow.max_arousal == MOTION_AROUSAL_CALM);
    s = run(300, sc_still);  CHECK(s.ends >= 1);
}

static void sc_dim_noise(int i, uint8_t *f) { (void)i; make_frame(f, 0, 0, 0, 0, -45, 26); }
static void sc_camera_moved(int i, uint8_t *f) { make_frame(f, 0, 0, 0, 0, i >= 40 ? 0 : 0, 3); if (i >= 40) { for (int k = 0; k < W * H; k++) f[k] = (uint8_t)(255 - f[k]); } }

static void test_motion_extras(void)
{
    printf("motion extras\n");
    stats_t s;
    /* a dim, noisy room must not trip false motion: the threshold floor follows the noise */
    s = run(120, sc_dim_noise); CHECK(s.starts == 0);
    /* the whole view changing (device moved / camera turned) is not a person */
    s = run(90, sc_camera_moved); CHECK(s.starts == 0 && s.lighting >= 1);

    /* approach and retreat */
    motion_core_t m; motion_config_t cfg = MOTION_CONFIG_DEFAULT();
    uint8_t f[W * H]; motion_result_t r; rng_state = 7;
    CHECK(motion_core_init(&m, &cfg, W, H) == 0);
    int approaching = 0, retreating = 0;
    for (int i = 0; i < 90; i++) {
        int size = i < 20 ? 0 : (i < 55 ? 10 + (i - 20) : 45 - (i - 55));
        if (size < 0) size = 0;
        make_frame(f, 70 - size / 2, 60 - size / 2, size, 225, 0, 3);
        motion_core_process(&m, f, (uint32_t)i * 100, &r);
        if (i > 25 && i < 50 && r.trend == MOTION_TREND_APPROACHING) approaching++;
        if (i > 60 && i < 85 && r.trend == MOTION_TREND_RETREATING) retreating++;
    }
    CHECK(approaching > 3); CHECK(retreating > 3);
    motion_core_deinit(&m);
}

static void test_expression(void)
{
    printf("expression\n");
    CHECK(emotion_from_label("Anger") == EMOTION_ANGRY);
    CHECK(emotion_from_label("  HAPPY ") == EMOTION_HAPPY);
    CHECK(emotion_from_label("???") == EMOTION_NEUTRAL);
    CHECK(emotion_from_label(NULL) == EMOTION_NEUTRAL);
    CHECK(response_emotion(EMOTION_HAPPY, 1.0f, true).brightness <= 40);
    CHECK(response_emotion(EMOTION_SAD, 0.0f, false).brightness < response_emotion(EMOTION_SAD, 1.0f, false).brightness);
    CHECK(response_auto(AUTO_STAGE_WARM, MOTION_AROUSAL_INTENSE, 0.5f, false).pattern == LED_PATTERN_RIPPLE);
    /* idle blinks; activation is calm first, then warm */
    CHECK(response_ambient(false).pattern == LED_PATTERN_BLINK);
    expression_t calm = response_auto(AUTO_STAGE_CALM, MOTION_AROUSAL_CALM, 0.0f, false);
    expression_t warm = response_auto(AUTO_STAGE_WARM, MOTION_AROUSAL_CALM, 0.0f, false);
    CHECK(calm.b > calm.r && warm.r > warm.b);            /* cool -> warm */
    CHECK(warm.brightness > calm.brightness);
    /* gentle touch stays warm and slow, a firmer touch gets dynamic */
    expression_t soft_t = response_touch(TOUCH_GESTURE_HOLD, 0.2f, 0.0f, true, 0.0f, false, false);
    expression_t firm_t = response_touch(TOUCH_GESTURE_HOLD, 0.95f, 0.0f, true, 0.0f, false, false);
    CHECK(firm_t.brightness > soft_t.brightness && firm_t.speed > soft_t.speed * 2.0f);
    CHECK(soft_t.pattern == LED_PATTERN_BREATHE && firm_t.pattern == LED_PATTERN_PULSE);

    behavior_t b; behavior_config_t bc = BEHAVIOR_CONFIG_DEFAULT(); bc.emotion_hold_s = 2.0f;
    behavior_init(&b, &bc);
    motion_result_t m = {0}; m.centroid_x = 0.9f; m.level = MOTION_LEVEL_ACTIVE; m.active = true; m.event = MOTION_EVT_START;
    behavior_on_motion(&b, &m, 1.0f);
    CHECK(b.mode == MODE_AUTO_RESPONSE && b.direction > 0.7f && behavior_wants_faces(&b, 1.0f));
    behavior_tick(&b, 1.5f);
    CHECK(b.auto_stage == AUTO_STAGE_CALM);           /* first seconds stay calm */
    behavior_tick(&b, 4.0f);
    CHECK(b.auto_stage == AUTO_STAGE_WARM);           /* they stayed, so it warms up */
    CHECK(behavior_expression(&b).r > behavior_expression(&b).b);
    CHECK(behavior_take_scan_request(&b) && !behavior_take_scan_request(&b));
    CHECK(!behavior_on_emotion(&b, EMOTION_HAPPY, 0.2f, 0.5f, 1.2f));
    CHECK(behavior_on_emotion(&b, EMOTION_HAPPY, 0.9f, 0.8f, 1.2f) && b.mode == MODE_EMOTION);
    CHECK(behavior_expression(&b).pattern == LED_PATTERN_SPARKLE);
    m.event = MOTION_EVT_END; m.active = false; m.level = MOTION_LEVEL_NONE;
    behavior_on_motion(&b, &m, 2.0f);
    behavior_tick(&b, 2.5f); CHECK(b.mode == MODE_EMOTION);
    behavior_tick(&b, 3.3f); CHECK(b.mode == MODE_AMBIENT);
    CHECK(!behavior_wants_faces(&b, 6.0f));
    behavior_on_face(&b, 6.0f); CHECK(behavior_wants_faces(&b, 7.0f));

    led_ring_t ring; uint8_t px[24 * 4];
    led_ring_init(&ring, 24, 0, 0.0f);
    expression_t x = response_auto(AUTO_STAGE_WARM, MOTION_AROUSAL_LIVELY, 1.0f, false);
    for (int k = 0; k < 40; k++) led_ring_render(&ring, &x, k * 0.05f, px);
    int best = 0, best_v = -1;
    for (int i = 0; i < 24; i++) { int v = px[i*4] + px[i*4+1] + px[i*4+2] + px[i*4+3]; if (v > best_v) { best_v = v; best = i; } }
    CHECK(best >= 5 && best <= 7);
}

/* ---------------- touch ---------------- */

#define PAD_BASE 400
typedef struct { touch_core_t core; uint16_t v[TOUCH_MAX_PADS]; uint32_t t; touch_result_t last; int taps, holds, strokes, hugs, doubles, pats, recals, starts, ends; } touch_rig_t;

static void rig_init(touch_rig_t *r)
{
    memset(r, 0, sizeof(*r));
    touch_config_t cfg = TOUCH_CONFIG_DEFAULT();
    touch_core_init(&r->core, &cfg);
    for (int i = 0; i < TOUCH_MAX_PADS; i++) r->v[i] = PAD_BASE;
    touch_result_t out;
    touch_core_process(&r->core, r->v, 0, &out);   /* first reading = baseline */
}

/* run for ms with the given pads pressed (bitmask), each pressed pad dropping by depth counts */
static void rig_run(touch_rig_t *r, uint32_t ms, uint16_t pads, int depth)
{
    for (uint32_t k = 0; k < ms; k += 20) {
        for (int i = 0; i < TOUCH_MAX_PADS; i++) r->v[i] = (uint16_t)(PAD_BASE - ((pads >> i) & 1 ? depth : 0));
        r->t += 20;
        touch_core_process(&r->core, r->v, r->t, &r->last);
        if (r->last.event == TOUCH_EVT_START) r->starts++;
        if (r->last.recalibrated) r->recals++;
        if (r->last.event == TOUCH_EVT_END) {
            r->ends++;
            switch (r->last.gesture) {
            case TOUCH_GESTURE_TAP: r->taps++; break;
            case TOUCH_GESTURE_DOUBLE_TAP: r->doubles++; break;
            case TOUCH_GESTURE_HOLD: r->holds++; break;
            case TOUCH_GESTURE_STROKE: r->strokes++; break;
            case TOUCH_GESTURE_HUG: r->hugs++; break;
            case TOUCH_GESTURE_PAT: r->pats++; break;
            default: break;
            }
        }
    }
}

static void test_touch(void)
{
    printf("touch_core\n");
    touch_rig_t r;

    /* nothing happening */
    rig_init(&r); rig_run(&r, 2000, 0, 0); CHECK(r.starts == 0 && r.ends == 0);

    /* small noise below the threshold is ignored */
    rig_init(&r);
    for (uint32_t k = 0; k < 100; k++) { for (int i = 0; i < TOUCH_MAX_PADS; i++) r.v[i] = (uint16_t)(PAD_BASE - (k % 3) * 4); r.t += 20; touch_core_process(&r.core, r.v, r.t, &r.last); CHECK(!r.last.active); }

    /* tap */
    rig_init(&r); rig_run(&r, 200, 1 << 0, 40); rig_run(&r, 800, 0, 0);
    CHECK(r.starts == 1 && r.taps == 1);

    /* double tap */
    rig_init(&r);
    rig_run(&r, 200, 1 << 0, 40); rig_run(&r, 200, 0, 0);
    rig_run(&r, 200, 1 << 0, 40); rig_run(&r, 600, 0, 0);
    CHECK(r.taps == 1 && r.doubles == 1);

    /* hold, with the glow pointing at the pad that is touched */
    rig_init(&r); rig_run(&r, 1500, 1 << 2, 60);
    CHECK(r.last.gesture == TOUCH_GESTURE_HOLD && r.last.has_angle);
    CHECK(fabs(r.last.angle - 90.0) < 1.0);      /* pad 2 sits at 90 degrees */
    CHECK(r.last.strength > 0.4f && r.last.strength <= 1.0f);
    rig_run(&r, 600, 0, 0); CHECK(r.holds == 1);

    /* firmer touch reads stronger */
    touch_rig_t soft, firm;
    rig_init(&soft); rig_run(&soft, 800, 1 << 0, 20);
    rig_init(&firm); rig_run(&firm, 800, 1 << 0, 110);
    CHECK(firm.last.strength > soft.last.strength + 0.5f);

    /* stroke around the shell */
    rig_init(&r);
    rig_run(&r, 200, 1 << 0, 50); rig_run(&r, 200, (1 << 0) | (1 << 1), 50); rig_run(&r, 200, 1 << 1, 50);
    rig_run(&r, 200, (1 << 1) | (1 << 2), 50); rig_run(&r, 300, 1 << 2, 50);
    CHECK(r.last.gesture == TOUCH_GESTURE_STROKE); CHECK(r.last.travel > 60.0f);
    rig_run(&r, 600, 0, 0); CHECK(r.strokes == 1);

    /* one broad hand over neighbouring pads is a hold, not a hug */
    rig_init(&r); rig_run(&r, 1200, (1 << 0) | (1 << 1) | (1 << 7), 70);
    CHECK(r.last.gesture == TOUCH_GESTURE_HOLD);

    /* hug: pads on opposite sides of the shell */
    rig_init(&r); rig_run(&r, 900, (1 << 0) | (1 << 2) | (1 << 5), 70);
    CHECK(r.last.gesture == TOUCH_GESTURE_HUG && r.last.pad_count == 3);
    rig_run(&r, 600, 0, 0); CHECK(r.hugs == 1);

    /* crown pad has no angle: a pat on the head, and it knows the zone */
    rig_init(&r); rig_run(&r, 900, 1 << 8, 60);
    CHECK(r.last.crown && !r.last.has_angle && r.last.active);
    CHECK(r.last.zone == TOUCH_ZONE_CROWN);
    rig_init(&r); rig_run(&r, 900, 1 << 0, 60); CHECK(r.last.zone == TOUCH_ZONE_FRONT);
    rig_init(&r); rig_run(&r, 900, 1 << 4, 60); CHECK(r.last.zone == TOUCH_ZONE_BACK);

    /* rhythmic patting reads as patting, not three separate taps */
    rig_init(&r);
    for (int k = 0; k < 4; k++) { rig_run(&r, 160, 1 << 2, 50); rig_run(&r, 240, 0, 0); }
    CHECK(r.pats >= 1 && r.taps == 1);   /* first tap is a tap, then it reads as patting */

    /* a long hold settles: same gesture, reported as sustained */
    rig_init(&r); rig_run(&r, 6000, 1 << 2, 60);
    CHECK(r.last.gesture == TOUCH_GESTURE_HOLD && !r.last.sustained);
    rig_run(&r, 6000, 1 << 2, 60);
    CHECK(r.last.sustained);
    CHECK(response_touch(TOUCH_GESTURE_HOLD, 0.3f, 0, true, 0, true, false).speed <
          response_touch(TOUCH_GESTURE_HOLD, 0.3f, 0, true, 0, false, false).speed);

    /* every pad at once = being picked up, not a hug */
    rig_init(&r); rig_run(&r, 3000, 0x1FF, 70);
    CHECK(r.recals >= 1 && !r.last.active);

    /* a pad stuck for a minute re-baselines itself instead of sticking forever */
    rig_init(&r); rig_run(&r, 62000, 1 << 3, 60);
    CHECK(r.recals >= 1 && !r.last.active);

    /* slow drift (warming shell) never presses */
    rig_init(&r);
    for (uint32_t k = 0; k < 600; k++) { for (int i = 0; i < TOUCH_MAX_PADS; i++) r.v[i] = (uint16_t)(PAD_BASE - k / 20); r.t += 20; touch_core_process(&r.core, r.v, r.t, &r.last); }
    CHECK(!r.last.active);
}

static void test_touch_behaviour(void)
{
    printf("touch behaviour\n");
    behavior_t b; behavior_config_t bc = BEHAVIOR_CONFIG_DEFAULT(); bc.touch_afterglow_s = 2.0f;
    behavior_init(&b, &bc);
    touch_result_t t = {0};
    t.event = TOUCH_EVT_START; t.active = true; t.pad_count = 1; t.has_angle = true; t.angle = 90.0f; t.strength = 0.8f;
    behavior_on_touch(&b, &t, 1.0f);
    CHECK(b.mode == MODE_TOUCH);
    /* an EchoAi result arriving mid-touch does not interrupt the touch */
    CHECK(behavior_on_emotion(&b, EMOTION_HAPPY, 0.9f, 0.8f, 1.2f) && b.mode == MODE_TOUCH);
    t.gesture = TOUCH_GESTURE_HOLD;
    behavior_on_touch(&b, &t, 2.0f);
    expression_t x = behavior_expression(&b);
    CHECK(x.pattern == LED_PATTERN_PULSE && x.r > 200 && x.b < 80);     /* firm touch: warm but dynamic */
    t.strength = 0.15f;                                                  /* same hold, barely any pressure */
    behavior_on_touch(&b, &t, 2.0f);
    CHECK(behavior_expression(&b).pattern == LED_PATTERN_BREATHE);       /* gentle touch: slow and warm */
    t.strength = 0.8f;
    CHECK(x.haptic == HAPTIC_PURR);
    CHECK(fabs(x.direction - 1.0) < 0.01);                              /* 90 deg = a quarter turn */
    /* hand leaves: warm afterglow, then back to the emotion that was waiting */
    t.event = TOUCH_EVT_END; t.active = false;
    behavior_on_touch(&b, &t, 3.0f);
    behavior_tick(&b, 4.0f); CHECK(b.mode == MODE_TOUCH);
    behavior_tick(&b, 5.2f); CHECK(b.mode == MODE_EMOTION);
    /* double tap switches night mode */
    CHECK(!b.night);
    t.gesture = TOUCH_GESTURE_DOUBLE_TAP; behavior_on_touch(&b, &t, 6.0f);
    CHECK(b.night);
    CHECK(response_touch(TOUCH_GESTURE_HUG, 1.0f, 0.0f, true, 0.0f, false, true).brightness <= 40);
    /* a hand on the shell counts as someone being present */
    behavior_t b2; behavior_config_t bc2 = BEHAVIOR_CONFIG_DEFAULT();
    behavior_init(&b2, &bc2);
    touch_result_t t2 = {0}; t2.active = true; t2.event = TOUCH_EVT_START;
    behavior_on_touch(&b2, &t2, 10.0f);
    CHECK(behavior_wants_faces(&b2, 10.0f));
    /* someone stepping out and straight back in is not greeted from scratch */
    behavior_t b3; behavior_config_t bc3 = BEHAVIOR_CONFIG_DEFAULT();
    behavior_init(&b3, &bc3);
    motion_result_t m3 = {0}; m3.event = MOTION_EVT_END;
    behavior_on_motion(&b3, &m3, 30.0f);
    b3.mode = MODE_AMBIENT;
    m3.event = MOTION_EVT_START; m3.active = true; m3.level = MOTION_LEVEL_PRESENCE;
    behavior_on_motion(&b3, &m3, 35.0f);
    behavior_tick(&b3, 35.0f);
    CHECK(b3.auto_stage == AUTO_STAGE_WARM);
}

int main(void)
{
    test_motion();
    test_motion_extras();
    test_expression();
    test_touch();
    test_touch_behaviour();
    printf(failures ? "%d FAILED\n" : "ALL HOST TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
