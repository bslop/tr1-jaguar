#!/usr/bin/env python3
# gen_jagpad.py v2 - SCULPTED Atari Jaguar controller for the title ring.
#
# Replaces the PSX PS1-controller item (ctrl_geom.bin/ctrl_atlas.bin) with a
# real 3D model: body silhouette TRACED from a cutout photo, raised control
# deck (beveled), cable boot, D-pad plate, A/B/C buttons and keypad panel as
# actual relief - and every top face UV-mapped to its own region of the PHOTO
# (quantized to title_pal), the same way the PSX inventory models are
# geometry + photo-derived texture.
#
#   JAGPAD_IMG=/path/to/cutout.png python3 tools/gen_jagpad.py
#
# Blob format (legacy title item, single pose, big-endian):
#   >HHHHH nv,nq,nt,atlasW,atlasH ; >hhh 0,0,0
#   verts >hhhH x,y,z,255 ; quads >HHHH+4uv ; tris >HHH+3uv
# The runtime STAGEDIET copy expands each quad to 36B and tri to 30B and must
# fit rblob[4][3456] (main.c) -> this script enforces that budget.
# Scale matches the old item (X ~ +-78), +Y down, FRONT toward -Z
# (more negative z = closer to the viewer = raised).
import os, struct, sys, math
import numpy as np
from PIL import Image

IMG = os.environ.get("JAGPAD_IMG", "")
BIMG = os.environ.get("JAGPAD_BACK", "")
OUT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if not IMG or not os.path.exists(IMG):
    print("JAGPAD_IMG not found:", IMG); sys.exit(1)

