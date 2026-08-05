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
verts_b = d["verts"]
# TEXTURE MODE (user's scanned model, 2026-08-04): "tfaces" = [[vi...],[uv...]]
# with real per-loop UVs + JAGPAD_TEX = the model's diffuse texture. The mesh
# is a closed scanned manifold, so visibility is backface-cull alone (the
# PS1 pad blob proof) - every face ships, NO z-sort, NO phase games.
TEXMODE = "tfaces" in d
if not TEXMODE:
    faces, mats = d["faces"], d["mats"]

# ---- scale to the ring-item envelope (X +-78), +Y down, front -Z ----
xs = [v[0] for v in verts_b]
sc = 156.0 / (max(xs) - min(xs))
cx = (max(xs)+min(xs))/2.0
ys = [v[1] for v in verts_b]; cy = (max(ys)+min(ys))/2.0
zs = [v[2] for v in verts_b]; cz = (max(zs)+min(zs))/2.0
# Standing, face toward the camera (-Z): the convention the photo-traced pad
# proved on the ring. Blender +Y(top)->-Y(up), +Z(face)->-Z(front).
FLAT = float(os.environ.get("JAGPAD_FLAT", "1.0"))  # bas-relief: painter
# order stays valid at every ring yaw (a deep stack inverts when spun)
verts = [(int(round((v[0]-cx)*sc)),
          int(round(-(v[1]-cy)*sc)),
          int(round(-(v[2]-cz)*sc*FLAT))) for v in verts_b]

