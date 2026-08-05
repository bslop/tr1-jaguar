#!/usr/bin/env python3
# gen_jagpad_prism.py - the SHIPPING Jaguar-pad ring item (2026-08-04).
#
# Structure = the proven PS1 ctrl blob: a silhouette PRISM (convex-hull
# outline x ~17 pts, extruded, ~64 large tris) whose front/back plates are
# BAKED orthographic renders of the user's full-resolution scan. Large faces
# survive the kernel's integer projection at ring distance (z=700); a closed
# prism + negative signed volume satisfies backface-cull-only visibility.
#
#   JAGPAD_OBJ=<base.obj> JAGPAD_TEX=<texture_diffuse.png> #     python3 tools/gen_jagpad_prism.py
#
# env: PAD_GAIN (default 1.0) plate brightness bias into title_pal
# outputs: ctrl_geom.bin (legacy single-pose blob) + ctrl_atlas.bin
# Requires title_pal.bin (gen_titlebg.py FIRST - grey ramp tail).
#
# ☠️ traps this file already paid for:
#  - barycentric weights are w0*c0+w1*c1+w2*c2 (a rotation streaks UVs)
#  - Douglas-Peucker on a CLOSED ring needs splitting at the x-extremes
#  - supersample the bake 4x then BOX down (2048^2 point-sampled = moire)
#  - the scan OBJ is already face +Z / top +Y; blob map (x,-y,-z) is a
#    rotation; the prism is then rotated 180 about Y so the FACE shows at
#    ring yaw 0

import json, os, struct, math
import numpy as np
from PIL import Image
OUT=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---- 0. parse the OBJ (quads fanned, v/vt) ----
Vs=[]; VTs=[]; Ftmp=[]
for line in open(os.environ["JAGPAD_OBJ"]):
    if line.startswith("v "):
        _,x,y,z=line.split()[:4]; Vs.append((float(x),float(y),float(z)))
    elif line.startswith("vt "):
        p=line.split(); VTs.append((float(p[1]),float(p[2])))
    elif line.startswith("f "):
        c=[]
        for tok in line.split()[1:]:
            a=tok.split("/")
            c.append((int(a[0])-1, int(a[1])-1 if len(a)>1 and a[1] else 0))
        for k in range(1,len(c)-1):
            Ftmp.append((c[0],c[k],c[k+1]))
V0=np.array(Vs); VT=np.array(VTs)
F=[[[a[0],b[0],c[0]],[a[1],b[1],c[1]]] for a,b,c in Ftmp]
print("obj: %dv %d tris"%(len(V0),len(F)))

tex=Image.open(os.environ["JAGPAD_TEX"]).convert("RGB")
tpx=np.asarray(tex); th,tw,_=tpx.shape
B=np.stack([V0[:,0], -V0[:,1], -V0[:,2]],1)

lo=B.min(0); hi=B.max(0); c=(lo+hi)/2
print("blob bounds X[%.2f,%.2f] Y[%.2f,%.2f] Z[%.2f,%.2f]"%(lo[0],hi[0],lo[1],hi[1],lo[2],hi[2]))
RES=1024
sc=(RES-10)/max(hi[0]-lo[0],hi[1]-lo[1])
def plate(back):
    img=np.zeros((RES,RES,3),dtype=np.uint8); zb=np.full((RES,RES),1e9)
    sgn=-1 if back else 1
    for vi,ti in F:
        P=[B[i] for i in vi]; uvq=[VT[t] for t in ti]
        pts=[(((p[0]-c[0])*sgn)*sc+RES/2,(p[1]-c[1])*sc+RES/2) for p in P]
        zs=[p[2]*sgn for p in P]     # keep SMALLEST bz (front) / largest (back->sgn)
        xsA=[p[0] for p in pts]; ysA=[p[1] for p in pts]
        ax0,ax1=int(max(0,min(xsA))),int(min(RES-1,max(xsA)))
        ay0,ay1=int(max(0,min(ysA))),int(min(RES-1,max(ysA)))
        if ax1<ax0 or ay1<ay0: continue
        dd=((pts[1][0]-pts[0][0])*(pts[2][1]-pts[0][1])-(pts[2][0]-pts[0][0])*(pts[1][1]-pts[0][1]))
        if abs(dd)<1e-9: continue
        gy,gx=np.mgrid[ay0:ay1+1,ax0:ax1+1]
        w0=((pts[1][0]-gx)*(pts[2][1]-gy)-(pts[2][0]-gx)*(pts[1][1]-gy))/dd
        w1=((pts[2][0]-gx)*(pts[0][1]-gy)-(pts[0][0]-gx)*(pts[2][1]-gy))/dd
        w2=1.0-w0-w1
        m=(w0>=-0.001)&(w1>=-0.001)&(w2>=-0.001)
        if not m.any(): continue
        z=(w0*zs[0]+w1*zs[1]+w2*zs[2])
        u=(w0*uvq[0][0]+w1*uvq[1][0]+w2*uvq[2][0]).clip(0,1)
        v=(w0*uvq[0][1]+w1*uvq[1][1]+w2*uvq[2][1]).clip(0,1)
        zbs=zb[ay0:ay1+1,ax0:ax1+1]
        upd=m&(z<zbs)
        if not upd.any(): continue
        tx=(u*(tw-1)).astype(int); ty=((1-v)*(th-1)).astype(int)
        sub=img[ay0:ay1+1,ax0:ax1+1]
        sub[upd]=tpx[ty[upd],tx[upd]]
        zbs[upd]=z[upd]
    return img,(zb<1e8)

