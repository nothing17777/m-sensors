"""M TOUCH sensor - standalone.

Runs only the touch sensor. No camera, no webcam, no network. If the motion side
gives you trouble, this still works.

    python run_touch.py              window: drag on the ring to touch the sphere
    python run_touch.py --selftest   no window: runs every gesture and prints the log

Mouse:  drag on the ring       touch that spot        drag around      stroke
        click the middle       pat on the crown       Shift + drag     hug
Keys:   [ ]  touch softer / firmer      1-8  press that pad      0  crown pad
        space  let go                   n    night mode          q  quit
"""
from __future__ import annotations

import argparse
import sys
import time

import cv2
import numpy as np

import response_map as rm
from behavior import Behavior, Mode
from config import AppConfig, TouchConfig
from led_sim import LedRing
from touch_detector import GESTURE_NAMES, Gesture, TouchDetector, TouchEvent, TouchPadSimulator

WINDOW = "M touch sensor"
PANEL = 420


RECENT = []


def log(t: float, msg: str) -> None:
    print(f"[{t:7.2f}s] {msg}", flush=True)
    RECENT.append(msg)
    del RECENT[:-6]


def describe(expr, touch, behavior, pressure) -> list:
    firm = "firm" if pressure > rm.TOUCH_FIRM_THRESHOLD else "gentle"
    gesture = GESTURE_NAMES[touch.gesture] if touch.active else "-"
    where = touch.zone if touch.active else "-"
    if touch.active and touch.has_angle:
        where += f" ({touch.angle:.0f} deg)"
    return [
        f"mode: {behavior.mode.value}{' (night)' if behavior.night else ''}",
        f"gesture: {gesture}{' settled' if touch.sustained else ''}",
        f"where: {where}",
        f"pressure: {firm} {pressure:.2f}  strength {touch.strength:.2f}",
        f"light: {expr.pattern.value} bri={expr.brightness}",
        "drag the ring | shift hug | middle crown | [ ] pressure | q quit",
    ]


def run_window(args) -> int:
    cfg = TouchConfig()
    app = AppConfig()
    detector = TouchDetector(cfg)
    pads = TouchPadSimulator(cfg)
    behavior = Behavior(touch_afterglow_s=app.touch_afterglow_s)
    ring = LedRing(n=app.num_leds)

    hand = {"angle": None, "pressure": 0.30, "spread": 18.0, "crown": False}
    key_pad = {"angle": None, "crown": False}
    recent = []

    def on_mouse(event, x, y, flags, param):
        if not (flags & cv2.EVENT_FLAG_LBUTTON):
            hand.update(angle=None, crown=False)
            return
        cx = cy = PANEL / 2
        dx, dy = x - cx, y - cy
        dist = (dx * dx + dy * dy) ** 0.5
        r = PANEL * 0.38
        if dist > r * 1.6:
            hand.update(angle=None, crown=False)
        elif dist < r * 0.42:
            hand.update(angle=None, crown=True)
        else:
            import math
            hand.update(angle=(math.degrees(math.atan2(dy, dx)) + 90.0) % 360.0, crown=False,
                        spread=95.0 if flags & cv2.EVENT_FLAG_SHIFTKEY else 18.0)

    cv2.namedWindow(WINDOW)
    cv2.setMouseCallback(WINDOW, on_mouse)
    print("M touch sensor | drag on the ring | [ ] pressure | q quit", flush=True)

    t0 = time.monotonic()
    last_gesture = Gesture.NONE
    last_sustained = False
    while True:
        now = time.monotonic() - t0
        angle = hand["angle"] if hand["angle"] is not None else key_pad["angle"]
        crown = hand["crown"] or key_pad["crown"]
        r = detector.process(pads.read(angle, hand["pressure"], hand["spread"], crown), int(now * 1000))
        behavior.on_touch(r, now)
        behavior.tick(now)

        if r.event == TouchEvent.START:
            log(now, f"TOUCH START   on the {r.zone} strength={r.strength:.2f}")
        elif r.event == TouchEvent.END:
            log(now, f"TOUCH END     gesture={GESTURE_NAMES[r.gesture]} duration={r.duration_ms / 1000:.1f}s")
        elif r.handled:
            log(now, "every pad at once -> being picked up, baselines reset")
        if r.active and r.gesture != last_gesture:
            log(now, f"touch gesture -> {GESTURE_NAMES[r.gesture]}")
        last_gesture = r.gesture if r.active else Gesture.NONE
        if r.sustained and not last_sustained:
            log(now, "touch settled in -> easing down")
        last_sustained = r.sustained

        expr = behavior.expression()
        sweep = app.night_sweep_s if now - behavior.night_since < app.night_sweep_s else None
        ring.set(expr, now, fade_s=sweep)
        cv2.imshow(WINDOW, ring.draw(PANEL, describe(expr, r, behavior, hand["pressure"])))

        key = cv2.waitKey(20) & 0xFF
        if key in (ord("q"), 27):
            break
        if key in (ord("["), ord("]")):
            hand["pressure"] = max(0.05, min(1.0, round(hand["pressure"] + (0.1 if key == ord("]") else -0.1), 2)))
            log(now, f"pressure {hand['pressure']:.2f} "
                     f"({'firm' if hand['pressure'] > rm.TOUCH_FIRM_THRESHOLD else 'gentle'})")
        elif key == ord("n"):
            behavior.toggle_night(now)
        elif key == ord(" "):
            key_pad.update(angle=None, crown=False)
        elif key == ord("0"):
            key_pad.update(angle=None, crown=True)
        elif ord("1") <= key <= ord("8"):
            key_pad.update(angle=cfg.pad_angles[key - ord("1")], crown=False)

    cv2.destroyAllWindows()
    return 0


