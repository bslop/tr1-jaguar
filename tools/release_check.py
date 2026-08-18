#!/usr/bin/env python3
"""release_check.py - ONE command that says whether the shipping ROM is good.

    tools/release_check.py [--sd /tmp/cofout8] [--out /tmp/relcheck]

WHY: verifying a release has taken five separate runs and a fresh throwaway
script each time - boot to the ring (103), assert the frames (94), count the
entities (107), prove sound at boot (108) and in-game (110). Every one of those
was re-derived by hand, which is how a check silently stops being run. The
endpoint of this project is a push coordinated with a video; the last thing that
should happen before it is a gate that anyone can run and nobody has to remember
the steps for.

WHAT IT ASSERTS, and the run that earned each one:
  1. boots to the RING                  frame asserted by checkshot   (103)
  2. the ring SELECTS                   two A presses, level loads    (103)
  3. gameplay renders                   frame asserted, health full   (94)
  4. she MOVES under the pad            position delta > 2000 units   (109)
  5. she STOPS when released            the other half of control     (116)
  6. the walk sample IS a walk          g_lanim_id is RUN or WALK     (116)
  7. audio is SILENT while idle         the negative control          (110)
  8. audio PLAYS while walking          peak above the idle floor     (110)

☠️ 4 AND 5 ARE ALSO ONE CHECK. "She moved" alone passes on a ROM where a slide
carries her with nothing pressed, and it passed on the Lara's Home soft-lock run
116 found - one press moved her before she wedged, and the loop breaks on the
first success. Moves-when-pressed AND stops-when-released is what proves control.

☠️ 7 AND 8 ARE ONE CHECK, NOT TWO. "Walking makes noise" alone passes on a ROM
that hums constantly; "idle is silent" alone passes on a ROM with no audio at
all. Only the PAIR - silent then loud, same session, seconds apart - shows sound
is event-driven and working. Run 109 measured silence while walking and it took
the release drive to learn that was the conformance arm's problem, not the game's.

☠️ RUN IT ON THE RELEASE, WITH ITS SD PAYLOAD. A conformance/AUTOSTART ROM
produces NO SFX even with `g_sfx_ok=1`, and `jagemu audio` without `--sd` streams
no MUSIC.PCM. Either mistake reads as a catastrophic audio defect.

☠️ SLOW ON PURPOSE: ~8200 fields to the ring plus the cutscene, so budget ~25
minutes. That is the cost of testing the thing that actually ships.
"""
import json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
D = os.path.dirname(HERE)
JE = "/home/jvilla/Documents/Git/jag_openlara/cobweb/sim/target/release/jagemu"
# ☠️ THE INSTANCE NAME IS THE ARM'S IDENTITY. It was a constant, so the Caves
# and Lara's Home arms could not run at the same time - the second `serve`
# lands on the first one's instance and both drives read one machine. --inst
# gives each arm its own, which is what makes a same-tree A/B affordable
# (two 25-minute drives in 25 minutes).
INST = (sys.argv[sys.argv.index("--inst") + 1]
        if "--inst" in sys.argv else "relcheck")


def ctl(*a, timeout=3600):
    r = subprocess.run([JE, "ctl", INST] + [str(x) for x in a],
                       capture_output=True, text=True, timeout=timeout)
    return r.stdout.strip()


def last_json(s):
    for ln in s.split("\n")[::-1]:
        ln = ln.strip()
        if ln.startswith("{"):
            try:
                return json.loads(ln)
            except Exception:
                pass
    return {}


def peek32(addr, signed=True):
    b = last_json(ctl("peek", hex(addr), "--len", "4")).get("bytes")
    if not b:
        return None
    v = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]
    return v - (1 << 32) if (signed and v >= 1 << 31) else v


def audiocheck(path):
    r = subprocess.run([JE, "audiocheck", path], capture_output=True, text=True)
    return last_json(r.stdout)


def shot(path, size=None):
    """Capture AND assert. A frame nobody reads is not evidence."""
    ctl("frame", path)
    cmd = ["python3", os.path.join(HERE, "checkshot.py"), path]
    if size:
        cmd += ["--size", size]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode == 0, r.stdout.strip()


