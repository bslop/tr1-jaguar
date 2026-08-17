#!/usr/bin/env python3
"""ledge_census.py - find every climbable ledge in the baked level, classified
the way TR1 classifies them, so the climb can be TESTED per type instead of
spot-checked wherever the player happens to be standing.

    python3 tools/ledge_census.py [--prefix mrt] [--class 2CLICK] [--top 8]

Reads mrt.bin + mrt_sect.bin (the SAME bytes the game collides against, so a
ledge listed here is a ledge the runtime sees) and reports, for every cell pair
that steps UP:

    room, world x/z to stand at, the yaw to face, the rise, and the TR1 class

TR1's own thresholds, from OpenLara's Lara::checkClimb (src/lara.h):
    rise <= 256              WALKUP     stepped over automatically
    256 <  rise <= 2*256+128 CLIMB2     ANIM_CLIMB_2, lands +512
    640 <  rise <= 3*256+128 CLIMB3     ANIM_CLIMB_3, lands +768
    896 <  rise <= 7*256+128 JUMPGRAB   ANIM_CLIMB_JUMP - jump + grab, NOT a
                                        standing climb even in the real game
    rise >  1920             WALL       nothing gets up it

☠️ Y GROWS DOWNWARD. A ledge ABOVE the floor you stand on has the SMALLER
floorY, so rise = standing_floor - ledge_floor and a POSITIVE rise is a step UP.

☠️ Cells whose floor reads 0x7FFF (wall) or 0x7FFE (opening: the room below
supplies the floor) are not floors and are skipped on both sides - baking the
seam height as ground is exactly the phantom-floor bug the boundary patch
exists to undo.

Only WITHIN-room neighbours are considered. Cross-room ledges exist but their
heights come from another room's blob via the portal, and the runtime resolves
that with room_floor_mr's own search; listing them here would report pairs the
collision code never compares.
"""
import os, struct, sys

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WALL, OPEN = 0x7FFF, 0x7FFE
CELL = 1024

# (name, low_exclusive, high_inclusive) - TR1's bands
CLASSES = [("WALKUP",   -10**9, 256),
           ("CLIMB2",   256,    2*256+128),
           ("CLIMB3",   2*256+128, 3*256+128),
           ("JUMPGRAB", 3*256+128, 7*256+128),
           ("WALL",     7*256+128, 10**9)]

def classify(rise):
    for name, lo, hi in CLASSES:
        if lo < rise <= hi:
            return name
    return "WALL"

# forward vector is (SIN(yaw), COS(yaw)) - see the ledge probe in main.c - so
# yaw 0 faces +Z and 16384 (90 degrees) faces +X.
DIRS = [(0, 1, 0, "+Z"), (1, 0, 16384, "+X"),
        (0, -1, -32768, "-Z"), (-1, 0, -16384, "-X")]


def load(prefix):
    idx = open(os.path.join(D, prefix + ".bin"), "rb").read()
    sect = open(os.path.join(D, prefix + "_sect.bin"), "rb").read()
    nroom = struct.unpack_from(">H", idx, 0)[0]
    rooms = []
    for r in range(nroom):
        _, soff = struct.unpack_from(">II", idx, 8 + r * 8)
        # ☠️ soff BIT 31 IS THE WATER-ROOM FLAG, not part of the offset.
        # tr2jag_multiroom.py: `index.append((goff, soff | 0x80000000 if water))`
        # and main.c reads it back as `rwater[i] = (e[4] & 0x80)`. The CAVES have
        # no water room so this never bit there; Lara's Home room 18 is the pool
        # and an unmasked read walks off the end of the file.
        soff &= 0x7FFFFFFF
        xS, zS = struct.unpack_from(">HH", sect, soff)
        ix, iz = struct.unpack_from(">ii", sect, soff + 4)
        cells = []
        for cx in range(xS):
            col = []
            for cz in range(zS):
                base = soff + 12 + (cx * zS + cz) * 6
                fy = struct.unpack_from(">h", sect, base)[0]
                # e[4]/e[5] are the X/Z SLANTS the runtime applies across the
                # cell (fy -= slant*frac). A sloped cell has no single floor
                # height, so a "rise" computed from its base value is not a
                # step - room 0 alone is 41/120 sloped and produced test spots
                # that were open hillside, not ledges.
                sx, sz = struct.unpack_from(">bb", sect, base + 4)
                col.append((fy & 0xFFFF if fy < 0 else fy, sx, sz))
            cells.append(col)
        rooms.append(dict(r=r, xS=xS, zS=zS, ix=ix, iz=iz, cells=cells))
    return rooms


