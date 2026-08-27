#!/usr/bin/env python3
"""floor_coverage.py - find cells the COLLISION data calls walkable that the
room MESH does not cover.

WHY: run 22 drove Lara onto a legitimate-looking ledge (room 11, floor 6656,
1024 units of headroom) and the screen went 81% black with only Lara lit. Not
scenery, not above the ceiling, not the camera leaving the room, not a room
crossing - the renderer simply had nothing to draw there. Room 11's mesh spans
local X 1024..11264 while its sector grid is 12 cells wide (0..12288), so the
whole of column 11 is standable collision with no geometry behind it.

The player experience is "I fall into some blackness then the area".

WHAT IT CHECKS (first pass, deliberately cheap and certain):
a walkable cell whose CENTRE lies outside the room mesh's XZ bounding box can
have no face over it, by construction. That is a lower bound on the problem -
holes INSIDE the bbox exist too and need per-face coverage, which is a second
pass. Reporting a lower bound honestly beats a fuzzy full answer.

    tools/floor_coverage.py            summary per room
    tools/floor_coverage.py --tsv      ROOM CX CZ WORLDX WORLDZ FLOOR CEIL

☠️ Y GROWS DOWNWARD. floor > ceiling numerically; headroom = floor - ceiling.
☠️ Cells reading 0x7FFF (WALL) or 0x7FFE (OPENING, the room below supplies the
floor) are not floors and are skipped - the same rule ledge_census.py uses.
"""
import os, struct, sys

def _disc(p):
    """Level-set files moved to disc/ (gitignored) on 2026-08-19. Prefer that,
    fall back to the old tree root so the tool works either side of the move."""
    import os as _o
    d = _o.path.join(_o.path.dirname(p), "disc", _o.path.basename(p))
    return d if _o.path.exists(d) else p

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WALL, OPEN, CELL = 0x7FFF, 0x7FFE, 1024


def load(prefix="mrt"):
    idx = open(_disc(os.path.join(D, prefix + ".bin")), "rb").read()
    geom = open(_disc(os.path.join(D, prefix + "_geom.bin")), "rb").read()
    sect = open(_disc(os.path.join(D, prefix + "_sect.bin")), "rb").read()
    nroom = struct.unpack_from(">H", idx, 0)[0]
    rooms = []
    for r in range(nroom):
        goff, soff = struct.unpack_from(">II", idx, 8 + r * 8)
        # ☠️ soff BIT 31 IS THE WATER-ROOM FLAG, not part of the offset.
        # tr2jag_multiroom.py: `index.append((goff, soff | 0x80000000 if water))`
        # and main.c reads it back as `rwater[i] = (e[4] & 0x80)`. The CAVES have
        # no water room so this never bit there; Lara's Home room 18 is the pool
        # and an unmasked read walks off the end of the file.
        soff &= 0x7FFFFFFF
        # ☠️ HEADER IS 16 BYTES, not 6: >HHHHH (vcount,qcount,tcount,atlasW,
        # atlasH) then >hhh (offX,0,offZ). The 6-byte reading in the kernel
        # comment omits the atlas pair and the offsets, and parsing at +6 makes
        # the vertex stream look like garbage (X capped at 256).
        vc, qc, tc, _aw, _ah = struct.unpack_from(">HHHHH", geom, goff)
        offX, _pad, offZ = struct.unpack_from(">hhh", geom, goff + 10)
        vs = [struct.unpack_from(">hhhH", geom, goff + 16 + i * 8) for i in range(vc)]
        xS, zS = struct.unpack_from(">HH", sect, soff)
        ix, iz = struct.unpack_from(">ii", sect, soff + 4)
        cells = []
        for cx in range(xS):
            col = []
            for cz in range(zS):
                b = soff + 12 + (cx * zS + cz) * 6
                fy = struct.unpack_from(">h", sect, b)[0]
                cy = struct.unpack_from(">h", sect, b + 2)[0]      # CEILING
                col.append((fy & 0xFFFF if fy < 0 else fy, cy))
            cells.append(col)
        rooms.append(dict(r=r, vs=vs, xS=xS, zS=zS, ix=ix, iz=iz, cells=cells,
                          soff=soff, goff=goff, vc=vc, qc=qc, tc=tc,
                          offX=offX * 256, offZ=offZ * 256))
    return rooms, geom


