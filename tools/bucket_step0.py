#!/usr/bin/env python3
# BUCKET campaign STEP 0: cluster-skippability simulation from real geometry
# (bins_planes mrt.bin/mrt_geom.bin) + real per-frame dispatch/camera captures
# (cap_spawn.jsonl / cap_r12.jsonl from jagemu serve peeks).
#
# Replays EXACTLY (integer math) what gpu_geotex.gas does per admitted room:
#   vc pre-pass transform (NEAR=64, FARD per-vert cull -> sentinel),
#   per-face STAGEDIET plane cull, stage + BEHINDF skip, screen backface cull,
#   y-clip-empty skip, else drawn.
# Then: cluster faces per room (k-means on centroids, quads+tris jointly,
# per-phase segments), apply a CONSERVATIVE cluster bbox test (6 half-space
# planes through the camera derived from the room's clip rect + near/far),
# and account net cycles: saved(face classes) - testcost * segments_walked.
import json, struct, sys, math
from collections import defaultdict

NEAR = 64
FOCAL = 190; FOCAL_Y = 190
CX = 160; CY = 120
RENDER_H = 240

# ---- cost model (GPU cycles, instruction-counted from gpu_geotex.gas;
#      BASE charges DRAM loads ~4-5cyc "free-bus"; CONT adds +2.7/DRAM access
#      per the 2026-07-20 silicon contention calib) ----
COST = {
  'A':  (75,   3),    # STAGEDIET plane-culled: preamble+plane test+face_adv (3 DRAM)
  'Bq': (380, 17),    # staged quad, BEHINDF skip (16 DRAM stage + 1 shadek)
  'Bt': (315, 13),
  'Cq': (435, 17),    # staged quad, screen backface cull
  'Ct': (370, 13),
  'Dq': (535, 17),    # staged quad, y-clip empty after scan
  'Dt': (450, 13),
  'Wq': (595, 17),    # staged quad, walks rows but every span x-clips empty
  'Wt': (510, 13),    # (+ WALK_ROW cyc per walked row, added separately)
}
WALK_ROW = 80.0       # per-row cost of a walked-but-empty scanline (uv_edge
                      # chain step + 2 hidden divs + clamps + sp_skip; real
                      # span setup measured ~150-200, empty ~120-150; charge 80
                      # = conservative low)
def cost_of(cls, contended, rows=0):
    c, dram = COST[cls]
    return c + (2.7 * dram if contended else 0.0) + WALK_ROW * rows

def sar(v, n):  # arithmetic shift right (python ints already arithmetic)
    return v >> n

def s16(v): return v - 0x10000 if v >= 0x8000 else v
def s32(v): return v - 0x100000000 if v >= 0x80000000 else v
def low16s(v): return s16(v & 0xFFFF)

def idiv_trunc(a, b):  # kernel abs/neg dance: trunc toward zero, b>0
    q = abs(a) // b
    return -q if a < 0 else q

# ---------------- level parse ----------------
def parse_level(mrt="mrt.bin", geom="mrt_geom.bin", face_planes=True):
    idx = open(mrt, "rb").read()
    n, aw, ah, _ = struct.unpack(">HHHH", idx[:8])
    offs = [struct.unpack(">II", idx[8+i*8:16+i*8])[0] for i in range(n)]
    g = open(geom, "rb").read()
    rooms = []
    for ri in range(n):
        o = offs[ri]
        vc, qc, tc, _aw, _ah, ox, oy, oz = struct.unpack(">HHHHHhhh", g[o:o+16])
        p = o + 16
        verts = []
        for i in range(vc):
            x, y, z, s = struct.unpack(">hhhH", g[p:p+8]); p += 8
            verts.append((x, y, z))
        faces = []  # (phase 'q'/'t', plane(nx,ny,nz,d), idxs, order)
        for fi in range(qc):
            pl = None
            if face_planes:
                l0, l1, d = struct.unpack(">IIi", g[p:p+12]); p += 12
                pl = (low16s(l0 >> 16), low16s(l0), low16s(l1), d)  # ny,nx? NO:
                # long0=(ny<<16)|nx -> high=ny low=nx ; kernel: r4=long0 (imult uses low16=nx)
                pl = (low16s(l0), low16s(l0 >> 16), low16s(l1), d)  # (nx,ny,nz,d)
            vs = struct.unpack(">HHHH", g[p:p+8]); p += 8
            p += 16  # uv 4*(2+2)
            faces.append(['q', pl, vs, fi])
        for fi in range(tc):
            pl = None
            if face_planes:
                l0, l1, d = struct.unpack(">IIi", g[p:p+12]); p += 12
                pl = (low16s(l0), low16s(l0 >> 16), low16s(l1), d)
            vs = struct.unpack(">HHH", g[p:p+6]); p += 6
            p += 12
            faces.append(['t', pl, vs, qc + fi])
        rooms.append(dict(i=ri, off=o, vc=vc, qc=qc, tc=tc,
                          offX=ox, offZ=oz, verts=verts, faces=faces))
    return rooms

