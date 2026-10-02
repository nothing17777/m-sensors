import os
import sys

import cv2
import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import response_map as rm  # noqa: E402
import synthetic as syn  # noqa: E402
from behavior import Behavior, Mode  # noqa: E402
from config import SENSOR_H, SENSOR_W, MotionConfig  # noqa: E402
from emotion_providers import EchoAiProvider, EmotionResult, EmotionWorker, MockProvider  # noqa: E402
from face_detector import crop_face_jpeg  # noqa: E402
from led_sim import LedRing  # noqa: E402
from motion_detector import Arousal, MotionDetector, MotionEvent, MotionLevel, MotionResult, Trend  # noqa: E402
from config import TouchConfig  # noqa: E402
from touch_detector import Gesture, TouchDetector, TouchEvent, TouchPadSimulator  # noqa: E402


def run(frames):
    det = MotionDetector(SENSOR_W, SENSOR_H, MotionConfig())
    return [det.process(f, t) for t, f in frames]


def make(seq_fn, n, seed=1):
    rng = np.random.default_rng(seed)
    bg = syn.background(rng)
    return [(i * 100, syn.frame(bg, rng, **seq_fn(i))) for i in range(n)]


def events(results, kind):
    return [i for i, r in enumerate(results) if r.event == kind]


def test_static_scene_never_triggers():
    res = run(make(lambda i: {}, 80))
    assert not events(res, MotionEvent.START)
    assert all(r.level == MotionLevel.NONE for r in res)


def test_heavy_noise_never_triggers():
    res = run(make(lambda i: {"noise": 9.0}, 80))
    assert not events(res, MotionEvent.START)


def test_still_subject_leaving_leaves_no_long_ghost():
    # person stands still for 20 s then leaves: session must end within ~5 s
    res = run(make(lambda i: {"square": (60, 40, 30, 230)} if 15 <= i < 215 else {}, 300))
    ends = events(res, MotionEvent.END)
    assert ends and ends[-1] - 215 <= 50


def test_moving_subject_start_update_end():
    res = run(make(lambda i: {"square": (10 + (i - 20) * 3, 40, 28, 220)} if 20 <= i < 50 else {}, 90))
    starts, ends = events(res, MotionEvent.START), events(res, MotionEvent.END)
    assert len(starts) == 1 and 20 <= starts[0] <= 23          # debounced, fast
    assert len(ends) == 1 and ends[0] >= 50 + 14               # ends after quiet_ms (1.5 s)
    assert events(res, MotionEvent.UPDATE)
    assert res[ends[0]].duration_ms > 2500


def test_dim_noisy_room_does_not_trip_motion():
    # a dim room raises camera noise; the threshold floor should follow it
    quiet = run(make(lambda i: {"noise": 2.0}, 120))
    noisy = run(make(lambda i: {"noise": 30.0, "offset": -45}, 120))
    assert not events(noisy, MotionEvent.START) and not events(quiet, MotionEvent.START)
    assert noisy[-1].threshold > quiet[-1].threshold == MotionConfig().pixel_threshold


def test_scene_upheaval_settles_instead_of_reporting_motion():
    # the device is moved: every pixel changes at once
    rng = np.random.default_rng(5)
    bg_a = syn.background(rng)
    bg_b = 255.0 - bg_a                       # a completely different view
    frames = [(i * 100, syn.frame(bg_a if i < 40 else bg_b, rng)) for i in range(90)]
    res = run(frames)
    assert not events(res, MotionEvent.START)
    assert any(r.settling for r in res)
    assert not res[-1].settling and not res[-1].active     # it recovers by itself


def test_approach_and_retreat():
    def scene(i):
        size = 0 if i < 20 else (10 + (i - 20) if i < 55 else max(0, 45 - (i - 55)))
        return {"square": (70 - size / 2, 60 - size / 2, size, 225)} if size else {}
    res = run(make(scene, 90))
    assert any(r.trend == Trend.APPROACHING for r in res[25:50])
    assert any(r.trend == Trend.RETREATING for r in res[60:85])


