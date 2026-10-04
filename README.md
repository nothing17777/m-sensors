# M — Motion and Touch Sensor System (software)

Camera-based motion sensing and capacitive touch sensing, both leading to light responses, for the M robot.
The parts list has no separate motion sensor; the OV5640 camera is the motion sensor
(Technical Overview §3.1 "Object Detection", §6.4 Auto-Response Mode).

```
camera -> motion sensor --(someone present)--> face detect -> face crop -> EchoAi -> emotion
              |                                                                 |
              +-> motion energy (calm/lively/intense) -> Auto-Response light    +-> Emotion light
```

Motion alone does **not** reveal emotion. It gives presence, direction and an energy level,
and it triggers the face scan. EchoAi provides the actual emotion.

The light states follow the whiteboard sketch:

| State | When | Light |
|---|---|---|
| Idle | Nobody around | A soft blink every four seconds: awake but quiet |
| Calm | Motion just started | A cool, gentle glow toward the person, so nobody is startled |
| Warm | They stayed (2.5 s, `calm_to_warm_s`) | The glow turns warm and opens out |
| Emotion | EchoAi answered | That emotion's colour and pattern, held 8 s |
| Touch | A hand on the shell | Warm response, overriding everything else |

Touch runs alongside it and takes priority, because it is the most direct contact:

```
MPR121 pads (50 Hz) -> touch sensor -> gesture (tap / double tap / hold / stroke / hug)
                                    -> warm light that meets the hand, then fades back
```

Gentle touch answers warm and slow; a firmer touch answers dynamically, brighter and quicker,
but still warm (the split is at `TOUCH_FIRM_THRESHOLD`, 0.45 of full strength).

| Gesture | What it means | Light |
|---|---|---|
| Tap | A quick poke | Gentle: warm gold pulse. Firm: bright warm ripple |
| Double tap | Two quick taps | Same, and switches night mode (proposal) |
| Hold | A hand resting on the shell | Gentle: slow warm breath. Firm: brighter warm pulse |
| Stroke | A hand sliding around the shell | Warm wave following the hand, faster when firmer |
| Hug | Hands on opposite sides | Full warm glow, brighter and quicker when firmer |
| Crown pat | The pad on top | Warm glow with no direction |
| Patting | Repeated gentle taps | Steady pulse in time with the hand |

Each pad belongs to a named zone (`front`, `right`, `back`, `left`, `crown`), so the rest of the system
hears "patted on the crown" rather than "pad 8". A hold lasting longer than `sustained_ms` (10 s) is
reported as **settled**: the light eases down further, for someone calming themselves.
Every pad pressed at once for `all_pads_ms` is read as the device being **picked up**, not a hug, and
the baselines are re-learned.

After the hand leaves, the warm glow lingers for 4 seconds, then the robot returns to whatever
it was doing (emotion, motion response, or resting).

| Folder | What it is |
|---|---|
| `pc_prototype/` | Python proof of concept: webcam, motion, face, EchoAi (or mock), simulated LED ring |
| `firmware/` | ESP-IDF project for ESP32-S3 + OV5640 + SK6812 RGBW ring |
| `firmware/components/motion_sensor/` | motion algorithm (`motion_core.c`, pure C) + camera task (`motion_sensor.c`) |
| `firmware/components/touch_sensor/` | touch algorithm (`touch_core.c`, pure C) + MPR121 I2C driver (`touch_sensor.c`) |
| `firmware/components/expression/` | emotion mapping table, behaviour state machine, LED patterns |
| `firmware/host_test/` | C unit tests that run on a PC (no hardware) |
| `tools/` | parity check (C and Python give identical results), demo video generator |

## 1. PC prototype (Windows PowerShell)

Three programs, all in `pc_prototype/`:

| Command | What it runs | Needs a webcam? |
|---|---|---|
| `python run_touch.py` | touch sensor only | no |
| `python run_motion.py` | motion sensor only (camera, face, emotion) | yes, or `--video` |
| `python main.py` | both together, as the robot runs them | yes |

Add `--selftest` to either standalone runner to check it with no window and no camera:
`python run_touch.py --selftest` walks through every gesture, `python run_motion.py --selftest`
runs a scripted scene. Both end in `... sensor OK`.


1. `cd <your clone>\pc_prototype`
2. `py -m venv .venv` — no output = success.
3. `.venv\Scripts\Activate.ps1` — no output = success; prompt now starts with `(.venv)`.
   If you get a "running scripts is disabled" error, run `Set-ExecutionPolicy -Scope Process Bypass` first, then repeat.
4. `pip install -r requirements.txt`
5. `python -m pytest -q` — expected: `14 passed`.
6. `python run_touch.py` — drag on the sphere to touch it. `[` and `]` change how hard,
   Shift+drag is a hug, the middle is the crown, `n` is night mode, `q` quits.
