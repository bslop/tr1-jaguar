#!/usr/bin/env python3
"""beacon_dark.py - fps from the beacon when the PICTURE IS BLACK.

beacon_fps_fast.py locates the beacon relative to the detected picture box, and
that detector needs a lit screen.  Ablation arms like NOFILL=1 draw nothing at
all, so the box detector returns None and the tool dies - which looks exactly
like a failed boot and would wrongly condemn the ROM.

With nothing else drawing, the beacon is the ONLY thing in the frame that
changes, so no box is needed: scan the WHOLE frame for the pixel with the
largest swing.  Same run-length audit as the fixed beacon tool, same refusal
when the runs look like noise.

usage: beacon_dark.py <roll.mkv> [skip]
"""
import collections, glob, os, shutil, subprocess, sys, tempfile
from PIL import Image

SCALE_W = 480          # coarse is fine: we only need to FIND one bright block


def main(mkv, skip=0.20):
    d = tempfile.mkdtemp()
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", mkv,
                    "-vf", f"fps=60,scale={SCALE_W}:-1,format=gray",
                    "-y", os.path.join(d, "c%05d.png")], check=True)
    fs = sorted(glob.glob(os.path.join(d, "c*.png")))
    if not fs:
        shutil.rmtree(d, ignore_errors=True); sys.exit("no frames decoded")
    fs = fs[int(len(fs) * skip):]
    ims = [Image.open(f).convert("L") for f in fs]
    W, H = ims[0].size
    px = [im.load() for im in ims]

    # coarse sweep of the whole frame for the largest swing
    best = None
    for y in range(0, H, 2):
        for x in range(0, W, 2):
            v0 = px[0][x, y]
            lo = hi = v0
            for i in range(0, len(ims), 3):        # subsample in time to find it
                t = px[i][x, y]
                if t < lo: lo = t
                if t > hi: hi = t
            if best is None or (hi - lo) > best[0]:
                best = (hi - lo, x, y)
    swing, bx, by = best
    if swing < 60:
        shutil.rmtree(d, ignore_errors=True)
        sys.exit(f"no blinking block found (best swing {swing}) - "
                 f"ROM did not boot, or FPSBEACON is not in this build")

    v = [p[bx, by] for p in px]
    lo, hi = min(v), max(v)
    thr = (lo + hi) / 2.0
    b = [1 if t > thr else 0 for t in v]
    runs, cur = [], 1
    for i in range(1, len(b)):
        if b[i] == b[i - 1]: cur += 1
        else: runs.append(cur); cur = 1
    runs.append(cur)
    tr = len(runs) - 1
    ones = 100.0 * sum(1 for r in runs if r == 1) / max(1, len(runs))
    secs = len(fs) / 60.0
    print(f"beacon found at capture ({bx},{by}) of {W}x{H}, swing {lo}->{hi}")
    print(f"fields per frame: {sorted(collections.Counter(runs).items())[:6]}")
    if ones > 15:
        shutil.rmtree(d, ignore_errors=True)
        sys.exit(f"REFUSING: {ones:.0f}% of runs are one capture frame - noise")
    print(f"{tr} transitions in {secs:.1f}s  =>  {tr/secs:.2f} fps")
    for im in ims: im.close()
    shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.20)
