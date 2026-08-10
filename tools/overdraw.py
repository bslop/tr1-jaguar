#!/usr/bin/env python3
"""room_cycles.py - OFFLINE per-room geotex kernel cycle profiler (jtest).

For each level room: emit a flat org-$4000 GPU program that copies the REAL
shipped gpu_geotex.bin into SRAM $F03000 (cycle-faithful execution address),
writes the kick param block exactly like gpu_geotex_kick(), and jumps in.
The room's REAL geometry blob + a room-centre camera + the real atlas ride
in the same flat blob. jtest (silicon-fidelity) runs it; the driver
budget-bisects until the DONE magic ($0A3DD05E) lands in the mailbox
capture -> cycles-to-done per room, no rig, no A10, seconds per room.

Usage:  python3 tools/room_cycles.py [--rooms 0,5,34] [--yaw 0]
Needs:  build/gpu_geotex.bin (make the ship flag set first), mrt.bin,
        mrt_geom.bin, mrt_atlas.bin next to the script's parent dir.
"""
import os, struct, subprocess, sys, tempfile

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JTEST = os.path.expanduser("~/Documents/Git/cobweb/sim/target/release/jtest")
KERN = os.path.join(D, "build/gpu_geotex.bin")
MAGIC = 0x0A3DD05E

# fixed DRAM addresses (free in the sim)
MAILBOX = 0x100000
VTXCACHE = 0x190000
FB = 0x1A0000

def sintab(a):
    import math
    return int(round(math.sin((a & 255) * 2 * math.pi / 256) * 65536))

def build_room_inputs(idx_room):
    mrt = open(os.path.join(D, "mrt.bin"), "rb").read()
    geom = open(os.path.join(D, "mrt_geom.bin"), "rb").read()
    n = struct.unpack(">H", mrt[0:2])[0]
    offs = []
    for i in range(n):
        goff, soff = struct.unpack(">II", mrt[8 + i*8: 16 + i*8])
        offs.append(goff & 0x7FFFFFFF)
    ends = offs[1:] + [len(geom)]
    blob = geom[offs[idx_room]:ends[idx_room]]
    vc, qc, tc, aw, ah = struct.unpack(">HHHHH", blob[:10])
    offX, offY, offZ = struct.unpack(">hhh", blob[10:16])
    xs = []; ys = []; zs = []
    for i in range(vc):
        x, y, z = struct.unpack(">hhh", blob[16 + i*8:22 + i*8])
        xs.append(x + (offX << 8)); ys.append(y); zs.append(z + (offZ << 8))
    cx = (min(xs) + max(xs)) // 2
    cy = (min(ys) + max(ys)) // 2
    cz = (min(zs) + max(zs)) // 2
    return blob, (vc, qc, tc), (cx, cy, cz)

def camblk_bytes(cx, cy, cz, yaw):
    cY = sintab(yaw + 64); sY = sintab(yaw)
    cP = sintab(6 + 64);  sP = sintab(6)
    camx = cx - ((sY * 1200) >> 16)
    camz = cz - ((cY * 1200) >> 16)
    camy = cy - 200
    return struct.pack(">7i", cY >> 4, sY >> 4, cP >> 4, sP >> 4,
                       camx, camy, camz)

WRAP = """\t.gpu
\t.org\t$4000
start:
\tmovei\t#kern,r10
\tmovei\t#$F03000,r11
\tmovei\t#{klongs},r12
kcopy:
\tload\t(r10),r13
\taddq\t#4,r10
\tstore\tr13,(r11)
\tsubq\t#1,r12
\tjr\tNE,kcopy
\taddq\t#4,r11
\tmovei\t#$F03F00,r14
\tmovei\t#roomb,r0
\tstore\tr0,(r14)
\tmovei\t#{mailbox},r0
\tstore\tr0,(r14+1)
\tmovei\t#{vtxcache},r0
\tstore\tr0,(r14+2)
\tmovei\t#{fb},r0
\tstore\tr0,(r14+3)
\tmovei\t#camb,r0
\tstore\tr0,(r14+4)
\tmovei\t#atl,r0
\tstore\tr0,(r14+5)
\tmovei\t#256,r0
\tstore\tr0,(r14+6)
\tmoveq\t#0,r0
\tstore\tr0,(r14+7)
\tmovei\t#{mailbox},r14
\tmoveq\t#0,r0
\tstore\tr0,(r14)
\tstore\tr0,(r14+1)
\tmovei\t#$F03000,r0
\tjump\tT,(r0)
\tnop
\t.align\t4
kern:
\t.incbin\t"{kern}"
\t.align\t4
camb:
\t.incbin\t"{camb}"
\t.align\t8
roomb:
\t.incbin\t"{roomb}"
\t.align\t8
atl:
\t.incbin\t"{atlas}"
"""