# ---------------- per-frame capture parse ----------------
def parse_caps(path, geombase, larablob):
    frames = []
    for ln in open(path):
        d = json.loads(ln)
        db = d['displist']['bytes']
        cb = d['camblk']['bytes']
        def be32(b, i): return s32((b[i]<<24)|(b[i+1]<<16)|(b[i+2]<<8)|b[i+3])
        cnt = be32(db, 0)
        ents = []
        for i in range(cnt):
            ptr = be32(db, 4+i*16) & 0xFFFFFFFF
            cx = be32(db, 4+i*16+4) & 0xFFFFFFFF
            cy = be32(db, 4+i*16+8) & 0xFFFFFFFF
            if ptr == larablob:  # Lara rides the dispatch; not clusterable
                continue
            ents.append(dict(off=ptr-geombase,
                             cx0=cx>>16, cx1=cx & 0xFFFF, cy0=cy>>16, cy1=cy & 0xFFFF))
        cam = [be32(cb, i*4) for i in range(7)]
        frames.append(dict(ents=ents, cam=cam))
    return frames

# ---------------- kernel replay ----------------
def transform_room(room, cam, fard):
    cY4, sY4, cP4, sP4, camx, camy, camz = cam
    oxw = room['offX'] << 8; ozw = room['offZ'] << 8
    out = []  # (sx,sy,behind, rz2 raw view z or None)
    for (x, y, z) in room['verts']:
        dx = (x + oxw) - camx; dy = y - camy; dz = (z + ozw) - camz
        rx = sar(dx*cY4 - dz*sY4, 12)
        rz = sar(dx*sY4 + dz*cY4, 12)
        ry = sar(dy*cP4 - rz*sP4, 12)
        rz2 = sar(dy*sP4 + rz*cP4, 12)
        if rz2 < NEAR or (fard is not None and rz2 >= fard):
            out.append((None, None, True))
            continue
        sx = CX + idiv_trunc(rx*FOCAL, rz2)
        sy = CY + idiv_trunc(ry*FOCAL_Y, rz2)
        out.append((sx, sy, False))
    return out

def classify_faces(room, cam, fard, ent):
    """returns list of (cls, phase) per face in walk order + drawn count"""
    cY4, sY4, cP4, sP4, camx, camy, camz = cam
    oxw = room['offX'] << 8; ozw = room['offZ'] << 8
    cx = camx - oxw; cy = camy; cz = camz - ozw
    keep_all = max(abs(cx), abs(cy), abs(cz)) >= 32768
    tv = transform_room(room, cam, fard)
    res = []
    for ph, pl, vs, order in room['faces']:
        if not keep_all and pl is not None:
            nx, ny, nz, d = pl
            ndc = nx*low16s(cx & 0xFFFF) if False else nx*cx + ny*cy + nz*cz
            # kernel imult uses low16 of cx.. but |c|<32768 here so identical
            if ndc < d:
                res.append(('A', ph)); continue
        pts = [tv[i] for i in vs]
        if any(p[2] for p in pts):
            res.append(('B'+ph, ph)); continue
        (sx0, sy0, _), (sx1, sy1, _), (sx2, sy2, _) = pts[0], pts[1], pts[2]
        ar = (sx1-sx0)*(sy2-sy0) - (sx2-sx0)*(sy1-sy0)
        if ar <= 0:
            res.append(('C'+ph, ph)); continue
        ymin = min(p[1] for p in pts); ymax = max(p[1] for p in pts)
        y0 = max(ymin, ent['cy0']); y1 = min(ymax-1, ent['cy1'])
        if y1 < y0:
            res.append(('D'+ph, ph)); continue
        sxs = [p[0] for p in pts]
        if max(sxs) <= ent['cx0']-2 or min(sxs) >= ent['cx1']+2:
            # walks y0..y1 but every span clamps empty (margin 2 for DDA jitter)
            res.append(('W'+ph, ph, y1-y0+1)); continue
        res.append(('E', ph))
    return res

