"""Capacitive touch sensor for M (Python reference implementation).

Mirrors firmware/components/touch_sensor/src/touch_core.c step for step
(verified by tools/parity_touch.py):

  1. per pad: delta = baseline - reading (MPR121 counts drop when touched)
  2. debounced press/release with hysteresis
  3. the baseline only tracks pads that are NOT being touched, and a pad held
     longer than stuck_ms re-baselines itself (wet shell, object leaning on it)
  4. session aggregation: which pads, where the hand is around the sphere,
     how far it travelled, how firm it is
  5. gestures: tap, double tap, hold, stroke, hug

TouchPadSimulator turns a hand position into realistic pad readings so the whole
pipeline can be exercised without the MPR121 hardware.
"""
from __future__ import annotations

import math
import random
from dataclasses import dataclass, field
from enum import IntEnum
from typing import List, Optional, Sequence

from config import TouchConfig


class TouchEvent(IntEnum):
    NONE = 0
    START = 1
    UPDATE = 2
    END = 3
    RECALIBRATED = 4


class Gesture(IntEnum):
    NONE = 0
    TAP = 1
    DOUBLE_TAP = 2
    HOLD = 3
    STROKE = 4
    HUG = 5
    PAT = 6          # rhythmic repeated taps


GESTURE_NAMES = ["none", "tap", "double tap", "hold", "stroke", "hug", "pat"]
# where a pad sits on the body, so the rest of the system need not know pad numbers
ZONE_IDS = ["none", "front", "right", "back", "left", "crown", "base"]


@dataclass
class TouchResult:
    event: TouchEvent = TouchEvent.NONE
    gesture: Gesture = Gesture.NONE
    active: bool = False
    pad_mask: int = 0
    pad_count: int = 0
    has_angle: bool = False
    angle: float = 0.0        # degrees around the sphere, 0 = camera side
    travel: float = 0.0       # signed degrees travelled this session
    span: float = 0.0         # how far the pressed pads spread around the shell
    strength: float = 0.0     # 0..1
    duration_ms: int = 0
    recalibrated: bool = False
    crown: bool = False
    zone: str = "none"        # zone of the most firmly touched pad
    zone_mask: int = 0
    sustained: bool = False   # held long enough to count as settling in
    handled: bool = False     # every pad at once: the device is being carried       # a pad with no angle (top of the sphere) is touched


def _pad_span(angles: List[float]) -> float:
    """How far the pressed pads spread around the shell: 0 for one pad, ~180 for
    two opposite sides. One broad hand covers neighbours; a hug does not."""
    if len(angles) < 2:
        return 0.0
    a = sorted(angles)
    max_gap = a[0] + 360.0 - a[-1]
    for i in range(1, len(a)):
        max_gap = max(max_gap, a[i] - a[i - 1])
    return 360.0 - max_gap


def _wrap180(d: float) -> float:
    while d > 180.0:
        d -= 360.0
    while d < -180.0:
        d += 360.0
    return d


