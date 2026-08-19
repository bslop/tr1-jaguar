#!/usr/bin/env python3
"""speckle_score.py - score FMV/display corruption off a console capture.

    tools/speckle_score.py shot.png [more.png ...]

Reports the percentage of pixels far from their 3x3 median, measured INSIDE THE
DRAWN PICTURE ONLY.

☠️ WHY THE DRAWN-REGION CROP IS NOT OPTIONAL (2026-08-18). Scoring the whole
frame makes any change that shrinks the picture look like a fix: the half-height
OP-object arm (bw2048) read 9.2% whole-frame against a 14% control and looked
like a breakthrough, because 64% of that frame was black and black scores zero.
Cropped to the drawn area it read 24.7% vs 19.5% - i.e. NO IMPROVEMENT, and the
opposite conclusion.

☠️ A FLAT FIELD SCORES 0.00% WHETHER OR NOT THE FAULT IS PRESENT. Run 7 proved
it (the fault substitutes plausible NEIGHBOURING bytes, invisible against one
palette index), and on 2026-08-18 this metric scored a flat YELLOW HANG SCREEN
as "perfectly clean". ALWAYS LOOK AT THE FRAME before believing a 0.00%.

Reference numbers from the jcc68k-miscompile hunt, same console/card/clip:
    corrupt (jcc68k 59e5896)   19-21%
    clean   (jcc68k 84d9fc2)    0.00%
    hold screen, no streaming   0.12%
"""
import sys
import numpy as np
from PIL import Image

def med3(a):
    p = np.pad(a, 1, mode='edge')
    return np.median(np.stack([p[dy:dy+a.shape[0], dx:dx+a.shape[1]]
                               for dy in range(3) for dx in range(3)]), axis=0)

for path in sys.argv[1:]:
    try:
        a = np.asarray(Image.open(path).convert('L'), dtype=np.float32)
    except Exception as e:
        print(f"  {path}: UNREADABLE ({e})"); continue
    rows = np.where(a.mean(axis=1) > 8)[0]
    cols = np.where(a.mean(axis=0) > 8)[0]
    name = path.split('/')[-1]
    if len(rows) < 20 or len(cols) < 20:
        print(f"  {name:16s} NO PICTURE (mean {a.mean():.1f}) - no signal, black boot, or a hang")
        continue
    sub = a[rows.min():rows.max()+1, cols.min():cols.max()+1]
    d = np.abs(sub - med3(sub))
    pct = 100.0 * float((d > 40).mean())
    print(f"  {name:16s} drawn {sub.shape[1]}x{sub.shape[0]}  speckle-in-picture {pct:6.2f}%")