def test_lighting_change_is_not_motion():
    res = run(make(lambda i: {"offset": 80} if i >= 30 else {}, 70))
    assert events(res, MotionEvent.LIGHTING)
    assert not events(res, MotionEvent.START)


def test_centroid_tracks_side():
    left = run(make(lambda i: {"square": (8 + i % 3, 45, 24, 230)} if i >= 15 else {}, 30))
    right = run(make(lambda i: {"square": (125 + i % 3, 45, 24, 230)} if i >= 15 else {}, 30))
    lx = [r.centroid_x for r in left if r.level != MotionLevel.NONE]
    rx = [r.centroid_x for r in right if r.level != MotionLevel.NONE]
    assert lx and rx and max(lx) < 0.3 and min(rx) > 0.7


def test_fast_motion_is_more_intense_than_slow():
    slow = run(make(lambda i: {"square": (20 + (i - 15) * 1.0, 40, 24, 220)} if i >= 15 else {}, 60))
    fast = run(make(lambda i: {"square": (60 + 55 * np.sin(i * 1.3), 20 + 30 * abs(np.cos(i * 0.9)), 40, 230)}
                    if i >= 15 else {}, 60))
    assert max(r.energy for r in fast) > max(r.energy for r in slow) + 0.15
    assert any(r.arousal == Arousal.INTENSE for r in fast)
    assert all(r.arousal == Arousal.CALM for r in slow)


def test_full_scenario_runs_and_recovers():
    res = run(list(syn.scenario()))
    assert len(events(res, MotionEvent.START)) >= 3
    assert len(events(res, MotionEvent.START)) == len(events(res, MotionEvent.END))
    assert not res[-1].active


def test_behavior_flow():
    b = Behavior(emotion_hold_s=2.0, min_confidence=0.4)
    det = MotionDetector(SENSOR_W, SENSOR_H)
    frames = make(lambda i: {"square": (100, 40, 30, 230)} if 15 <= i < 40 else {}, 140)
    seen = set()
    for t, f in frames:
        now = t / 1000
        b.on_motion(det.process(f, t), now)
        if 2.0 <= now < 2.05:
            assert b.mode == Mode.AUTO_RESPONSE and b.scan_due and b.wants_faces(now)
            assert b.direction > 0.2                                  # subject on the right
            assert not b.on_emotion(EmotionResult("happy", 0.2, 0.5), now)  # low confidence ignored
            assert b.on_emotion(EmotionResult("Happiness", 0.9, 0.8), now)
        b.tick(now)
        seen.add(b.mode)
        if b.mode == Mode.EMOTION:
            assert b.expression().pattern == rm.Pattern.SPARKLE
    assert seen == {Mode.AMBIENT, Mode.AUTO_RESPONSE, Mode.EMOTION}
    assert b.mode == Mode.AMBIENT and not b.wants_faces(frames[-1][0] / 1000)


def test_response_map():
    assert rm.normalize_label("Anger") == "angry"
    assert rm.normalize_label("???") == "neutral"
    night = rm.emotion_expression("happy", 1.0, night=True)
    assert night.brightness <= rm.NIGHT_BRIGHTNESS_CAP
    assert rm.emotion_expression("sad", 0.0).brightness < rm.emotion_expression("sad", 1.0).brightness
    assert rm.auto_response(Arousal.INTENSE, 0.5, stage=rm.AutoStage.WARM).pattern == rm.Pattern.RIPPLE
    # idle blinks; activation is calm (cool) first, then warm
    assert rm.ambient().pattern == rm.Pattern.BLINK
    calm = rm.auto_response(Arousal.CALM, 0.0, stage=rm.AutoStage.CALM)
    warm = rm.auto_response(Arousal.CALM, 0.0, stage=rm.AutoStage.WARM)
    assert calm.rgbw[2] > calm.rgbw[0] and warm.rgbw[0] > warm.rgbw[2]
    assert warm.brightness > calm.brightness
    assert set(rm.EMOTION_TABLE) == set(rm.EMOTIONS)


