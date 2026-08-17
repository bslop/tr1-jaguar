#!/usr/bin/env python3
"""conformance.py - drive EVERY test spot in the level and report what works.

USER REQUEST (2026-08-16): "test the entire level and home. Try out every
ledge/jump/obstacle/room/etc. Notate what works and doesn't work in comparison
to the PSX version."

★★★ THE UNLOCK IS RUNTIME TELEPORT. `SPAWNAT_*` is compile-time, so testing a
hundred spots that way is a hundred 15-minute builds. `jagemu ctl <inst> poke`
writes Lara's position live, so ONE build covers the whole level:

    poke g_lax/g_lay/g_laz/g_curroom  ->  settle  ->  drive  ->  read back

Verified before this tool was written: poking g_laz moved her 56832 -> 58624.

WHAT EACH SPOT IS TESTED FOR
  climb   walk into the ledge, then hold UP+B, and see whether Y rises by the
          census RISE and g_lafloor follows. TR1 classes: <=256 WALKUP,
          <=640 CLIMB2, <=896 CLIMB3, <=1920 JUMPGRAB.
  void    black% of the frame while she is STUCK. Both collision bugs found so
          far announced themselves this way (53% and 81%) - a room centre reads
          0.1-4%, and the whole-level ROOMTOUR baseline never exceeds 17.4%.

☠️ HOLD THE BUTTONS THROUGH THE WHOLE PULL-UP. Releasing mid-climb aborts it,
exactly as in the original game, and reads as a failed climb when it is not.
☠️ Walk in SHORT steps and sample between. 120 fields of UP carried Lara 2,525
units - 2.5 sectors - past the target, so she climbed a different ledge.
☠️ Symbol addresses are PER-BUILD. They are read from the ROM's own .elf.
☠️ SPAWNAT arms render Lara deformed; teleport does not, but judge the WORLD
from these frames regardless.

    tools/conformance.py <rom.cof> <build/openlara.elf> [--limit N] [--out DIR]
"""
import json, os, struct, subprocess, sys, time

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JE = "/home/jvilla/Documents/Git/jag_openlara/cobweb/sim/target/release/jagemu"
INST = "conf"
BOOT = 1300          # fields to reach the level (AUTOSTART exits the ring ~20)
SETTLE = 20          # fields after a teleport, so physics re-seats her
WALK_STEP = 12       # fields per walk sample - short on purpose (see above)


def nm(elf):
    out = subprocess.run(["m68k-neogeo-elf-nm", elf], capture_output=True, text=True).stdout
    syms = {}
    for ln in out.split("\n"):
        p = ln.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    need = ["g_lax", "g_lay", "g_laz", "g_lafloor", "g_curroom", "g_layaw",
            "g_layprev", "g_lavy"]
    miss = [n for n in need if n not in syms]
    if miss:
        print("missing symbols: %s" % miss, file=sys.stderr)
    return syms


def ctl(*args, timeout=120):
    r = subprocess.run([JE, "ctl", INST] + [str(a) for a in args],
                       capture_output=True, text=True, timeout=timeout)
    return r.stdout.strip()


def peek(addr, signed=True):
    out = ctl("peek", hex(addr), "--len", "4")
    for ln in out.split("\n")[::-1]:
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
    out = ctl("poke", hex(addr), "%d,%d,%d,%d" % ((v >> 24) & 255, (v >> 16) & 255,
                                                  (v >> 8) & 255, v & 255))
    if os.environ.get("CONF_DEBUG"):
        print("      poke %s = %d -> %s" % (hex(addr), val, out[-90:]), file=sys.stderr)
    return out


def poke16(addr, val):
    """16-bit write. See the note at the yaw poke: a 32-bit write here corrupts
    the neighbouring variable."""
    v = val & 0xFFFF
    out = ctl("poke", hex(addr), "%d,%d" % ((v >> 8) & 255, v & 255))
    if os.environ.get("CONF_DEBUG"):
        print("      poke16 %s = %d -> %s" % (hex(addr), val, out[-60:]), file=sys.stderr)
    return out


