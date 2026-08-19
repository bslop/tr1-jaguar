#!/usr/bin/env python3
"""room_compare.py - compare EVERY extracted room against the ORIGINAL level.

WHY.  Room 10 rendered with 22% of the screen never drawn and room 12 keeps an
~8% hole that no renderer flag touches (near plane, portal clip rect and the
empty-window room drop were each measured and each ruled out).  That points at
the GEOMETRY rather than the renderer - so check it against the source.

☠️☠️ READ-ONLY BY CONSTRUCTION.  It NEVER imports or runs the extractor:
`tools/tr2jag_multiroom.py` has a hardcoded OUTDIR and a bare run replaces the
38-room level with a 5-room stand-in, with git staying clean because the .bin
files are gitignored.  This re-implements the room parser instead.

THE INVARIANT IS AREA, NOT FACE COUNT.  The extractor SUBDIVIDES faces
(SUBDIV_MAX), so our counts are legitimately much higher than the original's.
Subdivision preserves total surface area; a DROPPED face does not.  So compare
area per room and flag any room where ours is materially short.

usage: room_compare.py [--level PATH] [--top N]
"""
import os, struct, sys

def _disc(p):
    """Level-set files moved to disc/ (gitignored) on 2026-08-19. Prefer that,
    fall back to the old tree root so the tool works either side of the move."""
    import os as _o
    d = _o.path.join(_o.path.dirname(p), "disc", _o.path.basename(p))
    return d if _o.path.exists(d) else p

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVEL = os.path.join(D, "assets/extracted/PSXDATA/LEVEL1.PSX")


class R:
    def __init__(s, d): s.d = d; s.p = 0
    def setpos(s, p): s.p = p
    def seek(s, n): s.p += n
    def u8(s):  v = s.d[s.p]; s.p += 1; return v
    def u16(s): v = struct.unpack_from("<H", s.d, s.p)[0]; s.p += 2; return v
    def s16(s): v = struct.unpack_from("<h", s.d, s.p)[0]; s.p += 2; return v
    def u32(s): v = struct.unpack_from("<I", s.d, s.p)[0]; s.p += 4; return v
    def s32(s): v = struct.unpack_from("<i", s.d, s.p)[0]; s.p += 4; return v


TILE_PAGE_BYTES = 256*256//2
CLUT_BYTES      = 16*2
NUM_TILES       = 13
NUM_CLUTS       = 1024


def read_orig_rooms(path):
    """Same layout as tr2jag_multiroom.read_all_rooms, geometry fields only.
    ☠️ The room list does NOT start at a fixed offset - it sits after the
    texture pages and the CLUT table, so the header walk below must match
    tr2jag_multiroom.main() exactly or the parse runs off the end."""
    r = R(open(path, "rb").read())
    r.u32(); r.u32(); r.seek(8)
    off = r.u32()
    tiles_off = off + 8
    cluts_off = tiles_off + NUM_TILES * TILE_PAGE_BYTES
    r.setpos(cluts_off + NUM_CLUTS * CLUT_BYTES)
    r.seek(4)
    n = r.u16()
    out = []
    for ri in range(n):
        ix = r.s32(); iz = r.s32(); r.s32(); r.s32()
        size = r.u32(); start = r.p; r.seek(2)
        vc = r.s16(); verts = []
        for _ in range(vc):
            x = r.s16(); y = r.s16(); z = r.s16(); r.u16()
            verts.append((x, y, z))
        rc = r.s16(); quads = []
        for _ in range(rc):
            v = [r.u16(), r.u16(), r.u16(), r.u16()]; r.u16()
            v[2], v[3] = v[3], v[2]                 # PSX quad swap
            quads.append(v)
        tc = r.s16(); tris = []
        for _ in range(tc):
            v = [r.u16(), r.u16(), r.u16()]; r.u16()
            tris.append(v)
        r.setpos(start + size * 2)
        npor = r.u16()
        for _ in range(npor):
            r.u16(); r.seek(6); r.seek(8 * 3)       # adj, normal, 4 verts s16*3
        zS = r.u16(); xS = r.u16()
        r.seek(zS * xS * 8)
        r.seek(2); r.seek(r.u16() * 20)             # ambient, lights
        nsm = r.u16(); r.seek(nsm * 20)             # static placements
        r.u16(); r.u16()                            # alternate, flags
        out.append(dict(i=ri, ix=ix, iz=iz, verts=verts, quads=quads, tris=tris,
                        xS=xS, zS=zS))
    return out


def tri_area(a, b, c):
    ux, uy, uz = b[0]-a[0], b[1]-a[1], b[2]-a[2]
    vx, vy, vz = c[0]-a[0], c[1]-a[1], c[2]-a[2]
    cx, cy, cz = uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx
    return 0.5 * (cx*cx + cy*cy + cz*cz) ** 0.5