def test_led_ring_glow_follows_direction():
    ring = LedRing(n=24, front_index=0, fade_s=0.0)
    for k in range(40):
        ring.set(rm.auto_response(Arousal.LIVELY, 1.0), k * 0.05)
    brightest = int(np.argmax(ring.pixels.sum(axis=1)))
    assert brightest in (5, 6, 7)   # direction +1 -> quarter ring from LED 0


def test_face_crop_and_parse():
    img = np.full((480, 640, 3), 128, np.uint8)
    jpeg = crop_face_jpeg(img, (300, 200, 80, 90), size=224)
    decoded = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
    assert decoded.shape == (224, 224, 3)
    p = EchoAiProvider.__new__(EchoAiProvider)
    r = p._parse_response({"emotions": {"sad": 0.7, "happy": 0.2}}, 120.0)
    assert r.label == "sad" and r.confidence == pytest.approx(0.7)


def test_worker_rate_limit_and_offline_fallback():
    class Failing(MockProvider):
        def analyze(self, face_jpeg):
            raise ConnectionError("no wifi")

    w = EmotionWorker(MockProvider(latency_s=0.0), interval_s=4.0, urgent_gap_s=1.0, error_backoff_s=10.0)
    assert w.ready(0.0)
    w.submit(b"x", 0.0)
    assert not w.ready(0.1)
    w._future.result()
    assert w.poll(0.2)[0] == "ok"
    assert not w.ready(2.0) and w.ready(2.0, urgent=True) and w.ready(4.0)

    f = EmotionWorker(Failing(), 4.0, 1.0, 10.0)
    f.submit(b"x", 0.0)
    try:
        f._future.result()
    except ConnectionError:
        pass
    assert f.poll(0.1)[0] == "error" and not f.online
    assert not f.ready(5.0, urgent=True) and f.ready(10.2)
    w.shutdown()
    f.shutdown()


