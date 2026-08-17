#!/usr/bin/env python3
"""Drive the RELEASE ROM past the title ring and into the level, then capture.

The film proves the front-end (logo -> FMV -> ring). It cannot prove the GAME,
because the ring waits for a button and `jagemu video` has no input. So use a
ctl session: boot, press A at the ring, let the level load, capture frames.
"""
import subprocess, sys, time, os

JE = "/home/jvilla/Documents/Git/jag_openlara/cobweb/sim/target/release/jagemu"
ROM = "/tmp/cofout5/OPENLARA.COF"
SD = "/tmp/cofout5"
INST = "rel"
OUT = "/tmp/rel5play"


def ctl(*a, timeout=900):
    return subprocess.run([JE, "ctl", INST] + [str(x) for x in a],
                          capture_output=True, text=True, timeout=timeout).stdout.strip()


os.makedirs(OUT, exist_ok=True)
subprocess.run([JE, "instances", "--prune"], capture_output=True)
srv = subprocess.Popen([JE, "serve", "--rom", ROM, "--sd", SD, "--instance", INST],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(10)
try:
    print("booting to the ring...", flush=True)
    ctl("run", 8200, timeout=3600)
    ctl("frame", os.path.join(OUT, "a_ring.png"))
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
            print("  captured g_%d after +%d fields" % (i, n), flush=True)
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
    for i, n in enumerate((2500, 2500, 2500, 2500)):
        ctl("run", n, timeout=1800)
        ctl("frame", os.path.join(OUT, "b_%d.png" % i))
        print("  captured b_%d after +%d fields" % (i, n), flush=True)
finally:
    ctl("release")
    srv.terminate()
print("done")
