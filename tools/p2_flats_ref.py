#!/usr/bin/env python3
"""
P2 REFERENCE: sector FLATS by exact VISPLANE accumulation, offline.

Supersedes the ray-march version (2026-08-02).  That one resolved each pixel by
marching z with a distance tolerance, so coverage could be understated and spans
fragmented - the px/span ratio survived but the absolute counts were soft.  This
is exact.

★ THE KEY STRUCTURAL FACT (and the reason a flats renderer is cheap):
  For a HORIZONTAL plane at world height h, the screen ROW alone determines the
  distance:
        Z = FOCAL_Y * (h - cam_y) / (y - CY)
  Z does not depend on the column.  So every row of a flat is a CONSTANT-Z
  segment, and coverage can be solved analytically - one divide per (row,
  height), then a line/square intersection per cell.  No per-vertex transform,
  no per-pixel march, no z-buffer needed: at a given row, the nearest surface is
  simply the one with the smallest positive Z.

  That is Doom's visplane, and it is why this collapses many faces into one span:
  a floor at one height is ONE run per row however many sectors tile it.

★ THE MEASUREMENT IS A CONTROLLED A/B FROM ONE RASTERISATION.  Identical pixels,
  two groupings:
     polygon spans - runs grouped by CELL (what our kernel draws today: one
                     face per sector, so a run breaks at every cell boundary)
     sector  spans - runs grouped by (height, texture)  (what a visplane draws)
  So the ratio is measured on the same content, not compared against a
  remembered "~9 px/span" from another campaign.

Usage:
    python3 tools/p2_flats_ref.py [sr_sr.bin] [--room N] [--yaws 8]
"""
import sys, struct, math

FOCAL, FOCAL_Y = 160.0, 80.0
RENDER_W, RENDER_H = 320, 120
CX, CY = RENDER_W / 2.0, RENDER_H / 2.0
NEAR = 64.0
NONE = 0xFFFF
SOLID = -127


def load(path):
    b = open(path, 'rb').read()
    nr, = struct.unpack_from(">H", b, 0)
    idx, off = {}, 2
    for _ in range(nr):
        ri, o = struct.unpack_from(">HI", b, off); off += 6
        idx[ri] = o
    base = (off + 7) & ~7
    rooms = {}
    for ri, o in idx.items():
        p = base + o
        magic, xS, zS, nseg = struct.unpack_from(">HHHH", b, p); p += 8
        if magic != 0x5232:
            raise SystemExit("stale sr blob (magic %04x): re-run the extractor "
                             "with SECTORBUILD=1 - cells must carry heights" % magic)
        cells = []
        for _ in range(xS * zS):
            ft, ct, fy, cy = struct.unpack_from(">HHbb", b, p); p += 6
            cells.append((ft, ct, fy, cy))
        segs = []
        for _ in range(nseg):
            a, nb, c, lo, hi = struct.unpack_from(">BBhhh", b, p); p += 8
            bands = []
            for _ in range(3):
                y0, y1, tx = struct.unpack_from(">hhH", b, p); p += 6
                if tx != NONE: bands.append((y0, y1, tx))
            segs.append((a, c, lo, hi, bands[:nb]))
        rooms[ri] = dict(xS=xS, zS=zS, cells=cells, segs=segs)
    return rooms


def build_planes(room):
    """Group cell surfaces into VISPLANES: (height, texture) -> [cell squares].
    This is the grouping the renderer exploits; doing it up front is what makes
    a span able to cross sector boundaries."""
    xS, zS, cells = room['xS'], room['zS'], room['cells']
    planes = {}
    surfaces = []          # (height, tex, x0, x1, z0, z1, cellid) for the control arm
    for sx in range(xS):
        for sz in range(zS):
            ft, ct, fy, cy = cells[sx * zS + sz]
            if fy == SOLID:
                continue
            x0, x1 = sx * 1024.0, (sx + 1) * 1024.0
            z0, z1 = sz * 1024.0, (sz + 1) * 1024.0
            cid = sx * zS + sz
            for kind, h_click, tex in (('f', fy, ft), ('c', cy, ct)):
                if tex == NONE or h_click == SOLID:
                    continue
                h = h_click * 256.0
                planes.setdefault((h, tex, kind), []).append((x0, x1, z0, z1))
                surfaces.append((h, tex, kind, x0, x1, z0, z1, cid))
    return planes, surfaces


