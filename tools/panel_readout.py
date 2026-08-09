#!/usr/bin/env python3
"""panel_readout.py - decode the vidpanel read-out off a console capture.

The panel (vidpanel.c) draws its OWN reference frame so this decoder recovers
the grid instead of guessing at it:

    panel 160x234 at framebuffer (0,0), 2px white border
    8 lamps, 16x10 at x=4, y=4+i*12
    12 bit rows, 32 bits, 4px per bit, 8px tall, at x=24, y=102+i*11
    row 0 is a fixed A5A5A5A5 calibration word

☠️ THE RULE THAT MAKES IT TRUSTWORTHY: if row 0 does not decode to A5A5A5A5,
this prints NOTHING but a refusal.  The predecessor latched its bounding box
onto bright video and reported confident nonsense; the 8px VIDDIAG squares it
replaced produced three wrong diagnoses in one afternoon.  An instrument that
cannot fail loudly is worse than no instrument.

usage: panel_readout.py <capture.png> [more.png ...]
"""
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("panel_readout.py: needs Pillow (pip install pillow)")

PAN_W, PAN_H = 160, 234
BITX, BITW, BITY0, BITDY, BITH = 24, 4, 102, 11, 8
CALIB = 0xA5A5A5A5

ROWNAMES = {1: "frames", 2: "fields", 3: "read ms", 4: "tom ms", 5: "copy ms",
            6: "pace ms", 7: "audio ms", 8: "gpu pc", 9: "tomfail",
            10: "panel ver", 11: "paint ms"}
LAMPNAMES = ["heartbeat", "gpu_ok", "jv_ok", "jv_ok2", "hello", "tom done",
             "pc in sram", "alive"]


def sample(px, W, H, ox, oy, sx, sy, fx, fy):
    """mean luma of a small box centred on framebuffer pixel (fx, fy)"""
    x = ox + (fx + 0.5) * sx
    y = oy + (fy + 0.5) * sy
    x0, x1 = int(x - sx * 0.3), int(x + sx * 0.3) + 1
    y0, y1 = int(y - sy * 0.3), int(y + sy * 0.3) + 1
    if x0 < 0 or y0 < 0 or x1 > W or y1 > H:
        return None
    n = t = 0
    for yy in range(y0, y1):
        for xx in range(x0, x1):
            t += px[xx, yy]
            n += 1
    return t / n if n else None


def read_row(px, W, H, geom, i):
    ox, oy, sx, sy = geom
    y = BITY0 + i * BITDY + BITH // 2
    v = 0
    for b in range(32):
        s = sample(px, W, H, ox, oy, sx, sy, BITX + b * BITW + BITW / 2.0, y)
        if s is None:
            return None
        v = (v << 1) | (1 if s > 128 else 0)
    return v


def find_geometry(im):
    """recover the grid from the panel's own border, then PROVE it.

    ☠️ The horizontal and vertical scales are NOT the same number.  A Cam Link
    frame of this console is pillarboxed - measured 2026-08-08 on a 720x576
    capture, the 320x240 framebuffer landed 512 px wide (1.6 px per fb pixel)
    and full height (2.4).  Assuming one uniform scale made the decoder report
    NO PANEL on a capture whose panel is plainly visible, which is the good
    failure mode but still a failure.  So: find the border, then verify.
    """
    g = im.convert("L")
    W, H = g.size
    px = g.load()

    # the border is the only full-height white line in the left half of a
    # 160-wide panel; the pale video behind it reads ~200, white reads ~245
    colhit = [sum(1 for y in range(0, H, 3) if px[x, y] > 230) for x in range(W)]
    rowhit = [sum(1 for x in range(0, W, 3) if px[x, y] > 230) for y in range(H)]
    ncol, nrow = (H + 2) // 3, (W + 2) // 3
    cols = [x for x in range(W) if colhit[x] > ncol * 0.6]
    rows_ = [y for y in range(H) if rowhit[y] > nrow * 0.3]
    if len(cols) < 2 or not rows_:
        return px, W, H, []
    # The two vertical border lines are unmistakable - they are the only
    # full-height white columns.  Horizontal scale and origin come from them.
    xl = sum(x for x in cols if x < (cols[0] + cols[-1]) / 2) / \
         max(1, sum(1 for x in cols if x < (cols[0] + cols[-1]) / 2))
    xr = sum(x for x in cols if x > (cols[0] + cols[-1]) / 2) / \
         max(1, sum(1 for x in cols if x > (cols[0] + cols[-1]) / 2))
    sx = (xr - xl) / 158.0           # border centres: fb x=0.5 and x=158.5
    ox = xl - 0.5 * sx
    # Vertical: the bottom border can sit past the bottom of the capture, so
    # do not measure from it - scan the plausible range and let the
    # calibration word pick.  A 320x240 frame on a 480/576-line capture is
    # somewhere near 2.0-2.5 image rows per fb row.
    yt = rows_[0]
    best = []
    for i in range(-30, 31):
        sy = (H / 240.0) * (1 + i * 0.004)
        for doy in range(-6, 12):
            oy = yt - 0.5 * sy + doy
            geom = (ox, oy, sx, sy)
            if read_row(px, W, H, geom, 0) == CALIB:
                best.append(geom)
    return px, W, H, best


