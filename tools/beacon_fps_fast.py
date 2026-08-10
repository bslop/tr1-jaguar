#!/usr/bin/env python3
"""beacon_fps_fast.py - beacon_fps.py, but ~30x cheaper to run.

WHY.  beacon_fps.py decodes a 50s roll into ~3000 FULL 4K PNGs (~6GB, ~10
minutes, and worse when arms are measured concurrently - which is exactly what
a campaign does).  Nothing in the measurement needs 4K: the beacon is a 32x8
block in a 320x240 framebuffer, which is ~280x70 pixels inside the captured
picture.  Downscaling to 960-wide greyscale keeps it ~24x6 - still far bigger
than the sampling point - and cuts the pixel volume ~36x.

☠️ DO NOT TRUST THIS UNTIL IT REPRODUCES A KNOWN ANSWER.  Editing measurement
tools mid-campaign is how this project has burned rolls before (and how the
`shutil` import crash ate a result an hour ago).  Validate against an arm whose
fps beacon_fps.py already reported, then use it:

    python3 tools/beacon_fps_fast.py <base roll.mkv>     # must read ~6.28

Everything else is deliberately identical to beacon_fps.py: same 60fps sample
rate, same 20% boot skip, same two-level threshold, same "search only the
beacon's own patch" rule (the flickering faces out-vary the beacon and a
highest-variance search lands on scenery).

usage: beacon_fps_fast.py <roll.mkv> [--skip 0.20]
"""
import collections
import glob
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image

SCALE_W = 960          # decode width; beacon stays ~24x6 px at this size


def picture_box(im):
    """the console picture inside a pillarboxed capture"""
    g = im.convert("L")
    W, H = g.size
    px = g.load()
    # step scaled down with the image so the probe grid stays comparable
    xs = max(1, W // 640)
    ys = max(1, H // 216)
    cols = [x for x in range(0, W, xs)
            if sum(1 for y in range(0, H, ys * 5) if px[x, y] > 40) > 3]
    rows = [y for y in range(0, H, ys)
            if sum(1 for x in range(0, W, xs * 5) if px[x, y] > 40) > 3]
    if not cols or not rows:
        return None
    return cols[0], rows[0], cols[-1] + xs, rows[-1] + ys


def main(mkv, skip=0.20):
    d = tempfile.mkdtemp()
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", mkv,
                    "-vf", f"fps=60,scale={SCALE_W}:-1,format=gray",
                    "-y", os.path.join(d, "c%05d.png")], check=True)
    fs = sorted(glob.glob(os.path.join(d, "c*.png")))
    if not fs:
        shutil.rmtree(d, ignore_errors=True)
        sys.exit("no frames decoded")
    fs = fs[int(len(fs) * skip):]
    ims = [Image.open(f).convert("L") for f in fs]
    bb = picture_box(ims[len(ims) // 2])
    if not bb:
        shutil.rmtree(d, ignore_errors=True)
        sys.exit("no picture found - dark roll?")
    x0, y0, x1, y1 = bb
    sx, sy = (x1 - x0) / 320.0, (y1 - y0) / 240.0

    # ☠️☠️ SELECT BY SWING, NEVER BY TRANSITION COUNT (2026-08-10).
    # This used to keep the point with the MOST transitions.  That biases
    # straight to a FLICKERING EDGE PIXEL: a point half-covered by the beacon
    # block sits near the threshold, so capture noise counts as frames.  It
    # reported a SYNCDRAIN arm at 13.32 fps against a 6.67 control - an exact
    # 2x "win" that was pure noise; the cleanest point in the SAME roll read
    # 6.67, dead level with the control.  An exact-integer ratio between arms
    # is the tell.  Pick the CLEANEST signal (largest swing) instead, and
    # refuse to report if the run lengths look like noise.
    best = None
    for fx in range(34, 62, 1):            # the beacon's own patch only
        for fy in range(48, 64, 1):        # 240-space (fb rows double)
            X, Y = int(x0 + fx * sx), int(y0 + fy * sy)
            if not (0 <= X < ims[0].size[0] and 0 <= Y < ims[0].size[1]):
                continue
            v = [im.load()[X, Y] for im in ims]
            lo, hi = min(v), max(v)
            if best is None or (hi - lo) > best[0]:
                best = (hi - lo, fx, fy, lo, hi, v)
    if best is None or best[0] < 60:       # not a two-level signal
        shutil.rmtree(d, ignore_errors=True)
        sys.exit("beacon not found - is FPSBEACON=1 in this build?")
    swing, fx, fy, lo, hi, v = best
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
    ones = 100.0 * sum(1 for r in runs if r == 1) / max(1, len(runs))
    secs = len(fs) / 60.0
    print(f"picture {x1-x0}x{y1-y0} of {ims[0].size[0]}x{ims[0].size[1]}")
    print(f"beacon at fb({fx},{fy}), swing {lo}->{hi}")
    # each run = one published frame held for N capture frames = N 60Hz FIELDS.
    # Seeing runs cluster on integers (8 and 10, say) IS the frame quantization.
    print(f"fields per frame: {sorted(collections.Counter(runs).items())[:6]}")
    if ones > 15:
        shutil.rmtree(d, ignore_errors=True)
        sys.exit(f"REFUSING: {ones:.0f}% of runs are 1 capture frame - "
                 f"noisy sample point, this number would be fiction")
    print(f"{tr} transitions in {secs:.1f}s  =>  {tr/secs:.2f} fps")
    for im in ims:
        im.close()
    shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.20)
