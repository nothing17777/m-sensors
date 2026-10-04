"""Swappable emotion providers (Build Plan §3: provider-based EchoAi integration).

- MockProvider:   works today, no API access needed (keyboard / cycling labels)
- EchoAiProvider: live EchoAi service. IPMD has not sent API docs yet, so the
                  request/response format is isolated in two methods marked
                  TODO(API docs) — only those need editing when docs arrive.

EmotionWorker runs the provider off the video loop so a slow or offline
network never freezes the LEDs (Build Plan §3: "the demo never stalls").
"""
from __future__ import annotations

import os
import time
from abc import ABC, abstractmethod
from collections import deque
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass, field
from typing import Deque, Optional, Tuple

EMOTION_LABELS = ("neutral", "happy", "sad", "angry", "surprised", "fearful", "disgusted")


@dataclass
class EmotionResult:
    label: str
    confidence: float          # 0..1
    intensity: float           # 0..1
    latency_ms: float = 0.0
    raw: dict = field(default_factory=dict)


class EmotionProvider(ABC):
    name = "base"

    @abstractmethod
    def analyze(self, face_jpeg: bytes) -> EmotionResult:
        ...


class MockProvider(EmotionProvider):
    name = "mock"

    def __init__(self, cycle: bool = False, latency_s: float = 0.25):
        self.cycle = cycle
        self.latency_s = latency_s
        self.label = "neutral"
        self._i = 0

    def set_label(self, label: str) -> None:
        self.label = label

    def analyze(self, face_jpeg: bytes) -> EmotionResult:
        t0 = time.perf_counter()
        time.sleep(self.latency_s)
        if self.cycle:
            self.label = EMOTION_LABELS[self._i % len(EMOTION_LABELS)]
            self._i += 1
        return EmotionResult(self.label, 0.90, 0.70, (time.perf_counter() - t0) * 1000,
                             {"mock": True, "bytes": len(face_jpeg)})


class EchoAiProvider(EmotionProvider):
    name = "echoai"

    def __init__(self, url: Optional[str] = None, api_key: Optional[str] = None, timeout_s: float = 3.0):
        self.url = url or os.environ.get("ECHOAI_URL")
        self.api_key = api_key or os.environ.get("ECHOAI_API_KEY", "")
        self.timeout_s = timeout_s
        if not self.url:
            raise ValueError("ECHOAI_URL is not set")
        import requests  # imported lazily so mock mode has no network dependency
        self._session = requests.Session()

    def analyze(self, face_jpeg: bytes) -> EmotionResult:
        t0 = time.perf_counter()
        kwargs = self._build_request(face_jpeg)
        resp = self._session.post(self.url, timeout=self.timeout_s, **kwargs)
        resp.raise_for_status()
        latency_ms = (time.perf_counter() - t0) * 1000
        return self._parse_response(resp.json(), latency_ms)

    # ---- TODO(API docs): the only two methods to adapt to the real EchoAi API ----
    def _build_request(self, face_jpeg: bytes) -> dict:
        headers = {"Authorization": f"Bearer {self.api_key}"} if self.api_key else {}
        return {"headers": headers, "files": {"image": ("face.jpg", face_jpeg, "image/jpeg")}}

    def _parse_response(self, data: dict, latency_ms: float) -> EmotionResult:
        # Accepts {"emotion": "happy", "confidence": 0.8, "intensity": 0.6}
        # or      {"emotions": {"happy": 0.8, "sad": 0.1, ...}}
        if "emotions" in data and isinstance(data["emotions"], dict) and data["emotions"]:
            label, conf = max(data["emotions"].items(), key=lambda kv: float(kv[1]))
        else:
            label = data.get("emotion") or data.get("label") or "neutral"
            conf = data.get("confidence", data.get("score", 0.0))
        conf = float(conf)
        intensity = float(data.get("intensity", conf))
        return EmotionResult(str(label), conf, max(0.0, min(1.0, intensity)), latency_ms, data)


class EmotionWorker:
    """Runs one provider call at a time in the background, with rate limiting."""

    def __init__(self, provider: EmotionProvider, interval_s: float, urgent_gap_s: float, error_backoff_s: float):
        self.provider = provider
        self.interval_s = interval_s
        self.urgent_gap_s = urgent_gap_s
        self.error_backoff_s = error_backoff_s
        self._pool = ThreadPoolExecutor(max_workers=1, thread_name_prefix="echoai")
        self._future: Optional[Future] = None
        self._last_submit = float("-inf")
        self._backoff_until = float("-inf")
        self.online = True
        self.latencies_ms: Deque[float] = deque(maxlen=200)
        self.submitted = self.succeeded = self.failed = 0

    def ready(self, now: float, urgent: bool = False) -> bool:
        if self._future is not None or now < self._backoff_until:
            return False
        gap = self.urgent_gap_s if urgent else self.interval_s
        return now - self._last_submit >= gap

    def submit(self, face_jpeg: bytes, now: float) -> None:
        self._last_submit = now
        self.submitted += 1
        self._future = self._pool.submit(self.provider.analyze, face_jpeg)

    def poll(self, now: float) -> Optional[Tuple[str, object]]:
        """Returns ("ok", EmotionResult), ("error", Exception) or None."""
        if self._future is None or not self._future.done():
            return None
        fut, self._future = self._future, None
        try:
            res = fut.result()
        except Exception as exc:  # network down, timeout, bad response...
            self.failed += 1
            self.online = False
            self._backoff_until = now + self.error_backoff_s
            return "error", exc
        self.succeeded += 1
        self.online = True
        self.latencies_ms.append(res.latency_ms)
        return "ok", res

    def shutdown(self) -> None:
        self._pool.shutdown(wait=False, cancel_futures=True)
