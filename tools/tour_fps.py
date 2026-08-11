#!/usr/bin/env python3
"""tour_fps.py - per-ROOM fps from ONE ROOMTOUR capture.

WHY NOT 38 ROLLS.  ROOMTOUR teleports through every room centre and holds each
for exactly ROOMTOUR_HOLD 60Hz fields, so a single recording contains all 38
rooms back to back.  One roll instead of thirty-eight.

WHY NOT DBGROOM FOR THE LABEL.  Its room number goes through menu_text, which
is 68000 pixels and measured at +60% of the frame all by itself - labelling the
rooms would change the thing being measured.  The holds are exact, so the
teleports are evenly spaced: find the scene cuts, check they land on a regular
comb, and the room index is just the segment number.

METHOD, per segment: count beacon transitions, exactly as beacon_fps_fast does
(same 60fps sample rate, same two-level threshold, same "search only the
beacon's own patch" rule).  The beacon point is located ONCE over the whole
clip so every room is read at the same pixel.

usage: tour_fps.py <roll.mkv> <band_h> [hold_fields] [n_rooms]
"""
import glob, os, subprocess, sys, tempfile
from PIL import Image

SCALE_W = 960


def picture_box(im):
    g = im.convert("L"); W, H = g.size; px = g.load()
    xs = max(1, W // 640); ys = max(1, H // 216)
    cols = [x for x in range(0, W, xs)
            if sum(1 for y in range(0, H, ys * 5) if px[x, y] > 40) > 3]
    rows = [y for y in range(0, H, ys)
            if sum(1 for x in range(0, W, xs * 5) if px[x, y] > 40) > 3]
    if not cols or not rows:
        return None
    return cols[0], rows[0], cols[-1] + xs, rows[-1] + ys


def main(mkv, band_h, hold=240, nrooms=38):
    d = tempfile.mkdtemp()
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", mkv,
                    "-vf", f"fps=60,scale={SCALE_W}:-1,format=gray",
                    "-y", os.path.join(d, "c%05d.png")], check=True)
    fs = sorted(glob.glob(os.path.join(d, "c*.png")))
    ims = [Image.open(f).convert("L") for f in fs]
    print(f"decoded {len(ims)} fields ({len(ims)/60.0:.1f}s)")

    bb = picture_box(ims[len(ims) // 2])
    x0, y0, x1, y1 = bb
    sx, sy = (x1 - x0) / 320.0, (y1 - y0) / float(2 * band_h)

    # locate the beacon ONCE, by largest swing, over the whole clip
    best = None
    for fx in range(34, 62):
        for fy in range(48, 64):
            X, Y = int(x0 + fx * sx), int(y0 + fy * sy)
            if not (0 <= X < ims[0].size[0] and 0 <= Y < ims[0].size[1]):
                continue
            v = [im.load()[X, Y] for im in ims]
            lo, hi = min(v), max(v)
            if best is None or (hi - lo) > best[0]:
                best = (hi - lo, X, Y, lo, hi, v)
    swing, BX, BY, lo, hi, v = best
    print(f"picture {x1-x0}x{y1-y0}; beacon at ({BX},{BY}) swing {lo}->{hi}")
    if swing < 60:
        sys.exit("beacon not found - is FPSBEACON=1 in this build?")
    thr = (lo + hi) / 2.0
    b = [1 if t > thr else 0 for t in v]

    # scene cuts: mean abs frame diff over the picture, subsampled
    step = 6
    prev = None; diff = []
    for im in ims:
        px = im.load()
        cur = [px[x, y] for y in range(y0, y1, step) for x in range(x0, x1, step)]
        diff.append(0 if prev is None else
                    sum(abs(a - c) for a, c in zip(cur, prev)) / len(cur))
        prev = cur
    order = sorted(range(len(diff)), key=lambda i: -diff[i])
    cuts = []
    for i in order:
        if all(abs(i - c) > hold * 0.5 for c in cuts):
            cuts.append(i)
        if len(cuts) >= nrooms + 2:
            break
    cuts.sort()
    gaps = [cuts[i+1] - cuts[i] for i in range(len(cuts) - 1)]
    gaps.sort()
    med = gaps[len(gaps)//2] if gaps else hold
    print(f"found {len(cuts)} cuts, median spacing {med} fields "
          f"(hold={hold})")
    if abs(med - hold) > hold * 0.15:
        print("☠️ spacing does NOT match the hold - segmentation is unsafe, "
              "not reporting per-room numbers")
        return
    # ☠️ USE THE CUTS THEMSELVES, NOT A PERIOD.  Two traps, both hit:
    #  - the measured spacing is 244 captured fields against a hold of 240, so
    #    stepping by the nominal value drifts half a room by the end.  (244/240
    #    is ~1.7% of VBLs not advancing frame_count - the game's own clock runs
    #    slower than the display under load, which is a finding in itself.)
    #  - a fitted comb anchored at the FIRST cut labelled a 2-frame boot
    #    segment "room 0" and shifted every index by one.
    # Consecutive teleports bound each room exactly, and the tour ANCHORS AT
    # THE END: after the last teleport the camera never moves again, so the
    # final cut is the entry to the LAST room.  Count backwards from it.
    runs_ok = []; cur = [0]
    for i in range(len(cuts) - 1):
        if abs((cuts[i+1] - cuts[i]) - med) < med * 0.25:
            cur.append(i + 1)
        else:
            runs_ok.append(cur); cur = [i + 1]
    runs_ok.append(cur)
    best_run = max(runs_ok, key=len)
    bounds = [cuts[k] for k in best_run]
    nseg = len(bounds) - 1
    print(f"regular run: {len(bounds)} teleports -> {nseg} bounded rooms")
    if nseg < nrooms - 2:
        print(f"☠️ only {nseg} bounded rooms for {nrooms} - capture too short "
              f"or cuts missed; not reporting")
        return
    # Anchor at the END: the last bounded segment is the last room the tour
    # visits.  A SETTLE window sits before the first teleport, so there is
    # normally one extra segment - drop the leading extras rather than label
    # them "room -1".
    first_room = nrooms - nseg
    if first_room < 0:
        bounds = bounds[-first_room:]
        nseg = len(bounds) - 1
        first_room = nrooms - nseg
    if first_room > 0:
        print(f"☠️ first {first_room} room(s) fell outside the regular run "
              f"and are NOT reported")
    print(f"\n{'room':>5} {'fields/frame (mode)':>20} {'frames':>7} {'fps':>7}")
    rows = []
    for k in range(nseg):
        r = first_room + k
        a, z = bounds[k], bounds[k+1]
        if z > len(b):
            break
        seg = b[a:z]
        runs, cur = [], 1
        for i in range(1, len(seg)):
            if seg[i] == seg[i-1]:
                cur += 1
            else:
                runs.append(cur); cur = 1
        runs.append(cur)
        tr = len(runs) - 1
        fps = tr / ((z - a) / 60.0)
        mode = max(set(runs), key=runs.count) if runs else 0
        rows.append((r, fps, tr, mode))
        print(f"{r:5d} {mode:20d} {tr:7d} {fps:7.2f}")
    if rows:
        f = sorted(x[1] for x in rows)
        print(f"\nmedian {f[len(f)//2]:.2f}   min {f[0]:.2f}   max {f[-1]:.2f}")
        print("worst rooms: " + ", ".join(
            f"r{r}={v:.2f}" for r, v, _, _ in sorted(rows, key=lambda x: x[1])[:6]))
    for im in ims:
        im.close()


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]),
         int(sys.argv[3]) if len(sys.argv) > 3 else 240,
         int(sys.argv[4]) if len(sys.argv) > 4 else 38)
