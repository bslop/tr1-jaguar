#!/usr/bin/env python3
# tr2jag_tex.py
#
# HOST tool: extracts the TEXTURES from a PlayStation Tomb Raider 1 level (.PSX)
# and converts them to an 8-bit INDEXED (paletted) atlas the Atari Jaguar
# renderer can use.
#
# WHY THIS LAYOUT (all proven against OpenLara's src/, which is the authoritative
# PSX reader):
#   loadTR1_PSX()      format.h:3349  - top level read order for a TR1 .PSX
#   readDataArrays()   format.h:3714  - rooms, meshData, anims, ... then objectTex
#   readRoom() PSX     format.h:5112  - room block is (info 16B)+(u32 size)+size*2 bytes
#                                        then portals/sectors/lights/meshes trailers
#   readObjectTex()PSX format.h:6127  - 16-byte per-face {x0y0,clut,x1y1,tile,x2y2,
#                                        unknown,x3y3,attribute}
#   Tile4              utils.h:1523    - 256x256 4-bit indices, ColorIndex4{a:4,b:4}
#                                        pixel(x,y): b=(y*256+x)/2; nib=(x&1)?hi:lo
#   CLUT / ColorCLUT   utils.h:1508    - 16 entries; ColorCLUT{r:5,g:5,b:5,a:1}
#                                        i.e. 16-bit LE: R=bits0-4 G=5-9 B=10-14 STP=15
#   texel decode       format.h:6492   - clut.color[(x&1)? src.index[i].b : .a]
#
# OUTPUTS (big-endian):
#   src/platform/jaguar/textures.bin : 8bpp indexed atlas (1 byte/texel, row-major)
#   src/platform/jaguar/texpal.bin   : 256 x u16 RGB16 palette (Jaguar framebuffer fmt)
#   src/platform/jaguar/texmap.bin   : object-texture -> atlas rect + local UVs table
#   src/platform/jaguar/textures.h   : C header describing dims/counts/format
#   tr1_psx/tex_preview/*.png        : decoded previews for eyeballing
#
# Jaguar RGB16 = R5<<11 | B5<<6 | G6  (blue in the MIDDLE), big-endian.

import struct, sys, os, zlib

LEVEL   = __import__("os").environ.get("TRLEVEL", __import__("os").path.join(__import__("os").path.dirname(__import__("os").path.dirname(__import__("os").path.abspath(__file__))), "assets/extracted/PSXDATA/LEVEL1.PSX"))
OUTDIR  = __import__("os").path.dirname(__import__("os").path.dirname(__import__("os").path.abspath(__file__)))
PREVDIR = "/tmp/tr1_tex_preview"

TILE_PAGE_BYTES = 256*256//2   # 32768  (Tile4)
CLUT_BYTES      = 16*2         # 32     (16 x ColorCLUT u16)
NUM_TILES       = 13
NUM_CLUTS       = 1024

# ---------------------------------------------------------------- byte readers
class R:
    def __init__(self, data): self.d = data; self.p = 0
    def setpos(self, p): self.p = p
    def seek(self, n): self.p += n
    def u8(self):  v = self.d[self.p]; self.p += 1; return v
    def u16(self): v = struct.unpack_from("<H", self.d, self.p)[0]; self.p += 2; return v
    def s16(self): v = struct.unpack_from("<h", self.d, self.p)[0]; self.p += 2; return v
    def u32(self): v = struct.unpack_from("<I", self.d, self.p)[0]; self.p += 4; return v

def skip_room(r):
    # replicate readRoom() TR1_PSX (format.h:5112) far enough to reach next room
    r.seek(16)                     # Room::Info  (4 x int32)
    size = r.u32()                 # Data.size  (u32, number of u16 words)
    start = r.p
    r.setpos(start + size*2)       # skip whole geometry block (format.h:5404)
    r.seek(r.u16()*32)             # portals    (u16 count, 32B each)
    z = r.u16(); x = r.u16()       # sectors: zSectors,xSectors
    r.seek(z*x*8)                  # 8B each
    r.seek(2)                      # ambient (u16)  (TR1: no ambient2/lightMode)
    r.seek(r.u16()*20)             # lights   (u16 count, 20B each PSX)
    r.seek(r.u16()*20)             # meshes   (u16 count, 20B each PSX)
    r.seek(4)                      # alternateRoom(s16) + flags(u16)

