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

f,fm=plate(False); b,bm=plate(True)
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
verts=[]
for (x,y) in P2: verts.append((-x,y, TH))      # front ring (180 about Y:
for (x,y) in P2: verts.append((-x,y,-TH))      # face lands at ring yaw 0)

# ---- 3. ear-clip the outline (works for concave) ----
def area2(a,b,c): return (b[0]-a[0])*(c[1]-a[1])-(c[0]-a[0])*(b[1]-a[1])
def earclip(pts):
    n=len(pts); idxs=list(range(n))
    # ensure CCW in pixel space (y down): signed area
    sa=sum(pts[i][0]*pts[(i+1)%n][1]-pts[(i+1)%n][0]*pts[i][1] for i in range(n))
    if sa<0: idxs=idxs[::-1]
    tris=[]
    guard=0
    while len(idxs)>3 and guard<1000:
        guard+=1
        n2=len(idxs)
        for k in range(n2):
            a,bb,c=idxs[(k-1)%n2],idxs[k],idxs[(k+1)%n2]
            if area2(pts[a],pts[bb],pts[c])<=0: continue
            ok=True
            for j in idxs:
                if j in (a,bb,c): continue
                w0=area2(pts[a],pts[bb],pts[j]); w1=area2(pts[bb],pts[c],pts[j]); w2=area2(pts[c],pts[a],pts[j])
                if w0>0 and w1>0 and w2>0: ok=False; break
            if ok:
                tris.append((a,bb,c)); idxs.pop(k); break
        else: break
    if len(idxs)==3: tris.append(tuple(idxs))
    return tris
cap=earclip(P2)
print("cap tris:",len(cap))

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
PW=120
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
    tris.append(([a,bb,c],[plate_uv(*P2[a],False),plate_uv(*P2[bb],False),plate_uv(*P2[c],False)]))
for (a,bb,c) in cap:
    tris.append(([N+c,N+bb,N+a],[plate_uv(*P2[c],True),plate_uv(*P2[bb],True),plate_uv(*P2[a],True)]))
for i in range(N):
    j=(i+1)%N
    a,bq2,c,d2=i,j,N+j,N+i
    tris.append(([a,bq2,c],[rimuv]*3))
    tris.append(([a,c,d2],[rimuv]*3))
# ---- 6. winding: negative signed volume (PS1 convention) ----
def vol():
    v=0
    for vi,_ in tris:
        a,c2,d2=[verts[k] for k in vi]
        v+=a[0]*(c2[1]*d2[2]-c2[2]*d2[1])-a[1]*(c2[0]*d2[2]-c2[2]*d2[0])+a[2]*(c2[0]*d2[1]-c2[1]*d2[0])
    return v
if vol()>0:
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
