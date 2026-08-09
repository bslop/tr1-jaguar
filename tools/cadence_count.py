#!/usr/bin/env python3
"""cadence_count.py - count DISPLAYED frames off a VIDCAD capture.

VIDCAD paints a 16x8 block at the top-left of every video frame in one of two
palette entries, alternating per frame.  It is immune to clip content, costs
the 68000 128 byte stores, and a 60fps capture of it answers the only two
questions that matter without any panel at all:

    how many frames did the clip actually display, and at what rate?

It matters because colour-based clip timing cannot tell "played fast" from
"ended early" - EIDOS finishing in 7.8s could be 152 frames at 19.5fps
(impossible: the pacer floors at 4 fields) or ~117 frames at 15fps (an early
exit).  This counts them.

usage: cadence_count.py <roll.mkv> [fps_of_extraction]
"""
import glob
import os
import subprocess
import sys
import tempfile

from PIL import Image


def extract(mkv, fps, d):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", mkv,
                    "-vf", f"fps={fps}", "-y", os.path.join(d, "c%05d.png")],
                   check=True)
    return sorted(glob.glob(os.path.join(d, "c*.png")))


def main(mkv, fps=60):
    with tempfile.TemporaryDirectory() as d:
        fs = extract(mkv, fps, d)
        if not fs:
            sys.exit("no frames extracted")
        # The parity block sits at the top-left of the picture area, whose
        # position in the capture is unknown (pillarbox).  Find it: the right
        # box is the one whose luma alternates hardest over time.
        ims = [Image.open(f).convert("L") for f in fs]
        W, H = ims[0].size
        best, bestscore = None, -1
        for bx in range(0, W // 3, 8):
            for by in range(0, H // 8, 4):
                vals = [im.crop((bx, by, bx + 12, by + 6))
                          .resize((1, 1)).getpixel((0, 0)) for im in ims[:200]]
                flips = sum(1 for i in range(1, len(vals))
                            if abs(vals[i] - vals[i - 1]) > 60)
                if flips > bestscore:
                    best, bestscore = (bx, by), flips
        bx, by = best
        vals = [im.crop((bx, by, bx + 12, by + 6))
                  .resize((1, 1)).getpixel((0, 0)) for im in ims]
        print(f"parity block at ({bx},{by}), {bestscore} flips in the first "
              f"{min(200, len(vals))} capture frames")
        # count transitions per second of capture
        total, per_sec, cur = 0, [], 0
        for i in range(1, len(vals)):
            if abs(vals[i] - vals[i - 1]) > 60:
                total += 1
                cur += 1
            if i % fps == 0:
                per_sec.append(cur)
                cur = 0
        print("frames displayed per second:",
              " ".join(f"{i+1}:{v}" for i, v in enumerate(per_sec)))
        print(f"TOTAL frames displayed: {total}")
        run = [i for i, v in enumerate(per_sec) if v > 0]
        if run:
            print(f"video active from t={run[0]}s to t={run[-1]+1}s "
                  f"({run[-1]+1-run[0]}s), mean "
                  f"{total/max(1,(run[-1]+1-run[0])):.2f} fps")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 60)
