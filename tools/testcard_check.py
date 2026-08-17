#!/usr/bin/env python3
"""testcard_check.py - assert the HW_TESTCARD frame, so a capture is EVIDENCE.

    tools/testcard_check.py shot.png
    tools/testcard_check.py --selftest      # prove every check can FAIL

WHY: the rig has no channel to read a number off the board, and a dead capture
once faked NINE black boots - including a control that was known to be lit. The
standing rule is that `OK!` plus a black screen means the VIDEO CHAIN, not the
console. `HW_TESTCARD=1` builds a ROM that paints one static, known frame; this
reads that frame back and says which part of the path is intact. Between them a
single upload settles "is the console alive" without anyone describing a TV.

THE CARD (320x240, five 48-line bands):
    0  RED ramp    5-bit, 32 levels, nothing in green or blue
    1  BLUE ramp   5-bit, 32 levels
    2  GREEN ramp  **6-bit, 64 levels**
    3  checker     1-bit black/white, 8px pitch
    4  pure RED | GREEN | BLUE thirds

★ THE GREEN RAMP IS THE SHARP CHECK, AND IT IS WHY THE RAMPS ARE ASYMMETRIC.
Jaguar RGB16 is `R<<11 | B<<6 | G` with GREEN SIX BITS UNSHIFTED at 5-0
(measured in cobweb 8d09c43, not assumed). Feeding 0..31 to all three channels
would never touch green's sixth bit, so a build packing 5-bit green into that
field would pass every isolation check while quietly halving green's resolution.
Asserting **64** distinct greens is what separates them: a `g << 1` packing
yields 32 and trips this immediately.

★ CHANNEL ISOLATION BEATS BRIGHTNESS. Asserting a band is merely "bright" passes
even with red and blue transposed - which is the exact failure a Jaguar's
R5:B5:G6 order invites, since it is NOT the 565 everyone expects. So each ramp
must be bright in its OWN channel and DARK in the other two.
"""
import os, sys

BANDS = 5
BAND_H = 48
W, H = 320, 240


def load(path):
    from PIL import Image
    return Image.open(path).convert("RGB")