class TouchDetector:
    def __init__(self, cfg: Optional[TouchConfig] = None):
        self.cfg = cfg or TouchConfig()
        self.n = self.cfg.pads
        self.baseline = [0] * self.n
        self.initialized = False
        self.delta = [0] * self.n
        self.reset()

    def reset(self) -> None:
        n = self.n
        self.pressed = [False] * n
        self.press_cnt = [0] * n
        self.release_cnt = [0] * n
        self.press_since = [0] * n
        self.active = False
        self.start_ms = 0
        self.angle = 0.0
        self.prev_angle = 0.0
        self.travel = 0.0
        self.has_angle = False
        self.had_angle = False
        self.crown = False
        self.last_tap_end_ms = 0
        self.tap_times: List[int] = []
        self.all_pads_since = 0
        self.had_tap = False
        self.gesture = Gesture.NONE

    def recalibrate(self) -> None:
        self.reset()
        self.initialized = False

    def process(self, values: Sequence[int], t_ms: int) -> TouchResult:
        c = self.cfg
        out = TouchResult(active=self.active)

        if not self.initialized:
            self.baseline = [int(v) << 8 for v in values[: self.n]]
            self.initialized = True
            return out

        recalibrated = False
        mask = count = max_delta = zone_mask = 0
        zone = "none"
        sx = sy = wsum = 0.0
        pressed_angles: List[float] = []
        crown = False

        for i in range(self.n):
            d = (self.baseline[i] >> 8) - int(values[i])
            self.delta[i] = max(0, min(65535, d))

            if not self.pressed[i]:
                if self.delta[i] > c.touch_threshold:
                    self.press_cnt[i] += 1
                    if self.press_cnt[i] >= c.press_frames:
                        self.pressed[i] = True
                        self.press_since[i] = t_ms
                        self.release_cnt[i] = 0
                else:
                    self.press_cnt[i] = 0
                # arithmetic shift, matching the C ">>" on a signed int32
                self.baseline[i] += ((int(values[i]) << 8) - self.baseline[i]) >> c.baseline_shift
            else:
                if self.delta[i] < c.release_threshold:
                    self.release_cnt[i] += 1
                    if self.release_cnt[i] >= c.release_frames:
                        self.pressed[i] = False
                        self.press_cnt[i] = 0
                else:
                    self.release_cnt[i] = 0
                if self.pressed[i] and t_ms - self.press_since[i] >= c.stuck_ms:
                    self.baseline[i] = int(values[i]) << 8
                    self.pressed[i] = False
                    self.press_cnt[i] = self.release_cnt[i] = 0
                    self.delta[i] = 0
                    recalibrated = True

            if self.pressed[i]:
                mask |= 1 << i
                count += 1
                if self.delta[i] > max_delta:
                    max_delta = self.delta[i]
                    zone = c.pad_zones[i]
                zone_mask |= 1 << ZONE_IDS.index(c.pad_zones[i])
                ang = c.pad_angles[i]
                if ang is None:
                    crown = True
                else:
                    w = float(self.delta[i])
                    pressed_angles.append(float(ang))
                    rad = math.radians(ang)
                    sx += w * math.cos(rad)
                    sy += w * math.sin(rad)
                    wsum += w

        # Every pad at once for a while is not a hug: the device is being picked up or
        # carried. Re-learn the baseline instead of holding a fake touch forever.
        handled = False
        if count >= self.n > 1:
            if self.all_pads_since == 0:
                self.all_pads_since = t_ms
            elif t_ms - self.all_pads_since >= c.all_pads_ms:
                for i in range(self.n):
                    self.baseline[i] = int(values[i]) << 8
                    self.pressed[i] = False
                    self.press_cnt[i] = self.release_cnt[i] = 0
                    self.delta[i] = 0
                mask = count = max_delta = zone_mask = 0
                zone = "none"
                pressed_angles.clear()
                sx = sy = wsum = 0.0
                self.all_pads_since = 0
                recalibrated = handled = True
        else:
            self.all_pads_since = 0

        has_angle = wsum > 0.0
        angle = self.angle
        if has_angle:
            angle = math.degrees(math.atan2(sy, sx))
            if angle < 0.0:
                angle += 360.0

        event = TouchEvent.RECALIBRATED if recalibrated else TouchEvent.NONE
        if not self.active and count > 0:
            self.active = True
            self.start_ms = t_ms
            self.travel = 0.0
            self.gesture = Gesture.NONE
            self.had_angle = False
            event = TouchEvent.START
        elif self.active and count > 0:
            if event != TouchEvent.RECALIBRATED:
                event = TouchEvent.UPDATE

        if self.active and has_angle:
            if self.had_angle:
                self.travel += _wrap180(angle - self.prev_angle)
            self.prev_angle = angle
            self.had_angle = True
        self.angle, self.has_angle, self.crown = angle, has_angle, crown

        duration = t_ms - self.start_ms if self.active else 0

        span = _pad_span(pressed_angles)
        if self.active and count > 0:
            if count >= c.hug_pads and span >= c.hug_span_deg and duration >= c.hug_min_ms:
                self.gesture = Gesture.HUG
            elif abs(self.travel) >= c.stroke_min_deg and self.gesture != Gesture.HUG:
                self.gesture = Gesture.STROKE
            elif duration >= c.hold_min_ms and self.gesture in (Gesture.NONE, Gesture.HOLD):
                self.gesture = Gesture.HOLD

        if self.active and count == 0:
            self.active = False
            event = TouchEvent.END
            if duration <= c.tap_max_ms and abs(self.travel) < c.stroke_min_deg and self.gesture == Gesture.NONE:
                # keep a short history of taps so repeated taps read as patting
                if self.tap_times and t_ms - self.tap_times[-1] > c.double_tap_gap_ms:
                    self.tap_times.clear()
                if len(self.tap_times) >= 8:
                    self.tap_times.clear()
                self.tap_times.append(t_ms)
                in_window = (len(self.tap_times) >= c.pat_taps
                             and t_ms - self.tap_times[-c.pat_taps] <= c.pat_window_ms)
                if in_window:
                    self.gesture = Gesture.PAT
                elif len(self.tap_times) >= 2 and t_ms - self.last_tap_end_ms <= c.double_tap_gap_ms:
                    self.gesture = Gesture.DOUBLE_TAP
                else:
                    self.gesture = Gesture.TAP
                self.had_tap = True
                self.last_tap_end_ms = t_ms
            else:
                self.had_tap = False
                self.tap_times.clear()

        out.event = event
        out.gesture = self.gesture
        out.active = self.active
        out.pad_mask = mask
        out.pad_count = count
        out.has_angle = has_angle
        out.angle = angle
        out.travel = self.travel
        out.span = span
        out.crown = crown
        out.duration_ms = duration
        out.zone = zone
        out.zone_mask = zone_mask
        out.handled = handled
        out.sustained = (self.active and duration >= c.sustained_ms
                         and self.gesture in (Gesture.HOLD, Gesture.HUG))
        out.recalibrated = recalibrated
        out.strength = min(1.0, max_delta / float(c.strength_full or 1))
        if not self.active and event != TouchEvent.END:
            out.gesture = Gesture.NONE
        return out


class TouchPadSimulator:
    """Turns a hand position into MPR121-like readings (lower value = touched)."""

    def __init__(self, cfg: Optional[TouchConfig] = None, base: int = 400, noise: int = 1, seed: int = 3):
        self.cfg = cfg or TouchConfig()
        self.base = base
        self.noise = noise
        self.rng = random.Random(seed)

    def read(self, angle: Optional[float] = None, pressure: float = 0.7,
             spread_deg: float = 18.0, crown: bool = False) -> List[int]:
        """angle=None means no hand. spread_deg wide (e.g. 90) simulates a hug."""
        vals = []
        depth = 30 + 90 * max(0.0, min(1.0, pressure))
        for i in range(self.cfg.pads):
            pad = self.cfg.pad_angles[i]
            d = 0.0
            if pad is None:
                if crown:
                    d = depth
            elif angle is not None:
                sep = abs(_wrap180(pad - angle))
                d = depth * math.exp(-(sep * sep) / (2 * spread_deg * spread_deg))
            noise = self.rng.randint(-self.noise, self.noise) if self.noise else 0
            vals.append(max(0, int(round(self.base - d + noise))))
        return vals
