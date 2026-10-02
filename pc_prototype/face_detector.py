"""Local face detection + face-crop encoding.

Only a small face crop ever leaves the device (Build Plan §3: local-first,
privacy-conscious uploads). On the ESP32-S3 this role is played by ESP-WHO /
ESP-DL face detection; here we use OpenCV's bundled Haar cascade (no downloads).
"""
from __future__ import annotations

from typing import Optional, Tuple

import cv2
import numpy as np

Box = Tuple[int, int, int, int]  # x, y, w, h in full-frame pixels


class FaceDetector:
    def __init__(self, work_width: int = 320, min_face_frac: float = 0.10):
        path = cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
        self.cascade = cv2.CascadeClassifier(path)
        if self.cascade.empty():
            raise RuntimeError(f"could not load Haar cascade: {path}")
        self.work_width = work_width
        self.min_face_frac = min_face_frac

    def detect(self, frame_bgr: np.ndarray) -> Optional[Box]:
        """Return the largest face, or None."""
        h, w = frame_bgr.shape[:2]
        s = self.work_width / float(w)
        small = cv2.resize(frame_bgr, (self.work_width, max(1, int(h * s))), interpolation=cv2.INTER_AREA)
        gray = cv2.equalizeHist(cv2.cvtColor(small, cv2.COLOR_BGR2GRAY))
        min_px = max(20, int(self.work_width * self.min_face_frac))
        faces = self.cascade.detectMultiScale(gray, scaleFactor=1.1, minNeighbors=5, minSize=(min_px, min_px))
        if len(faces) == 0:
            return None
        x, y, fw, fh = max(faces, key=lambda f: f[2] * f[3])
        return int(x / s), int(y / s), int(fw / s), int(fh / s)


def crop_face_jpeg(frame_bgr: np.ndarray, box: Box, margin: float = 0.25,
                   size: int = 224, quality: int = 85) -> bytes:
    """Square crop around the face with a margin, resized and JPEG-encoded."""
    h, w = frame_bgr.shape[:2]
    x, y, fw, fh = box
    side = int(max(fw, fh) * (1.0 + 2.0 * margin))
    cx, cy = x + fw // 2, y + fh // 2
    x0, y0 = max(0, cx - side // 2), max(0, cy - side // 2)
    x1, y1 = min(w, x0 + side), min(h, y0 + side)
    crop = frame_bgr[y0:y1, x0:x1]
    if crop.size == 0:
        raise ValueError("empty face crop")
    crop = cv2.resize(crop, (size, size), interpolation=cv2.INTER_AREA)
    ok, buf = cv2.imencode(".jpg", crop, [int(cv2.IMWRITE_JPEG_QUALITY), quality])
    if not ok:
        raise RuntimeError("JPEG encode failed")
    return buf.tobytes()
