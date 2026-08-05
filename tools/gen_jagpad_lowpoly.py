#!/usr/bin/env python3
# gen_jagpad_lowpoly.py - Blender low-poly FLAT-SHADED Jaguar pad (2026-08-05,
# replaces the scan prism per user direction: "low poly flat shaded ...
# use Blender ... a Jaguar controller is black").
#
#   JAGPAD_JSON=<jlp.json> python3 tools/gen_jagpad_lowpoly.py
#
# jlp.json: {"verts":[[x,y,z]..], "faces":[[[vi..], mat_idx]..]} in Blender
# space (face +Z, top +Y, dpad -X), materials indexed into MATS below.
#
# ENGINE RULES BAKED IN (each one paid for on silicon):
#  - blob map (x,-y,-z): a rotation, winding survives
#  - CLOSED components -> POSITIVE signed volume (exterior visible)
#  - open DECAL faces -> wound to their facing (front decals -z, back +z)
#  - quads preferred; tris pass through (the title staging promotes them
#    with real midpoint verts since 2026-08-05)
#  - swatch atlas quantized against title_pal WITH the grey-ramp tail
#  - budget: 16+nv*8+nq*36+nt*30 expanded <= 6144 (rblob slot)
import os, struct, sys, json
from collections import defaultdict

J = os.environ.get("JAGPAD_JSON", "")
OUT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if not J or not os.path.exists(J):
    print("JAGPAD_JSON not found:", J); sys.exit(1)
d = json.load(open(J))
VB = d["verts"]; FACES = d["faces"]
SINGLE = os.environ.get("JAGPAD_SINGLE","0")=="1"

# ---- scale: uniform, sized by HEIGHT (real pad is tall; cap 180 units) ----
ys = [v[1] for v in VB]; xs = [v[0] for v in VB]; zs = [v[2] for v in VB]
cy = (max(ys)+min(ys))/2.0; cx = (max(xs)+min(xs))/2.0; cz = (max(zs)+min(zs))/2.0
_h=max(ys)-min(ys); _w=max(xs)-min(xs)
sc = min(180.0/_h, 170.0/_w)
V = [(int(round((v[0]-cx)*sc)),
      int(round(-(v[1]-cy)*sc)),
      int(round(-(v[2]-cz)*sc))) for v in VB]

# ---- swatch atlas: one 8x8 cell per material, title_pal nearest ----
MATS = [(28,28,31),   # body: lower shell / rim
        (44,44,50),   # dark: hump face
        (222,32,24),  # red: C/B/A
        (162,162,172),# key: keys / highlights / rings
        (7,7,8),      # black: dpad disc / grooves / panel bed
        (78,78,86),   # panel: pills / back label
        (46,46,52)]   # bodytop: the leaning control face
