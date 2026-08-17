#!/usr/bin/env python3
"""checkshot.py - ASSERT over a rendered frame's pixels. Never eyeball one again.

    tools/checkshot.py shot.png [--size 320x80] [--baseline mrt] [--min-colours 24]
    tools/checkshot.py --selftest          # prove every check can FAIL

WHY: reading frames by eye has produced FOUR phantom defects in this project, and
writing down "measure, don't eyeball" three times did not stop the fourth:
  * run 67  an unexplained mid-level "LOADING" screen, carried as an open bug for
            three runs; the frame was ordinary gameplay at 0.1% black. I had
            mis-attributed a contact-sheet panel to the wrong frame index.
  * run 69  "coverage holes" in the Caves that were OPEN SKY (TR1 has no skybox;
            38% of room 0's cells have no ceiling).
  * runs 53/54  NOEMPTYY=1 and NOSDCULL=1 build, report **illegal=0**, and render
            NOTHING - one a flat luma-76 field, one a fully uncovered screen. Both
            looked like working ROMs to every check I had.
  * run 53  a dead ctl session returned a **64x1** frame and I read its "0.0%
            uncovered" as a spectacular fix.
jag_viewpoint (2026-08-17) made the general point after inventing a phantom
geometry bug off a *working* image minutes after writing that same lesson down:
**a screenshot is evidence only when something reads its pixels**, and discipline
demonstrably does not scale - a script does.

★ ASSERTIONS MUST DISCRIMINATE, NOT CONFIRM. Their sharpest check is that the RED
band contains no BLUE: asserting a band is merely *bright* passes even with a
swapped R5:B5:G6 layout. The checks here are chosen the same way - each one has a
specific historical failure it would have caught, named above.

☠️ THIS IS NOT A TEST CARD. Ours are game scenes, so every check has to be a
scene-INDEPENDENT invariant. In particular black% is NOT one: open-sky rooms are
legitimately 38% black, which is why --baseline compares against the recorded
per-level number rather than a constant.
"""
import os, sys

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(path):
    from PIL import Image
    im = Image.open(path)
    g = im.convert("L")
    return im.size, list(g.getdata())


def check(path, size=None, baseline=None, min_colours=24, quiet=False):
    """Returns a list of failure strings; empty means the frame is sane."""
    bad = []
    (w, h), px = load(path)

    # 1. SIZE. A dead ctl session hands back a 64x1 frame whose statistics are
    #    meaningless but perfectly computable (run 53).
    if size and (w, h) != size:
        bad.append("size %dx%d, expected %dx%d - a wrong size usually means the "
                   "capture/ctl session died, and every number below is then junk"
                   % (w, h, size[0], size[1]))
        return bad                      # nothing else is worth measuring

    n = float(w * h)
    black = 100.0 * sum(1 for p in px if p < 8) / n
    white = 100.0 * sum(1 for p in px if p >= 250) / n
    colours = len(set(px))
    mx = max(px)

    # 2. IS ANYTHING THERE. A flat field has one luma; a rendered scene has many.
    #    This is the check that catches NOEMPTYY/NOSDCULL - builds that report
    #    illegal=0 and draw nothing (runs 53/54).
    if colours < min_colours:
        bad.append("only %d distinct luma values (need %d) - this is a FLAT FIELD, "
                   "not a scene; illegal=0 does not mean it rendered"
                   % (colours, min_colours))
    if mx < 32:
        bad.append("maxluma %d - nothing bright anywhere" % mx)

    # 3. THE RIGHT EDGE IS PAINTED. RCLIPFIX: spans are right-EXCLUSIVE but the
    #    span end was clamped to an INCLUSIVE clip value, so column w-1 was black
    #    in EVERY scene for months. Invisible by eye, free to fix once measured.
    #    Compare the last column against the one beside it rather than to a
    #    constant, so a legitimately dark scene does not trip it.
    col_last = [px[y * w + (w - 1)] for y in range(h)]
    col_prev = [px[y * w + (w - 2)] for y in range(h)]
    if sum(col_prev) > 8 * h and sum(col_last) * 8 < sum(col_prev):
        bad.append("the RIGHTMOST COLUMN is dark while column w-2 is lit "
                   "(sum %d vs %d) - the RCLIPFIX signature, an inclusive/"
                   "exclusive span-end mismatch" % (sum(col_last), sum(col_prev)))

    # 4. FULLY UNCOVERED. With HOLEVIS the clear is white, so a mostly-white frame
    #    means the renderer covered nothing (run 54's all-culls-off build).
    if white > 90.0:
        bad.append("%.1f%% of the frame is white - under HOLEVIS that is an "
                   "UNCOVERED screen, i.e. nothing was drawn" % white)

    # 5. BASELINE. Per level, because open-sky rooms are legitimately dark
    #    (run 69). A recorded number, never a constant.
    if baseline:
        bp = os.path.join(D, "tools", ".black_" + baseline)
        try:
            thresh = float(open(bp).read().strip())
            if black > thresh + 12.0:
                bad.append("%.1f%% black vs the %s baseline %.1f%% (+12 slack)"
                           % (black, baseline, thresh))
        except Exception as e:
            bad.append("no baseline tools/.black_%s (%s) - run room_black.py"
                       % (baseline, type(e).__name__))

    if not quiet:
        print("  %s: %dx%d  black %.1f%%  white %.1f%%  maxluma %d  colours %d"
              % (os.path.basename(path), w, h, black, white, mx, colours))
    return bad


