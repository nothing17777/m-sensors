"""M MOTION sensor - standalone.

Runs only the motion sensor: webcam -> motion -> face -> emotion -> light.
No touch. If you have no webcam, use --video or --selftest.

    python run_motion.py                  webcam
    python run_motion.py --camera 1       a different webcam
    python run_motion.py --video clip.avi a video file instead
    python run_motion.py --selftest       no window, no camera: scripted scene + results

Emotions start OFF, so you can see the motion behaviour on its own: blink when nobody is
there, a cool calm glow when you walk in, warming up when you stay, brighter and faster the
more you move. Press 1-7 to switch the stand-in EchoAi on and pick what it reads.

Keys:  q quit   n night mode   m show what counts as movement   f force a face scan
       1-7 stand-in emotion (neutral happy sad angry surprised fearful disgusted)
       0   emotions off again, back to pure motion
"""
from __future__ import annotations

import argparse
import os
import sys
import time

import cv2
import numpy as np

import response_map as rm
from behavior import Behavior
from config import SENSOR_H, SENSOR_W, AppConfig, MotionConfig
from emotion_providers import EMOTION_LABELS, EchoAiProvider, EmotionWorker, MockProvider
from face_detector import FaceDetector, crop_face_jpeg
from led_sim import LedRing
from motion_detector import MotionDetector, MotionEvent, MotionLevel, Trend

WINDOW = "M motion sensor"
PANEL = 320
RECENT = []


def log(t: float, msg: str) -> None:
    print(f"[{t:7.2f}s] {msg}", flush=True)
    RECENT.append(msg)
    del RECENT[:-6]


def draw_camera(frame, detector, behavior, face, show_mask):
    """The webcam view with the motion overlay drawn on top."""
    view = frame.copy()
    h, w = view.shape[:2]
    motion = behavior.motion
    sx, sy = w / detector.cw, h / detector.ch
    if show_mask and motion.mask is not None:
        overlay = cv2.resize(motion.mask.astype(np.uint8) * 255, (w, h), interpolation=cv2.INTER_NEAREST)
        view[overlay > 0] = (0.5 * view[overlay > 0] + (0, 0, 127)).astype(np.uint8)
    if motion.level != MotionLevel.NONE:
        x0, y0, x1, y1 = motion.bbox
        cv2.rectangle(view, (int(x0 * sx), int(y0 * sy)), (int((x1 + 1) * sx), int((y1 + 1) * sy)),
                      (0, 200, 255), 2)
        cv2.circle(view, (int(motion.centroid_x * w), int(motion.centroid_y * h)), 6, (0, 200, 255), -1)
    if face is not None:
        x, y, fw, fh = face
        cv2.rectangle(view, (x, y), (x + fw, y + fh), (80, 255, 80), 2)
    level = ["calm", "lively", "intense"][motion.arousal] if motion.active else "none"
    cv2.putText(view, f"motion: {level}  energy={motion.energy:.2f}  area={motion.area_ratio:.1%}",
                (10, 25), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)
    return view


def describe(behavior, expr, worker, provider, emotion_on) -> list:
    m = behavior.motion
    stage = f" ({behavior.auto_stage.value})" if behavior.mode.value == "auto_response" else ""
    level = ["calm", "lively", "intense"][m.arousal] if m.active else "nobody moving"
    trend = "" if (not m.active or m.trend == Trend.STEADY) else f", {Trend(m.trend).name.lower()}"
    if not emotion_on:
        emotion_text = "off (press 1-7 to switch on)"
    elif behavior.emotion:
        emotion_text = f"{behavior.emotion.label}  ({provider.name} {'online' if worker.online else 'OFFLINE'})"
    else:
        emotion_text = f"waiting for a face ({provider.name})"
    return [
        f"mode: {behavior.mode.value}{stage}{' (night)' if behavior.night else ''}",
        f"movement: {level}{trend}",
        f"energy: {m.energy:.2f}  speed {m.speed:.2f}  area {m.area_ratio:.1%}",
        f"light: {expr.pattern.value} bri={expr.brightness}",
        f"emotion: {emotion_text}",
        "keys: 1-7 emotion  0 off  m mask  n night  q quit",
    ]