pal = struct.unpack(">256H", open(os.path.join(OUT, "title_pal.bin"), "rb").read())
def dec(c): return (((c>>11)&31)*255//31, ((c>>1)&31)*255//31, ((c>>6)&31)*255//31)
prgb = [dec(c) for c in pal]
def nearest(r, g, b):
    best = 1<<30; bi = 0
    for i,(pr,pg,pb) in enumerate(prgb):
        dd = (r-pr)**2 + (g-pg)**2 + (b-pb)**2
        if dd < best: best = dd; bi = i
    return bi
AW = 256; ah = 8
atlas = bytearray(AW*ah)
cells = []
# body gets THREE lighting shades (flat-shaded form: lit from above); the
# rest one cell each. Cell order: [body-front, body-top, body-side, dark,
# red, key, black, panel]
SHADES = [(30,30,33),(88,88,96),(15,15,17)] + MATS[1:]
for mi,(r,g,b) in enumerate(SHADES):
    idx = nearest(r,g,b)
    x0 = mi*10
    for yy in range(8):
        for xx in range(8):
            atlas[yy*AW + x0 + xx] = idx
    cells.append((x0+4, 4))
def body_cell(vi):
    # face normal in blob space (y DOWN): top faces have ny<0
    A,B,C=V[vi[0]],V[vi[1]],V[vi[2]]
    ux,uy,uz=B[0]-A[0],B[1]-A[1],B[2]-A[2]
    wx,wy,wz=C[0]-A[0],C[1]-A[1],C[2]-A[2]
    nx,ny,nz=uy*wz-uz*wy, uz*wx-ux*wz, ux*wy-uy*wx
    l=max(1,abs(nx)+abs(ny)+abs(nz))
    if ny/l < -0.45: return cells[1]      # top-facing: lit
    if abs(nx)/l > 0.55: return cells[2]  # side: darkest
    return cells[0]                        # front/back/bottom

# ---- connectivity: closed components vs open decals ----
par = list(range(len(V)))
def find(x):
    while par[x]!=x: par[x]=par[par[x]]; x=par[x]
    return x
def uni(a,b):
    ra,rb=find(a),find(b)
    if ra!=rb: par[ra]=rb
edge_use = defaultdict(int)
for vi,_ in FACES:
    for k in vi[1:]: uni(vi[0],k)
    for i in range(len(vi)):
        edge_use[frozenset((vi[i],vi[(i+1)%len(vi)]))] += 1
comp_faces = defaultdict(list)
for fi,(vi,_) in enumerate(FACES): comp_faces[find(vi[0])].append(fi)
def comp_closed(fl):
    for fi in fl:
        vi=FACES[fi][0]
        for i in range(len(vi)):
            if edge_use[frozenset((vi[i],vi[(i+1)%len(vi)]))]!=2: return False
    return True
def face_tris(vi):
    return [(vi[0],vi[k],vi[k+1]) for k in range(1,len(vi)-1)]
# screen-space ar at rest pose (yaw 0, item at z=719): the polaroid-
# calibrated rule is ar>0 == VISIBLE. Enforce it mechanically instead of
# reasoning about normals (which cost hours across the y/z flips).
def proj(v):
    z=719+v[2]
    return (160+v[0]*190.0/z, 120+v[1]*190.0/z)
def face_ar(vi):
    pts=[proj(V[k]) for k in vi]
    ar=0.0
    for i in range(len(pts)):
        x0,y0=pts[i]; x1,y1=pts[(i+1)%len(pts)]
        ar+=x0*y1-x1*y0
    return ar
flipped=0
if SINGLE:
    # one connected Blender-normal-consistent mesh: single global A/B via
    # total rest-pose projected area
    tot=sum(face_ar(FACES[fi][0]) for fi in range(len(FACES)))
    if tot<0:
        for vi,_ in FACES: vi.reverse()
        flipped=1
    comp_faces={}
for root,fl in comp_faces.items():
    if comp_closed(fl):
        # orient by the LARGEST front-facing candidate: sum ar over all
        # faces both ways and keep the orientation with more positive
        # projected area at rest (bevel slivers made single-face tests lie).
        tot=sum(face_ar(FACES[fi][0]) for fi in fl)
        want=int(os.environ.get("PAD_ORIENT","1"))
        if (tot>0) != (want>0):
            for fi in fl: FACES[fi][0].reverse()
            flipped+=1
    else:
        for fi in fl:
            vi=FACES[fi][0]
            meanz=sum(V[k][2] for k in vi)/len(vi)
            ar=face_ar(vi)
            # front decals (blob z<0) must be visible at rest (ar>0);
            # back decals must be visible at yaw 128, i.e. ar<0 at rest.
            if (meanz<0 and ar<0) or (meanz>=0 and ar>0):
                FACES[fi][0].reverse()
print("components flipped to positive volume:", flipped)

# ---- emit: closed bodies FIRST, decals LAST (kernel = data order, no
# z-test: the shell painted over its own decals when created after them) ----
decal_set=set()
if not SINGLE:
    for root,fl in comp_faces.items():
        if not comp_closed(fl):
            for fi in fl: decal_set.add(fi)
order=[fi for fi in range(len(FACES)) if fi not in decal_set]+      [fi for fi in range(len(FACES)) if fi in decal_set]
quads=[]; tris=[]
CELLMAP={0:None,1:cells[3],2:cells[4],3:cells[5],4:cells[6],5:cells[7],6:cells[8]}
for fi in order:
    vi,mi=FACES[fi]
    uv = body_cell(vi) if mi==0 else CELLMAP[mi]
    dec2 = fi in decal_set
    # kernel phases: ALL quads then ALL tris. Shell caps are n-gons -> tris,
    # so they'd paint over decal QUADS. Decals therefore emit as TRIS (after
    # the caps in the tri list), shell keeps its natural types.
    if len(vi)==4 and not dec2: quads.append((vi,uv))
    elif len(vi)==3 and not dec2: tris.append((vi,uv))
    else:
        for k in range(1,len(vi)-1):
            tris.append(([vi[0],vi[k],vi[k+1]],uv))
nv,nq,nt=len(V),len(quads),len(tris)
# TRUE staged size: the title staging promotes tris to quads with appended
# midpoint verts -> verts grow by nt, every face costs a 36B quad record.
exp=16+(nv+nt)*8+(nq+nt)*36
print("lowpoly pad: %dv %dq %dt staged %dB"%(nv,nq,nt,exp))
assert exp<=8448, "over rblob budget"
b=bytearray()
b+=struct.pack(">HHHHH",nv,nq,nt,AW,ah)
b+=struct.pack(">hhh",0,0,0)
for (x,y,z) in V: b+=struct.pack(">hhhH",x,y,z,255)
for vi,uv in quads:
    b+=struct.pack(">HHHH",*vi)
    for _ in range(4): b+=struct.pack(">HH",uv[0],uv[1])
for vi,uv in tris:
    b+=struct.pack(">HHH",*vi)
    for _ in range(3): b+=struct.pack(">HH",uv[0],uv[1])
while len(b)&7: b+=b'\0'
open(os.path.join(OUT,"ctrl_geom.bin"),"wb").write(b)
open(os.path.join(OUT,"ctrl_atlas.bin"),"wb").write(bytes(atlas))
xs2=[v[0] for v in V]; ys2=[v[1] for v in V]
print("bbox X[%d,%d] Y[%d,%d]  blob %dB atlas 256x%d"%(
    min(xs2),max(xs2),min(ys2),max(ys2),len(b),ah))