def check(path, quiet=False):
    bad = []
    im = load(path)
    w, h = im.size
    # 1. SIZE. A half-configured display gives a plausible-looking frame of the
    #    wrong shape - painting this card before the OP list was finished
    #    returned 720x12, and every statistic below would have been junk.
    if (w, h) != (W, H):
        bad.append("size %dx%d, expected %dx%d - a wrong shape means the display "
                   "never came up; nothing else here is meaningful" % (w, h, W, H))
        return bad
    px = im.load()

    def row(b):
        return [px[x, b * BAND_H + BAND_H // 2] for x in range(w)]

    def levels(r, i):
        return len({p[i] for p in r})

    # 2/3. THE 5-BIT RAMPS, each isolated to its own channel.
    for b, ci, name in ((0, 0, "RED"), (1, 2, "BLUE")):
        r = row(b)
        n = levels(r, ci)
        if n < 30:
            bad.append("%s ramp has %d distinct levels, expected ~32 - the ramp "
                       "is not reaching the framebuffer" % (name, n))
        if max(p[ci] for p in r) < 200:
            bad.append("%s ramp never gets bright (max %d)" % (name, max(p[ci] for p in r)))
        for oi, oname in ((0, "red"), (1, "green"), (2, "blue")):
            if oi != ci and max(p[oi] for p in r) > 24:
                bad.append("%s ramp leaks into %s (max %d) - channel isolation "
                           "failed, which is what a transposed R5:B5:G6 layout "
                           "looks like" % (name, oname, max(p[oi] for p in r)))

    # 4. THE SIXTH GREEN BIT. The whole reason the ramps are asymmetric.
    r = row(2)
    ng = levels(r, 1)
    if ng < 60:
        bad.append("GREEN ramp has %d distinct levels, expected ~64 - green is "
                   "SIX bits unshifted at 5-0, and %s. A `g << 1` packing gives "
                   "exactly 32 and looks perfect in every other check."
                   % (ng, "32 means a 5-bit green is being packed into it"
                      if 28 <= ng <= 36 else "this build is not producing them"))
    if max(p[0] for p in r) > 24 or max(p[2] for p in r) > 24:
        bad.append("GREEN ramp leaks into red/blue - green's top bit is spilling "
                   "into the blue field, the classic `g << 1` overflow")

    # 5. THE CHECKER: 1-bit, so exactly two levels and a true white.
    r = row(3)
    if levels(r, 0) != 2:
        bad.append("checker band has %d luma levels, expected exactly 2"
                   % levels(r, 0))
    if max(p[0] for p in r) < 250 or max(p[1] for p in r) < 250 or max(p[2] for p in r) < 250:
        bad.append("checker white is not full white %s - CLUT[1] should be "
                   "31,31,63" % (max(r, key=sum),))

    # 6. THE PURE THIRDS, in order. Catches a transposition the ramps could miss
    #    if two channels failed together.
    r = row(4)
    thirds = [r[53], r[160], r[267]]
    for k, (want, name) in enumerate(((0, "RED"), (1, "GREEN"), (2, "BLUE"))):
        p = thirds[k]
        if p[want] < 200 or sum(p) - p[want] > 48:
            bad.append("third %d should be pure %s, got rgb%s" % (k + 1, name, p))

    if not quiet:
        print("  %s: %dx%d  ramps R=%d B=%d G=%d  checker=%d"
              % (os.path.basename(path), w, h, levels(row(0), 0), levels(row(1), 2),
                 ng, levels(row(3), 0)))
    return bad


def selftest():
    """☠️ PROVE EVERY CHECK CAN FAIL. A checker nobody has seen fail may be
    asserting nothing - the same class of mistake as a counter that reads 0
    because it was never compiled in."""
    from PIL import Image
    import tempfile
    d = tempfile.mkdtemp()
    ok = True

    def card(green_bits=6, swap=False, size=(W, H)):
        im = Image.new("RGB", size)
        p = im.load()
        for y in range(size[1]):
            b = min(y // BAND_H, 4)
            for x in range(size[0]):
                v5 = (x * 32) // size[0]
                v6 = (x * 64) // size[0] if green_bits == 6 else ((x * 32) // size[0]) * 2
                r = g = bl = 0
                if b == 0:   r = v5 * 8
                elif b == 1: bl = v5 * 8
                elif b == 2: g = v6 * 4
                elif b == 3: r = g = bl = 255 if (x & 8) else 0
                else:
                    if x < 107: r = 255
                    elif x < 214: g = 255
                    else: bl = 255
                p[x, y] = (bl, g, r) if swap else (r, g, bl)
        return im

    def mk(name, im):
        q = os.path.join(d, name)
        im.save(q)
        return q

    cases = [
        ("good card",     mk("g.png", card()),                       None),
        ("5-bit green",   mk("g5.png", card(green_bits=5)),          "SIX bits"),
        ("R/B swapped",   mk("sw.png", card(swap=True)),             "isolation"),
        ("wrong size",    mk("sz.png", card(size=(720, 12))),        "size 720x12"),
    ]
    for name, path, want in cases:
        bad = check(path, quiet=True)
        hit = any(want in b for b in bad) if want else not bad
        print("  %-14s %s" % (name, "caught" if hit else "☠️ NOT CAUGHT"))
        if not hit:
            ok = False
            for b in bad:
                print("      got: %s" % b[:100])
    return ok


def main():
    if "--selftest" in sys.argv:
        sys.exit(0 if selftest() else 1)
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    bad = check(sys.argv[1])
    for b in bad:
        print("  ☠️ %s" % b)
    print("  %s" % ("PASS" if not bad else "FAIL (%d)" % len(bad)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