# ---------------- clustering ----------------
def kmeans_clusters(room, N, seed=1234):
    faces = room['faces']
    if len(faces) <= N:
        return [[i] for i in range(len(faces))]
    cents = []
    for ph, pl, vs, order in faces:
        xs = [room['verts'][i] for i in vs]
        cents.append(tuple(sum(c[j] for c in xs)/len(xs) for j in range(3)))
    # deterministic k-means++ -ish init: spread seeds by picking extremes
    rng = seed
    def rnd():
        nonlocal rng
        rng = (rng*1103515245 + 12345) & 0x7FFFFFFF
        return rng
    centers = [cents[rnd() % len(cents)]]
    while len(centers) < N:
        best, bi = -1, 0
        for i, c in enumerate(cents):
            d = min((c[0]-k[0])**2 + (c[1]-k[1])**2 + (c[2]-k[2])**2 for k in centers)
            if d > best: best, bi = d, i
        centers.append(cents[bi])
    assign = [0]*len(cents)
    for _ in range(12):
        changed = False
        for i, c in enumerate(cents):
            b, bd = 0, None
            for k, K in enumerate(centers):
                d = (c[0]-K[0])**2 + (c[1]-K[1])**2 + (c[2]-K[2])**2
                if bd is None or d < bd: bd, b = d, k
            if assign[i] != b: assign[i] = b; changed = True
        for k in range(len(centers)):
            m = [cents[i] for i in range(len(cents)) if assign[i] == k]
            if m:
                centers[k] = tuple(sum(c[j] for c in m)/len(m) for j in range(3))
        if not changed: break
    clus = defaultdict(list)
    for i, a in enumerate(assign): clus[a].append(i)
    return [v for v in clus.values() if v]

def cluster_bbox(room, members):
    mn = [ 1<<30]*3; mx = [-(1<<30)]*3
    for fi in members:
        for vi in room['faces'][fi][2]:
            v = room['verts'][vi]
            for j in range(3):
                if v[j] < mn[j]: mn[j] = v[j]
                if v[j] > mx[j]: mx[j] = v[j]
    return mn, mx

def cluster_skippable(room, mn, mx, cam, fard, ent):
    """conservative: all 8 bbox corners outside ONE of 6 planes.
    Exact integer forms matching kernel-implementable linear tests."""
    cY4, sY4, cP4, sP4, camx, camy, camz = cam
    oxw = room['offX'] << 8; ozw = room['offZ'] << 8
    corners = []
    for x in (mn[0], mx[0]):
        for y in (mn[1], mx[1]):
            for z in (mn[2], mx[2]):
                dx = (x + oxw) - camx; dy = y - camy; dz = (z + ozw) - camz
                rx = sar(dx*cY4 - dz*sY4, 12)
                rz = sar(dx*sY4 + dz*cY4, 12)
                ry = sar(dy*cP4 - rz*sP4, 12)
                rz2 = sar(dy*sP4 + rz*cP4, 12)
                corners.append((rx, ry, rz2))
    if all(c[2] < NEAR-2 for c in corners):  return 'near'
    if fard is not None and all(c[2] >= fard+2 for c in corners): return 'far'
    kL = ent['cx0'] - 2 - CX; kR = ent['cx1'] + 2 - CX
    kT = ent['cy0'] - 2 - CY; kB = ent['cy1'] + 2 - CY
    if all(c[0]*FOCAL   - kL*c[2] <= 0 for c in corners): return 'left'
    if all(c[0]*FOCAL   - kR*c[2] >= 0 for c in corners): return 'right'
    if all(c[1]*FOCAL_Y - kT*c[2] <= 0 for c in corners): return 'top'
    if all(c[1]*FOCAL_Y - kB*c[2] >= 0 for c in corners): return 'bottom'
    return None

