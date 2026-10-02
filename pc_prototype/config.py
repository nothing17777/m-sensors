"""Tunable parameters for the M motion sensor system.

MotionConfig MUST stay in sync with MOTION_CONFIG_DEFAULT() in
firmware/components/motion_sensor/include/motion_core.h, so values tuned on a
laptop webcam transfer 1:1 to the ESP32-S3. `tools/parity_check.py` verifies
both implementations produce identical results on the same frames.
"""
from dataclasses import dataclass

# The firmware captures FRAMESIZE_QQVGA grayscale from the OV5640.
# The PC prototype downsizes webcam frames to the same resolution first.
SENSOR_W, SENSOR_H = 160, 120


@dataclass
class MotionConfig:
    block: int = 2                # downscale block (px) -> 80x60 cells
    pixel_threshold: int = 22     # luma difference that counts as "changed"
    bg_shift: int = 4             # background learning rate 1/16 (static cells)
    fg_shift: int = 7             # background learning rate 1/128 (cells that are actively moving)
    stable_threshold: int = 10    # |cell - previous frame| <= this = not moving (absorbed at bg_shift, prevents ghosts)
    min_neighbors: int = 2        # changed cell needs >= N changed neighbours (noise filter)
    frames_to_trigger: int = 2    # consecutive moving frames before START
    warmup_frames: int = 10       # let camera auto-exposure settle
    noise_gain_q4: int = 64       # threshold floor = measured noise x this/16 (dim rooms)
    settle_frames: int = 8        # frames ignored after the whole scene changes
    trend_fast_alpha: float = 0.35   # area EMAs behind approach/retreat
    trend_slow_alpha: float = 0.07
    trend_deadband: float = 0.004
    quiet_ms: int = 1500          # no motion for this long -> END
    min_area_ratio: float = 0.01  # >= 1% of cells changed = motion
    active_area_ratio: float = 0.08   # >= 8% = ACTIVE (vs PRESENCE)
    lighting_ratio: float = 0.65  # > 65% changed at once = lighting change, not motion
    area_full_ratio: float = 0.25 # area that maps to energy 1.0
    speed_full: float = 1.5       # frame-widths/sec that maps to energy 1.0
    speed_alpha: float = 0.3      # speed smoothing
    energy_alpha: float = 0.2     # energy smoothing


@dataclass
class TouchConfig:
    """Keep in sync with TOUCH_CONFIG_DEFAULT() in
    firmware/components/touch_sensor/include/touch_core.h."""
    pads: int = 9
    # degrees around the sphere (0 = camera side); None = crown pad on top, no direction
    pad_angles: tuple = (0.0, 45.0, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0, None)
    # where each pad sits on the body, so the rest of the system can say "patted on the head"
    pad_zones: tuple = ("front", "right", "right", "back", "back", "back", "left", "left", "crown")
    pat_taps: int = 3             # taps in a row that mean patting
    pat_window_ms: int = 2500
    sustained_ms: int = 10000     # held this long = settled in, calmer still
    all_pads_ms: int = 2000       # every pad at once this long = being picked up
    touch_threshold: int = 12      # counts below baseline to count as a press
    release_threshold: int = 6     # hysteresis
    press_frames: int = 2
    release_frames: int = 2
    baseline_shift: int = 6        # baseline EMA 1/64 while the pad is untouched
    stuck_ms: int = 60000          # held this long -> that pad re-baselines itself
    tap_max_ms: int = 350
    double_tap_gap_ms: int = 500
    hold_min_ms: int = 700
    stroke_min_deg: float = 60.0
    hug_pads: int = 3
    hug_span_deg: float = 100.0    # ...spread this far around the shell (not one broad hand)
    hug_min_ms: int = 500
    strength_full: int = 120       # delta counts that read as full strength


@dataclass
class AppConfig:
    motion_hz: float = 10.0          # motion processing rate (matches firmware fps)
    face_hz: float = 5.0             # face detection rate while someone is present
    scan_interval_s: float = 4.0     # min gap between EchoAi uploads during a session
    urgent_scan_gap_s: float = 1.0   # min gap for the first scan after motion START
    error_backoff_s: float = 10.0    # wait after an EchoAi error (offline fallback)
    presence_grace_s: float = 3.0    # keep looking for faces this long after motion END
    emotion_hold_s: float = 8.0      # how long an emotion expression is shown
    min_confidence: float = 0.40     # ignore EchoAi results below this
    face_crop_size: int = 224        # TODO(API docs): confirm expected input size
    face_crop_margin: float = 0.25
    jpeg_quality: int = 85
    touch_hz: float = 50.0           # touch sampling rate (matches the firmware)
    touch_afterglow_s: float = 4.0   # warm glow lingers after the hand leaves
    regreet_cooldown_s: float = 5.0  # someone returning this soon skips the calm greeting
    night_sweep_s: float = 1.6       # night mode toggles with a deliberate sweep, not a snap
    num_leds: int = 24
    flip_direction: bool = False     # set True if the glow follows the wrong side
