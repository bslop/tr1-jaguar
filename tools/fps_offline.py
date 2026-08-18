#!/usr/bin/env python3
"""fps_offline.py - RENDERED frames per second, with no capture card at all.

    fps_offline.py --rom X.cof --elf X.elf --inst a [--boot 1500]
                   [--window 3600] [--drive up]

fps = 60 * delta(g_drawframes) / delta(frame_count), which is main.c:1143's own
formula. Needs DREWVIS=1 (that is where g_drawframes is ticked).

☠️ frame_count IS FIELDS, not frames. Reading it as frames reports a flat
60.00 fps on every arm - which is exactly what the first version of this tool
did, and it would have "proved" an 18% regression costless.
☠️ g_drawframes IS `static volatile` FOR A REASON. Nothing in a shipping build
reads it, so without volatile gcc deletes it and `nm` has no symbol to peek -
the instrument main.c documents was silently unusable on every shipping-flag
arm until 2026-08-18.
☠️ AND A STATIC SCENE PRICES NOTHING. Measuring room 22 with Lara idle and the
wolves asleep beyond their activation radius gave two arms with completely
different enemy code the SAME 400 frames: the code under test never ran. Use
--drive to hold a direction through the window so the paths being priced
execute.
★ A pixel A/B between two arms is NOT available as a cross-check: movement
advances per RENDERED frame, so the faster arm is somewhere else by the same
field number and every frame differs (measured: 74% of pixels, on a change
that touches nothing visible).
"""
import json, os, subprocess, sys, time
JE="/home/jvilla/Documents/Git/jag_openlara/cobweb/sim/target/release/jagemu"
A=sys.argv
def arg(k, d=None):
    return A[A.index(k)+1] if k in A else d
INST=arg("--inst","fps"); ROM=arg("--rom"); ELF=arg("--elf")
BOOT=int(arg("--boot","1500")); WIN=int(arg("--window","1800"))
DRIVE=arg("--drive")   # hold this button through the window
def ctl(*a):
    return subprocess.run([JE,"ctl",INST]+[str(x) for x in a],
                          capture_output=True,text=True,timeout=3600).stdout.strip()
def lj(s):
    for ln in s.split("\n")[::-1]:
        ln=ln.strip()
        if ln.startswith("{"):
            try: return json.loads(ln)
            except Exception: pass
    return {}
def peek32(a):
    b=lj(ctl("peek",hex(a),"--len","4")).get("bytes")
    return None if not b else (b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3]
sym={}
for ln in subprocess.run(["m68k-neogeo-elf-nm",ELF],capture_output=True,text=True).stdout.split("\n"):
    p=ln.split()
    if len(p)==3: sym[p[2]]=int(p[0],16)
srv=subprocess.Popen([JE,"serve","--rom",ROM,"--instance",INST],
                     stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
time.sleep(10)
try:
    # ☠️ frame_count is FIELDS (the VI ISR ticks it 60x/s whatever renders) -
    # reading it as frames reports a flat 60.00 fps on every arm, which is
    # exactly what the first run of this tool did. g_drawframes is the
    # RENDERED-frame counter and main.c:1143 states the formula.
    ctl("run",BOOT)
    # ☠️ A STATIC SCENE PRICES NOTHING. Measuring room 22 with Lara idle and the
    # wolves dormant beyond their 8192 activation radius reported the same 400
    # frames for arms whose enemy code differed completely - the code under test
    # never ran. Hold a direction through the window so she walks in, the wolves
    # wake, and every path being priced actually executes.
    if DRIVE: ctl("input", DRIVE)
    d0=peek32(sym["g_drawframes"]); f0=peek32(sym["frame_count"])
    ctl("run",WIN)
    d1=peek32(sym["g_drawframes"]); f1=peek32(sym["frame_count"])
    if DRIVE: ctl("release")
    print("%s: %d rendered frames / %d fields = %.2f fps"
          % (os.path.basename(ROM), d1-d0, f1-f0, (d1-d0)*60.0/max(1,f1-f0)))
finally:
    ctl("stop"); srv.terminate()
