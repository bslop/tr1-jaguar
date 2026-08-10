#!/usr/bin/env python3
"""beacon_audit.py - is a beacon fps number REAL or a noisy-pixel artifact?

beacon_fps_fast.py picks the sample point with the MOST transitions, which
biases toward flickering EDGE pixels: a point half-covered by the beacon block
sits near the threshold and every bit of capture noise counts as a frame.  An
exact-2x result between two arms is exactly what that failure looks like.

This audits instead of trusting:
  * picks the point with the LARGEST SWING (cleanest signal), not the most
    transitions;
  * prints the RUN-LENGTH histogram.  A real beacon holds each state for
    60/fps capture frames, so runs cluster.  Noise produces runs of 1.

usage: beacon_audit.py <roll.mkv> [skip]
"""
import glob, os, shutil, subprocess, sys, tempfile, collections
from PIL import Image

SCALE_W = 960


def picture_box(im):
    g = im.convert("L"); W, H = g.size; px = g.load()
    xs = max(1, W // 640); ys = max(1, H // 216)
    cols = [x for x in range(0, W, xs)
            if sum(1 for y in range(0, H, ys * 5) if px[x, y] > 40) > 3]
    rows = [y for y in range(0, H, ys)
            if sum(1 for x in range(0, W, xs * 5) if px[x, y] > 40) > 3]
    return (cols[0], rows[0], cols[-1] + xs, rows[-1] + ys) if cols and rows else None


def main(mkv, skip=0.20):
    d = tempfile.mkdtemp()
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", mkv,
                    "-vf", f"fps=60,scale={SCALE_W}:-1,format=gray",
                    "-y", os.path.join(d, "c%05d.png")], check=True)
    fs = sorted(glob.glob(os.path.join(d, "c*.png")))[int(
        len(sorted(glob.glob(os.path.join(d, "c*.png")))) * skip):]
    ims = [Image.open(f).convert("L") for f in fs]
    bb = picture_box(ims[len(ims) // 2])
    x0, y0, x1, y1 = bb
    sx, sy = (x1 - x0) / 320.0, (y1 - y0) / 240.0

    best = None   # choose by SWING, not by transition count
    for fx in range(34, 62, 1):
        for fy in range(48, 64, 1):
            X, Y = int(x0 + fx * sx), int(y0 + fy * sy)
            v = [im.load()[X, Y] for im in ims]
            lo, hi = min(v), max(v)
            if best is None or (hi - lo) > best[0]:
                best = (hi - lo, fx, fy, v, lo, hi)
    swing, fx, fy, v, lo, hi = best
    thr = (lo + hi) / 2.0
    b = [1 if t > thr else 0 for t in v]
    runs, cur = [], 1
    for i in range(1, len(b)):
        if b[i] == b[i - 1]:
            cur += 1
        else:
            runs.append(cur); cur = 1
    runs.append(cur)
    tr = len(runs) - 1
    secs = len(fs) / 60.0
    hist = collections.Counter(runs)
    print(f"  cleanest point fb({fx},{fy})  swing {lo}->{hi} ({swing})")
    print(f"  {tr} transitions in {secs:.1f}s  =>  {tr/secs:.2f} fps")
    print(f"  run lengths (capture frames per beacon state): "
          f"{sorted(hist.items())[:8]}")
    ones = 100.0 * hist.get(1, 0) / max(1, len(runs))
    print(f"  runs of length 1: {ones:.1f}%   "
          f"{'<-- NOISE, number is not trustworthy' if ones > 15 else 'clean'}")
    for im in ims:
        im.close()
    shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.20)