def black_pct(path):
    ctl("frame", path)
    try:
        from PIL import Image
    except ImportError:
        return -1.0
    im = Image.open(path).convert("L")
    px = list(im.getdata())
    return 100.0 * sum(1 for p in px if p < 8) / len(px)


def census(prefix="mrt"):
    """Test spots. `--prefix gym` sweeps LARA'S HOME instead of the Caves -
    304 ledges there (110 WALKUP / 90 CLIMB2 / 39 CLIMB3 / 55 JUMPGRAB / 10
    WALL) versus 26 filtered spots in the Caves.
    ☠️ Needs an AUTOGYM ROM: the mansion is a different level (g_useset=1) with
    its own room numbering, so a Caves ROM cannot be teleported into it."""
    out = subprocess.run(["python3", os.path.join(D, "tools", "ledge_census.py"),
                          "--prefix", prefix, "--tsv"],
                         capture_output=True, text=True).stdout
    spots = []
    for ln in out.strip().split("\n"):
        f = ln.split("\t")
        if len(f) >= 7:
            spots.append(dict(cls=f[0], room=int(f[1]), rise=int(f[2]),
                              x=int(f[3]), y=int(f[4]), z=int(f[5]), yaw=int(f[6])))
    return spots


def seat(sy, sp):
    """Put Lara at a spot and prove she is actually standing there.

    ☠️ POSITION ALONE IS NOT STATE. The first harness poked x/y/z/room and drove
    straight off. Between spots Lara was often still FALLING from the previous
    test, and a position poke does not clear her vertical velocity - so she
    resumed falling from the new place and left the world. Room 11 spots came
    back "97% black, floor -256" while the very same spot, seated cleanly,
    reads 0.6% black / floor 7680. The harness was manufacturing the bug it
    then reported.

    So: quiesce FIRST, zero the motion state, place her, settle, then VERIFY.
    """
    ctl("release")
    ctl("run", 20)                            # let the previous test finish
    for k in ("g_lavy", "g_fally", "g_jumped", "g_lajf"):
        if k in sy:
            poke(sy[k], 0)                    # not falling, not jumping
    poke(sy["g_curroom"], sp["room"])
    poke(sy["g_lax"], sp["x"]); poke(sy["g_lay"], sp["y"]); poke(sy["g_laz"], sp["z"])
    # ☠️ g_layprev IS PART OF THE POSITION. main.c: "feet Y before this frame's
    # gravity step, for the SWEPT grab test". Leave it holding the PREVIOUS
    # spot's Y and the sweep sees a multi-thousand-unit fall between frames -
    # which is why a teleport either dropped her out of the world (y -> -256)
    # or was undone during the settle. Position is not one variable.
    if "g_layprev" in sy:
        poke(sy["g_layprev"], sp["y"])
    # ☠️☠️ YAW IS 16-BIT. `g_layaw` sits at 0x19084e and `g_curroom` at
    # 0x190850, so a 4-byte poke of the yaw STOMPS THE ROOM two bytes later -
    # the room went to garbage, the floor lookup failed, and Lara either fell
    # out of the world or refused to move. Every poke returned ok:true, which
    # is exactly why this survived three runs of debugging: the writes were
    # all landing, just two bytes too wide.
    # ★ A poke helper must know the WIDTH of what it is writing. Defaulting to
    #   32 bits silently corrupts whatever is adjacent.
    if "g_layaw" in sy:
        poke16(sy["g_layaw"], sp["yaw"])
    ctl("run", SETTLE)
    y, fl = peek(sy["g_lay"]), peek(sy["g_lafloor"])
    if os.environ.get("CONF_DEBUG"):
        print("      seat room=%d want(x=%d y=%d z=%d) -> y=%s floor=%s"
              % (sp["room"], sp["x"], sp["y"], sp["z"], y, fl), file=sys.stderr)
    # ★ A harness that reports a confident WRONG verdict is worse than one that
    #   refuses. She is seated only if she is standing on the floor she landed
    #   on, near where we put her.
    ok = (y is not None and fl is not None and y == fl
          and abs(y - sp["y"]) <= 256)
    return ok, y, fl