def run_window(args, provider) -> int:
    app = AppConfig()
    if args.video:
        cap = cv2.VideoCapture(args.video)
    else:
        cap = cv2.VideoCapture(args.camera, cv2.CAP_DSHOW if os.name == "nt" else cv2.CAP_ANY)
    if not cap.isOpened():
        print(f"ERROR: could not open {'video ' + args.video if args.video else f'camera {args.camera}'}.")
        print("Try --camera 1, close other apps using the webcam, or run with --selftest.")
        return 2
    video_fps = (cap.get(cv2.CAP_PROP_FPS) or 30.0) if args.video else None

    detector = MotionDetector(SENSOR_W, SENSOR_H, MotionConfig())
    faces = FaceDetector()
    worker = EmotionWorker(provider, app.scan_interval_s, app.urgent_scan_gap_s, app.error_backoff_s)
    behavior = Behavior(app.emotion_hold_s, app.min_confidence, app.presence_grace_s,
                        regreet_cooldown_s=app.regreet_cooldown_s)
    ring = LedRing(n=app.num_leds)

    show_mask, face, frames = True, None, 0
    emotion_on = args.emotion or args.provider == "echoai"
    next_motion_t = next_face_t = 0.0
    last_mode, last_arousal, last_trend = behavior.mode, None, None
    t0 = time.monotonic()
    cv2.namedWindow(WINDOW)
    print(f"M motion sensor | provider={provider.name} | q to quit", flush=True)

    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                break
            now = frames / video_fps if video_fps else time.monotonic() - t0
            frames += 1

            if now >= next_motion_t:
                next_motion_t = max(next_motion_t + 1.0 / app.motion_hz, now)
                sensor = cv2.resize(cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY), (SENSOR_W, SENSOR_H),
                                    interpolation=cv2.INTER_AREA)
                m = detector.process(sensor, int(now * 1000))
                behavior.on_motion(m, now)
                if m.event == MotionEvent.START:
                    log(now, f"MOTION START  {m.level.name.lower()}  area={m.area_ratio:.1%}  x={m.centroid_x:.2f}")
                elif m.event == MotionEvent.END:
                    log(now, f"MOTION END    duration={m.duration_ms / 1000:.1f}s")
                elif m.event == MotionEvent.LIGHTING:
                    log(now, "SCENE CHANGED, re-learning the view")
                if m.active and m.arousal != last_arousal:
                    log(now, f"movement {['calm', 'lively', 'intense'][m.arousal]} ({m.energy:.2f})")
                    last_arousal = m.arousal
                if m.active and m.trend != last_trend and m.trend != Trend.STEADY:
                    log(now, f"person {Trend(m.trend).name.lower()}")
                    last_trend = m.trend
                if not m.active:
                    last_arousal = last_trend = None

            if emotion_on and behavior.wants_faces(now):
                if now >= next_face_t:
                    next_face_t = now + 1.0 / app.face_hz
                    face = faces.detect(frame)
                    if face is not None:
                        behavior.on_face(now)
            else:
                face = None

            if emotion_on and face is not None and worker.ready(now, urgent=behavior.scan_due):
                jpeg = crop_face_jpeg(frame, face, app.face_crop_margin, app.face_crop_size, app.jpeg_quality)
                worker.submit(jpeg, now)
                behavior.scan_due = False
                log(now, f"face scan -> {provider.name} ({len(jpeg) / 1024:.1f} KB)")

            polled = worker.poll(now)
            if polled:
                status, payload = polled
                if status == "ok":
                    behavior.on_emotion(payload, now)
                    log(now, f"EMOTION {payload.label} ({payload.latency_ms:.0f} ms)")
                else:
                    log(now, f"EchoAi offline, lights keep following movement: {payload}")

            behavior.tick(now)
            if behavior.mode != last_mode:
                log(now, f"{last_mode.value} -> {behavior.mode.value}")
                last_mode = behavior.mode
            expr = behavior.expression()
            sweep = app.night_sweep_s if now - behavior.night_since < app.night_sweep_s else None
            ring.set(expr, now, fade_s=sweep)
            view = draw_camera(frame, detector, behavior, face, show_mask)
            panel = ring.draw(PANEL, describe(behavior, expr, worker, provider, emotion_on))
            h = max(view.shape[0], panel.shape[0])
            canvas = np.zeros((h, view.shape[1] + panel.shape[1], 3), np.uint8)
            canvas[: view.shape[0], : view.shape[1]] = view
            canvas[: panel.shape[0], view.shape[1]:] = panel
            cv2.imshow(WINDOW, canvas)

            key = cv2.waitKey(1) & 0xFF
            if key in (ord("q"), 27):
                break
            if key == ord("n"):
                behavior.toggle_night(now)
            elif key == ord("m"):
                show_mask = not show_mask
            elif key == ord("f"):
                behavior.scan_due = True
                worker._last_submit = float("-inf")
            elif key == ord("0"):
                emotion_on = False
                behavior.emotion = None
                behavior.emotion_until = 0.0
                log(now, "emotions off: showing motion only")
            elif ord("1") <= key <= ord("7") and isinstance(provider, MockProvider):
                provider.set_label(EMOTION_LABELS[key - ord("1")])
                if not emotion_on:
                    emotion_on = True
                    log(now, "stand-in EchoAi switched on")
                behavior.scan_due = True
                worker._last_submit = float("-inf")
                log(now, f"stand-in emotion set to {provider.label}")
    except KeyboardInterrupt:
        pass
    finally:
        cap.release()
        worker.shutdown()
        cv2.destroyAllWindows()
    print(f"\nSUMMARY frames={frames} scans={worker.submitted} ok={worker.succeeded} errors={worker.failed}")
    return 0


