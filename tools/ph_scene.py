#!/usr/bin/env python3
# PERFHUNT scene profiler: serve/ctl driver.
import os
# Per scene: boot to steady state, then a 600-frame window collecting
#  - cumulative GPU timing at window start/end (steady-state decomposition)
#  - displist[0] write timestamps via watch (render cadence -> frame-time variance)
#  - GPU PC samples every 2 frames (sampling profiler; separates halt-idle from render)
import json, subprocess, sys, time

JAGEMU = os.environ.get("JAGEMU", "jagemu")
DISPLIST = None  # set per build below

def ctl(inst, *args):
    r = subprocess.run([JAGEMU, "ctl", inst] + [str(a) for a in args],
                       capture_output=True, text=True, timeout=300)
    out = r.stdout.strip()
    try:
        return json.loads(out)
    except Exception:
        return {"raw": out, "err": r.stderr.strip()}

def state(inst):
    return ctl(inst, "state")

def main():
    global DISPLIST
    inst, cof, drive = sys.argv[1], sys.argv[2], (len(sys.argv) > 3 and sys.argv[3] == "drive")
    outf = sys.argv[4] if len(sys.argv) > 4 else f"ph_scene_{inst}.json"
    DISPLIST = (0x1732cc if ("m68d" in cof or "PLAY_PH" in cof) else (0x173058 if "spawn" in cof else 0x172f48))  # PH_rd_* share PH_base BSS layout
    # launch serve
    p = subprocess.Popen([JAGEMU, "serve", "--rom", cof, "--instance", inst,
                          "--fidelity", "silicon"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    try:
        ctl(inst, "run", 650)                       # boot + settle
        if drive:
            ctl(inst, "input", "up")                # hold run forward
            ctl(inst, "run", 60)                    # get moving before window
        ctl(inst, "watch", hex(DISPLIST))
        s0 = state(inst)
        pcs = []
        cpcs = []
        hits = []
        for i in range(600):                        # 600 x 1 = 600 frames
            ctl(inst, "run", 1)
            st = state(inst)
            pcs.append(st["state"]["gpu"]["pc_hex"])
            cpcs.append(st["state"]["pc_hex"])
            if drive and i == 300:
                ctl(inst, "input", "up,left")       # add a turn mid-window
            if (i + 1) % 50 == 0:
                w = ctl(inst, "watchlog")
                if "hits" in w:
                    hits.extend(w["hits"])
                else:
                    hits.extend(w.get("watch", {}).get("hits", []))
        s1 = state(inst)
        json.dump({"cof": cof, "drive": drive,
                   "s0": s0["state"], "s1": s1["state"],
                   "pcs": pcs, "cpcs": cpcs, "hits": hits}, open(outf, "w"))
        print("wrote", outf)
    finally:
        ctl(inst, "stop")
        p.wait(timeout=10)

main()
