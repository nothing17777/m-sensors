"""M motion sensor system — PC proof of concept.

webcam -> motion sensor -> (someone present) -> face detect -> face crop
       -> EchoAi (or mock) -> emotion -> light expression (simulated LED ring)

Touch:  click and drag on the LED ring to touch the sphere (hold Shift for a hug,
        click the middle for a pat on the crown). Press [ and ] to touch more gently
        or more firmly - gentle answers warm and slow, firm answers bright and quick.

Keys (window mode):  q/Esc quit | n night mode | m motion mask | f force scan
                     1-7 set mock emotion (neutral happy sad angry surprised fearful disgusted)
                     [ / ] touch more gently / more firmly
"""
from __future__ import annotations

import argparse
import os
import sys
import time

import cv2
import numpy as np

from behavior import Behavior, Mode
from config import SENSOR_H, SENSOR_W, AppConfig, MotionConfig, TouchConfig
from emotion_providers import EMOTION_LABELS, EchoAiProvider, EmotionWorker, MockProvider
from face_detector import FaceDetector, crop_face_jpeg
import response_map as rm
from led_sim import LedRing
from motion_detector import MotionDetector, MotionEvent, Trend
from touch_detector import GESTURE_NAMES, Gesture, TouchDetector, TouchEvent, TouchPadSimulator


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--camera", type=int, default=0, help="webcam index (default 0)")
    ap.add_argument("--video", help="use a video file instead of the webcam")
    ap.add_argument("--provider", choices=["mock", "echoai"], default="mock")
    ap.add_argument("--mock-cycle", action="store_true", help="mock provider cycles through emotions")
    ap.add_argument("--headless", action="store_true", help="no window, log only")
    ap.add_argument("--max-seconds", type=float, default=0, help="stop after N seconds (0 = run forever)")
    ap.add_argument("--flip-direction", action="store_true", help="mirror which side the glow follows")
    return ap.parse_args(argv)


def log(now: float, msg: str) -> None:
    print(f"[{now:7.2f}s] {msg}", flush=True)