STYLE=os.environ.get("PAD_STYLE","flat")
f,fm=plate(False); b,bm=plate(True)
if STYLE=="flat":
    # CLEAN DRAWN PLATES (user 2026-08-04: "charcoal, C/B/A red, match the
    # dpad and pause/option, call it a day"): flat regions read perfectly at
    # ring scale and are immune to texture-step wobble. Drawn 4x, box down.
    from PIL import ImageDraw as _ID
    S4=4; W4=1024
    CHAR=(46,46,50); CHARD=(34,34,38); CHARL=(64,64,70)
    RED=(196,32,28); KEY=(88,88,94); KEYD=(26,26,30)
    fim=Image.new("RGB",(W4,W4),(0,0,0))
    dr=_ID.Draw(fim)
    # body fill comes from the MASK later; just draw the face art full-bleed
    dr.rectangle([0,0,W4,W4],fill=CHAR)
    dr.rectangle([0,0,W4,int(W4*0.16)],fill=CHARL)          # top bevel
    # dpad upper-left: cross
    cx,cy,arm,thk=int(W4*0.22),int(W4*0.30),int(W4*0.115),int(W4*0.062)
    dr.rectangle([cx-arm,cy-thk,cx+arm,cy+thk],fill=KEYD)
    dr.rectangle([cx-thk,cy-arm,cx+thk,cy+arm],fill=KEYD)
    dr.rectangle([cx-thk+6,cy-thk+6,cx+thk-6,cy+thk-6],fill=(52,52,58))
    # C B A red discs diagonal upper-right (C highest-left, A lowest-right)
    r4=int(W4*0.052)
    for k,(bx,by) in enumerate([(0.60,0.235),(0.71,0.30),(0.82,0.365)]):
        x,y=int(W4*bx),int(W4*by)
        dr.ellipse([x-r4,y-r4,x+r4,y+r4],fill=RED)
        dr.ellipse([x-r4,y-r4,x+r4,y-r4+r4],outline=(230,90,80))
    # pause / option pills centre
    for k,px4 in enumerate([0.40,0.50]):
        x,y=int(W4*px4),int(W4*0.30)
        dr.rounded_rectangle([x-int(W4*0.030),y-int(W4*0.014),
                              x+int(W4*0.030),y+int(W4*0.014)],
                             radius=int(W4*0.012),fill=KEYD,outline=CHARL)
    # keypad lower half: 3x4 grey keys on a dark bed
    bx0,by0,bx1,by1=int(W4*0.16),int(W4*0.46),int(W4*0.84),int(W4*0.94)
    dr.rounded_rectangle([bx0,by0,bx1,by1],radius=int(W4*0.03),fill=KEYD)
    kw=(bx1-bx0)/3.0; kh=(by1-by0)/4.0
    for r5 in range(4):
        for c5 in range(3):
            x0=int(bx0+c5*kw+kw*0.16); x1=int(bx0+(c5+1)*kw-kw*0.16)
            y0=int(by0+r5*kh+kh*0.20); y1=int(by0+(r5+1)*kh-kh*0.20)
            dr.rounded_rectangle([x0,y0,x1,y1],radius=6,fill=KEY)
    fim=fim.resize((RES,RES),Image.BOX)
    f=np.asarray(fim).copy(); f[~fm]=0
    # back: charcoal + darker inset panel (label plate)
    bim=Image.new("RGB",(W4,W4),CHARD)
    db=_ID.Draw(bim)
    db.rounded_rectangle([int(W4*0.28),int(W4*0.22),int(W4*0.72),int(W4*0.78)],
                         radius=int(W4*0.04),fill=(40,40,44),outline=CHARL)
    db.rectangle([int(W4*0.33),int(W4*0.30),int(W4*0.67),int(W4*0.40)],fill=KEY)
    bim=bim.resize((RES,RES),Image.BOX)
    b=np.asarray(bim).copy(); b[~bm]=0