def decode(path):
    im = Image.open(path)
    px, W, H, cands = find_geometry(im)
    if not cands:
        print(f"{path}: NO PANEL - calibration word never decoded "
              f"(dark roll, wrong build, or the clip already ended)")
        return None
    # ☠️ Do not just take the middle of the plateau: a geometry can decode the
    # calibration word while sampling the LAST bit row off its band (caught on
    # the synthetic fixture - row 11 came back 0).  Score every candidate by
    # how far its samples sit from the 128 threshold, over ALL rows, and keep
    # the most confident one.
    # A candidate must survive a PITCH check, not just the calibration word:
    # row 0 only pins the origin, so an sy that is 3% off still decodes row 0
    # and reads row 11 out of its band (caught on the synthetic fixture).
    # Sample the FIRST and LAST rows near the top and the bottom of their 8px
    # bands - a geometry with the right pitch reads the same value from both.
    def stable(g):
        ox, oy, sx, sy = g
        for i in (0, 11):
            vals = []
            for dy in (1, BITH - 2):
                v = 0
                for b in range(32):
                    s = sample(px, W, H, ox, oy, sx, sy,
                               BITX + b * BITW + BITW / 2.0,
                               BITY0 + i * BITDY + dy)
                    if s is None:
                        return False
                    v = (v << 1) | (1 if s > 128 else 0)
                vals.append(v)
            if vals[0] != vals[1]:
                return False
        return True
    solid = [g for g in cands if stable(g)] or cands
    geom = sorted(solid)[len(solid) // 2]
    ox, oy, sx, sy = geom
    rows = [read_row(px, W, H, geom, i) for i in range(12)]
    if any(r is None for r in rows):
        print(f"{path}: panel partly off-frame - refusing to report")
        return None
    lamps = []
    for i in range(8):
        s = sample(px, W, H, ox, oy, sx, sy, 4 + 8, 4 + i * 12 + 5)
        lamps.append(1 if (s or 0) > 128 else 0)

    print(f"{path}:  (origin {ox:.1f},{oy:.1f} scale {sx:.3f},{sy:.3f})")
    print("  lamps: " + "  ".join(
        f"{LAMPNAMES[i]}={'*' if lamps[i] else '.'}" for i in range(8)))
    for i in range(1, 12):
        if rows[i] or i in (1, 2):
            print(f"  row{i:<2} {ROWNAMES.get(i,''):<10} {rows[i]:>10}")
    # hold-screen panel (bit31 of row 10): whole-clip totals + why it ended
    if rows[10] & 0x80000000:
        exits = {1: "played out", 2: "CORRUPT RECORD", 3: "GD READ FAILED",
                 4: "stream exhausted early", 5: "pad skip", 0: "(loop ended)"}
        vnf = rows[10] & 0xFFFF
        ec = (rows[10] >> 16) & 0xF
        # the end-of-file read reports "exhausted" on a clip that played out,
        # so the frame count is the authority, not the code
        why = ("played out" if rows[1] >= vnf and vnf
               else exits.get(ec, ec))
        print(f"  clip: {rows[1]} of {vnf} frames displayed, stopped at "
              f"fi={rows[11]} - {why}")
    fr, fl = rows[1], rows[2]
    if fr and fl:
        print(f"  ==> {fr} frames in {fl} fields = {fr * 60.0 / fl:.2f} fps")
        acc = sum(rows[i] for i in (3, 4, 5, 6, 7, 11))
        # counters are vp_tick units: half-lines, ~31.7us
        ms = lambda t: t * 31.7 / 1000.0 / fr
        print(f"  ==> per frame: read {ms(rows[3]):.0f}  tom {ms(rows[4]):.0f}"
              f"  copy {ms(rows[5]):.0f}  pace {ms(rows[6]):.0f}"
              f"  audio {ms(rows[7]):.0f}  paint {ms(rows[11]):.0f}"
              f"  | UNTIMED {fl * 1000.0 / 60.0 / fr - ms(acc):.0f} ms")
    return rows


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        decode(p)