def run_selftest(args) -> int:
    """Drives every gesture with simulated pad readings and prints what came out."""
    cfg = TouchConfig()
    detector = TouchDetector(cfg)
    pads = TouchPadSimulator(cfg)
    behavior = Behavior(touch_afterglow_s=2.0)
    clock = {"t": 0}
    seen = []

    zone_seen = {"z": "none"}

    def feed(ms, label=None, **kw):
        for _ in range(0, ms, 20):
            r = detector.process(pads.read(**kw), clock["t"])
            clock["t"] += 20
            behavior.on_touch(r, clock["t"] / 1000)
            behavior.tick(clock["t"] / 1000)
            if r.active and r.zone != "none":
                zone_seen["z"] = r.zone
            if r.event == TouchEvent.END:
                seen.append(GESTURE_NAMES[r.gesture])
                expr = behavior.expression()
                print(f"  {label or '':<22} -> {GESTURE_NAMES[r.gesture]:<11} "
                      f"zone={zone_seen['z']:<6} light={expr.pattern.value} brightness={expr.brightness}")
                zone_seen["z"] = "none"
        return r

    print("Touch sensor self-test (no hardware, no camera)\n")
    feed(400, None)
    feed(200, "quick poke", angle=90); feed(800, "quick poke")
    feed(200, None, angle=90); feed(200, None); feed(200, None, angle=90); feed(800, "two quick pokes")
    for _ in range(4):
        feed(160, None, angle=45); feed(240, None)
    feed(800, "rhythmic patting")
    r = feed(1500, None, angle=180)
    print(f"  {'hand resting':<22} -> {GESTURE_NAMES[r.gesture]:<11} zone={r.zone:<6} "
          f"strength={r.strength:.2f}")
    r = feed(11000, None, angle=180)
    print(f"  {'...for a long time':<22} -> settled={r.sustained}")
    feed(600, "hand resting")
    for a in range(0, 200, 10):
        feed(60, None, angle=a)
    feed(600, "hand sliding around")
    feed(1500, None, angle=45, spread_deg=95, pressure=1.0)
    feed(600, "both hands (hug)")
    feed(900, None, angle=None, crown=True); feed(600, "hand on the crown")
    r = feed(3000, None, angle=0, spread_deg=400, pressure=1.0, crown=True)
    print(f"  {'picked up (all pads)':<22} -> handled, baselines reset")
    feed(600, None)

    expected = {"tap", "double tap", "pat", "hold", "stroke", "hug"}
    missing = expected - set(seen)
    print(f"\ngestures seen: {', '.join(sorted(set(seen)))}")
    if missing:
        print(f"MISSING: {', '.join(sorted(missing))}")
        return 1
    print("touch sensor OK")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true", help="no window: run every gesture and print the result")
    args = ap.parse_args(argv)
    return run_selftest(args) if args.selftest else run_window(args)


if __name__ == "__main__":
    sys.exit(main())