def down(img):
    return np.asarray(Image.fromarray(img).resize((256,256),Image.BOX))
fmask=np.asarray(Image.fromarray((fm*255).astype(np.uint8)).resize((256,256),Image.BOX))>127
fimg=down(f); bimg=down(b)

# ---- 1. convex-hull outline (pad silhouette is near-convex) ----
ys,xs=np.where(fmask)
pts=sorted(set(zip(xs.tolist(),ys.tolist())))
def half(ps):
    h=[]
    for q2 in ps:
        while len(h)>=2 and (h[-1][0]-h[-2][0])*(q2[1]-h[-2][1])-(h[-1][1]-h[-2][1])*(q2[0]-h[-2][0])<=0: h.pop()
        h.append(q2)
    return h
hull=half(pts)[:-1]+half(pts[::-1])[:-1]
def dp(pts2,eps):
    if len(pts2)<3: return pts2
    ax,ay=pts2[0]; bx2,by=pts2[-1]
    dmax,idx=0,0
    for i in range(1,len(pts2)-1):
        px,py=pts2[i]
        num=abs((by-ay)*px-(bx2-ax)*py+bx2*ay-by*ax)
        den=math.hypot(by-ay,bx2-ax) or 1
        dd=num/den
        if dd>dmax: dmax,idx=dd,i
    if dmax>eps:
        l=dp(pts2[:idx+1],eps); r=dp(pts2[idx:],eps)
        return l[:-1]+r
    return [pts2[0],pts2[-1]]
# DP needs a non-degenerate chord: split the ring at the x-extremes
i0=min(range(len(hull)),key=lambda i:hull[i][0])
i1=max(range(len(hull)),key=lambda i:hull[i][0])
if i0>i1: i0,i1=i1,i0
chainA=hull[i0:i1+1]; chainB=hull[i1:]+hull[:i0+1]
def simp(eps):
    a=dp(chainA,eps); b2=dp(chainB,eps)
    return a[:-1]+b2[:-1]
eps=2.0; poly=simp(eps)
while len(poly)>17: eps+=0.7; poly=simp(eps)
N=len(poly)
print("outline points:",N)

# ---- 2. blob-space scale ----
# plate pixels -> blob: x centred +-78 wide; y centred; z +-22
pxs=[p[0] for p in poly]; pys=[p[1] for p in poly]
cx=(max(pxs)+min(pxs))/2; cy=(max(pys)+min(pys))/2
sc=156.0/(max(pxs)-min(pxs))
TH=22
def to_blob(p): return (int(round((p[0]-cx)*sc)), int(round((p[1]-cy)*sc)))
P2=[to_blob(p) for p in poly]


# ---- 3. GRID-CLIP plate mesh (2026-08-04, replaces ear-clip+split):
# uniform cells clipped to the convex outline -> well-shaped tris, NO
# T-junctions, NO slivers. Midpoint tessellation of ear-clip fans made
# sliver chains whose INTEGER screen area flips sign mid-spin = fat wedge
# holes on silicon (the "yellow streaks" = dial art through the gaps).
CELL=int(os.environ.get("PAD_CELL","34"))
# hull as CCW (pixel space y-down: enforce by signed area)
sa=sum(P2[i][0]*P2[(i+1)%N][1]-P2[(i+1)%N][0]*P2[i][1] for i in range(N))
H=P2[:] if sa>0 else P2[::-1]
def clip_cell(x0,y0,x1,y1):
    poly=[(x0,y0),(x1,y0),(x1,y1),(x0,y1)]
    for i in range(len(H)):
        a=H[i]; b2=H[(i+1)%len(H)]
        out=[]
        for j in range(len(poly)):
            c=poly[j]; d2=poly[(j+1)%len(poly)]
            ic=(b2[0]-a[0])*(c[1]-a[1])-(b2[1]-a[1])*(c[0]-a[0])>=0
            idd=(b2[0]-a[0])*(d2[1]-a[1])-(b2[1]-a[1])*(d2[0]-a[0])>=0
            if ic: out.append(c)
            if ic!=idd:
                # p(t)=c+t(d-c) on line ab: t = -cross(b-a,c-a)/cross(b-a,d-c)
                cr_ca=(b2[0]-a[0])*(c[1]-a[1])-(b2[1]-a[1])*(c[0]-a[0])
                cr_dc=(b2[0]-a[0])*(d2[1]-c[1])-(b2[1]-a[1])*(d2[0]-c[0])
                if cr_dc!=0:
                    t=max(0.0,min(1.0,-cr_ca/cr_dc))
                    out.append((c[0]+(d2[0]-c[0])*t, c[1]+(d2[1]-c[1])*t))
        poly=out
        if not poly: return []
    return poly