def scan(rooms):
    out = []
    for rm in rooms:
        if not rm["vs"]:
            continue
        xs = [v[0] for v in rm["vs"]]
        zs = [v[2] for v in rm["vs"]]
        mnx, mxx, mnz, mxz = min(xs), max(xs), min(zs), max(zs)
        for cx in range(rm["xS"]):
            for cz in range(rm["zS"]):
                f, c = rm["cells"][cx][cz]
                if f in (WALL, OPEN):
                    continue
                lx = cx * CELL + CELL // 2          # cell centre, room-local
                lz = cz * CELL + CELL // 2
                if mnx <= lx <= mxx and mnz <= lz <= mxz:
                    continue                        # inside the mesh bbox
                out.append(dict(room=rm["r"], cx=cx, cz=cz,
                                off=rm["soff"] + 12 + (cx * rm["zS"] + cz) * 6,
                                wx=rm["ix"] + lx, wz=rm["iz"] + lz,
                                floor=struct.unpack(">h", struct.pack(">H", f))[0],
                                ceil=c,
                                bbox=(mnx, mxx, mnz, mxz)))
    return out


def faces(rm, geom):
    """Vertex-index lists for every face in a room.

    ☠️ THE FACE STRIDE IS NOT WHAT THE KERNEL COMMENT SAYS. With FACE_PLANES=1
    each face carries a 12-BYTE PLANE PREFIX before its indices, so a quad is
    12 + 4*2 + 8*2 = 36 bytes and a tri is 12 + 3*2 + 6*2 = 30 - not the 24/18
    the header block documents. Parsing at 24 makes every index garbage
    (they overflow vcount immediately, which is the tell). Verified against
    every room's blob length: qc*36 + tc*30 + <=4 bytes of 8-alignment padding.
    """
    goff = rm["goff"]
    vc, qc, tc = rm["vc"], rm["qc"], rm["tc"]
    qb = goff + 16 + vc * 8
    out = [struct.unpack_from(">4H", geom, qb + i * 36 + 12) for i in range(qc)]
    tb = qb + qc * 36
    out += [struct.unpack_from(">3H", geom, tb + i * 30 + 12) for i in range(tc)]
    return out


def scan_faces(rooms, geom):
    """Walkable cells with NO FACE over them, even though they sit INSIDE the
    room mesh's bounding box. The bbox pass (scan) cannot see these.

    Confirmed case: room 22 cell (17,11), floor 6400 / ceiling 4608 - a valid
    1792-unit standing space, inside the mesh bbox - has **zero** faces whose XZ
    span contains it, and driving Lara there gives 60-73% black AT EVERY YAW
    (so it is not a dark corner, the geometry is absent).
    """
    out = []
    for rm in rooms:
        if not rm["vs"]:
            continue
        F = faces(rm, geom)
        V = rm["vs"]
        boxes = []
        for f in F:
            if any(i >= len(V) for i in f):
                continue
            xs = [V[i][0] for i in f]; zs = [V[i][2] for i in f]
            boxes.append((min(xs), max(xs), min(zs), max(zs)))
        for cx in range(rm["xS"]):
            for cz in range(rm["zS"]):
                fy, c = rm["cells"][cx][cz]
                if fy in (WALL, OPEN):
                    continue
                lx = cx * CELL + CELL // 2
                lz = cz * CELL + CELL // 2
                if any(a <= lx <= b and cc <= lz <= d for a, b, cc, d in boxes):
                    continue
                out.append(dict(room=rm["r"], cx=cx, cz=cz,
                                off=rm["soff"] + 12 + (cx * rm["zS"] + cz) * 6,
                                wx=rm["ix"] + lx, wz=rm["iz"] + lz,
                                floor=struct.unpack(">h", struct.pack(">H", fy))[0],
                                ceil=c, bbox=None))
    return out


def scan_headroom(rooms):
    """Walkable cells with NO VERTICAL SPACE: floor <= ceiling (Y grows DOWN, so
    a real room has floor > ceiling numerically).

    Found by driving: in room 11 Lara walks freely until cell (2,4), which reads
    floor 6400 / ceiling 6400 - zero headroom - where she STOPS DEAD and the
    screen goes 53% black. Collision lets her enter a cell that has no space in
    it and no geometry to draw. Same visible symptom as the border-ring floors,
    different cause, so it needs its own pass.

    Conservative on purpose: only headroom <= 0. Lara needs ~762 units to stand,
    so cells with a small positive headroom are ALSO unstandable - but TR1 has
    legitimate crawlspaces, and walling those would change level topology on a
    guess. Zero-or-inverted is certain.
    """
    out = []
    for rm in rooms:
        for cx in range(rm["xS"]):
            for cz in range(rm["zS"]):
                f, c = rm["cells"][cx][cz]
                if f in (WALL, OPEN):
                    continue
                fs = struct.unpack(">h", struct.pack(">H", f))[0]
                if fs - c > 0:
                    continue
                out.append(dict(room=rm["r"], cx=cx, cz=cz,
                                off=rm["soff"] + 12 + (cx * rm["zS"] + cz) * 6,
                                wx=rm["ix"] + cx * CELL + CELL // 2,
                                wz=rm["iz"] + cz * CELL + CELL // 2,
                                floor=fs, ceil=c, bbox=None))
    return out


