#!/usr/bin/env python3
"""fps_measure.py — count FPSBEACON toggles in a capture clip -> fps.

The FPSBEACON build paints a 32x8 block (fb x=32..64, y=24..32) that inverts
on EVERY published frame, so beacon transitions = frames, whatever the scene.

The beacon is LOCATED, never hard-coded (the Cam Link re-negotiates its
resolution mid-session and the active picture area moves between boots):
highest per-pixel temporal variance cluster = the beacon.

Usage: python3 fps_measure.py capture.mkv [--fps-in 60]
"""
import subprocess, sys, os
import numpy as np

def read_gray_frames(path, scale_w=480):
    """Decode to grayscale at reduced width; return (frames, fps_in)."""
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height,r_frame_rate",
         "-of", "csv=p=0", path], capture_output=True, text=True)
    w, h, rate = probe.stdout.strip().split("\n")[0].split(",")
    w, h = int(w), int(h)
    num, den = rate.split("/")
    fps_in = float(num) / float(den)
    sh = int(round(int(h) * scale_w / w))
    p = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-vf",
         f"scale={scale_w}:{sh}", "-pix_fmt", "gray",
         "-f", "rawvideo", "-"], capture_output=True)
    n = len(p.stdout) // (scale_w * sh)
    fr = np.frombuffer(p.stdout[:n*scale_w*sh], dtype=np.uint8)
    return fr.reshape(n, sh, scale_w), fps_in

def main():
    path = sys.argv[1]
    frames, fps_in = read_gray_frames(path)
    n = len(frames)
    if n < 30:
        print("clip too short"); sys.exit(1)
    f = frames.astype(np.int16)
    # most-toggling pixel = inside the beacon; a bbox union of ALL active
    # pixels is wrong (scene flicker merges disjoint clusters into one huge
    # box that dilutes the swing — seen 2026-08-06)
    dif = np.abs(np.diff(f, axis=0))
    act = (dif > 100).sum(axis=0)          # count of large swings per pixel
    if act.max() < 20:
        print("no beacon found"); sys.exit(1)
    y, x = np.unravel_index(act.argmax(), act.shape)
    # RAW argmax pixel, not a window mean: window dilution once turned a
    # 9.8 fps arm into a phantom 3.7 (2026-08-06) — the raw pixel swings
    # ~180 gray levels and is unambiguous
    y0 = y1 = int(y); x0 = x1 = int(x)
    sig = f[:, y, x].astype(float)
    lo, hi = np.percentile(sig, 5), np.percentile(sig, 95)
    mid = (lo + hi) / 2
    if hi - lo < 60:
        print(f"beacon swing too small ({lo:.0f}->{hi:.0f})"); sys.exit(1)
    hyst = (hi - lo) * 0.25
    state = sig[0] > mid
    trans = 0
    for v in sig[1:]:
        if state and v < mid - hyst:
            state = False; trans += 1
        elif not state and v > mid + hyst:
            state = True; trans += 1
    secs = n / fps_in
    print(f"beacon at x[{x0}:{x1}] y[{y0}:{y1}] (of {frames.shape[2]}x"
          f"{frames.shape[1]}), swing {lo:.0f}->{hi:.0f}")
    print(f"{trans} transitions in {secs:.2f}s ({n} capture frames "
          f"@ {fps_in:.2f})")
    print(f"fps = {trans/secs:.3f}")

if __name__ == "__main__":
    main()
