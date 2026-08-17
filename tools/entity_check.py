#!/usr/bin/env python3
"""entity_check.py - stand Lara next to chosen ENTITIES and photograph them.

    tools/entity_check.py <rom.cof> <elf> --ents 29,26,1 [--prefix mrt] [--out DIR]
    tools/entity_check.py <rom.cof> <elf> --list

WHY: the conformance sweeps prove CLIMBING and nothing else. "Enemies and
pickups" is one of the four things on the user's DONE list and until run 63
nobody had confirmed either appears on screen. `<prefix>_spawn.h` carries the
whole entity table - 60 entries for the Caves, TR1 index order preserved, with
world x/y/z - so every enemy, pickup, door and switch has a known address to go
look at.

☠️ BOOTS ONCE for the whole list. One probe per entity meant a fresh ~4-minute
boot each time, which is why the audit kept being deferred.

☠️ THE ENTITY'S OWN Y IS NOT A PLACE TO STAND. A bat sits 2432 units up in the
air; put Lara there and she falls out of the test. The standing floor is read
from <prefix>_sect.bin at the cell she is placed in, exactly as room_floor_mr
would, and she is offset along the approach axis so the entity is IN FRONT of
her rather than inside her.
"""
import json, os, re, struct, subprocess, sys, time

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JE = "/home/jvilla/Documents/Git/jag_openlara/cobweb/sim/target/release/jagemu"
INST = "ent"
BOOT = 1300
SETTLE = 20
CELL = 1024
STANDOFF = 1536          # how far in front of the entity to stand
# ☠️ A PICKUP IS A SPRITE A FEW PIXELS WIDE. At 1536 units on a 320x80 band a
# medikit is not legible and "I cannot see it" is not the same as "it is not
# drawn". Use --standoff 512 for pickups; keep the default for enemies, which
# need room to be in frame at all.


def entities(prefix):
    src = open(os.path.join(D, prefix + "_spawn.h")).read()
    out = {}
    for m in re.finditer(r'\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),'
                         r'\s*(-?\d+),\s*0x[0-9A-Fa-f]+\s*\},\s*//\s*(\d+)\s+(\S+)', src):
        t, room, yaw, x, y, z, idx, name = m.groups()
        out[int(idx)] = dict(type=int(t), room=int(room), x=int(x), y=int(y),
                             z=int(z), name=name)
    return out


def floor_at(prefix, wx, wz):
    """Lowest floor any room supplies at this x/z - what she will stand on."""
    idx = open(os.path.join(D, prefix + ".bin"), "rb").read()
    sect = open(os.path.join(D, prefix + "_sect.bin"), "rb").read()
    n = struct.unpack_from(">H", idx, 0)[0]
    best = None
    for r in range(n):
        _, soff = struct.unpack_from(">II", idx, 8 + r * 8)
        soff &= 0x7FFFFFFF
        xS, zS = struct.unpack_from(">HH", sect, soff)
        ix, iz = struct.unpack_from(">ii", sect, soff + 4)
        lx, lz = wx - ix, wz - iz
        if lx < 0 or lx >= xS * CELL or lz < 0 or lz >= zS * CELL:
            continue
        f = struct.unpack_from(">h", sect, soff + 12 + ((lx // CELL) * zS + lz // CELL) * 6)[0]
        if f >= 0x7FFE or f < -32000:
            continue
        if best is None or f > best:      # +Y is DOWN: largest = lowest floor
            best = (f, r)
    return best


def main():
    pfx = sys.argv[sys.argv.index("--prefix") + 1] if "--prefix" in sys.argv else "mrt"
    ents = entities(pfx)
    if "--list" in sys.argv:
        for i in sorted(ents):
            e = ents[i]
            print("  %3d %-22s room %-3d (%d,%d,%d)" % (i, e["name"], e["room"], e["x"], e["y"], e["z"]))
        return
    rom, elf = sys.argv[1], sys.argv[2]
    want = [int(v) for v in sys.argv[sys.argv.index("--ents") + 1].split(",")]
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else "/tmp/entcheck"
    global STANDOFF
    if "--standoff" in sys.argv:
        STANDOFF = int(sys.argv[sys.argv.index("--standoff") + 1])
    os.makedirs(out, exist_ok=True)

    syms = {}
    for ln in subprocess.run(["m68k-neogeo-elf-nm", elf], capture_output=True,
                             text=True).stdout.split("\n"):
        p = ln.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)

    def ctl(*a, timeout=300):
        return subprocess.run([JE, "ctl", INST] + [str(x) for x in a],
                              capture_output=True, text=True, timeout=timeout).stdout.strip()

    def poke(addr, v):
        v &= 0xFFFFFFFF
        ctl("poke", hex(addr), "%d,%d,%d,%d" % ((v >> 24) & 255, (v >> 16) & 255,
                                                (v >> 8) & 255, v & 255))

    subprocess.run([JE, "instances", "--prune"], capture_output=True)
    srv = subprocess.Popen([JE, "serve", "--rom", rom, "--instance", INST],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(10)
    try:
        ctl("run", BOOT, timeout=900)
        for i in want:
            e = ents.get(i)
            if not e:
                print("  entity %d not in the table" % i); continue
            # ☠️ DO NOT ASSUME ONE APPROACH DIRECTION. Standing a fixed 1536 in
            # +X put Lara inside a wall for some entities and over a DROP for
            # others (a bat capture caught her mid-fall, arms out). Search the
            # four axes at a few distances and take the stand-off whose floor is
            # CLOSEST TO THE ENTITY'S OWN Y - that is what puts them on the same
            # level and in frame. Yaw follows the direction chosen: forward is
            # (SIN,COS), so 0 faces +Z, 16384 +X, -16384 -X, -32768 -Z.
            cand = None
            for dx, dz, yaw in ((1, 0, -16384), (-1, 0, 16384),
                                (0, 1, -32768), (0, -1, 0)):
                for dist in (STANDOFF, 1024, 768, 2048):
                    tx, tz = e["x"] + dx * dist, e["z"] + dz * dist
                    fl = floor_at(pfx, tx, tz)
                    if fl is None:
                        continue
                    dy = abs(fl[0] - e["y"])
                    if cand is None or dy < cand[0]:
                        cand = (dy, tx, tz, fl[0], fl[1], yaw)
            if cand is None:
                print("  %3d %-20s no floor anywhere around it - skipped" % (i, e["name"]))
                continue
            dy, sx, sz, fy, froom, yaw = cand
            ctl("release"); ctl("run", 20)
            for k in ("g_lavy", "g_fally", "g_jumped", "g_lajf", "g_hang", "g_autoj"):
                if k in syms:
                    poke(syms[k], 0)
            poke(syms["g_curroom"], froom)
            poke(syms["g_lax"], sx); poke(syms["g_lay"], fy); poke(syms["g_laz"], sz)
            if "g_layprev" in syms:
                poke(syms["g_layprev"], fy)
            if "g_layaw" in syms:                    # 16-bit! a 32-bit poke stomps g_curroom
                yv = yaw & 0xFFFF
                ctl("poke", hex(syms["g_layaw"]), "%d,%d" % ((yv >> 8) & 255, yv & 255))
            ctl("run", SETTLE)
            p = os.path.join(out, "e%02d_%s.png" % (i, e["name"]))
            ctl("frame", p)
            print("  %3d %-20s stood (%d,%d,%d) room %d dy=%d -> %s"
                  % (i, e["name"], sx, fy, sz, froom, dy, os.path.basename(p)), flush=True)
    finally:
        ctl("release")
        srv.terminate()


if __name__ == "__main__":
    main()