def main(argv=None) -> int:
    args = parse_args(argv)
    app = AppConfig(flip_direction=AppConfig.flip_direction or args.flip_direction)

    if args.provider == "echoai":
        try:
            provider = EchoAiProvider()
        except ValueError as exc:
            print(f"ERROR: {exc}. Set $env:ECHOAI_URL (and $env:ECHOAI_API_KEY) first.")
            return 2
    else:
        provider = MockProvider(cycle=args.mock_cycle)

    if args.video:
        cap = cv2.VideoCapture(args.video)
    else:
        backend = cv2.CAP_DSHOW if os.name == "nt" else cv2.CAP_ANY
        cap = cv2.VideoCapture(args.camera, backend)
    if not cap.isOpened():
        print(f"ERROR: could not open {'video ' + args.video if args.video else 'camera %d' % args.camera}")
        return 2
    video_fps = (cap.get(cv2.CAP_PROP_FPS) or 30.0) if args.video else None

    detector = MotionDetector(SENSOR_W, SENSOR_H, MotionConfig())
    faces = FaceDetector()
    worker = EmotionWorker(provider, app.scan_interval_s, app.urgent_scan_gap_s, app.error_backoff_s)
    behavior = Behavior(emotion_hold_s=app.emotion_hold_s, min_confidence=app.min_confidence,
                        presence_grace_s=app.presence_grace_s, touch_afterglow_s=app.touch_afterglow_s,
                        regreet_cooldown_s=app.regreet_cooldown_s, flip_direction=app.flip_direction)
    ring = LedRing(n=app.num_leds)
    touch = TouchDetector(TouchConfig())
    pads = TouchPadSimulator(TouchConfig())
    hand = {"angle": None, "pressure": 0.30, "spread": 18.0, "crown": False, "view_w": 0, "panel": 320}
    last_gesture = Gesture.NONE

    def on_mouse(event, x, y, flags, param):
        down = bool(flags & cv2.EVENT_FLAG_LBUTTON)
        px, py = x - hand["view_w"], y
        size = hand["panel"]
        cx = cy = size / 2
        r = size * 0.38
        dx, dy = px - cx, py - cy
        dist = (dx * dx + dy * dy) ** 0.5
        if not down or px < 0 or px > size or dist > r * 1.6:
            hand.update(angle=None, crown=False)
            return
        if dist < r * 0.45:
            hand.update(angle=None, crown=True)          # pat on the crown
        else:
            import math as _m
            hand.update(angle=(_m.degrees(_m.atan2(dy, dx)) + 90.0) % 360.0, crown=False,
                        spread=95.0 if flags & cv2.EVENT_FLAG_SHIFTKEY else 18.0)

    if not args.headless:
        cv2.namedWindow("M motion sensor")
        cv2.setMouseCallback("M motion sensor", on_mouse)

    show_mask = False
    face = None
    motion_period = 1.0 / app.motion_hz
    next_motion_t = next_face_t = 0.0
    last_mode = behavior.mode
    last_arousal = None
    last_trend = None
    last_sustained = False
    motion_starts = frames = 0
    t0 = time.monotonic()
    cam_warned = False
    last_frame_t = t0

    print(f"M motion PoC | provider={provider.name} | {'headless' if args.headless else 'window'}")
    try:
        while True:
            read_t0 = time.monotonic()
            ok, frame = cap.read()
            if not ok:
                break
            now = frames / video_fps if video_fps else time.monotonic() - t0
            frames += 1
            if args.max_seconds and now > args.max_seconds:
                break

            # A camera stuck well below the ~10 Hz motion rate (auto-exposure in low
            # light, or another process/app sharing the webcam) makes the light look
            # broken - stuck dim or jumping - even though the behaviour logic is fine.
            if not args.video and not cam_warned and frames > 5 and read_t0 - last_frame_t > 0.4:
                cam_warned = True
                print(f"WARNING: camera frame took {read_t0 - last_frame_t:.2f}s (< {1 / (read_t0 - last_frame_t):.1f} fps) - "
                      f"expected ~{app.motion_hz:.0f}+ fps. Close other apps using the webcam or add more light "
                      f"(low light forces a longer exposure per frame); the light will look stuck or jumpy until this clears.")
            last_frame_t = read_t0

            # 1. motion sensor at a fixed rate (same as firmware)
            if now >= next_motion_t:
                next_motion_t = max(next_motion_t + motion_period, now)
                gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
                sensor = cv2.resize(gray, (SENSOR_W, SENSOR_H), interpolation=cv2.INTER_AREA)
                m = detector.process(sensor, int(now * 1000))
                behavior.on_motion(m, now)
                if m.event == MotionEvent.START:
                    motion_starts += 1
                    log(now, f"MOTION START  level={m.level.name} area={m.area_ratio:.1%} x={m.centroid_x:.2f}")
                elif m.event == MotionEvent.END:
                    log(now, f"MOTION END    duration={m.duration_ms / 1000:.1f}s")
                elif m.event == MotionEvent.LIGHTING:
                    log(now, f"SCENE CHANGED (ignored, re-learning for {MotionConfig().settle_frames} frames)")
                if m.active and m.trend != last_trend:
                    if m.trend != Trend.STEADY:
                        log(now, f"person {Trend(m.trend).name.lower()}")
                    last_trend = m.trend
                if not m.active:
                    last_trend = None
                if m.active and m.arousal != last_arousal:
                    log(now, f"motion energy {m.energy:.2f} -> arousal {m.arousal.name}")
                    last_arousal = m.arousal
                if not m.active:
                    last_arousal = None

            # 2. face detection only while someone is (or just was) present
            if behavior.wants_faces(now):
                if now >= next_face_t:
                    next_face_t = now + 1.0 / app.face_hz
                    face = faces.detect(frame)
                    if face is not None:
                        behavior.on_face(now)
            else:
                face = None

            # 3. upload a face crop on event triggers only
            if face is not None and worker.ready(now, urgent=behavior.scan_due):
                jpeg = crop_face_jpeg(frame, face, app.face_crop_margin, app.face_crop_size, app.jpeg_quality)
                worker.submit(jpeg, now)
                behavior.scan_due = False
                log(now, f"face scan -> {provider.name} ({len(jpeg) / 1024:.1f} KB crop)")

            # 3b. touch sensor (mouse on the LED ring stands in for the MPR121 pads)
            tr = touch.process(pads.read(hand["angle"], hand["pressure"], hand["spread"], hand["crown"]),
                               int(now * 1000))
            behavior.on_touch(tr, now)
            if tr.event == TouchEvent.START:
                where = "the crown" if tr.crown and not tr.has_angle else f"{tr.angle:.0f} deg"
                log(now, f"TOUCH START   on the {tr.zone} ({where}) strength={tr.strength:.2f}")
            elif tr.event == TouchEvent.END:
                log(now, f"TOUCH END     gesture={GESTURE_NAMES[tr.gesture]} duration={tr.duration_ms / 1000:.1f}s")
            elif tr.handled:
                log(now, "TOUCH every pad at once -> being picked up, baselines reset")
            elif tr.recalibrated:
                log(now, "TOUCH pad stuck -> baseline reset")
            if tr.sustained and not last_sustained:
                log(now, "touch settled in -> easing down")
            last_sustained = tr.sustained
            if tr.active and tr.gesture != last_gesture:
                log(now, f"touch gesture -> {GESTURE_NAMES[tr.gesture]}")
            last_gesture = tr.gesture if tr.active else Gesture.NONE

            polled = worker.poll(now)
            if polled:
                status, payload = polled
                if status == "ok":
                    accepted = behavior.on_emotion(payload, now)
                    log(now, f"EMOTION {payload.label} conf={payload.confidence:.2f} "
                             f"latency={payload.latency_ms:.0f}ms {'' if accepted else '(ignored: low confidence)'}")
                else:
                    log(now, f"EchoAi error -> offline fallback for {app.error_backoff_s:.0f}s: {payload}")

            # 4. behaviour + lights
            behavior.tick(now)
            if behavior.mode != last_mode:
                log(now, f"mode {last_mode.value} -> {behavior.mode.value}")
                last_mode = behavior.mode
            expr = behavior.expression()
            sweep = app.night_sweep_s if now - behavior.night_since < app.night_sweep_s else None
            ring.set(expr, now, fade_s=sweep)

            if not args.headless:
                view = draw_view(frame, detector, behavior, face, show_mask)
                hand["view_w"] = view.shape[1]
                firm = "firm" if hand["pressure"] > rm.TOUCH_FIRM_THRESHOLD else "gentle"
                touch_line = (f"touch: {GESTURE_NAMES[behavior.touch.gesture]} on {behavior.touch.zone} ({firm})"
                              if behavior.touch.active else f"touch: click the ring ({firm})")
                stage = f" ({behavior.auto_stage.value})" if behavior.mode.value == "auto_response" else ""
                lines = [f"mode: {behavior.mode.value}{stage}{' (night)' if behavior.night else ''}",
                         touch_line,
                         f"light: {expr.pattern.value} bri={expr.brightness}",
                         f"arousal: {behavior.motion.arousal.name}  energy={behavior.motion.energy:.2f}",
                         f"emotion: {behavior.emotion.label if behavior.emotion else '-'}",
                         f"EchoAi: {provider.name} {'online' if worker.online else 'OFFLINE'}"]
                panel = ring.draw(320, lines)
                h = max(view.shape[0], panel.shape[0])
                canvas = np.zeros((h, view.shape[1] + panel.shape[1], 3), np.uint8)
                canvas[: view.shape[0], : view.shape[1]] = view
                canvas[: panel.shape[0], view.shape[1]:] = panel
                cv2.imshow("M motion sensor", canvas)
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
                elif key in (ord("["), ord("]")):
                    step = -0.1 if key == ord("[") else 0.1
                    hand["pressure"] = max(0.05, min(1.0, round(hand["pressure"] + step, 2)))
                    log(now, f"touch pressure {hand['pressure']:.2f} "
                             f"({'firm' if hand['pressure'] > rm.TOUCH_FIRM_THRESHOLD else 'gentle'})")
                elif ord("1") <= key <= ord("7") and isinstance(provider, MockProvider):
                    provider.set_label(EMOTION_LABELS[key - ord("1")])
                    log(now, f"mock emotion set to {provider.label}")
    except KeyboardInterrupt:
        pass
    finally:
        cap.release()
        worker.shutdown()
        if not args.headless:
            cv2.destroyAllWindows()

    lat = sorted(worker.latencies_ms)
    lat_txt = (f"avg {sum(lat) / len(lat):.0f} ms, p95 {lat[int(0.95 * (len(lat) - 1))]:.0f} ms"
               if lat else "n/a")
    print(f"\nSUMMARY frames={frames} motion_starts={motion_starts} scans={worker.submitted} "
          f"ok={worker.succeeded} errors={worker.failed} latency={lat_txt}")
    return 0


