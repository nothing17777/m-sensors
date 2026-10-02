"""On-screen simulator of the SK6812 RGBW ring (same patterns as firmware led_render.c)."""
from __future__ import annotations

import math
from typing import Optional

import cv2
import numpy as np

from response_map import Expression, Pattern


def render_pattern(expr: Expression, t: float, direction: float, n: int, front_index: int) -> np.ndarray:
    """Returns (n, 4) float array of RGBW 0..255."""
    idx = np.arange(n, dtype=np.float64)
    ph = t * expr.speed
    center = front_index + direction * (n / 4.0)
    dist = np.abs(((idx - center + n / 2.0) % n) - n / 2.0)
    p = expr.pattern
    if p == Pattern.SOLID:
        lvl = np.ones(n)
    elif p == Pattern.BLINK:
        frac = ph % 1.0
        g = max(math.exp(-0.5 * (frac / 0.06) ** 2), math.exp(-0.5 * ((1 - frac) / 0.06) ** 2))
        lvl = np.full(n, 0.14 + 0.86 * g)
    elif p == Pattern.BREATHE:
        lvl = np.full(n, 0.15 + 0.85 * (0.5 - 0.5 * math.cos(2 * math.pi * ph)))
    elif p == Pattern.GLOW_TOWARD:
        sigma = n / 8.0
        lvl = 0.12 + 0.88 * np.exp(-(dist ** 2) / (2 * sigma ** 2)) * (0.8 + 0.2 * math.sin(2 * math.pi * ph))
    elif p == Pattern.RIPPLE:
        lvl = 0.25 + 0.75 * (0.5 + 0.5 * np.cos(2 * math.pi * (dist / (n / 2.0) * 1.5 - ph)))
    elif p == Pattern.SPARKLE:
        step = int(t * max(expr.speed, 0.01) * 6)
        h = ((np.arange(n, dtype=np.uint64) * np.uint64(2654435761)) ^ np.uint64(step * 40503)) % np.uint64(1000)
        lvl = 0.4 + 0.6 * (h.astype(np.float64) / 999.0) ** 3
    elif p == Pattern.WAVE:
        lvl = 0.3 + 0.7 * (0.5 + 0.5 * np.sin(2 * math.pi * (idx / n - ph)))
    elif p == Pattern.PULSE:
        lvl = np.full(n, 0.25 + 0.75 * math.exp(-5.0 * (ph % 1.0)))
    elif p == Pattern.HEARTBEAT:
        frac = ph % 1.0
        lub = math.exp(-0.5 * (frac / 0.045) ** 2)
        dub = 0.55 * math.exp(-0.5 * ((frac - 0.18) / 0.05) ** 2)
        lvl = np.full(n, 0.10 + 0.90 * max(lub, dub))
    else:
        lvl = np.ones(n)
    base = np.array(expr.rgbw, dtype=np.float64) * (expr.brightness / 255.0)
    return base[None, :] * lvl[:, None]


class LedRing:
    def __init__(self, n: int = 24, front_index: int = 0, fade_s: float = 0.5):
        self.n, self.front_index, self.fade_s = n, front_index, fade_s
        self._cur: Optional[Expression] = None
        self._prev: Optional[Expression] = None
        self._switch_t = 0.0
        self._dir = 0.0
        self._last_t = None
        self.pixels = np.zeros((n, 4))

    def set(self, expr: Expression, now: float, fade_s: Optional[float] = None) -> None:
        fs = self.fade_s if fade_s is None else fade_s
        if self._cur is None:
            self._cur = self._prev = expr
            self._switch_t = now - fs
        elif expr.key() != self._cur.key():
            self._prev, self._switch_t = self._cur, now
        self._cur = expr
        dt = 0.0 if self._last_t is None else max(0.0, now - self._last_t)
        self._last_t = now
        self._dir += (expr.direction - self._dir) * min(1.0, dt * 4.0)
        k = min(1.0, (now - self._switch_t) / fs) if fs > 0 else 1.0
        new = render_pattern(self._cur, now, self._dir, self.n, self.front_index)
        if k < 1.0:
            old = render_pattern(self._prev, now, self._dir, self.n, self.front_index)
            new = old * (1 - k) + new * k
        self.pixels = np.clip(new, 0, 255)

    @staticmethod
    def _display_color(pixel) -> tuple:
        """RGBW -> what the eye sees. Real LEDs look far brighter than the raw
        values suggest, so a dim expression must not render as near-black."""
        R, G, B, W = pixel
        bgr = np.array([min(255.0, B + W * 0.8), min(255.0, G + W * 0.9), min(255.0, R + W)])
        peak = bgr.max()
        if peak < 1:
            return (0, 0, 0)
        shown = 255.0 * (peak / 255.0) ** 0.5          # perceptual boost
        return tuple(float(v / peak * shown) for v in bgr)

    def draw(self, size: int = 320, lines=()) -> np.ndarray:
        img = np.zeros((size + 20 * len(lines) + 10, size, 3), np.uint8)
        c, r = size // 2, int(size * 0.38)
        for i, pixel in enumerate(self.pixels):
            ang = 2 * math.pi * i / self.n - math.pi / 2  # LED 0 (camera side) at top
            x, y = int(c + r * math.cos(ang)), int(c + r * math.sin(ang))
            cv2.circle(img, (x, y), 13, self._display_color(pixel), -1, cv2.LINE_AA)
        img = cv2.add(img, cv2.GaussianBlur(img, (0, 0), 9))
        cv2.putText(img, "camera", (c - 28, 18), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (120, 120, 120), 1)
        for j, text in enumerate(lines):
            cv2.putText(img, text, (8, size + 18 + 20 * j), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (230, 230, 230), 1)
        return img
