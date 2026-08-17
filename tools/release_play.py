#!/usr/bin/env python3
"""Drive the RELEASE ROM past the title ring and into the level, then capture.

The film proves the front-end (logo -> FMV -> ring). It cannot prove the GAME,
because the ring waits for a button and `jagemu video` has no input. So use a
ctl session: boot, press A at the ring, let the level load, capture frames.
"""
import subprocess, sys, time, os

JE = "/home/jvilla/Documents/Git/jag_openlara/cobweb/sim/target/release/jagemu"
ROM = os.environ.get("REL_ROM", "/tmp/cofout6/OPENLARA.COF")
SD = os.environ.get("REL_SD", "/tmp/cofout6")
ELF = os.environ.get("REL_ELF", "/tmp/cofout6/OPENLARA.elf")
INST = "rel"
OUT = "/tmp/rel5play"


def ctl(*a, timeout=900):
    return subprocess.run([JE, "ctl", INST] + [str(x) for x in a],
                          capture_output=True, text=True, timeout=timeout).stdout.strip()


# ☠️ SYMBOLS ARE PER-BUILD. Read them from the ELF that came out of the SAME
# build as this ROM (build_cof.sh keeps it beside the COF as OPENLARA.elf).
# Using a conformance ROM's map here reads whatever happens to live at those
# addresses - g_health alone moves 0x13f936 -> 0x17b7f6 between the two.
SYMS = {}
if os.path.exists(ELF):
    import json as _json
    for _ln in subprocess.run(["m68k-neogeo-elf-nm", ELF],
                              capture_output=True, text=True).stdout.split("\n"):
        _p = _ln.split()
        if len(_p) == 3:
            SYMS[_p[2]] = int(_p[0], 16)


def peek(name, signed=True):
    """Read a 4-byte variable by NAME, or None if this build has no such symbol."""
    if name not in SYMS:
        return None
    import json as _json
    for ln in ctl("peek", hex(SYMS[name]), "--len", "4").split("\n")[::-1]:
        try:
            b = _json.loads(ln).get("bytes")
        except Exception:
            continue
        if b:
            v = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]
            return v - (1 << 32) if (signed and v >= 1 << 31) else v
    return None


def tele():
    """One line of where-is-she, for correlating a capture with the game state."""
    return "x=%s z=%s y=%s floor=%s room=%s health=%s" % tuple(
        peek(n) for n in ("g_lax", "g_laz", "g_lay", "g_lafloor", "g_curroom", "g_health"))


