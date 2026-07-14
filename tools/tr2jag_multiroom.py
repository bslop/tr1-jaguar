#!/usr/bin/env python3
# tr2jag_multiroom.py
#
# HOST tool: extract a CONNECTED SET of textured rooms from PSX Tomb Raider 1
# (LEVEL1.PSX) into ONE shared 8bpp atlas + per-room geometry/sector blobs, so
# the Jaguar can render several portal-connected rooms (Lara walks between them).
#
# This GENERALISES tr2jag_room.py (single room 0) to N rooms:
#   - parse a BFS-connected set of rooms from room 0
#   - union all their object-textures into ONE shelf-packed atlas + palette
#     (+ the same Lara flat-shade swatch strip as tr2jag_room.py)
#   - emit each room as a room0_tex-format geometry blob (its own offX/offZ)
#   - emit each room's sectors (room0_sect format) for multi-room collision
#
# OUTPUTS (big-endian) into src/platform/jaguar/:
#   mrt_atlas.bin  shared 8bpp atlas (row-major, width 256) + Lara swatch
#   mrt_pal.bin    256 * u16 RGB16 (Jaguar fb fmt) incl. Lara tones
#   mrt_geom.bin   concatenated per-room geometry blobs (room0_tex format, 8-aligned)
#   mrt_sect.bin   concatenated per-room sector blobs (room0_sect format, 8-aligned)
#   mrt.bin        index: u16 roomCount,atlasW,atlasH,pad ; roomCount*{u32 geom_off, sect_off}
#   mrt.h          #defines (room count, atlas dims, Lara swatch cells)
#
# Room geom blob (room0_tex format, per tr2jag_room.py):
#   HEADER 16B: u16 vcount,qcount,tcount,atlasW,atlasH ; s16 offX,offY,offZ
#   VERTS  vcount*8 : s16 x,y,z ; u16 shade
#   QUADS  qcount*24: u16 v0..v3 ; u16 u0,v0,u1,v1,u2,v2,u3,v3
#   TRIS   tcount*18: u16 v0,v1,v2 ; u16 u0,v0,u1,v1,u2,v2
#   world = local + (off<<8); Y absolute (offY=0).
# Sector blob (room0_sect format): u16 xS,zS ; s32 info_x,info_z ; xS*zS*{s16 floorY,ceilY}

import struct, sys, os, zlib
from collections import Counter, deque

_REPO   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVEL   = os.environ.get("TRLEVEL",
          os.path.join(_REPO, "assets/extracted/PSXDATA/LEVEL1.PSX"))
OUTDIR  = _REPO
PREFIX  = os.environ.get("TRPREFIX", "mrt")   # output file prefix (mrt / gym)

TILE_PAGE_BYTES = 256*256//2
CLUT_BYTES      = 16*2
NUM_TILES       = 13
NUM_CLUTS       = 1024
MAX_ROOMS       = int(os.environ.get("MRT_ROOMS", "5"))   # BFS depth cap

class R:
    def __init__(s,d): s.d=d; s.p=0
    def setpos(s,p): s.p=p
    def seek(s,n): s.p+=n
    def u8(s):  v=s.d[s.p]; s.p+=1; return v
    def u16(s): v=struct.unpack_from("<H",s.d,s.p)[0]; s.p+=2; return v
    def s16(s): v=struct.unpack_from("<h",s.d,s.p)[0]; s.p+=2; return v
    def u32(s): v=struct.unpack_from("<I",s.d,s.p)[0]; s.p+=4; return v
    def s32(s): v=struct.unpack_from("<i",s.d,s.p)[0]; s.p+=4; return v

def read_all_rooms(r):
    """Parse every room (geometry + portals + sectors + info). Leaves r after
    the last room. Returns list of dicts."""
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
            v[2],v[3]=v[3],v[2]                        # PSX quad swap
            quads.append(dict(v=v, tex=fl&0x7FFF, swapped=True))
        tc=r.s16(); tris=[]
        for _ in range(tc):
            v=[r.u16(),r.u16(),r.u16()]; fl=r.u16()
            tris.append(dict(v=v, tex=fl&0x7FFF))
        r.setpos(start+size*2)
        # trailer: portals, sectors, ...
        npor=r.u16(); ports=[]; portverts=[]
        for _ in range(npor):
            adj=r.u16(); r.seek(6)          # skip normal
            pv=[(r.s16(),r.s16(),r.s16()) for _ in range(4)]
            ports.append(adj); portverts.append(pv)
        zS=r.u16(); xS=r.u16(); sect=[]
        for _ in range(zS*xS):
            fidx=r.u16(); r.u16()          # floorIndex (into FloorData), boxIndex
            below=r.u8(); floor=struct.unpack("b",bytes([r.u8()]))[0]
            above=r.u8(); ceil=struct.unpack("b",bytes([r.u8()]))[0]
            sect.append((floor,ceil,fidx,below,above))
        r.seek(2); r.seek(r.u16()*20); r.seek(r.u16()*20)
        alt=r.u16(); rflags=r.u16()        # alternateRoom, flags (bit0=WATER)
        rooms.append(dict(i=ri, info_x=ix, info_z=iz, verts=verts, quads=quads,
                          tris=tris, ports=ports, portverts=portverts,
                          xS=xS, zS=zS, sect=sect, water=bool(rflags & 1)))
    return rooms, nrooms

# =======================================================================
#  LARA POSING (ported from tr2jag.cpp buildLaraMesh -> drawNodes +
#  matrixFrame + DECODE_ANGLES, adapted to TR1_PSX little-endian).
#  Byte layouts (all VERIFIED against format.h loadTR1_PSX and the file):
#   Model (20B) : u16 type@0, u16 idx@2, u16 mCount@4, u16 mStart@6,
#                 u32 node@8 (WORD index into nodesData u32[]),
#                 u32 frame@12, u16 anim@16, u16 pad@18.
#   Mesh header (12B): s16 cx,cy,cz ; s16 radius ; u16 flags ; s16 vCount.
#                 then vCount * short4(8B: x,y,z,pad) COORDS,
#                 then (vCount>0 ? vCount*short4(8B) normals : vCount*s16 intensity),
#                 then u16 rCount ; rCount * (u16 v0,v1,v2,v3 ; u16 flags),
#                 then u16 tCount ; tCount * (u16 v0,v1,v2 ; u16 flags).
#                 VERTS ARE ALREADY WORLD UNITS on PSX (NO /4 shift -- proven
#                 by baked-frame bbox matching the frame's own stored bbox).
#   Face flags  : texture = flags & 0x7FFF ; colored if texture < 256
#                 (colored -> palette index = flags & 0xFF).
#   Anim (32B)  : u32 frameOffset@0, u8 rate@4, u8 frameSize@5 (0 here ->
#                 compute 10+2*mCount words), u16 state@6 (run = state 1).
#   Frame       : MinMax box(12B) ; short3 pos(6B) ; u16 angles[] where
#                 angles[0] is skipped and joint j uses angles[2j+1],angles[2j+2].
#                 frameSize = 10+2*mCount words = 20+4*mCount bytes.
#   Node (16B)  : u32 flags (bit0=POP, bit1=PUSH) ; s32 x,y,z.
# =======================================================================
import math as _math

def _u16(d,o): return struct.unpack_from("<H",d,o)[0]
def _s16(d,o): return struct.unpack_from("<h",d,o)[0]
def _u32(d,o): return struct.unpack_from("<I",d,o)[0]
def _s32(d,o): return struct.unpack_from("<i",d,o)[0]

def _mat_id():        return [[1.0,0,0],[0,1.0,0],[0,0,1.0]],[0.0,0.0,0.0]
def _mat_clone(m):    R,t=m; return [row[:] for row in R], t[:]
def _translate_rel(m,x,y,z):
    R,t=m
    for i in range(3): t[i]+=R[i][0]*x+R[i][1]*y+R[i][2]*z
def _rot_axis(m,ia,ib,ang):
    R,t=m; c=_math.cos(ang); s=_math.sin(ang)
    for i in range(3):
        a=R[i][ia]; b=R[i][ib]; R[i][ia]=a*c-b*s; R[i][ib]=b*c+a*s
def _rot_yxz(m,ax,ay,az):       # matrixRotateYXZ_c: Y then X then Z
    K=2.0*_math.pi/1024.0
    if ay: _rot_axis(m,0,2,ay*K)
    if ax: _rot_axis(m,2,1,ax*K)
    if az: _rot_axis(m,1,0,az*K)
def _decode_angles(w0,w1):      # TR1 PSX: pairs stored SWAPPED vs PC; the
    # +1-word skip covers the frame's extra leading word (frameSize=10+2*mc
    # words = 1 more than PC's 9+2*mc). VALIDATED: baked frame-0 bbox matches
    # the frame's stored box to +-2u with THIS scheme only (2026-07-12 retest).
    a=w1; b=w0
    ax=(a & 0x3FF0)>>4
    ay=((a & 0x000F)<<6)|((b & 0xFC00)>>10)
    az=b & 0x03FF
    return ax,ay,az
def _clampi16(v):
    v=int(round(v)); return -32768 if v<-32768 else (32767 if v>32767 else v)

