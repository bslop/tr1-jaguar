#!/usr/bin/env python3
# STATICS CAMPAIGN census: parse LEVEL1.PSX room static placements + global
# staticMeshes table + mesh sizes; simulate per-room budget impact with the
# standard ship env (MRT_ROOMS=64 SUBDIV_MAX=6144 TEXSCALE=2 RAMP_PAL=1).
# Read-only: prints a report, writes nothing to the tree.
import struct, math, os
from collections import deque

LEVEL = "/home/jvilla/Documents/Git/jag_openlara/tr1_psx/extracted/PSXDATA/LEVEL1.PSX"
TILE_PAGE_BYTES = 256*256//2
CLUT_BYTES = 16*2
NUM_TILES = 13
NUM_CLUTS = 1024
MAX_ROOMS = 64
SUBDIV_MAX = 6144
VERT_BUDGET = 500
TEXSCALE = 2

class R:
    def __init__(s,d): s.d=d; s.p=0
    def setpos(s,p): s.p=p
    def seek(s,n): s.p+=n
    def u8(s):  v=s.d[s.p]; s.p+=1; return v
    def u16(s): v=struct.unpack_from("<H",s.d,s.p)[0]; s.p+=2; return v
    def s16(s): v=struct.unpack_from("<h",s.d,s.p)[0]; s.p+=2; return v
    def u32(s): v=struct.unpack_from("<I",s.d,s.p)[0]; s.p+=4; return v
    def s32(s): v=struct.unpack_from("<i",s.d,s.p)[0]; s.p+=4; return v

def _u16(d,o): return struct.unpack_from("<H",d,o)[0]
def _s16(d,o): return struct.unpack_from("<h",d,o)[0]
def _u32(d,o): return struct.unpack_from("<I",d,o)[0]
def _s32(d,o): return struct.unpack_from("<i",d,o)[0]

def read_all_rooms(r):
    r.seek(4)
    nrooms = r.u16()
    rooms = []
    for ri in range(nrooms):
        ix=r.s32(); iz=r.s32(); yb=r.s32(); yt=r.s32()
        size=r.u32(); start=r.p; r.seek(2)
        vc=r.s16(); verts=[]
        for _ in range(vc):
            x=r.s16(); y=r.s16(); z=r.s16(); light=r.u16()
            verts.append((x,y,z,light))
        rc=r.s16(); quads=[]
        for _ in range(rc):
            v=[r.u16(),r.u16(),r.u16(),r.u16()]; fl=r.u16()
            v[2],v[3]=v[3],v[2]
            quads.append(dict(v=v, tex=fl&0x7FFF))
        tc=r.s16(); tris=[]
        for _ in range(tc):
            v=[r.u16(),r.u16(),r.u16()]; fl=r.u16()
            tris.append(dict(v=v, tex=fl&0x7FFF))
        r.setpos(start+size*2)
        npor=r.u16(); ports=[]
        for _ in range(npor):
            adj=r.u16(); r.seek(6)
            for _ in range(4): r.s16(); r.s16(); r.s16()
            ports.append(adj)
        zS=r.u16(); xS=r.u16(); sect=[]
        for _ in range(zS*xS):
            fidx=r.u16(); r.u16()
            below=r.u8(); floor=struct.unpack("b",bytes([r.u8()]))[0]
            above=r.u8(); ceil=struct.unpack("b",bytes([r.u8()]))[0]
            sect.append((floor,ceil,below,above))
        r.seek(2)                 # ambient
        r.seek(r.u16()*20)        # lights
        nmesh=r.u16(); statics=[] # STATIC PLACEMENTS (the campaign target)
        for _ in range(nmesh):
            x=r.s32(); y=r.s32(); z=r.s32()
            rot=r.s16(); inten=r.u16(); mid=r.u16(); r.u16()  # pad
            statics.append(dict(x=x,y=y,z=z,rot=rot,inten=inten,mid=mid))
        alt=r.s16(); rflags=r.u16()
        rooms.append(dict(i=ri, info_x=ix, info_z=iz, yb=yb, yt=yt,
                          verts=verts, quads=quads, tris=tris, ports=ports,
                          xS=xS, zS=zS, sect=sect, statics=statics,
                          alt=alt, water=bool(rflags&1)))
    return rooms, nrooms

data=open(LEVEL,"rb").read()
r=R(data)
r.u32(); r.u32(); r.seek(8)
off=r.u32(); tiles_off=off+8
cluts_off=tiles_off+NUM_TILES*TILE_PAGE_BYTES
r.setpos(cluts_off+NUM_CLUTS*CLUT_BYTES)
rooms_all, nrooms = read_all_rooms(r)
print("== FILE: total rooms = %d" % nrooms)