def run_selftest(args) -> int:
    """Runs a scripted scene through the sensor and prints what it found. No camera."""
    import synthetic as syn
    detector = MotionDetector(SENSOR_W, SENSOR_H, MotionConfig())
    behavior = Behavior()
    starts = ends = 0
    arousals, trends = set(), set()
    print("Motion sensor self-test (no camera): idle, walking, waving, lights on, noise\n")
    for t, frame in syn.scenario():
        m = detector.process(frame, t)
        behavior.on_motion(m, t / 1000)
        behavior.tick(t / 1000)
        if m.event == MotionEvent.START:
            starts += 1
            print(f"  {t / 1000:6.1f}s  motion starts   area={m.area_ratio:.1%} x={m.centroid_x:.2f}")
        elif m.event == MotionEvent.END:
            ends += 1
            print(f"  {t / 1000:6.1f}s  motion ends     after {m.duration_ms / 1000:.1f}s")
        elif m.event == MotionEvent.LIGHTING:
            print(f"  {t / 1000:6.1f}s  scene changed   ignored, re-learning")
        if m.active:
            arousals.add(["calm", "lively", "intense"][m.arousal])
            trends.add(Trend(m.trend).name.lower())
    print(f"\nsessions: {starts} started, {ends} ended")
    print(f"movement levels seen: {', '.join(sorted(arousals))}")
    print(f"direction seen: {', '.join(sorted(trends))}")
    ok = starts >= 3 and starts == ends
    print("motion sensor OK" if ok else "UNEXPECTED: sessions did not balance")
    return 0 if ok else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--camera", type=int, default=0)
    ap.add_argument("--video")
    ap.add_argument("--provider", choices=["mock", "echoai"], default="mock")
    ap.add_argument("--emotion", action="store_true", help="start with the stand-in EchoAi switched on")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return run_selftest(args)
    if args.provider == "echoai":
        try:
            provider = EchoAiProvider()
        except ValueError as exc:
            print(f"ERROR: {exc}. Set $env:ECHOAI_URL (and $env:ECHOAI_API_KEY) first.")
            return 2
    else:
        provider = MockProvider()
    return run_window(args, provider)


if __name__ == "__main__":
    sys.exit(main())
