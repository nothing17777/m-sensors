"""Motion + emotion -> light expression.

Mirrors firmware/components/expression/src/response_map.c.
EMOTION_TABLE is a DRAFT of the Phase 1 "Emotion Mapping Table" deliverable —
confirm colours/patterns with IPMD. Negative high-arousal emotions (angry,
fearful) get soothing responses rather than being mirrored, since M
is meant as a calming tool.
"""
from __future__ import annotations

from dataclasses import dataclass, replace
from enum import Enum
from typing import Optional, Tuple

from motion_detector import Arousal, Trend
from touch_detector import Gesture

EMOTIONS = ("neutral", "happy", "sad", "angry", "surprised", "fearful", "disgusted")
_SYNONYMS = {
    "happiness": "happy", "joy": "happy", "smile": "happy",
    "sadness": "sad", "anger": "angry", "surprise": "surprised",
    "fear": "fearful", "scared": "fearful", "disgust": "disgusted",
    "contempt": "disgusted", "calm": "neutral", "none": "neutral",
}


class Pattern(str, Enum):
    SOLID = "solid"
    BLINK = "blink"                # idle: one soft blink every few seconds
    BREATHE = "breathe"
    GLOW_TOWARD = "glow_toward"   # bright spot on the side the motion is
    RIPPLE = "ripple"
    SPARKLE = "sparkle"
    WAVE = "wave"
    PULSE = "pulse"
    HEARTBEAT = "heartbeat"        # two-beat "lub-dub" pulse, for a hug


class Haptic(str, Enum):
    NONE = "none"
    SOFT_PULSE = "soft_pulse"   # one gentle bump, for a tap
    PURR = "purr"               # slow low buzz while held or hugged


@dataclass(frozen=True)
class Expression:
    pattern: Pattern
    rgbw: Tuple[int, int, int, int]  # SK6812 RGBW, 0..255
    brightness: int                  # 0..255
    speed: float                     # cycles per second
    direction: float = 0.0           # quarter-turns: -1 left, +1 right, +-2 = behind
    music: Optional[str] = None      # track id on microSD (music subsystem)
    haptic: "Haptic" = None          # optional actuator; ignored if no motor is fitted

    def key(self):
        return self.pattern, self.rgbw, self.brightness, self.speed


NIGHT_BRIGHTNESS_CAP = 40
NIGHT_SPEED_SCALE = 0.6

TOUCH_FIRM_THRESHOLD = 0.45     # above this a touch counts as firm

# Idle: a soft blink every four seconds, so the sphere reads as awake but quiet.
AMBIENT = Expression(Pattern.BLINK, (0, 0, 0, 170), 60, 0.25)

# One-shot flashes: shown regardless of mode for a brief moment, layered in by Behavior.
PICKUP_FLASH = Expression(Pattern.SPARKLE, (255, 255, 255, 220), 255, 3.2,
                          music="surprise_01", haptic=Haptic.SOFT_PULSE)
WINK_FLASH = Expression(Pattern.SPARKLE, (120, 210, 255, 130), 230, 4.5)   # kept cool/blue-dominant on purpose

# A few body zones get their own signature tint, blended into whatever the gesture is already
# showing. Left/right stay untinted (crown/front/back is plenty to tell apart at a glance).
ZONE_TINT = {
    "front": (255, 70, 70, 0),
    "back": (70, 130, 255, 0),
    "crown": (210, 70, 255, 0),
}
ZONE_TINT_MIX = 0.30


class AutoStage(str, Enum):
    """Activation runs calm first, then warms up while the person stays."""
    CALM = "calm"
    WARM = "warm"


# Stage 1 - calm: a gentle, cool "I noticed you" that does not startle anyone.
AUTO_CALM = {
    Arousal.CALM:    Expression(Pattern.GLOW_TOWARD, (120, 180, 255, 70), 80, 0.30),
    Arousal.LIVELY:  Expression(Pattern.GLOW_TOWARD, (110, 190, 255, 50), 100, 0.45),
    Arousal.INTENSE: Expression(Pattern.RIPPLE, (120, 170, 255, 40), 120, 0.70),
}

# Stage 2 - warm: once the person stays, the light warms up and opens out.
AUTO_WARM = {
    Arousal.CALM:    Expression(Pattern.GLOW_TOWARD, (255, 140, 40, 90), 120, 0.30, music="warm_01"),
    Arousal.LIVELY:  Expression(Pattern.GLOW_TOWARD, (255, 120, 20, 60), 150, 0.60, music="warm_01"),
    Arousal.INTENSE: Expression(Pattern.RIPPLE, (255, 95, 10, 30), 180, 1.00, music="warm_01"),
}

EMOTION_TABLE = {
    #            pattern            R    G    B    W   bri  speed  music
    "neutral":   Expression(Pattern.BREATHE, (0, 0, 0, 160), 90, 0.25, music="ambient_01"),
    "happy":     Expression(Pattern.SPARKLE, (255, 150, 0, 40), 170, 0.80, music="bright_01"),
    "surprised": Expression(Pattern.PULSE, (120, 200, 255, 60), 170, 1.20, music="chime_01"),
    "sad":       Expression(Pattern.BREATHE, (40, 80, 255, 20), 70, 0.15, music="comfort_01"),
    "fearful":   Expression(Pattern.BREATHE, (255, 110, 30, 60), 70, 0.15, music="comfort_01"),
    "angry":     Expression(Pattern.WAVE, (140, 60, 255, 0), 80, 0.20, music="calm_01"),
    "disgusted": Expression(Pattern.WAVE, (60, 200, 120, 0), 90, 0.30, music="calm_01"),
}