# ---- atlas: one 8x8 swatch cell per material, title_pal-nearest ----
AW = 256
pal = struct.unpack(">256H", open(os.path.join(OUT, "title_pal.bin"), "rb").read())
def dec(c): return (((c>>11)&31)*255//31, ((c>>1)&31)*255//31, ((c>>6)&31)*255//31)
prgb = [dec(c) for c in pal]
GAIN = float(os.environ.get("JAGPAD_GAIN", "1.6"))   # title_pal is dark; bias up
def nearest(r, g, b):
    r = min(255, int(r*GAIN)); g = min(255, int(g*GAIN)); b = min(255, int(b*GAIN))
    best = 1<<30; bi = 0
    for i,(pr,pg,pb) in enumerate(prgb):
        dd = (r-pr)**2 + (g-pg)**2 + (b-pb)**2
        if dd < best: best = dd; bi = i
    return bi

if TEXMODE:
    from PIL import Image
    T = os.environ.get("JAGPAD_TEX", "")
    if not T or not os.path.exists(T):
        print("JAGPAD_TEX not found:", T); sys.exit(1)
    FLATSHADE = os.environ.get("JAGPAD_FLATSHADE", "0") == "1"
    if FLATSHADE:
        # FLAT-SHADE (user 2026-08-04): one matched colour per face - matte
        # black body, red fire buttons, grey keys. Sample the diffuse at each
        # face's UV corners+centroid, average, quantize to title_pal (the
        # reserved grey-ramp tail keeps the darks NEUTRAL, not gold). Atlas =
        # one 8x8 swatch per unique palette index; face UVs hit cell centres.
        big = Image.open(T).convert("RGB")
        bw, bh = big.size
        bpx = big.load()
        def face_colour(uvs):
            # dense barycentric grid over the UV polygon, MEDIAN luma pick:
            # a mean lets keypad highlights drag the matte-black shell grey.
            pts = []
            a = uvs[0]
            for k in range(1, len(uvs)-1):
                b_, c_ = uvs[k], uvs[k+1]
                N = 6
                for i in range(N+1):
                    for j in range(N+1-i):
                        w0 = i/N; w1 = j/N; w2 = 1.0-w0-w1
                        u = a[0]*w2 + b_[0]*w0 + c_[0]*w1
                        v = a[1]*w2 + b_[1]*w0 + c_[1]*w1
                        x = min(bw-1, max(0, int(u*(bw-1))))
                        y = min(bh-1, max(0, int((1.0-v)*(bh-1))))
                        pts.append(bpx[x, y])
            pts.sort(key=lambda c: c[0]*3 + c[1]*6 + c[2])
            r, g, b = pts[len(pts)//2]
            return nearest(r, g, b)
        swatch = {}                       # palette idx -> cell centre (u,v)
        def cell_for(idx):
            if idx not in swatch:
                k = len(swatch)
                swatch[idx] = (k % 32 * 8 + 4, k // 32 * 8 + 4)
            return swatch[idx]
    else:
        # PHOTO-TEXTURE: downscale the diffuse to 256x256, quantize to
        # title_pal; blob v is TOP-ORIGIN so Blender UV v flips at lookup.
        ah = 256
        img = Image.open(T).convert("RGB").resize((AW, ah), Image.LANCZOS)
        px = img.load()
        try:
            import numpy as np
            arr = np.asarray(img, dtype=np.int32)
            arr = np.minimum(255, (arr * GAIN).astype(np.int32))
            p = np.array(prgb, dtype=np.int32)                    # 256x3
            flat = arr.reshape(-1, 3)
            # nearest palette index per pixel (chunked to bound memory)
            atlas = bytearray(AW*ah)
            CH = 8192
            for o in range(0, flat.shape[0], CH):
                c = flat[o:o+CH]
                dd = ((c[:, None, :] - p[None, :, :])**2).sum(2)
                idx = dd.argmin(1)
                atlas[o:o+len(idx)] = bytes(int(i) for i in idx)
        except ImportError:
            atlas = bytearray(AW*ah)
            for yy in range(ah):
                for xx in range(AW):
                    atlas[yy*AW+xx] = nearest(*px[xx, yy])
else:
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

if TEXMODE:
    # closed manifold: ship every face with its real UVs, winding preserved
    # (blob mapping (x,-y,-z) is a 180-deg rotation, not a reflection).
    quads = []; tris = []
    def uvpx(uv):
        u = max(0.0, min(1.0, uv[0])); v = max(0.0, min(1.0, uv[1]))
        return (int(u*255), int((1.0-v)*255))        # V flip: blob v is top-origin
    for vi, uvs in d["tfaces"]:
        if FLATSHADE:
            c = cell_for(face_colour(uvs))
            pts = [c]*len(vi)
        else:
            pts = [uvpx(u) for u in uvs]
        if len(vi) == 4:
            quads.append((vi, pts))
        elif len(vi) == 3:
            tris.append((vi, pts))
        else:                                        # ngon: fan
            for k in range(1, len(vi)-1):
                tris.append(([vi[0], vi[k], vi[k+1]],
                             [pts[0], pts[k], pts[k+1]]))
    if FLATSHADE:
        # build the swatch atlas now that every face has claimed a cell
        rows = (len(swatch) + 31) // 32
        ah = max(8, rows * 8)
        atlas = bytearray(AW*ah)
        for idx, (cx2, cy2) in swatch.items():
            for yy in range(cy2-4, cy2+4):
                for xx in range(cx2-4, cx2+4):
                    atlas[yy*AW+xx] = idx
        print("flatshade: %d unique colours" % len(swatch))
    nv, nq, nt = len(verts), len(quads), len(tris)
    expanded = 16 + nv*8 + nq*36 + nt*30
    BUDGET = int(os.environ.get("JAGPAD_BUDGET", "10560"))
    print("tex pad: %dv %dq %dt  expanded %dB (budget %d)" % (nv, nq, nt, expanded, BUDGET))
    if expanded > BUDGET:
        print("!!! OVER BUDGET"); sys.exit(1)
    b = bytearray()
    b += struct.pack(">HHHHH", nv, nq, nt, AW, ah)
    b += struct.pack(">hhh", 0, 0, 0)
    for (x, y, z) in verts: b += struct.pack(">hhhH", x, y, z, 255)
    for (vi, pts) in quads:
        b += struct.pack(">HHHH", *vi)
        for (u, v) in pts: b += struct.pack(">HH", u, v)
    for (vi, pts) in tris:
        b += struct.pack(">HHH", *vi)
        for (u, v) in pts: b += struct.pack(">HH", u, v)
    while len(b) & 7: b += b'\0'
    open(os.path.join(OUT, "ctrl_geom.bin"), "wb").write(b)
    open(os.path.join(OUT, "ctrl_atlas.bin"), "wb").write(bytes(atlas))
    xs2=[v[0] for v in verts]; ys2=[v[1] for v in verts]; zs2=[v[2] for v in verts]
    print("bbox X[%d,%d] Y[%d,%d] Z[%d,%d]  blob %dB atlas 256x%d"
          % (min(xs2),max(xs2),min(ys2),max(ys2),min(zs2),max(zs2),len(b),ah))
    sys.exit(0)

# ---- faces: PHASE-AWARE (kernel draws ALL quads then ALL tris, no z-sort).
# Detail plates sit ABOVE the body, so anything that must paint over another
# surface goes in the TRI phase, in authoring order (body first, details last).
# Only depth-safe SIDE WALLS (verts spanning front/back in z) stay quads. ----
quads = []; tris = []
def is_sidewall(vi):
    zs2 = [verts[k][2] for k in vi]
    return max(zs2) - min(zs2) >= 1       # any z-span = a wall (tops are flat)
for vi, mi in faces:
    uv = cells[mi]
    zs2 = [verts[k][2] for k in vi]
    span = max(zs2) - min(zs2)
    if span >= 3:
        # silhouette wall (body/back/deck) - KEEP or the model is see-through
        if len(vi) == 4: quads.append((vi, uv)); continue
    elif span >= 1:
        continue          # 1-2px detail wall: invisible, not worth bytes
    if len(vi) == 3:
        tris.append((vi, uv))
    else:                                  # quad top / ngon: fan to tris
        for k in range(1, len(vi)-1):
            tris.append(([vi[0], vi[k], vi[k+1]], uv))

# static painter: farther (larger z) first - authoring order was scrambled
# by Blender's join_triangles, and the kernel has no depth sort.
tris.sort(key=lambda f: -sum(verts[k][2] for k in f[0])/len(f[0]))
quads.sort(key=lambda f: -sum(verts[k][2] for k in f[0])/len(f[0]))
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
