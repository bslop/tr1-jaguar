import json, math, sys
import numpy as np
W,H = 320,120
CX,CY = 160,60
FOCAL, FOCAL_Y = 160, 80
NEAR = 64
d = json.load(open('/tmp/sector_room26.json'))
V = np.array(d['verts'], dtype=float)
faces = d['faces']

xs,ys,zs = V[:,0],V[:,1],V[:,2]
cxm,czm = (xs.min()+xs.max())/2,(zs.min()+zs.max())/2
ymid = (ys.min()+ys.max())/2
print("room 26 bbox  x %.0f..%.0f  y %.0f..%.0f  z %.0f..%.0f"
      %(xs.min(),xs.max(),ys.min(),ys.max(),zs.min(),zs.max()))

def render(camx,camy,camz,yaw,pitch=0.0):
    cY,sY = math.cos(yaw),math.sin(yaw)
    cP,sP = math.cos(pitch),math.sin(pitch)
    dx,dy,dz = V[:,0]-camx, V[:,1]-camy, V[:,2]-camz
    rx = dx*cY - dz*sY
    rz = dx*sY + dz*cY
    ry = dy*cP - rz*sP
    rz2 = dy*sP + rz*cP
    ok = rz2 >= NEAR
    sx = np.where(ok, CX + rx*FOCAL/np.maximum(rz2,1), 0)
    sy = np.where(ok, CY + ry*FOCAL_Y/np.maximum(rz2,1), 0)

    zbuf = np.full((H,W), 1e9)
    surf = np.full((H,W), -1, dtype=np.int32)
    cur_spans = 0
    surf_ids = {}
    for fi,f in enumerate(faces):
        idx = f['v']
        if not all(ok[i] for i in idx): continue
        px = [sx[i] for i in idx]; py = [sy[i] for i in idx]
        # BACKFACE CULL, as the kernel does: screen-space signed area. Without
        # this the "current" span count is inflated by every wall facing away,
        # and the comparison flatters the sector renderer.
        ar = 0.0
        for e in range(len(idx)):
            ax,ay = px[e],py[e]; bx,by = px[(e+1)%len(idx)],py[(e+1)%len(idx)]
            ar += ax*by - bx*ay
        if ar <= 0: continue
        zz = float(np.mean([rz2[i] for i in idx]))
        y0 = max(0,int(math.floor(min(py)))); y1 = min(H-1,int(math.ceil(max(py))))
        if y1 < y0: continue
        x0 = max(0,int(math.floor(min(px)))); x1 = min(W-1,int(math.ceil(max(px))))
        if x1 < x0: continue
        # CURRENT renderer: the kernel emits ONE SPAN PER ROW the face covers
        cur_spans += (y1-y0+1)
        key = (tuple(f['plane']), f['tex'])
        sid = surf_ids.setdefault(key, len(surf_ids))
        # scanline fill of the projected polygon (convex), z-buffered
        n = len(idx)
        for yy in range(y0,y1+1):
            xsx=[]
            for e in range(n):
                ax,ay = px[e],py[e]; bx,by = px[(e+1)%n],py[(e+1)%n]
                if (ay<=yy<by) or (by<=yy<ay):
                    t=(yy-ay)/(by-ay); xsx.append(ax+t*(bx-ax))
            if len(xsx)<2: continue
            xa,xb = int(math.floor(min(xsx))), int(math.ceil(max(xsx)))
            xa=max(0,xa); xb=min(W-1,xb)
            if xb<xa: continue
            seg = slice(xa,xb+1)
            m = zz < zbuf[yy,seg]
            zbuf[yy,seg] = np.where(m, zz, zbuf[yy,seg])
            surf[yy,seg] = np.where(m, sid, surf[yy,seg])
    # SECTOR renderer: per row, one span per contiguous run of the same surface
    sec_spans = 0
    for yy in range(H):
        row = surf[yy]
        vis = row >= 0
        if not vis.any(): continue
        changes = np.diff(row.astype(np.int64))
        sec_spans += 1 + int((changes != 0).sum())
    cov = 100.0*(surf>=0).mean()
    return cur_spans, sec_spans, cov

print("\n   yaw   coverage   CURRENT spans   SECTOR spans   ratio")
tot_c=tot_s=0
for deg in (0,45,90,135,180,225,270,315):
    c,s,cov = render(cxm, ymid, czm, math.radians(deg))
    tot_c+=c; tot_s+=s
    print("   %4d°   %5.1f%%      %6d          %5d        %4.1fx"%(deg,cov,c,s,c/max(s,1)))
print("\n   MEAN over 8 yaws: current %d  sector %d  => %.1fx fewer spans"
      %(tot_c/8, tot_s/8, (tot_c/8)/max(tot_s/8,1)))
print("   [G2 gate: sector must come in under ~1500]")