7. `python run_motion.py` — walk in and out of the camera view. `m` shows what counts as movement,
   `1`-`7` set the stand-in emotion, `f` forces a face scan.
8. `python main.py` — both sensors in one window. Wave at the webcam.
   **Touch:** click and drag on the LED ring on the right to touch the sphere. Hold `Shift` while dragging for a hug,
   and click the middle of the ring for a pat on the crown. The mouse stands in for the MPR121 pads.
   If the camera gives you trouble, use `run_touch.py`: it needs no camera at all.
   Keys: `q` quit · `m` motion mask · `n` night mode · `f` force face scan · `1`–`7` mock emotion
   (neutral, happy, sad, angry, surprised, fearful, disgusted).
   If the camera doesn't open: `python main.py --camera 1`.
   If the glow follows the wrong side: `python main.py --flip-direction`.

Expected console output when you walk in and face the camera:
```
[   4.13s] MOTION START  level=PRESENCE area=4.1% x=0.16
[   4.13s] mode ambient -> auto_response
[   4.35s] face scan -> mock (9.8 KB crop)
[   4.61s] EMOTION happy conf=0.90 latency=251ms
[   4.61s] mode auto_response -> emotion
```
A `SUMMARY` line on exit shows scan count and EchoAi latency (avg / p95).

### Switching to the live EchoAi API
1. `$env:ECHOAI_URL="<endpoint from IPMD>"` — no output = set.
2. `$env:ECHOAI_API_KEY="<key>"` — no output = set.
3. `python main.py --provider echoai`

The request/response format is unknown until IPMD sends API docs. Only two methods need editing:
`EchoAiProvider._build_request` and `EchoAiProvider._parse_response` in `emotion_providers.py`
(marked `TODO(API docs)`). Also confirm the input image size (`face_crop_size` in `config.py`, currently 224×224 JPEG).

If EchoAi errors or WiFi drops, the console shows `EchoAi error -> offline fallback`. The lights keep working
in Auto-Response mode, and uploads retry after 10 s.

## 2. Firmware (ESP32-S3)

