#!/usr/bin/env python3
"""sightline.py - what geometry actually exists in front of Lara at a spot?

    tools/sightline.py --prefix gym --at X,Y,Z,ROOM,YAW [--reach 5600] [--half 520]

WHY: runs 50-55 spent five runs hunting a mansion coverage hole through the
RENDERER - portal windows, room culls, near plane, screen-space rejects, the
world plane cull - eliminating each with measurements. The answer was that the
pixels had no geometry behind them at all, and this one query would have said so
on day one WITHOUT A SINGLE BUILD.

So: before blaming any cull for a hole, ask what is there to draw. Walks every
room's quads, keeps the ones whose centroid falls in a corridor ahead of the spot
along its yaw, and prints each room's face count and vertical span against the
floor Lara is standing on. A hole is explained the moment you see the spans stop
short of her feet.

☠️ Y GROWS DOWNWARD. A face with SMALLER y is HIGHER. "Above her" = y < her y.

Geometry layout (see AUTORUN_STATE): room blob header 16B `>HHHHH` vcount,
qcount, tcount, atlasW, atlasH then `>hhh` offX,0,offZ; verts `>hhhH` at +16;
quads 36B = 12B plane {(ny<<16)|nx, nz, d} + 4 u16 indices + 8 u16 UVs; tris 30B.
World = local + (off << 8) on X and Z only - Y is already world.
"""
import os, struct, sys

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# forward is (SIN(yaw), COS(yaw)) with yaw in 16-bit units, 65536 = full turn
import math


def rooms(prefix):
    idx = open(os.path.join(D, prefix + ".bin"), "rb").read()
    geom = open(os.path.join(D, prefix + "_geom.bin"), "rb").read()
    n = struct.unpack_from(">H", idx, 0)[0]
    out = []
    for r in range(n):
        goff, _ = struct.unpack_from(">II", idx, 8 + r * 8)
        vc, qc, tc, aw, ah = struct.unpack_from(">HHHHH", geom, goff)
        offX, _z, offZ = struct.unpack_from(">hhh", geom, goff + 10)
        ox, oz = offX << 8, offZ << 8
        V = [struct.unpack_from(">hhhH", geom, goff + 16 + i * 8)[:3] for i in range(vc)]
        fb = goff + 16 + vc * 8
        faces = []
        for i in range(qc):
            ii = struct.unpack_from(">HHHH", geom, fb + i * 36 + 12)
            faces.append(ii)
        tb = fb + qc * 36
        for i in range(tc):
            ii = struct.unpack_from(">HHH", geom, tb + i * 30 + 12)
            faces.append(ii)
        out.append(dict(r=r, V=V, faces=faces, ox=ox, oz=oz))
    return out


def main():
    pfx = sys.argv[sys.argv.index("--prefix") + 1] if "--prefix" in sys.argv else "mrt"
    at = sys.argv[sys.argv.index("--at") + 1].split(",")
    x, y, z, room, yaw = (int(v) for v in at)
    reach = int(sys.argv[sys.argv.index("--reach") + 1]) if "--reach" in sys.argv else 5600
    half = int(sys.argv[sys.argv.index("--half") + 1]) if "--half" in sys.argv else 520

    a = yaw * 2 * math.pi / 65536.0
    fx, fz = math.sin(a), math.cos(a)
    print("spot room %d at (%d,%d,%d) facing (%.2f,%.2f); corridor %d long, +-%d wide"
          % (room, x, y, z, fx, fz, reach, half))

    hits = {}
    for rm in rooms(pfx):
        for ii in rm["faces"]:
            xs = [rm["V"][j][0] + rm["ox"] for j in ii]
            ys = [rm["V"][j][1] for j in ii]
            zs = [rm["V"][j][2] + rm["oz"] for j in ii]
            cx, cz = sum(xs) / float(len(xs)), sum(zs) / float(len(zs))
            # project the centroid onto the facing direction and its perpendicular
            dx, dz = cx - x, cz - z
            along = dx * fx + dz * fz
            side = abs(-dx * fz + dz * fx)
            if 0 < along <= reach and side <= half:
                hits.setdefault(rm["r"], []).append((min(ys), max(ys), along))

    if not hits:
        print("  ☠️ NOTHING AT ALL in front of her - the whole view is a hole.")
        return
    print("  %-6s %-6s %-22s %s" % ("room", "faces", "y span (higher..lower)", "nearest"))
    for r in sorted(hits, key=lambda k: -len(hits[k])):
        v = hits[r]
        print("  %-6d %-6d %-22s %.0f"
              % (r, len(v), "%d .. %d" % (min(t[0] for t in v), max(t[1] for t in v)),
                 min(t[2] for t in v)))
    lowest = max(t[1] for v in hits.values() for t in v)
    print("  she stands at y=%d; the LOWEST geometry in view is y=%d" % (y, lowest))
    if lowest < y:
        print("  ☠️ EVERY face in view is ABOVE her feet by %d units - nothing covers"
              % (y - lowest))
        print("     the band between them. That is the hole, and it is MISSING")
        print("     GEOMETRY: no cull, portal window or clip can fill it.")


if __name__ == "__main__":
    main()
