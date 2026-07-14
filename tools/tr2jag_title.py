#!/usr/bin/env python3
# tr2jag_title.py — extract the TR1 TITLE.PSX PASSPORT (model type 71) as a
# set of STATIC POSED geotex blobs (room0_tex format) + an 8bpp mini-atlas
# whose pixels are indexed into the EXISTING title_pal (nearest-colour map),
# so the title screen CLUT never changes.
#
# OUTPUTS (big-endian):
#   pass_geom.bin   NPOSE posed blobs, 8-aligned, concatenated
#   pass_atlas.bin  8bpp mini atlas (width 256), indices into title_pal
#   pass.h          #defines: NPOSE, per-pose byte offsets, atlas dims
#
# Poses: anim 0 (passport opening) sampled at NPOSE points 0..last frame.
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tr2jag_multiroom as T
from tr2jag_multiroom import _u16, _s16, _u32, _s32, _mat_id, _mat_clone, \
     _translate_rel, _rot_yxz, _decode_angles, _clampi16

LEVEL = os.environ.get("TRTITLE",
        os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets/extracted/PSXDATA/TITLE.PSX"))
OUT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # jaguar/
NPOSE = int(os.environ.get("PASS_POSES", "5"))
PASS_TYPE = int(os.environ.get("PASS_TYPE", "71"))
PREFIXP   = os.environ.get("PASS_PREFIX", "pass")

def main():
    data = open(LEVEL, 'rb').read()
    r = T.R(data)
    r.u32(); r.u32(); r.seek(8)
    off = r.u32(); tiles_off = off + 8
    cluts_off = tiles_off + T.NUM_TILES*T.TILE_PAGE_BYTES
    r.setpos(cluts_off + T.NUM_CLUTS*T.CLUT_BYTES)
    rooms, nrooms = T.read_all_rooms(r)
    r.seek(r.u32()*2)
    mb=r.u32(); pMeshData=r.p; r.seek(mb*2)
    mn=r.u32(); pMeshOffs=r.p; r.seek(mn*4)
    an=r.u32(); pAnims=r.p;    r.seek(an*32)
    r.seek(r.u32()*6); r.seek(r.u32()*8); r.seek(r.u32()*2)
    nds=r.u32(); pNodes=r.p;   r.seek(nds*4)
    fds=r.u32(); pFrame=r.p;   r.seek(fds*2)
    mc=r.u32();  pModels=r.p;  r.seek(mc*20)
    r.seek(r.u32()*32)                      # staticMeshes
    objCount=r.u32()
    objtex=[]
    for i in range(objCount):
        x0=r.u8();y0=r.u8();clut=r.u16();x1=r.u8();y1=r.u8()
        tile=r.u16()&0x3FFF;x2=r.u8();y2=r.u8();r.u16();x3=r.u8();y3=r.u8();r.u16()
        objtex.append(dict(tile=tile,clut=clut,uv=[(x0,y0),(x1,y1),(x2,y2),(x3,y3)]))

    # tile/clut pixel access (mirrors tr2jag_multiroom)
    def tile_nibble(ti,x,y):
        base=tiles_off+ti*T.TILE_PAGE_BYTES
        b=data[base+(y*256+x)//2]
        return (b&0xF) if (x&1)==0 else (b>>4)
    def clut_rgb555(ci,idx):
        c,=struct.unpack_from("<H", data, cluts_off+ci*T.CLUT_BYTES+idx*2)
        r5=c&31; g5=(c>>5)&31; b5=(c>>10)&31; a=(c!=0)
        return r5,g5,b5,a

    # ---- find the passport ----
    pm=-1
    for i in range(mc):
        if _u16(data,pModels+i*20)==PASS_TYPE: pm=i; break
    assert pm>=0, "passport (type 71) not found"
    mo=pModels+pm*20
    mcount=_u16(data,mo+4); mstart=_u16(data,mo+6)
    node  =_u32(data,mo+8); anim  =_u16(data,mo+16)
    print("passport: model %d meshes=%d meshStart=%d node=%d anim=%d" %
          (pm, mcount, mstart, node, anim))

    # ---- meshes ----
    meshes=[]; quads=[]; tris=[]; total_v=0
    for i in range(mcount):
        boff=_u32(data, pMeshOffs+(mstart+i)*4); base=pMeshData+boff
        vCount=_s16(data, base+10); vAbs=abs(vCount); p=base+12
        verts=[]
        for j in range(vAbs):
            verts.append((_s16(data,p),_s16(data,p+2),_s16(data,p+4))); p+=8
        p += vAbs*8 if vCount>0 else vAbs*2
        rCount=_u16(data,p); p+=2
        vb=total_v
        for q in range(rCount):
            v=[_u16(data,p),_u16(data,p+2),_u16(data,p+4),_u16(data,p+6)]
            fl=_u16(data,p+8); p+=10
            quads.append(dict(v=[vb+x for x in v], tex=fl&0x7FFF, colored=(fl&0x7FFF)<256))
        tCount=_u16(data,p); p+=2
        for t in range(tCount):
            v=[_u16(data,p),_u16(data,p+2),_u16(data,p+4)]
            fl=_u16(data,p+6); p+=8
            tris.append(dict(v=[vb+x for x in v], tex=fl&0x7FFF, colored=(fl&0x7FFF)<256))
        meshes.append(verts); total_v+=vAbs
    if os.environ.get("PASS_DOUBLE","0")=="1":
        # double-sided: reversed-winding copy of every face (models whose
        # front quad winds against the kernel's backface cull, e.g. the
        # home photo, get culled to their BACK from every angle otherwise)
        quads += [dict(v=list(reversed(q['v'])), tex=q['tex'], colored=q['colored'], rev=1)
                  for q in quads]
        tris  += [dict(v=list(reversed(t['v'])), tex=t['tex'], colored=t['colored'], rev=1)
                  for t in tris]
    print("verts=%d quads=%d tris=%d" % (total_v,len(quads),len(tris)))

    # ---- pose baker (mirrors build_lara.bake) ----
    ao=pAnims+anim*32
    fofs=_u32(data,ao); fs=data[ao+5]
    fw=(fs if fs>0 else (10+2*mcount))*2
    aFrameStart=_u16(data,ao+22); aFrameEnd=_u16(data,ao+24)
    nframes=max(1, aFrameEnd-aFrameStart+1)
    print("anim frames:", nframes)
    def bake(fidx):
        fb=pFrame+fofs+fidx*fw
        ab=fb+18
        def angword(j):
            o=ab+(2*j+1)*2
            return _u16(data,o), _u16(data,o+2)
        out=[]; m=_mat_id(); stack=[]
        def emit(m,mi):
            R,t=m
            for (lx,ly,lz) in meshes[mi]:
                out.append((_clampi16(t[0]+R[0][0]*lx+R[0][1]*ly+R[0][2]*lz),
                            _clampi16(t[1]+R[1][0]*lx+R[1][1]*ly+R[1][2]*lz),
                            _clampi16(t[2]+R[2][0]*lx+R[2][1]*ly+R[2][2]*lz)))
        w0,w1=angword(0); ax,ay,az=_decode_angles(w0,w1); _rot_yxz(m,ax,ay,az)
        emit(m,0)
        for i in range(1,mcount):
            nb=pNodes+(node+(i-1)*4)*4
            fl=_u32(data,nb); nx=_s32(data,nb+4); ny=_s32(data,nb+8); nz=_s32(data,nb+12)
            if fl&1: m=stack.pop()
            if fl&2: stack.append(_mat_clone(m))
            _translate_rel(m,nx,ny,nz)
            w0,w1=angword(i); ax,ay,az=_decode_angles(w0,w1); _rot_yxz(m,ax,ay,az)
            emit(m,i)
        return out

    # PASS_FORCE_TEX: remap every textured face to one objtex (the mansion
    # picture), inset to trim its baked border so the image FILLS the quad.
    ftex=os.environ.get("PASS_FORCE_TEX","")
    if ftex!="":
        ft=int(ftex); ins=int(os.environ.get("PASS_TEX_INSET","6"))
        o=objtex[ft]; us=[u for u,_ in o['uv']]; vs=[v for _,v in o['uv']]
        u0,u1,v0,v1=min(us)+ins,max(us)-ins,min(vs)+ins,max(vs)-ins
        objtex[ft]=dict(tile=o['tile'],clut=o['clut'],
                        uv=[(u0,v0),(u1,v0),(u1,v1),(u0,v1)])
        for f in quads+tris:
            f['tex']=ft; f['colored']=False
    # ---- mini atlas: unique textures used, shelf-packed at width 256 ----
    used=sorted(set(f['tex'] for f in quads+tris if not f['colored']))
    tiles=[]; grp={}
    for ti in used:
        o=objtex[ti]
        us=[p[0] for p in o['uv']]; vs=[p[1] for p in o['uv']]
        # passport textures are quads in the title file; keep 4-corner bbox
        umin,umax,vmin,vmax=min(us),max(us),min(vs),max(vs)
        w=umax-umin+1; h=vmax-vmin+1
        px=[]
        for y in range(vmin,vmax+1):
            for x in range(umin,umax+1):
                px.append(clut_rgb555(o['clut'],tile_nibble(o['tile'],x,y)))
        grp[ti]=(len(tiles),umin,vmin)
        tiles.append(dict(w=w,h=h,px=px))
    # shelf pack
    AW=256; sx=sy=sh=0; pos=[]
    for t in tiles:
        if sx+t['w']>AW: sy+=sh; sx=0; sh=0
        pos.append((sx,sy)); sx+=t['w']; sh=max(sh,t['h'])
    ah=sy+sh
    # nearest-colour map into the EXISTING title palette
    tp=open(os.path.join(OUT,'title_pal.bin'),'rb').read()
    tpal=[struct.unpack_from(">H",tp,i*2)[0] for i in range(256)]
    def unpack16(c): return ((c>>11)&31,(c>>1)&31,(c>>6)&31)
    tprgb=[unpack16(c) for c in tpal]
    def nearest(r5,g5,b5):
        bd=1<<30; bi=0
        for i,(rr,gg,bb) in enumerate(tprgb):
            d=(r5-rr)**2+(g5-gg)**2+(b5-bb)**2
            if d<bd: bd=d; bi=i
        return bi
    # colored faces: 6x6 solid swatch per colour index (bottom strip)
    col_used=sorted(set(f['tex'] for f in quads+tris if f['colored']))
    SW=6
    sw_y=ah; sw_of={}
    if col_used:
        ah+=SW
        for k,ci in enumerate(col_used): sw_of[ci]=(k*SW, sw_y)
    ncache={}
    atlas=bytearray(AW*ah)
    for t,(ax,ay) in zip(tiles,pos):
        i=0
        for y in range(t['h']):
            row=(ay+y)*AW+ax
            for x in range(t['w']):
                r5,g5,b5,a=t['px'][i]; i+=1
                k=(r5,g5,b5)
                if k not in ncache: ncache[k]=nearest(r5,g5,b5)
                atlas[row+x]=ncache[k]
    for ci in col_used:
        o=objtex[ci]; ux,uy=o['uv'][0]
        r5,g5,b5,a4=clut_rgb555(o['clut'],tile_nibble(o['tile'],ux,uy))
        k=(r5,g5,b5)
        if k not in ncache: ncache[k]=nearest(r5,g5,b5)
        cx,cy=sw_of[ci]
        for yy in range(SW):
            for xx in range(SW): atlas[(cy+yy)*AW+cx+xx]=ncache[k]
    print("mini atlas: 256x%d, %d tiles, %d colours mapped, %d swatches" % (ah,len(tiles),len(ncache),len(col_used)))

    # ---- emit NPOSE blobs ----
    def face_uv(f):
        if f['colored']:
            cx,cy=sw_of[f['tex']]
            return [(cx+1,cy+1),(cx+SW-2,cy+1),(cx+SW-2,cy+SW-2),(cx+1,cy+SW-2)]
        g,umin,vmin=grp[f['tex']]; ax,ay=pos[g]
        return [(ax+(u-umin),ay+(v-vmin)) for (u,v) in objtex[f['tex']]['uv']]
    blob=bytearray(); offs=[]
    for pi in range(NPOSE):
        fidx=(pi*(nframes-1))//max(1,NPOSE-1)
        verts=bake(fidx)
        # center on the bbox so the title camera can frame it blind
        xs=[v[0] for v in verts]; ys=[v[1] for v in verts]; zs=[v[2] for v in verts]
        cx=(min(xs)+max(xs))//2; cy=(min(ys)+max(ys))//2; cz=(min(zs)+max(zs))//2
        verts=[(x-cx,y-cy,z-cz) for (x,y,z) in verts]
        if pi==0:
            print("bbox after center: X[%d..%d] Y[%d..%d] Z[%d..%d]" % (
                min(xs)-cx,max(xs)-cx,min(ys)-cy,max(ys)-cy,min(zs)-cz,max(zs)-cz))
        offs.append(len(blob))
        b=bytearray()
        b+=struct.pack(">HHHHH", len(verts),len(quads),len(tris),AW,ah)
        b+=struct.pack(">hhh", 0,0,0)
        for (x,y,z) in verts: b+=struct.pack(">hhhH", x,y,z,255)
        for q in quads:
            b+=struct.pack(">HHHH", *q['v'])
            uv4=face_uv(q)[:4]
            if q.get('rev'): uv4=list(reversed(uv4))
            for (u,v) in uv4: b+=struct.pack(">HH",u,v)
        for t in tris:
            b+=struct.pack(">HHH", *t['v'])
            uv3=face_uv(t)[:3]
            if t.get('rev'): uv3=list(reversed(uv3))
            for (u,v) in uv3: b+=struct.pack(">HH",u,v)
        while len(b)&7: b+=b'\0'
        blob+=b
    open(os.path.join(OUT,PREFIXP+'_geom.bin'),'wb').write(blob)
    open(os.path.join(OUT,PREFIXP+'_atlas.bin'),'wb').write(atlas)
    with open(os.path.join(OUT,PREFIXP+'.h'),'w') as f:
        f.write("/* generated by tr2jag_title.py */\n")
        f.write("#define PASS_NPOSE %d\n" % NPOSE)
        f.write("#define PASS_ATLAS_W 256\n#define PASS_ATLAS_H %d\n" % ah)
        for i,o in enumerate(offs):
            f.write("#define PASS_POSE%d_OFF %d\n" % (i,o))
    print("%s_geom.bin %d bytes, %d poses" % (PREFIXP, len(blob), NPOSE))

if __name__=="__main__":
    main()
