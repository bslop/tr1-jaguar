#!/usr/bin/env python3
"""room_black.py - per-room black% baseline, measured by standing in each room.

WHY: the conformance harness flags spots above "17.4% black", but that number is
a CAVES figure from a ROOMTOUR sweep. Judging LARA'S HOME against it is
apples-to-oranges - the mansion is lit differently and has far more open
doorways. Four false defects in this project came from trusting a number past
its range, so each level gets its own baseline.

Teleports Lara to the centre of every room and records black%. Unlike ROOMTOUR
(which needs a generated roomtour_tab.h, built for `mrt` only) this works on any
prefix straight from the sector data.

    tools/room_black.py <rom.cof> <elf> [--prefix gym]
"""
import json, os, struct, subprocess, sys, time

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JE = "/home/jvilla/Documents/Git/jag_openlara/cobweb/sim/target/release/jagemu"
INST = "rblack"
WALL, OPEN = 0x7FFF, 0x7FFE


def ctl(*a, timeout=180):
    return subprocess.run([JE, "ctl", INST] + [str(x) for x in a],
                          capture_output=True, text=True, timeout=timeout).stdout.strip()


def peek(addr):
    for ln in ctl("peek", hex(addr), "--len", "4").split("\n")[::-1]:
        try:
            b = json.loads(ln).get("bytes")
        except Exception:
            continue
        if b:
            v = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]
            return v - (1 << 32) if v >= 1 << 31 else v
    return None


def poke(addr, val, width=4):
    v = val & (0xFFFFFFFF if width == 4 else 0xFFFF)
    by = ("%d,%d,%d,%d" % ((v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255)
          if width == 4 else "%d,%d" % ((v >> 8) & 255, v & 255))
    ctl("poke", hex(addr), by)


def centres(prefix):
    """Middle-ish walkable cell of each room, in world coords."""
    idx = open(os.path.join(D, prefix + ".bin"), "rb").read()
    sect = open(os.path.join(D, prefix + "_sect.bin"), "rb").read()
    out = []
    for r in range(struct.unpack_from(">H", idx, 0)[0]):
        _, soff = struct.unpack_from(">II", idx, 8 + r * 8)
        soff &= 0x7FFFFFFF                     # bit31 = WATER room flag
        xS, zS = struct.unpack_from(">HH", sect, soff)
        ix, iz = struct.unpack_from(">ii", sect, soff + 4)
        best = None
        for cx in range(xS):
            for cz in range(zS):
                f = struct.unpack_from(">H", sect, soff + 12 + (cx * zS + cz) * 6)[0]
                if f in (WALL, OPEN):
                    continue
                d = abs(cx - xS / 2) + abs(cz - zS / 2)   # closest to the middle
                if best is None or d < best[0]:
                    best = (d, ix + cx * 1024 + 512, iz + cz * 1024 + 512,
                            struct.unpack(">h", struct.pack(">H", f))[0])
        if best:
            out.append(dict(room=r, x=best[1], z=best[2], y=best[3]))
    return out


def main():
    rom, elf = sys.argv[1], sys.argv[2]
    pfx = sys.argv[sys.argv.index("--prefix") + 1] if "--prefix" in sys.argv else "mrt"
    nm = subprocess.run(["m68k-neogeo-elf-nm", elf], capture_output=True, text=True).stdout
    sy = {p[2]: int(p[0], 16) for p in (l.split() for l in nm.split("\n")) if len(p) == 3}
    rooms = centres(pfx)
    subprocess.run([JE, "instances", "--prune"], capture_output=True)
    srv = subprocess.Popen([JE, "serve", "--rom", rom, "--instance", INST],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(10)
    ctl("run", 1300, timeout=600)
    from PIL import Image
    print("%-6s %-8s %-8s %s" % ("ROOM", "FLOOR", "BLACK%", "note"))
    vals = []
    for rm in rooms:
        ctl("release"); ctl("run", 20)
        for k in ("g_lavy", "g_fally", "g_jumped", "g_lajf"):
            if k in sy:
                poke(sy[k], 0)
        poke(sy["g_curroom"], rm["room"])
        poke(sy["g_lax"], rm["x"]); poke(sy["g_lay"], rm["y"]); poke(sy["g_laz"], rm["z"])
        if "g_layprev" in sy:
            poke(sy["g_layprev"], rm["y"])
        ctl("run", 25)
        fl = peek(sy["g_lafloor"]); y = peek(sy["g_lay"])
        p = "/tmp/rb_%02d.png" % rm["room"]
        ctl("frame", p)
        px = list(Image.open(p).convert("L").getdata())
        blk = 100.0 * sum(1 for q in px if q < 8) / len(px)
        ok = (y == fl)
        if ok:
            vals.append(blk)
        print("%-6d %-8s %-8.1f %s" % (rm["room"], fl, blk, "" if ok else "NOT SEATED - ignored"))
    if vals:
        vals.sort()
        print("\n%s baseline over %d seated rooms: median %.1f%%, max %.1f%%"
              % (pfx, len(vals), vals[len(vals) // 2], vals[-1]))
        # Persist it so conformance.py stops judging every level by the CAVES
        # figure. ☠️ The mansion's max is 36.3% (rooms 10/11 at their CENTRES)
        # against the Caves' 17.4% - a room-12 spot reading 30% is normal here
        # and would have been reported as a void by the old fixed threshold.
        with open(os.path.join(D, "tools", ".black_" + pfx), "w") as f:
            f.write("%.1f\n" % vals[-1])
        print("recorded tools/.black_%s = %.1f" % (pfx, vals[-1]))
    srv.terminate()


if __name__ == "__main__":
    main()
