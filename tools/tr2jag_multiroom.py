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

import struct, sys, os, zlib, math
_HOLEAUDIT=int(os.environ.get('MRT_HOLEAUDIT','0'))
_HA={'vert':[],'door':[],'fallthru':[],'hplan':[]}
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

# STAGEDIET (early backface cull): FACE_PLANES=1 prepends a 12-byte plane to
# every room face record (record = 12 + 6n bytes) so the kernel can cull
# backfaces BEFORE staging verts/UVs. Bins built with this flag REQUIRE a
# kernel/main.c built with STAGEDIET=1 (record strides move 24/18 -> 36/30).
FACE_PLANES       = int(os.environ.get("FACE_PLANES","0"))
FACE_PLANES_SIGN  = int(os.environ.get("FACE_PLANES_SIGN","-1"))  # -1 SETTLED by A/B 2026-07-20 (+1 = all-black: early cull keeps only backfaces, screen cull eats them)
FACE_PLANES_SLACK = int(os.environ.get("FACE_PLANES_SLACK","96"))
FACE_PLANES_TIGHT = int(os.environ.get("FACE_PLANES_TIGHT","0"))
# SHADE_KMAX: cap the per-face darkening level. The rect shade pass ORs k into
# whole rows, so OVERLAPPING faces combine bitwise (k=4 | k=3 = 7) and saturate
# to the darkest ramp entry no matter what any single face asked for. Capping k
# bounds that accumulation. 7 = original behaviour.
SHADE_KMAX = int(os.environ.get("SHADE_KMAX","7"))

# STATICS=1 (2026-07-22 campaign): bake each room's STATIC MESH placements
# (stalactites / plants / rock formations — the 20B records the room tail used
# to skip) into the room geometry as ordinary room faces, so subdivision,
# FACE_PLANES, painter sort, XCULL etc. all apply automatically. 0 = legacy
# byte-identical output. NOTE: statics push rooms 13/26 past 512 verts — the
# kernel vertex cache needs `make STATICS=1` (gpu.c vtxcache 512->768 verts).
STATICS = int(os.environ.get("STATICS","0"))

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
        r.seek(2); r.seek(r.u16()*20)      # ambient, lights (20B each)
        nsm=r.u16(); statics=[]            # STATIC PLACEMENTS (20B each; layout
        for _ in range(nsm):               #  verified vs format.h readRoom TR1_PSX)
            smx=r.s32(); smy=r.s32(); smz=r.s32()      # world coords
            srot=r.s16(); sint=r.u16(); smid=r.u16()   # rot(16384=90deg), intensity(0=bright), staticID
            r.u16()                                    # PSX pad
            statics.append(dict(x=smx,y=smy,z=smz,rot=srot,inten=sint,mid=smid))
        alt=r.u16(); rflags=r.u16()        # alternateRoom, flags (bit0=WATER)
        rooms.append(dict(i=ri, info_x=ix, info_z=iz, verts=verts, quads=quads,
                          tris=tris, ports=ports, portverts=portverts,
                          xS=xS, zS=zS, sect=sect, statics=statics,
                          water=bool(rflags & 1)))
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
               pModels, modelsCount, objCount,
               pStates=0, nStates=0, pRanges=0, nRanges=0, pCmds=0, nCmds=0,
               oid=0, swap_oid=None, swap_meshes=()):
    # locate Lara: model with objectID `oid`.  oid 0 = ITEM_LARA, oid 1 =
    # LARA_PISTOLS - the SAME 15-mesh tree with the pistols modelled into the
    # hand and thigh meshes, so every animation and the whole pose walk apply
    # unchanged and a gun-armed Lara is a DATA swap, not a second rig.
    laraModel=-1
    for i in range(modelsCount):
        if _u16(data, pModels+i*20) == oid: laraModel=i; break
    if laraModel<0: raise SystemExit("!! Lara model (type %d) not found" % oid)
    mo=pModels+laraModel*20
    mcount=_u16(data,mo+4); mstart=_u16(data,mo+6)
    node  =_u32(data,mo+8); anim  =_u16(data,mo+16)

    # MESH SUBSTITUTION (swap_oid/swap_meshes): take these mesh slots from a
    # DIFFERENT model, keeping this one's tree, nodes and animations.
    # ★ That is exactly what TR1's "draw pistols" is: LARA_PISTOLS (model 1)
    # carries real geometry ONLY in meshes 10 and 13 - the hands, with the
    # pistols modelled in - and DUMMIES (all 20v/12q/12t) everywhere else.
    # Substituting those two gives a gun-armed Lara that every one of her 160
    # animations still drives, because the skeleton never changed.
    mstart2 = None
    if swap_oid is not None and swap_meshes:
        _sm=-1
        for i in range(modelsCount):
            if _u16(data,pModels+i*20)==swap_oid: _sm=i; break
        if _sm<0: raise SystemExit("!! swap model %d not found"%swap_oid)
        mstart2=_u16(data,pModels+_sm*20+6)

    # --- per-mesh: verts + faces (topology constant across frames) ---
    meshes=[]; quads=[]; tris=[]; vbase_of=[]; total_v=0
    col_indices=set(); colored_faces=0
    for i in range(mcount):
        _ms = mstart2 if (mstart2 is not None and i in swap_meshes) else mstart
        boff=_u32(data, pMeshOff+(_ms+i)*4); base=pMeshData+boff
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

    # ---- LARA_MOVEAUDIT=1 -------------------------------------------------
    # Dump EVERY Lara animation with the engine's own motion data, so her
    # moveset can be verified against the game instead of tuned by feel.
    #  * speed/accel (16.16 at +8/+12) = horizontal velocity, units per 30 Hz
    #    TICK.  This is what fixed her walk/run speeds.
    #  * ANIM_CMD_JUMP (opcode 2) carries (velY, velZ) = the LAUNCH velocities
    #    for a jump anim.  These are the authoritative jump numbers; the hand-
    #    tuned JUMP_VEL/JUMP_FWD in main.c are guesses standing in for them.
    #  * the state-change table says which anims each state can dispatch to,
    #    i.e. the real move graph.
    if int(os.environ.get("LARA_MOVEAUDIT","0")):
        STATE_NAME={0:"WALK",1:"RUN",2:"STOP",3:"FORWARD_JUMP",4:"POSE",
            5:"FAST_BACK",6:"TURN_RIGHT",7:"TURN_LEFT",8:"DEATH",9:"FALL",
            10:"HANG",11:"REACH",12:"SPLAT",13:"TREAD",14:"LAND",15:"COMPRESS",
            16:"BACK",17:"SWIM",18:"GLIDE",19:"NULL",20:"FAST_TURN",
            21:"STEP_RIGHT",22:"STEP_LEFT",23:"ROLL",24:"SLIDE",
            25:"BACK_JUMP",26:"RIGHT_JUMP",27:"LEFT_JUMP",28:"UP_JUMP",
            29:"FALL_BACK",30:"HANG_LEFT",31:"HANG_RIGHT",32:"SLIDE_BACK",
            33:"SURF_TREAD",34:"SURF_SWIM",35:"DIVE",36:"PUSH_BLOCK",
            37:"PULL_BLOCK",38:"PUSH_PULL_READY",39:"PICK_UP",40:"SWITCH_DOWN",
            41:"SWITCH_UP",42:"USE_KEY",43:"USE_PUZZLE",44:"UNDERWATER_DEATH",
            45:"ROLL_2",46:"SPECIAL",47:"SURF_BACK",48:"SURF_LEFT",
            49:"SURF_RIGHT",50:"USE_MIDAS",51:"DIE_MIDAS",52:"SWAN_DIVE",
            53:"FAST_DIVE",54:"HANDSTAND",55:"WATER_OUT",56:"CLIMB_START",
            57:"CLIMB_UP",58:"CLIMB_LEFT",59:"CLIMB_END",60:"CLIMB_RIGHT",
            61:"CLIMB_DOWN",62:"UNUSED_1",63:"UNUSED_2",64:"UNUSED_3",
            65:"WADE",66:"WATER_ROLL",67:"PICK_UP_FLARE",68:"UNUSED_4",
            69:"UNUSED_5",70:"DEATH_SLIDE"}
        def _ma_s32(o):
            v=_u32(data,o)
            return v-(1<<32) if v>=(1<<31) else v
        def _ma_s16(o):
            v=_u16(data,o)
            return v-(1<<16) if v>=(1<<15) else v
        print("\n===== LARA MOVE AUDIT: %d animations (model anim base %d) ====="
              % (laraAnimCount, anim))
        print("%-4s %-16s %-9s %-5s %8s %9s  %s"
              % ("anim","state","frames","rate","speed","accel","commands"))
        by_state={}
        for rel in range(laraAnimCount):
            ao=pAnims+(anim+rel)*32
            st=_u16(data,ao+6); sp=_ma_s32(ao+8); ac=_ma_s32(ao+12)
            f0=_u16(data,ao+16); f1=_u16(data,ao+18)
            rate=data[ao+4]
            scC=_u16(data,ao+24); scO=_u16(data,ao+26)
            acC=_u16(data,ao+28); acO=_u16(data,ao+30)
            by_state.setdefault(st,[]).append(rel)
            # walk the anim-command stream
            cmds=[]; p=pCmds+acO*2
            for _ in range(acC):
                op=_u16(data,p); p+=2
                if   op==1: cmds.append("OFFSET(%d,%d,%d)"%(_ma_s16(p),_ma_s16(p+2),_ma_s16(p+4))); p+=6
                elif op==2: cmds.append("**JUMP velY=%d velZ=%d**"%(_ma_s16(p),_ma_s16(p+2))); p+=4
                elif op==3: cmds.append("EMPTY")
                elif op==4: cmds.append("KILL")
                elif op==5: cmds.append("SOUND(f%d,id%d)"%(_ma_s16(p),_ma_s16(p+2))); p+=4
                elif op==6: cmds.append("EFFECT(f%d,id%d)"%(_ma_s16(p),_ma_s16(p+2))); p+=4
                else: break
            print("%-4d %-16s %3d..%-3d %-5d %8.2f %9.4f  %s"
                  % (rel, STATE_NAME.get(st,"?%d"%st), f0, f1, rate,
                     sp/65536.0, ac/65536.0, " ".join(cmds)))
        print("\n----- STATE -> ANIMS -----")
        for st in sorted(by_state):
            print("  %2d %-18s anims %s"
                  % (st, STATE_NAME.get(st,"?"), by_state[st]))
        print()

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

def build_enemy(oid, nframes, data, pMeshData, pMeshOff, pAnims, pNodes,
                pFrame, pModels, modelsCount, want_state=None):
    """Bake an enemy model (by objectID) into a posed static mesh + N frames of
    its base animation, reusing Lara's mesh-tree pose walk. Returns None if the
    model is absent. Faces keep their tex id (colored if <256)."""
    bi=-1
    for i in range(modelsCount):
        if _u16(data,pModels+i*20)==oid: bi=i; break
    if bi<0: return None
    mo=pModels+bi*20
    mcount=_u16(data,mo+4); mstart=_u16(data,mo+6)
    node=_u32(data,mo+8); anim=_u16(data,mo+16)
    meshes=[]; quads=[]; tris=[]; vbase=[]; tv=0
    for m in range(mcount):
        boff=_u32(data,pMeshOff+(mstart+m)*4); base=pMeshData+boff
        vCount=_s16(data,base+10); vAbs=abs(vCount); p=base+12
        verts=[(_s16(data,p+j*8),_s16(data,p+j*8+2),_s16(data,p+j*8+4)) for j in range(vAbs)]
        p += vAbs*(8 if vCount>0 else 2) + 12 - 12   # after verts+normals/lights
        p = base+12 + vAbs*8 + (vAbs*8 if vCount>0 else vAbs*2)
        vbase.append(tv); tv+=vAbs; meshes.append(verts)
        rc=_u16(data,p); p+=2
        for _q in range(rc):
            v=[_u16(data,p),_u16(data,p+2),_u16(data,p+4),_u16(data,p+6)]
            tex=_u16(data,p+8)&0x7FFF; p+=10
            quads.append(([vbase[m]+k for k in v], tex))
        tc=_u16(data,p); p+=2
        for _t in range(tc):
            v=[_u16(data,p),_u16(data,p+2),_u16(data,p+4)]
            tex=_u16(data,p+6)&0x7FFF; p+=8
            tris.append(([vbase[m]+k for k in v], tex))
    # pose walk (same as build_lara.bake, module helpers)
    def anim_info(ai):
        ao=pAnims+ai*32; fs=data[ao+5]
        return _u32(data,ao), (fs if fs>0 else (10+2*mcount))*2
    # THE MODEL'S BASE ANIM IS NOT ALWAYS A CYCLE. The bear's is STATE_STOP
    # with ONE frame, so it baked a single static pose and slid along the floor
    # like a statue; the wolf's is state 8 (sleep), not its run. Pick the anim
    # with the MOST frames for the wanted state - selected by STATE, not by a
    # hardcoded index, so it survives a different level file.
    if want_state is not None:
        best=None
        for a in range(anim, anim+24):
            ao=pAnims+a*32
            fs=data[ao+5]; fb=(fs if fs>0 else (10+2*mcount))*2
            fo=_u32(data,ao); nx=_u32(data,pAnims+(a+1)*32)
            av=(nx-fo)//fb if nx>fo and fb>0 else 0
            if _u16(data,ao+6)==want_state and av>0 and (best is None or av>best[1]):
                best=(a,av)
        if best: anim=best[0]
    fofs,fbytes=anim_info(anim)
    nxt=_u32(data,pAnims+(anim+1)*32)
    avail=(nxt-fofs)//fbytes if nxt>fofs and fbytes>0 else 1
    nframes=max(1,min(nframes,avail))
    def bake(fidx):
        fb=pFrame+fofs+fidx*fbytes; ab=fb+18
        def angword(j): o=ab+(2*j+1)*2; return _u16(data,o),_u16(data,o+2)
        out=[]; mm=_mat_id(); stack=[]
        def emit(mat,mi):
            R,t=mat
            for (lx,ly,lz) in meshes[mi]:
                out.append((_clampi16(t[0]+R[0][0]*lx+R[0][1]*ly+R[0][2]*lz),
                            _clampi16(t[1]+R[1][0]*lx+R[1][1]*ly+R[1][2]*lz),
                            _clampi16(t[2]+R[2][0]*lx+R[2][1]*ly+R[2][2]*lz)))
        rx=_s16(data,fb+12); ry=_s16(data,fb+14); rz=_s16(data,fb+16)
        _translate_rel(mm,rx,ry,rz)
        w0,w1=angword(0); ax,ay,az=_decode_angles(w0,w1); _rot_yxz(mm,ax,ay,az); emit(mm,0)
        for i in range(1,mcount):
            nb=pNodes+(node+(i-1)*4)*4
            fl=_u32(data,nb); nx=_s32(data,nb+4); ny=_s32(data,nb+8); nz=_s32(data,nb+12)
            if fl&1: mm=stack.pop()
            if fl&2: stack.append(_mat_clone(mm))
            _translate_rel(mm,nx,ny,nz)
            w0,w1=angword(i); ax,ay,az=_decode_angles(w0,w1); _rot_yxz(mm,ax,ay,az); emit(mm,i)
        return out
    frames=[bake(f) for f in range(nframes)]
    return dict(oid=oid, vcount=tv, quads=quads, tris=tris, frames=frames,
                anim=anim)

