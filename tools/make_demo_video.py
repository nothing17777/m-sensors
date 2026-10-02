"""Write a synthetic 640x480 test video (moving 'person' blob) for headless runs.

Usage:  python tools/make_demo_video.py demo.avi
"""
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pc_prototype"))
import synthetic as syn  # noqa: E402


def main(path: str = "demo.avi", fps: int = 30) -> None:
    out = cv2.VideoWriter(path, cv2.VideoWriter_fourcc(*"MJPG"), fps, (640, 480))
    frames = 0
    for _, f in syn.scenario():
        big = cv2.cvtColor(cv2.resize(f, (640, 480), interpolation=cv2.INTER_NEAREST), cv2.COLOR_GRAY2BGR)
        for _ in range(fps // 10):          # scenario is 10 fps; repeat frames to reach video fps
            out.write(big)
            frames += 1
    out.release()
    print(f"wrote {path}: {frames} frames @ {fps} fps")


if __name__ == "__main__":
    main(*sys.argv[1:2])
