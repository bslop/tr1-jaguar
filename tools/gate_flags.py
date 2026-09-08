#!/usr/bin/env python3
"""gate_flags.py - read a build log and say what actually reached the compile line.

Split out of gate_release.sh because the shell version got all three of its
answers wrong on its first real build, and every one of the three was a
regex-vs-reality problem that is much easier to see (and to test) in one place:

  * ☠️ THE INVOCATIONS ARE PATH-PREFIXED. It matched " jas " with a leading
    space against a build whose every line reads `/usr/local/bin/jas ...`, and
    so reported HRESN missing from the one define path it had certainly reached.
    Anchor on a separator, not on a space.
  * ☠️ "ABSENT" AND "OFF" ARE DIFFERENT STATES for jas, which is passed every
    define explicitly. The HQ title kernel must carry `HRESN=0`; merely lacking
    the token would mean the GEOTEX_HQ substitution had stopped running, which
    is a different bug with the same silence.
  * ☠️ `-DFOO=0` STILL DEFINES FOO. On a gcc line any mention counts as ON --
    this Makefile is documented to turn a feature off by OMITTING it. On a jas
    line `-d FOO=0` is genuinely off, because the .gas sources test `.if FOO=1`.
    One string, two opposite meanings, decided by which tool is reading it.

    tools/gate_flags.py <build.log> [--width 160]
"""
import re, sys

FLAGS = ("FPSBEACON PADMUTE FASTBOOT AUTOSTART SPAWNAT DBGROOM HOLEVIS GUNDIAG "
         "CRUMB ROOMTOUR OTLIST NOFILL NOCLEAR HALFW WORLDCOUNT AUTOMENU").split()


def say(tag, msg):
    print("  %-6s %s" % (tag, msg))


def main():
    log = open(sys.argv[1], errors="replace").read().splitlines()
    width = "160"
    if "--width" in sys.argv:
        width = sys.argv[sys.argv.index("--width") + 1]

    def lines(tool):
        return [l for l in log if re.search(r"(^|/|\s)" + tool + r"\s", l)]

    # ☠️ ONLY THE .c TUs CAN CARRY A -D THAT MATTERS. Counting every gcc line
    # reported "4 of 11" on a correct build, because the other seven compile .S
    # files (startup, cpu68k, gdbios, the three blob wrappers, mrt_data) which
    # contain no reference to VIEW_W/HRESN/RENDER_W at all. A denominator that
    # includes lines that COULD NOT pass reads as a failure on a passing build.
    gcc = [l for l in lines(r"[a-z0-9_-]*gcc")
           if re.search(r"\s\S+\.c(\s|$)", l) and " -c " in l]
    jcc = lines("jcc68k")
    jas = lines("jas")
    gk = [l for l in jas if "gpu_geotex.gas" in l and "_hq" not in l]
    hq = [l for l in jas if "gpu_geotex_hq" in l]

    ok = True
    print("[1] HRESN on all three define paths")
    pat = r"HRESN=" + re.escape(width) + r"\b"
    ngcc = sum(1 for l in gcc if re.search(r"-D" + pat, l))
    njcc = sum(1 for l in jcc if re.search(pat, l))
    njas = sum(1 for l in gk if re.search(pat, l))
    say("gcc", "%d of %d compile lines carry -DHRESN=%s" % (ngcc, len(gcc), width))
    say("jcc68k", "%d of %d lines carry HRESN=%s" % (njcc, len(jcc), width))
    say("jas", "%d of %d game-kernel lines carry HRESN=%s" % (njas, len(gk), width))
    if not (ngcc and njcc and njas):
        print("  !!! HRESN did not reach every define path")
        ok = False

    if not hq:
        print("  !!! no HQ-kernel assembly found - GEOTEX_HQ did not build")
        ok = False
    elif [l for l in hq if not re.search(r"HRESN=0\b", l)]:
        print("  !!! HQ kernel does not carry HRESN=0 - the title would render narrow")
        ok = False
    else:
        say("HQ", "%d HQ-kernel line(s) carry HRESN=0 - the title stays 320" % len(hq))

    print()
    print("[2] instrument flags")
    on = []
    for f in FLAGS:
        hit = None
        for l in gcc + jcc:
            if re.search(r"-D" + f + r"(=|\s|$)", l):
                hit = f + " (gcc/jcc -D, and -DFOO=0 still defines FOO)"
                break
        if hit is None:
            for l in jas:
                m = re.search(r"-d\s+" + f + r"=(\S+)", l)
                if m and m.group(1) != "0":
                    hit = "%s=%s (jas)" % (f, m.group(1))
                    break
        if hit:
            on.append(hit)
    if on:
        say("FAIL", "instrument flags ON: " + ", ".join(on))
        ok = False
    else:
        say("ok", "none of the %d instrument flags is on" % len(FLAGS))

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