def cycles_for(idx, yaw, workdir, quiet=True):
    blob, counts, centre = build_room_inputs(idx)
    rb = os.path.join(workdir, "room.bin"); open(rb, "wb").write(blob)
    cb = os.path.join(workdir, "camb.bin")
    open(cb, "wb").write(camblk_bytes(*centre, yaw))
    wg = os.path.join(workdir, "wrap.gas")
    ksz = os.path.getsize(KERN)
    open(wg, "w").write(WRAP.format(
        klongs=(ksz + 3)//4, mailbox=MAILBOX, vtxcache=VTXCACHE, fb=FB,
        kern=KERN, camb=cb, roomb=rb,
        atlas=os.path.join(D, "mrt_atlas.bin")))
    gf = os.path.join(workdir, "cap.bin")
    def done(budget):
        if os.path.exists(gf): os.remove(gf)
        r = subprocess.run([JTEST, "golden", wg, "--assemble",
                            "--org", "0x4000", "--budget", str(budget),
                            "--capture", f"0x{MAILBOX:X}:8",
                            "--golden", gf, "--update"],
                           capture_output=True, text=True)
        if not os.path.exists(gf): return False
        cap = open(gf, "rb").read()
        return len(cap) >= 4 and struct.unpack(">I", cap[:4])[0] == MAGIC
    lo, hi = 1000, 64_000_000
    if not done(hi):
        return None, counts
    while lo + 1000 < hi:
        mid = (lo + hi) // 2
        if done(mid): hi = mid
        else: lo = mid
    return hi, counts


ODRAW_ADDR = 0x001C0020

def overdraw_for(idx, yaw, workdir, budget):
    """run the room ONCE at a budget known to complete, capture the npix total"""
    blob, counts, centre = build_room_inputs(idx)
    rb = os.path.join(workdir, "room.bin"); open(rb, "wb").write(blob)
    cb = os.path.join(workdir, "camb.bin")
    open(cb, "wb").write(camblk_bytes(*centre, yaw))
    wg = os.path.join(workdir, "wrap.gas")
    ksz = os.path.getsize(KERN)
    open(wg, "w").write(WRAP.format(
        klongs=(ksz + 3)//4, mailbox=MAILBOX, vtxcache=VTXCACHE, fb=FB,
        kern=KERN, camb=cb, roomb=rb,
        atlas=os.path.join(D, "mrt_atlas.bin")))
    gf = os.path.join(workdir, "od.bin")
    if os.path.exists(gf): os.remove(gf)
    subprocess.run([JTEST, "golden", wg, "--assemble", "--org", "0x4000",
                    "--budget", str(budget),
                    "--capture", f"0x{ODRAW_ADDR:X}:4",
                    "--golden", gf, "--update"],
                   capture_output=True, text=True)
    if not os.path.exists(gf):
        return None, counts
    cap = open(gf, "rb").read()
    if len(cap) < 4:
        return None, counts
    return struct.unpack(">I", cap[:4])[0], counts

def main():
    rooms = None; yaw = 0; budget = 40_000_000
    for a in sys.argv[1:]:
        if a.startswith("--rooms"): rooms = [int(x) for x in a.split("=",1)[1].split(",")]
        if a.startswith("--yaw"):   yaw = int(a.split("=",1)[1])
        if a.startswith("--budget"): budget = int(a.split("=",1)[1])
    mrt = open(os.path.join(D, "mrt.bin"), "rb").read()
    n = struct.unpack(">H", mrt[0:2])[0]
    if rooms is None: rooms = list(range(n))
    SCREEN = 320 * 120        # LOWRES render target
    tot_px = 0; nr = 0
    with tempfile.TemporaryDirectory() as td:
        for r in rooms:
            px, (vc, qc, tc) = overdraw_for(r, yaw, td, budget)
            if px is None:
                print(f"room {r:2d}: DNF", flush=True); continue
            tot_px += px; nr += 1
            print(f"room {r:2d}: faces {qc+tc:4d}  blitted px {px:9,d}"
                  f"  = {px/SCREEN:5.2f}x screen", flush=True)
    if nr:
        print(f"\nmean over {nr} rooms: {tot_px/nr:,.0f} px "
              f"= {tot_px/nr/SCREEN:.2f}x the {SCREEN} px screen")
        print("(>1.0x = redundant pixels the Blitter transfers and then covers)")

if __name__ == "__main__":
    main()
