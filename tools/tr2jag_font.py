#!/usr/bin/env python3
# tr2jag_font.py — extract TR1 gold-font glyph sprites (LEVEL1.PSX sprite
# table) into a Jaguar blob: per-glyph 8bpp pixels (0 = transparent,
# 1..15 = mini-palette index) + a 16-colour RGB16 palette.
#
# font_load.bin (big-endian):
#   u16 count, u16 palOff
#   count * { u8 ch, u8 w, u8 h, u8 yoff, u32 pixOff }   yoff = top vs baseline
#   pixels... (w*h bytes each)
#   at palOff: 16 * u16 RGB16 (Jaguar '(r<<11)|(b<<6)|(g<<1)' format)
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tr2jag_multiroom as T

LEVEL=os.environ.get("TRLEVEL", os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets/extracted/PSXDATA/LEVEL1.PSX"))
OUT  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# DEFAULT IS THE FULL SET (2026-08-01). font_load.bin is a generated artifact
# and is NOT tracked, so whatever this default produces is what a fresh checkout
# ends up shipping. It used to be "LOADING...", which is why the game had a
# 10-glyph font and the menu work looked font-blocked for weeks. Anything that
# needs a narrower blob should pass FONT_CHARS explicitly.
FULLSET = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
           "abcdefghijklmnopqrstuvwxyz"
           "0123456789.,!?'-")
WANT = os.environ.get("FONT_CHARS", FULLSET)

raw=open(LEVEL,'rb').read()
r=T.R(raw); r.u32(); r.u32(); r.seek(8)
off=r.u32(); tiles_off=off+8; cluts_off=tiles_off+T.NUM_TILES*T.TILE_PAGE_BYTES
r.setpos(cluts_off+T.NUM_CLUTS*T.CLUT_BYTES)
T.read_all_rooms(r); r.seek(r.u32()*2)
mb=r.u32(); r.seek(mb*2); mn=r.u32(); r.seek(mn*4)
an=r.u32(); r.seek(an*32); r.seek(r.u32()*6); r.seek(r.u32()*8); r.seek(r.u32()*2)
r.seek(r.u32()*4); r.seek(r.u32()*2)
mc=r.u32(); r.seek(mc*20); r.seek(r.u32()*32)
objCount=r.u32(); r.seek(objCount*16)
sprCount=r.u32(); pSpr=r.p

# The glyph run: 'A' is the sprite right after the three gold sparkles.
# Empirically (contact sheet): A..Z start at index 37, then a..z, 0..9, punct.
BASE_A = int(os.environ.get("FONT_BASE_A", "37"))
# PUNCTUATION (2026-08-01): identified by rendering sprites 96..119 and reading
# the contact sheet, not by guessing at TR's table order. Indices 99..108 are:
#     99 .    100 ,    101 !    102 ?    103 [
#    104 "    105 /    106 ^    107 '    108 -
# The APOSTROPHE is 107 - needed for "Lara's Home" on the title ring. 99 and 100
# are both small 8x8 marks and are easy to confuse; the period sits low, the
# comma carries a tail.
PUNCT={'.':99, ',':100, '!':101, '?':102, '[':103,
       '"':104, '/':105, '^':106, "'":107, '-':108}
def chidx(c):
    if 'A'<=c<='Z': return BASE_A + (ord(c)-65)
    if 'a'<=c<='z': return BASE_A + 26 + (ord(c)-97)
    if '0'<=c<='9': return BASE_A + 52 + (ord(c)-48)
    if c in PUNCT:  return PUNCT[c]
    raise SystemExit("unmapped char %r (add it to PUNCT after identifying its "
                     "sprite index from a contact sheet)" % c)

def tile_nibble(ti,x,y):
    b=raw[tiles_off+ti*T.TILE_PAGE_BYTES+(y*256+x)//2]
    return (b&0xF) if (x&1)==0 else (b>>4)

glyphs=[]; palclut=None
for c in WANT:
    i=chidx(c); o=pSpr+i*16
    l,t2,rr,b2,clut,tile,u0,v0,u1,v1 = struct.unpack_from("<hhhhHHBBBB",raw,o)
    w=u1-u0+1; h=v1-v0+1
    px=bytes(tile_nibble(tile&0x3FFF,u0+x,v0+y) for y in range(h) for x in range(w))
    # VERTICAL PLACEMENT (2026-08-01). The sprite record's `t2` is the glyph's
    # top relative to the text baseline, and it is -16 for EVERY glyph (-15 for
    # 'y'). Descenders are not shifted down, they are simply TALLER: p, g and y
    # measure 16x24 where a, e, o measure 16x16, hanging 8 px below the
    # baseline. So glyphs must be TOP-aligned. Bottom-aligning them - which is
    # what `yoff = 16 - h` does - lifts every descender 8 px, and "Step Left"
    # renders with a superscript p. Store the real offset instead of recomputing
    # a wrong one at draw time; it fits the record's spare pad byte.
    yoff=t2+16
    glyphs.append((c,w,h,yoff,px))
    if palclut is None: palclut=clut
    print("glyph %r idx=%d %dx%d yoff=%d clut=%d" % (c,i,w,h,yoff,clut))

# palette: PSX 16-colour clut -> Jaguar RGB16
pal=[]
for k in range(16):
    c,=struct.unpack_from("<H",raw,cluts_off+palclut*T.CLUT_BYTES+k*2)
    r5=c&31; g5=(c>>5)&31; b5=(c>>10)&31
    pal.append(((r5&31)<<11)|((b5&31)<<6)|((g5&31)<<1) if c else 0)

hdr=bytearray(); pix=bytearray()
body_off=4+len(glyphs)*8
for (c,w,h,yoff,px) in glyphs:
    hdr+=struct.pack(">BBBBI", ord(c), w, h, yoff & 0xFF, body_off+len(pix))
    pix+=px
palOff=body_off+len(pix)
blob=struct.pack(">HH", len(glyphs), palOff)+bytes(hdr)+bytes(pix)
blob+=b''.join(struct.pack(">H",p) for p in pal)
open(os.path.join(OUT,'font_load.bin'),'wb').write(blob)
print("font_load.bin %d bytes, %d glyphs" % (len(blob), len(glyphs)))
