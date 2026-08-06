#!/usr/bin/env python3
# gen_polaroid.py - turn the Lara's-Home ring item into a real POLAROID.
#
# Runs AFTER tr2jag_title.py's photo pass (which leaves photo_geom.bin as a
# thin 8-vert box whose every face maps the raw mansion picture, and
# photo_atlas.bin holding that picture at [0..67]x[0..75]). This composes a
# proper instant-photo atlas IN INDEX SPACE (no requantization loss):
#   FRONT: white frame (border 6px, classic wide 20px bottom) around the
#          picture, scaled to the inner window
#   BACK:  plain grey backing panel with a darker rim
#   EDGES: white strip
# and re-emits the SAME closed box with per-face UVs, single-sided (a closed
# manifold needs no PASS_DOUBLE - backface cull resolves it, like the PS1 pad).
#
# Palette: title_pal idx 0 is the warm near-white; the 245..254 grey-ramp tail
# (gen_titlebg.py) supplies the backing greys. Runs order:
#   gen_titlebg.py -> tr2jag_title.py (photo) -> gen_polaroid.py
import os, struct

OUT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
gb = open(os.path.join(OUT, "photo_geom.bin"), "rb").read()
nv, nq, nt, aw, ah = struct.unpack(">HHHHH", gb[:10])
atl = bytearray(open(os.path.join(OUT, "photo_atlas.bin"), "rb").read())
pal = struct.unpack(">256H", open(os.path.join(OUT, "title_pal.bin"), "rb").read())
def dec(c): return (((c>>11)&31)*255//31, ((c>>1)&31)*255//31, ((c>>6)&31)*255//31)
prgb = [dec(c) for c in pal]
def nearest(r, g, b):
    return min(range(256), key=lambda i:(r-prgb[i][0])**2+(g-prgb[i][1])**2+(b-prgb[i][2])**2)
WHITE = nearest(238, 230, 222)
GREY  = nearest(150, 150, 150)
DGREY = nearest(90, 90, 90)

# locate the mansion picture in the extractor's atlas via the RAW blob's
# biggest-face UV rect (the repack moved it and the old hardcoded (0,0)
# 68x76 blitted a wrong region - black square, 2026-08-05)
def _faces(gb, nv, nq, nt):
    o = 16 + nv*8
    fs = []
    for _ in range(nq):
        vi = struct.unpack(">HHHH", gb[o:o+8]); o += 8
        uv = [struct.unpack(">HH", gb[o+i*4:o+i*4+4]) for i in range(4)]; o += 16
        fs.append((vi, uv))
    return fs
_fs = _faces(gb, nv, nq, nt)
def _uvarea(uv):
    us = [u for u, v in uv]; vs = [v for u, v in uv]
    return (max(us)-min(us))*(max(vs)-min(vs)), min(us), min(vs), max(us), max(vs)
_best = max((_uvarea(uv) for vi, uv in _fs))
PICX, PICY = _best[1], _best[2]
PICW, PICH = _best[3]-_best[1]+1, _best[4]-_best[2]+1
print("picture located at (%d,%d) %dx%d" % (PICX, PICY, PICW, PICH))
# polaroid proportions: 84x104 with 6px border, 20px bottom margin
FW, FH = 84, 104
BL, BT, BR, BB = 6, 6, 6, 20
IW, IH = FW-BL-BR, FH-BT-BB         # 72x78 inner window

NAW = 256
NAH = FH + 8                        # front + back side by side, edge strip below
na = bytearray([WHITE]) * (NAW*NAH)
na = bytearray(NAW*NAH)
for i in range(NAW*NAH): na[i] = WHITE
# FRONT at (0,0): frame is already white; blit the picture scaled into it
for y in range(IH):
    sy = y*PICH//IH
    for x in range(IW):
        sx = x*PICW//IW
        na[(BT+y)*NAW + BL+x] = atl[(PICY+sy)*aw + PICX+sx]
# BACK at (FW+4, 0): BLACK - the PS1 reference's between-front spin
# phases (ph_010..ph_041, 22-22-21) show a solid black card back; the
# white EDGE faces supply the rim light exactly like the ref's bottom
# edge. (First read called it white - that was front-glare frames.)
BLACK = nearest(10, 10, 12)
BX = FW+4
for y in range(FH):
    for x in range(FW):
        na[y*NAW + BX+x] = BLACK
# EDGE strip at (BX+FW+4, 0) 8x8 white (already white)
EX = BX+FW+4

# ---- geometry: same thin box, per-face UVs, SINGLE-sided ----
# POLA_SCALE: the model-73 card (62x76 units) reads as a featureless white
# chip at ring distance next to the 156-unit pad - grow it to polaroid weight.
SCALE = float(os.environ.get("POLA_SCALE", "1.5"))
o = 16
V = [struct.unpack(">hhhH", gb[o+i*8:o+i*8+8])[:3] for i in range(nv)]
V = [(int(x*SCALE), int(y*SCALE), int(z*2)) for (x, y, z) in V]
# faces of the original model 73 box (first 6 = original winding):
#   front(z-1): (2,6,5,1)  back(z+1): (7,3,0,4)  and 4 edges
def uvq(x0, y0, x1, y1):
    return [(x0,y0),(x1,y0),(x1,y1),(x0,y1)]
quads = [
    # verts (l-t, r-t, r-b, l-b as seen)      uv rect
    ((2,6,5,1), uvq(0,0, FW-1,FH-1)),          # FRONT: the polaroid
    ((7,3,0,4), uvq(BX,0, BX+FW-1,FH-1)),      # BACK: the backing
    ((0,3,2,1), uvq(EX,0, EX+7,7)),            # left edge
    ((0,1,5,4), uvq(EX,0, EX+7,7)),            # top edge
    ((2,3,7,6), uvq(EX,0, EX+7,7)),            # bottom edge
    ((4,5,6,7), uvq(EX,0, EX+7,7)),            # right edge
]
b = bytearray()
b += struct.pack(">HHHHH", nv, len(quads), 0, NAW, NAH)
b += struct.pack(">hhh", 0, 0, 0)
for (x, y, z) in V: b += struct.pack(">hhhH", x, y, z, 255)
for vi, uv in quads:
    b += struct.pack(">HHHH", *vi)
    for (u, v) in uv: b += struct.pack(">HH", u, v)
while len(b) & 7: b += b'\0'
open(os.path.join(OUT, "photo_geom.bin"), "wb").write(b)
open(os.path.join(OUT, "photo_atlas.bin"), "wb").write(bytes(na))
print("polaroid: %dv %dq atlas %dx%d blob %dB (white=%d grey=%d/%d)"
      % (nv, len(quads), NAW, NAH, len(b), WHITE, GREY, DGREY))