# ---- data arrays walk (same as extractor) ----
nfloor=r.u32(); r.seek(nfloor*2)
mds=r.u32(); pMeshData=r.p; r.seek(mds*2)
moc=r.u32(); pMeshOff=r.p;  r.seek(moc*4)
an=r.u32();  pAnims=r.p;    r.seek(an*32)
r.seek(r.u32()*6); r.seek(r.u32()*8); r.seek(r.u32()*2)
nds=r.u32(); pNodes=r.p;   r.seek(nds*4)
fds=r.u32(); pFrame=r.p;   r.seek(fds*2)
mc=r.u32();  pModels=r.p;  r.seek(mc*20)
nstat=r.u32(); pStatics=r.p; r.seek(nstat*32)   # GLOBAL staticMeshes table
objCount=r.u32()
assert 1<=objCount<=8000, objCount
pObjtex=r.p
print("== GLOBAL staticMeshes table: %d records; objCount=%d" % (nstat,objCount))

statics_tab={}   # id -> dict
for i in range(nstat):
    o=pStatics+i*32
    sid=_u32(data,o); mesh=_u16(data,o+4)
    vbox=[_s16(data,o+6+j*2) for j in range(6)]
    cbox=[_s16(data,o+18+j*2) for j in range(6)]
    fl=_u16(data,o+30)
    statics_tab[sid]=dict(mesh=mesh,vbox=vbox,cbox=cbox,flags=fl)

objtex=[]
p=pObjtex
for i in range(objCount):
    x0=data[p];y0=data[p+1];clut=_u16(data,p+2);x1=data[p+4];y1=data[p+5]
    tile=_u16(data,p+6)&0x3FFF;x2=data[p+8];y2=data[p+9];x3=data[p+12];y3=data[p+13]
    objtex.append(dict(tile=tile,clut=clut,uv=[(x0,y0),(x1,y1),(x2,y2),(x3,y3)]))
    p+=16

def parse_mesh(midx):
    boff=_u32(data, pMeshOff+midx*4); base=pMeshData+boff
    cx=_s16(data,base); cy=_s16(data,base+2); cz=_s16(data,base+4)
    rad=_s16(data,base+6); fl=_u16(data,base+8)
    vCount=_s16(data, base+10); vAbs=abs(vCount); p=base+12
    verts=[]
    for j in range(vAbs):
        verts.append((_s16(data,p),_s16(data,p+2),_s16(data,p+4))); p+=8
    p += vAbs*8 if vCount>0 else vAbs*2
    rCount=_u16(data,p); p+=2
    quads=[]
    for q in range(rCount):
        v=[_u16(data,p),_u16(data,p+2),_u16(data,p+4),_u16(data,p+6)]
        flq=_u16(data,p+8); p+=10
        quads.append((v,flq&0x7FFF))
    tCount=_u16(data,p); p+=2
    tris=[]
    for t in range(tCount):
        v=[_u16(data,p),_u16(data,p+2),_u16(data,p+4)]
        flt=_u16(data,p+6); p+=8
        tris.append((v,flt&0x7FFF))
    return dict(verts=verts,quads=quads,tris=tris)

# ---- BFS room set from room 0, MAX_ROOMS=64 (ship env) ----
seen=set([0]); order=[0]; dq=deque([0])
while dq and len(order)<MAX_ROOMS:
    c=dq.popleft()
    for a in rooms_all[c]['ports']:
        if a not in seen and a<nrooms:
            seen.add(a); order.append(a); dq.append(a)
order=order[:MAX_ROOMS]
missing=[i for i in range(nrooms) if i not in seen]
print("== BFS from room 0 reaches %d rooms: %s" % (len(order), order))
if missing:
    print("== UNREACHED rooms: %s" % missing)
    for mi in missing:
        rm=rooms_all[mi]
        # is it an alternate of a reached room?
        alts=[j for j in range(nrooms) if rooms_all[j]['alt']==mi]
        print("   room %d: %dv %dq %dt, alt-of=%s its-alt=%d water=%s" %
              (mi,len(rm['verts']),len(rm['quads']),len(rm['tris']),alts,rm['alt'],rm['water']))

# ---- placements census ----
print("\n== STATIC PLACEMENTS per room (all rooms in file) ==")
tot=0; used_mids=set(); rotvals=set()
for rm in rooms_all:
    if rm['statics']:
        descr=[]
        for s in rm['statics']:
            used_mids.add(s['mid']); rotvals.add(s['rot']&0xFFFF); tot+=1
            descr.append("id%d@rot%d,int%d"%(s['mid'],(s['rot']&0xFFFF)>>8,s['inten']))
        print("  room %2d (%s): %d statics  %s" %
              (rm['i'], "IN-SET" if rm['i'] in seen else "unreached",
               len(rm['statics']), " ".join(descr)))
