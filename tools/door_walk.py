#!/usr/bin/env python3
"""door_walk.py - WALK through every doorway and prove the room actually changes.

    tools/door_walk.py <rom.cof> <elf> [--prefix gym] [--limit N]

WHY: `portal_open.py --audit` says both sides of every wall-portal have a
passable cell - that is DATA. It does not prove Lara can cross. Run 72 showed the
difference: the audit-equivalent geometry looked fine while she stood at the
Caves 0/1 seam for eleven samples unable to move.

So: for each wall-portal, seat her a stand-off back from the plane on the source
side, face the plane, walk, and assert `g_curroom` becomes the destination.

☠️ BOOTS ONCE for the whole level - 32 doors at a 4-minute boot each is why this
was never done.
☠️ Skips HORIZONTAL-plane portals (constant y). Those are VERTICAL portals -
you fall or swim through them, and walking at them proves nothing. gym room 18
is the pool and is reached that way.
"""
import importlib.util, json, os, struct, subprocess, sys, time

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JE = "/home/jvilla/Documents/Git/jag_openlara/cobweb/sim/target/release/jagemu"
INST = "door"
BOOT, SETTLE, CELL = 1300, 20, 1024
STANDOFF = 1300          # how far back from the plane to start
WALL, OPEN = 0x7FFF, 0x7FFE

_spec = importlib.util.spec_from_file_location("po", os.path.join(D, "tools", "portal_open.py"))
po = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(po)


