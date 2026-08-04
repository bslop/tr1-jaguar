#!/usr/bin/env python3
"""Decode the 68k crash beacon from a screen capture.

startup.S's exc_catch paints a red border and then beacon_paint writes SEVEN
rows of 32 one-bit blocks into every framebuffer:

    row 0        : 0xA5A5A5A5   calibration (alternating, spans the full width)
    rows 1..6    : the six scratch longs at $820..$834 --
                     $820 magic (0xEEEE0000), $824 SSP, $828.. the exception
                     frame; SR and PC live in there for every 68000 frame format

Geometry, straight from beacon_paint:
    bit b (MSB first) -> x = 2 + (31-b)*10, 9 px wide (1 px gutter)
    row r             -> y = 2 + r*10,      7 px tall
    white 0xFFFF = 1, dark 0x2104 = 0

The framebuffer is 320 px wide. Captures are usually pillarboxed 1920x1080, so
the active area is located automatically and scaled to 320 px. LOWRES renders
120 lines stretched 2x vertically by the OP, which the row pitch absorbs since
everything is measured in fractions of the active area.

Usage:
    decode_beacon.py capture.png [--crop L,T,R,B] [--rows N] [--debug]
    decode_beacon.py --selftest        # synthesise a beacon and decode it
"""
import sys


def _find_active(im):
    """Bounding box of non-black content: the pillarboxed game area."""
    px = im.load()
    W, H = im.size
    x0, y0, x1, y1 = W, H, -1, -1
    step = max(1, min(W, H) // 240)
    for y in range(0, H, step):
        for x in range(0, W, step):
            r, g, b = px[x, y][:3]
            if r + g + b > 40:
                if x < x0: x0 = x
                if y < y0: y0 = y
                if x > x1: x1 = x
                if y > y1: y1 = y
    if x1 < 0:
        return None
    return (x0, y0, x1 + 1, y1 + 1)


def decode(im, crop=None, rows=7, debug=False):
    from PIL import Image
    if crop is None:
        crop = _find_active(im)
        if crop is None:
            raise SystemExit("no non-black content found - is the screen black?")
    im = im.convert('RGB').crop(crop)
    # normalise the active area to the 320px framebuffer width; the beacon
    # occupies rows*10 scanlines of it.
    W, H = im.size
    px = im.load()

    def sample(fx, fy):
        """fx,fy are framebuffer coords in a 320 x (rows*10) grid."""
        x = int((fx + 0.5) * W / 320.0)
        y = int((fy + 0.5) * H / float(rows * 10))
        x = max(0, min(W - 1, x)); y = max(0, min(H - 1, y))
        r, g, b = px[x, y][:3]
        return (r + g + b) / 3.0

    # Calibration row fixes the threshold: A5A5A5A5 alternates, so its blocks
    # give us a real white level and a real dark level from THIS capture.
    lo, hi = [], []
    want0 = 0xA5A5A5A5
    for b in range(31, -1, -1):
        v = sample(2 + (31 - b) * 10 + 4, 2 + 3)
        (hi if (want0 >> b) & 1 else lo).append(v)
    thr = (sum(lo) / len(lo) + sum(hi) / len(hi)) / 2.0
    if debug:
        print("calibration: dark~%.0f white~%.0f threshold %.0f" %
              (sum(lo) / len(lo), sum(hi) / len(hi), thr))

    out = []
    for r in range(rows):
        val = 0
        for b in range(31, -1, -1):
            v = sample(2 + (31 - b) * 10 + 4, 2 + r * 10 + 3)
            if v > thr:
                val |= (1 << b)
        out.append(val)
    return out, thr


def report(vals):
    names = ["calibration", "magic", "SSP", "frame+0", "frame+4", "frame+8", "frame+12"]
    print("row  %-12s value" % "meaning")
    for i, v in enumerate(vals):
        n = names[i] if i < len(names) else "frame+%d" % (4 * (i - 2))
        print("%3d  %-12s 0x%08X" % (i, n, v))
    ok_cal = vals[0] == 0xA5A5A5A5
    print()
    print("calibration %s (0x%08X, expected 0xA5A5A5A5)" %
          ("OK" if ok_cal else "MISMATCH -> grid misaligned, decode NOT trustworthy", vals[0]))
    if len(vals) > 1:
        print("magic       %s (0x%08X, expected 0xEEEE0000)" %
              ("OK" if vals[1] == 0xEEEE0000 else "MISMATCH", vals[1]))
    if not ok_cal:
        return
    # 68000 group-0/2 frames: the 4-word frame starts with SR then PC.
    if len(vals) > 4:
        sr = (vals[3] >> 16) & 0xFFFF
        pc = ((vals[3] & 0xFFFF) << 16) | ((vals[4] >> 16) & 0xFFFF)
        print()
        print("exception frame: SR = 0x%04X   PC = 0x%08X" % (sr, pc))
        print("  (SR bits: T=%d S=%d IPL=%d)" %
              ((sr >> 15) & 1, (sr >> 13) & 1, (sr >> 8) & 7))
        print("  look the PC up with:  m68k-linux-gnu-objdump -d build/openlara.elf | grep -i ' %x:'" % pc)


def selftest():
    """Paint a synthetic beacon exactly as beacon_paint does, then decode it."""
    from PIL import Image
    vals = [0xA5A5A5A5, 0xEEEE0000, 0x001FFFF0, 0x27040000, 0x91DC0000, 0x12345678, 0x9ABCDEF0]
    W, H = 320, len(vals) * 10
    im = Image.new('RGB', (W, H), (0, 0, 0))
    px = im.load()
    for r, val in enumerate(vals):
        for b in range(31, -1, -1):
            col = (255, 255, 255) if (val >> b) & 1 else (33, 33, 33)
            x0 = 2 + (31 - b) * 10
            for yy in range(2 + r * 10, 2 + r * 10 + 7):
                for xx in range(x0, min(W, x0 + 9)):
                    px[xx, yy] = col
    # emulate a pillarboxed 1920x1080 capture with 2x vertical stretch
    big = Image.new('RGB', (1920, 1080), (0, 0, 0))
    big.paste(im.resize((1430, 1050), Image.NEAREST), (245, 0))
    got, _ = decode(big, rows=len(vals))
    ok = got == vals
    print("SELFTEST %s" % ("PASS" if ok else "FAIL"))
    for i, (a, b) in enumerate(zip(vals, got)):
        print("  row %d expected 0x%08X got 0x%08X %s" % (i, a, b, "" if a == b else "  <-- MISMATCH"))
    return 0 if ok else 1


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if args[0] == "--selftest":
        return selftest()
    from PIL import Image
    path = args[0]
    crop = None
    rows = 7
    debug = "--debug" in args
    for i, a in enumerate(args):
        if a == "--crop" and i + 1 < len(args):
            crop = tuple(int(x) for x in args[i + 1].split(","))
        if a == "--rows" and i + 1 < len(args):
            rows = int(args[i + 1])
    vals, _ = decode(Image.open(path), crop=crop, rows=rows, debug=debug)
    report(vals)
    return 0


if __name__ == "__main__":
    sys.exit(main())