# Level start, room 0 - a spot verified to seat cleanly (floor 3072, 1.1% black).
# ☠️ RECOVERY WAYPOINT. Once Lara is OUTSIDE the world, poking her to a good
# position does not bring her back: the floor lookup keeps returning -256 and
# every later spot inherits the wreck. Seating her at a known-good place FIRST
# re-establishes room/sector state, and the real target then takes. Without
# this, one bad spot early in the list poisons the entire rest of the run - the
# room-11 spots were "UNTESTABLE" purely because two room-3 spots ran first.
HOME = dict(room=0, x=75264, y=3072, z=3584, yaw=0)


def test_spot(sy, sp, outdir, idx):
    """Teleport, walk in, request the climb, report what happened."""
    seated, y, fl = seat(sy, sp)
    if not seated:
        seat(sy, HOME)                        # recover through a known-good spot
        seated, y, fl = seat(sy, sp)
    if not seated:
        return dict(rose=0, moved=0, floor=fl, y0=y, y1=y, black=-1,
                    verdict="UNTESTABLE")

    y0, z0 = peek(sy["g_lay"]), peek(sy["g_laz"])
    # ☠️ A 256 WALKUP IS AN AUTOMATIC STEP - she gains it while WALKING, with no
    # button at all (verified by hand: Y -1280 -> -1536 with floor following,
    # during the walk phase). Pressing UP+B afterwards then walks her straight
    # off the far side and she falls, so a FINAL-Y reading scores a successful
    # climb as negative. Track the BEST height reached across the whole drive,
    # and remember the floor at that moment: a climb counts if she ever STOOD
    # on the higher surface, not if she happened to still be there at the end.
    best = y0
    bestfl = peek(sy["g_lafloor"])

    def sample():
        nonlocal best, bestfl
        yn, fn = peek(sy["g_lay"]), peek(sy["g_lafloor"])
        if yn is not None and yn < best:      # Y grows DOWN: smaller = higher
            best, bestfl = yn, fn
        return yn

    ctl("input", "up")
    for _ in range(3):                       # short walk into the wall
        ctl("run", WALK_STEP)
        sample()
    ymid = peek(sy["g_lay"])

    # ☠️☠️ DO NOT PRESS A FOR A JUMPGRAB. This used to drive "up,a" on the
    # theory that a high ledge needs a running jump. It does not, and the A was
    # actively harmful: main.c arms the AUTO JUMP-REACH on (PAD_UP && g_fwdblk)
    # alone - walk into the wall and the game jumps for you, TR1-style. Pressing
    # A launched a MANUAL jump first, and because UP was held that selects the
    # DIRECTIONAL jump at JUMP_VEL_FWD 100, whose apex is 784. That is the "peak
    # jump = 774 units" I measured in run 45 and wrote off as physics: it was
    # never the up-jump (JUMP_VEL_UP 110, apex 954) at all, it was the harness
    # choosing the weaker move and then reporting the ledge unreachable.
    #
    # Eighth instrument false-defect, same shape as the other seven: a uniform
    # failure with a uniform cause in the harness. Hold UP and stay out of the
    # way - auto-arm launches, g_autograb latches the lip, then the shared
    # pull-up loop below finishes it.
    # ☠️ HOLD ACTION TOO, even though UP alone arms the auto-jump. Run 46 drove
    # UP by itself and it passed all of the Caves, which made it look right. It
    # is not: the arm gate is
    #     (pad & PAD_UP) && g_fwdblk && (g_gunst == GST_OFF || (pad & ACT_ACTION))
    # so UP alone only works while her hands are EMPTY. TR1's own rule is that
    # she never climbs without ACTION; the UP-only path is a convenience this
    # port adds on top. The Caves have no guns, so UP alone happened to work
    # there and would have failed the moment a build armed her.
    # Holding UP+ACTION satisfies the gate unconditionally AND is what the
    # airborne grab already wants, so it is correct in both worlds.
    if sp["cls"] == "JUMPGRAB":
        ctl("input", "up,b")                 # UP+ACTION: let the game auto-jump
        for _ in range(6):                   # arm -> compress -> launch -> grab
            ctl("run", 12)
            sample()
        ymid = peek(sy["g_lay"]) or ymid

    # ☠️ BOUND THE DRIVE, AND STOP WHEN SHE STOPS RISING.
    # The first version held UP+B for 8x15 = 120 fields unconditionally. Where
    # there was no ledge that simply walked her off into the level - room 11
    # spots came back 97% black, which looked like a rendering bug and was
    # purely the harness overshooting. Same trap as run 21 (120 fields carried
    # her 2,525 units, 2.5 sectors, past the target), repeated because "hold
    # through the pull-up" and "do not walk too far" pull in opposite
    # directions. Resolve it by watching Y: keep holding while she is RISING,
    # give up quickly when she is not.
    # ☠️ 8 x 15 = 120 FIELDS WAS TOO IMPATIENT. A CLIMB3 (768) pull-up runs the
    # animation at its OWN length now (run-1 fix: "ticks = cnt_pre" so it reads
    # as a climb, not a float), and a manual drive in run 25 needed ~195 fields
    # to finish one - 7680 -> 6912 exactly. At 8 iterations every CLIMB3 came
    # back PARTIAL and it looked like a real defect. Give it room; the stall
    # detector still exits early when she genuinely is not moving, so a longer
    # cap costs nothing on the spots that fail fast.
    # ☠️ EXIT ON *LANDING*, NOT ON VELOCITY. The pull-up EASES OUT, so the last
    # few units arrive slowly - and a "Y moved less than 8" test fires right
    # before she touches down. A hand drive of Caves room 19 CLIMB3 reached
    # Y 3840 with floor 3840 (an exact 768) while the harness had already
    # given up at 732 and called it PARTIAL. Same failure as run 31's window,
    # arriving through a different door.
    # The honest completion test is g_lafloor == g_lay: she is standing on the
    # surface she climbed to. Velocity only decides when to give up.
    ctl("input", "up,b")                     # HELD while the climb progresses
    ylast = ymid
    stalled = 0
    for _ in range(28):
        ctl("run", 15)
        ynow = peek(sy["g_lay"])
        if ynow is None:
            break
        if peek(sy["g_lafloor"]) == ynow and ynow < (ymid or 0) - 8:
            break                            # LANDED on a higher floor: done
        sample()
        if ylast - ynow > 2:                 # still rising at all (Y grows DOWN)
            stalled = 0
        else:
            stalled += 1
            if stalled >= 4:                 # four dead samples = really stuck
                break
        ylast = ynow
    y1, z1 = peek(sy["g_lay"]), peek(sy["g_laz"])
    fl = peek(sy["g_lafloor"])
    blk = black_pct(os.path.join(outdir, "s%03d.png" % idx))
    ctl("release")

    sample()
    rose = (y0 - best) if (y0 is not None and best is not None) else 0
    moved = abs((z1 or 0) - (z0 or 0))
    ok = rose >= sp["rise"] * 0.8 and bestfl == best   # she STOOD on it
    fl = bestfl
    return dict(rose=rose, moved=moved, floor=fl, y0=y0, y1=y1,
                black=blk, verdict=("CLIMBED" if ok else
                                    "PARTIAL" if rose > 64 else
                                    "NO-CLIMB"))


