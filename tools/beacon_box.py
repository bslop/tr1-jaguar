#!/usr/bin/env python3
"""beacon_box.py <capture.mkv> [x0 y0 x1 y1] - FPSBEACON fps from a KNOWN box.
WHY (2026-08-26): at VRESN=80 the beacon block lands at capture (168..208,
150..185) in 720x480.  fps_measure.py's variance LOCATOR locked onto a single
noisy edge pixel (x=84,y=3) and read 6.48 against a true 7.00, and
beacon_fps_fast.py hard-codes the 240-line geometry and reports "not found".
This one samples the block you name, checks the signal is BIMODAL (0/255 -
if it is not, you are not on the beacon), and prints per-5s windows so a
drift or a stall shows.  Default box = the VRESN=80 ship layout.
"""
import subprocess, sys, numpy as np
path=sys.argv[1]; x0,y0,x1,y1=(int(v) for v in sys.argv[2:6]) if len(sys.argv)>5 else (168,150,208,185)
W,H=720,480
raw=subprocess.run(["ffmpeg","-v","error","-i",path,"-f","rawvideo","-pix_fmt","gray","-"],capture_output=True).stdout
n=len(raw)//(W*H); fr=np.frombuffer(raw[:n*W*H],np.uint8).reshape(n,H,W)
sig=fr[:,y0:y1,x0:x1].mean(axis=(1,2))
lo,hi=np.percentile(sig,5),np.percentile(sig,95); mid=(lo+hi)/2
state=sig>mid; trans=int(np.count_nonzero(state[1:]!=state[:-1]))
secs=n/60.0
print("frames=%d (%.1fs)  box mean range %.1f..%.1f (p5..p95)  transitions=%d  fps=%.3f"%(n,secs,lo,hi,trans,trans/secs))
# bimodality: fraction of samples within 15% of either extreme
span=hi-lo; near=np.mean((np.abs(sig-lo)<0.15*span)|(np.abs(sig-hi)<0.15*span)); print("bimodal fraction=%.2f (want >0.9)"%near)
# per-5s windows
for k in range(0,n,300):
    s=state[k:k+300]; print("  %2d-%2ds: %.2f fps"%(k//60,(k+300)//60, np.count_nonzero(s[1:]!=s[:-1])/(len(s)/60.0)))
