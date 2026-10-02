"""Camera-based motion sensor for M (Python reference implementation).

Mirrors firmware/components/motion_sensor/src/motion_core.c step for step,
using float32 math so results match the ESP32 build exactly:

  1. block-average downscale (160x120 -> 80x60 cells)
  2. per-cell diff against a fixed-point (Q8) running background
  3. selective background update: cells that are actively moving learn slowly,
     everything else (incl. a still person or the spot they just left) learns
     at the normal rate, so no long "ghost" motion after someone walks away
  4. global-change check (lights switched on / auto-exposure jump)
  5. neighbour filter to drop sensor noise
  6. area, centroid, bbox, speed -> motion energy -> arousal (calm/lively/intense)
  7. debounced session events: START / UPDATE / END

Note: motion alone does NOT reveal emotion. It yields an *energy/arousal* level
used for the Auto-Response light, and it triggers the face scan that EchoAi
turns into an actual emotion.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from typing import Optional, Tuple

import numpy as np

from config import MotionConfig

f32 = np.float32


class MotionEvent(IntEnum):
    NONE = 0
    START = 1
    UPDATE = 2
    END = 3
    LIGHTING = 4


class MotionLevel(IntEnum):
    NONE = 0
    PRESENCE = 1
    ACTIVE = 2


class Trend(IntEnum):
    STEADY = 0
    APPROACHING = 1
    RETREATING = 2


class Arousal(IntEnum):
    CALM = 0
    LIVELY = 1
    INTENSE = 2


AROUSAL_T1 = f32(0.25)
AROUSAL_T2 = f32(0.60)
AROUSAL_HYST = f32(0.05)


@dataclass
class MotionResult:
    event: MotionEvent = MotionEvent.NONE
    level: MotionLevel = MotionLevel.NONE
    arousal: Arousal = Arousal.CALM
    trend: Trend = Trend.STEADY
    settling: bool = False     # scene was upheaved; frames ignored until it settles
    threshold: int = 0         # pixel threshold actually used this frame
    active: bool = False
    area_ratio: float = 0.0
    centroid_x: float = 0.5     # 0 = left of image, 1 = right
    centroid_y: float = 0.5     # 0 = top, 1 = bottom
    bbox: Tuple[int, int, int, int] = (0, 0, 0, 0)  # cell coords x0,y0,x1,y1 (inclusive)
    speed: float = 0.0          # smoothed, frame-widths per second
    energy: float = 0.0         # smoothed 0..1
    duration_ms: int = 0
    changed_cells: int = 0
    mask: Optional[np.ndarray] = None  # filtered cell mask (debug overlay)


def _ratio_to_cells(ratio: float, cells: int) -> int:
    return int(f32(ratio) * f32(cells) + f32(0.5))


def _adapt(bg: np.ndarray, cur8: np.ndarray, shift) -> np.ndarray:
    """bg += (cur - bg) / 2^shift, truncating toward zero (same as C)."""
    delta = cur8 - bg
    step = np.where(delta >= 0, delta >> shift, -((-delta) >> shift))
    return bg + step


def _next_arousal(current: Arousal, e: np.float32) -> Arousal:
    lvl = current
    if e >= AROUSAL_T2 + AROUSAL_HYST:
        lvl = Arousal.INTENSE
    elif e >= AROUSAL_T1 + AROUSAL_HYST and lvl == Arousal.CALM:
        lvl = Arousal.LIVELY
    if e <= AROUSAL_T1 - AROUSAL_HYST:
        lvl = Arousal.CALM
    elif e <= AROUSAL_T2 - AROUSAL_HYST and lvl == Arousal.INTENSE:
        lvl = Arousal.LIVELY
    return lvl


class MotionDetector:
    def __init__(self, width: int, height: int, cfg: Optional[MotionConfig] = None):
        self.cfg = cfg or MotionConfig()
        b = self.cfg.block
        self.width, self.height = width, height
        self.cw, self.ch = width // b, height // b
        if self.cw < 3 or self.ch < 3:
            raise ValueError("frame too small for block size")
        self.cells = self.cw * self.ch
        self.min_cells = max(1, _ratio_to_cells(self.cfg.min_area_ratio, self.cells))
        self.active_cells = _ratio_to_cells(self.cfg.active_area_ratio, self.cells)
        self.lighting_cells = _ratio_to_cells(self.cfg.lighting_ratio, self.cells)
        self.reset()

    def reset(self) -> None:
        self.bg: Optional[np.ndarray] = None
        self.prev: Optional[np.ndarray] = None
        self.frame_count = 0
        self.consec = 0
        self.active = False
        self.start_ms = 0
        self.last_motion_ms = 0
        self.has_prev_t = False
        self.prev_t = 0
        self.prev_valid = False
        self.prev_cx = f32(0)
        self.prev_cy = f32(0)
        self.speed = f32(0)
        self.energy = f32(0)
        self.area_fast = f32(0)
        self.area_slow = f32(0)
        self.noise_q8 = 0
        self.settle = 0
        self.arousal = Arousal.CALM
        self.trend = Trend.STEADY

    def recalibrate(self) -> None:
        """Call when the device or camera is moved: the scene is re-learned."""
        self.reset()

    def _downscale(self, gray: np.ndarray) -> np.ndarray:
        b = self.cfg.block
        g = gray[: self.ch * b, : self.cw * b].astype(np.int32)
        return g.reshape(self.ch, b, self.cw, b).sum(axis=(1, 3)) // (b * b)

    def process(self, gray: np.ndarray, t_ms: int) -> MotionResult:
        cfg = self.cfg
        if gray.ndim != 2 or gray.shape != (self.height, self.width):
            raise ValueError(f"expected {self.width}x{self.height} grayscale, got {gray.shape}")

        cur = self._downscale(gray)
        cur8 = cur << 8
        out = MotionResult(active=self.active, arousal=self.arousal, trend=self.trend,
                           speed=float(self.speed), energy=float(self.energy))

        if self.bg is None:
            self.bg, self.prev = cur8.copy(), cur
            self.frame_count = 1
            return out
        if self.frame_count < cfg.warmup_frames:
            # measure the sensor noise while warming up, so the threshold floor is
            # already right by the time the first events can fire
            nsum = int((np.abs(cur8 - self.bg) >> 8).sum())
            mean_q8 = (nsum << 8) // self.cells
            self.noise_q8 = self.noise_q8 + ((mean_q8 - self.noise_q8) >> 2)
            self.bg, self.prev = _adapt(self.bg, cur8, 1), cur
            self.frame_count += 1
            return out

        # 2-3. diff + selective background update. The threshold has a floor that follows
        # the measured sensor noise, so a dim, lamp-lit room does not trip false motion.
        thr = max(cfg.pixel_threshold, (self.noise_q8 * cfg.noise_gain_q4) >> 12)
        out.threshold = int(thr)
        diff = np.abs(cur8 - self.bg) >> 8
        raw = diff > thr
        raw_count = int(np.count_nonzero(raw))
        moving_cell = raw & (np.abs(cur - self.prev) > cfg.stable_threshold)
        self.bg = _adapt(self.bg, cur8, np.where(moving_cell, cfg.fg_shift, cfg.bg_shift))
        self.prev = cur
        quiet = diff[~raw]
        if quiet.size:
            mean_q8 = int(int(quiet.sum()) << 8) // int(quiet.size)
            self.noise_q8 = self.noise_q8 + ((mean_q8 - self.noise_q8) >> 5)

        # 4. whole-scene change: lights switched, or the device itself was moved. The
        # background is re-taken and the next few frames are ignored while things settle.
        lighting = raw_count > self.lighting_cells
        if lighting:
            self.settle = cfg.settle_frames
        settling = self.settle > 0
        out.settling = settling
        if lighting or settling:
            if self.settle > 0:
                self.settle -= 1
            self.bg = cur8.copy() if lighting else _adapt(self.bg, cur8, 1)
            mask = np.zeros_like(raw)
        else:
            # 5. neighbour filter
            p = np.pad(raw.astype(np.int32), 1)
            nb = (p[:-2, :-2] + p[:-2, 1:-1] + p[:-2, 2:] + p[1:-1, :-2] + p[1:-1, 2:]
                  + p[2:, :-2] + p[2:, 1:-1] + p[2:, 2:])
            mask = raw & (nb >= cfg.min_neighbors)

        count = int(np.count_nonzero(mask))
        moving = count >= self.min_cells
        area = f32(count) / f32(self.cells)
        out.changed_cells = count
        out.area_ratio = float(area)
        out.mask = mask

        dt_s = f32(0)
        if self.has_prev_t and t_ms > self.prev_t:
            dt_s = f32(t_ms - self.prev_t) / f32(1000)
        self.prev_t, self.has_prev_t = t_ms, True

        # 6. geometry + speed
        alpha_s = f32(cfg.speed_alpha)
        if moving:
            ys, xs = np.nonzero(mask)
            cxc = f32(int(xs.sum())) / f32(count)
            cyc = f32(int(ys.sum())) / f32(count)
            out.centroid_x = float((cxc + f32(0.5)) / f32(self.cw))
            out.centroid_y = float((cyc + f32(0.5)) / f32(self.ch))
            out.bbox = (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))
            out.level = MotionLevel.ACTIVE if count >= self.active_cells else MotionLevel.PRESENCE
            if self.prev_valid and dt_s > 0:
                dx, dy = cxc - self.prev_cx, cyc - self.prev_cy
                inst = np.sqrt(dx * dx + dy * dy) / f32(self.cw) / dt_s
                self.speed = f32(self.speed + alpha_s * (inst - self.speed))
            self.prev_cx, self.prev_cy, self.prev_valid = cxc, cyc, True
        else:
            self.speed = f32(self.speed + alpha_s * (f32(0) - self.speed))
            self.prev_valid = False

        # energy -> arousal
        if moving:
            a = np.minimum(area / f32(cfg.area_full_ratio), f32(1))
            s = np.minimum(self.speed / f32(cfg.speed_full), f32(1))
            target = f32(0.5) * a + f32(0.5) * s
        else:
            target = f32(0)
        self.energy = f32(self.energy + f32(cfg.energy_alpha) * (target - self.energy))
        self.arousal = _next_arousal(self.arousal, self.energy)

        # approach / retreat: a fast and a slow average of how much of the frame moves
        self.area_fast = f32(self.area_fast + f32(cfg.trend_fast_alpha) * (area - self.area_fast))
        self.area_slow = f32(self.area_slow + f32(cfg.trend_slow_alpha) * (area - self.area_slow))
        trend_d = f32(self.area_fast - self.area_slow)
        if not self.active:
            self.trend = Trend.STEADY
        elif trend_d > f32(cfg.trend_deadband):
            self.trend = Trend.APPROACHING
        elif trend_d < -f32(cfg.trend_deadband):
            self.trend = Trend.RETREATING
        else:
            self.trend = Trend.STEADY

        # 7. session state
        if moving:
            self.consec = min(self.consec + 1, 255)
            self.last_motion_ms = t_ms
        else:
            self.consec = 0

        event = MotionEvent.LIGHTING if lighting else MotionEvent.NONE
        if settling and self.active:        # the scene changed under us: end the session
            self.active, self.consec, event = False, 0, MotionEvent.END
        elif not self.active:
            if self.consec >= cfg.frames_to_trigger:
                self.active, self.start_ms, event = True, t_ms, MotionEvent.START
        elif moving:
            event = MotionEvent.UPDATE
        elif t_ms - self.last_motion_ms >= cfg.quiet_ms:
            self.active, event = False, MotionEvent.END

        out.event = event
        out.active = self.active
        out.duration_ms = (t_ms - self.start_ms) if (self.active or event == MotionEvent.END) else 0
        out.speed = float(self.speed)
        out.energy = float(self.energy)
        out.arousal = self.arousal
        out.trend = self.trend
        return out