def census(rooms):
    out = []
    for rm in rooms:
        xS, zS, cells = rm["xS"], rm["zS"], rm["cells"]
        for cx in range(xS):
            for cz in range(zS):
                here, hsx, hsz = cells[cx][cz]
                if here in (WALL, OPEN) or hsx or hsz:
                    continue                      # sloped: no single height
                hf = struct.unpack(">h", struct.pack(">H", here))[0]
                for dx, dz, yaw, label in DIRS:
                    nx, nz = cx + dx, cz + dz
                    if not (0 <= nx < xS and 0 <= nz < zS):
                        continue
                    there, tsx, tsz = cells[nx][nz]
                    if there in (WALL, OPEN) or tsx or tsz:
                        continue                  # sloped ledge top: skip
                    nf = struct.unpack(">h", struct.pack(">H", there))[0]
                    rise = hf - nf                 # +Y is DOWN: positive = step UP
                    if rise <= 0:
                        continue
                    out.append(dict(room=rm["r"], rise=rise, cls=classify(rise),
                                    x=rm["ix"] + cx * CELL + CELL // 2,
                                    z=rm["iz"] + cz * CELL + CELL // 2,
                                    y=hf, yaw=yaw, face=label))
    return out


def main():
    prefix, want, top, tsv = "mrt", None, 6, False
    a = sys.argv[1:]
    while a:
        k = a.pop(0)
        if k == "--prefix": prefix = a.pop(0)
        elif k == "--class": want = a.pop(0).upper()
        elif k == "--top": top = int(a.pop(0))
        elif k == "--tsv": tsv = True   # machine-readable: cls room rise x y z yaw
    led = census(load(prefix))
    if tsv:
        # one spot per class, spread across rooms - what climb_matrix.sh drives
        for name, _, _ in CLASSES:
            sel = [l for l in led if l["cls"] == name]
            for l in sorted(sel, key=lambda v: (v["room"], -v["rise"]))[:top]:
                print("%s\t%d\t%d\t%d\t%d\t%d\t%d"
                      % (name, l["room"], l["rise"], l["x"], l["y"], l["z"], l["yaw"]))
        return
    counts = {}
    for l in led:
        counts[l["cls"]] = counts.get(l["cls"], 0) + 1
    print("ledges by TR1 class (within-room steps up):")
    for name, _, _ in CLASSES:
        print("  %-9s %5d" % (name, counts.get(name, 0)))
    print()
    for name, _, _ in CLASSES:
        if want and name != want:
            continue
        sel = [l for l in led if l["cls"] == name]
        if not sel:
            continue
        # spread the samples across rooms so one geometry quirk cannot stand
        # in for a whole class
        seen, pick = set(), []
        for l in sorted(sel, key=lambda v: (v["room"], -v["rise"])):
            if l["room"] in seen and len(pick) >= 2:
                continue
            seen.add(l["room"]); pick.append(l)
            if len(pick) >= top:
                break
        print("== %s (%d found) - test spots:" % (name, len(sel)))
        for l in pick:
            print("   room %2d  rise %4d  stand (%6d,%6d) y=%5d  face %s  "
                  "SPAWNAT_ROOM=%d SPAWNAT_X=%d SPAWNAT_Y=%d SPAWNAT_Z=%d SPAWNAT_YAW=%d"
                  % (l["room"], l["rise"], l["x"], l["z"], l["y"], l["face"],
                     l["room"], l["x"], l["y"], l["z"], l["yaw"]))
        print()


if __name__ == "__main__":
    main()