def build_lara(data, pMeshData, pMeshOff, pAnims, pNodes, pFrame,
               pModels, modelsCount, objCount):
    # locate Lara: first model with type == 0 (ITEM_LARA)
    laraModel=-1
    for i in range(modelsCount):
        if _u16(data, pModels+i*20) == 0: laraModel=i; break
    if laraModel<0: raise SystemExit("!! Lara model (type 0) not found")
    mo=pModels+laraModel*20
    mcount=_u16(data,mo+4); mstart=_u16(data,mo+6)
    node  =_u32(data,mo+8); anim  =_u16(data,mo+16)

    # --- per-mesh: verts + faces (topology constant across frames) ---
    meshes=[]; quads=[]; tris=[]; vbase_of=[]; total_v=0
    col_indices=set(); colored_faces=0
    for i in range(mcount):
        boff=_u32(data, pMeshOff+(mstart+i)*4); base=pMeshData+boff
        vCount=_s16(data, base+10); vAbs=abs(vCount); p=base+12
        verts=[]
        for j in range(vAbs):
            x=_s16(data,p); y=_s16(data,p+2); z=_s16(data,p+4); p+=8
            verts.append((x,y,z))
        p += vAbs*8 if vCount>0 else vAbs*2          # skip normals / intensity
        rCount=_u16(data,p); p+=2
        vb=total_v
        for q in range(rCount):
            v=[_u16(data,p),_u16(data,p+2),_u16(data,p+4),_u16(data,p+6)]
            fl=_u16(data,p+8); p+=10
            tex=fl&0x7FFF; col=tex<256
            if col: col_indices.add(tex); colored_faces+=1
            quads.append(dict(v=[vb+v[0],vb+v[1],vb+v[2],vb+v[3]], tex=tex, colored=col))
        tCount=_u16(data,p); p+=2
        for t in range(tCount):
            v=[_u16(data,p),_u16(data,p+2),_u16(data,p+4)]
            fl=_u16(data,p+6); p+=8
            tex=fl&0x7FFF; col=tex<256
            if col: col_indices.add(tex); colored_faces+=1
            tris.append(dict(v=[vb+v[0],vb+v[1],vb+v[2]], tex=tex, colored=col))
        meshes.append(verts); vbase_of.append(vb); total_v+=vAbs

    # --- animations: RUN cycle + a STAND idle pose (appended as LAST frame) ---
    def anim_info(ai):
        ao=pAnims+ai*32
        fofs=_u32(data,ao); fs=data[ao+5]
        fw=fs if fs>0 else (10+2*mcount)
        return fofs, fw*2, _u16(data,ao+6)          # frameOffset, frameBytes, state
    # one posed frame: rootX/Y/Z held constant (in-place cycle), joints per frame
    def bake(frameOffset, fbytes, rootX, rootY, rootZ, fidx):
        fb=pFrame+frameOffset+fidx*fbytes
        ab=fb+18                                    # angles[] byte start
        def angword(joint):
            o=ab+(2*joint+1)*2
            return _u16(data,o), _u16(data,o+2)
        out=[]; m=_mat_id(); stack=[]
        def emit(m, mi):
            R,t=m
            for (lx,ly,lz) in meshes[mi]:           # PSX verts already world units
                out.append((_clampi16(t[0]+R[0][0]*lx+R[0][1]*ly+R[0][2]*lz),
                            _clampi16(t[1]+R[1][0]*lx+R[1][1]*ly+R[1][2]*lz),
                            _clampi16(t[2]+R[2][0]*lx+R[2][1]*ly+R[2][2]*lz)))
        _translate_rel(m, rootX, rootY, rootZ)
        w0,w1=angword(0); ax,ay,az=_decode_angles(w0,w1); _rot_yxz(m,ax,ay,az)
        emit(m, 0)
        for i in range(1,mcount):
            nb=pNodes+(node+(i-1)*4)*4
            fl=_u32(data,nb); nx=_s32(data,nb+4); ny=_s32(data,nb+8); nz=_s32(data,nb+12)
            if fl&1: m=stack.pop()
            if fl&2: stack.append(_mat_clone(m))
            _translate_rel(m,nx,ny,nz)
            w0,w1=angword(i); ax,ay,az=_decode_angles(w0,w1); _rot_yxz(m,ax,ay,az)
            emit(m, i)
        return out
    def anim_root(fofs):
        b=pFrame+fofs; return _s16(data,b+12),_s16(data,b+14),_s16(data,b+16)
    # RUN cycle (anim = model.animation, state 1)
    runOfs, runFb, _st = anim_info(anim)
    frameSize_words = runFb//2
    nextFofs=_u32(data, pAnims+(anim+1)*32)
    runFrames=(nextFofs-runOfs)//runFb if nextFofs>runOfs else 1
    if runFrames<1: runFrames=1
    frame0_box=[_s16(data,pFrame+runOfs+j*2) for j in range(6)]
    rrx,rry,rrz=anim_root(runOfs)
    frames=[bake(runOfs, runFb, rrx, rry, rrz, f) for f in range(runFrames)]
    # Frame layout after the RUN cycle (0..runFrames-1):
    #   STAND  (state 2 STOP)      -> 1 frame at index runFrames
    #   JUMP   (state 3 FWD-JUMP)  -> the FULL anim cycle (wind-up..land),
    #                                 jumpFrames frames from jumpStart
    runFrames = len(frames)
    def find_state(st):
        for ai in range(anim, anim+150):
            if anim_info(ai)[2]==st: return ai
        return None
    def anim_frames(ai):
        ofs,fb,_=anim_info(ai)
        nxt=_u32(data, pAnims+(ai+1)*32)
        return (nxt-ofs)//fb if nxt>ofs and fb>0 else 0
    def find_state_longest(st):   # the richest animation carrying this state
        best=None; bn=0
        for ai in range(anim, anim+150):
            if anim_info(ai)[2]==st:
                n=anim_frames(ai)
                if n>bn: bn=n; best=ai
        return best
    # diagnostic: which state each of Lara's animations carries + its length
    for ai in range(anim, anim+120):
        ofs,fb,st=anim_info(ai)
        nxt=_u32(data, pAnims+(ai+1)*32); n=(nxt-ofs)//fb if nxt>ofs and fb>0 else 0
        if st in (10,19,54,57):   # HANG, HANG_UP(pull-up), HANDSTAND, CLIMB_UP
            print("  anim %3d  state %2d  frames %d" % (ai, st, n))
    def append_pose(st, fidx):
        a=find_state(st)
        if a is None: frames.append([v for v in frames[0]]); return
        ofs,fb,_=anim_info(a); rx,ry,rz=anim_root(ofs)
        frames.append(bake(ofs, fb, rx, ry, rz, fidx))
    append_pose(2, 0)                 # STAND idle -> index runFrames
    standFrame = runFrames
    # JUMP anims (baked IN PLACE; the engine's arc physics does the motion, the
    # anim supplies the limb poses): FORWARD jump (state 3, running leap) and
    # UP jump (state 28, vertical from a standstill) — two distinct animations.
    def bake_anim(a, cap=28):
        start=len(frames)
        if a is not None:
            ofs,fb,_=anim_info(a); rx,ry,rz=anim_root(ofs)
            nxt=_u32(data, pAnims+(a+1)*32); jn=(nxt-ofs)//fb if nxt>ofs else 1
            jn=max(1,min(jn,cap))
            for f in range(jn): frames.append(bake(ofs, fb, rx, ry, rz, f))
        else:
            frames.append([v for v in frames[0]])
        return start, len(frames)-start
    def bake_cycle(st):
        return bake_anim(find_state(st))
    fjumpStart, fjumpFrames = bake_cycle(3)    # FORWARD jump (running)
    ujumpStart, ujumpFrames = bake_cycle(28)   # UP jump (standing / vertical)
    # CLIMB / pull-up (jump-grab from a hang): richest HANG_UP (19) = anim 97.
    # Capped — fps is low so the runtime subsamples anyway (keeps the blob small).
    climbA = find_state_longest(19) or find_state_longest(57) or find_state(28)
    climbStart, climbFrames = bake_anim(climbA, cap=16)
    print("  climb(hang-up): anim %s -> start %d frames %d" % (climbA, climbStart, climbFrames))
    # STANDING VAULT ("kick-flip" onto a ledge): TR ANIM_CLIMB_2 (abs idx anim+50)
    # — the short hands-plant/leg-kick vault. Baked in-place; the engine lifts her
    # feet while the anim plays. (ANIM_CLIMB_3=anim+42 is 34f — too big to embed.)
    ANIM_CLIMB_2=anim+50
    print("  vault anim %d frames %d" % (ANIM_CLIMB_2, anim_frames(ANIM_CLIMB_2)))
    vault2Start, vault2Frames = bake_anim(ANIM_CLIMB_2, cap=16)
    vault3Start, vault3Frames = vault2Start, vault2Frames   # reuse (no separate 3-click)
    print("  vault start %d f %d" % (vault2Start, vault2Frames))
    jumpStart, jumpFrames = fjumpStart, fjumpFrames   # (compat)
    nframes=len(frames)

    # ================= RUNTIME SKINNING DATA (mrt_lskin) =================
    # Instead of pre-posing every animation frame into vertices (~1800 B/frame),
    # store the skeleton + per-frame joint angles and pose Lara on the 68k. This
    # is ~30x smaller, so ALL of Lara's animations fit. Layout is emitted below.
    # --- node hierarchy for meshes 1..mcount-1 (flags,x,y,z) ---
    nodes=[]
    for i in range(1,mcount):
        nb=pNodes+(node+(i-1)*4)*4
        nodes.append((_u32(data,nb)&3, _s32(data,nb+4), _s32(data,nb+8), _s32(data,nb+12)))
    # --- per-frame joint angles (TR 10-bit -> 8-bit index for the 256 SINTAB) ---
    def frame_angles(fofs, fbytes, fidx):
        fb=pFrame+fofs+fidx*fbytes; ab=fb+18
        root=(_s16(data,fb+12), _s16(data,fb+14), _s16(data,fb+16))
        angs=[]
        for j in range(mcount):
            o=ab+(2*j+1)*2
            ax,ay,az=_decode_angles(_u16(data,o), _u16(data,o+2))
            angs.append(((ax>>2)&255,(ay>>2)&255,(az>>2)&255))
        return root, angs
    # --- how many animations belong to Lara (bounded by the next model) ---
    nextAnim=None
    for i in range(modelsCount):
        a2=_u16(data,pModels+i*20+16)
        if a2>anim and a2!=0xFFFF and (nextAnim is None or a2<nextAnim): nextAnim=a2
    laraAnimCount=(nextAnim-anim) if nextAnim else 32
    # --- collect every Lara animation's frames + an anim table ---
    skin_frames=[]; anim_table=[]
    for rel in range(laraAnimCount):
        ai=anim+rel
        ofs,fbytes,st=anim_info(ai)
        n=anim_frames(ai)
        if n<=0 or n>512: n=1
        rate=data[pAnims+ai*32+4] or 1
        start=len(skin_frames)
        for f in range(n): skin_frames.append(frame_angles(ofs,fbytes,f))
        anim_table.append((start,n,st,rate))
    print("  SKIN: mcount=%d verts=%d nodes=%d anims=%d frames=%d (%.1f KB)" %
          (mcount, total_v, len(nodes), laraAnimCount, len(skin_frames),
           len(skin_frames)*(6+mcount*3)/1024.0))

    tex_faces=[f for f in (quads+tris) if not f['colored']]
    return dict(laraModel=laraModel, mcount=mcount, mstart=mstart, node=node, anim=anim,
                vcount=total_v, quads=quads, tris=tris, frames=frames,
                framecount=nframes, framesize_words=frameSize_words,
                runFrames=runFrames, standFrame=standFrame,
                jumpStart=jumpStart, jumpFrames=jumpFrames,
                fjumpStart=fjumpStart, fjumpFrames=fjumpFrames,
                ujumpStart=ujumpStart, ujumpFrames=ujumpFrames,
                climbStart=climbStart, climbFrames=climbFrames,
                vault2Start=vault2Start, vault2Frames=vault2Frames,
                vault3Start=vault3Start, vault3Frames=vault3Frames,
                col_indices=col_indices, colored_faces=colored_faces,
                tex_faces=tex_faces, frame0_box=frame0_box,
                mesh_verts=meshes, vbase_of=vbase_of, nodes=nodes,
                skin_frames=skin_frames, anim_table=anim_table,
                lara_anim_base=anim, lara_anim_count=laraAnimCount)

