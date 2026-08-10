#!/usr/bin/env python3
"""framechange_fps.py - INDEPENDENT fps check that never looks at the beacon.

WHY.  beacon_fps.py counts transitions of a block the 68000 paints once per
published frame.  If anything about publishing changes between arms (duplicate
publishes, a flip that does not land, the block being clipped), that count can
be wrong in a way that is invisible from inside the same method.  The ablation
sweep produced results counterintuitive enough (every removal SLOWER) that the
instrument itself has to be cross-checked by something with a different failure
mode.

METHOD.  Decode at 60fps, mask OUT the beacon patch, and count how often the
rest of the picture changes at all.  A newly rendered frame differs from its
predecessor (idle breathing, the A1 flicker, camera drift); a repeated field
does not.  So "change events per second" is an independent estimate of the
RENDERED frame rate.

☠️ ASYMMETRIC FAILURE: this UNDERCOUNTS if two consecutive rendered frames are
pixel-identical (a truly frozen scene), so treat it as a LOWER BOUND.  It
cannot invent motion, so if it AGREES with the beacon the beacon is sound.

usage: framechange_fps.py <roll.mkv> [--skip 0.20]
"""
import glob
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops, ImageStat

SCALE_W = 480


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
    W, H = ims[0].size

    # blank the beacon's neighbourhood so its own toggling cannot be counted.
    # it lives near fb x 32..64, y 24..32 (of 320x240) -> fractions of the frame.
    box = (int(W * 0.05), int(H * 0.05), int(W * 0.30), int(H * 0.22))
    for im in ims:
        im.paste(0, box)

    diffs = []
    for i in range(1, len(ims)):
        st = ImageStat.Stat(ImageChops.difference(ims[i], ims[i - 1]))
        diffs.append(st.mean[0])
    if not diffs:
        shutil.rmtree(d, ignore_errors=True)
        sys.exit("too few frames")

    hi, lo = max(diffs), min(diffs)
    thr = lo + (hi - lo) * 0.15          # well above decoder noise floor
    events = 0
    armed = True
    for v in diffs:
        if v > thr and armed:
            events += 1
            armed = False
        elif v <= thr:
            armed = True
    secs = len(fs) / 60.0
    print(f"masked beacon box {box}, diff range {lo:.2f}..{hi:.2f}, thr {thr:.2f}")
    print(f"{events} picture-change events in {secs:.1f}s  =>  {events/secs:.2f} fps (lower bound)")
    for im in ims:
        im.close()
    shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.20)
