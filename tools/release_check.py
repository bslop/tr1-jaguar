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
  5. audio is SILENT while idle         the negative control          (110)
  6. audio PLAYS while walking          peak above the idle floor     (110)

☠️ 5 AND 6 ARE ONE CHECK, NOT TWO. "Walking makes noise" alone passes on a ROM
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
INST = "relcheck"


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

        for n in (1, 2):                            # ☠️ the ring needs TWO presses
            ctl("input", "a"); ctl("run", 30)
            ctl("release"); ctl("run", 240)
        ok, det = shot(os.path.join(out, "2_selected.png"))
        record("ring selects", ok, det)

        # ☠️ DO NOT ASSERT ON A FIXED DELAY - THE LEVEL IS STILL LOADING.
        # A flat `run 6000` then capture caught the LOADING screen: black, a
        # red progress bar, 3 distinct lumas, and it failed the scene check
        # while the game was perfectly healthy (she walked 17,484 units 300
        # fields later). The load does not take a constant time, so WAIT for the
        # scene instead of guessing - bounded, and report how long it took, so a
        # level that gets slower to load is visible rather than absorbed.
        # ★ This retries the CAPTURE, never the verdict: if no attempt produces a
        # scene the check still FAILS.
        ctl("run", 4000)                            # through the snow cutscene
        ok, det, waited = False, "", 4000
        for _try in range(8):
            ok, det = shot(os.path.join(out, "3_gameplay.png"))
            if ok:
                break
            ctl("run", 600)
            waited += 600
        det = "%s  [after %d fields]" % (det, waited)
        health = peek32(syms["g_health"]) if "g_health" in syms else None
        record("gameplay renders", ok, det)
        record("health is full", health == 1000, "g_health=%s" % health)

        z0 = peek32(syms["g_laz"]) if "g_laz" in syms else None
        ctl("audio", os.path.join(out, "idle.wav"))
        ctl("input", "up"); ctl("run", 300)
        ctl("audio", os.path.join(out, "walk.wav"))
        ctl("release")
        z1 = peek32(syms["g_laz"]) if "g_laz" in syms else None
        moved = abs((z1 or 0) - (z0 or 0))
        record("she moves under the pad", moved > 2000, "z moved %d units" % moved)

        ai = audiocheck(os.path.join(out, "idle.wav"))
        aw = audiocheck(os.path.join(out, "walk.wav"))
        ip, wp = ai.get("peak_dbfs"), aw.get("peak_dbfs")
        # the PAIR is the check - see the docstring
        record("idle is silent", ai.get("silent") is True, "idle peak %s dBFS" % ip)
        record("walking makes sound",
               wp is not None and ip is not None and wp > ip + 40,
               "walking peak %s dBFS vs idle %s" % (wp, ip))
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
