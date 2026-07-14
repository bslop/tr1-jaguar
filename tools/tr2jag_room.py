#!/usr/bin/env python3
# tr2jag_room.py
#
# HOST tool: extract ONE textured room's GEOMETRY (verts + faces + per-face UVs)
# from a PlayStation Tomb Raider 1 level (.PSX) for the Atari Jaguar renderer.
#
# This EXTENDS tr2jag_tex.py (same LEVEL1.PSX parse) but, instead of skipping the
# rooms, it CAPTURES room 0's geometry, then builds a COMPACT 8bpp atlas that
# holds only the object-textures room 0 actually uses, and emits per-face UVs into
# that small atlas.
#
# PROOF LINES (OpenLara src/format.h is the authoritative PSX reader):
#   readRoom() TR1_PSX      format.h:5112 .. 5404  (geometry block layout below)
#   room block bytes (all little-endian), starting after Info(16B)+size(u32):
#     [startOffset]
#       u16  (skip 2)                         format.h:5120-5122  (TR1_PSX seek 2)
#       s16  vCount                           format.h:5169
#       vCount * { s16 x, s16 y, s16 z,       format.h:5283-5286  (short3 pos)
#                  u16 lighting }             = 8 bytes each
#       s16  rCount                           format.h:5335-5337
#       rCount * { u16 v0,v1,v2,v3, u16 flags } = 10 bytes (FACE4)  readFace 5356
#       s16  tCount                           format.h:5361
#       tCount * { u16 v0,v1,v2,   u16 flags } =  8 bytes (FACE3)  readFace 5377
#     then setPos(startOffset + size*2) -> portals/sectors/lights/meshes trailer
#   PSX quad swap: swap vertices[2]<->[3]     format.h:5383-5389
#     and texCoord[2]<->[3] to keep pairing   mesh.h:1114-1118
#   face texture index = flags.value & 0x7FFF (texture:15,doubleSided:1) format.h:1699
#   objectTexture 16B {x0,y0,clut,x1,y1,tile:14,x2,y2,unk,x3,y3,attr}  format.h:6130
#   Tile4 / CLUT / texel decode : see tr2jag_tex.py header (unchanged)
#
# TR1 room vertex coords: x,z are ROOM-LOCAL (world units), y is ABSOLUTE world Y.
#   => offset stored is offX=info.x, offY=0, offZ=info.z ; world = local + offset.
#
# OUTPUTS (all big-endian):
#   src/platform/jaguar/room0_atlas.bin : compact 8bpp indexed atlas, row-major
#   src/platform/jaguar/room0_pal.bin   : 256 * u16 RGB16 (Jaguar fb fmt)
#   src/platform/jaguar/room0_tex.bin   : header + verts + quads(+uv) + tris(+uv)
#   src/platform/jaguar/room0_tex.h     : documents the byte layout
#   tr1_psx/tex_preview/room0_atlas.png : decoded atlas preview
#
# Jaguar RGB16 = R5<<11 | B5<<6 | G6  (blue in the MIDDLE), big-endian.

import struct, sys, os, zlib
from collections import Counter

LEVEL   = __import__("os").environ.get("TRLEVEL", __import__("os").path.join(__import__("os").path.dirname(__import__("os").path.dirname(__import__("os").path.abspath(__file__))), "assets/extracted/PSXDATA/LEVEL1.PSX"))
OUTDIR  = __import__("os").path.dirname(__import__("os").path.dirname(__import__("os").path.abspath(__file__)))
PREVDIR = "/tmp/tr1_tex_preview"

TILE_PAGE_BYTES = 256*256//2   # 32768  (Tile4)
CLUT_BYTES      = 16*2         # 32     (16 x ColorCLUT u16)
NUM_TILES       = 13
NUM_CLUTS       = 1024

class R:
    def __init__(self, data): self.d = data; self.p = 0
    def setpos(self, p): self.p = p
    def seek(self, n): self.p += n
    def u8(self):  v = self.d[self.p]; self.p += 1; return v
    def u16(self): v = struct.unpack_from("<H", self.d, self.p)[0]; self.p += 2; return v
    def s16(self): v = struct.unpack_from("<h", self.d, self.p)[0]; self.p += 2; return v
    def u32(self): v = struct.unpack_from("<I", self.d, self.p)[0]; self.p += 4; return v
    def s32(self): v = struct.unpack_from("<i", self.d, self.p)[0]; self.p += 4; return v