def main():
    data = open(LEVEL, "rb").read()
    print("input      : %s (%d bytes)" % (LEVEL, len(data)))
    print("first u32  : 0x%08X" % struct.unpack_from("<I", data, 0)[0])

    r = R(data)
    # ---- magic handling (format.h:3180). version already = VER_TR1_PSX (by file
    #      size, gameflow.h:538). read u32 magic; it is NOT in the exclusion list
    #      so a SECOND u32 is read -> pos = 8 on entry to loadTR1_PSX.
    r.u32(); r.u32()                       # pos = 8
    # loadTR1_PSX: seek(8); read offsetTexTiles (format.h:3352-3353)
    r.seek(8)                              # pos = 16
    offsetTexTiles = r.u32()
    texTiles = offsetTexTiles + 8          # format.h:3372  setPos(offsetTexTiles + 8)
    print("offsetTexTiles: %d  -> tiles start @ %d" % (offsetTexTiles, texTiles))

    # ---- tiles4 (13) + cluts (1024) ------------------------------------------
    tiles_off = texTiles
    cluts_off = tiles_off + NUM_TILES * TILE_PAGE_BYTES
    r.setpos(cluts_off + NUM_CLUTS * CLUT_BYTES)   # pos just after cluts
    print("tiles @ %d  cluts @ %d  (tiles=%d cluts=%d)" %
          (tiles_off, cluts_off, NUM_TILES, NUM_CLUTS))

    # ---- readDataArrays (format.h:3714) --------------------------------------
    r.seek(4)                              # skip unused u32 (TR1, not TR3PSX)
    roomsCount = r.u16()
    print("roomsCount : %d" % roomsCount)
    for i in range(roomsCount):
        skip_room(r)
    r.seek(r.u32()*2)                      # floors  (FloorData u16)  count is i32
    r.seek(r.u32()*2)                      # meshData (u16)
    r.seek(r.u32()*4)                      # meshOffsets (i32)
    animsCount = r.u32(); r.seek(animsCount*32)          # anims 32B each (TR1)
    r.seek(r.u32()*6)                      # states  (AnimState 6B)
    r.seek(r.u32()*8)                      # ranges  (AnimRange 8B)
    r.seek(r.u32()*2)                      # commands (i16)
    r.seek(r.u32()*4)                      # nodesData (u32)
    r.seek(r.u32()*2)                      # frameData (u16)
    modelsCount = r.u32(); r.seek(modelsCount*20)        # models 20B each (PSX)
    r.seek(r.u32()*32)                     # staticMeshes 32B each

    # ---- readObjectTex (format.h:6162 / 6127) --------------------------------
    objCount = r.u32()
    print("objectTexturesCount: %d" % objCount)
    if objCount < 1 or objCount > 8000:
        print("!! objCount looks wrong -> header/skip mismatch"); sys.exit(1)

    objtex = []
    for i in range(objCount):
        base = r.p
        x0 = r.u8(); y0 = r.u8()
        clut = r.u16()
        x1 = r.u8(); y1 = r.u8()
        tile = r.u16() & 0x3FFF            # tile:14
        x2 = r.u8(); y2 = r.u8()
        r.u16()                            # unknown2
        x3 = r.u8(); y3 = r.u8()
        attribute = r.u16()
        objtex.append(dict(tile=tile, clut=clut, attr=attribute,
                           uv=[(x0,y0),(x1,y1),(x2,y2),(x3,y3)]))

    # sanity
    bad = [o for o in objtex if o['tile'] >= NUM_TILES or o['clut'] >= NUM_CLUTS]
    print("objtex tile range OK: %s   clut range OK: %s" %
          (all(o['tile'] < NUM_TILES for o in objtex),
           all(o['clut'] < NUM_CLUTS for o in objtex)))
    if bad:
        print("!! %d object textures out of range (first: %s)" % (len(bad), bad[0]))

    # ---------------------------------------------------------------- decoders
    def clut_rgb555(clut_idx, nib):
        # ColorCLUT u16 LE: R=0-4 G=5-9 B=10-14 STP=15
        off = cluts_off + clut_idx*CLUT_BYTES + nib*2
        v = data[off] | (data[off+1] << 8)
        return (v & 31, (v >> 5) & 31, (v >> 10) & 31, (v >> 15) & 1)

    def tile_nibble(tile_idx, x, y):
        off = tiles_off + tile_idx*TILE_PAGE_BYTES + (y*256 + x)//2
        byte = data[off]
        return (byte >> 4) if (x & 1) else (byte & 0x0F)   # a:low b:high

    def jag16(r5, g5, b5):
        # Jaguar RGB16 = R5<<11 | B5<<6 | G6 ; G6 = g5<<1 (low bit 0)
        return ((r5 & 31) << 11) | ((b5 & 31) << 6) | ((g5 & 31) << 1)

    # ---- per object texture: bbox; DEDUP identical (tile,clut,bbox) tiles -----
    #      1102 faces reuse the same page regions heavily, so extract each unique
    #      (tile,clut,umin,vmin,w,h) once and point every face at that atlas tile.
    tiles_out = []          # unique tiles: dict(w,h,umin,vmin,px)
    grp_of_key = {}         # key -> group index
    for o in objtex:
        us = [p[0] for p in o['uv']]; vs = [p[1] for p in o['uv']]
        umin, umax, vmin, vmax = min(us), max(us), min(vs), max(vs)
        w = umax - umin + 1; h = vmax - vmin + 1
        o['umin'] = umin; o['vmin'] = vmin
        key = (o['tile'], o['clut'], umin, vmin, w, h)
        g = grp_of_key.get(key)
        if g is None:
            px = []
            for y in range(vmin, vmax+1):
                for x in range(umin, umax+1):
                    nib = tile_nibble(o['tile'], x, y)
                    px.append(clut_rgb555(o['clut'], nib))
            g = len(tiles_out)
            grp_of_key[key] = g
            tiles_out.append(dict(w=w, h=h, umin=umin, vmin=vmin, px=px))
        o['grp'] = g
    print("unique atlas tiles after dedup: %d (from %d object textures)" %
          (len(tiles_out), len(objtex)))

    # ---- build global 256-colour palette in Jaguar RGB16 ---------------------
    from collections import Counter
    hist = Counter()
    for t in tiles_out:
        for (r5,g5,b5,a) in t['px']:
            hist[jag16(r5,g5,b5)] += 1
    uniq = list(hist.keys())
    print("unique RGB16 colours in used textures: %d" % len(uniq))

    def unpack16(c):  # -> (r5,b5,g6)
        return ((c >> 11) & 31, (c >> 6) & 31, c & 63)

    if len(uniq) <= 256:
        palette = sorted(uniq)
        idx_of  = {c:i for i,c in enumerate(palette)}
        quantized = False
    else:
        # median cut in (r5,b5,g6) space weighted by count
        pts = [(unpack16(c), cnt, c) for c,cnt in hist.items()]
        buckets = [pts]
        while len(buckets) < 256:
            # pick bucket with largest weighted extent to split
            best_i, best_range, best_axis = -1, -1, 0
            for bi,bk in enumerate(buckets):
                if len(bk) < 2: continue
                for ax in range(3):
                    lo = min(p[0][ax] for p in bk); hi = max(p[0][ax] for p in bk)
                    if hi-lo > best_range:
                        best_range, best_i, best_axis = hi-lo, bi, ax
            if best_i < 0: break
            bk = buckets.pop(best_i)
            bk.sort(key=lambda p: p[0][best_axis])
            # split at weighted median
            total = sum(p[1] for p in bk); acc = 0; sp = 1
            for k,p in enumerate(bk):
                acc += p[1]
                if acc*2 >= total: sp = max(1, min(len(bk)-1, k+1)); break
            buckets.append(bk[:sp]); buckets.append(bk[sp:])
        palette = []
        idx_of = {}
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

    # ---- shelf-pack tiles into a width-256 atlas -----------------------------
    ATLAS_W = 256
    # sort by height desc for tighter shelves
    order = sorted(range(len(tiles_out)), key=lambda i: -tiles_out[i]['h'])
    shelf_x = 0; shelf_y = 0; shelf_h = 0; atlas_h = 0
    pos = [None]*len(tiles_out)
    for i in order:
        w = tiles_out[i]['w']; h = tiles_out[i]['h']
        if shelf_x + w > ATLAS_W:
            shelf_y += shelf_h; shelf_x = 0; shelf_h = 0
        pos[i] = (shelf_x, shelf_y)
        shelf_x += w
        shelf_h = max(shelf_h, h)
        atlas_h = max(atlas_h, shelf_y + h)
    atlas_h = (atlas_h + 1) & ~1
    print("atlas: %dx%d  (%d tiles)" % (ATLAS_W, atlas_h, len(tiles_out)))

    atlas = bytearray(ATLAS_W * atlas_h)   # 8bpp indices, default 0
    for i,t in enumerate(tiles_out):
        ax, ay = pos[i]; w = t['w']; h = t['h']
        for yy in range(h):
            row = (ay+yy)*ATLAS_W + ax
            src = yy*w
            for xx in range(w):
                r5,g5,b5,a = t['px'][src+xx]
                atlas[row+xx] = idx_of[jag16(r5,g5,b5)]

    # ---------------------------------------------------------------- write bins
    os.makedirs(OUTDIR, exist_ok=True)
    os.makedirs(PREVDIR, exist_ok=True)

    with open(os.path.join(OUTDIR, "textures.bin"), "wb") as f:
        f.write(atlas)                                   # 1 byte/texel, row-major
    with open(os.path.join(OUTDIR, "texpal.bin"), "wb") as f:
        for c in palette: f.write(struct.pack(">H", c)) # big-endian u16

    # texmap.bin : header {u16 objCount,u16 atlasW,u16 atlasH} then per objtex
    #   record (one per original object texture, indexed by face texture id).
    with open(os.path.join(OUTDIR, "texmap.bin"), "wb") as f:
        f.write(struct.pack(">HHH", len(objtex), ATLAS_W, atlas_h))
        for o in objtex:
            g = o['grp']; t = tiles_out[g]
            ax, ay = pos[g]
            f.write(struct.pack(">HHHH", ax, ay, t['w'], t['h']))
            for (u,v) in o['uv']:
                f.write(struct.pack(">BB", u - o['umin'], v - o['vmin']))
            f.write(struct.pack(">HHH", o['tile'], o['clut'], o['attr']))

    with open(os.path.join(OUTDIR, "textures.h"), "w") as f:
        f.write("// generated by tr2jag_tex.py - PSX TR1 LEVEL1 textures (8bpp indexed)\n")
        f.write("#define TEX_ATLAS_W   %d\n" % ATLAS_W)
        f.write("#define TEX_ATLAS_H   %d\n" % atlas_h)
        f.write("#define TEX_TILE_COUNT %d\n" % len(tiles_out))
        f.write("#define TEX_PAL_COUNT 256\n")
        f.write("// textures.bin : TEX_ATLAS_W*TEX_ATLAS_H bytes, 8bpp palette index, row-major\n")
        f.write("// texpal.bin   : 256 * u16 big-endian, Jaguar RGB16 = R5<<11|B5<<6|G6\n")
        f.write("// texmap.bin   : u16 count,atlasW,atlasH; then per objtex:\n")
        f.write("//   u16 atlasX,atlasY,w,h; u8 u0,v0,u1,v1,u2,v2,u3,v3 (tile-local);\n")
        f.write("//   u16 origTile,origClut,attribute\n")

    # ---------------------------------------------------------------- previews
    def write_png(path, w, h, rgb):   # rgb = bytes length w*h*3
        def chunk(tag, d):
            return (struct.pack(">I", len(d)) + tag + d +
                    struct.pack(">I", zlib.crc32(tag + d) & 0xffffffff))
        raw = bytearray()
        for y in range(h):
            raw.append(0)
            raw += rgb[y*w*3:(y+1)*w*3]
        png = b"\x89PNG\r\n\x1a\n"
        png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        png += chunk(b"IEND", b"")
        open(path, "wb").write(png)

    def rgb16_to_rgb888(c):
        r5 = (c >> 11) & 31; b5 = (c >> 6) & 31; g6 = c & 63
        return (bytes([(r5<<3)|(r5>>2)]) + bytes([(g6<<2)|(g6>>4)]) + bytes([(b5<<3)|(b5>>2)]))

    # (1) atlas preview via the FINAL 256-colour palette (verifies decode + quant)
    prgb = [rgb16_to_rgb888(c) for c in palette]
    buf = bytearray()
    for idx in atlas: buf += prgb[idx]
    write_png(os.path.join(PREVDIR, "atlas_indexed.png"), ATLAS_W, atlas_h, bytes(buf))

    # (2) per-page previews: paint each object texture into its source page using
    #     its OWN clut (direct RGB, no quantization) - verifies CLUT/bit order.
    pages = {}
    for i,o in enumerate(objtex):
        pg = o['tile']
        img = pages.setdefault(pg, bytearray(256*256*3))
        t = tiles_out[o['grp']]
        for yy in range(t['h']):
            for xx in range(t['w']):
                r5,g5,b5,a = t['px'][yy*t['w']+xx]
                gx = t['umin']+xx; gy = t['vmin']+yy
                off = (gy*256+gx)*3
                img[off]   = (r5<<3)|(r5>>2)
                img[off+1] = (g5<<3)|(g5>>2)
                img[off+2] = (b5<<3)|(b5>>2)
    for pg,img in sorted(pages.items()):
        write_png(os.path.join(PREVDIR, "page_%02d.png" % pg), 256, 256, bytes(img))
    print("pages with textures: %s" % sorted(pages.keys()))

    # ---------------------------------------------------------------- report
    print("\n=== RESULTS ===")
    print("object textures : %d" % len(objtex))
    print("atlas           : %dx%d (%d bytes) -> textures.bin" %
          (ATLAS_W, atlas_h, len(atlas)))
    print("palette         : %d unique -> 256 entries (quantized=%s) -> texpal.bin (512B)" %
          (len(uniq), quantized))
    print("unique tiles    : %d -> textures.bin atlas" % len(tiles_out))
    print("texmap records  : %d -> texmap.bin (one per object texture)" % len(objtex))
    print("previews        : %s" % PREVDIR)

if __name__ == "__main__":
    main()