def test_main_headless_end_to_end(tmp_path, monkeypatch, capsys):
    """Full loop on a synthetic video; face detector patched to 'see' a face while present."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "..", "tools"))
    import main as app
    import make_demo_video
    video = str(tmp_path / "demo.avi")
    make_demo_video.main(video)
    monkeypatch.setattr(app.FaceDetector, "__init__", lambda self, *a, **k: None)
    monkeypatch.setattr(app.FaceDetector, "detect", lambda self, frame: (280, 180, 90, 100))
    monkeypatch.setattr(app, "MockProvider", lambda cycle=False: MockProvider(cycle=True, latency_s=0.0))
    assert app.main(["--video", video, "--headless", "--mock-cycle"]) == 0
    out = capsys.readouterr().out
    assert "face scan -> mock" in out and "EMOTION " in out
    assert "-> emotion" in out          # the mode it comes from depends on video timing
    assert "errors=0" in out


def test_face_detector_loads():
    """Catches an OpenCV install without CascadeClassifier (e.g. OpenCV 5 or conflicting cv2 packages)."""
    from face_detector import FaceDetector
    assert FaceDetector().detect(np.zeros((480, 640, 3), np.uint8)) is None



# ---------------- touch sensor ----------------

class TouchRig:
    """Feeds the detector simulated pad readings at 50 Hz."""

    def __init__(self):
        self.det = TouchDetector(TouchConfig())
        self.sim = TouchPadSimulator(TouchConfig(), noise=1)
        self.t = 0
        self.events = []
        self.last = None
        self.last_end = None
        self.run(200)                      # settle: first reading sets the baseline

    def run(self, ms, **kw):
        for _ in range(0, ms, 20):
            r = self.det.process(self.sim.read(**kw), self.t)
            self.t += 20
            self.last = r
            if r.event in (TouchEvent.START, TouchEvent.END):
                self.events.append((r.event, r.gesture))
                if r.event == TouchEvent.END:
                    self.last_end = r
            if r.recalibrated:
                self.events.append((TouchEvent.RECALIBRATED, r.gesture))
        return self.last

    def gestures(self):
        return [g for e, g in self.events if e == TouchEvent.END]


def test_touch_nothing_happens_when_untouched():
    rig = TouchRig()
    rig.run(3000)
    assert not rig.events and not rig.last.active


def test_touch_tap_and_double_tap():
    rig = TouchRig()
    rig.run(200, angle=90); rig.run(800)
    assert rig.gestures() == [Gesture.TAP]
    rig.run(200, angle=90); rig.run(200); rig.run(200, angle=90); rig.run(600)
    assert rig.gestures()[-2:] == [Gesture.TAP, Gesture.DOUBLE_TAP]


def test_touch_hold_points_at_the_hand():
    rig = TouchRig()
    r = rig.run(1500, angle=180)
    assert r.gesture == Gesture.HOLD and r.has_angle and abs(r.angle - 180) < 5
    assert 0.4 < r.strength <= 1.0
    expr = rm.touch_expression(r.gesture, r.strength, r.angle, r.has_angle, r.travel)
    assert expr.rgbw[0] > 200 and expr.rgbw[2] < 80          # warm
    assert abs(expr.direction - 2.0) < 0.1 or abs(expr.direction + 2.0) < 0.1
    assert expr.haptic == rm.Haptic.PURR


def test_gentle_touch_is_warm_and_firm_touch_is_dynamic():
    gentle = rm.touch_expression(Gesture.HOLD, 0.15, 0.0, True)
    firm = rm.touch_expression(Gesture.HOLD, 0.95, 0.0, True)
    assert gentle.pattern == rm.Pattern.BREATHE and firm.pattern == rm.Pattern.PULSE
    assert firm.brightness > gentle.brightness and firm.speed > gentle.speed * 2
    assert gentle.rgbw[0] > 200 and firm.rgbw[0] > 200        # both stay warm
    tap_soft = rm.touch_expression(Gesture.TAP, 0.2, 0.0, True)
    tap_firm = rm.touch_expression(Gesture.TAP, 0.9, 0.0, True)
    assert tap_soft.pattern == rm.Pattern.PULSE and tap_firm.pattern == rm.Pattern.RIPPLE


def test_activation_goes_calm_then_warm():
    b = Behavior(calm_to_warm_s=2.0)
    det = MotionDetector(SENSOR_W, SENSOR_H)
    frames = make(lambda i: {"square": (70, 40, 30, 230)} if i >= 10 else {}, 80)
    stages = {}
    for t, f in frames:
        now = t / 1000
        b.on_motion(det.process(f, t), now)
        b.tick(now)
        if b.mode == Mode.AUTO_RESPONSE:
            stages.setdefault(b.auto_stage, b.expression())
    assert set(stages) == {rm.AutoStage.CALM, rm.AutoStage.WARM}
    assert stages[rm.AutoStage.CALM].rgbw[2] > stages[rm.AutoStage.CALM].rgbw[0]   # cool first
    assert stages[rm.AutoStage.WARM].rgbw[0] > stages[rm.AutoStage.WARM].rgbw[2]   # then warm


def test_touch_firmer_reads_stronger():
    soft = TouchRig().run(800, angle=0, pressure=0.05)
    firm = TouchRig().run(800, angle=0, pressure=1.0)
    assert firm.strength > soft.strength + 0.4


def test_touch_stroke_direction():
    rig = TouchRig()
    for a in range(0, 180, 10):
        rig.run(60, angle=a)
    assert rig.last.gesture == Gesture.STROKE and rig.last.travel > 60
    back = TouchRig()
    for a in range(180, 0, -10):
        back.run(60, angle=a)
    assert back.last.travel < -60
    assert rm.touch_expression(Gesture.STROKE, 0.8, 90, True, back.last.travel).speed < 0


def test_touch_hug_needs_hands_on_opposite_sides():
    broad = TouchRig().run(1500, angle=45, spread_deg=35)   # one wide hand
    assert broad.gesture == Gesture.HOLD
    hug = TouchRig().run(1500, angle=45, spread_deg=95, pressure=1.0)
    assert hug.gesture == Gesture.HUG and hug.pad_count >= 3 and hug.span >= 100


def test_touch_zones_are_reported():
    assert TouchRig().run(900, angle=0).zone == "front"
    assert TouchRig().run(900, angle=180).zone == "back"
    assert TouchRig().run(900, angle=None, crown=True).zone == "crown"


def test_rhythmic_patting_is_its_own_gesture():
    rig = TouchRig()
    for _ in range(4):
        rig.run(160, angle=90)
        rig.run(240)
    assert Gesture.PAT in rig.gestures() and rig.gestures().count(Gesture.TAP) == 1


def test_long_hold_settles():
    rig = TouchRig()
    r = rig.run(6000, angle=90)
    assert r.gesture == Gesture.HOLD and not r.sustained
    r = rig.run(6000, angle=90)
    assert r.sustained
    quiet = rm.touch_expression(Gesture.HOLD, 0.3, 90, True, sustained=True)
    normal = rm.touch_expression(Gesture.HOLD, 0.3, 90, True)
    assert quiet.speed < normal.speed and quiet.brightness < normal.brightness


def test_all_pads_at_once_is_being_picked_up():
    rig = TouchRig()
    rig.run(3000, angle=0, spread_deg=400, pressure=1.0, crown=True)   # every pad covered
    assert rig.last.handled or any(e == TouchEvent.RECALIBRATED for e, _ in rig.events)
    assert not rig.last.active


def test_touch_counts_as_presence_and_returning_skips_the_greeting():
    b = Behavior(presence_grace_s=3.0, regreet_cooldown_s=20.0, calm_to_warm_s=2.5)
    rig = TouchRig()
    b.on_touch(rig.run(900, angle=90), 10.0)
    assert b.wants_faces(10.0)                       # a hand means someone is here
    b.mode = Mode.AMBIENT
    b.last_motion_end = 30.0
    m = MotionResult(event=MotionEvent.START, active=True, level=MotionLevel.PRESENCE)
    b.on_motion(m, 35.0)
    b.tick(35.0)
    assert b.auto_stage == rm.AutoStage.WARM        # they only just left: no re-greeting


def test_touch_crown_pat_has_no_direction():
    r = TouchRig().run(900, angle=None, crown=True)
    assert r.active and r.crown and not r.has_angle


def test_touch_stuck_pad_recalibrates():
    rig = TouchRig()
    rig.run(62000, angle=90)
    assert any(e == TouchEvent.RECALIBRATED for e, _ in rig.events)
    assert not rig.last.active


def test_touch_slow_drift_is_not_a_touch():
    det = TouchDetector(TouchConfig())
    for k in range(600):
        det.process([400 - k // 20] * TouchConfig().pads, k * 20)
    assert not det.active


def test_touch_beats_motion_and_emotion():
    b = Behavior(emotion_hold_s=8.0, touch_afterglow_s=2.0)
    rig = TouchRig()
    r = rig.run(900, angle=90)
    b.on_touch(r, 1.0)
    assert b.mode == Mode.TOUCH
    assert b.on_emotion(EmotionResult("sad", 0.9, 0.8), 1.2) and b.mode == Mode.TOUCH
    rig.run(600)                                            # hand leaves
    b.on_touch(rig.last_end, 2.0)
    b.tick(3.0); assert b.mode == Mode.TOUCH                # warm afterglow
    b.tick(4.5); assert b.mode == Mode.EMOTION              # then the waiting emotion
    assert b.night is False
    b.on_touch(type(rig.last_end)(event=TouchEvent.END, gesture=Gesture.DOUBLE_TAP), 5.0)
    assert b.night is True                                  # double tap switches night mode