xs2=[p[0] for p in P2]; ys2=[p[1] for p in P2]
gx0,gx1=min(xs2),max(xs2); gy0,gy1=min(ys2),max(ys2)
vmap={}; capv=[]
def vid(pt):
    k=(int(round(pt[0])),int(round(pt[1])))
    if k not in vmap:
        vmap[k]=len(capv); capv.append(k)
    return vmap[k]
cap=[]
yy=gy0
while yy<gy1:
    xx=gx0
    while xx<gx1:
        poly=clip_cell(xx,yy,min(xx+CELL,gx1),min(yy+CELL,gy1))
        if len(poly)>=3:
            ids=[vid(pt) for pt in poly]
            ids2=[ids[0]]
            for k in ids[1:]:
                if k!=ids2[-1]: ids2.append(k)
            if len(ids2)>=3 and ids2[0]!=ids2[-1]:
                for k in range(1,len(ids2)-1):
                    a,b3,c=ids2[0],ids2[k],ids2[k+1]
                    ar=(capv[b3][0]-capv[a][0])*(capv[c][1]-capv[a][1])-(capv[c][0]-capv[a][0])*(capv[b3][1]-capv[a][1])
                    if ar!=0: cap.append((a,b3,c))
        xx+=CELL
    yy+=CELL
NCV=len(capv)
# deterministic jitter on INTERIOR verts: axis-aligned tri edges collapse in
# the kernel's edge walker (flat-top checkerboard, silicon 2026-08-04);
# PSX meshes never have them, ours were ALL axis-aligned. Boundary stays
# exact for the rim stitch.
bset={ (int(round(pt[0])),int(round(pt[1]))) for pt in [] }
def _onhull(pt):
    for i in range(len(H)):
        a=H[i]; b2=H[(i+1)%len(H)]
        cr=(b2[0]-a[0])*(pt[1]-a[1])-(b2[1]-a[1])*(pt[0]-a[0])
        if abs(cr)<=max(abs(b2[0]-a[0]),abs(b2[1]-a[1]))*1.5:
            dot=(pt[0]-a[0])*(b2[0]-a[0])+(pt[1]-a[1])*(b2[1]-a[1])
            L2=(b2[0]-a[0])**2+(b2[1]-a[1])**2
            if -0.05*L2<=dot<=1.05*L2: return True
    return False
for i2,(vx,vy) in enumerate(capv):
    if _onhull((vx,vy)): continue
    hsh=(vx*73856093 ^ vy*19349663)&7
    capv[i2]=(vx+(hsh%3)-1+((hsh>>1)&1)*2-1, vy+((hsh>>2)%3)-1+(hsh&1)*2-1)
print("grid mesh: %d verts %d tris (interior jittered)"%(NCV,len(cap)))
# boundary verts (on a hull edge) ordered along the perimeter, for the rim
def on_edge(pt):
    for i in range(len(H)):
        a=H[i]; b2=H[(i+1)%len(H)]
        cr=(b2[0]-a[0])*(pt[1]-a[1])-(b2[1]-a[1])*(pt[0]-a[0])
        if abs(cr)<=max(abs(b2[0]-a[0]),abs(b2[1]-a[1]))*1.5:
            dot=(pt[0]-a[0])*(b2[0]-a[0])+(pt[1]-a[1])*(b2[1]-a[1])
            L2=(b2[0]-a[0])**2+(b2[1]-a[1])**2
            if -0.05*L2<=dot<=1.05*L2:
                return i+max(0,min(1,dot/L2 if L2 else 0))
    return None
bnd=[]
for idx,pt in enumerate(capv):
    t=on_edge(pt)
    if t is not None: bnd.append((t,idx))
bnd.sort()
ring=[i for _,i in bnd]
NB=len(ring)
print("rim boundary verts:",NB)