def emit_enemy_h(OUTDIR, PREFIX, name, en):
    with open(os.path.join(OUTDIR,PREFIX+"_"+name+".h"),"w") as f:
        up=(PREFIX+"_"+name).upper()
        nf=len(en['frames']); vc=en['vcount']
        f.write("// generated - enemy %s (objectID %d), %d frames of the base anim\n"
                % (name, en['oid'], nf))
        f.write("#define %s_VCOUNT %d\n#define %s_FRAMES %d\n" % (up,vc,up,nf))
        f.write("#define %s_QCOUNT %d\n#define %s_TCOUNT %d\n"
                % (up,len(en['quads']),up,len(en['tris'])))
        f.write("static const short %s_verts[%d][%d*3] = {\n" % (up,nf,vc))
        for fr in en['frames']:
            f.write("  {"+",".join("%d"%c for v in fr for c in v)+"},\n")
        f.write("};\n")
        f.write("static const unsigned short %s_quads[%d][4] = {\n" % (up,max(1,len(en['quads']))))
        for (v,tex) in en['quads']: f.write("  {%d,%d,%d,%d},\n"%(v[0],v[1],v[2],v[3]))
        f.write("};\n")
        f.write("static const unsigned short %s_tris[%d][3] = {\n" % (up,max(1,len(en['tris']))))
        for (v,tex) in en['tris']: f.write("  {%d,%d,%d},\n"%(v[0],v[1],v[2]))
        f.write("};\n")

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
    # HORIZONTAL PORTAL (2026-07-30). sector_slant above walks straight PAST
    # func==1 to find the slant, discarding the destination room - so a doorway
    # in a WALL was indistinguishable from solid rock. Every room came out a
    # sealed box: floor==-127 with roomBelow==255 packs as 0x7FFF WALL, and only
    # VERTICAL portals (roomBelow != 255 -> 0x7FFE) stayed walkable. That is why
    # Lara could DROP into a room but never walk out of one, and why she seemed
    # to "teleport" in - she was crossing by walking off the previous room's grid
    # edge, where room_wall_at reads out-of-bounds as "not a wall" and the
    # multi-room floor search snapped her to the next room.
    # Returns the adjacent room, or None.
    def sector_portal(fidx):
        if fidx<=0 or fidx>=nfloor: return None
        i=fidx
        while True:
            cmd=floors[i]; i+=1
            func=cmd&0x1F; end=(cmd>>15)&1
            if func==1:
                return floors[i]              # destination room
            elif func==2:   i+=1
            elif func==3:   i+=1
            elif func==4:
                i+=1
                while i<nfloor and not ((floors[i]>>15)&1): i+=1
                i+=1
            elif 7<=func<=18: i+=1
            if end or i>=nfloor: break
        return None

    # The same walk again, but RETURNING the trigger instead of stepping over
    # it.  Layouts verified against format.h (union FloorData), NOT remembered:
    #   Command        func:5, tri:3, sub:7, end:1    -> sub (bits 8-14) = type
    #   TriggerInfo    timer:8, once:1, mask:5, :2
    #   TriggerCommand args:10, action:5, end:1
    # Trigger::Type  0 ACTIVATE 1 PAD 2 SWITCH 3 KEY 4 PICKUP 5 HEAVY
    #                6 ANTIPAD 7 COMBAT 8 DUMMY
    # Action::Type   0 ACTIVATE 1 CAMERA_SWITCH 2 FLOW 3 FLIP 4 FLIP_ON
    #                5 FLIP_OFF 6 CAMERA_TARGET 7 END 8 SOUNDTRACK 9 EFFECT
    #                10 SECRET 11 CLEAR_BODIES
    # ☠️ For SWITCH / KEY / PICKUP the FIRST command's args is the SWITCH (or
    # key/pickup) ENTITY INDEX, not an action — lara.h consumes it with
    # cmdIndex++ before reading any action.  CAMERA_SWITCH likewise eats the
    # NEXT word as its own parameter.  Both are kept verbatim here; decoding is
    # the runtime's job, and mis-reading either shifts every later action.
    def sector_triggers(fidx):
        if fidx<=0 or fidx>=nfloor: return None
        i=fidx
        while True:
            cmd=floors[i]; i+=1
            func=cmd&0x1F; sub=(cmd>>8)&0x7F; end=(cmd>>15)&1
            if func==4:                       # TRIGGER
                if i>=nfloor: return None
                info=floors[i]; i+=1
                cmds=[]
                while i<nfloor:
                    w=floors[i]; i+=1
                    cmds.append(w)
                    if (w>>15)&1: break
                return dict(type=sub, timer=info&0xFF, once=(info>>8)&1,
                            mask=(info>>9)&0x1F, cmds=cmds)
            elif func==1:   i+=1
            elif func==2:   i+=1
            elif func==3:   i+=1
            elif 7<=func<=18: i+=1
            if end or i>=nfloor: break
        return None
    mds=r.u32(); pMeshData=r.p; r.seek(mds*2)   # meshData (u16 words; meshOffsets are BYTE offsets into it)
    moc=r.u32(); pMeshOff=r.p;  r.seek(moc*4)   # meshOffsets (u32 each)
    an=r.u32();  pAnims=r.p;    r.seek(an*32)   # anims (32B each)
    if os.environ.get("LARA_ANIMDUMP"):
        # TR1 anim record (32 B): u32 frameOffset, u8 frameRate, u8 frameSize,
        # u16 state, s32 speed(16.16), s32 accel(16.16), u16 frameStart,
        # u16 frameEnd, ...  speed/accel are the ENGINE'S OWN velocity for the
        # animation - authoritative ground truth for Lara's travel, far better
        # than measuring pixels off a video.
        _sv=r.p
        for _ai in [int(x) for x in os.environ["LARA_ANIMDUMP"].split(",") if x!=""]:
            r.p = pAnims + _ai*32   # seek() is RELATIVE here; set p directly
            _fo=r.u32(); _fr=r.u8(); _fs=r.u8(); _st=r.u16()
            _sp=r.u32(); _ac=r.u32()
            _f0=r.u16(); _f1=r.u16()
            if _sp>=1<<31: _sp-=1<<32
            if _ac>=1<<31: _ac-=1<<32
            print("ANIM %3d: state=%2d frameRate=%d frames=%d..%d  speed=%9.3f  accel=%9.5f  (units/frame)"
                  % (_ai,_st,_fr,_f0,_f1,_sp/65536.0,_ac/65536.0))
        r.p = _sv
    nStates=r.u32(); pStates=r.p; r.seek(nStates*6)   # AnimState  6B: state, rangesCount, rangesOffset
    nRanges=r.u32(); pRanges=r.p; r.seek(nRanges*8)   # AnimRange  8B: low, high, nextAnim, nextFrame
    nCmds  =r.u32(); pCmds  =r.p; r.seek(nCmds*2)     # animCommands: int16 stream
    nds=r.u32(); pNodes=r.p;   r.seek(nds*4)    # nodesData (u32 words; model.node is a WORD index)
    fds=r.u32(); pFrame=r.p;   r.seek(fds*2)    # frameData (u16 words; frameOffset is a BYTE offset)
    mc=r.u32();  pModels=r.p;  r.seek(mc*20)    # models (20B each on PSX)
    modelsCount=mc
    nstat=r.u32(); pStatics=r.p; r.seek(nstat*32)  # staticMeshes (32B: u32 id,
    # u16 mesh, s16 vbox[6], s16 cbox[6], u16 flags — verified vs format.h)
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
    # RAMP_PAL=1: build a RAMP palette (K bases x M shades, brightest at ramp
    # offset 0) instead of baking 4 shade levels into duplicated tiles.
    # Shading then happens at RUNTIME (Blitter second pass adds a per-face /
    # per-scanline delta k to the texel index — silicon-probed 2026-07-20:
    # the intensity path (SRCSHADE/GOURD) writes zeros on 8bpp, but LFU-OR /
    # ADDDSEL arithmetic works). Kills the (tex,lvl) tile copies: ~205KB less
    # atlas at shipped params, and per-vertex light (already emitted in VERTS
    # +6) becomes usable for Gouraud. 0 = legacy byte-identical output.
    FLATFLOOR=int(os.environ.get("FLATFLOOR","0"))
    FLATMERGE=int(os.environ.get("FLATMERGE","0"))   # merge flat coplanar tiles
    RAMP_PAL=int(os.environ.get("RAMP_PAL","0"))
    RAMP_K=int(os.environ.get("RAMP_K","30"))     # base colours
    RAMP_M=int(os.environ.get("RAMP_M","8"))      # shades per base
    assert not RAMP_PAL or RAMP_K*RAMP_M<=240, "ramps must fit below slot 240"

    # ---- LARA SPAWN from the level's entity table (NOT assumed) ----
    # continue the readDataArrays walk: spriteTex -> spriteSeq -> cameras ->
    # soundSources -> boxes -> overlaps -> zones -> animTex -> entities
    # (order + record sizes per format.h loadTR1_PSX / readEntities: TR1 PSX
    # entity = type u16, room u16, x/y/z s32, rotation s16, intensity u16,
    # flags u16 = 22 bytes.)
    spawn=None
    entities=[]
    sp=r.p
    try:
        _nst = r.u32(); _pst = r.p; r.seek(_nst*16)   # spriteTextures (PSX 16B)
        _nss = r.u32(); _pss = r.p; r.seek(_nss*8)    # spriteSequences (8B)
        # MRT_SPRITEAUDIT=1: TR1 renders medikits/ammo/pickups as SPRITES, not
        # meshes - they are absent from the model table entirely, which is why
        # a model audit for 93/94 comes back empty. Report the sequences so the
        # renderer knows what it must billboard. Read-only; exits before any
        # write, so it cannot touch the shipping assets.
        if int(os.environ.get("MRT_SPRITEAUDIT","0")):
            print("\n=== SPRITE SEQUENCES (%d) ===" % _nss)
            for _i in range(_nss):
                _o = struct.unpack_from("<i", r.d, _pss+_i*8)[0]
                _l = struct.unpack_from("<h", r.d, _pss+_i*8+4)[0]
                _s = struct.unpack_from("<h", r.d, _pss+_i*8+6)[0]
                print("  objectID %3d  frames %3d  first sprite %4d" % (_o,-_l,_s))
            print("\n=== SPRITE TEXTURES: %d ===" % _nst)
            sys.exit(0)
        globals()['_SPR_P'] = _pst; globals()['_SPR_N'] = _nst

                # ---- MRT_MESHSCOPE=99: per-MESH breakdown of one model ------------
        if os.environ.get("MRT_MESHSCOPE"):
            _oid=int(os.environ["MRT_MESHSCOPE"])
            _bi=-1
            for _i in range(modelsCount):
                if _u16(data,pModels+_i*20)==_oid: _bi=_i; break
            if _bi<0: print("model %d absent"%_oid); sys.exit(0)
            _mo=pModels+_bi*20; _mc=_u16(data,_mo+4); _ms=_u16(data,_mo+6)
            print("\n=== model %d, %d meshes ===" % (_oid,_mc))
            for _m in range(_mc):
                _b=_u32(data,pMeshOff+(_ms+_m)*4); _base=pMeshData+_b
                _vc=_s16(data,_base+10); _va=abs(_vc); _p=_base+12
                _bb=None
                for _j in range(_va):
                    _x=_s16(data,_p); _y=_s16(data,_p+2); _z=_s16(data,_p+4); _p+=8
                    _bb=(_x,_y,_z,_x,_y,_z) if _bb is None else (
                        min(_bb[0],_x),min(_bb[1],_y),min(_bb[2],_z),
                        max(_bb[3],_x),max(_bb[4],_y),max(_bb[5],_z))
                _p += _va*8 if _vc>0 else _va*2
                _rc=_u16(data,_p); _p+=2+_rc*10
                _tc=_u16(data,_p)
                print("  mesh %d: verts=%3d quads=%2d tris=%2d  bbox %dx%dx%d"
                      % (_m,_va,_rc,_tc,
                         (_bb[3]-_bb[0]) if _bb else 0,
                         (_bb[4]-_bb[1]) if _bb else 0,
                         (_bb[5]-_bb[2]) if _bb else 0))
            sys.exit(0)

        # ---- MRT_GUNSCOPE=1: how big would GUN LARA be? -------------------
        # Model 1 is LARA_PISTOLS: the SAME 15-mesh tree as Lara (model 0) but
        # with the pistols modelled into the hand/thigh meshes. Report the cost
        # before committing to a design. Read-only, exits before any write.
        if int(os.environ.get("MRT_GUNSCOPE","0")):
            for _oid,_nm in ((0,"LARA"),(1,"LARA_PISTOLS")):
                _bi=-1
                for _i in range(modelsCount):
                    if _u16(data,pModels+_i*20)==_oid: _bi=_i; break
                if _bi<0: print("  model %d absent"%_oid); continue
                _mo=pModels+_bi*20
                _mc=_u16(data,_mo+4); _ms=_u16(data,_mo+6)
                _tv=0
                for _m in range(_mc):
                    _b=_u32(data,pMeshOff+(_ms+_m)*4); _base=pMeshData+_b
                    _vc=_s16(data,_base+10); _tv+=abs(_vc)
                print("  model %d %-13s meshes=%2d verts=%3d  -> %d bytes/frame"
                      " (6 shorts each)" % (_oid,_nm,_mc,_tv,_tv*6))
            print("\n  Lara ships %d baked frames today (mrt_lara.bin = %d bytes)"
                  % (0, os.path.getsize(os.path.join(OUTDIR,PREFIX+"_lara.bin"))
                     if os.path.exists(os.path.join(OUTDIR,PREFIX+"_lara.bin")) else 0))
            sys.exit(0)

    # ---- MRT_SPRITEDUMP=7,8: decode SPRITE textures and write PNGs -----------
        # TR1 renders medikits/ammo as SPRITES, not meshes (a model audit for 93/94
        # is empty; the sprite-sequence audit shows 93->sprite 7, 94->sprite 8).
        # PSX spriteTexture is 16B: int16 l,t,r,b (billboard extents in world
        # units), uint16 clut, uint16 tile, uint8 u0,v0,u1,v1 - the SAME fields the
        # objtex path already decodes, so tile_nibble/clut_rgb555 apply unchanged.
        # Read-only: writes PNGs to MRT_SPRITEDUMP_OUT and exits before any asset
        # write, so it cannot disturb the shipping tree.
        if os.environ.get("MRT_SPRITEDUMP"):
            from PIL import Image as _I
            outd = os.environ.get("MRT_SPRITEDUMP_OUT", "/tmp")
            for _si in [int(x) for x in os.environ["MRT_SPRITEDUMP"].split(",")]:
                b = _SPR_P + _si*16
                l,t,rr,bb = struct.unpack_from("<hhhh", data, b)
                clut, tile = struct.unpack_from("<HH", data, b+8)
                u0,v0,u1,v1 = struct.unpack_from("<BBBB", data, b+12)
                w = u1-u0+1; h = v1-v0+1
                print("  sprite %2d  tile %3d clut %5d  uv (%d,%d)-(%d,%d) = %dx%d"
                      "  extents l%d t%d r%d b%d" % (_si,tile,clut,u0,v0,u1,v1,w,h,l,t,rr,bb))
                im = _I.new("RGBA",(w,h))
                px = im.load()
                for yy in range(h):
                    for xx in range(w):
                        _sx = u0+xx; _sy = v0+yy
                        _o = tiles_off+tile*TILE_PAGE_BYTES+(_sy*256+_sx)//2
                        _b = data[_o]
                        _nb = (_b>>4) if (_sx & 1) else (_b & 0x0F)
                        _c = cluts_off+clut*CLUT_BYTES+_nb*2
                        _v = data[_c] | (data[_c+1]<<8)
                        r5,g5,b5 = (_v & 31), ((_v>>5) & 31), ((_v>>10) & 31)
                        px[xx,yy] = (r5*8, g5*8, b5*8,
                                     0 if (r5|g5|b5)==0 else 255)
                fn = os.path.join(outd, "sprite%d.png" % _si)
                im.resize((w*6,h*6), _I.NEAREST).save(fn)
                print("      -> %s" % fn)
            sys.exit(0)

        r.seek(r.u32()*16)             # cameras (16B)
        r.seek(r.u32()*16)             # soundSources (16B)
        nbox=r.u32(); r.seek(nbox*20)  # boxes (TR1 20B)
        r.seek(r.u32()*2)              # overlaps (u16)
        r.seek(2*3*nbox*2)             # zones TR1: 2 alts x (gnd1,gnd2,fly) x nbox u16
        r.seek(r.u32()*2)              # animTex block (u16 words)
        nent=r.u32()
        for i in range(nent):
            et=r.u16(); erm=r.u16(); ex=r.s32(); ey=r.s32(); ez=r.s32()
            erot=r.s16(); eint=r.u16(); eflg=r.u16()
            entities.append(dict(type=et, room=erm, x=ex, y=ey, z=ez,
                                 rot=erot, intensity=eint, flags=eflg))
            if et==0 and spawn is None:            # ITEM_LARA
                spawn=dict(room=erm, x=ex, y=ey, z=ez, rot=erot)
    finally:
        r.setpos(sp)
    # ---- MRT_ENTAUDIT=1: dump the entity table + FloorData triggers, STOP ---
    # ☠️ Exits BEFORE anything is written, so this can never wipe the level (a
    # bare extractor run replaces every mrt_* file and the .bin are gitignored).
    # Same pattern as LARA_MOVEAUDIT / LARA_TONEAUDIT: answer the question
    # offline instead of guessing at the runtime.
    if int(os.environ.get("MRT_ENTAUDIT","0")):
        import collections as _c
        _TN={0:"LARA",7:"ENEMY_WOLF",8:"ENEMY_BEAR",9:"ENEMY_BAT",
             35:"TRAP_FLOOR",40:"TRAP_DART_EMITTER",55:"SWITCH",
             57:"DOOR_1",58:"DOOR_2",59:"DOOR_3",60:"DOOR_4",
             68:"BRIDGE_1",69:"BRIDGE_2",70:"BRIDGE_3",83:"CRYSTAL",
             93:"MEDIKIT_SMALL",94:"MEDIKIT_BIG",169:"VIEW_TARGET"}
        _n=_c.Counter(e['type'] for e in entities)
        print("\n=== ENTITY TABLE: %d entities, %d distinct types ==="
              % (len(entities), len(_n)))
        for _t,_k in sorted(_n.items()):
            _rs=sorted(set(e['room'] for e in entities if e['type']==_t))
            print("  type %3d  x%-3d  %-18s rooms %s"
                  % (_t,_k,_TN.get(_t,"?"),_rs))
        print("\n  # type room        x       y       z    yaw  flags")
        for _i,_e in enumerate(entities):
            print("  %3d %4d %4d %8d %7d %7d %6d  %04X  %s"
                  % (_i,_e['type'],_e['room'],_e['x'],_e['y'],_e['z'],
                     (_e['rot']&0xFFFF)>>8,_e['flags'],_TN.get(_e['type'],"")))
        _TT=["ACTIVATE","PAD","SWITCH","KEY","PICKUP","HEAVY","ANTIPAD",
             "COMBAT","DUMMY"]
        _AC=["ACTIVATE","CAMERA_SWITCH","FLOW","FLIP","FLIP_ON","FLIP_OFF",
             "CAMERA_TARGET","END","SOUNDTRACK","EFFECT","SECRET","CLEAR_BODIES"]
        _cnt=0
        print("\n=== FLOORDATA TRIGGERS ===")
        for _rm in rooms_all:
            for _s,_sc in enumerate(_rm['sect']):
                _t=sector_triggers(_sc[2])
                if not _t: continue
                _cnt+=1
                _parts=[]; _k=0
                _ty=_TT[_t['type']] if _t['type']<len(_TT) else "?%d"%_t['type']
                if _t['type'] in (2,3,4):          # SWITCH / KEY / PICKUP
                    _parts.append("obj=e%d"%(_t['cmds'][0]&0x3FF)); _k=1
                while _k<len(_t['cmds']):
                    _w=_t['cmds'][_k]; _a=(_w>>10)&0x1F; _g=_w&0x3FF; _k+=1
                    _an=_AC[_a] if _a<len(_AC) else "?%d"%_a
                    if _a==0:
                        _et=entities[_g]['type'] if _g<len(entities) else -1
                        _parts.append("ACTIVATE e%d(%s)"%(_g,_TN.get(_et,_et)))
                    elif _a==1:
                        _parts.append("%s(cam %d)"%(_an,_g)); _k+=1
                    else:
                        _parts.append("%s(%d)"%(_an,_g))
                print("  room %2d sect(%2d,%2d)  %-8s once=%d timer=%-3d  %s"
                      % (_rm['i'],_s//_rm['zS'],_s%_rm['zS'],_ty,_t['once'],
                         _t['timer']," ; ".join(_parts)))
        print("  -- %d triggers"%_cnt)
        sys.exit(0)

    # ---- MRT_DOORAUDIT=1: dump the DOOR/SWITCH models, STOP ---------------
    # The PS1 footage shows TWO door looks in the Caves: carved STONE panels
    # (9m23-9m28) and lashed wooden POLE GATES (9m40-10m04).  Which of
    # DOOR_1..DOOR_4 is which cannot be told by eye, but a lattice of poles is
    # MANY faces and a slab is a handful - so the level file answers it.
    # Exits BEFORE anything is written, like MRT_ENTAUDIT.
    if int(os.environ.get("MRT_DOORAUDIT","0")):
        _WANT={55:"SWITCH",57:"DOOR_1",58:"DOOR_2",59:"DOOR_3",60:"DOOR_4"}
        print("\n=== DOOR / SWITCH MODELS (from the level file) ===")
        for i in range(modelsCount):
            oid=_u16(data,pModels+i*20)
            if oid not in _WANT: continue
            mcount=_u16(data,pModels+i*20+4); mstart=_u16(data,pModels+i*20+6)
            tv=tq=tt=0; bb=None; texs=set()
            for m in range(mcount):
                boff=_u32(data,pMeshOff+(mstart+m)*4); base=pMeshData+boff
                vCount=_s16(data,base+10); vAbs=abs(vCount); p=base+12
                for _j in range(vAbs):
                    x=_s16(data,p); y=_s16(data,p+2); z=_s16(data,p+4); p+=8
                    bb=(x,y,z,x,y,z) if bb is None else (
                        min(bb[0],x),min(bb[1],y),min(bb[2],z),
                        max(bb[3],x),max(bb[4],y),max(bb[5],z))
                p += vAbs*8 if vCount>0 else vAbs*2
                rc=_u16(data,p); p+=2
                for _q in range(rc): texs.add(_u16(data,p+8)&0x7FFF); p+=10
                tc=_u16(data,p); p+=2
                for _t in range(tc): texs.add(_u16(data,p+6)&0x7FFF); p+=8
                tv+=vAbs; tq+=rc; tt+=tc
            print("  model %3d %-8s meshes=%d verts=%3d quads=%3d tris=%3d "
                  "FACES=%3d  %d distinct tex ids"
                  % (oid,_WANT[oid],mcount,tv,tq,tt,tq+tt,len(texs)))
            if bb:
                print("      bbox size %dx%dx%d" % (bb[3]-bb[0],bb[4]-bb[1],bb[5]-bb[2]))
                # ★ WHERE the box sits relative to the ENTITY ORIGIN is the
                # whole answer to "where is the hinge": a door whose x runs
                # 0..1024 is hinged on its origin EDGE, one running -512..512
                # is centred on the sector.
                print("      bbox x[%5d..%5d] y[%5d..%5d] z[%5d..%5d]"
                      % (bb[0],bb[3],bb[1],bb[4],bb[2],bb[5]))
            # ★ door TEXTURE ids + their source objtex rects, so texturing the
            # door can reuse an atlas tile instead of the flat swatch. tex<256
            # is a FLAT COLOUR (no rect); tex>=256 is a real object-texture.
            for t in sorted(texs):
                if t < len(objtex):
                    o=objtex[t]
                    us=[u for u,_ in o['uv']]; vs=[v for _,v in o['uv']]
                    print("      tex %4d  tile=%d rect u[%d..%d] v[%d..%d] %s"
                          % (t,o['tile'],min(us),max(us),min(vs),max(vs),
                             "(FLAT COLOUR)" if t<256 else ""))
                else:
                    print("      tex %4d  (>= objCount %d)" % (t,len(objtex)))
        sys.exit(0)

    # ---- MRT_ENEMYAUDIT=1: scope the enemy models (wolf/bear/bat) ----------
    # MRT_MODELAUDIT=83,93,94 scopes ANY model ids instead - same report, same
    # early exit BEFORE anything is written, so it can never touch the shipping
    # assets (the OUTDIR-overwrite trap).
    if int(os.environ.get("MRT_ENEMYAUDIT","0")) or os.environ.get("MRT_MODELAUDIT"):
        _EN={7:"WOLF",8:"BEAR",9:"BAT"}
        if os.environ.get("MRT_MODELAUDIT"):
            _EN={int(x):("m%s"%x) for x in os.environ["MRT_MODELAUDIT"].split(",") if x.strip()}
        print("\n=== ENEMY MODELS (mesh/anim scope) ===")
        # models are ordered; anim ranges = this.anim .. next-model.anim
        mdl=[]
        for i in range(modelsCount):
            oid=_u16(data,pModels+i*20)
            mdl.append((oid, _u16(data,pModels+i*20+4), _u16(data,pModels+i*20+6),
                        _u16(data,pModels+i*20+16)))   # oid, mcount, mstart, anim
        for i,(oid,mcount,mstart,anim) in enumerate(mdl):
            if oid not in _EN: continue
            nanim = (mdl[i+1][3]-anim) if i+1<len(mdl) else 0
            tv=tq=tt=0; bb=None; texs=set(); colf=0
            for m in range(mcount):
                boff=_u32(data,pMeshOff+(mstart+m)*4); base=pMeshData+boff
                vCount=_s16(data,base+10); vAbs=abs(vCount); p=base+12
                for _j in range(vAbs):
                    x=_s16(data,p); y=_s16(data,p+2); z=_s16(data,p+4); p+=8
                    bb=(x,y,z,x,y,z) if bb is None else (
                        min(bb[0],x),min(bb[1],y),min(bb[2],z),
                        max(bb[3],x),max(bb[4],y),max(bb[5],z))
                p += vAbs*8 if vCount>0 else vAbs*2
                rc=_u16(data,p); p+=2
                for _q in range(rc):
                    tex=_u16(data,p+8)&0x7FFF; texs.add(tex); colf += (tex<256); p+=10
                tc=_u16(data,p); p+=2
                for _t in range(tc):
                    tex=_u16(data,p+6)&0x7FFF; texs.add(tex); colf += (tex<256); p+=8
                tv+=vAbs; tq+=rc; tt+=tc
            print("  model %2d %-4s meshes=%2d verts=%3d faces=%3d (%d flat-col) "
                  "anims=%d  %d tex" % (oid,_EN[oid],mcount,tv,tq+tt,colf,nanim,len(texs)))
            if bb: print("      bbox %dx%dx%d" % (bb[3]-bb[0],bb[4]-bb[1],bb[5]-bb[2]))
        print("\n  (Lara for scale: model 0)")
        sys.exit(0)

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
                      pModels, modelsCount, objCount,
                      pStates, nStates, pRanges, nRanges, pCmds, nCmds)

    # ---- MRT_GUNANIM=1: LARA_PISTOLS' ARM animations, and EXIT ------------
    # Swapping meshes 10/13 puts pistols in her fists but leaves her arms in
    # whatever the BODY animation is doing - she runs and stands with guns
    # hanging at her sides. TR1 drives the arms from the WEAPON model's own
    # animations while the body keeps its normal anim, which is what makes her
    # arms come up and point.
    # Authority is OpenLara's own wpnGetAnimIndex() in src/lara.h, not memory:
    #   pistols  AIM/HOLD = 0   PREPARE = 1   UNHOLSTER(draw) = 2   FIRE = 3
    #   (there is no holster anim - it is UNHOLSTER played BACKWARDS)
    # Arm joints are JOINT_ARM_R1..R3 = meshes 8,9,10 and ARM_L1..L3 = 11,12,13
    # (lara.h:240) - and 10/13 are exactly the hand meshes we already swap.
    # Emits only the 6 arm joints per frame: 3 anims is a few hundred bytes.
    if int(os.environ.get("MRT_GUNANIM","0")):
        gm=-1
        for i in range(modelsCount):
            if _u16(data,pModels+i*20)==1: gm=i; break
        if gm<0: raise SystemExit("!! LARA_PISTOLS (model 1) not found")
        gmo=pModels+gm*20
        gmc=_u16(data,gmo+4); ganim=_u16(data,gmo+16)
        def _ai(ai):
            ao=pAnims+ai*32
            fofs=_u32(data,ao); fs=data[ao+5]
            fw=fs if fs>0 else (10+2*gmc)
            return fofs, fw*2
        def _nframes(ai):
            ofs,fb=_ai(ai); nxt=_u32(data,pAnims+(ai+1)*32)
            return ((nxt-ofs)//fb if nxt>ofs and fb>0 else 1) or 1
        def _arm_angles(ai, fidx):
            ofs,fb=_ai(ai); base=pFrame+ofs+fidx*fb+18
            out=[]
            for j in (8,9,10,11,12,13):
                o=base+(2*j+1)*2
                ax,ay,az=_decode_angles(_u16(data,o),_u16(data,o+2))
                out.append(((ax>>2)&255,(ay>>2)&255,(az>>2)&255))
            return out
        # ☠️ VALIDATE THE READER AGAINST GROUND TRUTH BEFORE TRUSTING IT.
        # Run the exact same extraction on LARA's model, where mrt_lskin already
        # holds the right answer, and compare. A frame-size or offset mistake
        # here produces angles that are plausible-looking but wrong, and the
        # only symptom on screen is "her arms look a bit odd".
        _lm=-1
        for i in range(modelsCount):
            if _u16(data,pModels+i*20)==0: _lm=i; break
        _lanim=_u16(data,pModels+_lm*20+16)
        _at=lara['anim_table']; _sf=lara['skin_frames']
        _bad=0; _tot=0
        for _a in range(min(4,len(_at))):
            _st,_n,_,_ = _at[_a]
            for _f in range(min(_n,4)):
                got=_arm_angles(_lanim+_a,_f)
                ref=_sf[_st+_f][1]
                for _k,_j in enumerate((8,9,10,11,12,13)):
                    _tot+=3
                    for _c in range(3):
                        if got[_k][_c]!=ref[_j][_c]: _bad+=1
        print("  GUNANIM self-check vs mrt_lskin: %d/%d angle bytes MISMATCH" % (_bad,_tot))
        if _bad:
            raise SystemExit("!! the arm-angle reader disagrees with Lara's own "
                             "skin data - fix the frame stride/offset before "
                             "emitting gun animations")
        for _who,_base,_mc in (("LARA",_lanim,15),("PISTOLS",ganim,gmc)):
            for _a in range(4):
                _ao=pAnims+(_base+_a)*32
                _fs=data[_ao+5]; _fofs=_u32(data,_ao)
                _nx=_u32(data,pAnims+(_base+_a+1)*32)
                _fb=(_fs if _fs>0 else (10+2*_mc))*2
                print("    %-8s anim %3d: frameSize=%2d words  fb=%3d B  ofs=%6d  next=%6d  -> %d frames"
                      % (_who,_base+_a,_fs,_fb,_fofs,_nx,((_nx-_fofs)//_fb) if _nx>_fofs and _fb else 0))
        NAMES=[("AIM",0),("DRAW",2),("FIRE",3)]
        L=["// generated by MRT_GUNANIM - LARA_PISTOLS (model 1) ARM animations.",
           "// 6 arm joints per frame: meshes 8,9,10 (right) then 11,12,13 (left),",
           "// each 3 bytes of 8-bit YXZ angle - the same encoding as mrt_lskin.",
           "// TR1 plays these on the ARMS while the body keeps its own animation.",
           ""]
        for nm,rel in NAMES:
            n=_nframes(ganim+rel)
            L.append("#define GA_%s_N %d" % (nm,n))
            L.append("static const unsigned char GA_%s[%d][18] = {" % (nm,n))
            for f in range(n):
                a=_arm_angles(ganim+rel,f)
                L.append("  {"+",".join(str(v) for jm in a for v in jm)+"},")
            L.append("};")
            print("  GUNANIM %-5s anim %d (+%d): %d frames" % (nm,ganim+rel,rel,n))
        open(os.path.join(OUTDIR,PREFIX+"_gunanim.h"),"w").write("\n".join(L)+"\n")
        print("  -> %s_gunanim.h (model 1 mcount=%d animbase=%d)" % (PREFIX,gmc,ganim))
        return

    # ---- MRT_GUNONLY=1: bake GUN LARA (model 1) and EXIT ------------------
    # ☠️ SURGICAL, like MRT_ENEMYONLY. A full regen is what corrupts Lara -
    # this extractor no longer reproduces the shipping 8568 mesh - so this must
    # never fall through into one. It writes mrt_gun.bin/.h ONLY.
    # LARA_PISTOLS shares Lara's 15-mesh tree and differs by 36 vertices (the
    # pistols in her hands and the holsters), so the runtime can keep every
    # animation and just point at the other vertex/face tables.
    if int(os.environ.get("MRT_GUNONLY","0")):
        gun = build_lara(data, pMeshData, pMeshOff, pAnims, pNodes, pFrame,
                         pModels, modelsCount, objCount,
                         pStates, nStates, pRanges, nRanges, pCmds, nCmds,
                         oid=0, swap_oid=1, swap_meshes=(10, 13))
        # ☠️ DO THE GUN FACES REFERENCE TEXTURES THE ATLAS ACTUALLY HAS?
        # The atlas is packed from what the ROOMS and LARA use. Model 1's hand
        # meshes are extra geometry with their own objtex ids, and if those are
        # not packed the pistols render garbage texels - which would look like
        # a UV bug and cost a day.
        _lt=set(); _gt=set()
        for _f in lara['quads']+lara['tris']:
            if not _f['colored']: _lt.add(_f['tex'])
        for _f in gun['quads']+gun['tris']:
            if not _f['colored']: _gt.add(_f['tex'])
        _new=_gt-_lt
        print("  gun textured-face objtex: %d, of which NOT used by Lara: %d %s"
              % (len(_gt), len(_new), sorted(_new)[:12]))
        # narrow it to the SWAPPED meshes: only 10 and 13 are new geometry
        _vb=gun['vbase_of']
        def _mesh_of(_v):
            for _m in range(len(_vb)-1,-1,-1):
                if _v>=_vb[_m]: return _m
            return 0
        _ht=set(); _hc=0; _hn=0
        for _f in gun['quads']+gun['tris']:
            if _mesh_of(_f['v'][0]) in (10,13):
                _hn+=1
                if _f['colored']: _hc+=1
                else: _ht.add(_f['tex'])
        print("  HAND meshes (10,13): %d faces, %d flat-colour, textures %s"
              % (_hn,_hc,sorted(_ht)))
        print("  of those textures, missing from Lara's atlas: %s"
              % sorted(_ht-_lt))
        _gc=sum(1 for _f in gun['quads']+gun['tris'] if _f['colored'])
        print("  gun FLAT-COLOUR faces: %d of %d"
              % (_gc, len(gun['quads'])+len(gun['tris'])))
        print("GUN LARA: %d verts, %dq %dt, %d frames"
              % (gun['vcount'], len(gun['quads']), len(gun['tris']),
                 gun['framecount']))
        print("  vs Lara: %d verts, %dq %dt, %d frames"
              % (lara['vcount'], len(lara['quads']), len(lara['tris']),
                 lara['framecount']))
        if gun['framecount'] != lara['framecount']:
            print("  ☠️ FRAME COUNTS DIFFER - the runtime cannot share the"
                  " animation table; a swap would desync her pose")
        sys.exit(0)

    # MRT_ANIMPROBE=1: which animation does each enemy model point at, and how
    # many frames does it actually have? The BEAR bakes only ONE frame even
    # though 6 are requested, so its base anim is a static pose and the model
    # slides along the floor. Read-only; exits before any write.
    if int(os.environ.get("MRT_ANIMPROBE","0")):
        for _oid,_nm in ((9,"bat"),(7,"wolf"),(8,"bear")):
            _bi=-1
            for i in range(modelsCount):
                if _u16(data,pModels+i*20)==_oid: _bi=i; break
            if _bi<0: print("%s: model absent"%_nm); continue
            _mo=pModels+_bi*20
            _mc=_u16(data,_mo+4); _a0=_u16(data,_mo+16)
            print("%-5s model %d  meshes %d  base anim %d" % (_nm,_bi,_mc,_a0))
            for _a in range(_a0, _a0+10):
                _ao=pAnims+_a*32
                _fo=_u32(data,_ao); _fs=data[_ao+5]
                _fb=(_fs if _fs>0 else (10+2*_mc))*2
                _nx=_u32(data,pAnims+(_a+1)*32)
                _av=(_nx-_fo)//_fb if _nx>_fo and _fb>0 else 0
                _st=_u16(data,_ao+6)
                print("    anim %3d  state %2d  frames %3d" % (_a,_st,_av))
        sys.exit(0)

    # ---- ENEMIES: bake the BAT (simplest model) - fly-cycle posed frames ----
    # state per model: bat 1 = its fly cycle; wolf/bear 3 = RUN (OpenLara
    # src/enemy.h Wolf/Bear STATE_ enums).
    for _eoid, _ename, _enf, _est in ((9,"bat",8,None),(7,"wolf",6,3),(8,"bear",6,3)):
        _en = build_enemy(_eoid, _enf, data, pMeshData, pMeshOff, pAnims,
                          pNodes, pFrame, pModels, modelsCount, _est)
        if _en:
            emit_enemy_h(OUTDIR, PREFIX, _ename, _en)
            print("%s: %d verts, %dq %dt, %d frames (anim %d) -> %s_%s.h"
                  % (_ename.upper(), _en['vcount'], len(_en['quads']),
                     len(_en['tris']), len(_en['frames']), _en['anim'],
                     PREFIX, _ename))
    # MRT_ENEMYONLY=1: the enemy headers are all we wanted - EXIT before the
    # full regen, which is what corrupts Lara and the level.
    if int(os.environ.get("MRT_ENEMYONLY","0")):
        sys.exit(0)

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

    # ---- STATICS=1: bake static-mesh placements into room geometry ---------
    # Runs BEFORE the texture union / palette / subdivision passes, so the
    # baked faces flow through the whole pipeline (atlas packing, RAMP k,
    # FACE_PLANES prefix, painter sort) exactly like native room faces.
    if STATICS:
        smtab={}                        # staticID -> mesh index (global table)
        for _i in range(nstat):
            _o=pStatics+_i*32
            smtab[_u32(data,_o)]=_u16(data,_o+4)
        def parse_smesh(midx):
            # mesh layout = Lara's (build_lara): 12B header {cx,cy,cz,radius,
            # flags,vCount}, short4 world-unit coords, normals-or-intensity,
            # rCount quads {v0..v3,flags}, tCount tris {v0..v2,flags}.
            boff=_u32(data,pMeshOff+midx*4); base=pMeshData+boff
            vCount=_s16(data,base+10); vAbs=abs(vCount); p=base+12
            mverts=[]
            for _j in range(vAbs):
                mverts.append((_s16(data,p),_s16(data,p+2),_s16(data,p+4))); p+=8
            p += vAbs*8 if vCount>0 else vAbs*2      # normals / intensity
            rc=_u16(data,p); p+=2; mq=[]
            for _q in range(rc):
                v=[_u16(data,p),_u16(data,p+2),_u16(data,p+4),_u16(data,p+6)]
                fl=_u16(data,p+8); p+=10
                mq.append((v,fl&0x7FFF))
            tc=_u16(data,p); p+=2; mt=[]
            for _t in range(tc):
                v=[_u16(data,p),_u16(data,p+2),_u16(data,p+4)]
                fl=_u16(data,p+6); p+=8
                mt.append((v,fl&0x7FFF))
            return mverts,mq,mt
        smesh={sid:parse_smesh(mi) for sid,mi in smtab.items()}
        n_placed=0; n_missing=0
        for rm in rooms:
            for s in rm.get('statics',[]):
                if s['mid'] not in smesh:
                    n_missing+=1; continue
                mverts,mq,mt=smesh[s['mid']]
                lx=s['x']-rm['info_x']; ly=s['y']; lz=s['z']-rm['info_z']
                # placement intensity -> brightness 0..255 (format.h TR1
                # conversion: 0 = bright, 0x1FFF = dark, >0x1FFF = full bright)
                lit=255 if s['inten']>0x1FFF else max(0,min(255,255-(s['inten']>>5)))
                # Y-rotation. All Caves placements are quadrant multiples
                # (rot in {0,16384,32768,49152}) -> exact integer rotation;
                # float rotY fallback otherwise. Convention verified against
                # format.h Box::rotate90 and our Lara _rot_axis: 90deg ->
                # x' = z, z' = -x.
                qrot=s['rot']&0xFFFF
                if qrot%16384==0:
                    kq=qrot>>14
                    def rot(x,z,_k=kq):
                        return ((x,z),(z,-x),(-x,-z),(-z,x))[_k]
                else:
                    _a=qrot/65536.0*2*math.pi
                    _c=math.cos(_a); _s=math.sin(_a)
                    def rot(x,z,_c=_c,_s=_s):
                        return (int(round(x*_c+z*_s)), int(round(-x*_s+z*_c)))
                vb=len(rm['verts'])
                for (x,y,z) in mverts:
                    rx,rz=rot(x,z)
                    vx,vy,vz=lx+rx, ly+y, lz+rz
                    if not (-32768<=vx<=32767 and -32768<=vy<=32767 and -32768<=vz<=32767):
                        print("!! STATICS: s16 overflow room %d id %d v=(%d,%d,%d)"
                              % (rm['i'], s['mid'], vx, vy, vz))
                        vx=max(-32768,min(32767,vx)); vy=max(-32768,min(32767,vy))
                        vz=max(-32768,min(32767,vz))
                    rm['verts'].append((vx,vy,vz,lit))
                # Mesh faces keep FILE winding + order (no v2<->v3 swap: mesh
                # quads are initMesh-style, same as Lara's — the room-quad
                # swap would invert winding). TR statics' two-sided surfaces
                # (plants etc.) exist as mirrored face PAIRS in the data, so
                # the kernel backface cull keeps exactly the visible side.
                for (v,tex) in mq:
                    rm['quads'].append(dict(v=[vb+_i2 for _i2 in v],tex=tex,swapped=False))
                for (v,tex) in mt:
                    rm['tris'].append(dict(v=[vb+_i2 for _i2 in v],tex=tex))
                n_placed+=1
        print("STATICS: baked %d placements (%d meshes; %d missing IDs) into rooms"
              % (n_placed, len(smesh), n_missing))
        for rm in rooms:
            if rm.get('statics') and len(rm['verts'])>500:
                print("  !! room %d pre-subdiv verts %d > 500 (needs make STATICS=1 vtxcache bump)"
                      % (rm['i'], len(rm['verts'])))

    # ---- decoders ----
    def clut_rgb555(ci,nib):
        o=cluts_off+ci*CLUT_BYTES+nib*2
        v=data[o]|(data[o+1]<<8)
        return (v&31,(v>>5)&31,(v>>10)&31,(v>>15)&1)
    def tile_nibble(ti,x,y):
        o=tiles_off+ti*TILE_PAGE_BYTES+(y*256+x)//2
        b=data[o]; return (b>>4) if (x&1) else (b&0x0F)
    def jag16(r5,g5,b5): return ((r5&31)<<11)|((b5&31)<<6)|((g5&31)<<1)

    # ---- MRT_DOORPATCH=1: append door/lever textures to the EXISTING atlas ---
    # A SURGICAL patch that never runs the full regen (which corrupts Lara —
    # this extractor no longer reproduces the shipping 8568 mesh). It reuses
    # only the level-file decoders above, reads the SHIPPING mrt_pal.bin +
    # mrt_atlas.bin, appends the door face (objtex 897) and lever (896) as NEW
    # rows, and writes them back + a mrt_door.h header. The GPU kernel ignores
    # the atlas HEIGHT (gpu_geotex.gas:550 skips it; atlasW comes from a param,
    # v is never bounds-checked), so appended rows sample fine with no change to
    # atlasH, the g_swy swatch, geom, or Lara. Touches ONLY mrt_atlas.bin +
    # mrt_door.h. Exits before any other write.
    if int(os.environ.get("MRT_DOORPATCH","0")):
        AW=256
        pal_b=open(os.path.join(OUTDIR,PREFIX+"_pal.bin"),"rb").read()
        pal=[(pal_b[i*2]<<8)|pal_b[i*2+1] for i in range(256)]     # u16 BE
        pal_rgb=[((c>>11)&31,(c>>1)&31,(c>>6)&31) for c in pal]    # jag16 decode
        def nearest(r5,g5,b5):
            bi=0; bd=1<<30
            for i in range(256):
                pr,pg,pb=pal_rgb[i]
                d=(r5-pr)**2+(g5-pg)**2+(b5-pb)**2
                if d<bd: bd=d; bi=i
            return bi
        atl=bytearray(open(os.path.join(OUTDIR,PREFIX+"_atlas.bin"),"rb").read())
        assert len(atl)%AW==0, "atlas not a whole number of rows"
        H0=len(atl)//AW
        rects={}
        for nm,tid in (("DOOR",897),("SW",896)):
            if tid>=len(objtex): print("DOORPATCH: objtex %d missing"%tid); continue
            o=objtex[tid]; ts=o.get('ts',1)
            us=[p[0] for p in o['uv']]; vs=[p[1] for p in o['uv']]
            u0,v0,u1,v1=min(us),min(vs),max(us),max(vs)
            w=u1-u0+1; h=v1-v0+1; ay=len(atl)//AW
            atl+=bytearray(AW*h)
            for yy in range(h):
                for xx in range(w):
                    r5,g5,b5,a=clut_rgb555(o['clut'],
                        tile_nibble(o['tile'],(u0+xx)*ts,(v0+yy)*ts))
                    atl[(ay+yy)*AW+xx]=nearest(r5,g5,b5)
            rects[nm]=(1,ay+1,w-2,ay+h-2)      # inset 1 texel vs edge bleed
            print("DOORPATCH %-4s objtex %d -> atlas rows %d..%d (%dx%d)"
                  % (nm,tid,ay,ay+h-1,w,h))
        open(os.path.join(OUTDIR,PREFIX+"_atlas.bin"),"wb").write(atl)
        with open(os.path.join(OUTDIR,PREFIX+"_door.h"),"w") as f:
            f.write("// generated by MRT_DOORPATCH - door/lever atlas UV rects\n")
            f.write("// appended to %s_atlas.bin at rows %d.. (kernel ignores atlasH)\n"
                    % (PREFIX,H0))
            up=PREFIX.upper()
            for nm in ("DOOR","SW"):
                if nm in rects:
                    x0,y0,x1,y1=rects[nm]
                    f.write("#define %s_%s_TEX_U0 %d\n"%(up,nm,x0))
                    f.write("#define %s_%s_TEX_V0 %d\n"%(up,nm,y0))
                    f.write("#define %s_%s_TEX_U1 %d\n"%(up,nm,x1))
                    f.write("#define %s_%s_TEX_V1 %d\n"%(up,nm,y1))
        print("DOORPATCH: wrote %s_atlas.bin (%d rows) + %s_door.h"
              % (PREFIX,len(atl)//AW,PREFIX))
        sys.exit(0)

    # ---- MRT_GUNPATCH=1: the GUN HANDS -> atlas rows + mrt_gun.h ------------
    # LARA_PISTOLS (model 1) carries real geometry in ONLY meshes 10 and 13 -
    # the hands with the pistols modelled in (24 verts / 17 quads each; her bare
    # hands are 8/6). Everything else in that model is a dummy. So "draw
    # pistols" is those two meshes, and the runtime can hang them off the hand
    # matrices the skinner already computes instead of rebuilding her skin.
    # ☠️ Their textures are NOT in the atlas: the atlas is packed from what the
    # ROOMS and LARA use, and objtex 529,532..540 are neither. Appended here,
    # same surgical contract as MRT_DOORPATCH: read the shipping pal+atlas,
    # append rows, write back, emit the header, EXIT before anything else.
    if int(os.environ.get("MRT_GUNPATCH","0")):
        AW=256
        STEP=int(os.environ.get("MRT_GUNPATCH_STEP","1"))
        pal_b=open(os.path.join(OUTDIR,PREFIX+"_pal.bin"),"rb").read()
        pal=[(pal_b[i*2]<<8)|pal_b[i*2+1] for i in range(256)]
        pal_rgb=[((c>>11)&31,(c>>1)&31,(c>>6)&31) for c in pal]
        def nearest(r5,g5,b5):
            bi=0; bd=1<<30
            for i in range(256):
                pr,pg,pb=pal_rgb[i]
                d=(r5-pr)**2+(g5-pg)**2+(b5-pb)**2
                if d<bd: bd=d; bi=i
            return bi
        # --- read model 1 meshes 10 and 13 straight out of the level ---
        _bi=-1
        for i in range(modelsCount):
            if _u16(data,pModels+i*20)==1: _bi=i; break
        if _bi<0: raise SystemExit("!! LARA_PISTOLS (model 1) not found")
        _ms=_u16(data,pModels+_bi*20+6)
        hands={}
        for hi,mi in ((0,10),(1,13)):
            boff=_u32(data,pMeshOff+(_ms+mi)*4); base=pMeshData+boff
            vC=_s16(data,base+10); vA=abs(vC); p=base+12
            vs=[]
            for j in range(vA):
                vs.append((_s16(data,p),_s16(data,p+2),_s16(data,p+4))); p+=8
            p += vA*8 if vC>0 else vA*2
            rc=_u16(data,p); p+=2; qs=[]
            for q in range(rc):
                v=[_u16(data,p),_u16(data,p+2),_u16(data,p+4),_u16(data,p+6)]
                fl=_u16(data,p+8); p+=10
                qs.append((v, fl&0x7FFF))
            tc=_u16(data,p); p+=2; ts=[]
            for t in range(tc):
                v=[_u16(data,p),_u16(data,p+2),_u16(data,p+4)]
                fl=_u16(data,p+6); p+=8
                ts.append((v, fl&0x7FFF))
            hands[hi]=dict(verts=vs,quads=qs,tris=ts)
            print("GUNPATCH hand %d (mesh %d): %d verts %dq %dt"
                  % (hi,mi,len(vs),len(qs),len(ts)))
        # --- append every textured objtex these faces use ---
        atl=bytearray(open(os.path.join(OUTDIR,PREFIX+"_atlas.bin"),"rb").read())
        assert len(atl)%AW==0
        H0=len(atl)//AW
        need=set()
        for h in hands.values():
            for (v,tex) in h['quads']+h['tris']:
                if tex>=256 and tex<len(objtex): need.add(tex)
        rects={}; px=0; ay=len(atl)//AW; band=0; rows_added=0
        for tex in sorted(need):
            o=objtex[tex]; ts_=o.get('ts',1)
            us=[q[0] for q in o['uv']]; vs_=[q[1] for q in o['uv']]
            u0,v0,u1,v1=min(us),min(vs_),max(us),max(vs_)
            w=(u1-u0)//STEP+1; h=(v1-v0)//STEP+1
            if w>64 or h>64:          # ☠️ the junk-4th-corner trap, again
                us=us[:3]; vs_=vs_[:3]
                u0,v0,u1,v1=min(us),min(vs_),max(us),max(vs_)
                w=(u1-u0)//STEP+1; h=(v1-v0)//STEP+1
            if px+w>AW:               # shelf full - start a new band
                ay+=band; atl+=bytearray(AW*band); rows_added+=band
                px=0; band=0
            if h>band:
                atl+=bytearray(AW*(h-band)); rows_added+=(h-band); band=h
            for yy in range(h):
                for xx in range(w):
                    sx=(u0+xx*STEP)*ts_; sy=(v0+yy*STEP)*ts_
                    _o=tiles_off+o['tile']*TILE_PAGE_BYTES+(sy*256+sx)//2
                    _b=data[_o]
                    nib=(_b>>4) if (sx & 1) else (_b & 0x0F)
                    _c=cluts_off+o['clut']*CLUT_BYTES+nib*2
                    _v=data[_c]|(data[_c+1]<<8)
                    atl[(ay+yy)*AW+px+xx]=nearest(_v&31,(_v>>5)&31,(_v>>10)&31)
            rects[tex]=(px,ay,u0,v0)
            px+=w
        open(os.path.join(OUTDIR,PREFIX+"_atlas.bin"),"wb").write(atl)
        print("GUNPATCH: %d objtex -> +%d atlas rows (now %d)"
              % (len(need),rows_added,len(atl)//AW))
        def guv(tex,n):
            """0xFFFF = flat-colour face; main.c falls back to a swatch."""
            if tex not in rects: return [0xFFFF]*(n*2)
            px0,py0,u0,v0=rects[tex]; o=objtex[tex]; out=[]
            for i in range(n):
                u,v=o['uv'][i] if i<len(o['uv']) else o['uv'][-1]
                out += [px0+(u-u0)//STEP, py0+(v-v0)//STEP]
            return out
        with open(os.path.join(OUTDIR,PREFIX+"_gun.h"),"w") as f:
            f.write("// generated by MRT_GUNPATCH - Lara's PISTOL HANDS.\n")
            f.write("// LARA_PISTOLS (model 1) meshes 10 (right) and 13 (left):\n")
            f.write("// the hand WITH the gun. Drawn at the hand matrices the\n")
            f.write("// skinner already computes, so her skin is untouched.\n")
            f.write("// Textures appended to %s_atlas.bin at rows %d..\n"
                    % (PREFIX,H0))
            up=PREFIX.upper()
            for hi in (0,1):
                h=hands[hi]; nm="R" if hi==0 else "L"
                f.write("#define %s_GUN%s_VCOUNT %d\n"%(up,nm,len(h['verts'])))
                f.write("#define %s_GUN%s_QCOUNT %d\n"%(up,nm,len(h['quads'])))
                f.write("#define %s_GUN%s_TCOUNT %d\n"%(up,nm,len(h['tris'])))
                f.write("static const short %s_GUN%s_v[%d][3] = {\n"%(up,nm,max(1,len(h['verts']))))
                for (x,y,z) in h['verts']: f.write("  {%d,%d,%d},\n"%(x,y,z))
                if not h['verts']: f.write("  {0,0,0},\n")
                f.write("};\n")
                f.write("static const unsigned short %s_GUN%s_q[%d][4] = {\n"%(up,nm,max(1,len(h['quads']))))
                for (v,tex) in h['quads']: f.write("  {%d,%d,%d,%d},\n"%tuple(v))
                if not h['quads']: f.write("  {0,0,0,0},\n")
                f.write("};\n")
                f.write("static const unsigned short %s_GUN%s_quv[%d][8] = {\n"%(up,nm,max(1,len(h['quads']))))
                for (v,tex) in h['quads']: f.write("  {"+",".join("%d"%c for c in guv(tex,4))+"},\n")
                if not h['quads']: f.write("  {65535,65535,65535,65535,65535,65535,65535,65535},\n")
                f.write("};\n")
                f.write("static const unsigned short %s_GUN%s_t[%d][3] = {\n"%(up,nm,max(1,len(h['tris']))))
                for (v,tex) in h['tris']: f.write("  {%d,%d,%d},\n"%tuple(v))
                if not h['tris']: f.write("  {0,0,0},\n")
                f.write("};\n")
                f.write("static const unsigned short %s_GUN%s_tuv[%d][6] = {\n"%(up,nm,max(1,len(h['tris']))))
                for (v,tex) in h['tris']: f.write("  {"+",".join("%d"%c for c in guv(tex,3))+"},\n")
                if not h['tris']: f.write("  {65535,65535,65535,65535,65535,65535},\n")
                f.write("};\n")
        print("GUNPATCH: wrote %s_gun.h" % PREFIX)
        sys.exit(0)

    # ---- MRT_PICKPATCH=1: append the PICKUP SPRITES to the EXISTING atlas ---
    # Same surgical contract as MRT_DOORPATCH: read the SHIPPING pal + atlas,
    # append rows, write back, EXIT before anything else. TR1 draws medikits as
    # SPRITES (93 -> sprite 7, 94 -> sprite 8), so there is no mesh to extract -
    # the runtime billboards a quad onto these rects.
    # ☠️ SHELF-PACKED side by side, not a band each: at full res the two are
    # 104x48 and 88x64 = 112 rows = 28KB of atlas, and the atlas is LINKED INTO
    # THE ROM. Packed, and at MRT_PICKPATCH_STEP=2 (half res, the same call the
    # enemy skins made - a medikit is a few dozen pixels on screen), it is one
    # 32-row band = 8KB.
    if int(os.environ.get("MRT_PICKPATCH","0")):
        AW=256
        STEP=int(os.environ.get("MRT_PICKPATCH_STEP","2"))
        pal_b=open(os.path.join(OUTDIR,PREFIX+"_pal.bin"),"rb").read()
        pal=[(pal_b[i*2]<<8)|pal_b[i*2+1] for i in range(256)]
        pal_rgb=[((c>>11)&31,(c>>1)&31,(c>>6)&31) for c in pal]
        def nearest(r5,g5,b5):
            bi=0; bd=1<<30
            for i in range(256):
                pr,pg,pb=pal_rgb[i]
                d=(r5-pr)**2+(g5-pg)**2+(b5-pb)**2
                if d<bd: bd=d; bi=i
            return bi
        atl=bytearray(open(os.path.join(OUTDIR,PREFIX+"_atlas.bin"),"rb").read())
        assert len(atl)%AW==0, "atlas not a whole number of rows"
        H0=len(atl)//AW
        spr={}
        for nm,si in (("SM",7),("BG",8)):
            b=_SPR_P+si*16
            l,t,rr,bb=struct.unpack_from("<hhhh",data,b)
            clut,tile=struct.unpack_from("<HH",data,b+8)
            u0,v0,u1,v1=struct.unpack_from("<BBBB",data,b+12)
            # ☠️ TRIM TO THE ARTWORK.  The atlas has no alpha and the kernel
            # does not colour-key, so every transparent texel in the sprite's
            # surround renders as SOLID BLACK - the first silicon roll showed
            # the medikit inside a black rectangle. TR1 PSX marks transparency
            # as CLUT index 0, so shrink the source rect to the bounding box of
            # non-zero texels and carry the SAME proportional trim into the
            # world extents, or the billboard would be the right art at the
            # wrong size and offset.
            def _nz(sx,sy):
                _o=tiles_off+tile*TILE_PAGE_BYTES+(sy*256+sx)//2
                _b=data[_o]
                return ((_b>>4) if (sx & 1) else (_b & 0x0F)) != 0
            a0,b0,a1,b1 = u1,v1,u0,v0
            for _y in range(v0,v1+1):
                for _x in range(u0,u1+1):
                    if _nz(_x,_y):
                        if _x<a0: a0=_x
                        if _x>a1: a1=_x
                        if _y<b0: b0=_y
                        if _y>b1: b1=_y
            if a0>a1 or b0>b1: a0,b0,a1,b1 = u0,v0,u1,v1   # all-transparent guard
            fw=float(u1-u0+1); fh=float(v1-v0+1)
            nl = l + int(round((rr-l)*(a0-u0)/fw))
            nr = l + int(round((rr-l)*(a1-u0+1)/fw))
            nt = t + int(round((bb-t)*(b0-v0)/fh))
            nb2= t + int(round((bb-t)*(b1-v0+1)/fh))
            print("  %s trim uv (%d,%d)-(%d,%d) -> (%d,%d)-(%d,%d)  "
                  "world l%d t%d r%d b%d -> l%d t%d r%d b%d"
                  % (nm,u0,v0,u1,v1,a0,b0,a1,b1,l,t,rr,bb,nl,nt,nr,nb2))
            l,t,rr,bb = nl,nt,nr,nb2
            u0,v0,u1,v1 = a0,b0,a1,b1
            spr[nm]=dict(l=l,t=t,r=rr,b=bb,clut=clut,tile=tile,
                         u0=u0,v0=v0,w=(u1-u0)//STEP+1,h=(v1-v0)//STEP+1)
        band=max(v["h"] for v in spr.values())
        ay=len(atl)//AW
        atl+=bytearray(AW*band)
        px=0; rects={}
        for nm in ("SM","BG"):
            o=spr[nm]
            assert px+o["w"]<=AW, "pickup shelf overflow"
            for yy in range(o["h"]):
                for xx in range(o["w"]):
                    sx=o["u0"]+xx*STEP; sy=o["v0"]+yy*STEP
                    _o=tiles_off+o["tile"]*TILE_PAGE_BYTES+(sy*256+sx)//2
                    _b=data[_o]
                    nib=(_b>>4) if (sx & 1) else (_b & 0x0F)
                    _c=cluts_off+o["clut"]*CLUT_BYTES+nib*2
                    _v=data[_c]|(data[_c+1]<<8)
                    r5,g5,b5=(_v&31),((_v>>5)&31),((_v>>10)&31)
                    atl[(ay+yy)*AW+px+xx]=nearest(r5,g5,b5)
            rects[nm]=(px,ay,px+o["w"]-1,ay+o["h"]-1)
            print("PICKPATCH %-2s sprite %d -> atlas (%d,%d)-(%d,%d)  world %dx%d"
                  % (nm,7 if nm=="SM" else 8,rects[nm][0],rects[nm][1],
                     rects[nm][2],rects[nm][3],o["r"]-o["l"],o["b"]-o["t"]))
            px+=o["w"]
        open(os.path.join(OUTDIR,PREFIX+"_atlas.bin"),"wb").write(atl)
        up=PREFIX.upper()
        with open(os.path.join(OUTDIR,PREFIX+"_pick.h"),"w") as f:
            f.write("// generated by MRT_PICKPATCH - medikit SPRITE atlas rects\n")
            f.write("// TR1 renders 93/94 as sprites, not meshes; the runtime\n")
            f.write("// billboards a quad onto these. World extents are the\n")
            f.write("// sprite's own l/t/r/b, in TR world units.\n")
            for nm in ("SM","BG"):
                x0,y0,x1,y1=rects[nm]; o=spr[nm]
                f.write("#define %s_PICK_%s_U0 %d\n"%(up,nm,x0))
                f.write("#define %s_PICK_%s_V0 %d\n"%(up,nm,y0))
                f.write("#define %s_PICK_%s_U1 %d\n"%(up,nm,x1))
                f.write("#define %s_PICK_%s_V1 %d\n"%(up,nm,y1))
                f.write("#define %s_PICK_%s_L  %d\n"%(up,nm,o["l"]))
                f.write("#define %s_PICK_%s_T  %d\n"%(up,nm,o["t"]))
                f.write("#define %s_PICK_%s_R  %d\n"%(up,nm,o["r"]))
                f.write("#define %s_PICK_%s_B  %d\n"%(up,nm,o["b"]))
        print("PICKPATCH: wrote %s_atlas.bin (%d rows, +%d) + %s_pick.h"
              % (PREFIX,len(atl)//AW,band,PREFIX))
        sys.exit(0)

    # ---- MRT_ENEMYTEX=1: append the ENEMY skins to the EXISTING atlas -------
    # Same surgical contract as MRT_DOORPATCH above: reuse only the decoders,
    # read the SHIPPING mrt_pal.bin + mrt_atlas.bin, append rows, write back,
    # and EXIT before anything else is written. A full regen is what corrupts
    # Lara, so this must never fall through into one.
    #
    # WHY THIS IS NEEDED: build_enemy already keeps every face's real texture
    # id ("Faces keep their tex id"), but emit_enemy_h DISCARDS it and writes
    # vertex indices only - so every enemy sampled one flat swatch cell and a
    # bat, a wolf and a bear were the same grey silhouette.
    #
    # ☠️ SHELF-PACKED, not one-texture-per-row-band like DOORPATCH. The enemies
    # use dozens of objtex; giving each its own full 256-wide band would add
    # ~160KB to an image whose __bss_end is already within a few hundred bytes
    # of the 0x1FC000 guard. Packing them side by side keeps it to a few KB.
    ENTEX_STEP = int(os.environ.get("MRT_ENEMYTEX_STEP","2"))
    if int(os.environ.get("MRT_ENEMYTEX","0")):
        AW=256
        pal_b=open(os.path.join(OUTDIR,PREFIX+"_pal.bin"),"rb").read()
        pal=[(pal_b[i*2]<<8)|pal_b[i*2+1] for i in range(256)]
        pal_rgb=[((c>>11)&31,(c>>1)&31,(c>>6)&31) for c in pal]
        _ncache={}
        def nearest(r5,g5,b5):
            k=(r5,g5,b5)
            if k in _ncache: return _ncache[k]
            bi=0; bd=1<<30
            for i in range(256):
                pr,pg,pb=pal_rgb[i]
                d=(r5-pr)**2+(g5-pg)**2+(b5-pb)**2
                if d<bd: bd=d; bi=i
            _ncache[k]=bi; return bi
        atl=bytearray(open(os.path.join(OUTDIR,PREFIX+"_atlas.bin"),"rb").read())
        assert len(atl)%AW==0, "atlas not a whole number of rows"
        H0=len(atl)//AW

        # bake the three models with EXACTLY the same args as the normal path,
        # so face order matches the shipping mrt_<name>.h tables one-for-one
        ens={}
        for _eoid,_ename,_enf in ((9,"bat",8),(7,"wolf",6),(8,"bear",6)):
            e=build_enemy(_eoid,_enf,data,pMeshData,pMeshOff,pAnims,
                          pNodes,pFrame,pModels,modelsCount)
            if e: ens[_ename]=e

        # distinct textured objtex across all three
        # MRT_ENEMYTEX_MODELS limits which models get real skins. The atlas is
        # LINKED INTO THE ROM, so every skinned model is BSS budget: all three
        # cost ~82KB and overflow the 0x1FC000 guard. Default to the wolf - it
        # is the enemy you actually meet in the Caves (6 of them; one bear).
        _want=set(os.environ.get("MRT_ENEMYTEX_MODELS","wolf").split(","))
        tids=set()
        for _nm,e in sorted(ens.items()):
            t2=set(tex for (v,tex) in e['quads']+e['tris']
                   if tex>=256 and tex<len(objtex))
            print("   %-5s %3d distinct textures%s" %
                  (_nm,len(t2)," (skinned)" if _nm in _want else " (flat)"))
            if _nm in _want: tids |= t2
        tids=sorted(tids)
        print("ENEMYTEX: %d distinct object textures across %s"
              % (len(tids), ",".join(sorted(ens))))

        # measure, then shelf-pack tallest-first
        rects={}
        boxes=[]
        _skip=[]
        for tid in tids:
            o=objtex[tid]
            uv=list(o['uv'])
            # ☠️ A TRIANGLE objtex carries a JUNK 4th corner. Including it
            # blows the bbox out to most of a tile page (216x256 seen), which
            # is what rejected 73 of 153 skins and inflated the atlas by 1.6MB.
            # If dropping the 4th corner brings the box back to a sane size,
            # the 4th was junk - trust the first three.
            def _bb(pts):
                us=[q[0] for q in pts]; vs=[q[1] for q in pts]
                return min(us),min(vs),max(us),max(vs)
            u0,v0,u1,v1=_bb(uv)
            if (u1-u0+1>64 or v1-v0+1>64) and len(uv)>3:
                a0,b0,a1,b1=_bb(uv[:3])
                if a1-a0+1<=64 and b1-b0+1<=64:
                    u0,v0,u1,v1=a0,b0,a1,b1
                    uv=uv[:3]
            bw=(u1-u0)//ENTEX_STEP+1; bh=(v1-v0)//ENTEX_STEP+1
            # ☠️ SANITY CLAMP. A mesh skin patch is 16x16 or so; a handful of
            # objtex in this set span most of a 256x256 tile page (216x256),
            # which is not a wolf's fur - including them blew the atlas up by
            # 1.6MB and would have overflowed the ROM. Anything bigger than
            # 64x64 is not a character skin, so drop it to the flat tone.
            if bw>64 or bh>64:
                _skip.append((tid,bw,bh)); continue
            boxes.append((bh,bw,tid,u0,v0))
        boxes.sort(reverse=True)
        print("ENEMYTEX: %d skinnable, %d oversized rejected (>64px)"
              % (len(boxes), len(_skip)))
        if int(os.environ.get("MRT_ENEMYTEX","0"))==2:
            # PROBE: compare enemy face objtex against a KNOWN-GOOD room
            # texture, read-only, before anything is written.
            print("--- enemy face tex ids (wolf, first 12) ---")
            for (v,tex) in ens['wolf']['quads'][:12]:
                if tex < len(objtex):
                    o=objtex[tex]
                    print("   tex %5d uv=%s tile=%s ts=%s" %
                          (tex, o['uv'], o.get('tile'), o.get('ts')))
                else:
                    print("   tex %5d OUT OF RANGE (objtex has %d)" % (tex,len(objtex)))
            print("--- a room texture for comparison ---")
            for t in sorted(list(used))[:4] if 'used' in dir() else []:
                pass
            rt = rooms[0]['quads'][0]['tex']
            print("   room0 q0 tex %d uv=%s" % (rt, objtex[rt]['uv'] if rt<len(objtex) else None))
            print("--- how many enemy faces are COLORED (<256)? ---")
            for nm,e in sorted(ens.items()):
                c=sum(1 for (v,tex) in e['quads']+e['tris'] if tex<256)
                print("   %-5s %d of %d faces colored" % (nm,c,len(e['quads'])+len(e['tris'])))
            sys.exit(0)
        import collections as _c
        _h=_c.Counter((h,w) for h,w,_t,_u,_v in boxes)
        print("ENEMYTEX box sizes (hxw -> count):",
              sorted(_h.items(), key=lambda kv:-kv[0][0]*kv[0][1])[:10])
        print("ENEMYTEX total texels:", sum(h*w for h,w,_t,_u,_v in boxes))
        px=0; py=H0; shelf=0
        for h,w,tid,u0,v0 in boxes:
            if px+w>AW: px=0; py+=shelf; shelf=0
            if py+h>len(atl)//AW: atl+=bytearray(AW*(py+h-len(atl)//AW))
            o=objtex[tid]; ts=o.get('ts',1)
            for yy in range(h):
                for xx in range(w):
                    # ☠️ HALF-RES SKINS. At full res the wolf's 91 textures are
                    # 53KB of atlas, and the atlas is LINKED INTO THE ROM - more
                    # than the whole image has spare. Sampling every OTHER texel
                    # quarters that to ~13KB. A wolf is a few dozen pixels tall
                    # on a 320x120 screen, so the detail was never visible.
                    sx = (u0 + xx*ENTEX_STEP) * ts
                    sy = (v0 + yy*ENTEX_STEP) * ts
                    r5,g5,b5,a=clut_rgb555(o['clut'],
                        tile_nibble(o['tile'], sx, sy))
                    atl[(py+yy)*AW+px+xx]=nearest(r5,g5,b5)
            rects[tid]=(px,py,u0,v0)
            px+=w
            if h>shelf: shelf=h
        open(os.path.join(OUTDIR,PREFIX+"_atlas.bin"),"wb").write(atl)

        # per-face UVs, in the SAME order as mrt_<name>_quads/_tris
        def face_uv(tex,n):
            """actual per-CORNER uvs mapped into the packed rect, so rotated
            and mirrored skins keep their orientation; 0xFFFF = flat-colour
            face (tex<256), which main.c falls back to the tone swatch for."""
            if tex not in rects: return [0xFFFF]*(n*2)
            px0,py0,u0,v0=rects[tex]
            o=objtex[tex]
            out=[]
            for i in range(n):
                u,v=o['uv'][i] if i<len(o['uv']) else o['uv'][-1]
                out += [px0+(u-u0)//ENTEX_STEP, py0+(v-v0)//ENTEX_STEP]
            return out
        with open(os.path.join(OUTDIR,PREFIX+"_entex.h"),"w") as f:
            f.write("// generated by MRT_ENEMYTEX - per-face atlas UVs for the\n")
            f.write("// enemy models, appended to %s_atlas.bin at rows %d..%d\n"
                    % (PREFIX,H0,len(atl)//AW-1))
            f.write("// 0xFFFF = flat-colour face: main.c uses the tone swatch.\n")
            for nm,e in sorted(ens.items()):
                up=(PREFIX+"_"+nm).upper()
                f.write("static const unsigned short %s_quv[%d][8] = {\n"
                        % (up,max(1,len(e['quads']))))
                for (v,tex) in e['quads']:
                    f.write("  {"+",".join("%d"%c for c in face_uv(tex,4))+"},\n")
                f.write("};\n")
                f.write("static const unsigned short %s_tuv[%d][6] = {\n"
                        % (up,max(1,len(e['tris']))))
                for (v,tex) in e['tris']:
                    f.write("  {"+",".join("%d"%c for c in face_uv(tex,3))+"},\n")
                f.write("};\n")
        print("ENEMYTEX: wrote %s_atlas.bin (%d rows, +%d) + %s_entex.h"
              % (PREFIX,len(atl)//AW,len(atl)//AW-H0,PREFIX))
        sys.exit(0)


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
        # LARA_TEXSCALE=1 (2026-07-26): drop her full-res exemption.  It was
        # made when the renderer was 320x240; under LOWRES (320x120) her fine
        # alternating dark/pale HAIR STRANDS alias into a solid pale blob on the
        # back of her skull -- the "face on the back of her head" artifact.
        _t = TEXSCALE if int(os.environ.get("LARA_TEXSCALE","0")) else (1 if _i in lara_ts else TEXSCALE)
        _o['ts']=_t
        # Keep the UNSCALED texCoord[0].  A flat-colour objtex (index < 256) is
        # a degenerate 1x1 rect whose single texel IS the colour, so the lossy
        # //_t then *_t round-trip below shifts the read by up to one texel in
        # each axis and lands on an unrelated CLUT entry.  Harmless for a real
        # tile (it is a whole rect); fatal for a 1x1 colour lookup -- index 8
        # read (170,2) instead of (171,3) and came back pure BLACK, which is
        # what painted Lara's hips and thighs with solid black quads.
        _o['uv0']=_o['uv'][0]
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
            room_tex.add(q['tex'])
            q['lvl']=3 if RAMP_PAL else _face_lvl(rm,q)
            used_pairs.add((q['tex'],q['lvl']))
        for t in rm['tris']:
            room_tex.add(t['tex'])
            t['lvl']=3 if RAMP_PAL else _face_lvl(rm,t)
            used_pairs.add((t['tex'],t['lvl']))
    # RAMP_PAL: every face uses the single full-bright (lvl 3) tile copy; the
    # darkening that lvl used to bake moves to the runtime shade pass, driven
    # by the per-vertex light already emitted in the VERTS records.
    # LARA_LVL (2026-07-26): which SHADE_FACT copy of her tiles to bake.
    # SHADE_FACT=(90,141,200,256).  RAMP_PAL bakes every tile FULL BRIGHT (3) and
    # moves darkening to the runtime shade pass — but Lara's faces never get a
    # runtime k (all 375 carry k=0), so she is the one object that never gets
    # darkened.  The known-good 2026-07-20 build baked her at lvl 2: her hair
    # highlight was (222,170,107) there vs (247,219,132) today, a ratio of
    # 0.90/0.78/0.81 ~= SHADE_FACT[2]/SHADE_FACT[3] = 200/256 = 0.78.
    # That blow-out is the pale blob on the back of her skull.
    LARA_LVL=int(os.environ.get("LARA_LVL","3"))
    for f in lara['quads']:
        if not f['colored']: quad_tex.add(f['tex']); used_pairs.add((f['tex'],LARA_LVL))
    for f in lara['tris']:
        if not f['colored']: tri_tex.add(f['tex']); used_pairs.add((f['tex'],LARA_LVL))
    def corner_count(tex):
        if tex in room_tex: return 4       # preserve existing room atlas exactly
        return 4 if tex in quad_tex else 3 # Lara-only: real face vertex count

    # ---- dedup tiles by (tile,clut,bbox), decode once ----
    _LVBLUR=int(os.environ.get('LARA_VBLUR','0'))
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
            # LARA_VBLUR=1 (2026-07-26): LOWRES renders at HALF VERTICAL
            # resolution, so Lara's fine alternating dark/pale HAIR STRANDS
            # alias -- the sampler lands on the pale strand and the back of her
            # skull turns into a solid bright blob ("her face is on the back of
            # her head").  Point-sampling her at half res (LARA_TEXSCALE) makes
            # it WORSE; the correct treatment for aliasing is a LOW-PASS filter,
            # and only the VERTICAL axis is being decimated.  Box-filter each
            # texel with its vertical neighbours, full width preserved.
            if _LVBLUR and ti in lara_ts and h > 1:
                src=px[:]
                for _y in range(h):
                    for _x in range(w):
                        acc=[0,0,0,0]; n=0
                        for _dy in (-1,0,1):
                            yy=_y+_dy
                            if 0<=yy<h:
                                q=src[yy*w+_x]
                                for _c in range(4): acc[_c]+=q[_c]
                                n+=1
                        px[_y*w+_x]=tuple(acc[_c]//n for _c in range(4))
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
    _ramp_bases=None
    if RAMP_PAL:
        # ---- ramp palette: RAMP_K bases x RAMP_M shades ----------------
        # Weighted Lloyd (k-means) over the FULL-BRIGHT colour histogram.
        # Lara keeps guaranteed seats (8 of the K bases are fit to her
        # colours alone) for the same reason as the legacy branch: level
        # browns outvote her skin. Measured on Caves shipped params:
        # 130 full-bright colours -> K=30 weighted RMS err 1.77 RGB555.
        def _rgb(c): return ((c>>11)&31, (c>>1)&31, (c>>6)&31)
        def _lloyd(h, K, iters=12):
            # log-damped weights (2026-07-20): raw pixel counts made the fit
            # subdivision-dependent — coarse SUBDIV kills tile dedup, rock
            # pixels flooded the histogram and evicted the browns (the
            # "Death Star" regression). log2 keeps every material's vote
            # within a factor of ~20 instead of ~2000.
            import math
            pts=[(_rgb(c),1+int(math.log2(1+n))) for c,n in h.most_common()]
            if not pts: return []
            seeds=[p[0] for p in pts[:K]]
            for _ in range(iters):
                buck=[[0,0,0,0] for _ in seeds]
                for (c,n) in pts:
                    bi=min(range(len(seeds)),
                           key=lambda i:(c[0]-seeds[i][0])**2
                                       +(c[1]-seeds[i][1])**2
                                       +(c[2]-seeds[i][2])**2)
                    b=buck[bi]; b[0]+=c[0]*n; b[1]+=c[1]*n; b[2]+=c[2]*n; b[3]+=n
                seeds=[(round(b[0]/b[3]),round(b[1]/b[3]),round(b[2]/b[3]))
                       for b in buck if b[3]]
            return seeds
        # LARA_BASES (2026-07-26): her guaranteed ramp seats.  With only 8 of
        # K=30 bases fitted to her colours, her HAIR-HIGHLIGHT cluster is
        # represented by a base far BRIGHTER than the real texel — (247,219,132)
        # today vs (222,170,107) in the known-good 2026-07-20 build — so the
        # back of her skull blows out into a pale blob.  More seats = a closer
        # centre = correct brightness.
        LARA_BASES=int(os.environ.get("LARA_BASES","8")) if hist_lara else 0
        _lb=_lloyd(hist_lara,LARA_BASES)
        _wb=_lloyd(hist,RAMP_K-LARA_BASES)
        # force the darkest WORLD base to true black (void/clear renders
        # slot 0). Never a Lara seat: blackening her hair base collapsed
        # her hair tones (reported as hair artifacts).
        if _wb:
            _di=min(range(len(_wb)),key=lambda i:299*_wb[i][0]+587*_wb[i][1]+114*_wb[i][2])
            _wb[_di]=(31,0,31) if os.environ.get("BASE0PROBE") else (0,0,0)
        bases=(_lb+_wb)[:RAMP_K]
        # base 0 MUST be the darkest base (2026-07-20): the rect-shade pass
        # ORs k into whole rows, including cleared (index-0) void pixels that
        # nothing repaints (cave mouths). index k lands in base 0's ramp, so
        # base 0 dark => void stays dark; a bright base 0 painted the cave
        # mouth white. Sort whole ramp order dark->bright (order is free:
        # texel assignment below is nearest-base, independent of order).
        bases.sort(key=lambda c:299*c[0]+587*c[1]+114*c[2])
        print("RAMP base0 luma=%d (blackened world base sorts first)"
              % ((299*bases[0][0]+587*bases[0][1]+114*bases[0][2])//1000))
        # ~x0.845 per step, spans the legacy SHADE_FACT range (256..~80)
        FACM=[256,216,183,155,131,111,94,79][:RAMP_M]
        idx_of={}
        for c in uniq:
            r,g2,b=_rgb(c)
            bi=min(range(len(bases)),
                   key=lambda i:(r-bases[i][0])**2+(g2-bases[i][1])**2
                               +(b-bases[i][2])**2)
            idx_of[c]=bi*RAMP_M          # atlas byte = ramp base = BRIGHTEST
        palette=[0]*256
        for bi,(r,g2,b) in enumerate(bases):
            for s in range(RAMP_M):
                f=FACM[s]
                palette[bi*RAMP_M+s]=jag16((r*f)>>8,(g2*f)>>8,(b*f)>>8)
        print("RAMP palette: %d bases x %d shades, %d colours mapped"
              % (len(bases),RAMP_M,len(uniq)))
        _ramp_bases=bases
    elif len(uniq)>240:
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
    if not RAMP_PAL:
        palette=sorted(uniq)[:240]; idx_of={c:i for i,c in enumerate(palette)}
        while len(palette)<256: palette.append(0)
    print("palette colours:", len(uniq))

    # ---- ATLAS TILING: PLAN THE MERGES *BEFORE* PACKING (2026-08-02) -------
    # Face count is the only lever that has ever moved fps, and this is the only
    # LOSSLESS way to pull it: the median room face is EXACTLY one 1024 sector,
    # so floors are grids of identical quads. Merge a run into one big quad and
    # the picture is unchanged - but the merged quad needs the texture REPEATED
    # across it, and the Blitter cannot wrap under affine mapping (the AND-mask
    # is A2-only, the fractional stepping is A1-only; DSTA2 swaps their roles but
    # you can never have both on one pointer). So bake PRE-TILED atlas entries.
    #
    # This must run BEFORE the shelf-pack, because the tiled variants are extra
    # tiles. It is purely geometric (plane + texture), so it does not need UVs -
    # which is what makes running it this early possible.
    # Measured potential: -30.1% faces at a 2x2 cap for ~1.1 MB of atlas if done
    # blanket, so it is BUDGETED - variants ranked by faces-saved-per-byte.
    ATILE = int(os.environ.get("ATLASTILE", "0"))
    # (_pk/_rect are defined below and reused by the room loop)       # KB of atlas to spend
    tile_plan = {}          # (room_i, cell-rect) -> variant key
    tile_variants = {}      # (tex, w, h) -> tile index once baked
    if True:
        def _pk(verts, f):
            vs=[verts[i] for i in f['v'][:3]]
            ux,uy,uz=(vs[1][k]-vs[0][k] for k in range(3))
            wx,wy,wz=(vs[2][k]-vs[0][k] for k in range(3))
            nx,ny,nz=uy*wz-uz*wy, uz*wx-ux*wz, ux*wy-uy*wx
            L=math.sqrt(nx*nx+ny*ny+nz*nz) or 1.0
            nx,ny,nz=nx/L,ny/L,nz/L
            return (round(nx*64),round(ny*64),round(nz*64),
                    round((nx*vs[0][0]+ny*vs[0][1]+nz*vs[0][2])/8.0))
        def _rect(verts,f):
            vs=[verts[i] for i in f['v']]
            a,b,c=vs[0],vs[1],vs[2]
            ux,uy,uz=(b[k]-a[k] for k in range(3)); wx,wy,wz=(c[k]-a[k] for k in range(3))
            n=(uy*wz-uz*wy, uz*wx-ux*wz, ux*wy-uy*wx)
            ax=max(range(3),key=lambda k:abs(n[k]))
            i1,i2=[k for k in range(3) if k!=ax]
            us=[v[i1] for v in vs]; vv=[v[i2] for v in vs]
            u0,u1,v0,v1=min(us),max(us),min(vv),max(vv)
            if u1==u0 or v1==v0: return None
            for uu,tv in zip(us,vv):
                if uu not in (u0,u1) or tv not in (v0,v1): return None
            return (u0,v0,u1,v1)
        # group, decompose greedily at a 2x2 cap, and rank by value/byte
        cand = {}
        _dbg=[0,0,0,0]
        for rm in rooms:
            vts = rm['verts']
            grp = {}
            for f in rm['quads']:
                r = _rect(vts,f)
                if r is None: continue
                gk = grp_of_tex.get((f['tex'], f.get('lvl', 0)))
                if gk is None: continue
                grp.setdefault((_pk(vts,f), gk[0]), []).append((r,f))
        # NOTE: full placement is applied in the room loop; here we only need
        # the VARIANT DEMAND so the atlas can be sized correctly.
            for (pkey,tex),lst in grp.items():
                _dbg[0]+=1; _dbg[1]+=len(lst)
                if len(lst) >= 4: _dbg[2]+=1
                if len(lst) < 4: continue
                cand[(tex,2,2)] = cand.get((tex,2,2),0) + (len(lst)//4)*3
        budget = ATILE*1024
        ranked = sorted(cand.items(), key=lambda kv: -kv[1]/float(kv[0][1]*kv[0][2]))
        spent = 0
        for (tex,w,h),saved in ranked:
            tw = tiles_out[tex]['w']*w
            th = tiles_out[tex]['h']*h
            cost = tw*th
            if not cost or spent+cost > budget: continue
            src = tiles_out[tex]
            px = []
            for yy in range(th):
                sy2 = (yy % src['h'])*src['w']
                for xx in range(tw):
                    px.append(src['px'][sy2 + (xx % src['w'])])
            vidx = len(tiles_out)
            tiles_out.append(dict(px=px, w=tw, h=th, umin=0, vmin=0))
            # A merged face must reach this tile through the NORMAL uv path, so
            # give it a synthetic objtex whose corners span the whole tiled
            # variant, and a grp_of_tex entry pointing at it with umin/vmin = 0.
            # TWO synthetic objtex per variant, one per winding, with UVs
            # ordered to match. The merged quad emits verts in a CANONICAL
            # corner order; whether that faces the right way depends on the
            # plane, so the caller picks A or B. Reversing BOTH verts and UVs
            # flips the winding while preserving the texture mapping.
            stexA = len(objtex)
            objtex.append(dict(tile=0, clut=0,
                               uv=[(0,0),(tw-1,0),(tw-1,th-1),(0,th-1)]))
            stexB = len(objtex)
            objtex.append(dict(tile=0, clut=0,
                               uv=[(0,0),(0,th-1),(tw-1,th-1),(tw-1,0)]))
            for _lv in range(4):
                grp_of_tex[(stexA,_lv)] = (vidx, 0, 0)
                grp_of_tex[(stexB,_lv)] = (vidx, 0, 0)
            tile_variants[(tex,w,h)] = (vidx, stexA, tw, th, stexB)
            spent += cost
        print("ATLAS TILING DEBUG: rooms=%d groups=%d faces_in_groups=%d groups>=4=%d"
              % (len(rooms), _dbg[0], _dbg[1], _dbg[2]))
        print("ATLAS TILING: %d variants baked, %d bytes of atlas (budget %d KB),"
              " projected face saving %d"
              % (len(tile_variants), spent, ATILE,
                 sum(v for k,v in cand.items() if k in tile_variants)))

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
    # Regenerated 2026-08-02 after the TEXSCALE round-trip bug was fixed (see
    # 'uv0' above).  The old table was a snapshot of the SHIFTED read, and its
    # index 8 -- Lara's SHORTS, 6 faces on her hips plus 2 on each thigh -- was
    # 0x0000, i.e. pure black.  That is what put the black blotches on her
    # backside.  Skin (index 14, 130 faces) is unchanged by the fix.
    #   6  boots/shins   8  shorts    10 torso trim  13 head
    #   14 SKIN          21 holsters  25 belt        34 top
    LARA_TONE_CANON = {6:0x5810, 8:0x7156, 10:0xa1da, 13:0xcae4,
                       14:0xdb28, 21:0x318c, 25:0x5910, 34:0x94a8}
    # LARA_TONE_NOCANON=1 bypasses the table so the median-texel rule can be
    # compared against it; LARA_TONE="8:0x1234,21:0x..." overrides single
    # entries.  Index 8 shipped as 0x0000 (pure black) and painted her hips
    # and thighs with two solid black quads -- see the tone audit below.
    for _kv in os.environ.get("LARA_TONE","").split(","):
        if ":" in _kv:
            _k,_v=_kv.split(":",1); LARA_TONE_CANON[int(_k)]=int(_v,0)
    _NOCANON=int(os.environ.get("LARA_TONE_NOCANON","0"))
    def lara_col_tone(ci):
        if ci in LARA_TONE_CANON and not _NOCANON: return LARA_TONE_CANON[ci]
        o=objtex[ci]
        # A flat-colour objtex is a degenerate 1x1 rect: its single texel IS the
        # colour, exactly as format.h getColor() reads it for VER_TR1_PSX.  Read
        # it at the UNSCALED texCoord[0] -- 'uv' has been through the lossy
        # TEXSCALE round-trip and is off by up to one texel, which on a 1x1 rect
        # is a different colour entirely.
        _u,_v=o.get('uv0',o['uv'][0])
        if len(set(o['uv'][:3]))==1:
            r5,g5,b5,a = clut_rgb555(o['clut'], tile_nibble(o['tile'], _u, _v))
            return jag16(r5,g5,b5)
        # MEDIAN texel of the objtex region, not texel[uv0]: for a rect larger
        # than 1x1 the first-texel rule is a lottery (uv0 landed on a SHADOW
        # pixel in GYM.PSX -> Lara's flat-colored limbs rendered dark red-brown,
        # 2026-07-13).
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
    _spill=[]
    print("lara colored-face tones (idx->RGB16):", end=" ")
    # LARA_COLRAMP (2026-07-26): under RAMP_PAL, point each flat tone at the
    # nearest RAMP BASE (bi*RAMP_M) instead of a reserved flat slot 246..253.
    # The reserved band is NOT ramp-aligned, so a runtime shade k added to it
    # walks into 254/255 = the UI black/white -- that is why shading her
    # COLOURED faces turned her limbs grey-green/black.  On a ramp base, k just
    # steps down the ramp like every other surface in the game.
    _COLRAMP = int(os.environ.get("LARA_COLRAMP","0")) and RAMP_PAL and _ramp_bases
    for j,ci in enumerate(col_indices[:30]):
        tone=lara_col_tone(ci)
        if _COLRAMP:
            tr,tg,tb=((tone>>11)&31,(tone>>1)&31,(tone>>6)&31)
            bi=min(range(len(_ramp_bases)),
                   key=lambda i:(tr-_ramp_bases[i][0])**2+(tg-_ramp_bases[i][1])**2
                               +(tb-_ramp_bases[i][2])**2)
            col_pal[ci]=bi*RAMP_M
        elif LARA_COL_BASE+j <= 253:
            slot=LARA_COL_BASE+j
            palette[slot]=tone; col_pal[ci]=slot
        else:
            # ☠️ THE RESERVED FLAT BAND IS ONLY 8 SLOTS (246..253; 254/255 are
            # the UI black/white). The Caves has exactly 8 colour indices and
            # fits; LARA'S HOME HAS 13 and walked straight off the end of the
            # 256-entry palette - `IndexError: list assignment index out of
            # range` - which is why the mansion could not be extracted at all
            # under the full recipe, and why the container had to fall back to
            # a reduced flag set just to emit its headers.
            # Past the band, point the tone at the NEAREST COLOUR ALREADY IN
            # THE PALETTE instead of demanding a new slot. Under RAMP_PAL that
            # lands on a ramp entry, so a runtime shade k still steps down the
            # ramp rather than walking into the UI colours - the same reasoning
            # as LARA_COLRAMP above, applied only where it is forced.
            tr,tg,tb=((tone>>11)&31,(tone>>1)&31,(tone>>6)&31)
            def _d(pi):
                c=palette[pi]
                return (tr-((c>>11)&31))**2+(tg-((c>>1)&31))**2+(tb-((c>>6)&31))**2
            col_pal[ci]=min(range(254), key=_d)
            _spill.append(ci)
        print("%d:%04x%s" % (ci, tone, "->ramp%d"%(col_pal[ci]//RAMP_M) if _COLRAMP else ""), end=" ")
    print()
    if _spill:
        print("  NOTE: %d colour index(es) past the %d-slot flat band matched to "
              "the nearest palette entry instead: %s"
              % (len(_spill), 254-LARA_COL_BASE, _spill))
    # LARA_TONEAUDIT=1: for each flat-colour index, dump the objtex source rect
    # and a histogram of the texels inside it, split opaque/transparent.  Index
    # 8 resolved to 0x0000 and painted her hips black; this says whether the
    # patch really is black art or whether we are reading CLUT[0] = the PSX
    # TRANSPARENT entry and calling it a colour.
    if int(os.environ.get("LARA_TONEAUDIT","0")):
        for ci in col_indices[:30]:
            if ci >= len(objtex): print("TONEAUDIT %3d: no objtex" % ci); continue
            o=objtex[ci]
            us=[p[0] for p in o['uv'][:3]]; vs=[p[1] for p in o['uv'][:3]]
            u0,u1,v0,v1=min(us),max(us),min(vs),max(vs)
            hist={}; nop=0; ntr=0
            for y in range(v0,v1+1):
                for x in range(u0,u1+1):
                    r5,g5,b5,a=clut_rgb555(o['clut'], tile_nibble(o['tile'],x*o['ts'],y*o['ts']))
                    if a: nop+=1
                    else: ntr+=1
                    k=(r5,g5,b5,bool(a)); hist[k]=hist.get(k,0)+1
            top=sorted(hist.items(), key=lambda kv:-kv[1])[:4]
            print("TONEAUDIT %3d: tile=%d clut=%d uv=(%d,%d)-(%d,%d) opaque=%d transp=%d  %s"
                  % (ci,o['tile'],o['clut'],u0,v0,u1,v1,nop,ntr,
                     " ".join("(%2d,%2d,%2d)%s x%d"%(k[0],k[1],k[2],"" if k[3] else "T",n)
                              for k,n in top)))
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

    # ---- DOOR + LEVER textures: APPENDED to the atlas ------------------------
    # The doors/levers are ENTITY models, so their textures are NOT in the
    # room+Lara union.  Rather than add them to `used` (which reshuffles the
    # shelf packer and moves EVERY existing UV -> a full regen touching
    # mrt_geom + mrt_lara), decode them here into NEW rows appended after the
    # atlas is finalized: existing tiles never move, so room/Lara UVs stay
    # byte-identical.  Each texel maps to the nearest ramp base, so the palette
    # is unchanged too.  Door face = objtex 897 (shared by all four doors);
    # lever = objtex 896.  Emitted to _spawn.h as MRT_*_TEX_U0.. rects.
    def _pal_idx(r5,g5,b5):
        if RAMP_PAL and _ramp_bases:
            return min(range(len(_ramp_bases)),
                key=lambda i:(r5-_ramp_bases[i][0])**2+(g5-_ramp_bases[i][1])**2
                            +(b5-_ramp_bases[i][2])**2)*RAMP_M
        return idx_of.get(jag16(r5,g5,b5),0)
    door_tex_rect={}                     # name -> (x0,y0,x1,y1) atlas texels
    for _nm,_tid in (("DOOR",897),("SW",896)):
        if _tid>=len(objtex): continue
        _o=objtex[_tid]
        _us=[p[0] for p in _o['uv']]; _vs=[p[1] for p in _o['uv']]
        _u0,_v0,_u1,_v1=min(_us),min(_vs),max(_us),max(_vs)
        _w=_u1-_u0+1; _h=_v1-_v0+1
        _ts=_o.get('ts',1); _ay=atlas_h
        atlas+=bytearray(ATLAS_W*_h)
        for _yy in range(_h):
            for _xx in range(_w):
                _r,_g,_b,_a=clut_rgb555(_o['clut'],
                    tile_nibble(_o['tile'],(_u0+_xx)*_ts,(_v0+_yy)*_ts))
                atlas[(_ay+_yy)*ATLAS_W+_xx]=_pal_idx(_r,_g,_b)
        atlas_h+=_h
        # inset 1 texel so the affine DDA can't sample the neighbouring row
        door_tex_rect[_nm]=(1, _ay+1, _w-2, _ay+_h-2)
    atlas_h=(atlas_h+1)&~1
    print("door/lever atlas rects:", door_tex_rect)

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
    _khist={}    # RAMP_PAL face-shade histogram (k=0 faces skip the shade blit)
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
        # FLATFLOOR (2026-07-29, perf experiment): collapse HORIZONTAL faces'
        # UVs onto a single texel so the whole polygon samples ONE colour - its
        # OWN texture's, so nothing new is added to the atlas and the poly keeps
        # its natural tone. Walls keep their texture. The point is the Blitter:
        # every pixel of a floor span then reads the SAME DRAM address instead
        # of striding across a 256-wide atlas, and this renderer is bus-bound
        # (the perf campaign's conclusion; Tom already spends 29% of its cycles
        # waiting). 1 = floors only, 2 = floors + ceilings, 3 = every face.
        if FLATFLOOR:
            vts=rm['verts']
            for f in rm['quads']+rm['tris']:
                vv=[vts[i] for i in f['v']]
                x0,y0,z0=vv[0][:3]; x1,y1,z1=vv[1][:3]; x2,y2,z2=vv[2][:3]
                ax,ay,az=x1-x0,y1-y0,z1-z0
                bx,by,bz=x2-x0,y2-y0,z2-z0
                nx=ay*bz-az*by; ny=az*bx-ax*bz; nz=ax*by-ay*bx
                horiz = abs(ny) >= abs(nx) and abs(ny) >= abs(nz)
                if FLATFLOOR==5:
                    # SIZE-BASED MIX (2026-08-04): flatten only BIG faces (the
                    # bus load - whatever fills the screen, wall OR floor), keep
                    # small detail textured. Newell area; thresh FLATAREA (world
                    # units^2; a 1024x1024 sector floor ~= 1.05e6).
                    m=len(vv); nxx=nyy=nzz=0.0
                    for _k in range(m):
                        _a=vv[_k]; _b=vv[(_k+1)%m]
                        nxx+=(_a[1]-_b[1])*(_a[2]+_b[2])
                        nyy+=(_a[2]-_b[2])*(_a[0]+_b[0])
                        nzz+=(_a[0]-_b[0])*(_a[1]+_b[1])
                    take = 0.5*math.sqrt(nxx*nxx+nyy*nyy+nzz*nzz) > \
                           int(os.environ.get("FLATAREA","450000"))
                elif FLATFLOOR==4: take = not horiz     # WALLS only (verticals)
                elif FLATFLOOR>=3: take=True
                elif FLATFLOOR==2: take=horiz
                else:
                    # floors only: horizontal AND facing UP. Sign VERIFIED by
                    # rendering: ny<0 selected the CEILING, so up-facing floors
                    # are ny>0 here (the cross product's handedness, not the
                    # +Y-down convention, decides this - do not re-derive it
                    # from first principles, look at the picture).
                    take = horiz and ny > 0
                if not take: continue
                us=[u for (u,v) in f['uv']]; vs=[v for (u,v) in f['uv']]
                cu=(min(us)+max(us))//2; cv=(min(vs)+max(vs))//2
                f['uv']=[(cu,cv)]*len(f['uv'])

        # FLATMERGE (2026-08-03): the ONLY frame-rate lever left. Measured in
        # r34 (624 faces): base 4.00 fps == NOBLIT 4.00 fps -> the Blitter fill
        # is FREE; cost is per-FACE/per-VERTEX GPU work, scaling with FACE
        # COUNT. Textured floor tiles can't merge (each tiles its own texture),
        # but the FLATFLOOR faces above are now flat colour, so adjacent
        # same-colour coplanar tiles MERGE with no visual change -> fewer faces.
        # Greedy-mesh axis-aligned horizontal rects on the 1024 sector grid;
        # mark merged faces 'flat' so the subdivider never splits them back.
        if FLATMERGE:
            # GRID cell size per world axis: X/Z are 1024 (sectors), Y is 256
            # (TR1 clicks) so vertical WALL rects grid-align too. FLATMERGE>=2
            # merges ALL THREE axis planes (walls+floors); ==1 = floors only.
            # TEXTURE-PRESERVING (2026-08-04): only faces that ACTUALLY combine
            # into a multi-cell rect get flattened+merged (they sample one texel);
            # singletons and non-rect faces keep their ORIGINAL texture. Fill is
            # free, so the fps win comes from the merge (fewer verts/faces after
            # VPRUNE), NOT from flattening — so we flatten only where we must.
            # No FLATFLOOR needed; the flat texel is computed per face here.
            GRID=(1024,256,1024)
            # FLATMERGE: 1=floors(horiz) only, 2=all axes, 3=WALLS only (verticals)
            AXES=(0,2) if FLATMERGE==3 else ((1,0,2) if FLATMERGE>=2 else (1,))
            def flat_texel(f):
                us=[u for (u,v) in f['uv']]; vs=[v for (u,v) in f['uv']]
                return ((min(us)+max(us))//2, (min(vs)+max(vs))//2)
            def axis_rect(f):
                vv=[verts[i] for i in f['v']]
                for axis in AXES:
                    if len(set(v[axis] for v in vv))!=1: continue   # planar in axis
                    ua,ub=[a for a in (0,1,2) if a!=axis]
                    ga,gb=GRID[ua],GRID[ub]
                    us=sorted(set(v[ua] for v in vv)); vs=sorted(set(v[ub] for v in vv))
                    if len(us)!=2 or len(vs)!=2: continue           # axis-aligned rect
                    if (us[1]-us[0])%ga or (vs[1]-vs[0])%gb: continue
                    if us[0]%ga or vs[0]%gb: continue               # grid-aligned
                    return (axis, vv[0][axis], ua, ub, ga, gb, us[0], us[1], vs[0], vs[1])
                return None
            groups={}; kept=[]
            for f in rm['quads']:
                r=axis_rect(f)
                if r is None: kept.append(f); continue
                axis,c,ua,ub,ga,gb,u0,u1,v0,v1=r
                groups.setdefault((axis,c,ua,ub,ga,gb,f['tex'],flat_texel(f)),
                                  []).append((u0,u1,v0,v1,f))
            def add_vert(x,y,z,lit):
                verts.append((x,y,z,lit)); return len(verts)-1
            nmerged=0
            for key,rects in groups.items():
                axis,c,ua,ub,ga,gb,tex,ft=key
                cells={}
                for u0,u1,v0,v1,f in rects:
                    for gu in range(u0//ga, u1//ga):
                        for gv in range(v0//gb, v1//gb):
                            cells[(gu,gv)]=f      # last wins as winding template
                used=set()
                for (gu,gv) in sorted(cells):
                    if (gu,gv) in used: continue
                    tf=cells[(gu,gv)]
                    w=1
                    while (gu+w,gv) in cells and (gu+w,gv) not in used: w+=1
                    h=1; grow=True
                    while grow:
                        for dx in range(w):
                            if (gu+dx,gv+h) not in cells or (gu+dx,gv+h) in used:
                                grow=False; break
                        if grow: h+=1
                    for dx in range(w):
                        for dz in range(h): used.add((gu+dx,gv+dz))
                    if w*h==1:
                        kept.append(tf)          # singleton: keep ORIGINAL texture
                        continue
                    U0,U1=gu*ga,(gu+w)*ga; V0,V1=gv*gb,(gv+h)*gb
                    # replicate the template face's corner order at merged bounds,
                    # in the plane's two varying axes (holds the constant axis at c)
                    tfu=[verts[i][ua] for i in tf['v']]; tfv=[verts[i][ub] for i in tf['v']]
                    tu0,tv0=min(tfu),min(tfv)
                    lit=sum(verts[i][3] for i in tf['v'])//len(tf['v'])
                    nv=[]
                    for i in tf['v']:
                        co=[0,0,0]; co[axis]=c
                        co[ua]=U0 if verts[i][ua]==tu0 else U1
                        co[ub]=V0 if verts[i][ub]==tv0 else V1
                        nv.append(add_vert(co[0],co[1],co[2],lit))
                    kept.append(dict(v=nv, tex=tex, uv=[ft]*len(nv),
                                     colored=tf.get('colored',False),
                                     lvl=tf.get('lvl'), swapped=tf.get('swapped',False),
                                     flat=True))
                    nmerged+=1
            if nmerged:
                if int(os.environ.get("FLATMERGE_LOG","0")):
                    print("  FLATMERGE room %d: %d quads -> %d (%d merged rects)"
                          % (rm['i'], len(rm['quads']), len(kept), nmerged))
                rm['quads']=kept

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
            if f.get('flat') or extent(f)<=SUBDIV_MAX or len(verts)>=VERT_BUDGET:
                outq.append(f); continue   # flat merged rects never re-split
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
        # PRUNE unreferenced verts + reindex (2026-08-04): FLATMERGE and the
        # subdivider ORPHAN verts (merged-away faces, dead midpoints). vc_loop
        # transforms EVERY vert in rm['verts'] once per room, so a dead vert is
        # pure per-vertex transform cost with no pixel. Compacting to only the
        # referenced verts is what makes FACE reduction actually cut the
        # per-VERTEX transform (ALLCULL showed transform caps r18 at 5.25 fps).
        # Pure compaction: reindexing faces to a subset list never changes what
        # renders. Default ON; VPRUNE=0 disables for an A/B.
        if int(os.environ.get("VPRUNE","1")):
            used_idx=set()
            for f in rm['quads']+rm['tris']: used_idx.update(f['v'])
            if len(used_idx) < len(rm['verts']):
                order=sorted(used_idx)
                remap={old:new for new,old in enumerate(order)}
                rm['verts']=[rm['verts'][old] for old in order]
                for f in rm['quads']+rm['tris']:
                    f['v']=[remap[i] for i in f['v']]
    # ---- ROOM FACE AUDIT (2026-08-01) -----------------------------------
    # The renderer is FACE-BOUND (halving scanlines bought 2.6%; 240 lines cost
    # 4.4%; flat-shading the whole floor was exactly 0; SPANSHADE is free), and
    # 48.5% of per-face work never yields a pixel. So the only structural perf
    # lever left is EMITTING FEWER FACES -- but "fewer" has to be aimed with a
    # measurement, not a guess. LARA_MINAREA=400 was exactly this idea applied
    # blind to Lara and it punched 78 holes in her limbs.
    # ROOMAUDIT=1 prints the world-area distribution of every room face so a
    # threshold can be chosen from data. Areas are world units^2; a TR sector
    # is 1024x1024 = 1048576, so a full floor tile is ~1.0e6.
    def _rface_area(verts, f):
        vs = [verts[i] for i in f['v']]
        nx = ny = nz = 0.0
        n = len(vs)
        for k in range(n):                      # Newell: works for any polygon
            a = vs[k]; b = vs[(k + 1) % n]
            nx += (a[1] - b[1]) * (a[2] + b[2])
            ny += (a[2] - b[2]) * (a[0] + b[0])
            nz += (a[0] - b[0]) * (a[1] + b[1])
        return 0.5 * math.sqrt(nx * nx + ny * ny + nz * nz)

    _RAUD = int(os.environ.get("ROOMAUDIT", "0"))
    _raud_areas = []

    # ---- MERGEAUDIT (2026-08-01): can coplanar same-texture faces merge? ----
    # Face count is the ONLY lever that has ever moved fps (-45% on Lara bought
    # a vsync rung), but every way of getting it costs visible detail. There is
    # one lossless route: the median room face is EXACTLY one 1024-unit sector,
    # so floors are grids of identical quads -- merge a run of them into one big
    # quad and the picture is unchanged.
    # ☠️ Hardware texture WRAP cannot do it: the Jaguar Blitter's AND-mask lives
    # only on A2 (`A2_MASK`, gated by A2_FLAGS bit 15) while the fractional
    # stepping affine mapping needs lives only on A1 (A1_FPIXEL/FSTEP/FINC).
    # DSTA2 can swap their roles but you can never have BOTH on one pointer.
    # ⇒ the merged quad needs a PRE-TILED atlas entry instead (bake a 2x2 / 4x4
    # repeat once), which is an extractor-only change: no kernel bytes, no A10.
    # This audit bounds the prize before any of that is built. A group = faces
    # sharing a plane (quantised normal + offset) AND a texture; merging one
    # group to a single face is the OPTIMISTIC bound.

    _mrect = {}
    def _face_rect2d(verts, f):
        """(u0,v0,u1,v1) if the face is an axis-aligned rect in its plane
        basis, else None. Basis = the two world axes most perpendicular to
        the face normal, so floors index (x,z) and walls (z,y) or (x,y)."""
        vs = [verts[i] for i in f['v']]
        a, b, c = vs[0], vs[1], vs[2]
        ux, uy, uz = (b[k]-a[k] for k in range(3))
        wx, wy, wz = (c[k]-a[k] for k in range(3))
        n = (uy*wz-uz*wy, uz*wx-ux*wz, ux*wy-uy*wx)
        ax = max(range(3), key=lambda k: abs(n[k]))     # dominant normal axis
        i1, i2 = [k for k in range(3) if k != ax]
        us = [v[i1] for v in vs]; vsq = [v[i2] for v in vs]
        u0, u1, v0, v1 = min(us), max(us), min(vsq), max(vsq)
        if u1 == u0 or v1 == v0:
            return None                                  # degenerate in-plane
        # every corner must sit ON the bbox corner for this to be a true rect
        for uu, vv in zip(us, vsq):
            if uu not in (u0, u1) or vv not in (v0, v1):
                return None
        return (u0, v0, u1, v1)

    # ---- SECTORAUDIT (2026-08-01) — GATE G1 for SECTOR_RENDERER_SCOPE.md ----
    # A sector renderer can only draw what is expressible as sector FLATS
    # (floor/ceiling planes, possibly sloped) and WALLS (vertical surfaces on a
    # sector boundary). Anything else needs a polygon fallback, and maintaining
    # two renderers is how the win evaporates. KILL CRITERION: "neither" > ~15%.
    _SAUD = int(os.environ.get("SECTORAUDIT", "0"))
    _g3 = []
    _SBUILD = int(os.environ.get('SECTORBUILD','0'))
    _p1 = {'segs':0,'real':0,'hit':0,'miss':0,'skew':0,
           'banded':0,'bandcount':0,'bandmax':0,'bandhist':{},
           'dflat':0,'rflat':0,'fhit':0,'fmiss':0,'fheight':0,
           'emitseg':0}
    _srblob = bytearray(); _srindex = []
    _SDUMP_ROOM = int(os.environ.get('SECTORDUMP', '-1'))
    _SDUMP = _SDUMP_ROOM >= 0
    _saud = {'flat':0, 'wall':0, 'oblique':0, 'wall_offgrid':0, 'flat_offgrid':0,
             'faces':0}
    def _sector_class(verts, f):
        vs = [verts[i] for i in f['v']]
        a, b, c = vs[0], vs[1], vs[2]
        ux, uy, uz = (b[k]-a[k] for k in range(3))
        wx, wy, wz = (c[k]-a[k] for k in range(3))
        nx, ny, nz = uy*wz-uz*wy, uz*wx-ux*wz, ux*wy-uy*wx
        L = math.sqrt(nx*nx+ny*ny+nz*nz) or 1.0
        ny_n = abs(ny/L)
        SEC = 1024
        if ny_n > 0.75:                      # floor / ceiling (sloped ok)
            xs = [v[0] for v in vs]; zs = [v[2] for v in vs]
            ongrid = (min(xs) % SEC == 0 and max(xs) % SEC == 0 and
                      min(zs) % SEC == 0 and max(zs) % SEC == 0)
            return 'flat', ongrid
        if ny_n < 0.25:                       # vertical -> wall
            xs = [v[0] for v in vs]; zs = [v[2] for v in vs]
            # a sector-boundary wall is constant in x or z, on a 1024 line
            ongrid = ((max(xs)==min(xs) and min(xs) % SEC == 0) or
                      (max(zs)==min(zs) and min(zs) % SEC == 0))
            return 'wall', ongrid
        return 'oblique', False

    _MAUD = int(os.environ.get("MERGEAUDIT", "0"))
    _maud = {}      # (room, plane-key, tex) -> face count
    def _plane_key(verts, f):
        vs = [verts[i] for i in f['v'][:3]]
        ux, uy, uz = (vs[1][k] - vs[0][k] for k in range(3))
        wx, wy, wz = (vs[2][k] - vs[0][k] for k in range(3))
        nx, ny, nz = uy*wz - uz*wy, uz*wx - ux*wz, ux*wy - uy*wx
        L = math.sqrt(nx*nx + ny*ny + nz*nz) or 1.0
        nx, ny, nz = nx/L, ny/L, nz/L
        d = nx*vs[0][0] + ny*vs[0][1] + nz*vs[0][2]
        # quantise: 1/64 on the normal, 8 world units on the offset
        return (round(nx*64), round(ny*64), round(nz*64), round(d/8.0))

    _merged_total = 0
    for rm in rooms:
        if ATILE and tile_variants:
            # ---- APPLY THE MERGES ------------------------------------------
            # Replace each exact 2x2 block of same-plane, same-tile quads with
            # ONE _mg_quad over the block, textured from the pre-tiled variant.
            # Pixel-identical by construction: the variant IS the tile repeated.
            _mg_vts = rm['verts']
            _mg_groups = {}
            for _mg_f in rm['quads']:
                _mg_r = _rect(_mg_vts, _mg_f)
                if _mg_r is None: continue
                gk = grp_of_tex.get((_mg_f['tex'], f.get('lvl', 0)))
                if gk is None: continue
                _mg_groups.setdefault((_pk(_mg_vts, _mg_f), gk[0]), []).append((_mg_r, _mg_f))
            _mg_drop = set()
            _mg_add = []
            for (_mg_pkey, _mg_g), _mg_lst in _mg_groups.items():
                _mg_var = tile_variants.get((_mg_g, 2, 2))
                if _mg_var is None or len(_mg_lst) < 4: continue
                # index the cells, and remember the plane's two basis axes
                _mg_f0 = _mg_lst[0][1]
                _mg_vs0 = [_mg_vts[i] for i in _mg_f0['v']]
                _mg_a, _mg_b, _mg_c = _mg_vs0[0], _mg_vs0[1], _mg_vs0[2]
                _mg_ux, _mg_uy, _mg_uz = (_mg_b[k]-_mg_a[k] for k in range(3))
                _mg_wx, _mg_wy, _mg_wz = (_mg_c[k]-_mg_a[k] for k in range(3))
                _mg_nvec = (_mg_uy*_mg_wz-_mg_uz*_mg_wy, _mg_uz*_mg_wx-_mg_ux*_mg_wz, _mg_ux*_mg_wy-_mg_uy*_mg_wx)
                _mg_axn = max(range(3), key=lambda k: abs(_mg_nvec[k]))
                _mg_i1, _mg_i2 = [k for k in range(3) if k != _mg_axn]
                _mg_cell = {}
                for _mg_r, _mg_f in _mg_lst: _mg_cell[(_mg_r[0], _mg_r[1])] = (_mg_r, _mg_f)
                # vertex lookup on this plane, by its two in-plane coords
                _mg_vlook = {}
                for _mg_r, _mg_f in _mg_lst:
                    for _mg_vi in _mg_f['v']:
                        _mg_vlook[(_mg_vts[_mg_vi][_mg_i1], _mg_vts[_mg_vi][_mg_i2])] = _mg_vi
                _mg_used = set()
                for (_mg_u0, _mg_v0) in sorted(_mg_cell):
                    if (_mg_u0, _mg_v0) in _mg_used: continue
                    _mg_r0, _mg_fa = _mg_cell[(_mg_u0, _mg_v0)]
                    _mg_du, _mg_dv = _mg_r0[2]-_mg_r0[0], _mg_r0[3]-_mg_r0[1]
                    _mg_quad = [(_mg_u0, _mg_v0), (_mg_u0+_mg_du, _mg_v0), (_mg_u0, _mg_v0+_mg_dv), (_mg_u0+_mg_du, _mg_v0+_mg_dv)]
                    if any(_mg_q in _mg_used or _mg_q not in _mg_cell for _mg_q in _mg_quad): continue
                    if any(_mg_cell[_mg_q][0][2]-_mg_cell[_mg_q][0][0] != _mg_du or
                           _mg_cell[_mg_q][0][3]-_mg_cell[_mg_q][0][1] != _mg_dv for _mg_q in _mg_quad): continue
                    _mg_U0, _mg_V0, _mg_U1, _mg_V1 = _mg_u0, _mg_v0, _mg_u0+2*_mg_du, _mg_v0+2*_mg_dv
                    _mg_corners = [(_mg_U0,_mg_V0),(_mg_U1,_mg_V0),(_mg_U1,_mg_V1),(_mg_U0,_mg_V1)]
                    if any(_mg_cc not in _mg_vlook for _mg_cc in _mg_corners): continue
                    # _mg_order the merged _mg_quad's verts to match _mg_fa's own winding
                    # CANONICAL corner order, matching stexA's UV order. The
                    # previous version took the vertex order from the SOURCE
                    # face's winding while assigning UVs in a fixed order; when
                    # those disagreed the quad came out either texture-rotated
                    # or wound BACKWARDS, and a backwards quad is backface-
                    # culled -> the black holes seen on silicon 2026-08-02.
                    _mg_mv = [_mg_vlook[(_mg_U0,_mg_V0)], _mg_vlook[(_mg_U1,_mg_V0)],
                              _mg_vlook[(_mg_U1,_mg_V1)], _mg_vlook[(_mg_U0,_mg_V1)]]
                    def _mg_nrm(_idxs):
                        _q=[_mg_vts[_i] for _i in _idxs[:3]]
                        _e1=[_q[1][_k]-_q[0][_k] for _k in range(3)]
                        _e2=[_q[2][_k]-_q[0][_k] for _k in range(3)]
                        return (_e1[1]*_e2[2]-_e1[2]*_e2[1],
                                _e1[2]*_e2[0]-_e1[0]*_e2[2],
                                _e1[0]*_e2[1]-_e1[1]*_e2[0])
                    _mg_na=_mg_nrm(_mg_mv); _mg_nb=_mg_nrm(_mg_fa['v'])
                    _mg_nf = dict(_mg_fa)
                    if sum(_mg_na[_k]*_mg_nb[_k] for _k in range(3)) >= 0:
                        _mg_nf['v']   = _mg_mv
                        _mg_nf['tex'] = _mg_var[1]        # stexA
                    else:
                        # reverse BOTH verts and UVs: flips winding, keeps the
                        # texture mapping (reversing only verts would mirror it)
                        _mg_nf['v']   = [_mg_mv[0], _mg_mv[3], _mg_mv[2], _mg_mv[1]]
                        _mg_nf['tex'] = _mg_var[4]        # stexB
                    _mg_add.append(_mg_nf)
                    for _mg_q in _mg_quad: _mg_used.add(_mg_q); _mg_drop.add(id(_mg_cell[_mg_q][1]))
            if _mg_drop:
                rm['quads'] = [_mg_f for _mg_f in rm['quads'] if id(_mg_f) not in _mg_drop] + _mg_add
                _merged_total += len(_mg_drop) - len(_mg_add)
        _subdivide(rm)
        if _RAUD:
            _vv = rm['verts']
            for _f in rm['quads']:
                _raud_areas.append((_rface_area(_vv, _f), 4))
            for _f in rm['tris']:
                _raud_areas.append((_rface_area(_vv, _f), 3))
        if _SBUILD:
            # ---- P1: derive WALL SEGMENTS from the sector grid, then CHECK
            # ---- them against the room's real wall faces. A sector renderer
            # ---- can only draw what this derivation finds, so the match rate
            # ---- IS the correctness measure for phase 1.
            _xS, _zS = rm['xS'], rm['zS']
            _cells = rm['sect']
            def _fl(sx, sz):
                if sx < 0 or sz < 0 or sx >= _xS or sz >= _zS: return None
                fo, ce, fi, be, ab = _cells[sx*_zS + sz]
                return (fo, ce)
            _segs = []          # (axis, coord, a0, a1)  axis 0 = const x
            for _sx in range(_xS):
                for _sz in range(_zS):
                    _me = _fl(_sx, _sz)
                    if _me is None: continue
                    _soli = (_me[0] == -127)
                    for _d, (_nx, _nz) in ((0, (_sx+1, _sz)), (1, (_sx, _sz+1))):
                        _nb = _fl(_nx, _nz)
                        _nsoli = (_nb is None) or (_nb[0] == -127)
                        if _soli and _nsoli:
                            continue            # rock against rock: no surface
                        if _soli != _nsoli or _me[0] != _nb[0] or _me[1] != _nb[1]:
                            # a height discontinuity or a solid boundary -> wall
                            if _d == 0:
                                _segs.append((0, (_sx+1)*1024, _sz*1024, (_sz+1)*1024))
                            else:
                                _segs.append((1, (_sz+1)*1024, _sx*1024, (_sx+1)*1024))
            # real wall faces, as (axis, coord, lo, hi)
            _real = []
            for _f in rm['quads'] + rm['tris']:
                _cl, _og = _sector_class(rm['verts'], _f)
                if _cl != 'wall': continue
                _vv = [rm['verts'][i] for i in _f['v']]
                _xsw = [v[0] for v in _vv]; _zsw = [v[2] for v in _vv]
                _ysw = [v[1] for v in _vv]
                if max(_xsw) == min(_xsw):
                    _real.append((0, _xsw[0], min(_zsw), max(_zsw),
                                  min(_ysw), max(_ysw), _f['tex']))
                elif max(_zsw) == min(_zsw):
                    _real.append((1, _zsw[0], min(_xsw), max(_xsw),
                                  min(_ysw), max(_ysw), _f['tex']))
                else:
                    _real.append(None)
            _segset = {}
            for _a, _c, _l, _h in _segs:
                _segset.setdefault((_a, _c), []).append((_l, _h))
            _hit = _miss = _skew = 0
            # TEXTURE BANDS: TR's sector table carries no texture, so each
            # derived segment takes its texture(s) from the real wall faces
            # that overlap it. A tall wall is split vertically into several
            # faces, so a segment needs a LIST of (y0,y1,tex) bands - exactly
            # Doom's upper/middle/lower, generalised.
            _bands = {}
            for _r in _real:
                if _r is None: _skew += 1; continue
                _a, _c, _l, _h, _y0, _y1, _tx = _r
                _cands = _segset.get((_a, _c), [])
                _ov = [(sl, sh) for sl, sh in _cands if not (_h <= sl or _l >= sh)]
                if _ov:
                    _hit += 1
                    for sl, sh in _ov:
                        _bands.setdefault((_a, _c, sl, sh), set()).add((_y0, _y1, _tx))
                else:
                    _miss += 1
            # ---- FLATS: floor + ceiling planes straight from the cells.
            # Each non-solid cell owns a floor at floor*256 and a ceiling at
            # ceil*256, spanning exactly its 1024 sector square (which is why
            # ROOMAUDIT found the median room face is EXACTLY one sector).
            _dflat = set(); _dfh = {}
            for _sx in range(_xS):
                for _sz in range(_zS):
                    _fo, _ce, _fi, _be, _ab = _cells[_sx*_zS + _sz]
                    if _fo == -127:            # solid rock: no floor, no ceiling
                        continue
                    _dflat.add((_sx, _sz, 'f'))
                    _dfh[(_sx, _sz, 'f')] = _fo*256
                    if _ce != -127:
                        _dflat.add((_sx, _sz, 'c'))
                        _dfh[(_sx, _sz, 'c')] = _ce*256
            _ftex = {}; _ctex = {}
            for _f in rm['quads'] + rm['tris']:
                _cl, _og = _sector_class(rm['verts'], _f)
                if _cl != 'flat': continue
                _vv = [rm['verts'][i] for i in _f['v']]
                _fy = sum(v[1] for v in _vv)/float(len(_vv))
                _xs3 = [v[0] for v in _vv]; _zs3 = [v[2] for v in _vv]
                for _cx in range(min(_xs3)//1024, (max(_xs3)-1)//1024 + 1):
                    for _cz in range(min(_zs3)//1024, (max(_zs3)-1)//1024 + 1):
                        for _kind, _tbl in (('f', _ftex), ('c', _ctex)):
                            _k2 = (_cx, _cz, _kind)
                            if _k2 in _dfh and abs(_dfh[_k2] - _fy) <= 256:
                                _tbl[(_cx, _cz)] = _f['tex']
            _rf_hit = _rf_miss = 0
            for _f in rm['quads'] + rm['tris']:
                _cl, _og = _sector_class(rm['verts'], _f)
                if _cl != 'flat': continue
                _vv = [rm['verts'][i] for i in _f['v']]
                _xs2 = [v[0] for v in _vv]; _zs2 = [v[2] for v in _vv]
                _cx0 = min(_xs2)//1024; _cx1 = (max(_xs2)-1)//1024
                _cz0 = min(_zs2)//1024; _cz1 = (max(_zs2)-1)//1024
                _fy = sum(v[1] for v in _vv)/float(len(_vv))
                _got = False; _hgot = False
                for _cx in range(_cx0, _cx1+1):
                    for _cz in range(_cz0, _cz1+1):
                        for _kind in ('f','c'):
                            if (_cx,_cz,_kind) in _dflat:
                                _got = True
                                # STRICT: does the derived HEIGHT match the face?
                                # tolerance 256 = one click, to absorb slopes.
                                if abs(_dfh[(_cx,_cz,_kind)] - _fy) <= 256:
                                    _hgot = True
                if _got: _rf_hit += 1
                else: _rf_miss += 1
                if _hgot: _p1['fheight'] += 1
            # ---- SERIALISE (P1 deliverable) --------------------------
            # Only TEXTURED segments are emitted: an untextured one is a
            # phantom discontinuity that is never drawn, so it would cost
            # traversal for nothing.
            _emit = []
            for (_a, _c, _l, _h), _bs in sorted(_bands.items()):
                _bl = sorted(_bs)[:3]                 # max observed is 3
                _emit.append((_a, _c, _l, _h, _bl))
            _rb = bytearray()
            _rb += struct.pack(">HHHH", 0x5232, _xS, _zS, len(_emit))  # "R2" magic: cells carry heights
            # P2 (2026-08-02): the cell record MUST carry the floor/ceiling
            # HEIGHT, not just the texture - a flats renderer cannot project a
            # sector floor without its Y. Heights are TR clicks (the same units
            # the level stores; world Y = clicks*256), signed, and -127 marks
            # SOLID ROCK / no surface. 6 bytes per cell: ftex, ctex, fy, cy.
            for _sx in range(_xS):
                for _sz in range(_zS):
                    _fo, _ce, _fi2, _be2, _ab2 = _cells[_sx*_zS + _sz]
                    _fo = max(-127, min(127, int(_fo)))
                    _ce = max(-127, min(127, int(_ce)))
                    _rb += struct.pack(">HHbb",
                        _ftex.get((_sx, _sz), 0xFFFF) & 0xFFFF,
                        _ctex.get((_sx, _sz), 0xFFFF) & 0xFFFF,
                        _fo, _ce)
            for _a, _c, _l, _h, _bl in _emit:
                _rb += struct.pack(">BBhhh", _a, len(_bl), _c, _l, _h)
                for _i in range(3):
                    if _i < len(_bl):
                        _y0, _y1, _tx = _bl[_i]
                        _rb += struct.pack(">hhH", int(_y0), int(_y1), _tx & 0xFFFF)
                    else:
                        _rb += struct.pack(">hhH", 0, 0, 0xFFFF)
            while len(_rb) & 7: _rb += b'\0'
            _srindex.append((rm['i'], len(_srblob)))
            _srblob += _rb
            _p1['emitseg'] += len(_emit)
            _p1['dflat'] += len(_dflat)
            _p1['rflat'] += _rf_hit + _rf_miss
            _p1['fhit'] += _rf_hit; _p1['fmiss'] += _rf_miss
            for _k, _v in _bands.items():
                _p1['banded'] += 1
                _p1['bandcount'] += len(_v)
                _p1['bandmax'] = max(_p1['bandmax'], len(_v))
                _p1['bandhist'][min(len(_v), 6)] = _p1['bandhist'].get(min(len(_v), 6), 0) + 1
            _p1['segs'] += len(_segs); _p1['real'] += len(_real)
            _p1['hit'] += _hit; _p1['miss'] += _miss; _p1['skew'] += _skew
        if _SDUMP and rm['i'] == _SDUMP_ROOM:
            import json as _json
            _out = {'room': rm['i'],
                    'verts': [[v[0], v[1], v[2]] for v in rm['verts']],
                    'faces': []}
            for _f in rm['quads'] + rm['tris']:
                _cl, _og = _sector_class(rm['verts'], _f)
                _out['faces'].append({'v': list(_f['v']), 'cls': _cl,
                                      'plane': list(_plane_key(rm['verts'], _f)),
                                      'tex': _f['tex']})
            open('/tmp/sector_room%d.json' % rm['i'], 'w').write(_json.dumps(_out))
            print("SECTORDUMP: wrote /tmp/sector_room%d.json (%d verts, %d faces)"
                  % (rm['i'], len(_out['verts']), len(_out['faces'])))
        if _SAUD:
            # G3: a sector renderer transforms 2D wall endpoints, not 3D verts.
            _pts = set()
            for _f in rm['quads'] + rm['tris']:
                _cl, _og = _sector_class(rm['verts'], _f)
                if _cl == 'wall':
                    for _i in _f['v']:
                        _v = rm['verts'][_i]
                        _pts.add((_v[0], _v[2]))       # (x,z) only
            _g3.append((rm['i'], len(rm['verts']), len(_pts),
                        len(rm['quads'])+len(rm['tris'])))
            _vv3 = rm['verts']
            for _f in rm['quads'] + rm['tris']:
                _cl, _og = _sector_class(_vv3, _f)
                _saud[_cl] += 1; _saud['faces'] += 1
                if _cl in ('wall','flat') and not _og:
                    _saud[_cl+'_offgrid'] += 1
        if _MAUD:
            _vv2 = rm['verts']
            for _f in rm['quads'] + rm['tris']:
                _k = (rm['i'], _plane_key(_vv2, _f), _f['tex'])
                _maud[_k] = _maud.get(_k, 0) + 1
                if _MAUD >= 2:
                    _mrect.setdefault(_k, []).append(
                        (_face_rect2d(_vv2, _f), len(_f['v'])))
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
        def face_k(f):
            # RAMP_PAL: per-face shade delta 0..7 (0 = full bright) packed
            # into u[0] bits 13-15 (real u < 256). Kernel SHADEPASS extracts
            # it and ORs k into the span (texel = base*8 walks the ramp).
            # Mapping matches the legacy SHADE_FACT bands: light >= 192 is
            # FULL BRIGHT (k=0) — those faces skip the second blit entirely,
            # which is also the fps lever (HW: always-shade cost -17%).
            if not RAMP_PAL: return 0
            vs=[verts[i][3] for i in f['v']]
            avg=sum(vs)//len(vs)
            k=(223-avg)>>5
            k=0 if k<0 else (7 if k>7 else k)
            if k>SHADE_KMAX: k=SHADE_KMAX
            _khist[k]=_khist.get(k,0)+1
            return k
        def vert_k(vi):
            # per-VERTEX shade delta (task 6 vertical Gouraud): same curve as
            # face_k but per corner. verts[] carries light 0..255 (255=bright).
            if not RAMP_PAL: return 0
            k=(223-verts[vi][3])>>5
            if k>SHADE_KMAX: k=SHADE_KMAX
            return 0 if k<0 else (7 if k>7 else k)
        def pack_uv(f):
            # EVERY corner's u carries its own vertex k in bits 13-15 (u<8192;
            # the kernel extracts per corner into K_BUF and interpolates along
            # the A edge chain). face_k() still feeds the k histogram.
            o=bytearray(); face_k(f)
            for j,(u,v) in enumerate(f['uv']):
                o+=struct.pack(">HH", u|(vert_k(f['v'][j])<<13), v)
            return o
        def face_plane(f):
            # STAGEDIET: 12-byte plane PREPENDED to the face record so the
            # kernel can backface-cull before staging verts/UVs.
            #   long0 = (ny<<16)|(nx&0xFFFF)   long1 = nz&0xFFFF   long2 = d
            # Kernel culls iff N.C < d, C = camera in room-local units.
            # N is quantized to |comp|<=127 so imult products against a
            # clamped s16 camera fit s32; d = min(N.v_i) over the face's
            # verts (handles bent quads) minus SLACK*|N| (conservative
            # margin for quantization — the exact screen cull stays the
            # final gate, this test must only take clear backfaces).
            vv=[verts[i] for i in f['v']]
            x0,y0,z0=vv[0][:3]; x1,y1,z1=vv[1][:3]; x2,y2,z2=vv[2][:3]
            ax,ay,az=x1-x0,y1-y0,z1-z0
            bx,by,bz=x2-x0,y2-y0,z2-z0
            nx=ay*bz-az*by; ny=az*bx-ax*bz; nz=ax*by-ay*bx
            if FACE_PLANES_SIGN<0: nx,ny,nz=-nx,-ny,-nz
            while max(abs(nx),abs(ny),abs(nz))>127:
                nx>>=1; ny>>=1; nz>>=1
            if nx==0 and ny==0 and nz==0:
                return struct.pack(">IIi",0,0,-(1<<27))  # degenerate: never cull
                # (-2^27 not INT32_MIN: kernel cmp N.C-d must not overflow)
            if FACE_PLANES_TIGHT:
                # MATCH THE SCREEN CULL (2026-07-29). The screen-space test
                # rejects iff the signed area about vertex 0 is negative, i.e.
                # iff N.C < N.v0. Using min(N.v_i) makes the pre-transform test
                # strictly WEAKER by (N.v0 - min) -- the bent-quad gap -- so
                # mildly back-facing faces survive it, get fully transformed,
                # and are then thrown away by the screen test. Measured cost of
                # that gap: 27.9% of ALL staged faces (CULLCOUNT + NOBFCULL,
                # 2026-07-29). Anchoring d at v0 makes the two tests agree, so
                # it rejects no face the screen cull would have kept -- up to
                # the 7-bit normal quantization, which is why SLACK survives as
                # a separate knob.
                d=nx*vv[0][0]+ny*vv[0][1]+nz*vv[0][2]
            else:
                d=min(nx*v[0]+ny*v[1]+nz*v[2] for v in vv)
            d-=FACE_PLANES_SLACK*math.isqrt(nx*nx+ny*ny+nz*nz)
            return struct.pack(">IIi",((ny&0xFFFF)<<16)|(nx&0xFFFF),nz&0xFFFF,d)
        for q in quads:
            if FACE_PLANES: b+=face_plane(q)
            b+=struct.pack(">HHHH", *q['v'])
            b+=pack_uv(q)
        for t in tris:
            if FACE_PLANES: b+=face_plane(t)
            b+=struct.pack(">HHH", *t['v'])
            b+=pack_uv(t)
        while len(b)&7: b+=b'\0'
        geom+=b
        soff=len(sect)
        sb=bytearray()
        sb+=struct.pack(">HH", rm['xS'], rm['zS'])
        sb+=struct.pack(">ii", rm['info_x'], rm['info_z'])
        nslope=0
        # DOORWAY CELLS (2026-07-30). TR1 does NOT record horizontal room links
        # in the sector floor data - measured: of 1199 floor==-127 sectors only
        # 76 have floor data and NONE carries an FD portal command, while 481
        # NORMAL-floor sectors carry roomBelow (that is how TR does a vertical
        # portal: a floor you fall through). So the old
        # "floor==-127 and roomBelow!=255 -> 0x7FFE" test matched nothing and
        # every room was packed as a SEALED BOX: Lara could only cross rooms by
        # walking off the grid edge, where room_wall_at reads out-of-bounds as
        # "not a wall" and the multi-room floor search snapped her across
        # (the "I get close and get transported" bug), and any room whose
        # interior never touched an edge was inescapable.
        # The real horizontal link is the room's PORTAL list, which we already
        # parse for render clipping. Mark every wall cell under a portal's XZ
        # footprint as 0x7FFE = walkable, neighbour supplies the floor.
        doorcells=set()
        for _pv in rm['portverts']:
            _xs=[v[0] for v in _pv]; _zs=[v[2] for v in _pv]
            _ys=[v[1] for v in _pv]
            # ☠️ A FLOOR/CEILING portal is degenerate in Y, and its XZ
            # footprint is a big horizontal rectangle. Marking cells under it
            # as walkable doorways turns a LEDGE RIM into something Lara
            # crosses horizontally instead of dropping off. Only WALL portals
            # (degenerate in x or z) are doorways.
            if max(_ys)-min(_ys) < 16:
                if _HOLEAUDIT: _HA['hplan'].append((rm['i'],len(_pv)))
                continue
            def _rng(a,b,n):
                # a portal in a WALL is degenerate on one axis (a plane of
                # constant x or z); that plane sits on the grid boundary, so
                # clamp it to the cell just inside this room rather than
                # stepping to b-1 and landing one cell short.
                lo=a//1024; hi=(b-1)//1024 if b>a else a//1024
                lo=min(max(lo,0),n-1); hi=min(max(hi,0),n-1)
                return range(lo,hi+1)
            for _sx in _rng(min(_xs),max(_xs),rm['xS']):
                for _sz in _rng(min(_zs),max(_zs),rm['zS']):
                    doorcells.add(_sx*rm['zS']+_sz)
        _ci=-1
        for (floor,ceil,fidx,below,above) in rm['sect']:
            _ci+=1
            # floor==-127 is BOTH walls and floor openings; roomBelow (below
            # != 255) means a vertical portal -> 0x7FFE OPENING (walkable off
            # the edge, room below supplies the floor), else 0x7FFF WALL
            # OPENING (0x7FFE) = walkable, another room supplies the floor.
            # Applies to BOTH a vertical portal (roomBelow) and a horizontal
            # one (an FD portal command = a doorway); only genuinely solid
            # rock stays 0x7FFF. No runtime change is needed - room_wall_at
            # treats only 0x7FFF as solid and room_floor_mr already skips
            # >=0x7FFE and takes the floor from a neighbouring room.
            hport = (sector_portal(fidx) is not None) or (_ci in doorcells)
            fy=(0x7FFE if (below!=255 or hport) else 0x7FFF) if floor==-127 else floor*256
            if _HOLEAUDIT:
                if floor==-127 and below!=255:
                    _HA['vert'].append((rm['i'],_ci//rm['zS'],_ci%rm['zS'],below,hport))
                elif floor==-127 and hport:
                    _HA['door'].append((rm['i'],_ci//rm['zS'],_ci%rm['zS']))
                elif floor!=-127 and below!=255:
                    _HA['fallthru'].append((rm['i'],_ci//rm['zS'],_ci%rm['zS'],below,floor*256))
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

    if _HOLEAUDIT:
        print("\n=== FLOOR-DATA CLASSIFICATION ===")
        print("  VERTICAL HOLES (floor==-127, roomBelow set) : %d" % len(_HA['vert']))
        print("  DOORWAY cells  (floor==-127, horiz portal)  : %d" % len(_HA['door']))
        print("  FALL-THROUGH   (REAL floor + roomBelow set) : %d" % len(_HA['fallthru']))
        print("  HORIZONTAL portals skipped (floor/ceiling)  : %d  %s" % (len(_HA['hplan']),_HA['hplan'][:12]))
        print("\n  -- vertical holes --")
        for v in _HA['vert'][:20]: print("     room %2d sect(%2d,%2d) below=%d hport=%s" % v)
        print("\n  -- REAL floor WITH a room below (TR's fall-through floors) --")
        for v in _HA['fallthru'][:30]: print("     room %2d sect(%2d,%2d) below=%-3d floorY=%d" % v)
        sys.exit(0)

    # ---- build Lara run-cycle blob (mrt_lara.bin) --------------------
    # Faces reference the SHARED atlas (textured -> tile UVs; colored ->
    # solid swatch cell). PSX MESH quads (initMesh) are NOT vertex-swapped -
    # only ROOM quads (readRoom) get the v2<->v3 swap. So Lara's quads keep
    # their file order (no swap); swapping them inverts winding -> the kernel
    # back-face-culls them -> missing polys.
    def lara_quad_uv(f):
        if f.get('leak') and _leakuv: return [_leakuv]*4   # HEADLEAK: brow leak
        if f['colored']:
            uc,vc=col_cell[f['tex']]; return [(uc,vc)]*4
        return inset_uv(face_uv(f['tex'], False, LARA_LVL)) # no swap (mesh, not room)
    def lara_tri_uv(f):
        if f.get('leak') and _leakuv: return [_leakuv]*3   # HEADLEAK: brow leak
        if f.get('fill'): return [f['filluv']]*3   # HEADFILL: one flat texel
        if f['colored']:
            uc,vc=col_cell[f['tex']]; return [(uc,vc)]*3
        return inset_uv(face_uv(f['tex'], False, LARA_LVL)[:3])
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
    # DEFAULT IS 0 (2026-07-31): at 400 this deleted 94 of her 375 faces, all
    # from the limbs, leaving 78 boundary edges -- i.e. it PUNCHED THE HOLES in
    # her arms and legs. It was a perf measure taken while she was mis-wound and
    # half-invisible anyway; with the winding fixed the holes are just damage.
    _MINAREA=float(os.environ.get("LARA_MINAREA","0"))
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
    # LARA_DROPMESH (2026-08-02): delete whole meshes outright, independent of
    # the area diet.  Meshes 10 and 13 are a symmetric 6-face pair at hip height
    # (her holsters).  They reference the WRONG atlas cell: on one build they
    # rendered as GOLD blocks under her butt, on the next as solid BLACK
    # quads on each cheek - structured art either way, so a valid cell owned by
    # something else, not sampling noise.  Until the objtex->atlas mapping is
    # fixed, dropping them is the honest stopgap: TR1 Lara without visible
    # holsters looks right, two black blotches do not.
    # LARA_TEXDUMP=<mesh ids>: what texture does each face of these meshes
    # reference?  Her HANDS (meshes 10 and 13, 6 faces each) render as solid
    # GOLD or BLACK quads at hip height while she runs - structured art, so a
    # VALID atlas cell owned by something else.  This names the cell.
    _TXD=set(int(x) for x in os.environ.get("LARA_TEXDUMP","").split(",") if x!="")
    if _TXD:
        for _fq in list(lquads)+list(ltris):
            _mm=_meshof(_fq['v'])
            if _mm not in _TXD: continue
            print("TEXDUMP mesh %2d: %s tex=%-6d colored=%s uv=%s"
                  % (_mm, 'quad' if len(_fq['v'])==4 else 'tri ',
                     _fq['tex'], _fq['colored'],
                     (lara_quad_uv(_fq) if len(_fq['v'])==4 else lara_tri_uv(_fq))[:2]))
    _DROP=set(int(x) for x in os.environ.get("LARA_DROPMESH","").split(",") if x!="")
    if _DROP:
        _n0=len(lquads)+len(ltris)
        lquads=[f for f in lquads if _meshof(f['v']) not in _DROP]
        ltris =[f for f in ltris  if _meshof(f['v']) not in _DROP]
        print("LARA_DROPMESH %s: removed %d faces"
              % (sorted(_DROP), _n0-len(lquads)-len(ltris)))
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
    def _modal_uv(mi):
        """Most common atlas texel over mesh mi's faces -> its dominant colour.
        The MODE, not the first texel: a rare specular highlight must never win
        (taking the first head texel nearly put a pale hair highlight on the
        hull)."""
        cand=[]
        for f in list(ltris)+list(lquads):
            if _meshof(f['v'])==mi and not f['colored'] and not f.get('fill'):
                for uv in (lara_tri_uv(f) if len(f['v'])==3 else lara_quad_uv(f)):
                    u,v=int(uv[0]),int(uv[1])
                    if 0<=u<ATLAS_W and 0<=v*ATLAS_W+u<len(atlas):
                        cand.append((atlas[v*ATLAS_W+u],(u,v)))
        if not cand: return None
        from collections import Counter as _CU
        m=_CU(i for i,_ in cand).most_common(1)[0][0]
        return next(uv for i,uv in cand if i==m)

    def _dark_uv(mi):
        """Darkest atlas texel over mesh mi's faces. For hole CAPS: they stand in
        for the inside of the body, so they must read as shadow, never as a
        bright surface colour."""
        def _lum(idx):
            # `palette` holds RGB16 in Jaguar fb order: R<<11 | B<<6 | G<<1
            if idx >= len(palette): return 999.0
            v=palette[idx]
            return (((v>>11)&0x1F)*30 + ((v>>1)&0x1F)*59 + ((v>>6)&0x1F)*11)/100.0
        best=None
        for f in list(ltris)+list(lquads):
            if _meshof(f['v'])!=mi or f['colored'] or f.get('fill'): continue
            for uv in (lara_tri_uv(f) if len(f['v'])==3 else lara_quad_uv(f)):
                u,v=int(uv[0]),int(uv[1])
                if not (0<=u<ATLAS_W and 0<=v*ATLAS_W+u<len(atlas)): continue
                idx=atlas[v*ATLAS_W+u]
                L=_lum(idx)
                if best is None or L<best[0]: best=(L,(u,v))
        return best[1] if best else None

    def _svol_about(faces, c):
        """Signed volume of a face list about point c."""
        t=0.0
        for q in faces:
            vs=[(_allv[i][0]-c[0],_allv[i][1]-c[1],_allv[i][2]-c[2]) for i in q]
            for k in range(1,len(vs)-1):
                a,b,d=vs[0],vs[k],vs[k+1]
                t+=(a[0]*(b[1]*d[2]-b[2]*d[1])-a[1]*(b[0]*d[2]-b[2]*d[0])
                    +a[2]*(b[0]*d[1]-b[1]*d[0]))/6.0
        return t

    # ---- CAPHOLES (2026-07-31) -------------------------------------------
    # Her TORSO ships an OPEN NECK HOLE -- its only 4 boundary edges, the socket
    # the separate head mesh plugs into. Wherever the head does not quite cover
    # that socket you look straight INTO the torso and see background through
    # her neck (user, on silicon). TR could ship it open because the PSX never
    # backface-culls character meshes; we do. Close any SMALL boundary loop with
    # a flat fan in that mesh's dominant colour, drawn first.
    _CAPH=int(os.environ.get("LARA_CAPHOLES","1"))
    if _CAPH:
        from collections import Counter as _CE
        _ec=_CE()
        for f in list(lquads)+list(ltris):
            vs=list(f['v'])
            for a,b in zip(vs, vs[1:]+vs[:1]): _ec[(min(a,b),max(a,b))]+=1
        _bym={}
        for e,n in _ec.items():
            if n==1: _bym.setdefault(_meshof([e[0]]),[]).append(e)
        _ncap=0
        for _mi4 in sorted(_bym):
            _es=_bym[_mi4]
            if not (3 <= len(_es) <= 6): continue
            _adj={}
            for a,b in _es:
                _adj.setdefault(a,[]).append(b); _adj.setdefault(b,[]).append(a)
            if any(len(v)!=2 for v in _adj.values()): continue   # not one clean loop
            _s=_es[0][0]; _loop=[_s]; _p=None; _c=_s
            while True:
                _n=[x for x in _adj[_c] if x!=_p]
                if not _n: break
                _p,_c=_c,_n[0]
                if _c==_s: break
                _loop.append(_c)
            if len(_loop)!=len(_es): continue                    # open / multi-loop
            # DARKEST texel, not the modal one. The cap plugs a socket you are
            # never supposed to see into, so it must read as shadow. Using the
            # torso's DOMINANT texel gave RGB(148,164,148) -- her pale top --
            # and the cap glared out at her neck as a bright vertical streak
            # through her hair, which I twice mistook for her FACE leaking
            # through (silicon 2026-07-31).
            _uvc=_dark_uv(_mi4) or _modal_uv(_mi4)
            if _uvc is None: continue
            _fan=[[_loop[0],_loop[k],_loop[k+1]] for k in range(1,len(_loop)-1)]
            # Match the mesh's own winding sense by comparing signed volumes
            # about the mesh centroid, rather than reasoning about the sign.
            _mf=[f['v'] for f in (list(lquads)+list(ltris)) if _meshof(f['v'])==_mi4]
            _mc=[sum(_allv[i][k] for q in _mf for i in q)/float(sum(len(q) for q in _mf))
                 for k in range(3)]
            if _svol_about(_fan,_mc)*_svol_about(_mf,_mc) < 0:
                _fan=[t[::-1] for t in _fan]
            for _t in _fan:
                ltris.append({'v':list(_t),'tex':0,'colored':False,
                              'fill':True,'filluv':_uvc})
                _ncap+=1
            print("LARA CAPHOLES: mesh %d neck/hole loop of %d verts -> +%d tris "
                  "(uv %s)"%(_mi4,len(_loop),len(_fan),_uvc))
        if not _ncap: print("LARA CAPHOLES: no small boundary loops found")

    # ---- HEADFILL (2026-07-31) ------------------------------------------
    # Her head is 85 faces packed into a ~6x5 PIXEL area at LOWRES -- about a
    # THIRD OF A PIXEL per face -- so most of them fall under the rasteriser's
    # minimum size and never paint at all, and the clear colour shows through.
    # That is a SIZE problem, not a winding or culling one, which is why every
    # orientation and cull-threshold fix failed on it, and why the user saw her
    # "fill in" at higher vertical resolution.
    #
    # Fix: draw a COARSE INNER HULL of the head FIRST, in a flat head-coloured
    # texel, so whatever the detail faces fail to cover lands on solid head
    # instead of background. 12 triangles at a few px each survive rasterising
    # where 85 at 0.3 px do not. It reuses her EXISTING head vertices -- the 8
    # that are extreme along each +-x+-y+-z diagonal -- so it adds no vertices,
    # needs no skinning change, and follows the head through every animation
    # for free. Being the convex hull of 8 of her own surface points, it can
    # never poke outside the head on a convex-ish mesh.
    _HFILL=int(os.environ.get("LARA_HEADFILL","1"))
    _HMESH=int(os.environ.get("LARA_HEADFILLMESH","14"))
    if _HFILL and _HMESH < len(_vb):
        _hv=list(range(_vb[_HMESH], _vb[_HMESH]+len(lara['mesh_verts'][_HMESH])))
        if len(_hv) >= 8:
            _hc=[sum(_allv[i][k] for i in _hv)/float(len(_hv)) for k in range(3)]
            # A BOX through 8 diagonal-extreme vertices was always the wrong
            # shape, and splitting the head by RADIUS to dodge the braid made it
            # worse: the outer 30% by radius is not the braid, it is the whole
            # outer SHELL in every direction, so the "core" hull shrank to an
            # inner blob and the "braid" hull rebuilt the same stretched box --
            # half her head went missing (silicon 2026-07-31).
            #
            # Do it properly: take the extreme vertex along each of 26
            # directions (6 axes + 12 edges + 8 corners) and CONVEX-HULL them.
            # That is a coarse head that wraps the real silhouette from every
            # angle, still made only of her own vertices (so skinning is free),
            # and still guaranteed inside the true convex hull of the head.
            # 3 = 342 sample directions. MEASURED: at 1 the hull leaves 15 of her
            # 46 head verts OUTSIDE it (9 units proud) -- that unbacked RIM is
            # what showed as a missing chunk once HEADLOD thinned the detail
            # faces covering it. At 3 only 6 verts are proud, by <=4 units.
            # LARA_HULLDIRS=0 is CORNERS ONLY - the 8 body diagonals, giving an
            # 8-vertex box hull of ~12 tris.
            #
            # ### DO NOT SET IT BACK TO 0 (2026-08-01) ###
            # 0 was a MISDIAGNOSIS and the user's verdict on it was that her
            # head was "halfway off". The reasoning that produced it -- "48 tris
            # over a 6x5 px head is 0.6 px each, so the hull is sub-pixel too"
            # -- divided the head area by EVERY hull triangle, but a convex hull
            # only ever shows its FRONT-FACING half, and the head is 7.5 x 8.9
            # buffer px, not 6 x 5. Re-measured against her real mesh-14 verts
            # (104 x 143 x 123 units, 1 buffer line ~= 13.8 units):
            #
            #   DIRS  tris  head left UNCOVERED  px per front-facing tri
            #     0     12      40.6u = 2.94 px          8.73
            #     1     32       8.7u = 0.63 px          3.27
            #     2     38       4.7u = 0.34 px          2.76
            #     3     48       3.9u = 0.28 px          2.18
            #
            # Nothing here is sub-pixel; even at 3 a front tri is over 2 px. The
            # only setting that fails is 0, and it fails by leaving ~3 px of a
            # ~8 px head with no backing at all -- about 38% of her head, which
            # is exactly what "halfway off" looks like. Coverage was never in
            # tension with triangle size on this mesh; buy the coverage.
            #
            # The hole around the HAIR TIE that 0 was meant to fix was never the
            # hull's doing: the hull backs the SKULL, the braid is a protrusion
            # outside it, and the braid's own faces had been thinned away by
            # LARA_HEADLOD. That is fixed where it belongs, in HEADLOD below.
            _DD=int(os.environ.get("LARA_HULLDIRS","3"))
            if _DD<=0:
                _dirs=[(x,y,z) for x in (-1,1) for y in (-1,1) for z in (-1,1)]
            else:
                _rg=range(-_DD,_DD+1)
                _dirs=[(x,y,z) for x in _rg for y in _rg for z in _rg
                       if (x,y,z)!=(0,0,0)]
            _P=[]
            for _d in _dirs:
                _b=max(_hv, key=lambda i,d=_d: (d[0]*(_allv[i][0]-_hc[0])
                                               +d[1]*(_allv[i][1]-_hc[1])
                                               +d[2]*(_allv[i][2]-_hc[2])))
                if _b not in _P: _P.append(_b)

            def _hull3(pts):
                """Incremental 3D convex hull -> list of [i,j,k] outward tris."""
                V=lambda i:_allv[i]
                def sub(a,b): return (a[0]-b[0],a[1]-b[1],a[2]-b[2])
                def cross(a,b): return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],
                                        a[0]*b[1]-a[1]*b[0])
                def dot(a,b): return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]
                # seed: 4 non-coplanar points
                if len(pts)<4: return []
                a=pts[0]
                b=next((p for p in pts[1:] if sub(V(p),V(a))!=(0,0,0)), None)
                if b is None: return []
                c=None
                for p in pts:
                    if p in (a,b): continue
                    if cross(sub(V(b),V(a)),sub(V(p),V(a)))!=(0,0,0): c=p; break
                if c is None: return []
                n0=cross(sub(V(b),V(a)),sub(V(c),V(a)))
                d=None
                for p in pts:
                    if p in (a,b,c): continue
                    if dot(n0,sub(V(p),V(a)))!=0: d=p; break
                if d is None: return []
                if dot(n0,sub(V(d),V(a)))>0: a,b=b,a      # make (a,b,c) face away from d
                F=[[a,b,c],[a,c,d],[a,d,b],[b,d,c]]
                def outward(f,p):
                    n=cross(sub(V(f[1]),V(f[0])),sub(V(f[2]),V(f[0])))
                    return dot(n,sub(V(p),V(f[0])))>1e-9
                for p in pts:
                    if p in (a,b,c,d): continue
                    vis=[f for f in F if outward(f,p)]
                    if not vis: continue
                    edges={}
                    for f in vis:
                        for e in ((f[0],f[1]),(f[1],f[2]),(f[2],f[0])):
                            k=(min(e),max(e))
                            edges[k]=edges.get(k,0)+1
                    horizon=[]
                    for f in vis:
                        for e in ((f[0],f[1]),(f[1],f[2]),(f[2],f[0])):
                            if edges[(min(e),max(e))]==1: horizon.append(e)
                    F=[f for f in F if f not in vis]
                    for (u,w) in horizon: F.append([u,w,p])
                return [f for f in F if len(set(f))==3]

            _hull=_hull3(_P)
            # ENFORCE a consistent orientation instead of trusting the
            # incremental build. Getting the horizon-edge direction backwards
            # flips SOME new faces and not others, and a globally MIXED hull
            # renders as half a head with stray quads beside it (silicon
            # 2026-07-31) -- the later whole-hull sign match cannot repair that.
            # A hull is convex, so "outward" is exact and unambiguous: a face
            # whose normal points back at the hull centroid is simply reversed.
            if _hull:
                _cc=[sum(_allv[i][k] for f in _hull for i in f)/float(3*len(_hull))
                     for k in range(3)]
                _fix=0
                for _f in _hull:
                    p=[_allv[i] for i in _f]
                    ux,uy,uz=(p[1][0]-p[0][0],p[1][1]-p[0][1],p[1][2]-p[0][2])
                    wx,wy,wz=(p[2][0]-p[0][0],p[2][1]-p[0][1],p[2][2]-p[0][2])
                    nx,ny,nz=uy*wz-uz*wy, uz*wx-ux*wz, ux*wy-uy*wx
                    if (nx*(p[0][0]-_cc[0])+ny*(p[0][1]-_cc[1])+nz*(p[0][2]-_cc[2])) < 0:
                        _f.reverse(); _fix+=1
                print("LARA HEADFILL: hull orientation normalised (%d of %d "
                      "tris reversed)"%(_fix,len(_hull)))
            # COVERAGE: how much of the head sticks OUT past the hull? Those
            # vertices are exactly the rim the hull cannot back, so their count
            # predicts where holes will appear if the detail faces are thinned.
            _out=0; _worst=0.0
            if _hull:
                for _i in _hv:
                    p=_allv[_i]; ins=True; wd=0.0
                    for _f in _hull:
                        q=[_allv[j] for j in _f]
                        ux,uy,uz=(q[1][0]-q[0][0],q[1][1]-q[0][1],q[1][2]-q[0][2])
                        wx,wy,wz=(q[2][0]-q[0][0],q[2][1]-q[0][1],q[2][2]-q[0][2])
                        nx,ny,nz=uy*wz-uz*wy, uz*wx-ux*wz, ux*wy-uy*wx
                        L=(nx*nx+ny*ny+nz*nz)**0.5 or 1.0
                        d=(nx*(p[0]-q[0][0])+ny*(p[1]-q[0][1])+nz*(p[2]-q[0][2]))/L
                        if d>1e-6: ins=False; wd=max(wd,d)
                    if not ins: _out+=1; _worst=max(_worst,wd)
            print("LARA HEADFILL: convex hull of %d extreme verts -> %d tris "
                  "| %d/%d head verts OUTSIDE it (max %.0f units proud)"
                  %(len(_P),len(_hull),_out,len(_hv),_worst))
            _pct=1.0; _core=_hv
            _box=_hull
            # Match the HEAD's own winding sense rather than reasoning about it:
            # compare signed volumes and reverse the box if the signs disagree.
            def _svol(faces):
                t=0.0
                for q in faces:
                    vs=[_allv[i] for i in q]
                    for k in range(1,len(vs)-1):
                        a,b,c=vs[0],vs[k],vs[k+1]
                        t+=(a[0]*(b[1]*c[2]-b[2]*c[1])-a[1]*(b[0]*c[2]-b[2]*c[0])
                            +a[2]*(b[0]*c[1]-b[1]*c[0]))/6.0
                return t
            _hface=[f['v'] for f in (list(lquads)+list(ltris)) if _meshof(f['v'])==_HMESH]
            if _svol(_box)*_svol(_hface) < 0:
                _box=[q[::-1] for q in _box]
            # Flat colour for the hull. Taking the FIRST head face's texel put a
            # pale HAIR-HIGHLIGHT on it, so the hull painted CREAM and glared
            # through the middle of her hair (silicon, 2026-07-31). Instead take
            # the MODAL atlas index over every head face's texel: hair covers
            # most of her head, so the mode is hair-brown and the rare specular
            # highlight can never win.
            _hcand=[]
            for f in list(ltris)+list(lquads):
                if _meshof(f['v'])==_HMESH and not f['colored']:
                    for _uv in (lara_tri_uv(f) if len(f['v'])==3 else lara_quad_uv(f)):
                        _u,_v=int(_uv[0]),int(_uv[1])
                        if 0<=_u<ATLAS_W and 0<=_v*ATLAS_W+_u<len(atlas):
                            _hcand.append((atlas[_v*ATLAS_W+_u],(_u,_v)))
            _huv=None
            if _hcand:
                from collections import Counter as _CC
                _mode=_CC(i for i,_ in _hcand).most_common(1)[0][0]
                _huv=next(uv for i,uv in _hcand if i==_mode)
                print("LARA HEADFILL: hull texel = atlas idx %d at uv %s "
                      "(mode of %d head texels)"%(_mode,_huv,len(_hcand)))
            _nfill=0
            if _huv is not None:
                for _q in _box:
                    if len(set(_q))<3: continue
                    # fan: works for the hull's triangles and for quads alike
                    for _tri in [(_q[0],_q[k],_q[k+1]) for k in range(1,len(_q)-1)]:
                        if len(set(_tri))<3: continue
                        ltris.append({'v':list(_tri),'tex':0,'colored':False,
                                      'fill':True,'filluv':_huv})
                        _nfill+=1
            print("LARA HEADFILL: +%d flat backing tris on mesh %d (uv %s)"
                  %(_nfill,_HMESH,_huv))
            # ---- HEADLOD: the head cannot resolve 85 faces at 6x5 px --------
            # Adding the hull on TOP of all 85 detail faces both overflows the
            # blob and pays for detail nobody can see. Keep only the N LARGEST
            # detail faces over the hull; the rest were sub-pixel anyway. This
            # takes her face count BELOW the original 375, so she also renders
            # faster than before. LARA_HEADLOD=-1 keeps every face.
            # 60, not 20: at 20 the hair's lower fringe and the hair TIE were
            # among the dropped faces, leaving a persistent black stripe under
            # the hair (seen on silicon 2026-07-31). The hull covers the skull
            # volume but not the hair skirt hanging below it.
            #
            # ### THE DEFAULT IS -1 (KEEP EVERY HEAD FACE) AND THAT IS LOAD-
            # ### BEARING (2026-08-01) ###
            # The head that was verified solid on silicon (df00e96, 425 faces)
            # was built with LARA_HEADLOD=-1 passed on the COMMAND LINE, while
            # this default sat at 40. Every later probe build was made without
            # that override, so it silently shipped a 40-face head -- and 40 is
            # below the threshold that keeps the braid and the hair tie, which
            # is precisely where the user then reported seeing through her. The
            # hull cannot cover for them: it backs the skull, and the braid is a
            # protrusion outside the skull's convex hull.
            # So the fix is not a smaller hull (that misdiagnosis is written up
            # at LARA_HULLDIRS above, and cost her half a head); it is to stop
            # thinning a mesh that is only 133 faces to begin with. She rendered
            # at 425 faces on silicon at full detail, so the budget is there.
            # If you lower this, expect the hair tie to open up again.
            _HLOD=int(os.environ.get("LARA_HEADLOD","-1"))
            if _HLOD >= 0:
                # PROTECT THE NECK-END BAND (2026-08-01). On this head -y is UP
                # (brow faces measure y=-59..-67; range -84..53), so the neck is
                # the most POSITIVE y. LARA_LODAUDIT showed the bottom band was
                # dropped ENTIRELY -- 0 of 4 kept -- because those faces are the
                # SMALLEST on the head (mean area 270 vs ~500 elsewhere), so an
                # area-ranked LOD sheds them first and every time. They are the
                # hair fringe that covers her neck, and losing them is the pale
                # sliver the user saw under her hair in EVERY captured frame
                # (persistent = missing geometry, unlike the brow leak, which
                # flickered). Keep them regardless of area: it costs 4 faces.
                _headsnap=[f for f in lquads+ltris if _meshof(f['v'])==_HMESH]
                _hys=[sum(_allv[i][1] for i in f['v'])/float(len(f['v']))
                      for f in _headsnap if not f.get('fill')]
                _ylo,_yhi=(min(_hys),max(_hys)) if _hys else (0.0,0.0)
                _NECKF=float(os.environ.get("LARA_HEADNECKBAND","0.80"))
                _ythr=_ylo+(_yhi-_ylo)*_NECKF
                def _necky(f):
                    return sum(_allv[i][1] for i in f['v'])/float(len(f['v'])) >= _ythr
                def _keep_head(seq, n):
                    head=[f for f in seq if _meshof(f['v'])==_HMESH and not f.get('fill')]
                    prot=[f for f in head if _necky(f)]
                    cand=[f for f in head if not _necky(f)]
                    cand.sort(key=lambda f: -_farea(f['v'][:3] if len(f['v'])==3 else f['v']))
                    # the protected neck band is ADDITIVE, not taken out of the
                    # detail budget: charging it to `n` thinned the CROWN from
                    # 9 kept to 6 to pay for 4 neck faces, trading one hole for
                    # another. LARA_HEADLOD is the area-ranked budget; the neck
                    # ring is 4 faces on top of it.
                    drop=set(id(f) for f in cand[n:])
                    return [f for f in seq if id(f) not in drop], len(drop)
                _nq_head=sum(1 for f in lquads if _meshof(f['v'])==_HMESH)
                _nt_head=sum(1 for f in ltris if _meshof(f['v'])==_HMESH and not f.get('fill'))
                _tot=_nq_head+_nt_head
                # split the budget between quads and tris in proportion
                _qn=int(round(_HLOD*_nq_head/float(_tot))) if _tot else 0
                lquads,_dq=_keep_head(lquads,_qn)
                ltris,_dt=_keep_head(ltris,_HLOD-_qn)
                print("LARA HEADLOD: head %d faces -> %d detail + %d hull "
                      "(dropped %d sub-pixel)"%(_tot,_tot-_dq-_dt,_nfill,_dq+_dt))
                # LARA_LODAUDIT=1: WHERE on the head did the dropped faces sit?
                # "Raise the LOD" only helps if the gap the user sees coincides
                # with dropped geometry. On this head -y is UP (the brow faces
                # measure y=-59..-67 and the range is -84..27), so the NECK is
                # the most POSITIVE y. Buckets the kept/dropped split by height.
                if os.environ.get("LARA_LODAUDIT"):
                    _all=[f for f in _headsnap if not f.get('fill')]
                    _kept=set(id(f) for f in lquads+ltris)
                    _ys=[]
                    for f in _all:
                        cy=sum(_allv[i][1] for i in f['v'])/float(len(f['v']))
                        _ys.append((cy, id(f) in _kept,
                                    _farea(f['v'][:3] if len(f['v'])==3 else f['v'])))
                    _lo=min(y for y,_,_ in _ys); _hi=max(y for y,_,_ in _ys)
                    print("   LODAUDIT: head y %.0f (top) .. %.0f (neck), %d faces"
                          %(_lo,_hi,len(_ys)))
                    _NB=6
                    for _b in range(_NB):
                        _a=_lo+(_hi-_lo)*_b/_NB; _z=_lo+(_hi-_lo)*(_b+1)/_NB
                        _in=[t for t in _ys if _a<=t[0]<=_z]
                        if not _in: continue
                        _k=sum(1 for t in _in if t[1])
                        print("     y %6.0f..%6.0f : kept %2d / %2d   mean area %6.0f%s"
                              %(_a,_z,_k,len(_in),
                                sum(t[2] for t in _in)/len(_in),
                                "   <-- NECK END" if _b==_NB-1 else
                                ("   <-- TOP" if _b==0 else "")))

    # ---- HEADAUDIT: is a pale patch her FACE leaking, or her BRAID? ---------
    # Answers it from the data instead of another flash: for every head face,
    # take the mean palette luminance of its texels and its centroid depth.
    # PALE faces at the FRONT of the head = her face (a leak when the camera is
    # behind her); pale faces at the BACK = braid/highlight and are correct.
    if os.environ.get("LARA_HEADAUDIT"):
        _hm=int(os.environ.get("LARA_HEADFILLMESH","14"))
        _pal=open(os.path.join(OUTDIR,PREFIX+"_pal.bin"),"rb").read()
        def _lum(idx):
            v=(_pal[idx*2]<<8)|_pal[idx*2+1]
            r=((v>>11)&0x1F); b=((v>>6)&0x1F); g=((v>>1)&0x1F)
            return (r*30+g*59+b*11)/100.0*255/31.0
        _rows=[]
        for f in list(lquads)+list(ltris):
            if _meshof(f['v'])!=_hm or f.get('fill'): continue
            uvs=(lara_tri_uv(f) if len(f['v'])==3 else lara_quad_uv(f))
            ls=[]
            for uv in uvs:
                u,v=int(uv[0]),int(uv[1])
                if 0<=u<ATLAS_W and 0<=v*ATLAS_W+u<len(atlas): ls.append(_lum(atlas[v*ATLAS_W+u]))
            if not ls: continue
            c=[sum(_allv[i][k] for i in f['v'])/float(len(f['v'])) for k in range(3)]
            _rows.append((sum(ls)/len(ls), c))
        if _rows:
            _zs=sorted(c[2] for _,c in _rows); _mid=_zs[len(_zs)//2]
            _pale=[r for r in _rows if r[0]>=110]
            _pf=sum(1 for l,c in _pale if c[2]> _mid); _pb=len(_pale)-_pf
            print("LARA HEADAUDIT: %d head faces, %d PALE (lum>=110)"%(len(_rows),len(_pale)))
            print("   pale faces: %d on the +z half, %d on the -z half (median z %.0f)"%(_pf,_pb,_mid))
            for l,c in sorted(_pale,key=lambda r:-r[0])[:6]:
                print("     lum %5.1f at (%.0f,%.0f,%.0f)"%(l,c[0],c[1],c[2]))
            # classify by the face NORMAL, not the centroid: a face is visible
            # from behind iff its normal points backward, regardless of where
            # its centre sits. Centroid-vs-median misses the SIDES of the head.
            _nb=[]
            for f in list(lquads)+list(ltris):
                if _meshof(f['v'])!=_hm or f.get('fill') or f['colored']: continue
                q=[_allv[i] for i in f['v'][:3]]
                ux,uy,uz=(q[1][0]-q[0][0],q[1][1]-q[0][1],q[1][2]-q[0][2])
                wx,wy,wz=(q[2][0]-q[0][0],q[2][1]-q[0][1],q[2][2]-q[0][2])
                nz=ux*wy-uy*wx
                nzz=uy*wz-uz*wy
                n=(uy*wz-uz*wy, uz*wx-ux*wz, ux*wy-uy*wx)
                uvs=(lara_tri_uv(f) if len(f['v'])==3 else lara_quad_uv(f))
                ls=[]
                for uv in uvs:
                    u,v=int(uv[0]),int(uv[1])
                    if 0<=u<ATLAS_W and 0<=v*ATLAS_W+u<len(atlas): ls.append(_lum(atlas[v*ATLAS_W+u]))
                if not ls: continue
                L=sum(ls)/len(ls)
                c=[sum(_allv[i][k] for i in f['v'])/float(len(f['v'])) for k in range(3)]
                if L>=110 and n[2]<0: _nb.append((L,c,n))
            print("   PALE faces whose NORMAL points BACKWARD (visible from behind): %d"%len(_nb))
            for L,c,n in sorted(_nb,key=lambda r:-r[0])[:8]:
                print("     lum %5.1f at (%.0f,%.0f,%.0f)"%(L,c[0],c[1],c[2]))
            _back=[r for r in _pale if r[1][2] <= _mid]
            print("   PALE faces on the BACK (-z) half -- these ARE visible from behind:")
            for l,c in _back:
                print("     lum %5.1f at (%.0f,%.0f,%.0f)"%(l,c[0],c[1],c[2]))
            _zr=[c[2] for _,c in _rows]; _yr=[c[1] for _,c in _rows]
            print("   head z range %.0f..%.0f   y range %.0f..%.0f"
                  %(min(_zr),max(_zr),min(_yr),max(_yr)))

    # LARA_HEADSORT (2026-07-31): there is no depth buffer and her face order is
    # BAKED here from model-space depth, so it is only correct from one viewing
    # direction. With the cull exact (LPLANES) and the silhouette solid, what
    # was left was her FACE painting over the back of her skull -- i.e. the head
    # is ordered front-last. The camera rides BEHIND her essentially always, so
    # reversing the head's depth order draws the back of her head last, on top.
    # -1 reverses (default), 1 keeps the whole-body convention.
    _HSORT=float(os.environ.get("LARA_HEADSORT","1"))
    def _fzkey(f):
        vs=[_allv[i] for i in f['v']]
        # HEADFILL backing tris sort FIRST within their mesh so the real
        # detail faces paint over them.
        _z=sum(v[2] for v in vs)/len(vs)
        _s=_HSORT if _meshof(f['v'])==int(os.environ.get("LARA_HEADFILLMESH","14")) else 1.0
        return (_meshof(f['v']), 0 if f.get('fill') else 1, -_z*_s)
    # ---- LARA WINDING FIX (2026-07-30) ----------------------------------
    # Her head shows NOTCHES: the inside of the skull shows through where front
    # faces are missing. Cull tuning (TINYKEEP 2/4/8/16 px^2) never touched it,
    # because the fault is the SOURCE DATA: 11 of her 375 faces are wound
    # OPPOSITE to the rest of their own mesh, so they are culled as back-faces
    # at any threshold. Audit (mesh: faces, minority-wound):
    #     mesh 1: 25/2, mesh 4: 25/2, mesh 14 (head): 85/7  -> 11 total
    # matching the "11 mis-wound TR1 faces" note. Fix per mesh: take the
    # MAJORITY orientation and reverse the odd ones out (vertex order and UVs
    # together, so texturing follows the flip).
    _mv=lara['mesh_verts']; _vb=lara['vbase_of']
    _lv={}
    for _mi,_vs in enumerate(_mv):
        for _k,_v in enumerate(_vs): _lv[_vb[_mi]+_k]=_v
    def _orient(f):
        p=[_lv[i] for i in f['v'][:3]]
        ax,ay,az=p[1][0]-p[0][0],p[1][1]-p[0][1],p[1][2]-p[0][2]
        bx,by,bz=p[2][0]-p[0][0],p[2][1]-p[0][1],p[2][2]-p[0][2]
        nx,ny,nz=ay*bz-az*by, az*bx-ax*bz, ax*by-ay*bx
        cx=sum(v[0] for v in p)/3.0; cy=sum(v[1] for v in p)/3.0; cz=sum(v[2] for v in p)/3.0
        return (nx,ny,nz,cx,cy,cz)
    _bymesh={}
    for _f in list(lquads)+list(ltris):
        _bymesh.setdefault(_meshof(_f['v']),[]).append(_f)
    _flipped=0
    for _mesh in (sorted(_bymesh) if os.environ.get("LARA_WINDAUTO") else []):
        _fs=_bymesh[_mesh]
        _vi=set()
        for _f in _fs: _vi.update(_f['v'])
        if not _vi: continue
        _ox=sum(_lv[i][0] for i in _vi)/len(_vi)
        _oy=sum(_lv[i][1] for i in _vi)/len(_vi)
        _oz=sum(_lv[i][2] for i in _vi)/len(_vi)
        _sign={}
        for _f in _fs:
            nx,ny,nz,cx,cy,cz=_orient(_f)
            _sign[id(_f)] = 1 if (nx*(cx-_ox)+ny*(cy-_oy)+nz*(cz-_oz)) >= 0 else -1
        _maj = 1 if sum(1 for v in _sign.values() if v>0)*2 >= len(_fs) else -1
        for _f in _fs:
            if _sign[id(_f)] != _maj:
                _f['v'] = list(reversed(list(_f['v'])))
                if 'uv' in _f and _f['uv']: _f['uv'] = list(reversed(list(_f['uv'])))
                _flipped += 1
    print("LARA WINDING FIX: reversed %d minority-wound faces of %d"
          % (_flipped, sum(len(v) for v in _bymesh.values())))
    if os.environ.get("LARA_OBJ"):
        # Export her rest-pose mesh as OBJ + audit it. A CLOSED mesh has no
        # boundary edges (every edge shared by exactly 2 faces); holes in the
        # source data show up as boundary edges immediately. Open lara.obj in
        # Blender to see her directly.
        _mv2=lara['mesh_verts']; _vb2=lara['vbase_of']
        _lv2={}
        for _mi,_vs in enumerate(_mv2):
            for _k,_v in enumerate(_vs): _lv2[_vb2[_mi]+_k]=_v
        _faces=list(lquads)+list(ltris)
        _used=sorted({i for f in _faces for i in f['v']})
        _remap={g:i+1 for i,g in enumerate(_used)}
        with open("lara.obj","w") as fh:
            fh.write("# Lara rest pose, exported from the TR1 level data\n")
            for g in _used:
                v=_lv2[g]; fh.write("v %d %d %d\n"%(v[0],-v[1],v[2]))
            # one OBJ group per source mesh, so Blender shows body parts by
            # name instead of anonymous connected components
            _bymesh={}
            for f in _faces:
                _bymesh.setdefault(_meshof(f['v']),[]).append(f)
            for _mi2 in sorted(_bymesh):
                fh.write("o mesh_%02d\n"%_mi2)
                for f in _bymesh[_mi2]:
                    fh.write("f %s\n"%" ".join(str(_remap[i]) for i in f['v']))
        from collections import Counter as _C
        _edges=_C()
        for f in _faces:
            vs=list(f['v'])
            for a,b in zip(vs, vs[1:]+vs[:1]):
                _edges[(min(a,b),max(a,b))]+=1
        _bound=[e for e,n in _edges.items() if n==1]
        _over=[e for e,n in _edges.items() if n>2]
        # ---- ORIENTATION, decided in GAME coordinates (2026-07-31) ----------
        # For a WATERTIGHT mesh the signed volume sum(P0 . (P1 x P2))/6 is
        # positive iff the faces are wound so their normals point OUTWARD.
        # The kernel keeps the face whose normal points AT the camera: rooms are
        # seen from inside (inward normals), Lara from outside (OUTWARD). So a
        # NEGATIVE volume here means every one of her faces is culled -- which
        # is exactly what a see-through head looks like. Note lara.obj negates y
        # (a reflection), so its volume sign is the OPPOSITE of this one; decide
        # the winding from THIS number, not from Blender's.
        _vol={}
        for f in _faces:
            _mi3=_meshof(f['v']); vs=[_lv2[i] for i in f['v']]
            for _t in range(1,len(vs)-1):
                a,b,c=vs[0],vs[_t],vs[_t+1]
                _vol[_mi3]=_vol.get(_mi3,0.0)+(a[0]*(b[1]*c[2]-b[2]*c[1])
                                              -a[1]*(b[0]*c[2]-b[2]*c[0])
                                              +a[2]*(b[0]*c[1]-b[1]*c[0]))/6.0
        _nin=sum(1 for v in _vol.values() if v<0)
        print("LARA ORIENT (game coords): %d/%d meshes NEGATIVE-volume "
              "(= the sense this kernel KEEPS -- do not 'fix' it)"%(_nin,len(_vol)))
        for _mi3 in sorted(_vol):
            print("     mesh %2d: signed volume %+12.0f  (%s)"
                  %(_mi3,_vol[_mi3],"INWARD" if _vol[_mi3]<0 else "outward"))
        print("LARA OBJ: %d verts, %d faces -> lara.obj"%(len(_used),len(_faces)))
        print("   edges %d | BOUNDARY (hole) %d | shared>2 (non-manifold) %d"
              %(len(_edges),len(_bound),len(_over)))
        _bym={}
        for e in _bound:
            _bym.setdefault(_meshof([e[0]]),[]).append(e)
        for _m2 in sorted(_bym):
            print("     mesh %2d: %d boundary edges"%(_m2,len(_bym[_m2])))
    lquads.sort(key=_fzkey)
    ltris.sort(key=_fzkey)
    print("LARA FACE DIET: quads %d->%d tris %d->%d (minarea %g)"%(
        _nq0,len(lquads),_nt0,len(ltris),_MINAREA))
    _DROPH=set(int(x) for x in os.environ.get("LARA_DROPFACE","").split(",") if x.strip()!="")
    _nqk=sum(1 for i in range(len(lquads)) if i not in _DROPH)
    _ntk=sum(1 for i in range(len(ltris)) if len(lquads)+i not in _DROPH)
    lb+=struct.pack(">HHHHHH", vcount, _nqk, _ntk,
                    nframes, ATLAS_W, atlas_h)
    # LARA WINDING FIX (2026-07-20): TR1 character meshes contain mirrored /
    # inconsistently-wound faces (the PSX never backface-culls models); the
    # Jaguar kernel DOES cull, which broke her elbows and opened see-through
    # gaps. Reorient every face outward (normal vs mesh centroid, T-pose
    # coords) so the cull keeps the visible side. Faces that are genuinely
    # two-sided exist as mirrored PAIRS in the data — reorienting preserves
    # one face per side, which is exactly right. LARA_WINDFIX=0 disables;
    # LARA_WINDSIGN flips the outward convention if the kernel's cull sense
    # ever changes.
    # DEFAULT IS 0 -- NO WINDING FIX AT ALL (settled on silicon 2026-07-31).
    # Her data is watertight and 100% consistently wound (Blender's exact
    # recalc-outside changes zero faces on zero meshes), and the raw sense is
    # the CORRECT one for this kernel: flashed raw, she faces away from the
    # camera with a solid body. Both older modes were damage:
    #   mode 1 (old default) reoriented 192 of 281 faces on a per-face centroid
    #     guess, i.e. it scrambled a correct mesh, and had to skip the head;
    #   mode 2 flipped all 375 -- she then rendered INSIDE-OUT, showing her face
    #     and chest while the camera was behind her ("Lara is backwards").
    # ★ Her raw meshes measure a NEGATIVE signed volume in game coords. That is
    # not a defect: the projection flips handedness (screen y runs DOWN), so
    # negative-volume winding is exactly what this kernel keeps. Do NOT "fix" it.
    _WINDFIX=int(os.environ.get("LARA_WINDFIX","0"))
    _WSIGN=float(os.environ.get("LARA_WINDSIGN","1"))
    _mcen={}
    for _mi in range(len(_vb)):
        _mvs=lara['mesh_verts'][_mi]
        if _mvs:
            _mcen[_mi]=tuple(sum(c[i] for c in _mvs)/len(_mvs) for i in range(3))
        else:
            _mcen[_mi]=(0,0,0)
    _nflip=[0]; _fliphist={}
    # never reorient the HEAD mesh (14): its face/hair region is concave, the
    # outward-centroid test misjudges it, and flipping the face polys painted
    # her FACE over the back of her hair ("head is turned", user 2026-07-20).
    # Same delicacy the old face diet learned (LARA_KEEPMESH=14).
    _WSKIP=set(int(x) for x in os.environ.get("LARA_WINDSKIP","14").split(",") if x!="")
    def _windfix(vl, uvl):
        if not _WINDFIX: return vl, uvl
        # MODE 2 (2026-07-31, Blender audit): the raw TR1 data is PROVABLY fine
        # -- every one of the 15 meshes is watertight (mesh 7's 4 boundary edges
        # are its neck hole) and 100% consistently wound, and Blender's exact
        # recalc-outside changes ZERO faces on ZERO meshes. The premise behind
        # mode 1 ("TR1 character meshes are inconsistently wound") is false for
        # this model, and its per-face centroid test reorients 192 of 281 faces,
        # i.e. it SCRAMBLES a correct mesh -- which is also why the head had to
        # be excluded. With the data consistent, the only other valid
        # orientation is a single GLOBAL reversal, applied to every mesh
        # (head included -- there is no per-face judgement left to get wrong).
        if _WINDFIX == 2:
            _nflip[0]+=1
            _fliphist[_meshof(vl)]=_fliphist.get(_meshof(vl),0)+1
            return vl[0:1]+vl[:0:-1], uvl[0:1]+uvl[:0:-1]
        if _meshof(vl) in _WSKIP: return vl, uvl
        p=[_allv[i] for i in vl[:3]]
        ux,uy,uz=p[1][0]-p[0][0],p[1][1]-p[0][1],p[1][2]-p[0][2]
        wx,wy,wz=p[2][0]-p[0][0],p[2][1]-p[0][1],p[2][2]-p[0][2]
        nx=uy*wz-uz*wy; ny=uz*wx-ux*wz; nz=ux*wy-uy*wx
        mc=_mcen[_meshof(vl)]
        fx=sum(_allv[i][0] for i in vl)/len(vl)-mc[0]
        fy=sum(_allv[i][1] for i in vl)/len(vl)-mc[1]
        fz=sum(_allv[i][2] for i in vl)/len(vl)-mc[2]
        if (nx*fx+ny*fy+nz*fz)*_WSIGN < 0:
            _nflip[0]+=1
            _fliphist[_meshof(vl)]=_fliphist.get(_meshof(vl),0)+1
            return vl[0:1]+vl[:0:-1], uvl[0:1]+uvl[:0:-1]
        return vl, uvl
    # DIAGNOSTIC (2026-07-25): drop specific Lara face indices (position in
    # lquads+ltris) to prove which faces paint skin on the back of her skull.
    _DROP=set(int(x) for x in os.environ.get("LARA_DROPFACE","").split(",") if x.strip()!="")
    if _DROP: print("LARA_DROPFACE: dropping %d faces %s"%(len(_DROP),sorted(_DROP)))
    # DIAGNOSTIC (2026-07-25): TINT specific faces to a flat, distinctive texel
    # instead of dropping them.  Dropping perturbs the blob's face ORDER, which
    # changes overdraw globally and makes a bisect unreliable; tinting leaves
    # count and order identical, so whatever turns the tint colour on screen IS
    # the face you are looking for.
    _TINT=set(int(x) for x in os.environ.get("LARA_TINTFACE","").split(",") if x.strip()!="")
    # LARA_TINTPALE=1 (2026-08-01): DIAGNOSTIC. Tint every PALE head face, in
    # EMIT order, so whatever is showing through her hair identifies itself
    # instead of being guessed at. Tinting (not dropping) keeps face count and
    # order identical, so nothing else about the frame changes.
    if os.environ.get("LARA_TINTPALE"):
        _hm2=int(os.environ.get("LARA_HEADFILLMESH","14"))
        def _lum2(idx):
            # 0..255, same scale as LARA_HEADAUDIT. Without the *255/31 this
            # topped out at 31 and could never reach LARA_PALELUM's default of
            # 110, so TINTPALE silently tinted NOTHING (found 2026-08-01).
            if idx>=len(palette): return 0.0
            v=palette[idx]
            r=((v>>11)&0x1F); b=((v>>6)&0x1F); g=((v>>1)&0x1F)
            return (r*30+g*59+b*11)/100.0*255/31.0
        _n=0
        for _e,_f in enumerate(list(lquads)+list(ltris)):
            if _meshof(_f['v'])!=_hm2 or _f.get('fill') or _f['colored']: continue
            _ls=[]
            for uv in (lara_tri_uv(_f) if len(_f['v'])==3 else lara_quad_uv(_f)):
                u,v=int(uv[0]),int(uv[1])
                if 0<=u<ATLAS_W and 0<=v*ATLAS_W+u<len(atlas): _ls.append(_lum2(atlas[v*ATLAS_W+u]))
            if _ls and sum(_ls)/len(_ls) >= float(os.environ.get("LARA_PALELUM","110")):
                _TINT.add(_e); _n+=1
        print("LARA_TINTPALE: tinting %d pale head faces (emit indices)"%_n)
    # ---- HEADLEAK (2026-08-01): the FIX the TINTPALE diagnostic pointed at ----
    # LARA_HEADAUDIT proved the pale patch popping through her hair is NOT the
    # braid: of 40 head faces 11 are pale and ALL 11 sit on the +z (face) half,
    # none on the back. Of those, exactly THREE have a normal pointing BACKWARD,
    # so the cull KEEPS them while the camera rides behind her -- they are her
    # BROW, high on the head (head y spans -84..27), and they flicker in and out
    # as the pose shifts them across the cull boundary. That is the user's "pop
    # in from her face on the upper right side"; it appeared in 3 of 20 captured
    # frames.
    # Retexture ONLY those to the darkest head texel. Not all 11 pale faces --
    # the other 8 face forward and are her actual FACE, which must stay intact
    # for any front-on view. Geometry, face count and emit order are untouched,
    # so nothing else about the frame changes.
    _HLEAK=int(os.environ.get("LARA_HEADLEAK","1"))
    _leakuv=None
    if _HLEAK:
        _hm3=int(os.environ.get("LARA_HEADFILLMESH","14"))
        # SAME SCALING AS LARA_HEADAUDIT (0..255). The TINTPALE diagnostic's
        # _lum2 omits the *255/31 and so tops out at 31 -- with the shared
        # LARA_PALELUM=110 threshold it can NEVER fire. That silently made this
        # test find nothing (2026-08-01); TINTPALE has the same latent bug.
        def _lum3(idx):
            if idx>=len(palette): return 0.0
            v=palette[idx]
            r=((v>>11)&0x1F); b=((v>>6)&0x1F); g=((v>>1)&0x1F)
            return (r*30+g*59+b*11)/100.0*255/31.0
        _thr=float(os.environ.get("LARA_PALELUM","110"))
        _dark=(1e9,None); _leak=[]; _dbg=[0,0,0]
        for _f in list(lquads)+list(ltris):
            # ☠️ DO NOT PUT `or _f['colored']` BACK (2026-08-01).
            # Skipping flat-colour faces is what left the leak alive after
            # af5e2d7 "darken EVERY pale head face" claimed to have closed it.
            # LARA_HEADAUDIT counts 85 head faces and 33 pale; this loop was
            # counting 77 and 25. The missing 8 are exactly her flat-COLOUR
            # head faces, and they are the brightest things on her head:
            #   swatch  6 / 13  rgb(205,148, 90)  lum 158.4
            #   swatch 14       rgb(222,164, 98)  lum 173.8
            # i.e. SKIN. Those luminances are the 159.0 / 174.6 faces the audit
            # lists as visible-from-behind, and they match the pale triangles
            # measured off the capture card at lum 151-180 punching through her
            # hair. The hull (lum 17) and the neck cap (lum 61) were both far
            # too dark to be the source, which is what ruled them out.
            # Colour faces need no special handling downstream: they reference
            # the atlas through a solid swatch cell exactly like textured faces
            # do, and lara_{tri,quad}_uv already tests `leak` BEFORE `colored`,
            # so marking one retextures it like any other face.
            if _meshof(_f['v'])!=_hm3 or _f.get('fill'): continue
            _uvs=(lara_tri_uv(_f) if len(_f['v'])==3 else lara_quad_uv(_f))
            _ls=[]
            for uv in _uvs:
                u,v=int(uv[0]),int(uv[1])
                if 0<=u<ATLAS_W and 0<=v*ATLAS_W+u<len(atlas):
                    _L=_lum3(atlas[v*ATLAS_W+u]); _ls.append(_L)
                    if _L<_dark[0]: _dark=(_L,(u,v))     # darkest head texel
            if not _ls: continue
            # same normal test the audit validated: n[2] < 0 == points backward
            q=[_allv[i] for i in _f['v'][:3]]
            ux,uy,uz=(q[1][0]-q[0][0],q[1][1]-q[0][1],q[1][2]-q[0][2])
            wx,wy,wz=(q[2][0]-q[0][0],q[2][1]-q[0][1],q[2][2]-q[0][2])
            nz=ux*wy-uy*wx
            cz=sum(_allv[i][2] for i in _f['v'])/float(len(_f['v']))
            _dbg[0]+=1
            if sum(_ls)/len(_ls) >= _thr: _dbg[1]+=1
            if nz < 0: _dbg[2]+=1
            # MODE 1 (default): darken EVERY pale head face. Restricting it to
            # backward-facing +z faces (mode 2, the first cut) left a bright
            # cream patch still flickering in her hair on silicon - measured
            # off the user's 2026-08-01 screencast, the leaking texels are
            # palette 145 RGB(216,160,96) and 143 RGB(200,144,88), i.e. SKIN.
            # At ~6 px across, the detail faces carry no legibility the hull
            # does not already give, so the trade is worth it: her face texels
            # go dark, and a flickering bright patch on the back of her skull
            # goes away. Set LARA_HEADLEAK=2 for the narrow test if she is ever
            # rendered close up (a cutscene), where her face must read.
            _pale_enough = sum(_ls)/len(_ls) >= _thr
            if _pale_enough and (_HLEAK == 1 or (nz < 0 and cz > 0)):
                _leak.append(_f)
        print("LARA HEADLEAK dbg: head faces seen %d, pale %d, backward %d, pale+backward+front %d"
              %(_dbg[0],_dbg[1],_dbg[2],len(_leak)))
        if _leak and _dark[1] is not None:
            _leakuv=_dark[1]
            for _f in _leak: _f['fill']=False; _f['leak']=True
            print("LARA HEADLEAK: retexturing %d PALE head faces (mode %d) to "
                  "the darkest head texel %s (lum %.1f)"
                  %(len(_leak),_HLEAK,_leakuv,_dark[0]))
        else:
            print("LARA HEADLEAK: no leaking pale faces found")
    # ---- LARA_MESHAUDIT=1 (2026-08-01): WHICH MESH is still bright? ---------
    # HEADLEAK only ever looks at the head. When a pale patch survives it, the
    # next question is which OTHER mesh is showing through, and guessing that
    # costs a ~210 s upload per guess. This answers it offline. Runs AFTER
    # HEADLEAK, so it reports what actually ships.
    if os.environ.get("LARA_MESHAUDIT"):
        _thrA=float(os.environ.get("LARA_PALELUM","110"))
        def _lumA(idx):
            if idx>=len(palette): return 0.0
            v=palette[idx]
            r=((v>>11)&0x1F); b=((v>>6)&0x1F); g=((v>>1)&0x1F)
            return (r*30+g*59+b*11)/100.0*255/31.0
        # ☠️ NAME EVERY LOCAL HERE WITH AN _a SUFFIX. This block sits in the
        # middle of main(), so a bare `_m` shadows the `math` alias and the
        # room-plane pass dies 200 lines later with
        # "'int' object has no attribute 'sqrt'" (cost one run, 2026-08-01).
        _pera={}
        for _fa in list(lquads)+list(ltris):
            _ma=_meshof(_fa['v'])
            _uva=(lara_tri_uv(_fa) if len(_fa['v'])==3 else lara_quad_uv(_fa))
            _lsa=[]
            for _t in _uva:
                _ua,_va=int(_t[0]),int(_t[1])
                if 0<=_ua<ATLAS_W and 0<=_va*ATLAS_W+_ua<len(atlas):
                    _lsa.append(_lumA(atlas[_va*ATLAS_W+_ua]))
            if not _lsa: continue
            _La=sum(_lsa)/len(_lsa)
            _ysa=[_allv[i][1] for i in _fa['v']]
            _da=_pera.setdefault(_ma,{'n':0,'pale':0,'max':0.0,'ytop':1e9,'paley':[]})
            _da['n']+=1
            if _La>_da['max']: _da['max']=_La
            if _La>=_thrA:
                _da['pale']+=1
                _da['paley'].append(sum(_ysa)/len(_ysa))
            _da['ytop']=min(_da['ytop'],min(_ysa))
        print("LARA MESHAUDIT (post-HEADLEAK) -- brightest surface per mesh:")
        for _ma in sorted(_pera):
            _da=_pera[_ma]
            _pya=('  pale faces at mean y %.0f..%.0f'
                  %(min(_da['paley']),max(_da['paley']))) if _da['paley'] else ''
            print("   mesh %2d: %3d faces, %3d PALE(>=%.0f), maxlum %5.1f, ytop %5.0f%s"
                  %(_ma,_da['n'],_da['pale'],_thrA,_da['max'],_da['ytop'],_pya))

    # ---- LARA_FLATAUDIT=1 (2026-08-02): would FLAT-SHADING Lara cut FACES? --
    # User asked what happens if Lara drops texture mapping for flat/Gouraud.
    # Losing textures only removes PER-PIXEL work, and per-pixel is 18% of Tom
    # (82% is per-face setup) - the world was flat-shaded tonight and measured
    # NULL. So the only way flat shading can pay is if it lets COPLANAR,
    # EDGE-ADJACENT faces MERGE into one, because face count is the sole lever
    # that has ever moved this machine.
    # This prints the UPPER BOUND on that: connected components of the
    # coplanar-adjacency graph. A real merge must also stay convex/simple, so
    # the true saving is <= this. If the upper bound is small, the idea is dead
    # without touching the board.
    # ☠️ locals are _b-suffixed - a bare _m here shadows the `math` alias and
    # kills the room-plane pass 200 lines later.
    if os.environ.get("LARA_FLATAUDIT"):
        def _nrmb(_fb):
            _ab,_bb,_cb=[_allv[i] for i in _fb['v'][:3]]
            _u=(_bb[0]-_ab[0],_bb[1]-_ab[1],_bb[2]-_ab[2])
            _w=(_cb[0]-_ab[0],_cb[1]-_ab[1],_cb[2]-_ab[2])
            _n=(_u[1]*_w[2]-_u[2]*_w[1],_u[2]*_w[0]-_u[0]*_w[2],_u[0]*_w[1]-_u[1]*_w[0])
            _L=math.sqrt(_n[0]**2+_n[1]**2+_n[2]**2)
            if _L<1e-9: return None,0.0
            _n=(_n[0]/_L,_n[1]/_L,_n[2]/_L)
            return _n,(_n[0]*_ab[0]+_n[1]*_ab[1]+_n[2]*_ab[2])
        _fl=list(lquads)+list(ltris)
        _texb=sum(1 for _f in _fl if not _f['colored'])
        _colb=sum(1 for _f in _fl if _f['colored'])
        _par=list(range(len(_fl)))
        def _find(a):
            while _par[a]!=a: _par[a]=_par[_par[a]]; a=_par[a]
            return a
        def _uni(a,b):
            a,b=_find(a),_find(b)
            if a!=b: _par[b]=a
        _info=[]
        for _i,_f in enumerate(_fl):
            _n,_d=_nrmb(_f); _info.append((_meshof(_f['v']),_n,_d,set(_f['v'])))
        # bucket by mesh so the pairwise scan stays small
        _bym={}
        for _i,(_ms,_n,_d,_vs) in enumerate(_info): _bym.setdefault(_ms,[]).append(_i)
        _adj=0
        for _ms,_idx in _bym.items():
            for _x in range(len(_idx)):
                _i=_idx[_x]; _ni,_di,_vi=_info[_i][1],_info[_i][2],_info[_i][3]
                if _ni is None: continue
                for _y in range(_x+1,len(_idx)):
                    _j=_idx[_y]; _nj,_dj,_vj=_info[_j][1],_info[_j][2],_info[_j][3]
                    if _nj is None: continue
                    if len(_vi & _vj)<2: continue          # must share an EDGE
                    if (_ni[0]*_nj[0]+_ni[1]*_nj[1]+_ni[2]*_nj[2])<0.999: continue
                    if abs(_di-_dj)>1.0: continue          # same plane
                    _adj+=1; _uni(_i,_j)
        _grp={}
        for _i in range(len(_fl)): _grp.setdefault(_find(_i),[]).append(_i)
        _merged=len(_grp)
        _multi=sum(1 for _g in _grp.values() if len(_g)>1)
        print("")
        print("=== LARA_FLATAUDIT: can flat shading cut her FACE COUNT? ===")
        print("    faces total            : %d  (textured %d, already flat-colour %d)"
              %(len(_fl),_texb,_colb))
        print("    coplanar adjacent pairs: %d" % _adj)
        print("    merge groups (UPPER BOUND on faces after merging): %d" % _merged)
        print("    groups with >1 face    : %d" % _multi)
        print("    => BEST CASE face cut  : %d faces (%.1f%%)"
              %(len(_fl)-_merged, 100.0*(len(_fl)-_merged)/max(len(_fl),1)))
        print("    [context: -45%% of her faces was worth ONE vsync rung;")
        print("     -13%% measured NULL. Under ~-30%% this cannot pay.]")
        print("")

    _TU,_TV=int(os.environ.get("LARA_TINTU","0")),int(os.environ.get("LARA_TINTV","0"))
    if _TINT: print("LARA_TINTFACE: tinting %d faces at uv(%d,%d)"%(len(_TINT),_TU,_TV))
    _fi=[0]
    # ---- LPLANES (2026-07-26): bake a real MESH-LOCAL plane per Lara face ----
    # Lara's faces ship a DUMMY plane, so the kernel's exact N.C cull never
    # applies to her and her ONLY hidden-surface test is a screen-space signed
    # area whose sign is quantised on sub-pixel triangles -> her face paints over
    # the back of her skull.  Her meshes are RIGID, so the normal is STATIC in
    # mesh-local space and can be baked here for free; the runtime then only has
    # to transform the CAMERA into each mesh's space (15/frame, not 375).
    # N is normalised to .12 (|N| = 4096) so the runtime can use muls.w; d = N.P0
    # carries the same .12 scale, so `N.C - d` is directly comparable.
    def _plane_of(vl):
        p=[_allv[i] for i in vl[:3]]
        ux,uy,uz=p[1][0]-p[0][0],p[1][1]-p[0][1],p[1][2]-p[0][2]
        wx,wy,wz=p[2][0]-p[0][0],p[2][1]-p[0][1],p[2][2]-p[0][2]
        nx=uy*wz-uz*wy; ny=uz*wx-ux*wz; nz=ux*wy-uy*wx
        L=_m.sqrt(nx*nx+ny*ny+nz*nz)
        if L<1e-9: return (0,0,0,-(1<<27))          # degenerate -> never culled
        k=4096.0/L
        nx,ny,nz=int(round(nx*k)),int(round(ny*k)),int(round(nz*k))
        d=nx*p[0][0]+ny*p[0][1]+nz*p[0][2]
        return (nx,ny,nz,d)
    # ---- LARA_SHADE (2026-07-26) ----------------------------------------
    # Lara ships with shade level k=0 on ALL 375 faces (verified: max u=246, so
    # the k bits in u[0] 13-15 are never set), while room geometry carries k=1..7.
    # She therefore renders FULL BRIGHT in a shaded world.  The visible symptom is
    # her pale HAIR-HIGHLIGHT texels (247,219,132) blazing on the back of her
    # skull, which at 320x120 reads as "her face is on the back of her head".
    # k is a per-face darkening step the SHADEPASS applies; 0 = full bright.
    _LSHADE=int(os.environ.get("LARA_SHADE","0"))
    if _LSHADE: print("LARA_SHADE: packing k=%d into u[0] of every Lara face"%_LSHADE)
    def _shade_uv(uvl, f):
        # ONLY textured faces. Her COLOURED faces use the reserved swatch slots
        # 242..253, which are NOT ramp-aligned (ramp bases are bi*RAMP_M), so
        # adding k there walks into 254/255 = the UI black/white -> her limbs
        # went grey-green/black with white blotches when I shaded everything.
        # once LARA_COLRAMP puts her flat tones on ramp bases, COLOURED faces
        # are shadeable too -- shading only half of her is what the user
        # rejected on silicon ("weird shading on her butt", discoloured shorts).
        if not _LSHADE: return uvl
        if f['colored'] and not _COLRAMP: return uvl
        u0,v0=uvl[0]
        return [((u0 & 0x1FFF) | (_LSHADE<<13), v0)] + list(uvl[1:])
    _pq=[]; _pt=[]                      # mesh-local planes, in EMIT order
    for f in lquads:
        if _fi[0] in _DROP:
            _fi[0]+=1; nq_drop=1; continue
        _fi[0]+=1
        vv4,uv4=_windfix(list(f['v']), list(lara_quad_uv(f)))
        if (_fi[0]-1) in _TINT: uv4=[(_TU,_TV)]*4
        _pq.append(_plane_of(vv4)); uv4=_shade_uv(uv4,f)
        lb+=struct.pack(">HHHH", *vv4)
        for (u,vv) in uv4: lb+=struct.pack(">HH",u,vv)
    for f in ltris:
        if _fi[0] in _DROP:
            _fi[0]+=1; continue
        _fi[0]+=1
        vv3,uv3=_windfix(list(f['v']), list(lara_tri_uv(f)))
        if (_fi[0]-1) in _TINT: uv3=[(_TU,_TV)]*3
        _pt.append(_plane_of(vv3)); uv3=_shade_uv(uv3,f)
        lb+=struct.pack(">HHH", *vv3)
        for (u,vv) in uv3: lb+=struct.pack(">HH",u,vv)
    # LPLANES table -> a generated header (no blob-format change, no new binary
    # in mrt_data.S).  Indexed EXACTLY like mq_list/mt_list: quad q, tri t.
    with open(os.path.join(OUTDIR,PREFIX+"_lplanes.h"),"w") as _f:
        # include guard: main.c pulls this in EARLY too, to size lara_blob[]
        # from the real face counts instead of a hand-maintained constant.
        _f.write("#ifndef MRT_LPLANES_H\n#define MRT_LPLANES_H\n")
        _f.write("/* generated by tr2jag_multiroom.py - MESH-LOCAL face planes for Lara.\n")
        _f.write("   N is .12 (|N|=4096), d = N.P0 at the same .12 scale, so the\n")
        _f.write("   runtime test is simply (N.C_local - d).  See LARA_HEAD_CAMPAIGN.md. */\n")
        _f.write("#define MRT_LPLANE_QCOUNT %d\n#define MRT_LPLANE_TCOUNT %d\n"%(len(_pq),len(_pt)))
        for nm,tab in (("q",_pq),("t",_pt)):
            _f.write("static const int16_t mrt_lplane_%sn[%d][3] = {\n"%(nm,max(len(tab),1)))
            for (nx,ny,nz,d) in tab: _f.write("  {%d,%d,%d},\n"%(nx,ny,nz))
            if not tab: _f.write("  {0,0,0},\n")
            _f.write("};\n")
            _f.write("static const int32_t mrt_lplane_%sd[%d] = {\n"%(nm,max(len(tab),1)))
            for (nx,ny,nz,d) in tab: _f.write("  %d,\n"%d)
            if not tab: _f.write("  0,\n")
            _f.write("};\n")
        _f.write("#endif /* MRT_LPLANES_H */\n")
    print("LPLANES: baked %d quad + %d tri mesh-local planes"%(len(_pq),len(_pt)))
    if _WINDFIX: print("LARA WINDING FIX: reoriented %d of %d faces (sign %g) per-mesh %s"%(
        _nflip[0], len(lquads)+len(ltris), _WSIGN, _fliphist))
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

    # FOURBPP (2026-08-04): pack the atlas at 4bpp (the PSX source's native
    # depth) so the Blitter reads HALF the texture bytes - this renderer is
    # bus-bound. Median-cut the 256-colour palette to 16 (weighted by atlas
    # usage), remap the 8bpp atlas to 4-bit indices, pack 2 texels/byte. Dest
    # framebuffer stays 8bpp so the 16 colours occupy CLUT entries 0..15.
    # Kernel pairs with A1_FIXED=PIXEL4 (make ... FOURBPP=1). ATLAS_W stays 256
    # PIXELS (the Blitter halves the byte pitch from the pixel-size flag).
    if int(os.environ.get("FOURBPP","0")):
        hist=[0]*256
        for _b in atlas: hist[_b]+=1
        def _dec(c): return ((c>>11)&31,(c>>6)&31,(c>>1)&31)   # R,B,G (Jag order)
        def _enc(r,b,g): return ((r&31)<<11)|((b&31)<<6)|((g&31)<<1)
        cols=[[_dec(palette[i]),hist[i]] for i in range(256) if hist[i]>0]
        if not cols: cols=[[_dec(palette[0]),1]]
        boxes=[cols]
        while len(boxes)<16:
            best=-1; bi=-1; bch=0
            for k,bx in enumerate(boxes):
                if len(bx)<2: continue
                for ch in range(3):
                    vals=[c[0][ch] for c in bx]; sp=max(vals)-min(vals)
                    if sp>best: best=sp; bi=k; bch=ch
            if bi<0: break
            bx=boxes.pop(bi); bx.sort(key=lambda c:c[0][bch]); mid=len(bx)//2
            boxes.append(bx[:mid]); boxes.append(bx[mid:])
        pal16=[]
        for bx in boxes:
            w=sum(c[1] for c in bx) or 1
            pal16.append((sum(c[0][0]*c[1] for c in bx)//w,
                          sum(c[0][1]*c[1] for c in bx)//w,
                          sum(c[0][2]*c[1] for c in bx)//w))
        while len(pal16)<16: pal16.append((0,0,0))
        def _near(rbg):
            best=1<<30; bj=0
            for j,p in enumerate(pal16):
                d=(rbg[0]-p[0])**2+(rbg[1]-p[1])**2+(rbg[2]-p[2])**2
                if d<best: best=d; bj=j
            return bj
        remap=[_near(_dec(palette[i])) for i in range(256)]
        packed=bytearray(len(atlas)//2)
        for i in range(0,len(atlas)&~1,2):
            packed[i>>1]=(remap[atlas[i]]<<4)|remap[atlas[i+1]]
        atlas=packed
        palette=[_enc(*pal16[i]) if i<16 else 0 for i in range(256)]
        print("FOURBPP: atlas -> %d bytes (4bpp), 16-colour palette" % len(atlas))

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
        # MUST match what was actually EMITTED into mrt_lara.bin, not the
        # pre-drop list length.  LARA_DROPFACE removes faces from the blob; if
        # these counts still said len(lquads) the C side would read past the end
        # of Lara's blob (tolerated in jagemu, BLACK SCREEN on silicon - cost a
        # flash 2026-07-25).
        f.write("#define MRT_LARA_QCOUNT     %d\n" % _nqk)
        f.write("#define MRT_LARA_TCOUNT     %d\n" % _ntk)
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
                      ("UPJUMP",28),("FALLBACK",29),("SLIDEBACK",32)]:
            # ☠️ LANIM_VAULT2 used to be emitted here as ("VAULT2",0) — state 0
            # is WALK, so it resolved to anim 1 and would have played her WALK
            # cycle as a vault. Nothing ever used it. The real vault anims are
            # picked by ledge height in main.c: VAULT(50)/CLIMB3(42)/CLIMBJUMP(26).
            f.write("#define LANIM_%-9s %d\n" % (nm, anim_for_state(st)))
        # ---- FOOTFALL FRAMES, straight from TR1's anim SOUND commands -------
        # Each animation's animCommand stream carries SOUND(frame, id) entries;
        # id 0 is the footstep. The frames are ABSOLUTE, so subtract frameStart
        # to get the offset within the cycle.
        #   RUN  (anim 0): frames 4 and 15 of 22   (0.18 / 0.68)
        #   WALK (anim 1): frames 4 and 24 of 36   (0.11 / 0.67)
        # main.c used to guess a single pair of EIGHTHS (2/8, 6/8) for both,
        # which is late for the run and badly wrong for the walk — its own
        # comment asked for exactly this table.
        _animBase=lara['anim']; _nAnim=len(atable)
        _foot=[]
        for _rel in range(_nAnim):
            _ao=pAnims+(_animBase+_rel)*32
            _f0=_u16(data,_ao+16)
            _acC=_u16(data,_ao+28); _acO=_u16(data,_ao+30)
            _p=pCmds+_acO*2; _hits=[]
            for _ in range(_acC):
                _op=_u16(data,_p); _p+=2
                if   _op==1: _p+=6
                elif _op==2: _p+=4
                elif _op in (3,4): pass
                elif _op in (5,6):
                    _fr=_u16(data,_p); _id=_u16(data,_p+2); _p+=4
                    if _op==5 and (_id & 0xFF)==0:
                        _d=_fr-_f0
                        if 0<=_d<255: _hits.append(_d)
                else: break
            _foot.append(((_hits+[255,255])[0], (_hits+[255,255])[1]))
        f.write("// TR1 footfall frames per animation (255 = none), from the\n")
        f.write("// anim SOUND commands with sound id 0. Offset within the cycle.\n")
        f.write("#define MRT_LARA_ANIMCOUNT %d\n" % _nAnim)
        f.write("static const unsigned char mrt_lara_foot[%d][2] = {\n" % _nAnim)
        for _i in range(0, _nAnim, 8):
            f.write("  " + " ".join("{%3d,%3d}," % t for t in _foot[_i:_i+8]) + "\n")
        f.write("};\n")
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
        if RAMP_PAL:
            f.write("// ramp palette: slot = base*RAMP_M + shade, 0=brightest;\n")
            f.write("// runtime shade pass adds k (0..RAMP_M-1) to the texel index\n")
            f.write("#define MRT_RAMP_K      %d\n" % RAMP_K)
            f.write("#define MRT_RAMP_M      %d\n" % RAMP_M)
        f.write("#define MRT_FACE_PLANES %d\n" % FACE_PLANES)
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

        # ---- DOOR + LEVER atlas UV rects (appended tiles; see atlas build) ---
        f.write("\n// door/lever textures appended to the atlas (real TR1\n"
                "// object-textures, not the flat swatch)\n")
        for _nm in ("DOOR","SW"):
            if _nm in door_tex_rect:
                _x0,_y0,_x1,_y1=door_tex_rect[_nm]
                f.write("#define %s_%s_TEX_U0 %d\n" % (up,_nm,_x0))
                f.write("#define %s_%s_TEX_V0 %d\n" % (up,_nm,_y0))
                f.write("#define %s_%s_TEX_U1 %d\n" % (up,_nm,_x1))
                f.write("#define %s_%s_TEX_V1 %d\n" % (up,_nm,_y1))

        # ---- ENTITIES ---------------------------------------------------
        # ☠️ THE WHOLE TR1 ORDER IS KEPT, including entities in rooms we did
        # not extract.  Trigger commands address entities BY INDEX into this
        # array, so compacting it would silently repoint every trigger.
        # Entities outside the extracted set get room = _ENT_NOROOM.
        _TN={0:"LARA",7:"ENEMY_WOLF",8:"ENEMY_BEAR",9:"ENEMY_BAT",
             35:"TRAP_FLOOR",40:"TRAP_DART_EMITTER",55:"SWITCH",
             57:"DOOR_1",58:"DOOR_2",59:"DOOR_3",60:"DOOR_4",
             68:"BRIDGE_1",69:"BRIDGE_2",70:"BRIDGE_3",83:"CRYSTAL",
             93:"MEDIKIT_SMALL",94:"MEDIKIT_BIG",169:"VIEW_TARGET"}
        f.write("\n// ---- ENTITIES (world units; TR1 index order PRESERVED --\n"
                "// trigger commands address entities by index into this array)\n")
        f.write("#define %s_ENT_NOROOM 255\n" % up)
        f.write("#define %s_ENTCOUNT   %d\n" % (up, len(entities)))
        for _t in ("SWITCH","DOOR_1","DOOR_4","MEDIKIT_SMALL","MEDIKIT_BIG",
                   "CRYSTAL","ENEMY_WOLF","ENEMY_BEAR","ENEMY_BAT"):
            _v=[k for k,v in _TN.items() if v==_t]
            if _v: f.write("#define %s_ENT_%-14s %3d\n" % (up,_t,_v[0]))
        f.write("static const struct { unsigned short type; unsigned char room;\n"
                "                      unsigned char yaw; int x, y, z;\n"
                "                      unsigned short flags; } %s_ent[%d] = {\n"
                % (PREFIX, len(entities)))
        for _i,_e in enumerate(entities):
            _rm = g2l[_e['room']] if _e['room'] in g2l else 255
            f.write("  {%4d,%4d,%4d, %7d,%7d,%7d, 0x%04X },  // %2d %s%s\n"
                    % (_e['type'], _rm, (_e['rot'] & 0xFFFF) >> 8,
                       _e['x'], _e['y'], _e['z'], _e['flags'], _i,
                       _TN.get(_e['type'], "type %d" % _e['type']),
                       "" if _rm != 255 else "  (room not extracted)"))
        f.write("};\n")

        # ---- FLOORDATA TRIGGERS ----------------------------------------
        # Layouts verified against format.h, not remembered:
        #   Command  func:5, tri:3, sub:7, end:1   -> sub = Trigger::Type
        #   TriggerCommand  args:10, action:5, end:1
        # ☠️ For SWITCH(2) / KEY(3) / PICKUP(4) the FIRST cmd's args is the
        # switch/key/pickup ENTITY INDEX, not an action -- lara.h consumes it
        # before reading any action.  CAMERA_SWITCH(1) likewise eats the NEXT
        # word as its parameter.  Both are stored verbatim; decoding is the
        # runtime's job, and getting this wrong shifts every later action.
        _trig=[]; _tcmd=[]
        for rm in rooms:
            for _s,(_fl,_ce,_fi,_bl,_ab) in enumerate(rm['sect']):
                _t=sector_triggers(_fi)
                if not _t: continue
                _trig.append((g2l[rm['i']], _s//rm['zS'], _s%rm['zS'],
                              _t['type'], _t['timer'], _t['once'], _t['mask'],
                              len(_t['cmds']), len(_tcmd)))
                _tcmd.extend(_t['cmds'])
        f.write("\n// ---- FLOORDATA TRIGGERS -------------------------------\n"
                "// type: 0 ACTIVATE 1 PAD 2 SWITCH 3 KEY 4 PICKUP 5 HEAVY\n"
                "//       6 ANTIPAD 7 COMBAT 8 DUMMY\n"
                "// cmd words are (end<<15)|(action<<10)|args, TR1 verbatim.\n"
                "// action: 0 ACTIVATE 1 CAMERA_SWITCH 2 FLOW 3 FLIP 4 FLIP_ON\n"
                "//   5 FLIP_OFF 6 CAMERA_TARGET 7 END 8 SOUNDTRACK 9 EFFECT\n"
                "//   10 SECRET 11 CLEAR_BODIES\n")
        f.write("#define %s_TRIGCOUNT    %d\n" % (up, len(_trig)))
        f.write("#define %s_TRIGCMDCOUNT %d\n" % (up, len(_tcmd)))
        f.write("static const struct { unsigned char room, sx, sz, type;\n"
                "                      unsigned char timer, once, mask, ncmd;\n"
                "                      unsigned short cmd0; } %s_trig[%d] = {\n"
                % (PREFIX, max(len(_trig),1)))
        for _r in _trig:
            f.write("  {%3d,%3d,%3d,%2d, %3d,%2d,%3d,%2d, %4d },\n" % _r)
        if not _trig: f.write("  {0,0,0,0, 0,0,0,0, 0 },\n")
        f.write("};\n")
        f.write("static const unsigned short %s_trigcmd[%d] = {\n"
                % (PREFIX, max(len(_tcmd),1)))
        for _i in range(0, max(len(_tcmd),1), 12):
            f.write("  " + "".join("0x%04X," % w for w in (_tcmd[_i:_i+12] or [0])) + "\n")
        f.write("};\n")
        print("entities: %d (%d in the extracted set); triggers: %d, %d cmd words"
              % (len(entities), sum(1 for e in entities if e['room'] in g2l),
                 len(_trig), len(_tcmd)))
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
    if RAMP_PAL and _khist:
        _tot=sum(_khist.values())
        print("face-shade k histogram:", " ".join("k%d:%d"%(k,_khist[k]) for k in sorted(_khist)),
              "| k=0 (skip) = %.0f%%" % (100.0*_khist.get(0,0)/_tot))
    print("total data    : %.1f KB" % ((len(atlas)+len(pal)+len(geom)+len(sect))/1024.0))
    if ATILE and _merged_total:
        print("ATLAS TILING: MERGED AWAY %d room faces (2x2 blocks -> 1 quad)"
              % _merged_total)
    elif ATILE:
        print("ATLAS TILING: 0 faces merged - the placement found no exact 2x2 blocks")

    if _SBUILD and _p1['real']:
        _t = _p1['real']
        print("\n=== P1: WALLS DERIVED FROM THE SECTOR GRID ===")
        print("    derived wall segments        : %d" % _p1['segs'])
        print("    real wall faces in the level : %d" % _t)
        print("      covered by a derived segment: %5d  (%.1f%%)"
              % (_p1['hit'], 100.0*_p1['hit']/_t))
        print("      NOT covered                 : %5d  (%.1f%%)"
              % (_p1['miss'], 100.0*_p1['miss']/_t))
        print("      not axis-aligned (skew)     : %5d  (%.1f%%)"
              % (_p1['skew'], 100.0*_p1['skew']/_t))
        print("    [P1 gate: >=90%% covered, or the sector model cannot draw the level]")
        if _p1['banded']:
            print("    TEXTURE BANDS (segments that got a texture from a face):")
            print("      segments textured : %d of %d derived (%.1f%%)"
                  % (_p1['banded'], _p1['segs'], 100.0*_p1['banded']/max(_p1['segs'],1)))
            print("      mean bands/segment: %.2f   max %d"
                  % (_p1['bandcount']/float(_p1['banded']), _p1['bandmax']))
            print("      band histogram    : " + "  ".join(
                  "%d:%d" % (k, _p1['bandhist'][k]) for k in sorted(_p1['bandhist'])))
        if _p1['rflat']:
            print("    FLATS (floor/ceiling planes from the cell table):")
            print("      derived flat planes : %d" % _p1['dflat'])
            print("      real flat faces     : %d" % _p1['rflat'])
            print("        covered           : %5d  (%.1f%%)"
                  % (_p1['fhit'], 100.0*_p1['fhit']/_p1['rflat']))
            print("        NOT covered       : %5d  (%.1f%%)"
                  % (_p1['fmiss'], 100.0*_p1['fmiss']/_p1['rflat']))
            print("        ...and the derived HEIGHT matches (<=1 click):")
            print("        height-matched    : %5d  (%.1f%%)   <-- the real number"
                  % (_p1['fheight'], 100.0*_p1['fheight']/_p1['rflat']))
        if _srblob:
            _idx = bytearray(struct.pack(">H", len(_srindex)))
            for _ri, _off in _srindex: _idx += struct.pack(">HI", _ri, _off)
            while len(_idx) & 7: _idx += b'\0'
            wr(PREFIX+"_sr.bin", bytes(_idx) + bytes(_srblob))
            print("    EMITTED %s_sr.bin: %d rooms, %d segments, %d bytes"
                  % (PREFIX, len(_srindex), _p1['emitseg'], len(_idx)+len(_srblob)))
            print("      (vs mrt_geom.bin at %d bytes for the polygon path)" % len(geom))
        print()

    if _SAUD and _saud['faces']:
        _n = _saud['faces']
        print("\n=== GATE G1: SECTOR-MODEL FIT (%d faces) ===" % _n)
        for _c in ('flat','wall','oblique'):
            print("    %-8s %5d  (%5.1f%%)" % (_c, _saud[_c], 100.0*_saud[_c]/_n))
        print("    of which NOT on the 1024 sector grid:")
        print("      flat off-grid %5d (%4.1f%% of all faces)"
              % (_saud['flat_offgrid'], 100.0*_saud['flat_offgrid']/_n))
        print("      wall off-grid %5d (%4.1f%% of all faces)"
              % (_saud['wall_offgrid'], 100.0*_saud['wall_offgrid']/_n))
        _need = _saud['oblique'] + _saud['flat_offgrid'] + _saud['wall_offgrid']
        print("    => NEEDS POLYGON FALLBACK: %d faces (%.1f%%)   [KILL if >15%%]"
              % (_need, 100.0*_need/_n))
        print()
        print("=== GATE G3: TRANSFORM BUDGET (per room, per frame) ===")
        print("    room   3D verts now   2D wall pts   reduction   faces")
        _g3.sort(key=lambda t: -t[1])
        for _r, _nv, _np, _nf in _g3[:10]:
            print("    %4d      %5d          %5d      -%4.1f%%     %4d"
                  % (_r, _nv, _np, 100.0*(_nv-_np)/max(_nv,1), _nf))
        _tv = sum(t[1] for t in _g3); _tp = sum(t[2] for t in _g3)
        print("    ALL ROOMS  %5d          %5d      -%4.1f%%"
              % (_tv, _tp, 100.0*(_tv-_tp)/max(_tv,1)))
        print("    [G3 target: <100 transformed points per frame]")
        print()

    if _MAUD and _maud:
        _tot_f = sum(_maud.values()); _grp = len(_maud)
        _sz = sorted(_maud.values(), reverse=True)
        print("\n=== MERGE AUDIT: coplanar + same-texture groups ===")
        print("  %d faces in %d groups  =>  OPTIMISTIC bound if each group"
              " merged to 1 face: %d faces (-%.1f%%)"
              % (_tot_f, _grp, _grp, 100.0*(_tot_f-_grp)/max(_tot_f,1)))
        for _t in (2, 4, 8, 16):
            _in = sum(n for n in _sz if n >= _t)
            _gg = sum(1 for n in _sz if n >= _t)
            print("    groups of >=%2d faces: %4d groups holding %5d faces"
                  " (%4.1f%% of all faces)" % (_t, _gg, _in, 100.0*_in/max(_tot_f,1)))
        print("    biggest groups:", _sz[:12])
        print()

    if _MAUD >= 2 and _mrect:
        def _decompose(rects, cap, shapes=None):
            if shapes is None: shapes = []
            """Greedy maximal-rectangle cover of the occupied cells.
            cap = max cells merged along either axis (the atlas-width limit).
            Anything that is not an axis-aligned QUAD in its plane basis
            (tris, slopes, non-rect quads) can never merge and is passed
            through untouched."""
            good = [r for r, nv in rects if r is not None and nv == 4]
            fixed = len(rects) - len(good)          # unmergeable, counted as-is
            if not good:
                return fixed
            us = sorted({r[0] for r in good} | {r[2] for r in good})
            vs = sorted({r[1] for r in good} | {r[3] for r in good})
            ui = {x: i for i, x in enumerate(us)}
            vi = {x: i for i, x in enumerate(vs)}
            W, H = len(us) - 1, len(vs) - 1
            if W <= 0 or H <= 0:
                return fixed + len(good)
            occ = [[False]*W for _ in range(H)]
            for (u0, v0, u1, v1) in good:
                for yy in range(vi[v0], vi[v1]):
                    for xx in range(ui[u0], ui[u1]):
                        occ[yy][xx] = True
            out = 0
            for yy in range(H):
                for xx in range(W):
                    if not occ[yy][xx]:
                        continue
                    w = 0
                    while xx + w < W and occ[yy][xx+w] and w < cap:
                        w += 1
                    h = 0
                    while yy + h < H and h < cap and \
                          all(occ[yy+h][xx+k] for k in range(w)):
                        h += 1
                    for dy in range(h):
                        for dx in range(w):
                            occ[yy+dy][xx+dx] = False
                    out += 1
                    shapes.append((w, h))
            return fixed + out
        for cap, label in ((99, "unlimited"), (4, "capped 4x4 (atlas width)"),
                           (2, "capped 2x2")):
            tot = 0; orig = 0; variants = {}
            for _k, v in _mrect.items():
                sh = []
                tot += _decompose(v, cap, sh)
                orig += len(v)
                for wh in sh:
                    if wh != (1, 1):
                        variants[(_k[2], wh[0], wh[1])] = 1   # (tex, w, h)
            # atlas cost: a TEXSCALE=2 tile is ~64x64 at 8bpp = 4096 B
            kb = sum(w*h for _, w, h in variants) * 4096 / 1024.0
            print("  GREEDY RECTANGLES, %-24s: %5d faces -> %5d  (-%.1f%%)"
                  "   | %d pre-tiled atlas variants, ~%.0f KB"
                  % (label, orig, tot, 100.0*(orig-tot)/max(orig,1),
                     len(variants), kb))
        print("  (current atlas is 280 KB; the Jaguar has 2 MB total)")
        # ---- PER-ROOM: only one room draws at a time, so this is what a
        # ---- player standing in a given room actually gets.
        _perroom = {}
        for _k, v in _mrect.items():
            r = _k[0]
            e = _perroom.setdefault(r, [0, 0])
            e[0] += len(v)
            e[1] += _decompose(v, 2)          # 2x2 cap = the affordable one
        print("\n  PER-ROOM (2x2 cap, the affordable tiling):")
        print("    room   faces  ->merged   cut     vs Lara(425)")
        for r, (a, b) in sorted(_perroom.items(), key=lambda kv: -kv[1][0])[:12]:
            drawn_before = a + 425
            drawn_after  = b + 425
            print("    %4d   %5d  ->%5d   -%4.1f%%   drawn %d->%d  (-%.1f%% of frame load)"
                  % (r, a, b, 100.0*(a-b)/max(a,1), drawn_before, drawn_after,
                     100.0*(drawn_before-drawn_after)/max(drawn_before,1)))
        # ---- what does a BUDGET actually buy? -------------------------------
        # A blanket pre-tile is unaffordable, but variants are not equal value:
        # tile only the ones that repay their atlas bytes. Cost of a (w,h)
        # variant = w*h*4096 B; benefit = (w*h - 1) faces per rectangle placed.
        for cap in (4, 2):
            val = {}
            for _k, v in _mrect.items():
                sh = []
                _decompose(v, cap, sh)
                for (w, h) in sh:
                    if (w, h) == (1, 1): continue
                    key = (_k[2], w, h)
                    e = val.setdefault(key, [0, w*h*4096])
                    e[0] += (w*h - 1)
            ranked = sorted(val.items(), key=lambda kv: -kv[1][0]/kv[1][1])
            print("  budget curve, cap %dx%d:" % (cap, cap))
            for budget_kb in (32, 64, 128, 256):
                spent = 0; saved = 0
                for key, (sv, cost) in ranked:
                    if spent + cost <= budget_kb*1024:
                        spent += cost; saved += sv
                print("     +%4d KB atlas -> %5d faces saved (%4.1f%% of 7750)"
                      % (budget_kb, saved, 100.0*saved/7750.0))
        print()

    if _RAUD and _raud_areas:
        _ar = sorted(a for a, _ in _raud_areas)
        _n = len(_ar)
        _SEC = 1024.0 * 1024.0                       # one TR sector, world u^2
        print("\n=== ROOM FACE AUDIT: %d faces (%d quads, %d tris) ===" %
              (_n, sum(1 for _, _k in _raud_areas if _k == 4),
               sum(1 for _, _k in _raud_areas if _k == 3)))
        print("  area percentiles (in SECTORS, 1 sector = 1024^2):")
        for _p in (1, 5, 10, 25, 50, 75, 90, 99):
            print("    p%-3d %10.4f sectors" % (_p, _ar[min(_n - 1, _n * _p // 100)] / _SEC))
        print("  cumulative share of faces below a threshold, and the share of")
        print("  total AREA they carry (what you would actually lose):")
        _tota = sum(_ar) or 1.0
        for _t in (0.001, 0.004, 0.01, 0.02, 0.05, 0.1, 0.25):
            _cut = _t * _SEC
            _c = sum(1 for a in _ar if a < _cut)
            _lost = sum(a for a in _ar if a < _cut)
            print("    < %6.3f sectors : %5d faces (%5.1f%% of faces) carrying %5.2f%% of area"
                  % (_t, _c, 100.0 * _c / _n, 100.0 * _lost / _tota))
        print()

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