1. Install ESP-IDF **v5.3 or newer** with the Windows installer (https://dl.espressif.com/dl/esp-idf/), then open the **"ESP-IDF 5.x PowerShell"** shortcut.
2. `cd <your clone>\firmware`
3. `idf.py set-target esp32s3` — ends with `Build files have been written to ...`.
4. `idf.py build` — the component manager downloads `esp32-camera` and `led_strip`. Ends with `Project build complete`.
5. Find the board's COM port in Device Manager → Ports, then: `idf.py -p COM5 flash monitor` (replace `COM5`).
   Exit the monitor with `Ctrl+]`.

Expected monitor output:
```
I (812) motion_sensor: started at 10 fps
I (812) m: M motion sensor running
I (1320) motion_sensor: motion core ready: 160x120 -> 80x60 cells
I (5120) m: MOTION START level=PRESENCE area=3.2% x=0.41
I (5120) m: face scan requested (motion at x=0.41) - no EchoAi module linked yet
I (5520) m: EMOTION happy ("happy") conf=0.90
```

Before flashing, check:
- `main/board_pins.h` — camera, LED **and MPR121 (SDA/SCL)** pins **must match your wiring** (defaults are the ESP32-S3-EYE / Freenove S3 CAM layout; LED data on GPIO14).
- `sdkconfig.defaults` — assumes 8 MB **octal** PSRAM (N8R8). For quad PSRAM, change to `CONFIG_SPIRAM_MODE_QUAD=y`.
- If you see `camera init failed`, check wiring/pins. If grayscale isn't supported by your module, set `.pixel_format = PIXFORMAT_YUV422` in `main.c`.
- `DEMO_FAKE_EMOTIONS 1` in `main.c` fakes EchoAi answers. Set it to `0` once the real integration exists.
- If the MPR121 isn't wired yet you'll see `touch sensor init failed`. Motion and lights keep working.
- Touch pads: 8 copper areas evenly around the equator (pad 0 on the camera side, numbered clockwise) plus one
  on top. Wire them to MPR121 electrodes 0-8, and put the pads **inside** the shell against the PETG wall.

### Integrating the EchoAi / face module (for teammates)
- Motion asks for a scan by calling `app_request_face_scan(const motion_result_t *m)`. It's a weak function; define it in your module to replace the demo.
- Report results with `m_on_emotion(const char *label, float confidence, float intensity)`.
- Call `m_on_face()` whenever a face is seen (keeps presence alive while someone sits still).
- If your module owns the camera (e.g. RGB565 frames for ESP-WHO), don't call `motion_sensor_start()`.
  Call `motion_sensor_init_feed()` once, then `motion_sensor_feed(fb->buf, fb->len, fb->width, fb->height, fb->format, NULL)` per frame.

## 3. Host tests + parity check (WSL / Linux / macOS)

1. `make -C firmware/host_test` — expected: `ALL HOST TESTS PASSED`.
2. `python3 tools/parity_check.py` — expected: `375 frames compared, 0 mismatches -> PASS`.
3. `python3 tools/parity_touch.py` — expected: `3770 samples compared, 0 mismatches -> PASS`.

## Tuning

All thresholds live in `pc_prototype/config.py` (`MotionConfig`) **and** `MOTION_CONFIG_DEFAULT()` in
`firmware/components/motion_sensor/include/motion_core.h`. Tune on the webcam, copy values to the C header, and run the parity check.

| Symptom | Change |
|---|---|
| Triggers on nothing / flicker | raise `pixel_threshold` (22 → 28) or `min_area_ratio` |
| Misses small or distant movement | lower `min_area_ratio` (0.01 → 0.006) |
| Session ends too soon when someone pauses | raise `quiet_ms` |
| Everything reads "intense" | raise `speed_full` / `area_full_ratio` |

Touch thresholds live in `pc_prototype/config.py` (`TouchConfig`) and `TOUCH_CONFIG_DEFAULT()` in
`firmware/components/touch_sensor/include/touch_core.h`. Use `touch_sensor_deltas()` on the robot to see raw counts.

| Symptom | Change |
|---|---|
| Touches are missed | lower `touch_threshold` (12 → 8), or use larger pads |
| It registers touches on its own | raise `touch_threshold`, and check the shell is dry and the wiring is short |
| Light flickers as the hand rests | raise `release_threshold` gap or `release_frames` |
| A gentle rest reads as a stroke | raise `stroke_min_deg` |
| A single wide hand reads as a hug | raise `hug_span_deg` or `hug_pads` |
| Everything reads as a firm touch | raise `TOUCH_FIRM_THRESHOLD`, or raise `strength_full` |
| The calm stage is too short or too long | change `calm_to_warm_s` in the behaviour config |
| False motion in a dark room | raise `noise_gain_q4` (64 = 4x the measured noise) |
| Motion reported when the device is moved | raise `settle_frames` |
| A long hold eases down too soon | raise `sustained_ms` |
| Ordinary handling reads as a pick-up | raise `all_pads_ms` |

The motion sensor also: raises its own threshold to match measured camera noise (dim, lamp-lit rooms),
re-learns the scene for `settle_frames` after the whole view changes (the device is moved or the lights
switch), reports whether someone is **approaching or retreating**, and skips the calm greeting for
someone who returns within `regreet_cooldown_s`.

Known behaviour: a person who stays perfectly still fades from motion detection after ~3 s (face detection keeps presence alive);
after someone leaves, motion ends within ~3 s + `quiet_ms`.

## Open items for IPMD
- EchoAi sandbox access + API docs (endpoint, auth, input size/format, response fields, latency).
- Sign-off on the draft Emotion Mapping Table (`response_map.py` / `expression.c`). Angry and fearful get soothing responses, not mirrored ones.
- Sign-off on the touch responses and on double tap switching night mode.
- Whether an actuator (the sketch's note) is in scope. The code already emits a haptic hint (`app_haptic()` on the
  robot, `expression.haptic` in Python), but the BOM has no motor.
- Consent/retention guidance for face crops used in demos.

## Oct 9 assignment status (per Min Lee's 2026-10-02 review)
1. **4-mood remap — done.** `response_map.py` and `expression.c` now route all 7 detector
   emotions through a `Mood` table (Warm, Sad, Calm, Dynamic): neutral->Calm, happy->Warm,
   sad->Sad, angry/surprised/fearful/disgusted->Dynamic. `EMOTION_TABLE` keeps one entry per
   emotion so existing callers/tests are unaffected, but the four Dynamic emotions now share
   the identical look. Host tests (`firmware/host_test`) still pass.
2. **Live EchoAi API — blocked.** The EchoAi API docs haven't arrived yet. `emotion_providers.py`
   already has an `EchoAiProvider` scaffold with the only two methods that need editing
   (`_build_request` / `_parse_response`) marked `TODO(API docs)`; swap `DEMO_FAKE_EMOTIONS`
   / the mock provider for it once the docs land.
3. **Hardware touch test — blocked.** No MPR121 is wired to this machine, so gestures could
   only be exercised via the PC prototype's synthetic/replay tooling
   (`pc_prototype/run_touch.py`, `tools/parity_touch.py`), not real hardware. Needs the touch
   board wired up to verify on-device.
4. **Push — done,** to this repo (`m-sensors`) per instruction; not pushed to the IPMD
   central repo.