def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(2)
    rom, elf = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[sys.argv.index("--limit") + 1]) if "--limit" in sys.argv else 0
    outdir = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else "/tmp/conf"
    os.makedirs(outdir, exist_ok=True)

    sy = nm(elf)
    pfx = sys.argv[sys.argv.index("--prefix") + 1] if "--prefix" in sys.argv else "mrt"
    spots = census(pfx)
    if limit:
        spots = spots[:limit]
    print("conformance: %d spots, rom=%s" % (len(spots), os.path.basename(rom)))

    subprocess.run([JE, "instances", "--prune"], capture_output=True)
    srv = subprocess.Popen([JE, "serve", "--rom", rom, "--instance", INST],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(10)
    ctl("run", BOOT, timeout=600)

    print("%-9s %-5s %-6s %-8s %-7s %-7s %s" %
          ("CLASS", "ROOM", "RISE", "ROSE", "FLOOR", "BLACK%", "VERDICT"))
    rows = []
    for i, sp in enumerate(spots):
        try:
            r = test_spot(sy, sp, outdir, i)
        except Exception as e:
            r = dict(rose=0, moved=0, floor=None, black=-1,
                     verdict="ERROR:%s" % type(e).__name__)
        rows.append(dict(spot=sp, res=r))
        print("%-9s %-5d %-6d %-8s %-7s %-7s %s" %
              (sp["cls"], sp["room"], sp["rise"], r["rose"], r["floor"],
               ("%.1f" % r["black"]) if r["black"] >= 0 else "-", r["verdict"]))
    json.dump(rows, open(os.path.join(outdir, "results.json"), "w"), indent=1)

    # ☠️☠️ SCORE BY CLASS. "CLIMBED" is the right answer for a ledge and the
    # WRONG answer for a WALL - those spots are the NEGATIVE CONTROL, and a
    # summary that counts them as passes is congratulating the engine for
    # climbing what it must refuse. This went unnoticed while WALL only ever
    # meant a 2048/2816 step nothing could climb; once ledge_census started
    # classing no-headroom pairs as WALL (TR1 refuses a target Lara does not
    # fit in), the engine climbed several of them and the old summary read
    # 29/30 "CLIMBED" - its best score yet, describing a level that had just
    # got MORE wrong.
    climbable = [r for r in rows if r["spot"]["cls"] != "WALL"]
    walls = [r for r in rows if r["spot"]["cls"] == "WALL"]
    ok = sum(1 for r in climbable if r["res"]["verdict"] == "CLIMBED")
    refused = sum(1 for r in walls if r["res"]["verdict"] == "NO-CLIMB")
    print("\nLEDGES  %d/%d climbed   %d PARTIAL   %d failed to climb" %
          (ok, len(climbable),
           sum(1 for r in climbable if r["res"]["verdict"] == "PARTIAL"),
           sum(1 for r in climbable if r["res"]["verdict"] == "NO-CLIMB")))
    print("WALLS   %d/%d correctly refused   %d CLIMBED THAT SHOULD NOT BE" %
          (refused, len(walls), len(walls) - refused))
    for r in walls:
        if r["res"]["verdict"] != "NO-CLIMB":
            print("   ☠️ room %-3d rise %-5d rose %s"
                  % (r["spot"]["room"], r["spot"]["rise"], r["res"]["rose"]))
    # ☠️ USE THE LEVEL'S OWN BASELINE. This used to hardcode 20% "vs the 17.4%
    # ROOMTOUR baseline" - a CAVES number. Lara's Home has rooms reading 36.3%
    # black standing at their CENTRES, so that threshold reported normal rooms
    # as voids and nearly produced a fifth false defect. tools/room_black.py
    # writes tools/.black_<prefix>.
    bpath = os.path.join(D, "tools", ".black_" + pfx)
    try:
        thresh = float(open(bpath).read().strip())
        src = "%s baseline" % pfx
    except Exception:
        thresh, src = 20.0, "default (no baseline recorded - run room_black.py)"
    dark = [r for r in rows if r["res"]["black"] > thresh]
    print("%d spots ended above %.1f%% black (%s)" % (len(dark), thresh, src))
    for r in dark:
        print("   room %d %s  black %.1f%%" %
              (r["spot"]["room"], r["spot"]["cls"], r["res"]["black"]))
    srv.terminate()


if __name__ == "__main__":
    main()