def main():
    data=open(LEVEL,"rb").read()
    r=R(data)
    r.u32(); r.u32(); r.seek(8)
    off=r.u32(); tiles_off=off+8
    cluts_off=tiles_off+NUM_TILES*TILE_PAGE_BYTES
    r.setpos(cluts_off+NUM_CLUTS*CLUT_BYTES)
    rooms_all, nrooms = read_all_rooms(r)
    print("total rooms in level:", nrooms)

    # after all rooms -> objectTextures (readDataArrays continues).
    # capture data-array base pointers so we can pose Lara (model 0) below.
    # (all VERIFIED against format.h loadTR1_PSX / readDataArrays and the file.)
    nfloor=r.u32(); floors=list(struct.unpack_from("<%dH"%nfloor, data, r.p)); r.seek(nfloor*2)  # FloorData[]
    # FloorData slope: a sector's floorIndex points at a command list; the FLOOR
    # command (func==2) carries slantX:8,slantZ:8 (s8 each). Walk the list (skip
    # portal/ceiling/trigger, honour the end bit) and return the floor slant.
    # (funcs per format.h FloorData enum: 1=PORTAL 2=FLOOR 3=CEILING 4=TRIGGER.)
    def sector_slant(fidx):
        if fidx<=0 or fidx>=nfloor: return (0,0)
        i=fidx
        while True:
            cmd=floors[i]; i+=1
            func=cmd&0x1F; end=(cmd>>15)&1
            if func==2:                       # FLOOR slant
                w=floors[i]; i+=1
                sx=w&0xFF; sz=(w>>8)&0xFF
                if sx>=128: sx-=256
                if sz>=128: sz-=256
                return (sx,sz)
            elif func==1:   i+=1              # PORTAL: 1 data word
            elif func==3:   i+=1              # CEILING slant: 1 data word
            elif func==4:                     # TRIGGER: setup word + cmd list
                i+=1
                while i<nfloor and not ((floors[i]>>15)&1): i+=1
                i+=1
            elif 7<=func<=18: i+=1            # triangle floor/ceiling: 1 data word
            if end or i>=nfloor: break
        return (0,0)
    mds=r.u32(); pMeshData=r.p; r.seek(mds*2)   # meshData (u16 words; meshOffsets are BYTE offsets into it)
    moc=r.u32(); pMeshOff=r.p;  r.seek(moc*4)   # meshOffsets (u32 each)
    an=r.u32();  pAnims=r.p;    r.seek(an*32)   # anims (32B each)
    r.seek(r.u32()*6); r.seek(r.u32()*8); r.seek(r.u32()*2)  # states/ranges/commands
    nds=r.u32(); pNodes=r.p;   r.seek(nds*4)    # nodesData (u32 words; model.node is a WORD index)
    fds=r.u32(); pFrame=r.p;   r.seek(fds*2)    # frameData (u16 words; frameOffset is a BYTE offset)
    mc=r.u32();  pModels=r.p;  r.seek(mc*20)    # models (20B each on PSX)
    modelsCount=mc
    r.seek(r.u32()*32)                      # staticMeshes
    objCount=r.u32()
    if objCount<1 or objCount>8000:
        print("!! objCount wrong:", objCount); sys.exit(1)
    objtex=[]
    for i in range(objCount):
        x0=r.u8();y0=r.u8();clut=r.u16();x1=r.u8();y1=r.u8()
        tile=r.u16()&0x3FFF;x2=r.u8();y2=r.u8();r.u16();x3=r.u8();y3=r.u8();attr=r.u16()
        objtex.append(dict(tile=tile,clut=clut,uv=[(x0,y0),(x1,y1),(x2,y2),(x3,y3)]))
    # TEXSCALE=2: half-res textures (quarter atlas bytes); applied per-objtex
    # AFTER the used-set is known so LARA'S textures stay FULL-RES (her
    # holster/boot 1px details mangled at half-res; she's always on screen).
    TEXSCALE=int(os.environ.get("TEXSCALE","1"))

    # ---- LARA SPAWN from the level's entity table (NOT assumed) ----
    # continue the readDataArrays walk: spriteTex -> spriteSeq -> cameras ->
    # soundSources -> boxes -> overlaps -> zones -> animTex -> entities
    # (order + record sizes per format.h loadTR1_PSX / readEntities: TR1 PSX
    # entity = type u16, room u16, x/y/z s32, rotation s16, intensity u16,
    # flags u16 = 22 bytes.)
    spawn=None
    sp=r.p
    try:
        r.seek(r.u32()*16)             # spriteTextures (PSX 16B)
        r.seek(r.u32()*8)              # spriteSequences (8B)
        r.seek(r.u32()*16)             # cameras (16B)
        r.seek(r.u32()*16)             # soundSources (16B)
        nbox=r.u32(); r.seek(nbox*20)  # boxes (TR1 20B)
        r.seek(r.u32()*2)              # overlaps (u16)
        r.seek(2*3*nbox*2)             # zones TR1: 2 alts x (gnd1,gnd2,fly) x nbox u16
        r.seek(r.u32()*2)              # animTex block (u16 words)
        nent=r.u32()
        for i in range(nent):
            et=r.u16(); erm=r.u16(); ex=r.s32(); ey=r.s32(); ez=r.s32()
            erot=r.s16(); r.u16(); r.u16()
            if et==0:                  # ITEM_LARA
                spawn=dict(room=erm, x=ex, y=ey, z=ez, rot=erot)
                break
    finally:
        r.setpos(sp)
    if spawn is None:
        raise SystemExit("!! Lara entity (type 0) not found in entity table")
    print("LARA SPAWN (entity table): room %d  x=%d y=%d z=%d rot=%d" %
          (spawn['room'], spawn['x'], spawn['y'], spawn['z'], spawn['rot']))

    # ===================================================================
    #  LARA (model 0): parse mesh tree + faces, pose the RUN cycle.
    #  Byte layouts VERIFIED against format.h loadTR1_PSX and the file
    #  (frame-0 baked bbox matches the frame's own stored bbox to +-2u).
    # ===================================================================
    lara = build_lara(data, pMeshData, pMeshOff, pAnims, pNodes, pFrame,
                      pModels, modelsCount, objCount)

    # ---- BFS-connected room set from room 0 ----
    seen=set([0]); order=[0]; dq=deque([0])
    while dq and len(order)<MAX_ROOMS:
        c=dq.popleft()
        for a in rooms_all[c]['ports']:
            if a not in seen and a<nrooms:
                seen.add(a); order.append(a); dq.append(a)
    order=order[:MAX_ROOMS]
    rooms=[rooms_all[i] for i in order]
    print("room set:", order)

    # ---- decoders ----
    def clut_rgb555(ci,nib):
        o=cluts_off+ci*CLUT_BYTES+nib*2
        v=data[o]|(data[o+1]<<8)
        return (v&31,(v>>5)&31,(v>>10)&31,(v>>15)&1)
    def tile_nibble(ti,x,y):
        o=tiles_off+ti*TILE_PAGE_BYTES+(y*256+x)//2
        b=data[o]; return (b>>4) if (x&1) else (b&0x0F)
    def jag16(r5,g5,b5): return ((r5&31)<<11)|((b5&31)<<6)|((g5&31)<<1)

    # ---- union used object-textures across all rooms ----
    used=set()
    for rm in rooms:
        for q in rm['quads']: used.add(q['tex'])
        for t in rm['tris']:  used.add(t['tex'])
    for f in lara['tex_faces']:            # Lara's textured faces share the atlas
        used.add(f['tex'])
    used=sorted(i for i in used if i<objCount)
    print("union object-textures:", len(used))
    lara_ts=set(f['tex'] for f in lara['tex_faces'])
    for _i,_o in enumerate(objtex):
        _t = 1 if _i in lara_ts else TEXSCALE
        _o['ts']=_t
        if _t>1: _o['uv']=[(x//_t,y//_t) for (x,y) in _o['uv']]
    if TEXSCALE>1: print("TEXSCALE %d (lara exempt: %d textures)"%(TEXSCALE,len(lara_ts)))

    # Corner count per objtex for the tile bounding box. A TRIANGLE object
    # texture stores a junk (0,0) 4th UV corner; if included it inflates the
    # tile to ~256px and explodes the atlas. Rooms KEEP the legacy 4-corner
    # behavior (byte-identical output); Lara-only textures (disjoint index
    # range) use their true face vertex count so triangles pack tightly.
    # M3 BAKED LIGHTING: face level = avg of its verts' PSX light (0..3,
    # 0=dark). Tiles get darkened copies per level; faces point at them.
    SHADE_FACT=(90, 141, 200, 256)     # /256 brightness per level
    def _face_lvl(rm, f):
        vs=rm['verts']; n=len(f['v'])
        return min(3, (sum(vs[x][3] for x in f['v'])//n)>>6)
    room_tex=set(); quad_tex=set(); tri_tex=set()
    used_pairs=set()
    for rm in rooms:
        for q in rm['quads']:
            room_tex.add(q['tex']); q['lvl']=_face_lvl(rm,q)
            used_pairs.add((q['tex'],q['lvl']))
        for t in rm['tris']:
            room_tex.add(t['tex']); t['lvl']=_face_lvl(rm,t)
            used_pairs.add((t['tex'],t['lvl']))
    for f in lara['quads']:
        if not f['colored']: quad_tex.add(f['tex']); used_pairs.add((f['tex'],3))
    for f in lara['tris']:
        if not f['colored']: tri_tex.add(f['tex']); used_pairs.add((f['tex'],3))
    def corner_count(tex):
        if tex in room_tex: return 4       # preserve existing room atlas exactly
        return 4 if tex in quad_tex else 3 # Lara-only: real face vertex count

    # ---- dedup tiles by (tile,clut,bbox), decode once ----
    tiles_out=[]; grp_of_key={}; grp_of_tex={}
    for (ti,lvl) in sorted(used_pairs):
        o=objtex[ti]
        nc=corner_count(ti)
        us=[p[0] for p in o['uv'][:nc]]; vs=[p[1] for p in o['uv'][:nc]]
        umin,umax,vmin,vmax=min(us),max(us),min(vs),max(vs)
        w=umax-umin+1; h=vmax-vmin+1
        key=(o['tile'],o['clut'],umin,vmin,w,h,lvl)
        g=grp_of_key.get(key)
        if g is None:
            fac=SHADE_FACT[lvl]
            px=[]
            for y in range(vmin,vmax+1):
                for x in range(umin,umax+1):
                    r5,g5,b5,al=clut_rgb555(o['clut'],tile_nibble(o['tile'],x*o['ts'],y*o['ts']))
                    px.append(((r5*fac)>>8,(g5*fac)>>8,(b5*fac)>>8,al))
            g=len(tiles_out); grp_of_key[key]=g
            tiles_out.append(dict(w=w,h=h,umin=umin,vmin=vmin,px=px))
        grp_of_tex[(ti,lvl)]=(g,umin,vmin)
    print("unique atlas tiles (with shade levels):", len(tiles_out))

    # ---- palette (all tiles) ----
    # LARA GETS GUARANTEED SEATS: quantizing her skin/outfit against a level
    # full of wood/rock browns remapped her to the scenery (mansion 2026-07-13
    # made her skin several shades too dark). Her tiles' top colours enter the
    # palette FIRST; the rooms bid for the rest.
    lara_tile_idx=set(g for (key,(g,_u,_v)) in grp_of_tex.items()
                      if key[0] in lara_ts)
    hist=Counter(); hist_lara=Counter()
    for i,t in enumerate(tiles_out):
        tgt = hist_lara if i in lara_tile_idx else hist
        for (r5,g5,b5,a) in t['px']: tgt[jag16(r5,g5,b5)]+=1
    uniq=list(set(hist)|set(hist_lara))
    # PALETTE BUDGET (fixed 2026-07-13 — the old 250-colour cap OVERLAPPED the
    # reserved bands: slots 242..253 were silently overwritten by Lara tones,
    # corrupting every texel quantized into them):
    #   0..239   quantized texel colours
    #   240..241 pickup/door pokes (runtime constants; emitted as 0)
    #   242..245 Lara flat-shade swatch
    #   246..253 Lara colored-face tones
    #   254..255 UI black/white
    if len(uniq)>240:
        keep_l=[c for c,_ in hist_lara.most_common(64)]
        _kl=set(keep_l)
        keep=keep_l+[c for c,_ in hist.most_common() if c not in _kl][:240-len(keep_l)]
        print("palette seats: %d Lara + %d room"%(len(keep_l),len(keep)-len(keep_l)))
        def _rgb(c): return ((c>>11)&31, (c>>1)&31, (c>>6)&31)
        remap={}
        for c in uniq:
            if c in keep: continue
            r,g2,b=_rgb(c)
            best=None;bd=1<<30
            for k in keep:
                kr,kg,kb=_rgb(k)
                d=(r-kr)*(r-kr)+(g2-kg)*(g2-kg)+(b-kb)*(b-kb)
                if d<bd: bd=d; best=k
            remap[c]=best
        print("palette quantized: %d -> 240 colours" % len(uniq))
        for t in tiles_out:
            np=[]
            for ppx in t['px']:
                c=jag16(ppx[0],ppx[1],ppx[2])
                if c in remap:
                    r,g2,b=_rgb(remap[c])
                    np.append((r,g2,b,ppx[3]))
                else:
                    np.append(ppx)
            t['px']=np
        uniq=keep
    palette=sorted(uniq)[:240]; idx_of={c:i for i,c in enumerate(palette)}
    while len(palette)<256: palette.append(0)
    print("palette colours:", len(uniq))

    # ---- shelf-pack tiles into width-256 atlas ----
    ATLAS_W=256
    ordr=sorted(range(len(tiles_out)), key=lambda i:-tiles_out[i]['h'])
    sx=sy=sh=atlas_h=0; pos=[None]*len(tiles_out)
    for i in ordr:
        w=tiles_out[i]['w']; h=tiles_out[i]['h']
        if sx+w>ATLAS_W: sy+=sh; sx=0; sh=0
        pos[i]=(sx,sy); sx+=w; sh=max(sh,h); atlas_h=max(atlas_h,sy+h)
    atlas_h=(atlas_h+1)&~1
    atlas=bytearray(ATLAS_W*atlas_h)
    for i,t in enumerate(tiles_out):
        ax,ay=pos[i]; w=t['w']; h=t['h']
        for yy in range(h):
            row=(ay+yy)*ATLAS_W+ax; src=yy*w
            for xx in range(w):
                r5,g5,b5,a=t['px'][src+xx]; atlas[row+xx]=idx_of[jag16(r5,g5,b5)]

    # ---- Lara flat-shade swatch (same scheme as tr2jag_room.py) ----
    LARA_IDX_BASE=242; LARA_N=4; LARA_CELL=8
    lara_tones=[(9,6,3),(13,9,5),(18,13,8),(24,19,13)]
    for k in range(LARA_N):
        r5,g5,b5=lara_tones[k]; palette[LARA_IDX_BASE+k]=jag16(r5,g5,b5)
    LARA_SW_Y=atlas_h
    atlas+=bytearray(ATLAS_W*LARA_CELL)
    for k in range(LARA_N):
        cx=k*LARA_CELL
        for yy in range(LARA_CELL):
            row=(LARA_SW_Y+yy)*ATLAS_W+cx
            for xx in range(LARA_CELL): atlas[row+xx]=LARA_IDX_BASE+k
    atlas_h+=LARA_CELL

    # ---- Lara COLORED-face swatch band --------------------------------
    # Lara's non-textured (colored) faces reference a small palette index
    # (face flags & 0x7FFF < 256). PSX has no 8bpp global palette in the
    # shared room atlas, so we map each DISTINCT colored index to its own
    # solid atlas cell (approximate TR1-Lara tones; refine later if wanted).
    # Palette slots 246..253 (above the flat-shade swatch at 242..245).
    LARA_COL_BASE=246
    # Real TR1_PSX flat-face colour: index ci IS an objectTexture index (<256);
    # the colour is the FIRST texel of that objtex (format.h getColor TR1_PSX:
    # clut[ tile.index[(uv0.y*256+uv0.x)/2].(a|b by uv0.x&1) ]). Derive it.
    # CANONICAL Lara flat-face tones, derived from LEVEL1.PSX where the
    # objtex-first-texel rule demonstrably works (limbs verified correct on
    # hardware). In GYM.PSX the same indices point at unrelated dark texels
    # (rule breaks there -> dark red-brown limbs, 2026-07-13). Lara is the
    # same model in every level; her skin doesn't change per house.
    LARA_TONE_CANON = {6:0xcae4, 8:0x0000, 10:0xa1da, 13:0xcae4,
                       14:0xdb28, 21:0x73a0, 25:0x5910, 34:0x94a8}
    def lara_col_tone(ci):
        if ci in LARA_TONE_CANON: return LARA_TONE_CANON[ci]
        # MEDIAN texel of the objtex region, not texel[uv0]: the first-texel
        # rule is a lottery (uv0 landed on a SHADOW pixel in GYM.PSX -> Lara's
        # flat-colored limbs rendered dark red-brown, 2026-07-13).
        o=objtex[ci]
        us=[p[0] for p in o['uv'][:3]]; vs=[p[1] for p in o['uv'][:3]]
        u0,u1=min(us),max(us); v0,v1=min(vs),max(vs)
        cols=[]
        for y in range(v0,v1+1):
            for x in range(u0,u1+1):
                r5,g5,b5,a = clut_rgb555(o['clut'], tile_nibble(o['tile'], x*o['ts'], y*o['ts']))
                if a: cols.append((r5,g5,b5))
        if not cols:
            ux,uy=o['uv'][0]
            r5,g5,b5,a = clut_rgb555(o['clut'], tile_nibble(o['tile'], ux*o['ts'], uy*o['ts']))
            return jag16(r5,g5,b5)
        cols.sort(key=lambda c:c[0]+c[1]+c[2])
        m=cols[len(cols)//2]
        return jag16(m[0],m[1],m[2])
    col_indices=sorted(lara['col_indices'])
    # ROOM colored faces too (tex<256 = flat palette colour, e.g. mansion
    # walls) — they rendered BLACK because their UVs indexed objtex garbage.
    room_cols=set()
    for rm in rooms:
        for q in rm['quads']:
            if q['tex']<256: room_cols.add(q['tex']&0xFF)
        for t in rm['tris']:
            if t['tex']<256: room_cols.add(t['tex']&0xFF)
    col_indices=sorted(set(col_indices)|room_cols)
    print("colored indices (lara+rooms):", len(col_indices))
    if len(uniq) > LARA_COL_BASE:
        print("!! tile palette (%d) overlaps Lara colored swatch base %d" % (len(uniq),LARA_COL_BASE))
    if len(col_indices) > 30:
        print("!! more than 30 distinct colored indices:", col_indices)
    col_pal={}                 # colored index -> palette slot
    print("lara colored-face tones (idx->RGB16):", end=" ")
    for j,ci in enumerate(col_indices[:30]):
        slot=LARA_COL_BASE+j
        palette[slot]=lara_col_tone(ci); col_pal[ci]=slot
        print("%d:%04x" % (ci, palette[slot]), end=" ")
    print()
    LARA_COL_SW_Y=atlas_h
    atlas+=bytearray(ATLAS_W*LARA_CELL)
    col_cell={}                # colored index -> (uCenter,vCenter) in atlas
    for j,ci in enumerate(col_indices[:30]):
        cx=j*LARA_CELL
        for yy in range(LARA_CELL):
            row=(LARA_COL_SW_Y+yy)*ATLAS_W+cx
            for xx in range(LARA_CELL): atlas[row+xx]=col_pal[ci]
        col_cell[ci]=(cx+LARA_CELL//2, LARA_COL_SW_Y+LARA_CELL//2)
    atlas_h+=LARA_CELL
    print("atlas: %dx%d = %d bytes (%.1f KB)" % (ATLAS_W,atlas_h,len(atlas),len(atlas)/1024.0))

    def inset_uv(pts):
        # pull every corner 1 texel toward the face's UV centroid: the affine
        # DDA overshoots up to ~1 texel, and tiles are shelf-packed edge-to-
        # edge, so overshoot samples the NEIGHBOUR tile (blue gym-mat lines
        # bleeding onto the wood floor; stray skin lines on Lara).
        n=len(pts)
        su=sum(p[0] for p in pts); sv=sum(p[1] for p in pts)
        out=[]
        for (u,v) in pts:
            nu=u+(1 if u*n<su else (-1 if u*n>su else 0))
            nv=v+(1 if v*n<sv else (-1 if v*n>sv else 0))
            out.append((nu,nv))
        return out
    def room_uv(tex,swap,n,lvl):
        if tex < 256:                       # flat-colour face -> swatch cell
            uc,vc = col_cell.get(tex & 0xFF, (0,0))
            return [(uc,vc)]*n
        return inset_uv(face_uv(tex,swap,lvl)[:n])
    def face_uv(tex,swap,lvl=3):
        g,umin,vmin=grp_of_tex[(tex,lvl)]; ax,ay=pos[g]
        out=[(ax+(u-umin),ay+(v-vmin)) for (u,v) in objtex[tex]['uv']]
        if swap: out[2],out[3]=out[3],out[2]
        return [(min(ATLAS_W-1,max(0,u)),min(atlas_h-1,max(0,v))) for (u,v) in out]

    def sar8(v): return v>>8 if v>=0 else -((-v)>>8)

    # ---- build per-room geometry + sector blobs ----
    geom=bytearray(); sect=bytearray(); index=[]
    def _face_sort(rm):
        # STATIC PAINTER ORDER (kernel draws faces in data order, no depth
        # sort): shell first, interior verticals LAST. Key: XZ distance of
        # the face centroid from the room centroid DESC (walls early, central
        # pillars late); tie-break horizontal-before-vertical (a pillar
        # always occludes the floor patch at its own footprint - the
        # "see-through rock"). Quads and tris sort within their own lists
        # (the kernel renders all quads then all tris).
        verts=rm['verts']
        cx=sum(v[0] for v in verts)/max(1,len(verts))
        cz=sum(v[2] for v in verts)/max(1,len(verts))
        def key(f):
            vs=[verts[i] for i in f['v']]
            fx=sum(v[0] for v in vs)/len(vs); fz=sum(v[2] for v in vs)/len(vs)
            d2=(fx-cx)*(fx-cx)+(fz-cz)*(fz-cz)
            ys=[v[1] for v in vs]
            vert_extent=max(ys)-min(ys)          # tall face = vertical
            return (-d2, vert_extent)
        rm['quads'].sort(key=key)
        rm['tris'].sort(key=key)
    SUBDIV_MAX=int(os.environ.get("SUBDIV_MAX","1536"))   # world units
    VERT_BUDGET=500                                        # vtxcache cap 512
    def _subdivide(rm):
        # resolve explicit atlas UVs first (splits need fractional corners)
        verts=rm['verts']
        for f in rm['quads']: f['uv']=room_uv(f['tex'],f['swapped'],4,f['lvl'])
        for f in rm['tris']:  f['uv']=room_uv(f['tex'],False,3,f['lvl'])
        def extent(f):
            vs=[verts[i] for i in f['v']]
            xs=[v[0] for v in vs]; ys=[v[1] for v in vs]; zs=[v[2] for v in vs]
            return max(max(xs)-min(xs), max(ys)-min(ys), max(zs)-min(zs))
        def midvert(a,b):
            va,vb=verts[a],verts[b]
            nv=((va[0]+vb[0])//2,(va[1]+vb[1])//2,(va[2]+vb[2])//2,
                (va[3]+vb[3])//2)
            verts.append(nv); return len(verts)-1
        def miduv(ua,ub): return ((ua[0]+ub[0])//2,(ua[1]+ub[1])//2)
        outq=[]; work=list(rm['quads'])
        while work:
            f=work.pop()
            if extent(f)<=SUBDIV_MAX or len(verts)>=VERT_BUDGET:
                outq.append(f); continue
            v=f['v']; uv=f['uv']
            vs=[verts[i] for i in v]
            dx=max(p2[0] for p2 in vs)-min(p2[0] for p2 in vs)
            dy=max(p2[1] for p2 in vs)-min(p2[1] for p2 in vs)
            dz=max(p2[2] for p2 in vs)-min(p2[2] for p2 in vs)
            # split across the LONGEST axis: quad corners 0-1-2-3; edges
            # (0,1)&(3,2) vs (1,2)&(0,3) — pick the pair whose midpoints
            # separate the long axis best (split edges 01/32 or 12/03)
            def span(i,j):
                a,b=verts[v[i]],verts[v[j]]
                return abs(a[0]-b[0])+abs(a[1]-b[1])+abs(a[2]-b[2])
            if span(0,1)+span(3,2) >= span(1,2)+span(0,3):
                mA=midvert(v[0],v[1]); mB=midvert(v[3],v[2])
                uA=miduv(uv[0],uv[1]); uB=miduv(uv[3],uv[2])
                work.append(dict(v=[v[0],mA,mB,v[3]],uv=[uv[0],uA,uB,uv[3]],tex=f['tex'],swapped=f['swapped']))
                work.append(dict(v=[mA,v[1],v[2],mB],uv=[uA,uv[1],uv[2],uB],tex=f['tex'],swapped=f['swapped']))
            else:
                mA=midvert(v[1],v[2]); mB=midvert(v[0],v[3])
                uA=miduv(uv[1],uv[2]); uB=miduv(uv[0],uv[3])
                work.append(dict(v=[v[0],v[1],mA,mB],uv=[uv[0],uv[1],uA,uB],tex=f['tex'],swapped=f['swapped']))
                work.append(dict(v=[mB,mA,v[2],v[3]],uv=[uB,uA,uv[2],uv[3]],tex=f['tex'],swapped=f['swapped']))
        outt=[]; work=list(rm['tris'])
        while work:
            f=work.pop()
            if extent(f)<=SUBDIV_MAX or len(verts)>=VERT_BUDGET:
                outt.append(f); continue
            v=f['v']; uv=f['uv']
            # split the longest edge
            def span2(i,j):
                a,b=verts[v[i]],verts[v[j]]
                return abs(a[0]-b[0])+abs(a[1]-b[1])+abs(a[2]-b[2])
            e=max(((0,1),(1,2),(2,0)),key=lambda ij:span2(*ij))
            i,j=e; k=3-i-j
            m=midvert(v[i],v[j]); um=miduv(uv[i],uv[j])
            work.append(dict(v=[v[i],m,v[k]],uv=[uv[i],um,uv[k]],tex=f['tex']))
            work.append(dict(v=[m,v[j],v[k]],uv=[um,uv[j],uv[k]],tex=f['tex']))
        rm['quads']=outq; rm['tris']=outt
    for rm in rooms:
        _subdivide(rm)
        _face_sort(rm)
        goff=len(geom)
        verts=rm['verts']; quads=rm['quads']; tris=rm['tris']
        offX=sar8(rm['info_x']); offZ=sar8(rm['info_z'])
        b=bytearray()
        b+=struct.pack(">HHHHH", len(verts),len(quads),len(tris),ATLAS_W,atlas_h)
        b+=struct.pack(">hhh", offX,0,offZ)
        for (x,y,z,light) in verts:
            # TR1 PSX raw vertex light IS the brightness (0=dark..255=bright):
            # format.h converts raw -> PC luma -> brightness and the math
            # cancels to `raw`. The old PC-style 255-(light>>5) crushed
            # everything to 248..255 (uniformly-bright rooms).
            s=light; s=0 if s<0 else (255 if s>255 else s)
            b+=struct.pack(">hhhH", x,y,z,s)
        for q in quads:
            b+=struct.pack(">HHHH", *q['v'])
            for (u,v) in q['uv']: b+=struct.pack(">HH",u,v)
        for t in tris:
            b+=struct.pack(">HHH", *t['v'])
            for (u,v) in t['uv']: b+=struct.pack(">HH",u,v)
        while len(b)&7: b+=b'\0'
        geom+=b
        soff=len(sect)
        sb=bytearray()
        sb+=struct.pack(">HH", rm['xS'], rm['zS'])
        sb+=struct.pack(">ii", rm['info_x'], rm['info_z'])
        nslope=0
        for (floor,ceil,fidx,below,above) in rm['sect']:
            # floor==-127 is BOTH walls and floor openings; roomBelow (below
            # != 255) means a vertical portal -> 0x7FFE OPENING (walkable off
            # the edge, room below supplies the floor), else 0x7FFF WALL
            fy=(0x7FFE if below!=255 else 0x7FFF) if floor==-127 else floor*256
            # WATER SURFACE: TR1 stores the floor of a sector ABOVE a pool at
            # the water line with roomBelow -> the water room. Mark it with
            # bit0 of floorY (floors are *256, low bits free) so the runtime
            # can tell "standing on water" from solid ground.
            if floor!=-127 and below!=255 and below<len(rooms_all) and rooms_all[below].get('water'):
                fy |= 1
            cy=-32768 if ceil==-127 else ceil*256
            sx,sz=(0,0) if floor==-127 else sector_slant(fidx)
            if sx or sz: nslope+=1
            # cell: s16 floorY, s16 ceilY, s8 slantX, s8 slantZ  (stride 6)
            sb+=struct.pack(">hhbb", fy, cy, sx, sz)
        nws=sum(1 for k in range(8, len(sb), 6) if sb[k-8+4-4] is not None) if False else 0
        nws=sum(1 for off in range(12, len(sb), 6) if sb[off+1] & 1 and sb[off] != 0x7F)
        print("  room %2d sloped sectors: %d / %d%s" % (rm['i'], nslope, len(rm['sect']),
              ("  WATER-SURFACE cells: %d" % nws) if nws else ("  [WATER ROOM]" if rm.get('water') else "")))
        while len(sb)&7: sb+=b'\0'
        sect+=sb
        # soff bit31 = WATER room flag (sect offsets are far below 2^31;
        # the runtime masks it off)
        index.append((goff, soff | (0x80000000 if rm.get('water') else 0)))

    # ---- build Lara run-cycle blob (mrt_lara.bin) --------------------
    # Faces reference the SHARED atlas (textured -> tile UVs; colored ->
    # solid swatch cell). PSX MESH quads (initMesh) are NOT vertex-swapped -
    # only ROOM quads (readRoom) get the v2<->v3 swap. So Lara's quads keep
    # their file order (no swap); swapping them inverts winding -> the kernel
    # back-face-culls them -> missing polys.
    def lara_quad_uv(f):
        if f['colored']:
            uc,vc=col_cell[f['tex']]; return [(uc,vc)]*4
        return inset_uv(face_uv(f['tex'], False, 3)) # no swap (mesh, not room)
    def lara_tri_uv(f):
        if f['colored']:
            uc,vc=col_cell[f['tex']]; return [(uc,vc)]*3
        return inset_uv(face_uv(f['tex'], False, 3)[:3])
    lb=bytearray()
    vcount=lara['vcount']; nframes=lara['framecount']
    lquads=lara['quads']; ltris=lara['tris']
    # LARA FACE DIET: her 375 faces cost 38% of the Jaguar frame in per-face
    # kernel setup while she stands ~100px tall — drop sub-pixel DETAIL faces
    # by model-space area (T-pose coords). LARA_MINAREA env tunes it.
    import math as _m
    _allv=[]
    for _mm in lara['mesh_verts']: _allv.extend(_mm)
    def _farea(vi):
        p=[_allv[i] for i in vi]
        def cr(a,b,c):
            ux,uy,uz=b[0]-a[0],b[1]-a[1],b[2]-a[2]
            wx,wy,wz=c[0]-a[0],c[1]-a[1],c[2]-a[2]
            cx=uy*wz-uz*wy; cy=uz*wx-ux*wz; cz=ux*wy-uy*wx
            return _m.sqrt(cx*cx+cy*cy+cz*cz)
        a=cr(p[0],p[1],p[2])
        if len(p)==4: a+=cr(p[0],p[2],p[3])
        return a/2.0
    _MINAREA=float(os.environ.get("LARA_MINAREA","400"))
    _nq0,_nt0=len(lquads),len(ltris)
    # never diet the HEAD mesh (14): the hair is tiny tris there — dropping
    # them opened skin-tone gaps through her hair
    _vb=lara['vbase_of']
    def _meshof(vi):
        g=vi[0]
        for _mi in range(len(_vb)-1,-1,-1):
            if g>=_vb[_mi]: return _mi
        return 0
    _KEEP=set(int(x) for x in os.environ.get("LARA_KEEPMESH","14").split(",") if x!="")
    lquads=[f for f in lquads if _meshof(f['v']) in _KEEP or _farea(f['v'])>=_MINAREA]
    ltris=[f for f in ltris if _meshof(f['v']) in _KEEP or _farea(f['v'][:3])>=_MINAREA]
    _hist={}
    for f in (lara['quads']+lara['tris']):
        _mi=_meshof(f['v'])
        if (f in lquads) or (f in ltris): continue
        _hist[_mi]=_hist.get(_mi,0)+1
    print("LARA FACE DIET dropped per mesh:", dict(sorted(_hist.items())))
    # INTRA-MESH painter order: the 3rd-person camera sees Lara FROM BEHIND;
    # tiny front-of-head tris (eyes/cheeks, 2-6px) flip winding at glancing
    # angles, dodge the backface cull, and painted OVER her hair (they sat
    # late in the list). Order each mesh's faces FRONT (+z, far from the
    # behind-camera) first, BACK (hair) last, so hair wins overlaps.
    def _fzkey(f):
        vs=[_allv[i] for i in f['v']]
        return (_meshof(f['v']), -sum(v[2] for v in vs)/len(vs))
    lquads.sort(key=_fzkey)
    ltris.sort(key=_fzkey)
    print("LARA FACE DIET: quads %d->%d tris %d->%d (minarea %g)"%(
        _nq0,len(lquads),_nt0,len(ltris),_MINAREA))
    lb+=struct.pack(">HHHHHH", vcount, len(lquads), len(ltris),
                    nframes, ATLAS_W, atlas_h)
    for f in lquads:
        lb+=struct.pack(">HHHH", *f['v'])       # file order (no swap)
        for (u,vv) in lara_quad_uv(f): lb+=struct.pack(">HH",u,vv)
    for f in ltris:
        lb+=struct.pack(">HHH", *f['v'])
        for (u,vv) in lara_tri_uv(f): lb+=struct.pack(">HH",u,vv)
    # NOTE: baked posed frames are GONE — Lara is posed at runtime from mrt_lskin.

    # ---- mrt_lskin.bin: skeleton + ALL-animation joint angles (runtime skin) --
    mcount=lara['mcount']; mv=lara['mesh_verts']; vbase=lara['vbase_of']
    nodes=lara['nodes']; sframes=lara['skin_frames']; atable=lara['anim_table']
    framestride=6+mcount*3                 # bytes/frame: root(3*s16) + mcount*(3*u8)
    if framestride & 1: framestride += 1   # pad EVEN so 68000 s16 reads stay aligned
    sk=bytearray()
    # HEADER: u16 mcount, vcount, qcount, tcount, animcount, framecount, framestride, pad
    sk+=struct.pack(">HHHHHHHH", mcount, vcount, len(lquads), len(ltris),
                    len(atable), len(sframes), framestride, 0)
    for i in range(mcount):                # MESHINFO: u16 vbase, u16 vlen
        sk+=struct.pack(">HH", vbase[i], len(mv[i]))
    for verts in mv:                       # MESHVERTS: s16 x,y,z (local)
        for (x,y,z) in verts: sk+=struct.pack(">hhh", x,y,z)
    for (fl,x,y,z) in nodes:               # NODES: u16 flags, s16 x,y,z
        sk+=struct.pack(">Hhhh", fl, _clampi16(x), _clampi16(y), _clampi16(z))
    for (root,angs) in sframes:            # FRAMES: s16 root; mcount*(u8 ax,ay,az)
        fb0=len(sk)
        sk+=struct.pack(">hhh", *root)
        for (ax,ay,az) in angs: sk+=struct.pack(">BBB", ax,ay,az)
        while len(sk)-fb0 < framestride: sk+=b'\0'   # pad to even framestride
    for (start,n,st,rate) in atable:       # ANIMS: u16 start,count; u8 state,rate
        sk+=struct.pack(">HHBB", start, n&0xFFFF, st&0xFF, rate&0xFF)
    while len(sk)&7: sk+=b'\0'

    # ---- write files ----
    def wr(name,b):
        open(os.path.join(OUTDIR,name),"wb").write(b)
    wr(PREFIX+"_atlas.bin", atlas)
    pal=bytearray()
    for c in palette: pal+=struct.pack(">H",c)
    wr(PREFIX+"_pal.bin", pal)
    wr(PREFIX+"_geom.bin", geom)
    wr(PREFIX+"_sect.bin", sect)
    idx=bytearray()
    idx+=struct.pack(">HHHH", len(rooms), ATLAS_W, atlas_h, 0)
    for (g,s) in index: idx+=struct.pack(">II", g, s)
    wr(PREFIX+".bin", idx)
    wr(PREFIX+"_lara.bin", lb)
    wr(PREFIX+"_lskin.bin", sk)

    with open(os.path.join(OUTDIR,PREFIX+"_lara.h"),"w") as f:
        f.write("// generated by tr2jag_multiroom.py - PSX TR1 LEVEL1 Lara run cycle\n")
        f.write("// mrt_lara.bin (BIG-ENDIAN):\n")
        f.write("//   HEADER 12B: u16 vcount,qcount,tcount,framecount,atlasW,atlasH\n")
        f.write("//   QUADS  qcount*24: u16 v0,v1,v2,v3 ; u16 (u,v)x4  (v2<->v3 + uv2<->uv3 swapped)\n")
        f.write("//   TRIS   tcount*18: u16 v0,v1,v2   ; u16 (u,v)x3\n")
        f.write("//   FRAMES framecount*(vcount*6): s16 x,y,z  (Lara-local, +Y down, world units)\n")
        f.write("#define MRT_LARA_VCOUNT     %d\n" % lara['vcount'])
        f.write("#define MRT_LARA_QCOUNT     %d\n" % len(lquads))
        f.write("#define MRT_LARA_TCOUNT     %d\n" % len(ltris))
        f.write("#define MRT_LARA_FRAMECOUNT %d\n" % lara['framecount'])
        f.write("#define MRT_LARA_FRAMESIZE  %d  // bytes per frame (vcount*6)\n" % (lara['vcount']*6))
        f.write("#define MRT_LARA_RUNFRAMES  %d  // run cycle = frames 0..RUNFRAMES-1\n" % lara['runFrames'])
        f.write("#define MRT_LARA_STANDFRAME %d  // idle stand pose\n" % lara['standFrame'])
        f.write("#define MRT_LARA_JUMPSTART  %d  // (compat) forward-jump first frame\n" % lara['jumpStart'])
        f.write("#define MRT_LARA_JUMPFRAMES %d  // (compat) forward-jump length\n" % lara['jumpFrames'])
        f.write("#define MRT_LARA_FJUMPSTART  %d  // FORWARD (running) jump first frame\n" % lara['fjumpStart'])
        f.write("#define MRT_LARA_FJUMPFRAMES %d  // forward-jump length\n" % lara['fjumpFrames'])
        f.write("#define MRT_LARA_UJUMPSTART  %d  // UP (standing/vertical) jump first frame\n" % lara['ujumpStart'])
        f.write("#define MRT_LARA_UJUMPFRAMES %d  // up-jump length\n" % lara['ujumpFrames'])
        f.write("#define MRT_LARA_CLIMBSTART  %d  // hang->pull-up (jump grab) first frame\n" % lara['climbStart'])
        f.write("#define MRT_LARA_CLIMBFRAMES %d  // hang->pull-up length\n" % lara['climbFrames'])
        f.write("#define MRT_LARA_VAULT2START %d  // standing kick-flip vault (2 clicks)\n" % lara['vault2Start'])
        f.write("#define MRT_LARA_VAULT2FRAMES %d\n" % lara['vault2Frames'])
        f.write("#define MRT_LARA_VAULT3START %d  // standing kick-flip vault (3 clicks)\n" % lara['vault3Start'])
        f.write("#define MRT_LARA_VAULT3FRAMES %d\n" % lara['vault3Frames'])
        f.write("\n// ---- runtime skinning (mrt_lskin.bin, BIG-ENDIAN) ----\n")
        f.write("//   HEADER 16B: u16 mcount,vcount,qcount,tcount,animcount,framecount,framestride,pad\n")
        f.write("//   MESHINFO mcount*{u16 vbase,vlen}; MESHVERTS vcount*{s16 x,y,z}\n")
        f.write("//   NODES (mcount-1)*{u16 flags,s16 x,y,z}\n")
        f.write("//   FRAMES framecount*{s16 rootx,y,z; mcount*(u8 ax,ay,az)}  (angle idx 0..255)\n")
        f.write("//   ANIMS animcount*{u16 frameStart,frameCount; u8 state,rate}\n")
        f.write("#define MRT_LSKIN_MCOUNT     %d\n" % lara['mcount'])
        f.write("#define MRT_LSKIN_VCOUNT     %d\n" % lara['vcount'])
        f.write("#define MRT_LSKIN_ANIMCOUNT  %d\n" % len(lara['anim_table']))
        f.write("#define MRT_LSKIN_FRAMECOUNT %d\n" % len(lara['skin_frames']))
        f.write("#define MRT_LSKIN_FRAMESTRIDE %d  // bytes/frame (padded even)\n" % ((6+lara['mcount']*3+1)&~1))
        # relative anim index for each TR state we drive from the state machine
        atable=lara['anim_table']
        def anim_for_state(st, last=False):
            hits=[i for i,e in enumerate(atable) if e[2]==st]
            if not hits: return 0
            return hits[-1] if last else hits[0]
        for nm,st in [("WALK",0),("RUN",1),("STOP",2),("FJUMP",3),("FASTBACK",5),
                      ("TURNR",6),("TURNL",7),("DEATH",8),("FALL",9),("HANG",10),
                      ("REACH",11),("TREAD",13),("COMPRESS",15),("BACK",16),
                      ("HANGUP",19),("FASTTURN",20),("STEPR",21),("STEPL",22),
                      ("SLIDE",24),("BACKJUMP",25),("RIGHTJUMP",26),("LEFTJUMP",27),
                      ("UPJUMP",28),("FALLBACK",29),("SLIDEBACK",32),
                      ("VAULT2",0)]:
            f.write("#define LANIM_%-9s %d\n" % (nm, anim_for_state(st)))
        # PICK_UP state 39 exists twice: 130=UNDERWATER, 135=LAND -> use LAND
        f.write("#define LANIM_PICKUP    %d\n" % anim_for_state(39, last=True))
        # HANDSTAND (state 54): hang pull-up while holding WALK
        f.write("#define LANIM_HANDSTAND %d\n" % anim_for_state(54))
        # RELAXED breathing idle (TR1 ANIM_STAND_NORMAL=103) - the plain
        # anim_for_state(2) pick is the ALERT stand (weapons-ready look)
        f.write("#define LANIM_IDLE      %d\n" % 103)
        # the standing kick-flip vault (ANIM_CLIMB_2 = abs anim+50) by index:
        f.write("#define LANIM_VAULT     %d\n" % 50)
        f.write("#define LANIM_CLIMB3    %d\n" % 42)
        f.write("#define LANIM_CLIMBJUMP %d\n" % 26)

    with open(os.path.join(OUTDIR,PREFIX+".h"),"w") as f:
        f.write("// generated by tr2jag_multiroom.py - PSX TR1 LEVEL1 rooms %s\n" % order)
        f.write("#define MRT_ROOMCOUNT   %d\n" % len(rooms))
        f.write("#define MRT_ATLAS_W     %d\n" % ATLAS_W)
        f.write("#define MRT_ATLAS_H     %d\n" % atlas_h)
        f.write("#define MRT_LARA_IDX_BASE %d\n" % LARA_IDX_BASE)
        f.write("#define MRT_LARA_N        %d\n" % LARA_N)
        f.write("#define MRT_LARA_CELL     %d\n" % LARA_CELL)
        f.write("#define MRT_LARA_SW_Y     %d\n" % LARA_SW_Y)
        f.write("// mrt.bin: u16 roomCount,atlasW,atlasH,pad; roomCount*{u32 geom_off,sect_off}\n")
        f.write("// mrt_geom/mrt_sect: concatenated room0_tex / room0_sect blobs (8-aligned)\n")
        # portal ADJACENCY (local indices) for room-visibility culling: draw only
        # the room Lara is in + its portal neighbours (within frustum).
        g2l={g:i for i,g in enumerate(order)}
        f.write("// portal adjacency (local room indices; 255-padded)\n")
        f.write("#define MRT_ADJ_MAX 8\n")
        f.write("static const unsigned char mrt_adj[MRT_ROOMCOUNT][MRT_ADJ_MAX] = {\n")
        for rm in rooms:
            adj=sorted(set(g2l[a] for a in rm['ports'] if a in g2l))
            row=(adj+[255]*8)[:8]
            f.write("  {%s}, // room %d -> %s\n" % (",".join(str(v) for v in row), rm['i'], adj))
        f.write("};\n")

    # ---- spawn header (values straight from the entity table) ----
    if spawn['room'] not in g2l:
        raise SystemExit("!! Lara spawn room %d not in extracted set %s" %
                         (spawn['room'], order))
    with open(os.path.join(OUTDIR,PREFIX+"_spawn.h"),"w") as f:
        up=PREFIX.upper()
        f.write("// generated by tr2jag_multiroom.py - Lara entity from the level file\n")
        f.write("// original room %d -> local index %d; rot s16 (16384 = 90deg)\n" %
                (spawn['room'], g2l[spawn['room']]))
        f.write("#define %s_SPAWN_ROOM %d\n" % (up, g2l[spawn['room']]))
        f.write("#define %s_SPAWN_X    %d\n" % (up, spawn['x']))
        f.write("#define %s_SPAWN_Y    %d\n" % (up, spawn['y']))
        f.write("#define %s_SPAWN_Z    %d\n" % (up, spawn['z']))
        f.write("#define %s_SPAWN_YAW  %d\n" % (up, (spawn['rot'] & 0xFFFF) >> 8))
        # portal adjacency incl. VERTICAL portals (floor/ceiling openings) -
        # the hand-written gym table lacked them (balcony->hall = black hole)
        f.write("static const unsigned char %s_adjgen[%d][8] = {\n" % (PREFIX, len(rooms)))
        for rm in rooms:
            links=set(rm['ports'])
            for (fl,cl,fi,bw,ab) in rm['sect']:
                if bw!=255: links.add(bw)
                if ab!=255: links.add(ab)
            adj=sorted(set(g2l[a] for a in links if a in g2l))
            if len(adj)>8: print("!! room %d has %d adj (cap 8): %s" % (rm['i'],len(adj),adj))
            row=(adj+[255]*8)[:8]
            f.write("  {%s}, // room %d -> %s\n" % (",".join(str(v) for v in row), rm['i'], adj))
        f.write("};\n")
        # PORTAL GEOMETRY for portal-window clipping: per room, each portal =
        # {dst LOCAL room, 4 verts world x,y,z}. Portal verts are ROOM-LOCAL
        # like room verts (world x/z = local + info; y absolute).
        f.write("/* portals: ofs[i]..ofs[i+1] index 13-long records:")
        f.write(" dst,x0,y0,z0,...,x3,y3,z3 (world) */\n")
        recs=[]; ofs=[0]
        for rm in rooms:
            for a,pv in zip(rm['ports'], rm['portverts']):
                if a not in g2l: continue
                rec=[g2l[a]]
                for (vx,vy,vz) in pv:
                    rec += [vx+rm['info_x'], vy, vz+rm['info_z']]
                recs.append(rec)
            ofs.append(len(recs))
        f.write("static const long %s_portalv[][13] = {\n" % PREFIX)
        for rec in recs:
            f.write("  {%s},\n" % ",".join(str(v) for v in rec))
        f.write("};\n")
        f.write("static const unsigned short %s_portal_ofs[%d] = { %s };\n" %
                (PREFIX, len(ofs), ",".join(str(v) for v in ofs)))

    print("\n=== multiroom results ===")
    print("rooms         :", order)
    print("atlas         : %dx%d = %.1f KB" % (ATLAS_W,atlas_h,len(atlas)/1024.0))
    print("geom blob     : %d bytes  sect blob: %d bytes" % (len(geom),len(sect)))
    print("total data    : %.1f KB" % ((len(atlas)+len(pal)+len(geom)+len(sect))/1024.0))
    for k,rm in enumerate(rooms):
        g,s=index[k]
        print("  room %2d: %3dv %3dq %3dt  geom@%d sect@%d  off=(%d,%d)" %
              (rm['i'],len(rm['verts']),len(rm['quads']),len(rm['tris']),g,s,
               sar8(rm['info_x']),sar8(rm['info_z'])))

    # ---- Lara validation report -------------------------------------
    f0=lara['frames'][0]
    xs=[v[0] for v in f0]; ys=[v[1] for v in f0]; zs=[v[2] for v in f0]
    tex_used=[f['tex'] for f in (lara['quads']+lara['tris']) if not f['colored']]
    print("\n=== LARA (model 0, run cycle) ===")
    print("mesh count    : %d  (mStart=%d node=%d animIndex=%d)" %
          (lara['mcount'], lara['mstart'], lara['node'], lara['anim']))
    print("vcount        : %d   qcount: %d  tcount: %d" %
          (lara['vcount'], len(lara['quads']), len(lara['tris'])))
    print("framecount    : %d   file frameSize: %d words (%d bytes)  out frameSize: %d bytes (vcount*6)" %
          (lara['framecount'], lara['framesize_words'], lara['framesize_words']*2, lara['vcount']*6))
    print("colored faces : %d (indices %s -> atlas swatch cells)" %
          (lara['colored_faces'], sorted(lara['col_indices'])))
    print("textured faces: %d  objtex range [%d..%d]  objCount=%d  (all<objCount: %s)" %
          (len(tex_used), min(tex_used), max(tex_used), objCount, max(tex_used)<objCount))
    print("frame0 bbox   : X[%d..%d] Y[%d..%d] Z[%d..%d]  (H=%d, W=%d, D=%d units)" %
          (min(xs),max(xs),min(ys),max(ys),min(zs),max(zs),
           max(ys)-min(ys), max(xs)-min(xs), max(zs)-min(zs)))
    print("frame0 stored box (file): X[%d..%d] Y[%d..%d] Z[%d..%d]  (must ~match baked)" %
          tuple(lara['frame0_box']))
    print("mrt_lara.bin  : %d bytes" % len(lb))

if __name__=="__main__":
    main()
