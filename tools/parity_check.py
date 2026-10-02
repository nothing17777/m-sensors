"""Verify the C firmware core and the Python prototype give identical results.

Usage (Linux/macOS/WSL):  make -C firmware/host_test  &&  python tools/parity_check.py
"""
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "pc_prototype"))

import synthetic as syn  # noqa: E402
from config import SENSOR_H, SENSOR_W  # noqa: E402
from motion_detector import MotionDetector  # noqa: E402

REPLAY = os.path.join(ROOT, "firmware", "host_test", "build", "replay")


def main() -> int:
    frames = list(syn.scenario())
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as fh:
        fh.write(b"ESFR" + struct.pack("<HHI", SENSOR_W, SENSOR_H, len(frames)))
        for t, f in frames:
            fh.write(struct.pack("<I", t) + f.tobytes())
        path = fh.name
    c_rows = subprocess.run([REPLAY, path], capture_output=True, text=True, check=True).stdout.strip().splitlines()
    os.unlink(path)

    det = MotionDetector(SENSOR_W, SENSOR_H)
    mismatches = 0
    for (t, f), c_line in zip(frames, c_rows):
        r = det.process(f, t)
        py = [t, int(r.event), int(r.level), int(r.arousal), int(r.active), r.changed_cells,
              r.centroid_x, r.centroid_y, *r.bbox, r.speed, r.energy, r.duration_ms,
              int(r.trend), int(r.settling), r.threshold]
        c = [float(v) for v in c_line.split(",")]
        for name, a, b in zip("t event level arousal active cells cx cy x0 y0 x1 y1 speed energy dur "
                              "trend settling threshold".split(), py, c):
            if abs(float(a) - b) > 1e-5:
                mismatches += 1
                if mismatches <= 10:
                    print(f"MISMATCH t={t} {name}: python={a} c={b}")
    if len(c_rows) != len(frames):
        print(f"row count differs: python={len(frames)} c={len(c_rows)}")
        return 1
    print(f"{len(frames)} frames compared, {mismatches} mismatches -> {'PASS' if mismatches == 0 else 'FAIL'}")
    return 0 if mismatches == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
