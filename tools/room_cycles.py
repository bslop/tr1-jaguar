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
Needs:  build/gpu_geotex.bin (make the ship flag set first) and the level set
        <prefix>.bin / <prefix>_geom.bin / <prefix>_atlas.bin beside the parent
        dir.  --prefix=gym prices LARA'S HOME instead of the Caves.
        --rooms=0,1,13  --yaw=N (N in 256ths of a turn: 64 = +X).
"""
import os, struct, subprocess, sys, tempfile

def _disc(p):
    """Resolve a level-set file: disc/ first (2026-08-19 layout), then the old
    tree root, so a tool works either side of the move."""
    import os as _o
    d = _o.path.join(_o.path.dirname(p), "disc", _o.path.basename(p))
    return d if _o.path.exists(d) else p

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# ☠️ THE COBWEB CHECKOUT MOVED and this path was never updated, so the tool had
# been dead with a FileNotFoundError - which is why no one noticed it was also
# Caves-only. Resolve it, and let JTEST= override.
JTEST = os.environ.get("JTEST") or next(
    (q for q in (
        os.path.expanduser("~/Documents/Git/jag_openlara/cobweb/sim/target/release/jtest"),
        os.path.expanduser("~/Documents/Git/cobweb/sim/target/release/jtest"),
    ) if os.path.exists(q)),
    os.path.expanduser("~/Documents/Git/jag_openlara/cobweb/sim/target/release/jtest"))
KERN = os.path.join(D, "build/gpu_geotex.bin")
MAGIC = 0x0A3DD05E

# fixed DRAM addresses (free in the sim)
MAILBOX = 0x100000
VTXCACHE = 0x190000
FB = 0x1A0000

# ☠️ THIS TOOL WAS CAVES-ONLY PURELY BECAUSE THE PATHS WERE HARDCODED.
# Same trap mrt_boundary_audit.py had: nothing about the profiler is
# level-specific, but "mrt.bin"/"mrt_geom.bin"/"mrt_atlas.bin" were baked in, so
# the mansion could not be priced at all. --prefix gym switches the set.
PREFIX = "mrt"

def _p(name):
    return _disc(os.path.join(D, PREFIX + name))

def sintab(a):
    import math
    return int(round(math.sin((a & 255) * 2 * math.pi / 256) * 65536))

def build_room_inputs(idx_room):
    mrt = open(_p(".bin"), "rb").read()
    geom = open(_p("_geom.bin"), "rb").read()
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
        atlas=_p("_atlas.bin")))
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

def main():
    rooms = None; yaw = 0
    for a in sys.argv[1:]:
        if a.startswith("--rooms"): rooms = [int(x) for x in a.split("=",1)[1].split(",")]
        if a.startswith("--yaw"): yaw = int(a.split("=",1)[1])
        if a.startswith("--prefix"):
            global PREFIX
            PREFIX = a.split("=",1)[1]
    mrt = open(_p(".bin"), "rb").read()
    n = struct.unpack(">H", mrt[0:2])[0]
    if rooms is None: rooms = list(range(n))
    with tempfile.TemporaryDirectory() as td:
        rows = []
        for r in rooms:
            cyc, (vc, qc, tc) = cycles_for(r, yaw, td)
            rows.append((cyc or 0, r, vc, qc, tc))
            print(f"room {r:2d}: verts {vc:4d} faces {qc+tc:4d} "
                  f"cycles {cyc if cyc else 'DNF'}", flush=True)
        rows.sort(reverse=True)
        print("\n=== heaviest rooms (kernel cycles, yaw %d) ===" % yaw)
        for cyc, r, vc, qc, tc in rows[:12]:
            print(f"  room {r:2d}: {cyc:>10,} cycles  ({qc+tc} faces)")

if __name__ == "__main__":
    main()
