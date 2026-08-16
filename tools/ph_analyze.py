#!/usr/bin/env python3
import json, sys, statistics
import os
from collections import Counter
S=os.environ.get('SCRATCH','scratch')
SY=json.load(open(S+'/ph_kernel_syms.json'))
code=sorted((v,n) for n,v in SY.items() if 0xf03000<=v<0xf03e60)
def kreg(pc):
    if not (0xf03000<=pc<0xf03e60): return "OUT_%x"%pc
    lo="?"
    for v,n in code:
        if v<=pc: lo=n
        else: break
    return lo
def load_nm(f):
    out=[]
    for line in open(f):
        p=line.split()
        if len(p)==3: out.append((int(p[0],16),p[2]))
    out.sort(); return out
def sym(nm,pc):
    lo="?"
    for v,n in nm:
        if v<=pc: lo=n
        else: break
    return lo
TICKS_PER_FRAME=443657  # 26.59MHz / 59.94Hz
for sc,nmf in (('spawn','ph_spawn_nm.txt'),('r12','ph_r12_nm.txt'),('drive','ph_r12_nm.txt')):
    nm=load_nm(S+'/'+nmf)
    d=json.load(open(f'{S}/ph_scene2_{sc}.json'))
    s0,s1=d['s0'],d['s1']; g0,g1=s0['gpu'],s1['gpu']
    cyc=g1['cycles']-g0['cycles']; ins=g1['instret']-g0['instret']
    fr=s1['frame']-s0['frame']; wall=fr*TICKS_PER_FRAME
    print(f"=== {sc}: {fr} frames | GPU busy {100*cyc/wall:.1f}% of wall, IPC {ins/max(cyc,1):.3f}")
    t0,t1=g0['timing'],g1['timing']
    items=[(k,t1[k]-t0[k]) for k in t1 if t1[k]-t0[k]>0]
    for k,dv in sorted(items,key=lambda kv:-kv[1]):
        print(f"   {k:16s} {dv:>12,d} {100*dv/cyc:5.2f}%gpu {100*dv/wall:5.2f}%wall")
    n=len(d['pcs'])
    c=Counter(kreg(int(p,16)) for p in d['pcs'])
    print(f"   GPU PC samples ({n}):", ", ".join(f"{k}:{100*v/n:.1f}%" for k,v in c.most_common(14)))
    halt=c.get('halt',0)/n; stopped=1-cyc/wall
    print(f"   -> GPU truly-stopped {100*stopped:.1f}% + halt-spin {100*halt*cyc/wall:.1f}% = 68k-serial ~{100*(stopped+halt*cyc/wall):.1f}% of wall")
    cc=Counter(sym(nm,int(p,16)) for p in d['cpcs'])
    print(f"   68K PC samples ({n}):", ", ".join(f"{k}:{100*v/n:.1f}%" for k,v in cc.most_common(12)))
    hits=d['hits']; rend=sorted(set(h['frame'] for h in hits))
    # render start = first of the write pair: collapse pairs closer than 3 frames
    starts=[]
    for f2 in rend:
        if not starts or f2-starts[-1]>3: starts.append(f2)
    gaps=[b-a for a,b in zip(starts,starts[1:])]
    if gaps:
        sg=sorted(gaps)
        print(f"   renders {len(starts)} | frame-time(fields): mean {statistics.mean(gaps):.2f} median {statistics.median(gaps)} p90 {sg[int(len(sg)*0.9)]} max {max(gaps)} min {min(gaps)}")
        print(f"   gaps histogram: {dict(sorted(Counter(gaps).items()))}")
    print()