def room_trailer(r, capture=False):
    # advance past a room's post-geometry trailer to the next room (format.h:5406+)
    # capture=True: also return (xSectors, zSectors, sectors[]) for collision.
    r.seek(r.u16()*32)             # portals    (u16 count, 32B each)
    z = r.u16(); x = r.u16()       # sectors: zSectors,xSectors
    sect = None
    if capture:
        # TR1 Sector (8B): u16 floorIndex, u16 boxIndex, u8 roomBelow, s8 floor,
        #                  u8 roomAbove, s8 ceiling.  layout: sectors[sx*zSectors+sz]
        sect = []
        for _ in range(z*x):
            r.u16(); r.u16()                       # floorIndex, boxIndex (unused here)
            roomBelow = r.u8()
            floor     = struct.unpack("b", bytes([r.u8()]))[0]
            roomAbove = r.u8()
            ceiling   = struct.unpack("b", bytes([r.u8()]))[0]
            sect.append(dict(floor=floor, ceiling=ceiling,
                             roomBelow=roomBelow, roomAbove=roomAbove))
    else:
        r.seek(z*x*8)              # 8B each
    r.seek(2)                      # ambient (u16)  (TR1: no ambient2/lightMode)
    r.seek(r.u16()*20)             # lights   (u16 count, 20B PSX)
    r.seek(r.u16()*20)             # meshes   (u16 count, 20B PSX)
    r.seek(4)                      # alternateRoom(s16) + flags(u16)
    if capture:
        return x, z, sect          # xSectors, zSectors, sectors

def skip_room(r):
    r.seek(16)                     # Info
    size = r.u32()
    start = r.p
    r.setpos(start + size*2)       # skip geometry
    room_trailer(r)

def read_room_geom(r):
    # read Info + geometry of the room at r.p, capturing verts+faces. Leaves r
    # positioned at the NEXT room (runs the trailer). Returns a dict.
    info_x  = r.s32(); info_z = r.s32(); yBottom = r.s32(); yTop = r.s32()
    size = r.u32()
    start = r.p
    r.seek(2)                      # TR1_PSX pad (format.h:5120)

    vCount = r.s16()
    verts = []
    for _ in range(vCount):
        x = r.s16(); y = r.s16(); z = r.s16(); light = r.u16()
        verts.append((x, y, z, light))

    rCount = r.s16()
    quads = []
    for _ in range(rCount):
        v = [r.u16(), r.u16(), r.u16(), r.u16()]
        flags = r.u16()
        tex = flags & 0x7FFF
        # PSX quad: swap vertices[2]<->[3] AND uv[2]<->[3] (format.h:5386, mesh.h:1118)
        v[2], v[3] = v[3], v[2]
        quads.append(dict(v=v, tex=tex, swapped=True))

    tCount = r.s16()
    tris = []
    for _ in range(tCount):
        v = [r.u16(), r.u16(), r.u16()]
        flags = r.u16()
        tex = flags & 0x7FFF
        tris.append(dict(v=v, tex=tex))

    # jump to end of geometry block, run trailer to reach next room (capture sectors)
    r.setpos(start + size*2)
    xSectors, zSectors, sectors = room_trailer(r, capture=True)

    return dict(info_x=info_x, info_z=info_z, yTop=yTop, yBottom=yBottom,
                verts=verts, quads=quads, tris=tris,
                xSectors=xSectors, zSectors=zSectors, sectors=sectors,
                geom_start=start, geom_end=start + size*2)

