#!/usr/bin/env python3
"""tour_pair.py <ref.mkv> <arm.mkv>... - per-ROOM still-camera flicker across
ROOMTOUR captures, rooms paired by CONTENT (nearest mean image), not by
segment index: the cut detector drifts between captures and index pairing
silently compares different rooms (2026-08-26: it "showed" a 94% fix in a
room that was a different room). Metric per room = world pixels toggling in
>= 50% of the frames rendered in the settled last 90 fields of the hold
(frame-rate fair: a >=N-toggle threshold favours the faster arm).
"""
import subprocess, sys, numpy as np
hold=120; w,h=360,240; WIN=90
def analyse(path):
    raw=subprocess.run(["ffmpeg","-v","error","-i",path,"-f","rawvideo","-pix_fmt","gray","-vf","scale=360:240","-"],capture_output=True).stdout
    n=len(raw)//(w*h); fr=np.frombuffer(raw[:n*w*h],np.uint8).reshape(n,h,w).astype(np.int16)
    lo=fr.min(axis=0); hi=fr.max(axis=0); dd=np.abs(np.diff(fr,axis=0))>12; togall=dd.sum(axis=0)
    beacon=(lo<24)&(hi>200)&(togall>200)
    def dil(m,k):
        o=m.copy()
        for dy in range(-k,k+1):
            for dx in range(-k,k+1): o|=np.roll(np.roll(m,dy,0),dx,1)
        return o
    mask=np.ones((h,w),bool); mask[dil(beacon,4)]=False
    mask[:10,:]=False; mask[-10:,:]=False; mask[:,:56]=False; mask[:,-56:]=False; mask[95:235,140:225]=False
    bsig=fr[:,beacon].mean(axis=1); bstate=bsig>bsig.mean()
    m=fr.copy(); m[:,~mask]=0
    d=np.abs(m[1:]-m[:-1]).mean(axis=(1,2)); order=np.argsort(-d); cuts=[]
    for i in order:
        if all(abs(i-c)>hold*0.5 for c in cuts): cuts.append(int(i)+1)
        if len(cuts)>=37: break
    segs=sorted(cuts)+[n]; out=[]
    for k in range(len(segs)-1):
        a,b=segs[k],segs[k+1]
        if not (hold*0.75<=b-a<=hold*1.25): continue
        a=b-WIN; frames=int(np.count_nonzero(bstate[a+1:b]!=bstate[a:b-1]))
        t=dd[a:b-1].sum(axis=0); thr=max(2,int(round(0.5*max(frames-1,1)))); r=int(((t>=thr)&mask).sum())
        mean=fr[a:b].mean(axis=0); mean[~mask]=0
        out.append(dict(seg=k,frames=frames,racing=r,mean=mean[::4,::4].astype(np.float32)))
    return out
ref=analyse(sys.argv[1]); others=[analyse(p) for p in sys.argv[2:]]
names=['A2']+[f'arm{i+1}' for i in range(len(others))]
print("pairing by nearest mean image (MSE); '-' = no segment within tolerance")
tot=[0]*(1+len(others)); cnt=0
rows=[]
for s in ref:
    row=[s['racing']]
    for o in others:
        best=min(o,key=lambda x: ((x['mean']-s['mean'])**2).mean())
        mse=((best['mean']-s['mean'])**2).mean()
        row.append(best['racing'] if mse<60 else None)
    rows.append((s['seg'],row))
    if all(v is not None for v in row):
        cnt+=1
        for i,v in enumerate(row): tot[i]+=v
for seg,row in rows:
    if max(v for v in row if v is not None)>=100: print("ref seg %2d: "%seg+"  ".join(("%5d"%v if v is not None else "    -") for v in row))
print("matched rooms:",cnt,"| totals over matched rooms:",dict(zip(names,tot)))
