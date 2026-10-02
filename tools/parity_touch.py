"""Verify the C touch core and the Python touch detector agree.

Usage (Linux/macOS/WSL):  make -C firmware/host_test  &&  python tools/parity_touch.py
"""
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "pc_prototype"))

from config import TouchConfig  # noqa: E402
from touch_detector import TouchDetector, TouchPadSimulator  # noqa: E402

REPLAY = os.path.join(ROOT, "firmware", "host_test", "build", "replay_touch")


def scenario():
    """~90 s at 50 Hz: taps, a double tap, a hold, strokes both ways, a hug,
    a crown pat, a stuck pad and slow drift."""
    cfg = TouchConfig()
    sim = TouchPadSimulator(cfg, noise=1, seed=11)
    t = 0
    frames = []

    def hold(ms, **kw):
        nonlocal t
        for _ in range(0, ms, 20):
            frames.append((t, sim.read(**kw)))
            t += 20

    hold(1000)
    hold(200, angle=90); hold(300)
    hold(200, angle=90); hold(800)                      # double tap
    hold(1500, angle=180); hold(600)                    # hold
    for a in range(0, 200, 10):                         # stroke clockwise
        hold(60, angle=a)
    hold(800)
    for a in range(200, 0, -10):                        # stroke back
        hold(60, angle=a)
    hold(800)
    hold(1500, angle=45, spread_deg=95, pressure=1.0)   # hug
    hold(800)
    hold(900, angle=None, crown=True); hold(600)        # crown pat
    for _ in range(4):                                  # rhythmic patting
        hold(160, angle=90); hold(240)
    hold(800)
    hold(12000, angle=0); hold(800)                     # a long, settling hold
    hold(3000, angle=0, spread_deg=400, pressure=1.0, crown=True)   # every pad at once: picked up
    hold(1000)
    hold(62000, angle=270)                              # stuck pad
    hold(1000)
    return frames


def main() -> int:
    cfg = TouchConfig()
    frames = scenario()
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as fh:
        fh.write(b"ESTP" + struct.pack("<BI", cfg.pads, len(frames)))
        for t, vals in frames:
            fh.write(struct.pack("<I", t) + struct.pack(f"<{cfg.pads}H", *vals))
        path = fh.name
    rows = subprocess.run([REPLAY, path], capture_output=True, text=True, check=True).stdout.strip().splitlines()
    os.unlink(path)

    det = TouchDetector(cfg)
    names = ("t event gesture active mask count has_angle angle travel span strength duration recal crown "
             "zone zone_mask sustained handled").split()
    bad = 0
    for (t, vals), line in zip(frames, rows):
        r = det.process(vals, t)
        from touch_detector import ZONE_IDS
        py = [t, int(r.event), int(r.gesture), int(r.active), r.pad_mask, r.pad_count, int(r.has_angle),
              r.angle, r.travel, r.span, r.strength, r.duration_ms, int(r.recalibrated), int(r.crown),
              ZONE_IDS.index(r.zone), r.zone_mask, int(r.sustained), int(r.handled)]
        c = [float(v) for v in line.split(",")]
        for name, a, b in zip(names, py, c):
            d = abs(float(a) - b)
            if name == "angle":
                d = min(d, 360.0 - d)          # 0 and 360 degrees are the same direction
            if d > 1e-3:
                bad += 1
                if bad <= 10:
                    print(f"MISMATCH t={t} {name}: python={a} c={b}")
    if len(rows) != len(frames):
        print(f"row count differs: python={len(frames)} c={len(rows)}")
        return 1
    print(f"{len(frames)} samples compared, {bad} mismatches -> {'PASS' if bad == 0 else 'FAIL'}")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