def orig_area(rm):
    A = 0.0
    for v in rm['quads']:
        p = [rm['verts'][i] for i in v]
        A += tri_area(p[0], p[1], p[2]) + tri_area(p[0], p[2], p[3])
    for v in rm['tris']:
        p = [rm['verts'][i] for i in v]
        A += tri_area(p[0], p[1], p[2])
    return A


def read_ours(dirn, face_planes):
    """mrt.bin index + mrt_geom.bin blobs (room0_tex format)."""
    mrt = open(os.path.join(dirn, "mrt.bin"), "rb").read()
    geom = open(os.path.join(dirn, "mrt_geom.bin"), "rb").read()
    n = struct.unpack_from(">H", mrt, 0)[0]
    offs = []
    for i in range(n):
        goff, soff = struct.unpack_from(">II", mrt, 8 + i*8)
        offs.append(goff & 0x7FFFFFFF)
    rooms = []
    QP = 12 if face_planes else 0            # FACE_PLANES prepends a 12B plane
    for i, o in enumerate(offs):
        vc, qc, tc = struct.unpack_from(">HHH", geom, o)
        ox, oy, oz = struct.unpack_from(">hhh", geom, o + 10)
        vp = o + 16
        verts = []
        for k in range(vc):
            x, y, z = struct.unpack_from(">hhh", geom, vp + k*8)
            verts.append((x, y, z))
        fp = vp + vc*8
        A = 0.0
        for k in range(qc):
            base = fp + k*(QP + 24) + QP
            v = struct.unpack_from(">HHHH", geom, base)
            p = [verts[j] for j in v]
            A += tri_area(p[0], p[1], p[2]) + tri_area(p[0], p[2], p[3])
        fp += qc*(QP + 24)
        for k in range(tc):
            base = fp + k*(QP + 18) + QP
            v = struct.unpack_from(">HHH", geom, base)
            p = [verts[j] for j in v]
            A += tri_area(p[0], p[1], p[2])
        rooms.append(dict(i=i, vc=vc, qc=qc, tc=tc, area=A, off=(ox, oy, oz)))
    return rooms


def main():
    level = LEVEL
    for a in sys.argv[1:]:
        if a.startswith("--level"): level = a.split("=", 1)[1]
    fp = 1
    try:
        h = open(_disc(os.path.join(D, "mrt.h"))).read()
        fp = 1 if "MRT_FACE_PLANES 1" in h else 0
    except OSError:
        pass
    orig = read_orig_rooms(level)
    ours = read_ours(D, fp)
    print(f"original rooms: {len(orig)}   extracted rooms: {len(ours)}   "
          f"FACE_PLANES={fp}")

    # ☠️ MATCH BY ROOM ORIGIN, NEVER BY AREA. A greedy nearest-area match
    # paired five of our rooms onto originals another room had already claimed
    # and invented a "short" room that way. The blob header carries offX/offZ
    # where world = local + (off<<8), so (offX<<8, offZ<<8) IS the original's
    # (info_x, info_z) - an exact, unique key.
    bykey = {(o['ix'], o['iz']): o for o in orig}
    print(f"\n{'ours':>4} {'orig':>4} {'our faces':>9} {'orig faces':>10} "
          f"{'our area':>12} {'orig area':>12} {'ratio':>7}")
    short = []
    unmatched = []
    for r in ours:
        key = (r['off'][0] << 8, r['off'][2] << 8)
        o = bykey.get(key)
        if o is None:
            unmatched.append((r['i'], key)); continue
        oa = orig_area(o)
        if oa <= 0: continue
        ratio = r['area'] / oa
        nf_o = len(o['quads']) + len(o['tris'])
        print(f"{r['i']:>4} {o['i']:>4} {r['qc']+r['tc']:>9} {nf_o:>10} "
              f"{r['area']:>12,.0f} {oa:>12,.0f} {ratio:>7.3f}")
        if ratio < 0.97:
            short.append((r['i'], o['i'], ratio, oa - r['area']))
    if unmatched:
        print("\n☠️ NO ORIGIN MATCH (mapping assumption wrong):")
        for i, k in unmatched: print(f"   our room {i}: origin {k}")
    if short:
        print("\n☠️ ROOMS SHORT OF THE ORIGINAL (area lost = dropped faces):")
        for i, oi, ratio, lost in sorted(short, key=lambda t: t[2]):
            print(f"   our room {i:2d} (orig {oi:2d}): {ratio*100:5.1f}% of "
                  f"original area, {lost:,.0f} units missing")
    else:
        print("\nno room is short of its original area")


if __name__ == "__main__":
    main()