# ---------------- main sweep ----------------
def run(scene, capfile, geombase, larablob, fard, Ns=(4,6,8), testcost=60.0,
        contended=False, quiet=False):
    rooms = parse_level()
    frames = parse_caps(capfile, geombase, larablob)
    # precompute clusters per room per N
    clus = {}
    for N in Ns:
        for r in rooms:
            key = (r['i'], N)
            cl = kmeans_clusters(r, N)
            clus[key] = [(members, cluster_bbox(r, members)) for members in cl]
    stats = {N: defaultdict(float) for N in Ns}
    tot = defaultdict(float)
    for fr in frames:
        cam = fr['cam']
        for ent in fr['ents']:
            room = next((r for r in rooms if r['off'] == ent['off']), None)
            if room is None:
                tot['unmatched'] += 1; continue
            cls = classify_faces(room, cam, fard, ent)
            tot['frames_ents'] += 1
            for t in cls:
                c = t[0]; rows = t[2] if len(t) > 2 else 0
                tot['n_'+c] += 1
                if c != 'E': tot['cyc_skippable_pool'] += cost_of(c, contended, rows)
            tot['faces'] += len(cls)
            tot['drawn'] += sum(1 for t in cls if t[0] == 'E')
            for N in Ns:
                st = stats[N]
                segs = 0; saved = 0.0; fskip = 0; violations = 0
                for members, (mn, mx) in clus[(room['i'], N)]:
                    phases = set(room['faces'][fi][0] for fi in members)
                    nseg = len(phases)  # quad-phase seg + tri-phase seg
                    segs += nseg
                    skip = cluster_skippable(room, mn, mx, cam, fard, ent)
                    if skip:
                        for fi in members:
                            t = cls[fi]; c = t[0]; rows = t[2] if len(t) > 2 else 0
                            if c == 'E':
                                violations += 1  # MUST be 0 (conservative)
                            else:
                                saved += cost_of(c, contended, rows)
                            fskip += 1
                st['segs'] += segs
                st['saved'] += saved
                st['fskip'] += fskip
                st['violations'] += violations
    nfr = len(frames)
    out = dict(scene=scene, frames=nfr, fard=fard, contended=contended,
               testcost=testcost)
    out['faces_per_frame'] = tot['faces']/nfr
    out['drawn_per_frame'] = tot['drawn']/nfr
    out['class_per_frame'] = {k[2:]: tot[k]/nfr for k in sorted(tot) if k.startswith('n_')}
    out['skippable_pool_cyc_per_frame'] = tot['cyc_skippable_pool']/nfr
    out['sweep'] = {}
    for N in Ns:
        st = stats[N]
        net = (st['saved'] - testcost*st['segs'])/nfr
        out['sweep'][N] = dict(
            segs_per_frame=st['segs']/nfr,
            faces_skipped_per_frame=st['fskip']/nfr,
            pct_faces_skipped=100.0*st['fskip']/max(tot['faces'],1),
            saved_cyc_per_frame=st['saved']/nfr,
            test_cyc_per_frame=testcost*st['segs']/nfr,
            net_cyc_per_frame=net,
            violations=st['violations'])
    return out

if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", required=True)
    ap.add_argument("--cap", required=True)
    ap.add_argument("--geombase", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--larablob", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--fard", type=int, default=9000)
    ap.add_argument("--testcost", type=float, default=60.0)
    ap.add_argument("--contended", action="store_true")
    args = ap.parse_args()
    r = run(args.scene, args.cap, args.geombase, args.larablob, args.fard,
            testcost=args.testcost, contended=args.contended)
    print(json.dumps(r, indent=1))