def main():
    data = open(LEVEL, "rb").read()
    print("input      : %s (%d bytes)" % (LEVEL, len(data)))

    r = R(data)
    r.u32(); r.u32()                       # magic handling -> pos = 8
    r.seek(8)                              # pos = 16
    offsetTexTiles = r.u32()
    texTiles = offsetTexTiles + 8
    tiles_off = texTiles
    cluts_off = tiles_off + NUM_TILES * TILE_PAGE_BYTES
    r.setpos(cluts_off + NUM_CLUTS * CLUT_BYTES)
    print("tiles @ %d  cluts @ %d" % (tiles_off, cluts_off))

    # ---- readDataArrays ----
    r.seek(4)                              # unused u32
    roomsCount = r.u16()
    print("roomsCount : %d" % roomsCount)

    # ---- ROOM 0 : capture geometry, then skip rooms 1..N-1 ----
    room0 = read_room_geom(r)
    print("room0 geom block : bytes [%d .. %d)" % (room0['geom_start'], room0['geom_end']))
    print("room0 info       : x=%d z=%d yTop=%d yBottom=%d" %
          (room0['info_x'], room0['info_z'], room0['yTop'], room0['yBottom']))
    print("room0 counts     : verts=%d quads=%d tris=%d" %
          (len(room0['verts']), len(room0['quads']), len(room0['tris'])))
    for i in range(1, roomsCount):
        skip_room(r)

    # ---- rest of readDataArrays to reach objectTextures ----
    r.seek(r.u32()*2)                      # floors
    r.seek(r.u32()*2)                      # meshData
    r.seek(r.u32()*4)                      # meshOffsets
    animsCount = r.u32(); r.seek(animsCount*32)
    r.seek(r.u32()*6)                      # states
    r.seek(r.u32()*8)                      # ranges
    r.seek(r.u32()*2)                      # commands
    r.seek(r.u32()*4)                      # nodesData
    r.seek(r.u32()*2)                      # frameData
    modelsCount = r.u32(); r.seek(modelsCount*20)
    r.seek(r.u32()*32)                     # staticMeshes

    objCount = r.u32()
    print("objectTexturesCount: %d" % objCount)
    if objCount < 1 or objCount > 8000:
        print("!! objCount looks wrong -> header/skip mismatch"); sys.exit(1)

    objtex = []
    for i in range(objCount):
        x0 = r.u8(); y0 = r.u8()
        clut = r.u16()
        x1 = r.u8(); y1 = r.u8()
        tile = r.u16() & 0x3FFF
        x2 = r.u8(); y2 = r.u8()
        r.u16()                            # unknown2
        x3 = r.u8(); y3 = r.u8()
        attribute = r.u16()
        objtex.append(dict(tile=tile, clut=clut, attr=attribute,
                           uv=[(x0,y0),(x1,y1),(x2,y2),(x3,y3)]))

    # ---------------------------------------------------------------- decoders
    def clut_rgb555(clut_idx, nib):
        off = cluts_off + clut_idx*CLUT_BYTES + nib*2
        v = data[off] | (data[off+1] << 8)
        return (v & 31, (v >> 5) & 31, (v >> 10) & 31, (v >> 15) & 1)

    def tile_nibble(tile_idx, x, y):
        off = tiles_off + tile_idx*TILE_PAGE_BYTES + (y*256 + x)//2
        byte = data[off]
        return (byte >> 4) if (x & 1) else (byte & 0x0F)   # a:low b:high

    def jag16(r5, g5, b5):
        return ((r5 & 31) << 11) | ((b5 & 31) << 6) | ((g5 & 31) << 1)

    # ---- which object-textures does ROOM 0 use? ----
    used_tex = set()
    for q in room0['quads']: used_tex.add(q['tex'])
    for t in room0['tris']:  used_tex.add(t['tex'])
    used_tex = sorted(i for i in used_tex if i < objCount)
    print("room0 uses %d unique object-textures" % len(used_tex))

    # ---- dedup used object-textures by (tile,clut,bbox); decode each once ----
    tiles_out = []        # unique atlas tiles: dict(w,h,umin,vmin,px)
    grp_of_key = {}
    grp_of_tex = {}       # objtex index -> (group, umin, vmin)
    for ti in used_tex:
        o = objtex[ti]
        us = [p[0] for p in o['uv']]; vs = [p[1] for p in o['uv']]
        umin, umax, vmin, vmax = min(us), max(us), min(vs), max(vs)
        w = umax - umin + 1; h = vmax - vmin + 1
        key = (o['tile'], o['clut'], umin, vmin, w, h)
        g = grp_of_key.get(key)
        if g is None:
            px = []
            for y in range(vmin, vmax+1):
                for x in range(umin, umax+1):
                    px.append(clut_rgb555(o['clut'], tile_nibble(o['tile'], x, y)))
            g = len(tiles_out)
            grp_of_key[key] = g
            tiles_out.append(dict(w=w, h=h, umin=umin, vmin=vmin, px=px))
        grp_of_tex[ti] = (g, umin, vmin)
    print("unique atlas tiles after dedup: %d" % len(tiles_out))
    _area = sum(t['w']*t['h'] for t in tiles_out)
    _dims = Counter((t['w'],t['h']) for t in tiles_out)
    print("tile total px area: %d  maxW=%d maxH=%d  dims=%s" %
          (_area, max(t['w'] for t in tiles_out), max(t['h'] for t in tiles_out),
           dict(sorted(_dims.items()))))

    # ---- build ROOM-0 palette (only room 0's colours) in Jaguar RGB16 ----
    hist = Counter()
    for t in tiles_out:
        for (r5,g5,b5,a) in t['px']:
            hist[jag16(r5,g5,b5)] += 1
    uniq = list(hist.keys())
    print("unique RGB16 colours in room0 textures: %d" % len(uniq))

    def unpack16(c): return ((c >> 11) & 31, (c >> 6) & 31, c & 63)

    if len(uniq) <= 256:
        palette = sorted(uniq)
        idx_of  = {c:i for i,c in enumerate(palette)}
        quantized = False
    else:
        pts = [(unpack16(c), cnt, c) for c,cnt in hist.items()]
        buckets = [pts]
        while len(buckets) < 256:
            best_i, best_range, best_axis = -1, -1, 0
            for bi,bk in enumerate(buckets):
                if len(bk) < 2: continue
                for ax in range(3):
                    lo = min(p[0][ax] for p in bk); hi = max(p[0][ax] for p in bk)
                    if hi-lo > best_range: best_range, best_i, best_axis = hi-lo, bi, ax
            if best_i < 0: break
            bk = buckets.pop(best_i); bk.sort(key=lambda p: p[0][best_axis])
            total = sum(p[1] for p in bk); acc = 0; sp = 1
            for k,p in enumerate(bk):
                acc += p[1]
                if acc*2 >= total: sp = max(1, min(len(bk)-1, k+1)); break
            buckets.append(bk[:sp]); buckets.append(bk[sp:])
        palette = []; idx_of = {}
        for i,bk in enumerate(buckets):
            wsum = sum(p[1] for p in bk) or 1
            r5 = round(sum(p[0][0]*p[1] for p in bk)/wsum)
            b5 = round(sum(p[0][1]*p[1] for p in bk)/wsum)
            g6 = round(sum(p[0][2]*p[1] for p in bk)/wsum)
            rep = ((r5 & 31) << 11) | ((b5 & 31) << 6) | (g6 & 63)
            palette.append(rep)
            for p in bk: idx_of[p[2]] = i
        quantized = True
    while len(palette) < 256: palette.append(0)
    print("palette entries: %d  (quantized=%s)" % (len(palette), quantized))

    # ---- shelf-pack tiles into a compact width-256 atlas ----
    ATLAS_W = 256
    order = sorted(range(len(tiles_out)), key=lambda i: -tiles_out[i]['h'])
    shelf_x = shelf_y = shelf_h = atlas_h = 0
    pos = [None]*len(tiles_out)
    for i in order:
        w = tiles_out[i]['w']; h = tiles_out[i]['h']
        if shelf_x + w > ATLAS_W:
            shelf_y += shelf_h; shelf_x = 0; shelf_h = 0
        pos[i] = (shelf_x, shelf_y)
        shelf_x += w; shelf_h = max(shelf_h, h)
        atlas_h = max(atlas_h, shelf_y + h)
    atlas_h = (atlas_h + 1) & ~1
    print("atlas: %dx%d  (%d tiles, %d bytes)" %
          (ATLAS_W, atlas_h, len(tiles_out), ATLAS_W*atlas_h))

    atlas = bytearray(ATLAS_W * atlas_h)
    for i,t in enumerate(tiles_out):
        ax, ay = pos[i]; w = t['w']; h = t['h']
        for yy in range(h):
            row = (ay+yy)*ATLAS_W + ax; src = yy*w
            for xx in range(w):
                r5,g5,b5,a = t['px'][src+xx]
                atlas[row+xx] = idx_of[jag16(r5,g5,b5)]

    # ---- Lara flat-shade swatch strip appended to the atlas bottom ----
    # A tiny ramp of solid-index cells so the SAME gpu_geotex kernel can draw
    # Lara: each Lara face's UVs point at the cell matching its shade (0..N-1).
    # Palette indices LARA_IDX_BASE.. are unused by the room (only 0..46 used).
    LARA_IDX_BASE = 242            # 242..245 (clear of room 0..46 and blink 254/255)
    LARA_N        = 4              # dark..light shade cells
    LARA_CELL     = 8             # px per cell (solid), 8-wide blocks along a row
    lara_tones = [(9,6,3),(13,9,5),(18,13,8),(24,19,13)]  # (r5,g5,b5) tan ramp
    for k in range(LARA_N):
        r5,g5,b5 = lara_tones[k]
        palette[LARA_IDX_BASE + k] = jag16(r5,g5,b5)
    LARA_SW_Y = atlas_h
    atlas += bytearray(ATLAS_W * LARA_CELL)     # one 8-row strip
    for k in range(LARA_N):
        cx = k * LARA_CELL
        for yy in range(LARA_CELL):
            row = (LARA_SW_Y+yy)*ATLAS_W + cx
            for xx in range(LARA_CELL):
                atlas[row+xx] = LARA_IDX_BASE + k
    atlas_h += LARA_CELL
    print("lara swatch: %d cells idx %d.. at atlas y=%d (atlas now %dx%d)" %
          (LARA_N, LARA_IDX_BASE, LARA_SW_Y, ATLAS_W, atlas_h))

    # ---- per-face UVs into the small atlas ----
    #   objtex uv corner k -> atlas (ax + uv.x - umin, ay + uv.y - vmin)
    def face_atlas_uv(tex, swap23):
        g, umin, vmin = grp_of_tex[tex]
        ax, ay = pos[g]
        uv = objtex[tex]['uv']
        out = [(ax + (u - umin), ay + (v - vmin)) for (u,v) in uv]
        if swap23:
            out[2], out[3] = out[3], out[2]
        # clamp to atlas (defensive; should already be in range)
        return [(min(ATLAS_W-1, max(0, u)), min(atlas_h-1, max(0, v))) for (u,v) in out]

    # ---------------------------------------------------------------- write bins
    os.makedirs(OUTDIR, exist_ok=True)
    os.makedirs(PREVDIR, exist_ok=True)

    with open(os.path.join(OUTDIR, "room0_atlas.bin"), "wb") as f:
        f.write(atlas)
    with open(os.path.join(OUTDIR, "room0_pal.bin"), "wb") as f:
        for c in palette: f.write(struct.pack(">H", c))

    verts = room0['verts']; quads = room0['quads']; tris = room0['tris']
    # offsets stored in PACKED (>>8) units to fit s16, matching the reference
    # tr2jag.cpp convention: worldX = localX + (offX<<8), worldZ likewise.
    # Y is ABSOLUTE world Y in the vertex data already, so offY=0.
    def sar8(v):  # arithmetic >>8 (exact here: info.x/z are multiples of 256)
        return v >> 8 if v >= 0 else -((-v) >> 8)
    offX = sar8(room0['info_x']); offY = 0; offZ = sar8(room0['info_z'])

    # int16 fit check on offsets + verts
    def fits_s16(v): return -32768 <= v <= 32767
    off_fit = fits_s16(offX) and fits_s16(offZ)
    vrange = [ (min(v[0] for v in verts), max(v[0] for v in verts)),
               (min(v[1] for v in verts), max(v[1] for v in verts)),
               (min(v[2] for v in verts), max(v[2] for v in verts)) ]
    print("offsets s16 fit  : %s  (offX=%d offZ=%d packed; world=local+(off<<8))" %
          (off_fit, offX, offZ))
    print("vert local X/Y/Z : %s" % (vrange,))

    with open(os.path.join(OUTDIR, "room0_tex.bin"), "wb") as f:
        # header (16 bytes): u16 vcount,qcount,tcount,atlasW,atlasH ; s16 offX,offY,offZ
        f.write(struct.pack(">HHHHH", len(verts), len(quads), len(tris), ATLAS_W, atlas_h))
        f.write(struct.pack(">hhh", offX, offY, offZ))
        # verts: s16 x,y,z ; u16 shade  (shade: higher=brighter, inverted from PSX darkness)
        for (x,y,z,light) in verts:
            # PSX lighting: 0(bright)..0x1FFF(dark). brightness8 = 255-(light>>5)
            b8 = 255 - (light >> 5)
            if b8 < 0: b8 = 0
            if b8 > 255: b8 = 255
            f.write(struct.pack(">hhhH", x, y, z, b8))
        # quads: u16 v[4] ; 4 * (u16 u, u16 v) into the small atlas
        for q in quads:
            f.write(struct.pack(">HHHH", *q['v']))
            for (u,v) in face_atlas_uv(q['tex'], q['swapped']):
                f.write(struct.pack(">HH", u, v))
        # tris: u16 v[3] ; 3 * (u16 u, u16 v)  (first 3 corners pair v0,v1,v2)
        for t in tris:
            f.write(struct.pack(">HHH", *t['v']))
            for (u,v) in face_atlas_uv(t['tex'], False)[:3]:
                f.write(struct.pack(">HH", u, v))

    room0_tex_bytes = 16 + len(verts)*8 + len(quads)*(8+16) + len(tris)*(6+12)

    # ---- room-0 SECTORS for collision (floor-follow + wall block) ----
    # room0_sect.bin (BIG-ENDIAN):
    #   u16 xSectors, zSectors
    #   s32 info_x, info_z            (world units; sector = [(wx-info_x)>>10]*zSectors + [(wz-info_z)>>10])
    #   xSectors*zSectors * { s16 floorY, s16 ceilY }
    #     floorY = floor*256 (world Y, +down); WALL (floor==-127) -> floorY = 0x7FFF (32767)
    #     ceilY  = ceiling*256;                WALL (ceiling==-127) -> ceilY = -32768
    WALL = -127
    xS = room0['xSectors']; zS = room0['zSectors']; sect = room0['sectors']
    fys = []
    with open(os.path.join(OUTDIR, "room0_sect.bin"), "wb") as f:
        f.write(struct.pack(">HH", xS, zS))
        f.write(struct.pack(">ii", room0['info_x'], room0['info_z']))
        for s in sect:
            if s['floor'] == WALL:
                fy = 0x7FFF
            else:
                fy = s['floor'] * 256; fys.append(fy)
            cy = -32768 if s['ceiling'] == WALL else s['ceiling'] * 256
            f.write(struct.pack(">hh", fy, cy))
    nwall = sum(1 for s in sect if s['floor'] == WALL)
    print("sectors          : %dx%d = %d  (%d wall, %d floor)" %
          (xS, zS, len(sect), nwall, len(fys)))
    if fys:
        print("floor world-Y    : min=%d max=%d  (room verts Y: %s)" %
              (min(fys), max(fys), vrange[1]))

    with open(os.path.join(OUTDIR, "room0_tex.h"), "w") as f:
        f.write("// generated by tr2jag_room.py - PSX TR1 LEVEL1 ROOM 0 (Caves start)\n")
        f.write("// geometry + per-face UVs into a compact room-0 8bpp atlas.\n//\n")
        f.write("#define ROOM0_ATLAS_W   %d\n" % ATLAS_W)
        f.write("#define ROOM0_ATLAS_H   %d\n" % atlas_h)
        f.write("#define ROOM0_VCOUNT    %d\n" % len(verts))
        f.write("#define ROOM0_QCOUNT    %d\n" % len(quads))
        f.write("#define ROOM0_TCOUNT    %d\n" % len(tris))
        f.write("#define ROOM0_PAL_COUNT 256\n")
        f.write("// Lara flat-shade swatch cells (solid palette-index blocks in the atlas):\n")
        f.write("#define ROOM0_LARA_IDX_BASE %d\n" % LARA_IDX_BASE)
        f.write("#define ROOM0_LARA_N        %d\n" % LARA_N)
        f.write("#define ROOM0_LARA_CELL     %d\n" % LARA_CELL)
        f.write("#define ROOM0_LARA_SW_Y     %d\n" % LARA_SW_Y)
        f.write("//   Lara face shade s (0..ROOM0_LARA_N-1) -> UV cell corners:\n")
        f.write("//     u in [s*CELL+1 .. s*CELL+CELL-2], v in [SW_Y+1 .. SW_Y+CELL-2]\n//\n")
        f.write("// room0_atlas.bin : ROOM0_ATLAS_W*ROOM0_ATLAS_H bytes, 8bpp palette index, row-major\n")
        f.write("// room0_pal.bin   : 256 * u16 big-endian, Jaguar RGB16 = R5<<11|B5<<6|G6\n//\n")
        f.write("// room0_tex.bin  (ALL BIG-ENDIAN):\n")
        f.write("//   HEADER (16 bytes):\n")
        f.write("//     u16 vcount, qcount, tcount, atlasW, atlasH\n")
        f.write("//     s16 offX, offY, offZ   (PACKED >>8 units; offY=0)\n")
        f.write("//   VERTS  (vcount * 8 bytes):  s16 x, y, z ; u16 shade (0..255, higher=brighter)\n")
        f.write("//   QUADS  (qcount * 24 bytes): u16 v0,v1,v2,v3 ; u16 u0,v0,u1,v1,u2,v2,u3,v3\n")
        f.write("//   TRIS   (tcount * 18 bytes): u16 v0,v1,v2    ; u16 u0,v0,u1,v1,u2,v2\n")
        f.write("//   UVs index the compact room-0 atlas (0..atlasW-1, 0..atlasH-1).\n")
        f.write("//   NOTE: UVs are u16 (not u8): atlasH=%d exceeds 255, so a single\n" % atlas_h)
        f.write("//         shared u8-addressable atlas is impossible (tile area 148480 px).\n")
        f.write("//   x,z are room-local world units; y is ABSOLUTE world Y.\n")
        f.write("//   worldX = x + (offX<<8);  worldZ = z + (offZ<<8);  worldY = y  (offY=0)\n")
        f.write("//   Vertex/UV pairing already PSX-swapped (quad v2<->v3) to quad-list order.\n")

    # ---------------------------------------------------------------- preview PNG
    def write_png(path, w, h, rgb):
        def chunk(tag, d):
            return (struct.pack(">I", len(d)) + tag + d +
                    struct.pack(">I", zlib.crc32(tag + d) & 0xffffffff))
        raw = bytearray()
        for y in range(h):
            raw.append(0); raw += rgb[y*w*3:(y+1)*w*3]
        png = b"\x89PNG\r\n\x1a\n"
        png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        png += chunk(b"IEND", b"")
        open(path, "wb").write(png)

    def rgb16_to_888(c):
        r5 = (c >> 11) & 31; b5 = (c >> 6) & 31; g6 = c & 63
        return bytes([(r5<<3)|(r5>>2), (g6<<2)|(g6>>4), (b5<<3)|(b5>>2)])

    prgb = [rgb16_to_888(c) for c in palette]
    buf = bytearray()
    for idx in atlas: buf += prgb[idx]
    write_png(os.path.join(PREVDIR, "room0_atlas.png"), ATLAS_W, atlas_h, bytes(buf))

    # ---------------------------------------------------------------- report
    print("\n=== RESULTS (room 0) ===")
    print("verts / quads / tris : %d / %d / %d" % (len(verts), len(quads), len(tris)))
    print("unique object-textures used : %d  -> %d atlas tiles" % (len(used_tex), len(tiles_out)))
    print("compact atlas        : %dx%d = %d bytes (%.1f KB, limit 256KB)" %
          (ATLAS_W, atlas_h, len(atlas), len(atlas)/1024.0))
    print("palette              : %d unique -> 256 (quantized=%s)" % (len(uniq), quantized))
    print("room0_tex.bin        : %d bytes" % room0_tex_bytes)
    print("outputs              : room0_atlas.bin room0_pal.bin room0_tex.bin room0_tex.h")
    print("preview              : %s/room0_atlas.png" % PREVDIR)

if __name__ == "__main__":
    main()