os.makedirs(OUT, exist_ok=True)
subprocess.run([JE, "instances", "--prune"], capture_output=True)
srv = subprocess.Popen([JE, "serve", "--rom", ROM, "--sd", SD, "--instance", INST],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(10)
try:
    print("booting to the ring...", flush=True)
    ctl("run", 8200, timeout=3600)
    ctl("frame", os.path.join(OUT, "a_ring.png"))
    def tour_loop(n=30):
        """Wall-follow: walk, and when she stops, turn the SAME way and retry.
        ☠️ Alternating the turn direction just reverses her (measured run 68:
        56,802 units, no new ground). Same-way turning makes a dead end a
        corner."""
        stuck, dist = 0, 0
        px, pz = peek("g_lax"), peek("g_laz")
        rooms = set()
        for i in range(n):
            ctl("input", "up")
            ctl("run", 120, timeout=1800)
            ctl("release"); ctl("run", 20)
            nx, nz = peek("g_lax"), peek("g_laz")
            moved = abs((nx or 0) - (px or 0)) + abs((nz or 0) - (pz or 0))
            dist += moved
            if moved < 64:
                stuck += 1
                ctl("input", "right")
                ctl("run", 45 + 25 * (stuck % 4))
                ctl("release"); ctl("run", 10)
            else:
                stuck = 0
            px, pz = nx, nz
            r = peek("g_curroom")
            if r is not None:
                rooms.add(r)
            ctl("frame", os.path.join(OUT, "t_%02d.png" % i))
            print("  %2d moved %-6d total %-7d %s" % (i, moved, dist, tele()), flush=True)
        print("ROOMS VISITED: %s   distance %d" % (sorted(rooms), dist), flush=True)

    if "--gym" in sys.argv:
        # LARA'S HOME is ring PAGE 4 (main.c:6729) and PAD_RIGHT advances the
        # page (main.c:6694), so rotate 4 then select. ☠️ Under GYMSD the menu
        # deliberately REFUSES this item because its blobs are stubs - the
        # release is built WITHOUT GYMSD, so a refusal here would be a real bug.
        # ☠️ THE RING SWALLOWS INPUT WHILE IT SPINS. At 60 fields per press only
        # 2 of 4 registered and the ring sat on "Sound" (page 2) while the script
        # believed it was on page 4. The ring also PACES itself to ~10fps, so a
        # press needs the spin to finish: 200 fields, and capture each step so
        # the page is READ, never assumed.
        for i in range(4):
            ctl("input", "right"); ctl("run", 30); ctl("release"); ctl("run", 200)
            ctl("frame", os.path.join(OUT, "g_step%d.png" % i))
        ctl("frame", os.path.join(OUT, "g_page4.png"))
        print("rotated to ring page 4 (Lara's Home)", flush=True)
        ctl("input", "a"); ctl("run", 30); ctl("release")
        for i, n in enumerate((900, 1800, 1800, 1800)):
            ctl("run", n, timeout=1800)
            ctl("frame", os.path.join(OUT, "g_%d.png" % i))
            print("  captured g_%d after +%d fields  %s" % (i, n, tele()), flush=True)
        if "--tour" in sys.argv:
            print("touring Lara's Home", flush=True)
            tour_loop(24)
        raise SystemExit

    # ☠️ THE RING NEEDS **TWO** PRESSES. The first opens the PASSPORT at its
    # "Start Game" page (verified: the capture showed the open book with
    # "A Select / Start Game / B Back"); the second actually starts. One press
    # left every later frame identical and read as "the press did nothing".
    for n, tag in ((1, "open passport"), (2, "start game")):
        print("pressing A (%s)" % tag, flush=True)
        ctl("input", "a")
        ctl("run", 30)
        ctl("release")
        ctl("run", 240)
        ctl("frame", os.path.join(OUT, "press%d.png" % n))
    # ☠️ "Start Game" runs the SNOW CUTSCENE before the level. The first pass
    # captured only that and looked like the game had not started. Run long
    # enough to get through it.
    if "--tour" in sys.argv and "--gym" not in sys.argv:
        # ☠️ PRESSING UP FOREVER STOPS AT THE FIRST WALL. The --play drive walked
        # 16 sectors and then sat at x=74704 z=21444 for nine straight samples,
        # which looks like a hang and is just a corridor turning. Watch her
        # POSITION and turn when she stops: that is the difference between a
        # 16-sector capture and a play-through.
        ctl("run", 6000, timeout=3600)
        turn, stuck, dist = "left", 0, 0
        px, pz = peek("g_lax"), peek("g_laz")
        for i in range(30):
            ctl("input", "up")
            ctl("run", 120, timeout=1800)
            ctl("release"); ctl("run", 20)
            nx, nz = peek("g_lax"), peek("g_laz")
            moved = abs((nx or 0) - (px or 0)) + abs((nz or 0) - (pz or 0))
            dist += moved
            if moved < 64:                      # wedged - turn and try again
                stuck += 1
                # ☠️ ALTERNATING THE TURN JUST REVERSES HER. Measured: she
                # ping-ponged along one corridor between z 15396 and z 21480 for
                # the whole tour, covering 56,802 units and NO new ground, and
                # never left room 0. Turn the SAME way every time (classic
                # wall-following) so a dead end becomes a corner, not a U-turn.
                ctl("input", "right")
                ctl("run", 45 + 25 * (stuck % 4))
                ctl("release"); ctl("run", 10)
            else:
                stuck = 0
            px, pz = nx, nz
            ctl("frame", os.path.join(OUT, "t_%02d.png" % i))
            print("  %2d moved %-6d total %-7d %s" % (i, moved, dist, tele()), flush=True)
        raise SystemExit

    if "--play" in sys.argv:
        # ☠️ VERIFY THE HEADLINE FEATURES IN THE SHIPPING ROM, not in a test
        # build. The conformance sweeps prove CLIMBING; nothing has ever
        # confirmed that ENEMIES and PICKUPS appear in the release the user
        # would flash. Walk her forward through the opening caves and film it.
        ctl("run", 6000, timeout=3600)          # through the cutscene into play
        ctl("frame", os.path.join(OUT, "p_00.png"))
        for i in range(14):
            ctl("input", "up")
            ctl("run", 120, timeout=1800)
            ctl("release")
            ctl("run", 30)
            ctl("frame", os.path.join(OUT, "p_%02d.png" % (i + 1)))
            print("  walked %2d  %s" % (i + 1, tele()), flush=True)
        raise SystemExit

    for i, n in enumerate((2500, 2500, 2500, 2500)):
        ctl("run", n, timeout=1800)
        ctl("frame", os.path.join(OUT, "b_%d.png" % i))
        print("  captured b_%d after +%d fields" % (i, n), flush=True)
finally:
    ctl("release")
    srv.terminate()
print("done")
