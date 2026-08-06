# faithful title-kernel visibility sim: NO z-sort, faces in DATA order
# (quads then tris), single-sign screen-area cull. CULLSIGN=+1 keeps ar>0.
# (Promoted to tools/ 2026-08-05 after living dangerously in the session
# scratchpad - this file is the offline oracle for every ring-item and
# staging experiment; silicon artifacts reproduce here.)
import struct, math, os, sys
import numpy as np
from PIL import Image
SIN=[int(round(math.sin(i*2*math.pi/256)*65536)) for i in range(256)]
def S(a): return SIN[a&255]
def C(a): return SIN[(a+64)&255]
def load(gpath,apath,ppath):
    pal=struct.unpack(">256H",open(ppath,"rb").read())
    prgb=np.array([(((c>>11)&31)*255//31,((c>>1)&31)*255//31,((c>>6)&31)*255//31) for c in pal],dtype=np.uint8)
    blob=open(gpath,"rb").read()
    nv,nq,nt,aw,ah=struct.unpack(">HHHHH",blob[:10])
    o=16
    V=np.array(struct.unpack(">%dh"%(nv*4),blob[o:o+nv*8]),dtype=np.int32).reshape(nv,4)[:,:3]; o+=nv*8
    faces=[]
    for _ in range(nq):
        vi=struct.unpack(">HHHH",blob[o:o+8]); o+=8
        uv=[struct.unpack(">HH",blob[o+i*4:o+i*4+4]) for i in range(4)]; o+=16
        faces.append((list(vi),uv))
    for _ in range(nt):
        vi=struct.unpack(">HHH",blob[o:o+6]); o+=6
        uv=[struct.unpack(">HH",blob[o+i*4:o+i*4+4]) for i in range(3)]; o+=12
        faces.append((list(vi),uv))
    atl=np.frombuffer(open(apath,"rb").read(),dtype=np.uint8)
    atl=atl.reshape(-1,aw) if len(atl)>=aw else atl.reshape(1,-1)
    return V,faces,atl,prgb
def render(V,faces,atl,prgb,yaw,pitch,roll,px,py,pz,cullsign):
    img=np.zeros((240,320,3),dtype=np.uint8); img[:]= (25,20,30)
    P=[]
    for x,y,z in V:
        ly,lz=int(y),int(z)
        if pitch: t=(ly*C(pitch)-lz*S(pitch))>>16; lz=(ly*S(pitch)+lz*C(pitch))>>16; ly=t
        lx=(int(x)*C(yaw)+lz*S(yaw))>>16; lz2=(lz*C(yaw)-int(x)*S(yaw))>>16
        if roll: t=(lx*C(roll)-ly*S(roll))>>16; ly=(lx*S(roll)+ly*C(roll))>>16; lx=t
        X,Y,Z=lx+px,ly+py,lz2+pz
        Y+=-20
        cp,sp=C(6)/65536.0,S(6)/65536.0
        Y2=Y*cp-Z*sp; Z2=max(16,Y*sp+Z*cp)
        # INTEGER projection like the kernel
        P.append((int(160+X*190.0/Z2),int(120+Y2*190.0/Z2)))
    H,W,_=img.shape
    for vi,uv in faces:          # DATA ORDER - no sort (kernel truth)
        pts=[P[i] for i in vi]
        ar=0
        for i in range(len(pts)):
            x0,y0=pts[i]; x1,y1=pts[(i+1)%len(pts)]
            ar+=x0*y1-x1*y0
        if cullsign*ar <= 0: continue          # cull + degenerate
        for t in ([(0,1,2)] if len(vi)==3 else [(0,1,2),(0,2,3)]):
            p=[pts[i] for i in t]; q=[uv[i] for i in t]
            xs=[pp[0] for pp in p]; ys=[pp[1] for pp in p]
            x0,x1=max(0,min(xs)),min(W-1,max(xs))
            y0,y1=max(0,min(ys)),min(H-1,max(ys))
            if x1<x0 or y1<y0: continue
            d=((p[1][0]-p[0][0])*(p[2][1]-p[0][1])-(p[2][0]-p[0][0])*(p[1][1]-p[0][1]))
            if d==0: continue
            gy,gx=np.mgrid[y0:y1+1,x0:x1+1]
            w0=((p[1][0]-gx)*(p[2][1]-gy)-(p[2][0]-gx)*(p[1][1]-gy))/d
            w1=((p[2][0]-gx)*(p[0][1]-gy)-(p[0][0]-gx)*(p[2][1]-gy))/d
            w2=1.0-w0-w1
            m=(w0>=-0.001)&(w1>=-0.001)&(w2>=-0.001)
            if not m.any(): continue
            uu=(w0*q[0][0]+w1*q[1][0]+w2*q[2][0]).clip(0,atl.shape[1]-1).astype(int)
            vv=(w0*q[0][1]+w1*q[1][1]+w2*q[2][1]).clip(0,atl.shape[0]-1).astype(int)
            img[gy[m],gx[m]]=prgb[atl[vv[m],uu[m]]]
    return img
