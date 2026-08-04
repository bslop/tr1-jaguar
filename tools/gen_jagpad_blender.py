#!/usr/bin/env python3
# gen_jagpad_blender.py - convert a Blender-authored Jaguar pad (JSON dump of
# verts/faces/materials) into the Controls ring item (ctrl_geom.bin +
# ctrl_atlas.bin). Flat per-material colours as swatch cells in a tiny atlas,
# quantized to title_pal (same contract as tr2jag_title.py / gen_jagpad.py).
#
#   JAGPAD_JSON=/path/bpad.json python3 tools/gen_jagpad_blender.py
#
# Blob (legacy title item, single pose, big-endian):
#   >HHHHH nv,nq,nt,atlasW,atlasH ; >hhh 0,0,0
#   verts >hhhH x,y,z,255 ; quads >HHHH+4uv ; tris >HHH+3uv
# Budget: STAGEDIET-expanded 16+nv*8+nq*36+nt*30 <= rblob (9216, main.c).
# Axes: Blender +Y(top)/+Z(front) -> blob -Y(top is negative)/-Z(front).
import os, struct, sys, json

J = os.environ.get("JAGPAD_JSON", "")
OUT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if not J or not os.path.exists(J):
    print("JAGPAD_JSON not found:", J); sys.exit(1)
d = json.load(open(J))
verts_b, faces, mats = d["verts"], d["faces"], d["mats"]

# ---- scale to the ring-item envelope (X +-78), +Y down, front -Z ----
xs = [v[0] for v in verts_b]
sc = 156.0 / (max(xs) - min(xs))
cx = (max(xs)+min(xs))/2.0
ys = [v[1] for v in verts_b]; cy = (max(ys)+min(ys))/2.0
zs = [v[2] for v in verts_b]; cz = (max(zs)+min(zs))/2.0
verts = [(int(round((v[0]-cx)*sc)),
          int(round(-(v[1]-cy)*sc)),
          int(round(-(v[2]-cz)*sc))) for v in verts_b]

# ---- atlas: one 8x8 swatch cell per material, title_pal-nearest ----
AW = 256
pal = struct.unpack(">256H", open(os.path.join(OUT, "title_pal.bin"), "rb").read())
def dec(c): return (((c>>11)&31)*255//31, ((c>>1)&31)*255//31, ((c>>6)&31)*255//31)
prgb = [dec(c) for c in pal]
def nearest(r, g, b):
    best = 1<<30; bi = 0
    for i,(pr,pg,pb) in enumerate(prgb):
        dd = (r-pr)**2 + (g-pg)**2 + (b-pb)**2
        if dd < best: best = dd; bi = i
    return bi
ah = 8
atlas = bytearray(AW*ah)
cells = []
for mi, m in enumerate(mats):
    idx = nearest(int(m[0]*255), int(m[1]*255), int(m[2]*255))
    x0 = mi*10
    for yy in range(8):
        for xx in range(8):
            atlas[yy*AW + x0 + xx] = idx
    cells.append((x0+4, 4))            # cell centre texel

# ---- faces ----
quads = []; tris = []
for vi, mi in faces:
    uv = cells[mi]
    if len(vi) == 4: quads.append((vi, uv))
    elif len(vi) == 3: tris.append((vi, uv))
    else:                               # ngon: fan
        for k in range(1, len(vi)-1):
            tris.append(([vi[0], vi[k], vi[k+1]], uv))

nv, nq, nt = len(verts), len(quads), len(tris)
expanded = 16 + nv*8 + nq*36 + nt*30
BUDGET = int(os.environ.get("JAGPAD_BUDGET", "9216"))
print("blender pad: %dv %dq %dt  expanded %dB (budget %d)" % (nv, nq, nt, expanded, BUDGET))
if expanded > BUDGET:
    print("!!! OVER BUDGET"); sys.exit(1)

b = bytearray()
b += struct.pack(">HHHHH", nv, nq, nt, AW, ah)
b += struct.pack(">hhh", 0, 0, 0)
for (x, y, z) in verts: b += struct.pack(">hhhH", x, y, z, 255)
for (vi, uv) in quads:
    b += struct.pack(">HHHH", *vi)
    for _ in range(4): b += struct.pack(">HH", uv[0], uv[1])
for (vi, uv) in tris:
    b += struct.pack(">HHH", *vi)
    for _ in range(3): b += struct.pack(">HH", uv[0], uv[1])
while len(b) & 7: b += b'\0'
open(os.path.join(OUT, "ctrl_geom.bin"), "wb").write(b)
open(os.path.join(OUT, "ctrl_atlas.bin"), "wb").write(bytes(atlas))
xs2=[v[0] for v in verts]; ys2=[v[1] for v in verts]; zs2=[v[2] for v in verts]
print("bbox X[%d,%d] Y[%d,%d] Z[%d,%d]  blob %dB atlas 256x%d"
      % (min(xs2),max(xs2),min(ys2),max(ys2),min(zs2),max(zs2),len(b),ah))
