#!/usr/bin/env python3
"""race_px.py <capture.mkv> [mask.png] - the A1 still-camera metric: WORLD
pixels that toggle >=8 times over the capture, with the FPSBEACON block,
Lara and the picture borders masked.
☠️ THE BEACON MASK IS DERIVED FROM THE DATA (pixels swinging ~0<->~250,
dilated 4 px), not hard-coded: a hand-placed box missed the block's top rows
on 2026-08-26 and every "racing world px" number that session (536/463/469/
482) was ~90% beacon edge + Lara's breathing. Corrected: base 60, M68A2 30.
Look at the mask image before believing the number.
"""
import subprocess, sys, numpy as np
from PIL import Image
path=sys.argv[1]; out=sys.argv[2] if len(sys.argv)>2 else None; w,h=360,240
raw=subprocess.run(["ffmpeg","-v","error","-i",path,"-f","rawvideo","-pix_fmt","gray","-vf","scale=360:240","-"],capture_output=True).stdout
n=len(raw)//(w*h); fr=np.frombuffer(raw[:n*w*h],np.uint8).reshape(n,h,w).astype(np.int16)
d=np.abs(np.diff(fr,axis=0))>12; tog=d.sum(axis=0)
lo=fr.min(axis=0); hi=fr.max(axis=0)
beacon=(lo<24)&(hi>200)&(tog>50)
def dil(m,k):
    o=m.copy()
    for dy in range(-k,k+1):
        for dx in range(-k,k+1):
            o|=np.roll(np.roll(m,dy,0),dx,1)
    return o
beacon=dil(beacon,4)
mask=np.ones((h,w),bool); mask[beacon]=False
mask[:10,:]=False; mask[-10:,:]=False; mask[:,:56]=False; mask[:,-56:]=False
mask[95:235,140:225]=False   # Lara
r=(tog>=8)&mask
print("%s: WORLD racing px=%d   (beacon masked=%d px)   >=16:%d  >=32:%d  >=100:%d"%(path.split('/')[-2],r.sum(),beacon.sum(),((tog>=16)&mask).sum(),((tog>=32)&mask).sum(),((tog>=100)&mask).sum()))
if out:
    base=fr.mean(axis=0).astype(np.uint8); rgb=np.stack([base]*3,-1)
    rgb[~mask]=(rgb[~mask]*0.4).astype(np.uint8); rgb[r]=[255,0,0]
    Image.fromarray(rgb).resize((720,480),Image.NEAREST).save(out)