# Touch responses. The whiteboard sketch's "turn soft / warm": every touch answers warm.
# haptic is a proposal - the BOM has no motor yet.
# Gentle touch answers warm and slow; a firmer touch answers dynamically -
# brighter, faster and more saturated. {gesture: (gentle, firm)}
TOUCH_TABLE = {
    Gesture.NONE: (
        Expression(Pattern.GLOW_TOWARD, (255, 140, 50, 100), 120, 0.50, music="touch_01", haptic=Haptic.SOFT_PULSE),
        Expression(Pattern.GLOW_TOWARD, (255, 110, 20, 40), 170, 0.90, music="touch_02", haptic=Haptic.SOFT_PULSE)),
    Gesture.TAP: (
        Expression(Pattern.PULSE, (255, 150, 40, 100), 130, 1.20, music="touch_01", haptic=Haptic.SOFT_PULSE),
        Expression(Pattern.RIPPLE, (255, 110, 10, 20), 190, 1.80, music="touch_02", haptic=Haptic.SOFT_PULSE)),
    Gesture.HOLD: (
        Expression(Pattern.BREATHE, (255, 120, 30, 120), 110, 0.22, music="warm_01", haptic=Haptic.PURR),
        Expression(Pattern.PULSE, (255, 100, 10, 40), 170, 0.90, music="warm_02", haptic=Haptic.PURR)),
    Gesture.STROKE: (
        Expression(Pattern.WAVE, (255, 140, 40, 90), 130, 0.45, music="warm_01", haptic=Haptic.PURR),
        Expression(Pattern.WAVE, (255, 100, 10, 30), 180, 1.10, music="warm_02", haptic=Haptic.PURR)),
    Gesture.HUG: (
        Expression(Pattern.HEARTBEAT, (255, 130, 50, 160), 160, 1.0, music="warm_01", haptic=Haptic.PURR),
        Expression(Pattern.HEARTBEAT, (255, 110, 20, 60), 200, 1.3, music="warm_02", haptic=Haptic.PURR)),
}
TOUCH_TABLE[Gesture.DOUBLE_TAP] = TOUCH_TABLE[Gesture.TAP]
# rhythmic patting: a steady, soothing pulse in time with the hand
TOUCH_TABLE[Gesture.PAT] = (
    Expression(Pattern.PULSE, (255, 135, 35, 90), 140, 0.70, music="warm_01", haptic=Haptic.SOFT_PULSE),
    Expression(Pattern.PULSE, (255, 135, 35, 90), 140, 0.70, music="warm_01", haptic=Haptic.SOFT_PULSE))


def touch_expression(gesture, strength: float, angle: float, has_angle: bool,
                     travel: float = 0.0, sustained: bool = False, night: bool = False,
                     zone: str = "none") -> Expression:
    strength = max(0.0, min(1.0, strength))
    gentle, firm = TOUCH_TABLE.get(gesture, TOUCH_TABLE[Gesture.NONE])
    base = firm if strength > TOUCH_FIRM_THRESHOLD else gentle
    k = 0.85 + 0.25 * strength
    speed = base.speed
    if gesture == Gesture.STROKE and travel < 0:
        speed = -speed                      # the wave follows the hand
    direction = base.direction
    if has_angle:
        a = angle
        while a > 180:
            a -= 360
        while a < -180:
            a += 360
        direction = a / 90.0                # quarter-turns, so the glow meets the hand
    rgbw = base.rgbw
    tint = ZONE_TINT.get(zone)
    if tint is not None:
        rgbw = tuple(int(c * (1 - ZONE_TINT_MIX) + t * ZONE_TINT_MIX) for c, t in zip(rgbw, tint))
    brightness = int(base.brightness * k)
    if sustained:                     # settled in: ease down into something quieter
        brightness = int(brightness * 0.8)
        speed *= 0.6
    return apply_night(replace(base, rgbw=rgbw, brightness=brightness, speed=speed, direction=direction), night)


def normalize_label(label: Optional[str]) -> str:
    s = (label or "").strip().lower()
    return s if s in EMOTIONS else _SYNONYMS.get(s, "neutral")


def apply_night(expr: Expression, night: bool) -> Expression:
    if not night:
        return expr
    return replace(expr, brightness=min(expr.brightness, NIGHT_BRIGHTNESS_CAP),
                   speed=expr.speed * NIGHT_SPEED_SCALE)


def ambient(night: bool = False) -> Expression:
    return apply_night(AMBIENT, night)


def auto_response(arousal: Arousal, direction: float, night: bool = False,
                  stage: "AutoStage" = None, trend: Optional[Trend] = None) -> Expression:
    table = AUTO_WARM if stage == AutoStage.WARM else AUTO_CALM
    base = table[arousal]
    if trend == Trend.RETREATING:
        # a farewell: ripple outward and fade, instead of the steady glow-toward
        base = replace(base, pattern=Pattern.RIPPLE, brightness=int(base.brightness * 0.55),
                       speed=max(base.speed, 0.45) * 1.4)
    return apply_night(replace(base, direction=direction), night)


def emotion_expression(label: str, intensity: float, night: bool = False) -> Expression:
    base = EMOTION_TABLE[normalize_label(label)]
    k = 0.6 + 0.4 * max(0.0, min(1.0, intensity))
    return apply_night(replace(base, brightness=int(base.brightness * k)), night)