verts=[]
for (x,y) in capv: verts.append((x,y,-TH))     # front sheet (toward camera)
for (x,y) in capv: verts.append((x,y, TH))     # back sheet
# ---- 4. atlas: front 124x?, back beside, rim swatch; quantize ----
pal=struct.unpack(">256H",open(OUT+"/title_pal.bin","rb").read())
def dec(c): return (((c>>11)&31)*255//31,((c>>1)&31)*255//31,((c>>6)&31)*255//31)
prgb=[dec(c) for c in pal]
GAIN=float(os.environ.get("PAD_GAIN","1.0"))
def nearest(r,g,b):
    r=min(255,int(r*GAIN)); g=min(255,int(g*GAIN)); b=min(255,int(b*GAIN))
    best=1<<30; bi=0
    for i,(rr,gg,bb) in enumerate(prgb):
        d=(r-rr)**2+(g-gg)**2+(b-bb)**2
        if d<best: best,bi=d,i
    return bi
# PW: plate texel width. The item is ~35px on screen - 120 texels was 4x
# minified, and the kernel's edge law (gpu_geotex.gas:1983: +-1px edge error
# = +-du TEXELS) turns minification into streaks. ~1:1 texels:pixels keeps
# per-tri UV spans under the safe bound with the 96-tri geometry.
PW=int(os.environ.get("PAD_PW","48"))
fsm=Image.fromarray(fimg).resize((PW,PW),Image.BOX)
bsm=Image.fromarray(bimg).resize((PW,PW),Image.BOX)
AH=PW+8
atlas=bytearray(256*AH)
fq=np.asarray(fsm); bq=np.asarray(bsm)
ncache={}
def q(r,g,b):
    k=(r>>2,g>>2,b>>2)
    if k not in ncache: ncache[k]=nearest(r,g,b)
    return ncache[k]
for y in range(PW):
    for x in range(PW):
        r,g,b=fq[y,x]; atlas[y*256+x]=q(r,g,b)
        r,g,b=bq[y,x]; atlas[y*256+128+x]=q(r,g,b)
RIM=q(45,45,48)
print('rim idx',RIM,'rgb',prgb[RIM])
for y in range(AH-8,AH):
    for x in range(0,8): atlas[y*256+x]=RIM
rimuv=(4,AH-4)
def plate_uv(x,y,back):
    # blob (x,y) -> plate pixel in the 120-wide plate image
    u=(x/sc)+ (max(pxs)-min(pxs))/2
    v=(y/sc)+ (max(pys)-min(pys))/2
    uu=u/(max(pxs)-min(pxs))*(PW-1); vv=v/(max(pys)-min(pys))*(PW-1)
    uu=max(0,min(PW-1,uu)); vv=max(0,min(PW-1,vv))
    if back: return (128+int(PW-1-uu), int(vv))
    return (int(uu),int(vv))

# ---- 5. faces: front cap, back cap (reversed), rim ----
tris=[]
for (a,bb,c) in cap:
    tris.append(([a,bb,c],[plate_uv(*capv[a],False),plate_uv(*capv[bb],False),plate_uv(*capv[c],False)]))
for (a,bb,c) in cap:
    tris.append(([NCV+c,NCV+bb,NCV+a],[plate_uv(*capv[c],True),plate_uv(*capv[bb],True),plate_uv(*capv[a],True)]))
for k in range(NB):
    a=ring[k]; b4=ring[(k+1)%NB]
    tris.append(([a,b4,NCV+b4],[rimuv]*3))
    tris.append(([a,NCV+b4,NCV+a],[rimuv]*3))
# ---- 6. winding: negative signed volume (PS1 convention) ----
def vol():
    v=0
    for vi,_ in tris:
        a,c2,d2=[verts[k] for k in vi]
        v+=a[0]*(c2[1]*d2[2]-c2[2]*d2[1])-a[1]*(c2[0]*d2[2]-c2[2]*d2[0])+a[2]*(c2[0]*d2[1]-c2[1]*d2[0])
    return v
VOLSIGN=int(os.environ.get("PAD_VOLSIGN","1"))
if (vol()>0) != (VOLSIGN>0):
    tris=[([v[0],v[2],v[1]],[u[0],u[2],u[1]]) for v,u in tris]
print("signed volume:",vol())

nv,nt=len(verts),len(tris)
exp=16+nv*8+nt*30
print("prism: %dv %dt expanded %dB"%(nv,nt,exp))
assert exp<=10560
bb2=bytearray()
bb2+=struct.pack(">HHHHH",nv,0,nt,256,AH)
bb2+=struct.pack(">hhh",0,0,0)
for (x,y,z) in verts: bb2+=struct.pack(">hhhH",x,y,z,255)
for vi,uvs in tris:
    bb2+=struct.pack(">HHH",*vi)
    for (u,v) in uvs: bb2+=struct.pack(">HH",int(u),int(v))
while len(bb2)&7: bb2+=b'\0'
open(OUT+"/ctrl_geom.bin","wb").write(bb2)
open(OUT+"/ctrl_atlas.bin","wb").write(bytes(atlas))
print("blob %dB atlas 256x%d"%(len(bb2),AH))
