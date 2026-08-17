#!/usr/bin/env python3
"""holevis_scan.py - read HOLEVIS frames and say whether the gap is SKY or a HOLE.

    tools/holevis_scan.py /tmp/holevis/*.png [--prefix gym --room 0]

WHY: black pixels cannot be interrogated. TR1 has no skybox, so an open ceiling
renders black - and so does a coverage hole. Runs 50-59 spent nine runs on
"mansion holes" that were open sky, and run 94 hit the same wall from the other
side: 11 of 28 driven frames failed a black-percentage check purely because the
tour never left room 0, which is 39% open to the sky.

HOLEVIS=1 paints the per-frame clear WHITE, so uncovered pixels are unambiguous.
That gives the pixels an identity, but not yet a VERDICT - white is still both
sky and hole. This adds the discriminator:

  ★ SKY IS ABOVE THE HORIZON. A gap in the ceiling is uncovered sky and sits in
    the UPPER band. A dropped floor quad, a rejected near-plane wall or a portal
    clip gap is uncovered GROUND and reaches the BOTTOM rows. Uncovered pixels
    touching the bottom edge are therefore the ones worth chasing.

☠️ THIS IS A RATIO, NOT A THRESHOLD. It reports the split; it does not decide
what is acceptable. A frame looking straight up is legitimately all-sky. Compare
against the room's open-ceiling fraction, which is printed when --prefix/--room
are given (the same count sightline.py makes).

☠️ IT ONLY MEANS ANYTHING ON A HOLEVIS BUILD. On a normal build the clear is
BLACK, every frame reads 0% uncovered, and that would look like a clean bill of
health for a ROM full of holes. So the scan REFUSES a frame with no white at all
unless --allow-covered is passed, rather than quietly reporting zero.
"""
import os, struct, sys

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WHITE = 250          # HOLEVIS clears to palette 255; allow for any dither


def open_ceiling_fraction(prefix, room):
    """What fraction of this room's cells have NO ceiling - i.e. is open sky.
    Same computation sightline.py prints; -32768 is the 'no ceiling' marker."""
    idx = open(os.path.join(D, prefix + ".bin"), "rb").read()
    sect = open(os.path.join(D, prefix + "_sect.bin"), "rb").read()
    _, soff = struct.unpack_from(">II", idx, 8 + room * 8)
    soff &= 0x7FFFFFFF
    xS, zS = struct.unpack_from(">HH", sect, soff)
    n = xS * zS
    sky = sum(1 for i in range(n)
              if struct.unpack_from(">h", sect, soff + 12 + i * 6 + 2)[0] == -32768)
    return sky / float(n), sky, n


def scan(path):
    from PIL import Image
    im = Image.open(path).convert("L")
    w, h = im.size
    px = list(im.getdata())
    total = float(w * h)
    white = [i for i, v in enumerate(px) if v >= WHITE]
    if not white:
        return dict(size=(w, h), pct=0.0, top=0.0, bottom=0.0, floor_rows=0,
                    covered=True)
    # split at the horizon: the upper half can only be ceiling/sky from a
    # standing eye height, the lower half is where ground geometry lives
    half = h // 2
    up = sum(1 for i in white if i // w < half)
    dn = len(white) - up
    # ★ the sharpest signal: how many of the BOTTOM rows contain uncovered
    # pixels. Sky never reaches the bottom of the frame while she is standing.
    bottom_rows = 0
    for y in range(h - 1, half - 1, -1):
        if any(px[y * w + x] >= WHITE for x in range(w)):
            bottom_rows += 1
        else:
            break
    return dict(size=(w, h), pct=100.0 * len(white) / total,
                top=100.0 * up / total, bottom=100.0 * dn / total,
                floor_rows=bottom_rows, covered=False)


def main():
    files = [a for a in sys.argv[1:] if a.endswith(".png")]
    prefix = sys.argv[sys.argv.index("--prefix") + 1] if "--prefix" in sys.argv else None
    room = int(sys.argv[sys.argv.index("--room") + 1]) if "--room" in sys.argv else None
    allow = "--allow-covered" in sys.argv
    if not files:
        print(__doc__)
        sys.exit(2)

    if prefix is not None and room is not None:
        f, sky, n = open_ceiling_fraction(prefix, room)
        print("room %d of %s: %d of %d cells have NO CEILING (%.0f%%) - that is the"
              % (room, prefix, sky, n, 100.0 * f))
        print("  budget uncovered SKY is allowed to spend before it means anything")

    print("  %-14s %-9s %-8s %-8s %-8s %s"
          % ("frame", "size", "uncov%", "upper%", "lower%", "rows up from bottom"))
    worst, blank = None, 0
    for p in sorted(files):
        r = scan(p)
        if r["covered"]:
            blank += 1
        print("  %-14s %-9s %-8.1f %-8.1f %-8.1f %d"
              % (os.path.basename(p), "%dx%d" % r["size"], r["pct"], r["top"],
                 r["bottom"], r["floor_rows"]))
        if not r["covered"] and (worst is None or r["floor_rows"] > worst[1]["floor_rows"]):
            worst = (p, r)

    if blank == len(files) and not allow:
        print("  ☠️ EVERY frame is 0%% uncovered. On a HOLEVIS build that is possible")
        print("     but suspicious; on a NORMAL build it is guaranteed and means")
        print("     nothing. Confirm HOLEVIS reached the compile line, or pass")
        print("     --allow-covered if you really did capture a fully-covered set.")
        sys.exit(1)
    if worst:
        p, r = worst
        print("  worst: %s reaches %d rows up from the bottom edge"
              % (os.path.basename(p), r["floor_rows"]))
        if r["floor_rows"] == 0:
            print("  ✅ NO uncovered pixel touches the bottom of any frame - every gap")
            print("     is above the horizon, which is what open sky looks like.")
        else:
            print("  ☠️ uncovered GROUND: sky cannot reach the bottom rows while she")
            print("     is standing. Chase this one - near-plane face rejection and")
            print("     portal clip gaps both land here.")


if __name__ == "__main__":
    main()
