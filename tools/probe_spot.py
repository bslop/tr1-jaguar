#!/usr/bin/env python3
"""probe_spot.py - hand-drive ONE spot and print the state machine every step.

WHY: the standing lesson of this campaign is that a uniform failure has a
uniform cause in the HARNESS, and the way it gets caught is driving a single
case with telemetry instead of reading a verdict column. conformance.py answers
"did she climb"; this answers "what did the game actually do", which is the only
thing that separates a collision bug from a Lara-state bug from a harness bug.

    tools/probe_spot.py <rom.cof> <elf> --at X,Y,Z,ROOM,YAW [--frames N]
                        [--keys up] [--shots DIR]

Prints, per step: feet Y, floor, the room, and every gate on the climb path -
  g_fwdblk   is she blocked ahead? (the auto jump-reach will not arm without it)
  g_autoj    armed
  g_jumped   launched
  g_lavy     vertical velocity - shows the launch speed jump_reach_vel picked
  g_hang     the grab latched
  g_vault    a vault is running
so a failure names the FIRST gate that did not open.
"""
import json, os, subprocess, sys, time

JE = "/home/jvilla/Documents/Git/jag_openlara/cobweb/sim/target/release/jagemu"
INST = "probe"
BOOT = 1300
SETTLE = 20

WATCH = ["g_drawframes", "frame_count", "g_hopcap", "g_gov_on", "g_visrooms", "g_drewrooms", "g_gunst", "g_lay", "g_lafloor", "g_curroom", "g_floorroom", "g_fwdblk", "g_autoj", "g_autojv",
         "g_jumped", "g_lavy", "g_hang", "g_vault", "g_lax", "g_laz"]


def ctl(*a, timeout=180):
    return subprocess.run([JE, "ctl", INST] + [str(x) for x in a],
                          capture_output=True, text=True, timeout=timeout).stdout.strip()


def nm(elf):
    out = subprocess.run(["m68k-neogeo-elf-nm", elf], capture_output=True, text=True).stdout
    return {p[2]: int(p[0], 16) for p in (l.split() for l in out.split("\n")) if len(p) == 3}


def peek(addr, signed=True):
    for ln in ctl("peek", hex(addr), "--len", "4").split("\n")[::-1]:
        try:
            b = json.loads(ln).get("bytes")
        except Exception:
            continue
        if b:
            v = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]
            return v - (1 << 32) if (signed and v >= 1 << 31) else v
    return None


def poke(addr, val):
    v = val & 0xFFFFFFFF
    return ctl("poke", hex(addr), "%d,%d,%d,%d" % ((v >> 24) & 255, (v >> 16) & 255,
                                                   (v >> 8) & 255, v & 255))


def poke16(addr, val):   # ☠️ yaw is 16-bit; a 32-bit write stomps g_curroom
    v = val & 0xFFFF
    return ctl("poke", hex(addr), "%d,%d" % ((v >> 8) & 255, v & 255))


