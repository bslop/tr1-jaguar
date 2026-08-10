#!/usr/bin/env python3
"""beacon_fps.py - frame rate from an FPSBEACON capture, with NOTHING of ours
painted on the screen except the beacon block itself.

WHY THIS EXISTS (2026-08-09).  Four on-screen instruments lied in one day:
bars drawn black-for-zero (so "0" and "never painted" looked the same), a
sampler that read the capture's pillarbox instead of the picture, a cell
decoder that reported 711 fps, and a menu_text readout whose OWN cost was
183ms of the phase it sat in - it made the flip wait look like the dominant
term in the frame when it was mostly the readout.  Anything painted on the
68000 to measure the 68000 changes the answer.

FPSBEACON paints a 32x8 block at fb (32..64, 24..32) that inverts on every
published frame.  That is the whole instrument: no glyphs, no bars.

☠️ DO NOT locate the beacon by "highest temporal variance" (what
fps_measure.py does): the flickering faces out-vary it and the search lands on
scenery - that is how 0.93 fps and 0.48 fps got reported for a game running
near 6.3.  Search only the small neighbourhood where the beacon must be.

☠️ The Cam Link renegotiates resolution mid-session (720x576 and 3840x2160
both seen today) and the picture is pillarboxed inside the frame, so the box
is measured per run, never assumed.

usage: beacon_fps.py <roll.mkv> [--skip 0.20]
"""
import glob
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image


def picture_box(im):
    """the console picture inside a pillarboxed capture"""
    g = im.convert("L")
    W, H = g.size
    px = g.load()
    cols = [x for x in range(0, W, 6)
            if sum(1 for y in range(0, H, 10) if px[x, y] > 40) > 3]
    rows = [y for y in range(0, H, 6)
            if sum(1 for x in range(0, W, 10) if px[x, y] > 40) > 3]
    if not cols or not rows:
        return None
    return cols[0], rows[0], cols[-1] + 6, rows[-1] + 6


def main(mkv, skip=0.20):
    d = tempfile.mkdtemp()
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", mkv,
                    "-vf", "fps=60", "-y", os.path.join(d, "c%05d.png")],
                   check=True)
    fs = sorted(glob.glob(os.path.join(d, "c*.png")))
    if not fs:
        sys.exit("no frames decoded")
    fs = fs[int(len(fs) * skip):]                 # skip the boot chain
    ims = [Image.open(f).convert("L") for f in fs]
    bb = picture_box(ims[len(ims) // 2])
    if not bb:
        sys.exit("no picture found - dark roll?")
    x0, y0, x1, y1 = bb
    sx, sy = (x1 - x0) / 320.0, (y1 - y0) / 240.0

    best = None
    for fx in range(34, 62, 4):                   # the beacon's own patch only
        for fy in range(48, 64, 2):               # 240-space (fb rows double)
            X, Y = int(x0 + fx * sx), int(y0 + fy * sy)
            v = [im.load()[X, Y] for im in ims]
            lo, hi = min(v), max(v)
            if hi - lo < 60:                      # not a two-level signal
                continue
            thr = (lo + hi) / 2.0
            b = [1 if t > thr else 0 for t in v]
            tr = sum(1 for i in range(1, len(b)) if b[i] != b[i - 1])
            if best is None or tr > best[0]:
                best = (tr, fx, fy, lo, hi)
    if best is None:
        sys.exit("beacon not found - is FPSBEACON=1 in this build?")
    tr, fx, fy, lo, hi = best
    secs = len(fs) / 60.0
    print(f"picture {x1-x0}x{y1-y0} of {ims[0].size[0]}x{ims[0].size[1]}")
    print(f"beacon at fb({fx},{fy}), swing {lo}->{hi}")
    print(f"{tr} transitions in {secs:.1f}s  =>  {tr/secs:.2f} fps")
    # a 50s roll decodes to ~3000 4K PNGs (~6GB).  mkdtemp does not clean up
    # after itself, so a campaign of arms silently ate tens of GB of /tmp.
    # Everything above is already computed; the frames are dead weight now.
    for im in ims:
        im.close()
    shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.20)