def selftest():
    """☠️ PROVE EVERY CHECK CAN FAIL. A checker nobody has seen fail is a checker
    that might be asserting nothing - which is the same class of mistake as a
    round-trip verification performed in the units you assumed."""
    from PIL import Image
    import tempfile, random
    ok = True
    d = tempfile.mkdtemp()

    def mk(name, f, size=(320, 80)):
        im = Image.new("L", size)
        im.putdata([f(x, y) for y in range(size[1]) for x in range(size[0])])
        p = os.path.join(d, name)
        im.save(p)
        return p

    random.seed(1)
    cases = [
        ("flat field",      mk("flat.png", lambda x, y: 76),                 "FLAT FIELD"),
        ("wrong size",      mk("tiny.png", lambda x, y: 76, (64, 1)),        "size 64x1"),
        ("dark right col",  mk("rcol.png", lambda x, y: 0 if x >= 319 else 40 + (x * 7 + y * 3) % 200), "RIGHTMOST COLUMN"),
        ("all white",       mk("white.png", lambda x, y: 255),               "UNCOVERED"),
        ("healthy scene",   mk("good.png", lambda x, y: 20 + (x * 5 + y * 11) % 220), None),
    ]
    for name, path, want in cases:
        bad = check(path, size=(320, 80), min_colours=24, quiet=True)
        hit = any(want in b for b in bad) if want else not bad
        print("  %-16s %s" % (name, "caught" if hit else "☠️ NOT CAUGHT"))
        if not hit:
            ok = False
            for b in bad:
                print("      got: %s" % b[:90])
    return ok


def main():
    if "--selftest" in sys.argv:
        sys.exit(0 if selftest() else 1)
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    path = sys.argv[1]
    size = None
    if "--size" in sys.argv:
        w, _, h = sys.argv[sys.argv.index("--size") + 1].partition("x")
        size = (int(w), int(h))
    baseline = sys.argv[sys.argv.index("--baseline") + 1] if "--baseline" in sys.argv else None
    minc = int(sys.argv[sys.argv.index("--min-colours") + 1]) if "--min-colours" in sys.argv else 24
    bad = check(path, size, baseline, minc)
    for b in bad:
        print("  ☠️ %s" % b)
    print("  %s" % ("PASS" if not bad else "FAIL (%d)" % len(bad)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