def main():
    rom, elf = sys.argv[1], sys.argv[2]
    at = sys.argv[sys.argv.index("--at") + 1].split(",")
    x, y, z, room, yaw = (int(v) for v in at)
    frames = int(sys.argv[sys.argv.index("--frames") + 1]) if "--frames" in sys.argv else 14
    keys = sys.argv[sys.argv.index("--keys") + 1] if "--keys" in sys.argv else "up"
    shots = sys.argv[sys.argv.index("--shots") + 1] if "--shots" in sys.argv else None
    if shots:
        os.makedirs(shots, exist_ok=True)

    sy = nm(elf)
    # ★ RAW DRAM COUNTERS. gpu_geotex.gas keeps its per-face tallies at fixed
    # addresses, not symbols, so --raw NAME=0xADDR reads them alongside the
    # variables. The kernel's own attribution algebra (gpu_geotex.gas:1369):
    #     total faces = worldcull + staged
    #     near-plane  = bexit
    #     screen-space= staged - bexit - rastered
    #     drawn       = rastered
    for a in sys.argv:
        if a.startswith("--raw="):
            k, v = a[6:].split("=")
            sy[k] = int(v, 0)
            WATCH.append(k)
    have = [w for w in WATCH if w in sy]
    missing = [w for w in WATCH if w not in sy]
    if missing:
        print("(not in this build, so not shown: %s)" % ",".join(missing))

    subprocess.run([JE, "instances", "--prune"], capture_output=True)
    srv = subprocess.Popen([JE, "serve", "--rom", rom, "--instance", INST],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(10)
    try:
        ctl("run", BOOT, timeout=600)
        # seat exactly as conformance.py does - quiesce, zero motion, place, settle
        ctl("release"); ctl("run", 20)
        # ☠️ ZERO THE DRAW MASKS AFTER SEATING. They accumulate (see main.c),
        # so a bit set during boot or at the previous spot would be read as
        # "drawn HERE". Clearing at the seat makes them attributable.
        for k in ("g_lavy", "g_fally", "g_jumped", "g_lajf", "g_hang", "g_autoj",
                  "g_visrooms", "g_drewrooms"):
            if k in sy:
                poke(sy[k], 0)
        poke(sy["g_curroom"], room)
        poke(sy["g_lax"], x); poke(sy["g_lay"], y); poke(sy["g_laz"], z)
        if "g_layprev" in sy:
            poke(sy["g_layprev"], y)
        if "g_layaw" in sy:
            poke16(sy["g_layaw"], yaw)
        ctl("run", SETTLE)
        # --set SYM=VAL: force a variable AFTER seating. Added to test whether a
        # value the game computes for itself is causing what you are looking at
        # (the draw-distance governor clamping g_hopcap to 1, run 51).
        for a in sys.argv:
            if a.startswith("--set="):
                k, v = a[6:].split("=")
                if k in sy:
                    poke(sy[k], int(v))
                    print("set %s = %s" % (k, v))
                else:
                    print("☠️ --set %s: not in this build" % k)

        # ☠️ --phases: a SEQUENCE of key holds, because one fixed key set for the
        # whole run cannot work a switch. Holding `up,b` to reach a switch and
        # throw it does NOT work - measured run 79: she never moved at all, x
        # pinned, because ACTION held during a walk is a different state from
        # ACTION pressed while standing at the switch.
        #   --phases "up:6,-:1,b:3,-:1,up:8"   ("-" = release everything)
        if "--phases" in sys.argv:
            print("%-4s %s" % ("step", "  ".join("%-9s" % w for w in have)))
            n = 0
            for ph in sys.argv[sys.argv.index("--phases") + 1].split(","):
                # ☠️ a set: phase contains its own colons - take the count from
                # the RIGHT, not the first colon ("set:g_layaw=0xC000:1").
                if ph.startswith("set:"):
                    keys, _, cnt = ph.rpartition(":")
                else:
                    keys, _, cnt = ph.partition(":")
                cnt = int(cnt or 1)
                if keys.startswith("set:"):
                    # ☠️ A PHASE THAT POKES. Needed to separate two effects that
                    # arrive together: using a switch leaves Lara SQUARED TO THE
                    # LEVER (yaw reset to 0), which is plausibly correct TR1
                    # behaviour, and a drive that then presses UP walks her into
                    # the wall rather than through the door it just opened. To ask
                    # "does the passage work" you must restore the facing without
                    # re-testing the switch.
                    k, _, v = keys[4:].partition("=")
                    if k in sy:
                        # ☠️ NOT `n` - that is the step counter, and reusing it
                        # here renumbered every later step label to the poked
                        # VALUE ("up49153"). A cosmetic bug that makes a log
                        # unreadable is still a bug in an instrument.
                        val = int(v, 0)
                        if k == "g_layaw":              # 16-bit! see the seat note
                            poke16(sy[k], val)
                        else:
                            poke(sy[k], val)
                        print("   set %s = %s" % (k, v), flush=True)
                    else:
                        print("   ☠️ set %s: not in this build" % k, flush=True)
                elif keys in ("-", "none", ""):
                    ctl("release")
                else:
                    ctl("input", keys)
                for _ in range(cnt):
                    ctl("run", 12)
                    vals = {w: peek(sy[w]) for w in have}
                    print("%-4s %s" % ("%s%d" % (keys, n),
                                       "  ".join("%-9s" % vals[w] for w in have)), flush=True)
                    if shots:
                        ctl("frame", os.path.join(shots, "s%02d.png" % n))
                    n += 1
            ctl("release")
            return

        print("seated: " + "  ".join("%s=%s" % (w, peek(sy[w])) for w in have[:3]))
        print("%-4s %s" % ("step", "  ".join("%-9s" % w for w in have)))
        ctl("input", keys)
        for i in range(frames):
            ctl("run", 12)
            vals = {w: peek(sy[w]) for w in have}
            print("%-4d %s" % (i, "  ".join("%-9s" % vals[w] for w in have)))
            if shots:
                ctl("frame", os.path.join(shots, "s%02d.png" % i))
    finally:
        ctl("release")
        srv.terminate()


if __name__ == "__main__":
    main()