print("TOTAL placements: %d  distinct staticIDs: %s" % (tot, sorted(used_mids)))
print("rotation raw values used: %s (16384=90deg)" % sorted(rotvals))

# ---- mesh census per used staticID ----
print("\n== STATIC MESH census (per used staticID) ==")
mesh_of={}
for sid in sorted(used_mids):
    st=statics_tab.get(sid)
    if st is None:
        print("  staticID %d: NOT IN GLOBAL TABLE!"%sid); continue
    m=parse_mesh(st['mesh'])
    mesh_of[sid]=m
    texs=set(t for _,t in m['quads'])|set(t for _,t in m['tris'])
    ntex=len([t for t in texs if t>=256]); ncol=len([t for t in texs if t<256])
    xs=[v[0] for v in m['verts']]; ys=[v[1] for v in m['verts']]; zs=[v[2] for v in m['verts']]
    print("  staticID %2d -> mesh %3d: %3dv %3dq %3dt  tex:%d col:%d  bbox X[%d..%d] Y[%d..%d] Z[%d..%d] flags=%04x" %
          (sid, st['mesh'], len(m['verts']), len(m['quads']), len(m['tris']),
           ntex, ncol, min(xs),max(xs),min(ys),max(ys),min(zs),max(zs), st['flags']))