def main():
    rom, elf = sys.argv[1], sys.argv[2]
    pfx = sys.argv[sys.argv.index("--prefix") + 1] if "--prefix" in sys.argv else "gym"
    limit = int(sys.argv[sys.argv.index("--limit") + 1]) if "--limit" in sys.argv else 0

    idx = open(os.path.join(D, pfx + ".bin"), "rb").read()
    sect = open(os.path.join(D, pfx + "_sect.bin"), "rb").read()
    nroom = struct.unpack_from(">H", idx, 0)[0]
    meta = []
    for r in range(nroom):
        _, soff = struct.unpack_from(">II", idx, 8 + r * 8)
        soff &= 0x7FFFFFFF
        xS, zS = struct.unpack_from(">HH", sect, soff)
        ix, iz = struct.unpack_from(">ii", sect, soff + 4)
        meta.append((soff, xS, zS, ix, iz))

    def cellval(r, wx, wz):
        soff, xS, zS, ix, iz = meta[r]
        lx, lz = wx - ix, wz - iz
        if not (0 <= lx < xS * CELL and 0 <= lz < zS * CELL):
            return None
        return struct.unpack_from(">H", sect, soff + 12 + ((lx // CELL) * zS + lz // CELL) * 6)[0]

    syms = {}
    for ln in subprocess.run(["m68k-neogeo-elf-nm", elf], capture_output=True,
                             text=True).stdout.split("\n"):
        p = ln.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)

    def ctl(*a, timeout=300):
        return subprocess.run([JE, "ctl", INST] + [str(x) for x in a],
                              capture_output=True, text=True, timeout=timeout).stdout.strip()

    def peek(name):
        if name not in syms:
            return None
        for ln in ctl("peek", hex(syms[name]), "--len", "4").split("\n")[::-1]:
            try:
                b = json.loads(ln).get("bytes")
            except Exception:
                continue
            if b:
                v = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]
                return v - (1 << 32) if v >= 1 << 31 else v
        return None

    def poke(name, v):
        v &= 0xFFFFFFFF
        ctl("poke", hex(syms[name]), "%d,%d,%d,%d" % ((v >> 24) & 255, (v >> 16) & 255,
                                                     (v >> 8) & 255, v & 255))

    # build the test list first, so a geometry problem is reported without booting
    tests = []
    for (r, dst, xs, ys, zs) in po.portals(pfx):
        x0, x1, y0, y1, z0, z1 = min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)
        if y0 == y1 and x0 != x1 and z0 != z1:
            continue                                   # vertical portal
        cand = None
        # ☠️ THE SPAN CENTRE IS NOT ALWAYS STANDABLE, AND THE SOURCE ROOM MUST OWN
        # THE CELL. Two failure modes from run 74, both harness:
        #   * 2->5 and 2->6 "failed" with her moving only 164/188 units - wedged
        #     on the stand-off cell itself, not refused at the door (a refused
        #     door shows ~1260, the full stand-off);
        #   * four doors seated into room 12 because rooms OVERLAP, so the point
        #     resolved to a different room than the one under test.
        # So sweep along the span and out from the plane, and PREFER a cell no
        # other room supplies a floor for - exclusive ownership is what makes the
        # runtime agree with the room we asked for.
        def owners(wx, wz):
            n = 0
            for q in range(nroom):
                v = cellval(q, wx, wz)
                if v is not None and v < OPEN:
                    n += 1
            return n

        cands = []
        for sgn in (-1, 1):
          for frac in (0.5, 0.3, 0.7, 0.15, 0.85):
            for dist in (1300, 768, 1800):
              mx = int(x0 + (x1 - x0) * frac)
              mz = int(z0 + (z1 - z0) * frac)
            # ☠️ FACE THE DOOR, NOT AWAY FROM IT. Forward is (SIN,COS): yaw 0
            # faces +Z, 16384 +X, -16384 -X, -32768 -Z. The first version had the
            # Z case INVERTED - standing at smaller z it faced -Z - and 9 of 10
            # doors "failed" while she walked away from each one. The X case was
            # right, which is why 2->1 passed and hid it.
              if z0 == z1:
                sx, sz, yaw = mx, z0 + sgn * dist, (0 if sgn < 0 else -32768)
              elif x0 == x1:
                sx, sz, yaw = x0 + sgn * dist, mz, (16384 if sgn < 0 else -16384)
              else:
                continue
              v = cellval(r, sx, sz)
              if v is None or v >= OPEN:               # need a REAL floor to stand on
                continue
              fy = struct.unpack(">h", struct.pack(">H", v))[0]
            # ☠️☠️ A PORTAL HAS A **HEIGHT**, AND A DOORWAY ON ANOTHER LEVEL IS
            # NOT A FAILED DOORWAY. Ignoring the y extent made 7 of 12 gym doors
            # "fail": e.g. 3->1's plane is z=53247 and the floor past it is -1280
            # while she stands on 1280 - a 2560-unit STEP UP, ten clicks, which
            # the move gate refuses exactly as TR does. Her feet must be at the
            # doorway's own floor (y1 = max y = the LOWEST point, +Y is down),
            # and the floor beyond must be within one step.
              if abs(fy - y1) > 512:
                continue
              # the cell just past the plane, to reject a doorway that is a step up
              bx = sx if z0 == z1 else (x0 - sgn * 512)
              bz = (z0 - sgn * 512) if z0 == z1 else sz
              # ☠️ READ THE **DESTINATION** ROOM'S FLOOR PAST THE PLANE, NOT THE
              # SOURCE'S. Past the seam the source room reads OPEN (0x7FFE) - that
              # is what portal_open just made it - so a source-side lookup is
              # never a floor and the step-up test silently never ran. Measured:
              # Caves 17->14 has room 17 on 7168 and room 14 on 6656, a 512 step
              # UP (two clicks) that the move gate refuses because it needs a
              # VAULT, not a walk - and it was being scored as a dead door.
              beyond = cellval(dst, bx, bz)
              if beyond is None or beyond >= OPEN:
                continue                               # nothing to arrive on
              by = struct.unpack(">h", struct.pack(">H", beyond))[0]
              if fy - by > 256:                        # LARA_STEPUP: a vault, not a walk
                continue
              # ☠️ RANK, DO NOT FIRST-MATCH. Taking the first exclusively-owned
              # cell sent her to stand-offs far along the span and 1800 out; she
              # then walked 10,000+ units and ended up in a THIRD room, and the
              # gym score went 10/12 -> 7/14. The centre of the doorway at the
              # standard stand-off is the right place to start from; exclusivity
              # and distance are tie-breakers, not the objective.
              cands.append((owners(sx, sz) > 1, abs(frac - 0.5),
                            abs(dist - 1300), sx, sz, yaw, fy))
        if cands:
            cands.sort()
            cand = cands[0][3:]
        if cand:
            tests.append((r, dst) + cand)
    if limit:
        tests = tests[:limit]
    total_wall = sum(1 for (r, d, xs, ys, zs) in po.portals(pfx)
                     if not (min(ys) == max(ys) and min(xs) != max(xs) and min(zs) != max(zs)))
    print("%s: %d of %d wall-portals are WALKABLE AT FLOOR LEVEL and get tested"
          % (pfx, len(tests), total_wall), flush=True)
    print("  (the rest are doorways at another height - a step up, a ledge, a"
          " balcony - and walking at them proves nothing)", flush=True)

    subprocess.run([JE, "instances", "--prune"], capture_output=True)
    srv = subprocess.Popen([JE, "serve", "--rom", rom, "--instance", INST],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(10)
    ok = fail = 0
    try:
        ctl("run", BOOT, timeout=900)
        for (r, dst, sx, sz, yaw, fy) in tests:
            ctl("release"); ctl("run", 20)
            for k in ("g_lavy", "g_fally", "g_jumped", "g_lajf", "g_hang", "g_autoj"):
                if k in syms:
                    poke(k, 0)
            poke("g_curroom", r)
            poke("g_lax", sx); poke("g_lay", fy); poke("g_laz", sz)
            if "g_layprev" in syms:
                poke("g_layprev", fy)
            yv = yaw & 0xFFFF                          # ☠️ yaw is 16-bit
            ctl("poke", hex(syms["g_layaw"]), "%d,%d" % ((yv >> 8) & 255, yv & 255))
            ctl("run", SETTLE)
            seated = peek("g_curroom")
            # ☠️ A BAD SEAT IS NOT A DOOR FAILURE. Rooms overlap, so a stand-off
            # point can resolve to a different room than the one being tested
            # (one test seated in room 12 while probing room 7). Report it as
            # UNTESTABLE rather than blaming the doorway.
            if seated != r:
                print("  --   %2d -> %-2d  UNTESTABLE: seat resolved to room %s"
                      % (r, dst, seated), flush=True)
                continue
            # ☠️ LOG WHETHER SHE MOVED. "ended in the same room" has two very
            # different causes - she walked to the door and was refused, or she
            # never moved at all (facing a wall, wedged on the stand-off cell) -
            # and without distance the tool cannot tell them apart. Run 73's tour
            # crossed gym 1->0 for real, so a tool that calls that door dead is
            # measuring itself.
            # ☠️ DO NOT WALK 12,000 UNITS AT A DOOR 1,300 AWAY, AND ASSERT ON
            # "REACHED", NOT "ENDED IN". Three doors failed while she walked
            # 9-11k units and finished in a THIRD room - she had gone through the
            # doorway and out the far side, so an end-state test scored a working
            # door as dead. Collect every room seen and pass if dst appears.
            got = None
            seen = set()
            x0p, z0p = peek("g_lax"), peek("g_laz")
            ctl("input", "up")
            for _ in range(5):
                ctl("run", 60, timeout=600)
                cur = peek("g_curroom")
                if cur is not None:
                    seen.add(cur)
                if cur == dst:
                    got = dst
                    break
            ctl("release")
            moved = abs((peek("g_lax") or 0) - (x0p or 0)) + abs((peek("g_laz") or 0) - (z0p or 0))
            if got == dst:
                ok += 1
                print("  ok   %2d -> %-2d" % (r, dst), flush=True)
            else:
                fail += 1
                print("  ☠️ FAIL %2d -> %-2d  saw %s, moved %d  (stand %d,%d,%d yaw %d)"
                      % (r, dst, sorted(seen), moved, sx, fy, sz, yaw), flush=True)
    finally:
        ctl("release")
        srv.terminate()
    print("DOORS: %d crossed, %d FAILED" % (ok, fail))


if __name__ == "__main__":
    main()