def render(room, cam, yaw_deg):
    """Exact per-row visplane resolve.  Returns (tag_sector, tag_poly) where each
    is a RENDER_W*RENDER_H list; tag_sector identifies the (height,texture) a
    pixel belongs to, tag_poly identifies the CELL.  Same pixels, two groupings."""
    planes, surfaces = build_planes(room)
    cx, cyw, cz = cam
    a = math.radians(yaw_deg)
    sa, ca = math.sin(a), math.cos(a)

    tag_s = [None] * (RENDER_W * RENDER_H)
    tag_p = [None] * (RENDER_W * RENDER_H)
    best = [None] * RENDER_W          # per-row nearest Z

    for sy in range(RENDER_H):
        yr = sy - CY
        if yr == 0:
            continue
        for i in range(RENDER_W):
            best[i] = None
        row_off = sy * RENDER_W
        for (h, tex, kind, x0, x1, z0, z1, cid) in surfaces:
            dh = h - cyw
            # floors are below the camera and appear below the horizon; ceilings
            # above.  Same sign test does both.
            if dh == 0 or (dh > 0) != (yr > 0):
                continue
            Z = FOCAL_Y * dh / yr
            if Z < NEAR:
                continue
            # world line at camera distance Z:  P(X) = (ox + X*ca, oz - X*sa)
            ox = cx + Z * sa
            oz = cz + Z * ca
            lo, hi = -1e18, 1e18
            if abs(ca) > 1e-9:
                t0, t1 = (x0 - ox) / ca, (x1 - ox) / ca
                if t0 > t1: t0, t1 = t1, t0
                lo, hi = max(lo, t0), min(hi, t1)
            elif not (x0 <= ox <= x1):
                continue
            if abs(sa) > 1e-9:
                t0, t1 = (oz - z1) / sa, (oz - z0) / sa
                if t0 > t1: t0, t1 = t1, t0
                lo, hi = max(lo, t0), min(hi, t1)
            elif not (z0 <= oz <= z1):
                continue
            if hi <= lo:
                continue
            # camera X -> screen column
            sx0 = CX + FOCAL * lo / Z
            sx1 = CX + FOCAL * hi / Z
            if sx1 < sx0: sx0, sx1 = sx1, sx0
            i0 = max(0, int(math.ceil(sx0 - 0.5)))
            i1 = min(RENDER_W - 1, int(math.floor(sx1 - 0.5)))
            for i in range(i0, i1 + 1):
                b = best[i]
                if b is None or Z < b[0]:
                    best[i] = (Z, h, tex, kind, cid)
        for i in range(RENDER_W):
            b = best[i]
            if b is not None:
                tag_s[row_off + i] = (b[1], b[2], b[3])     # height, tex, kind
                tag_p[row_off + i] = b[4]                   # cell id
    return tag_s, tag_p


def count_spans(tag):
    spans = 0
    for sy in range(RENDER_H):
        run = None
        off = sy * RENDER_W
        for i in range(RENDER_W):
            v = tag[off + i]
            if v != run:
                if v is not None:
                    spans += 1
                run = v
    return spans


def main():
    args = sys.argv[1:]
    path = args[0] if args and not args[0].startswith('--') else 'sr_sr.bin'
    def opt(n, d): return args[args.index(n) + 1] if n in args else d
    want = int(opt('--room', '26'))
    nyaw = int(opt('--yaws', '8'))
    rooms = load(path)
    if want not in rooms:
        raise SystemExit("room %d not in blob; have %s" % (want, sorted(rooms)))
    rm = rooms[want]
    xS, zS, cells = rm['xS'], rm['zS'], rm['cells']
    gx, gz = xS // 2, zS // 2
    ft, ct, fy, cyc = cells[gx * zS + gz]
    if fy == SOLID:
        for i, c in enumerate(cells):
            if c[2] != SOLID:
                gx, gz, (ft, ct, fy, cyc) = i // zS, i % zS, c; break
    cam = (gx * 1024 + 512, fy * 256 - 512, gz * 1024 + 512)
    planes, surfaces = build_planes(rm)
    print("room %d: %dx%d sectors, %d drawable flat surfaces, %d VISPLANES"
          % (want, xS, zS, len(surfaces), len(planes)))
    print("camera %s (floor click %d)\n" % (cam, fy))
    print("  yaw   sector  polygon   ratio   px    px/span(sec)  px/span(poly)")
    tS = tP = tPX = 0
    for k in range(nyaw):
        yaw = 360.0 * k / nyaw
        ts, tp = render(rm, cam, yaw)
        s, p = count_spans(ts), count_spans(tp)
        px = sum(1 for v in ts if v is not None)
        tS += s; tP += p; tPX += px
        print("  %3d   %6d  %7d   %5.2fx  %5d   %6.1f        %6.1f"
              % (yaw, s, p, (p / s if s else 0), px,
                 (px / s if s else 0), (px / p if p else 0)))
    print("  ---")
    print("  MEAN  %6.0f  %7.0f   %5.2fx  %5.0f   %6.1f        %6.1f"
          % (tS / nyaw, tP / nyaw, (tP / tS if tS else 0), tPX / nyaw,
             (tPX / tS if tS else 0), (tPX / tP if tP else 0)))
    print()
    print("  sector = runs grouped by (height,texture) - a visplane span")
    print("  polygon= runs grouped by CELL - what the kernel draws today")
    print("  Identical pixels in both columns; only the grouping differs.")


if __name__ == '__main__':
    main()
