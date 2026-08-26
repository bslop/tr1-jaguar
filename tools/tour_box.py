#!/usr/bin/env python3
"""tour_box.py <tour.mkv> [hold_fields=120] [x0 y0 x1 y1] - per-room fps from a
ROOMTOUR capture using the beacon BOX (see beacon_box.py: the variance locator
in tour_fps.py picks an edge pixel at VRESN=80).  Rooms are found from the
scene cuts (teleports), checked against the expected hold comb, and read as
beacon transitions per segment.  Prints per-room fps + median/min/max."""
import subprocess, sys, numpy as np
path=sys.argv[1]; hold=int(sys.argv[2]) if len(sys.argv)>2 else 120
box=tuple(int(v) for v in sys.argv[3:7]) if len(sys.argv)>6 else (168,150,208,185)
W,H=720,480
raw=subprocess.run(["ffmpeg","-v","error","-i",path,"-f","rawvideo","-pix_fmt","gray","-"],capture_output=True).stdout
n=len(raw)//(W*H); fr=np.frombuffer(raw[:n*W*H],np.uint8).reshape(n,H,W)
x0,y0,x1,y1=box
sig=fr[:,y0:y1,x0:x1].mean(axis=(1,2)); mid=(np.percentile(sig,5)+np.percentile(sig,95))/2; state=sig>mid
# scene cuts: whole-frame abs diff with the beacon box masked out
m=fr.astype(np.int16); m[:,y0:y1,x0:x1]=0
d=np.abs(m[1:]-m[:-1]).mean(axis=(1,2))
nrooms=38
# tour_fps.py's rule: take the largest diffs greedily, each at least hold/2
# from every cut already taken, until nrooms-1 cuts (teleports) are chosen.
order=np.argsort(-d); cuts=[]
for i in order:
    if all(abs(i-c)>hold*0.5 for c in cuts): cuts.append(int(i)+1)
    if len(cuts)>=nrooms-1: break
segs=sorted(cuts); thr=d[order[len(cuts)-1]] if cuts else 0
print("frames=%d (%.1fs) cuts=%d (thr %.2f) expected hold=%d fields"%(n,n/60,len(segs),thr,hold))
gaps=np.diff(segs); 
if len(gaps): print("gap comb: median %d, min %d, max %d"%(np.median(gaps),gaps.min(),gaps.max()))
res=[]
bounds=segs+[n]
for k in range(len(segs)):
    a,b=bounds[k],bounds[k+1]
    if not (hold*0.75 <= b-a <= hold*1.25): continue   # only true holds
    s=state[a:b]; t=np.count_nonzero(s[1:]!=s[:-1]); fps=t/((b-a)/60.0); res.append((k,a,b,fps))
    print("  seg %2d  f%5d-%5d  %.2f fps"%(k,a,b,fps))
if res:
    v=np.array([r[3] for r in res]); print("rooms=%d  median %.2f  min %.2f  max %.2f  mean %.2f"%(len(v),np.median(v),v.min(),v.max(),v.mean()))