def patch(bad):
    """Wall off every flagged cell. floorY -> 0x7FFF (WALL), in place.

    ☠️ SAME SIZE, ALWAYS. mrt.bin's per-room offsets index into mrt_sect.bin,
    and the ROM layout feeds the A10 boot lottery - a resize would move both.
    Two bytes overwritten per cell, nothing inserted.

    ☠️ RUN THIS *AFTER* mrt_boundary_audit.py --patch, never before. That pass
    converts 403 seam cells to 0x7FFE OPENING, and scan() deliberately skips
    OPENING cells: a doorway on the border ring IS legitimately walkable
    because the neighbouring room supplies its floor. Reversing the order
    would wall real doorways and seal the level.
    """
    pfx = sys.argv[sys.argv.index("--prefix") + 1] if "--prefix" in sys.argv else "mrt"
    sect_path = _disc(os.path.join(D, pfx + "_sect.bin"))
    bak = sect_path + ".precov"
    if not os.path.exists(bak):
        open(bak, "wb").write(open(sect_path, "rb").read())
        print("backup written: %s" % bak)
    buf = bytearray(open(sect_path, "rb").read())
    before = len(buf)
    for b in bad:
        buf[b["off"]] = 0x7F
        buf[b["off"] + 1] = 0xFF
    open(sect_path, "wb").write(buf)
    # ☠️ name the file we ACTUALLY wrote - the old message hardcoded
    # "mrt_sect.bin" and printed it while patching gym_sect.bin, which reads as
    # a patch applied to the wrong level.
    print("WALLED %d cells in %s (size %d, unchanged: %s)"
          % (len(bad), os.path.basename(sect_path), len(buf), before == len(buf)))


def main():
    tsv = "--tsv" in sys.argv
    do_patch = "--patch" in sys.argv
    pfx = sys.argv[sys.argv.index("--prefix") + 1] if "--prefix" in sys.argv else "mrt"
    rooms, geom = load(pfx)
    bad = scan(rooms)
    facehole = scan_faces(rooms, geom)
    tight = scan_headroom(rooms)
    seen = {b["off"] for b in bad}
    bad += [t for t in tight if t["off"] not in seen]
    # ☠️ CORRECTION (run 39): this pass was BLAMED for a 100%-black ROM in run
    # 33 and that was WRONG. Reverting it did not restore rendering; the real
    # cause was cobweb bf31dee's jcc68k ABI change. Re-tested on the mansion
    # with the good toolchain: walling all 84 face-holes renders fine (3.1%
    # black) and FIXED four spots where Lara fell instead of climbing - room 8
    # had 76 of them. NO-CLIMB went 6 -> 2 (the 2 are correct WALL refusals).
    # ★ A remedy blamed while another variable was uncontrolled deserves a
    #   re-test once that variable is fixed, not a permanent black mark.
    # Still OPT-IN, because it is a coarser test than the bbox pass (a face
    # bounding box over-covers) and because a genuine hole may want GEOMETRY
    # rather than removed collision. Turn it on deliberately, per level.
    if "--faces" in sys.argv:
        seen = {b["off"] for b in bad}
        bad += [t for t in facehole if t["off"] not in seen]
    if do_patch:
        patch(bad)
        return
    if tsv:
        for b in bad:
            print("%d\t%d\t%d\t%d\t%d\t%d\t%d" %
                  (b["room"], b["cx"], b["cz"], b["wx"], b["wz"], b["floor"], b["ceil"]))
        return
    total = sum(sum(1 for cx in range(r["xS"]) for cz in range(r["zS"])
                    if r["cells"][cx][cz][0] not in (WALL, OPEN))
                for r in rooms)
    per = {}
    for b in bad:
        per[b["room"]] = per.get(b["room"], 0) + 1
    print("STANDABLE CELLS WITH NO MESH OVER THEM (outside the room's vertex bbox)")
    print("  %d of %d walkable cells across %d rooms (%.1f%%)"
          % (len(bad), total, len(rooms), 100.0 * len(bad) / total if total else 0))
    for r in sorted(per, key=lambda k: -per[k]):
        rm = rooms[r]
        xs = [v[0] for v in rm["vs"]]; zs = [v[2] for v in rm["vs"]]
        print("  room %-3d %3d cells   mesh X %5d..%-5d Z %5d..%-5d   grid %dx%d (%d x %d)"
              % (r, per[r], min(xs), max(xs), min(zs), max(zs),
                 rm["xS"], rm["zS"], rm["xS"] * CELL, rm["zS"] * CELL))


if __name__ == "__main__":
    main()
