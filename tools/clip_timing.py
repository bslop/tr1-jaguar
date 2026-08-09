#!/usr/bin/env python3
"""clip_timing.py - date the boot-clip boundaries in a console capture.

The panel gives per-frame phase numbers, but the question a human actually
asks is "how long did EIDOS take?", and that is answerable without any
instrument at all: the clips have completely different palettes (EIDOS blue,
CORE gold), so the mean hue of the frame steps at every boundary.  This is the
ground truth the read-out is checked against - 10s for EIDOS means Tom is
decoding, ~26s+ means the 68000 is.

usage: clip_timing.py <dir-of-fNNN.png>   (frames must be 1 per second)
"""
import sys, glob, os

from PIL import Image


def stats(path):
    im = Image.open(path).convert("RGB").resize((64, 48))
    px = list(im.getdata())
    n = len(px)
    r = sum(p[0] for p in px) / n
    g = sum(p[1] for p in px) / n
    b = sum(p[2] for p in px) / n
    return r, g, b


def main(d):
    fs = sorted(glob.glob(os.path.join(d, "f*.png")))
    if not fs:
        sys.exit(f"no frames in {d}")
    prev = None
    print(f"{'sec':>4} {'R':>5} {'G':>5} {'B':>5}  {'lum':>5}  change")
    for i, f in enumerate(fs):
        r, g, b = stats(f)
        lum = 0.299 * r + 0.587 * g + 0.114 * b
        ch = ""
        if prev:
            d3 = abs(r - prev[0]) + abs(g - prev[1]) + abs(b - prev[2])
            if d3 > 40:
                ch = f"<== SCENE CHANGE (delta {d3:.0f})"
        print(f"{i+1:>4} {r:5.1f} {g:5.1f} {b:5.1f}  {lum:5.1f}  {ch}")
        prev = (r, g, b)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__))