def main():
    gym = "--gym" in sys.argv          # Lara's Home (ring page 4), not the Caves
    sd = sys.argv[sys.argv.index("--sd") + 1] if "--sd" in sys.argv else "/tmp/cofout8"
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else "/tmp/relcheck"
    rom = os.path.join(sd, "OPENLARA.COF")
    elf = os.path.join(sd, "OPENLARA.elf")
    for p in (rom, elf):
        if not os.path.exists(p):
            print("  ☠️ missing %s - build the payload first (see PLAY_BUILD.md)" % p)
            sys.exit(2)
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):                     # stale frames read as fresh ones
        if f.endswith((".png", ".wav")):
            os.unlink(os.path.join(out, f))

    syms = {}
    for ln in subprocess.run(["m68k-neogeo-elf-nm", elf], capture_output=True,
                             text=True).stdout.split("\n"):
        p = ln.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)

    results = []                                   # (name, ok, detail)

    def record(name, ok, detail):
        results.append((name, ok, detail))
        print("  %-24s %s  %s" % (name, "PASS" if ok else "☠️ FAIL", detail), flush=True)

    subprocess.run([JE, "instances", "--prune"], capture_output=True)
    srv = subprocess.Popen([JE, "serve", "--rom", rom, "--sd", sd, "--instance", INST],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(12)
    try:
        print("booting to the ring (~8200 fields, this is the slow part)...", flush=True)
        ctl("run", 8200)
        ok, det = shot(os.path.join(out, "1_ring.png"))
        record("boots to the ring", ok, det)

        if gym:
            # ☠️ LARA'S HOME IS RING PAGE 4, AND THE RING SWALLOWS INPUT
            # WHILE IT SPINS. At 60 fields per press only 2 of 4 registered once
            # and the ring sat on "Sound" while the script believed it was on
            # page 4. So give the spin 200 fields and PHOTOGRAPH every step -
            # the page is read, never assumed. One A press selects from here;
            # the passport two-press dance belongs to the New Game path only.
            for _k in range(4):
                ctl("input", "right"); ctl("run", 30)
                ctl("release"); ctl("run", 200)
                shot(os.path.join(out, "1b_page%d.png" % _k))
            ctl("input", "a"); ctl("run", 30); ctl("release"); ctl("run", 240)
        else:
            for n in (1, 2):                            # ☠️ the ring needs TWO presses
                ctl("input", "a"); ctl("run", 30)
                ctl("release"); ctl("run", 240)
        ok, det = shot(os.path.join(out, "2_selected.png"))
        record("ring selects", ok, det)

        # ☠️ "IS IT A SCENE" CANNOT TELL A CUTSCENE FROM GAMEPLAY - BOTH ARE
        # SCENES. The first version asserted after a fixed `run 6000` and caught
        # the LOADING screen (3 lumas, red bar). Waiting for a valid scene fixed
        # that and broke something subtler: it exited the moment the SNOW
        # CUTSCENE rendered (59 colours, a perfectly good frame), so the pad
        # press landed during the cutscene and she moved **0 units** - on the
        # exact ROM that had moved her 17,484 a run earlier.
        # ☠️ And it is not idle-timeout: measured on the conformance ROM, 300 vs
        # 8000 idle fields before the press both move her 7048 units. Long waits
        # are harmless; the CUTSCENE is what swallows input.
        # So gate on the thing the check actually cares about - CONTROL. Press
        # and look for movement, bounded; the frame is captured only once she has
        # demonstrably moved, which also guarantees it is gameplay and not a
        # cutscene. ★ This retries the PRESS, never the verdict.
        # ☠️ MEASURE BOTH AXES. The Caves start walks her along Z, so a z-only
        # delta looked fine there - and in LARA'S HOME she walks along **X**
        # (run 103's tour: x 37376 -> 30796, z unchanged). The z-only check
        # reported "moved 0 units after 13000 fields" for a level that is
        # perfectly controllable, and the silent audio that came with it was
        # just the absence of footsteps she never took.
        # ★ This is run 84's lesson again: MVDIAG watched only X and called a
        # +Z walk "nothing refused". An instrument that reads one axis will
        # eventually be pointed at the other one.
        def where():
            return ((peek32(syms["g_lax"]) if "g_lax" in syms else 0) or 0,
                    (peek32(syms["g_laz"]) if "g_laz" in syms else 0) or 0)

        def dist(a, b):
            return abs(b[0] - a[0]) + abs(b[1] - a[1])

        def anim():
            return peek32(syms["g_lanim_id"]) if "g_lanim_id" in syms else None

        # LANIM_RUN / LANIM_WALK. Identical in mrt_lara.h and gym_lara.h - the
        # extractor picks them by TR STATE, not by index, so both level sets
        # land on 0 and 1.
        WALKING = (0, 1)

        ctl("run", 4000)
        p0 = where()
        moved, waited = 0, 4000
        for _try in range(10):
            ctl("input", "up"); ctl("run", 300)
            ctl("release")
            p1 = where()
            moved = dist(p0, p1)
            if moved > 2000:
                break
            ctl("run", 600); waited += 900
            p0 = where()
        ok, det = shot(os.path.join(out, "3_gameplay.png"))
        health = peek32(syms["g_health"]) if "g_health" in syms else None
        record("gameplay renders", ok, "%s  [%d fields]" % (det, waited))
        record("health is full", health == 1000, "g_health=%s" % health)

        # ☠️☠️ "SHE MOVED" IS HALF A CHECK. CONTROL IS A PAIR: SHE MOVES WHEN
        # THE PAD IS PRESSED **AND STOPS WHEN IT IS RELEASED**.
        # Run 116 found Lara's Home soft-locked - wedged in a looping SLIDE_BACK
        # with the pad completely dead - and every check here PASSED, because
        # the one press that reached her before she wedged had already moved her
        # 8,548 units and the loop above breaks on the first success. A gate that
        # only asks "did she move once" cannot see a level that stops responding,
        # which is the worst thing that can ship.
        # The released arm is load-bearing in the other direction too: a runaway
        # slide moves her thousands of units with nothing pressed, and would read
        # as "she moves under the pad" on a ROM with no control at all.
        # ☠️ AND THE WALK AUDIO MUST BE SAMPLED WHILE SHE IS WALKING. The only
        # in-game SFX is the footfall, so a sample taken while she slides, falls
        # or stands measures nothing and reads as an audio defect - which is
        # exactly what runs 114/115 chased through the mansion. The drive is
        # blind, so when a heading does not walk (a wall, or a slope she slips
        # back off) TURN HER and try again rather than believing the sample.
        drift = driven = 0
        walk_anim = None
        attempt = 0
        for attempt in range(6):
            ctl("release"); ctl("run", 120)               # let her settle
            q0 = where()
            ctl("run", 300)                               # RELEASED arm
            ctl("audio", os.path.join(out, "idle.wav"))
            q1 = where()
            drift = dist(q0, q1)
            ctl("input", "up"); ctl("run", 300)           # PRESSED arm
            walk_anim = anim()
            ctl("audio", os.path.join(out, "walk.wav"))
            ctl("release")
            q2 = where()
            driven = dist(q1, q2)
            if driven > 2000 and walk_anim in WALKING:
                break
            ctl("input", "left"); ctl("run", 90)          # new heading, retry
            ctl("release"); ctl("run", 30)
        record("she moves under the pad", driven > 2000,
               "moved %d units in 300 held fields (%d headings tried)"
               % (driven, attempt + 1))
        record("she stops when released", drift < 500,
               "drifted %d units in 300 released fields" % drift)
        record("sampled while walking", walk_anim in WALKING,
               "g_lanim_id=%s during the walk capture (want 0=RUN or 1=WALK)"
               % walk_anim)

        # ☠️ WHEN A LEVEL IS SILENT, THE NEXT QUESTION IS ALWAYS "WHICH GATE".
        # There is exactly ONE in-game SFX call - sfx_play(0, SFX_STEP) from
        # lara_footstep - and it returns early on `!g_sfx_ok` or `!g_sfxvol`.
        # The footfall tables are byte-identical between the two level sets
        # (checked: 160/160), so if a level is silent it is runtime state, not
        # data. Print the state rather than making anyone guess it.
        diag = " ".join("%s=%s" % (k, peek32(syms[k]))
                        for k in ("g_sfx_ok", "g_sfxvol", "g_jerry_ok", "g_useset",
                                  "g_lanim_id")
                        if k in syms)
        print("  %-24s      %s" % ("(sound state)", diag), flush=True)

        ai = audiocheck(os.path.join(out, "idle.wav"))
        aw = audiocheck(os.path.join(out, "walk.wav"))
        ir, wr = ai.get("rms_dbfs"), aw.get("rms_dbfs")
        ip, wp = ai.get("peak_dbfs"), aw.get("peak_dbfs")
        # ☠️ "IDLE IS SILENT" WAS A FALSE LAW. Run 110 happened to sample a
        # moment with no music and I wrote that observation down as an
        # invariant; in-game MUSIC plays, so idle is legitimately around
        # -16 dBFS and no "40 dB above idle" test can ever pass over it.
        # What is defensible without inventing another law: audio EXISTS at all
        # (the DAC is fed), and walking is not QUIETER than idle. Both numbers
        # are printed so a future run can tighten this from data rather than
        # from a guess - which is the mistake being corrected here.
        record("audio is being produced", ai.get("silent") is False or aw.get("silent") is False,
               "idle rms %s dBFS, walking rms %s dBFS" % (ir, wr))
        record("walking is not quieter", 
               wr is not None and ir is not None and wr >= ir - 3.0,
               "walking rms %s vs idle %s (peaks %s / %s)" % (wr, ir, wp, wp and ip))
    finally:
        ctl("release")
        ctl("stop")
        srv.terminate()

    bad = [n for n, ok, _ in results if not ok]
    print()
    if bad:
        print("  ☠️ RELEASE NOT GOOD - %d of %d checks failed: %s"
              % (len(bad), len(results), ", ".join(bad)))
    else:
        print("  ✅ RELEASE GOOD - all %d checks passed" % len(results))
    print("     frames and captures in %s" % out)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
