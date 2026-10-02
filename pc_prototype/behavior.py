"""Behaviour state machine for the motion -> emotion flow.

    AMBIENT --motion START--> AUTO_RESPONSE --confident EchoAi result--> EMOTION
       ^                          |                                        |
       +------ motion END --------+<------ hold expires (motion active) ---+
       +<----------------------------------- hold expires (no motion) -----+

Mirrors firmware/components/expression/src/behavior.c.
"""
from __future__ import annotations

from enum import Enum
from typing import Optional

import response_map as rm
from emotion_providers import EmotionResult
from motion_detector import MotionEvent, MotionLevel, MotionResult
from touch_detector import Gesture, TouchEvent, TouchResult


class Mode(str, Enum):
    AMBIENT = "ambient"
    AUTO_RESPONSE = "auto_response"
    EMOTION = "emotion"
    TOUCH = "touch"


class Behavior:
    def __init__(self, emotion_hold_s: float = 8.0, min_confidence: float = 0.4,
                 presence_grace_s: float = 3.0, calm_to_warm_s: float = 2.5,
                 touch_afterglow_s: float = 4.0, regreet_cooldown_s: float = 20.0,
                 flip_direction: bool = False, pickup_flash_s: float = 1.3,
                 wink_flash_s: float = 0.4):
        self.emotion_hold_s = emotion_hold_s
        self.min_confidence = min_confidence
        self.presence_grace_s = presence_grace_s
        self.calm_to_warm_s = calm_to_warm_s
        self.touch_afterglow_s = touch_afterglow_s
        self.regreet_cooldown_s = regreet_cooldown_s
        self.flip_direction = flip_direction
        self.pickup_flash_s = pickup_flash_s
        self.wink_flash_s = wink_flash_s
        self.mode = Mode.AMBIENT
        self.night = False
        self.night_since = float("-inf")
        self.motion = MotionResult()
        self.touch = TouchResult()
        self.touch_until = float("-inf")
        self.auto_since = float("-inf")
        self.auto_stage = rm.AutoStage.CALM
        self.direction = 0.0
        self.emotion: Optional[EmotionResult] = None
        self.emotion_until = 0.0
        self.last_motion_end = float("-inf")
        self.last_face_seen = float("-inf")
        self.scan_due = False
        self.pickup_until = float("-inf")
        self.wink_until = float("-inf")
        self.pickup_active = False
        self.wink_active = False

    def on_motion(self, m: MotionResult, now: float) -> None:
        self.motion = m
        if m.level != MotionLevel.NONE:
            d = m.centroid_x * 2.0 - 1.0
            self.direction = -d if self.flip_direction else d
        if m.event == MotionEvent.START:
            self.scan_due = True
            if self.mode == Mode.AMBIENT:
                self.mode = Mode.AUTO_RESPONSE
                self.wink_until = now + self.wink_flash_s   # a quick "I see you" flash before the glow settles in
                # Someone who just stepped out and came back is already known, so skip the
                # calm greeting and pick up where it left off.
                returning = now - self.last_motion_end < self.regreet_cooldown_s
                self.auto_since = now - self.calm_to_warm_s if returning else now
                self.auto_stage = rm.AutoStage.WARM if returning else rm.AutoStage.CALM
        elif m.event == MotionEvent.END:
            self.last_motion_end = now
            if self.mode == Mode.AUTO_RESPONSE:
                self.mode = Mode.AMBIENT

    def on_emotion(self, r: EmotionResult, now: float) -> bool:
        if r.confidence < self.min_confidence:
            return False
        self.emotion = r
        self.emotion_until = now + self.emotion_hold_s
        if self.mode != Mode.TOUCH:        # being touched wins; the emotion resumes after
            self.mode = Mode.EMOTION
        return True

    def on_touch(self, t: TouchResult, now: float) -> None:
        """Touch has priority over motion and emotion: it is the most direct contact."""
        was_handled = self.touch.handled
        self.touch = t
        if t.handled and not was_handled:
            self.pickup_until = now + self.pickup_flash_s   # being lifted is the most surprising thing
        if t.active:
            self.mode = Mode.TOUCH
            self.touch_until = now + self.touch_afterglow_s
        elif t.event == TouchEvent.END:
            self.touch_until = now + self.touch_afterglow_s
            if t.gesture == Gesture.DOUBLE_TAP:
                self.toggle_night(now)   # proposal: double tap switches night mode

    def toggle_night(self, now: float) -> None:
        self.night = not self.night
        self.night_since = now

    def on_face(self, now: float) -> None:
        """A face was detected: keeps presence alive while a person sits still."""
        self.last_face_seen = now

    def tick(self, now: float) -> None:
        self.pickup_active = now < self.pickup_until
        self.wink_active = now < self.wink_until
        self.auto_stage = (rm.AutoStage.WARM if now - self.auto_since >= self.calm_to_warm_s
                           else rm.AutoStage.CALM)
        if self.mode == Mode.TOUCH:
            if not self.touch.active and now >= self.touch_until:
                if self.emotion_until > now:
                    self.mode = Mode.EMOTION
                elif self.motion.active:
                    self.mode = Mode.AUTO_RESPONSE
                else:
                    self.mode = Mode.AMBIENT
            return
        if self.mode == Mode.EMOTION and now >= self.emotion_until:
            self.mode = Mode.AUTO_RESPONSE if self.motion.active else Mode.AMBIENT
        elif self.mode == Mode.AMBIENT and self.motion.active:
            self.mode = Mode.AUTO_RESPONSE

    def wants_faces(self, now: float) -> bool:
        """Only run face detection / uploads when someone is (or just was) there."""
        return (self.motion.active or self.mode == Mode.EMOTION
                or self.touch.active or now < self.touch_until   # a hand means someone is here
                or now - self.last_motion_end < self.presence_grace_s
                or now - self.last_face_seen < self.presence_grace_s)

    def expression(self) -> rm.Expression:
        if self.pickup_active:                 # being lifted overrides everything, even mid-touch
            return rm.apply_night(rm.PICKUP_FLASH, self.night)
        if self.mode == Mode.TOUCH:
            return rm.touch_expression(self.touch.gesture, self.touch.strength, self.touch.angle,
                                       self.touch.has_angle, self.touch.travel,
                                       self.touch.sustained, self.night, self.touch.zone)
        if self.wink_active and self.mode == Mode.AUTO_RESPONSE:
            return rm.apply_night(rm.WINK_FLASH, self.night)
        if self.mode == Mode.EMOTION and self.emotion is not None:
            return rm.emotion_expression(self.emotion.label, self.emotion.intensity, self.night)
        if self.mode == Mode.AUTO_RESPONSE:
            return rm.auto_response(self.motion.arousal, self.direction, self.night,
                                    self.auto_stage, self.motion.trend)
        return rm.ambient(self.night)