def draw_view(frame, detector: MotionDetector, behavior: Behavior, face, show_mask: bool):
    view = frame.copy()
    h, w = view.shape[:2]
    m = behavior.motion
    sx, sy = w / detector.cw, h / detector.ch
    if show_mask and m.mask is not None:
        overlay = cv2.resize(m.mask.astype(np.uint8) * 255, (w, h), interpolation=cv2.INTER_NEAREST)
        view[overlay > 0] = (0.5 * view[overlay > 0] + (0, 0, 127)).astype(np.uint8)
    if m.level.value:
        x0, y0, x1, y1 = m.bbox
        cv2.rectangle(view, (int(x0 * sx), int(y0 * sy)), (int((x1 + 1) * sx), int((y1 + 1) * sy)), (0, 200, 255), 2)
        cv2.circle(view, (int(m.centroid_x * w), int(m.centroid_y * h)), 6, (0, 200, 255), -1)
    if face is not None:
        x, y, fw, fh = face
        cv2.rectangle(view, (x, y), (x + fw, y + fh), (80, 255, 80), 2)
    cv2.putText(view, f"motion: {'ACTIVE' if m.active else 'none'} {m.level.name} area={m.area_ratio:.1%}",
                (10, 25), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)
    return view


if __name__ == "__main__":
    sys.exit(main())
