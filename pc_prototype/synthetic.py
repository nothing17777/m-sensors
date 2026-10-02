"""Deterministic synthetic 160x120 grayscale scenes for tests and C/Python parity."""
from __future__ import annotations

from typing import Iterator, Tuple

import numpy as np

from config import SENSOR_H, SENSOR_W


def background(rng: np.random.Generator) -> np.ndarray:
    y, x = np.mgrid[0:SENSOR_H, 0:SENSOR_W]
    base = 70 + 30 * np.sin(x / 17.0) + 20 * np.cos(y / 11.0)
    return np.clip(base + rng.normal(0, 2, base.shape), 0, 255)


def frame(bg: np.ndarray, rng: np.random.Generator, noise: float = 3.0, square=None, offset: float = 0.0) -> np.ndarray:
    img = bg + offset + rng.normal(0, noise, bg.shape)
    if square is not None:
        x, y, s, val = square
        x0, y0 = int(max(0, x)), int(max(0, y))
        x1, y1 = int(min(SENSOR_W, x + s)), int(min(SENSOR_H, y + s))
        if x1 > x0 and y1 > y0:
            img[y0:y1, x0:x1] = val
    return np.clip(img, 0, 255).astype(np.uint8)


def scenario(seed: int = 7) -> Iterator[Tuple[int, np.ndarray]]:
    """~48 s at 10 fps: idle, slow walk, fast wave, idle, lights on, sensor noise, left-side presence."""
    rng = np.random.default_rng(seed)
    bg = background(rng)
    t = 0
    step = 100

    def emit(**kw):
        nonlocal t
        f = frame(bg, rng, **kw)
        out = (t, f)
        t += step
        return out

    for _ in range(40):                       # idle (includes warmup)
        yield emit()
    for i in range(40):                       # slow walk left -> right
        yield emit(square=(10 + i * 2.5, 40, 28, 220))
    for _ in range(50):                       # quiet -> END
        yield emit()
    for i in range(30):                       # fast waving
        x = 60 + 50 * np.sin(i * 1.3)
        yield emit(square=(x, 30 + 20 * np.cos(i * 0.9), 36, 230))
    for _ in range(50):
        yield emit()
    for _ in range(10):                       # lights switched on
        yield emit(offset=80)
    bg = np.clip(bg + 80, 0, 255)
    for _ in range(25):
        yield emit()
    for _ in range(30):                       # heavy sensor noise, no subject
        yield emit(noise=9.0)
    for i in range(40):                       # small presence on the left
        yield emit(square=(12 + (i % 4), 50, 18, 20))
    for _ in range(60):
        yield emit()