# ---- per-room budget simulation (subdivision replicated, dummy UVs) ----
def subdiv_counts(verts, quads, tris):
    """Replicate _subdivide's split decisions (extent/SUBDIV_MAX/VERT_BUDGET).
    verts: list of (x,y,z,light). quads/tris: dicts with 'v'. Returns v,q,t."""
    verts=list(verts)
    def extent(f):
        vs=[verts[i] for i in f['v']]
        xs=[v[0] for v in vs]; ys=[v[1] for v in vs]; zs=[v[2] for v in vs]
        return max(max(xs)-min(xs), max(ys)-min(ys), max(zs)-min(zs))
    def midvert(a,b):
        va,vb=verts[a],verts[b]
        verts.append(((va[0]+vb[0])//2,(va[1]+vb[1])//2,(va[2]+vb[2])//2,
                      (va[3]+vb[3])//2))
        return len(verts)-1
    outq=[]; work=[dict(v=list(f['v'])) for f in quads]
    while work:
        f=work.pop()
        if extent(f)<=SUBDIV_MAX or len(verts)>=VERT_BUDGET:
            outq.append(f); continue
        v=f['v']
        def span(i,j):
            a,b=verts[v[i]],verts[v[j]]
            return abs(a[0]-b[0])+abs(a[1]-b[1])+abs(a[2]-b[2])
        if span(0,1)+span(3,2) >= span(1,2)+span(0,3):
            mA=midvert(v[0],v[1]); mB=midvert(v[3],v[2])
            work.append(dict(v=[v[0],mA,mB,v[3]]))
            work.append(dict(v=[mA,v[1],v[2],mB]))
        else:
            mA=midvert(v[1],v[2]); mB=midvert(v[0],v[3])
            work.append(dict(v=[v[0],v[1],mA,mB]))
            work.append(dict(v=[mB,mA,v[2],v[3]]))
    outt=[]; work=[dict(v=list(f['v'])) for f in tris]
    while work:
        f=work.pop()
        if extent(f)<=SUBDIV_MAX or len(verts)>=VERT_BUDGET:
            outt.append(f); continue
        v=f['v']
        def span2(i,j):
            a,b=verts[v[i]],verts[v[j]]
            return abs(a[0]-b[0])+abs(a[1]-b[1])+abs(a[2]-b[2])
        e=max(((0,1),(1,2),(2,0)),key=lambda ij:span2(*ij))
        i,j=e; k=3-i-j
        m=midvert(v[i],v[j])
        outt.append if False else None
        work.append(dict(v=[v[i],m,v[k]]))
        work.append(dict(v=[m,v[j],v[k]]))
    return len(verts), len(outq), len(outt)

def statics_localized(rm):
    """Transform each placement's mesh into room-local verts/faces."""
    sv=[]; sq=[]; st_=[]
    warn=[]
    for s in rm['statics']:
        tab=statics_tab.get(s['mid'])
        if tab is None: continue
        m=mesh_of[s['mid']]
        lx=s['x']-rm['info_x']; ly=s['y']; lz=s['z']-rm['info_z']
        ang=((s['rot']&0xFFFF)>>6)/1024.0*2*math.pi
        c=math.cos(ang); sn=math.sin(ang)
        inten=s['inten']
        light=255 if inten>0x1FFF else max(0,min(255,255-(inten>>5)))
        vb=len(sv)
        for (x,y,z) in m['verts']:
            wx=lx + x*c + z*sn
            wy=ly + y
            wz=lz - x*sn + z*c
            for nm,vv in (("x",wx),("y",wy),("z",wz)):
                if not (-32768<=vv<=32767): warn.append("s16 OVERFLOW %s=%d id%d"%(nm,vv,s['mid']))
            sv.append((int(round(wx)),int(round(wy)),int(round(wz)),light))
        for (v,t) in m['quads']: sq.append(dict(v=[vb+i for i in v],tex=t))
        for (v,t) in m['tris']:  st_.append(dict(v=[vb+i for i in v],tex=t))
    return sv,sq,st_,warn

print("\n== PER-ROOM BUDGET IMPACT (post-subdiv, ship env SUBDIV_MAX=%d VERT_BUDGET=%d) ==" % (SUBDIV_MAX,VERT_BUDGET))
print("%-5s %-22s %-22s %s" % ("room","base v/q/t","with-statics v/q/t","notes"))
busts=[]
tot_dv=tot_dq=tot_dt=0
for gi in order:
    rm=rooms_all[gi]
    bv,bq,bt=subdiv_counts(rm['verts'],rm['quads'],rm['tris'])
    sv,sq,st_,warn=statics_localized(rm)
    if not rm['statics']:
        continue
    nverts=list(rm['verts'])
    voff=len(nverts)
    nverts+= [v for v in sv]
    nq=list(rm['quads'])+[dict(v=[voff+i for i in f['v']]) for f in sq]
    nt=list(rm['tris'])+[dict(v=[voff+i for i in f['v']]) for f in st_]
    wv,wq,wt=subdiv_counts(nverts,nq,nt)
    tot_dv+=wv-bv; tot_dq+=wq-bq; tot_dt+=wt-bt
    note=""
    base_pre=len(rm['verts']); with_pre=len(nverts)
    if with_pre>500: note+=" PRE-SUBDIV VERTS %d>500!"%with_pre
    if wv>512: note+=" KERNEL CAP BUST %d>512!"%wv
    elif wv>=500: note+=" at-vert-budget(subdiv starved)"
    if wq>448: note+=" q>448"
    if wt>256: note+=" t>256"
    for w in warn: note+=" "+w
    print("  %3d  %4d/%3d/%3d        %4d/%3d/%3d        %d statics%s" %
          (gi, bv,bq,bt, wv,wq,wt, len(rm['statics']), note))
    if "!" in note: busts.append(gi)
print("TOTAL deltas across set: verts +%d quads +%d tris +%d" % (tot_dv,tot_dq,tot_dt))
print("BUSTS: %s" % (busts if busts else "none"))

# ---- atlas growth estimate ----
# current room+lara texture set vs statics additions; statics get ts=TEXSCALE.
used_room=set()
for gi in order:
    rm=rooms_all[gi]
    for q in rm['quads']: used_room.add(q['tex'])
    for t in rm['tris']:  used_room.add(t['tex'])
new_keys={}   # (tile,clut,umin,vmin,w,h) -> px area (after TEXSCALE)
ncol_new=set()
stat_q_tex=set(); stat_t_tex=set()
for gi in order:
    rm=rooms_all[gi]
    for s in rm['statics']:
        if s['mid'] not in mesh_of: continue
        m=mesh_of[s['mid']]
        for (_,t) in m['quads']:
            if t<256: ncol_new.add(t)
            else: stat_q_tex.add(t)
        for (_,t) in m['tris']:
            if t<256: ncol_new.add(t)
            else: stat_t_tex.add(t)
newtex=(stat_q_tex|stat_t_tex)-used_room
area4=0; area_true=0
for t in sorted(newtex):
    o=objtex[t]
    uv=[(x//TEXSCALE,y//TEXSCALE) for (x,y) in o['uv']]
    nc = 4 if t in stat_q_tex else 3
    for label,n in (("4corner",4),("true",nc)):
        us=[p[0] for p in uv[:n]]; vs=[p[1] for p in uv[:n]]
        w=max(us)-min(us)+1; h=max(vs)-min(vs)+1
        if label=="4corner": area4+=w*h
        else: area_true+=w*h
print("\n== ATLAS GROWTH (statics textures) ==")
print("statics tex indices: %d total, %d NEW vs room set (%d shared)" %
      (len(stat_q_tex|stat_t_tex), len(newtex), len((stat_q_tex|stat_t_tex)&used_room)))
print("new-tile area at TEXSCALE=%d: %d px true-corner (%.1f KB, ~%d atlas rows)"
      % (TEXSCALE, area_true, area_true/1024.0, (area_true+255)//256))
print("               (legacy 4-corner bbox would be %d px = %.1f KB)" % (area4, area4/1024.0))
print("colored (tex<256) indices used by statics: %s" % sorted(ncol_new))