im = Image.open(IMG).convert("RGB")
ROT = int(os.environ.get("JAGPAD_ROT", "0"))
if ROT: im = im.rotate(-ROT, expand=True)      # turn the pad upright
if im.size[0] > 1000:                   # normalize scale so morphology radii
    im = im.resize((900, im.size[1]*900//im.size[0]), Image.LANCZOS)
W, H = im.size
px = np.asarray(im).astype(np.int32)

# ---- 1. silhouette: NOT-background segmentation + morphological cable cut
# background = light uniform backdrop; foreground = pad + cable + connector.
# Erode (kills the thin cable/connector), keep the largest component (the pad
# body), dilate back. No lum-threshold fragmentation, no longest-run hacks.
lum = (px[:, :, 0]*3 + px[:, :, 1]*6 + px[:, :, 2]) // 10
if int(os.environ.get("JAGPAD_GREEN","0")):
    fg = ~((px[:,:,1] > px[:,:,0]+20) & (px[:,:,1] > px[:,:,2]+20))
else:
    fg = lum < int(os.environ.get("JAGPAD_THRESH","115"))  # pad vs backdrop
def erode(m, r):
    for _ in range(r):
        m = m & np.roll(m, 1, 0) & np.roll(m, -1, 0) \
              & np.roll(m, 1, 1) & np.roll(m, -1, 1)
    return m
def dilate(m, r):
    for _ in range(r):
        m = m | np.roll(m, 1, 0) | np.roll(m, -1, 0) \
              | np.roll(m, 1, 1) | np.roll(m, -1, 1)
    return m
fg = dilate(fg, 10); fg = erode(fg, 10)   # CLOSE: heal specular-highlight seams
core = erode(fg.copy(), 32)               # OPEN: amputate cable/connector
# largest connected component (BFS, 4-neigh)
from collections import deque
lab = np.zeros(core.shape, np.int32); nl = 0; sizes = {}
for sy, sx in zip(*np.nonzero(core)):
    if lab[sy, sx]: continue
    nl += 1; q = deque([(sy, sx)]); lab[sy, sx] = nl; cnt = 0
    while q:
        yq, xq = q.popleft(); cnt += 1
        for dy, dx in ((1,0),(-1,0),(0,1),(0,-1)):
            ny, nx = yq+dy, xq+dx
            if 0 <= ny < H and 0 <= nx < W and core[ny, nx] and not lab[ny, nx]:
                lab[ny, nx] = nl; q.append((ny, nx))
    sizes[nl] = cnt
big = max(sizes, key=sizes.get)
mask = dilate(lab == big, 29)
rows = [y for y in range(H) if mask[y].any()]
y0, y1 = min(rows), max(rows)
L = {}; R = {}
for y in range(y0, y1+1):
    xs_ = np.nonzero(mask[y])[0]
    if len(xs_): L[y] = int(xs_[0]); R[y] = int(xs_[-1])
ys = sorted(L)
def med(d, y):
    v = [d[k] for k in range(y-2, y+3) if k in d]; return sorted(v)[len(v)//2]
Ls = {y: med(L, y) for y in ys}; Rs = {y: med(R, y) for y in ys}
NRIM = int(os.environ.get("JAGPAD_RIM","8"))                                   # rim points per side
step = max(1, (y1-y0)//(NRIM-1))
sample = [y for y in range(y0, y1+1, step) if y in Ls][:NRIM]
if sample[-1] != y1: sample[-1] = y1
poly = [(Rs[y], y) for y in sample] + [(Ls[y], y) for y in reversed(sample)]
bx0 = min(p[0] for p in poly); bx1 = max(p[0] for p in poly)
bw = bx1-bx0; bh = y1-y0
cx = (bx0+bx1)/2.0; cy = (y0+y1)/2.0
SCALE = 156.0/bw

# ---- 2. atlas: photo -> 256-wide, nearest title_pal index ----
AW = 256
ah = int(round(H*AW/W))
small = np.asarray(im.resize((AW, ah), Image.LANCZOS)).astype(np.int32)
pal = struct.unpack(">256H", open(os.path.join(OUT, "title_pal.bin"), "rb").read())
pr = np.array([((c >> 11) & 31)*255//31 for c in pal])
pb = np.array([((c >> 6) & 31)*255//31 for c in pal])
pg = np.array([((c >> 1) & 31)*255//31 for c in pal])
flat = small.reshape(-1, 3)
d = ((flat[:, 0:1]-pr)**2 + (flat[:, 1:2]-pg)**2 + (flat[:, 2:3]-pb)**2)
atlas = d.argmin(axis=1).astype(np.uint8).reshape(ah, AW)
ahf = ah
if BIMG and os.path.exists(BIMG):
    bim = Image.open(BIMG).convert("RGB")
    BROT = int(os.environ.get("JAGPAD_BROT", "0"))
    if BROT: bim = bim.rotate(-BROT, expand=True)
    BW, BH = bim.size
    bh_ = int(round(BH*AW/BW))
    bsm = np.asarray(bim.resize((AW, bh_), Image.LANCZOS)).astype(np.int32)
    bf = bsm.reshape(-1, 3)
    bd = ((bf[:, 0:1]-pr)**2 + (bf[:, 1:2]-pg)**2 + (bf[:, 2:3]-pb)**2)
    batl = bd.argmin(axis=1).astype(np.uint8).reshape(bh_, AW)
    atlas = np.vstack([atlas, batl]); ah += bh_
if ah & 1: atlas = np.vstack([atlas, atlas[-1:]]); ah += 1

def fuv(fx, fy):        # pad-bbox fraction -> atlas px
    ix = bx0 + fx*bw; iy = y0 + fy*bh
    return (min(AW-1, max(0, int(ix*AW/W))), min(ahf-1, max(0, int(iy*ahf/H))))
def fxy(fx, fy):        # pad-bbox fraction -> world x,y (+Y down)
    return (int(round((bx0+fx*bw-cx)*SCALE)), int(round((y0+fy*bh-cy)*SCALE)))
DKUV = fuv(0.50, 0.47)  # charcoal texel for sides/back

verts = []; quads = []; tris = []
def V(x, y, z):
    verts.append((x, y, z)); return len(verts)-1
def deckz(fy):          # THE BEVEL: deck surface slopes toward the keypad
    return -14.0 + max(0.0, min(1.0, fy/0.535))*3.0

# ---- 3. CLEAN MODEL (user 2026-08-04): body + ONE beveled deck wedge.
# All control detail comes from the photo texture; no micro-relief plates
# (they rendered as pixel noise at ring size). Front face = two-ring grid
# (rim -> inner ring -> centre) so the photo doesn't warp across slivers.
rim_f = []; rim_b = []; rim_uv = []; inner = []; inner_uv = []
ICX, ICY = cx, (y0+y1)/2.0
for (ix, iy) in poly:
    x = int(round((ix-cx)*SCALE)); y = int(round((iy-cy)*SCALE))
    rim_f.append(V(x, y, -8)); rim_b.append(V(x, y, 10))
    rim_uv.append((min(AW-1, int(ix*AW/W)), min(ahf-1, int(iy*ahf/H))))
    jx = ICX + (ix-ICX)*0.55; jy = ICY + (iy-ICY)*0.55
    inner.append(V(int(round((jx-cx)*SCALE)), int(round((jy-cy)*SCALE)), -8))
    inner_uv.append((min(AW-1, int(jx*AW/W)), min(ahf-1, int(jy*ahf/H))))
cf = V(0, 0, -8); cuv = (min(AW-1, int(ICX*AW/W)), min(ahf-1, int(ICY*ahf/H)))
cbk = V(0, 0, 10)
n = len(rim_f)
def buv(k):
    if not (BIMG and os.path.exists(BIMG)): return DKUV
    u, v = rim_uv[k]
    return (AW-1-u, min(ah-1, ahf + int(v*(ah-ahf-1)/max(1,ahf-1))))
back_tris = []; front_tris = []
for i in range(n):
    j = (i+1) % n
    front_tris.append(((rim_f[i], rim_f[j], inner[i]),
                       (rim_uv[i], rim_uv[j], inner_uv[i])))
    front_tris.append(((inner[i], rim_f[j], inner[j]),
                       (inner_uv[i], rim_uv[j], inner_uv[j])))
    front_tris.append(((inner[i], inner[j], cf),
                       (inner_uv[i], inner_uv[j], cuv)))
    back_tris.append(((rim_b[j], rim_b[i], cbk), (buv(j), buv(i), DKUV)))
    quads.append(((rim_f[i], rim_f[j], rim_b[j], rim_b[i]), (DKUV,)*4))

# THE BEVEL: one raised deck wedge over the control area (D-pad/buttons),
# sloping toward the keypad; photo-textured top, dark side band.
drim = []; duv = []; dbot = []
DECKPTS = [(0.06,0.50),(0.05,0.28),(0.10,0.10),(0.30,0.03),(0.70,0.03),
           (0.90,0.10),(0.95,0.28),(0.94,0.50)]
for (fx, fy) in DECKPTS:
    x, y = fxy(fx, fy)
    drim.append(V(x, y, int(round(deckz(fy))))); duv.append(fuv(fx, fy))
    dbot.append(V(x, y, -8))
dc = V(*fxy(0.5, 0.26), int(round(deckz(0.26)))); dcuv = fuv(0.5, 0.26)
deck_tris = []
for i in range(len(drim)):
    j = (i+1) % len(drim)
    deck_tris.append(((drim[i], drim[j], dc), (duv[i], duv[j], dcuv)))
    quads.append(((dbot[i], dbot[j], drim[j], drim[i]), (DKUV,)*4))

tris = front_tris + deck_tris + tris  # body face, deck, then raised tops
tris = back_tris + tris             # back first (culled from the front)

# ---- 9. emit + budget check ----
nv, nq, nt = len(verts), len(quads), len(tris)
expanded = 16 + nv*8 + nq*36 + nt*30
b = bytearray()
b += struct.pack(">HHHHH", nv, nq, nt, AW, ah)
b += struct.pack(">hhh", 0, 0, 0)
for (x, y, z) in verts: b += struct.pack(">hhhH", x, y, z, 255)
for (vi, uv) in quads:
    b += struct.pack(">HHHH", *vi)
    for (u, v) in uv: b += struct.pack(">HH", u, v)
for (vi, uv) in tris:
    b += struct.pack(">HHH", *vi)
    for (u, v) in uv: b += struct.pack(">HH", u, v)
while len(b) & 7: b += b'\0'
print("jagpad v2: %d verts %dq %dt  blob %dB  STAGEDIET-expanded %dB"
      % (nv, nq, nt, len(b), expanded))
BUDGET = int(os.environ.get("JAGPAD_BUDGET","3456"))
if expanded > BUDGET:
    print("!!! OVER BUDGET - refuse to write"); sys.exit(1)
open(os.path.join(OUT, "ctrl_geom.bin"), "wb").write(b)
open(os.path.join(OUT, "ctrl_atlas.bin"), "wb").write(atlas.tobytes())
xs=[v[0] for v in verts]; ys2=[v[1] for v in verts]; zs=[v[2] for v in verts]
print("bbox X[%d,%d] Y[%d,%d] Z[%d,%d]  atlas 256x%d"
      % (min(xs), max(xs), min(ys2), max(ys2), min(zs), max(zs), ah))
