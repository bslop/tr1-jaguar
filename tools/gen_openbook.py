#!/usr/bin/env python3
# gen_openbook.py - author the OPEN passport spread directly (user refs
# 11-42-22 PSX vs 12-34-24). Model 71 is a nearly-closed FAN: its pages
# face inward and no orientation shows a flat two-page spread (offline
# sorted-painter sweep peaked at ~3% page pixels). The PSX plays an
# opening animation our pose bake doesn't capture, so - like the
# polaroid (gen_polaroid.py) - the spread is composed here instead.
#
# PAGE-FLIP (user 2026-08-06, PSX ref pf_085..125): the PSX rows are
# SPREADS - "Start Game" shows visa+photo, "Go Back" shows plain
# pages - and switching rows turns a leaf about the spine. Verts 8..11
# are that LEAF; main.c animates them in MODEL space each frame (the
# blob is copied to RAM and re-baked per frame, so writing the four
# verts is enough). Leaf front = the photo page (seen lying RIGHT,
# row 0); leaf back = the plain page (seen lying LEFT, row 1). The
# base pages carry what each row reveals underneath: visa on the
# left, forms on the right. The painter sort orders leaf-over-base.
# Runs AFTER:
#   PASS_TYPE=71 PASS_PREFIX=pass2 PASS_DOUBLE=1 tr2jag_title.py
# and REWRITES pass2_geom.bin only.
import os, struct

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
gb = open(os.path.join(D, "pass2_geom.bin"), "rb").read()
aw, ah = struct.unpack(">HH", gb[6:10])

# paint a solid dark-grey spine strip into a spare atlas corner (the
# cover art's dark texels are mottled red-black and read as BACKGROUND
# showing through the gutter on silicon)
atl = bytearray(open(os.path.join(D, "pass2_atlas.bin"), "rb").read())
for y in range(288, 294):
    for x in range(240, 252):
        atl[y*aw + x] = 246              # grey ramp (49,49,49)
open(os.path.join(D, "pass2_atlas.bin"), "wb").write(bytes(atl))

# page tiles in the extracted atlas (72x96 each):
#   (0,0)   cover crest      (72,0)  visa/stamps     (144,0) forms
#   (0,96)  plain pale       (72,96) text page       (144,96) forms table
#   (0,192) photo ID page
VISA  = (72, 0, 143, 95)        # base LEFT  (under everything, row 0 look)
FORMS = (144, 96, 215, 191)     # base RIGHT (revealed when the leaf lifts)
PHOTO = (0, 192, 71, 287)       # leaf FRONT (lying right = row 0 spread)
PLAIN = (0, 96, 71, 191)        # leaf BACK  (lying left  = row 1 spread)
SPINE = (241, 289, 249, 292)    # clean dark-grey strip painted above

HW, HH, VZ, SW = 94, 60, 8, 4   # half-width, half-height, V depth, spine half-w
V = [
    (-HW, -HH, -VZ), (-SW, -HH,  VZ), ( SW, -HH,  VZ), ( HW, -HH, -VZ),
    (-HW,  HH, -VZ), (-SW,  HH,  VZ), ( SW,  HH,  VZ), ( HW,  HH, -VZ),
    # LEAF at rest (lying on the RIGHT page, 2 units toward the camera
    # so the sort always places it over the base): 8=inner-top,
    # 9=outer-top, 10=outer-bot, 11=inner-bot. main.c rewrites these.
    ( SW, -HH,  VZ-2), ( HW, -HH, -VZ-2),
    ( HW,  HH, -VZ-2), ( SW,  HH,  VZ-2),
]
def uvq(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
quads = [
    ((0, 1, 5, 4), uvq(*VISA)),         # base left page
    ((2, 3, 7, 6), uvq(*FORMS)),        # base right page
    ((1, 2, 6, 5), uvq(*SPINE)),        # spine gutter
    ((8, 9, 10, 11), uvq(*PHOTO)),      # leaf front (photo left edge at spine)
    # leaf back: seen when lying LEFT - outer edge is then screen-left,
    # so the tile's u=0 goes on the OUTER verts (9/10)
    ((9, 8, 11, 10), [(PLAIN[0], PLAIN[1]), (PLAIN[2], PLAIN[1]),
                      (PLAIN[2], PLAIN[3]), (PLAIN[0], PLAIN[3])]),
]
b = bytearray()
b += struct.pack(">HHHHH", len(V), len(quads), 0, aw, ah)
b += struct.pack(">hhh", 0, 0, 0)
for (x, y, z) in V: b += struct.pack(">hhhH", x, y, z, 255)
for vi, uv in quads:
    b += struct.pack(">HHHH", *vi)
    for (u, v) in uv: b += struct.pack(">HH", u, v)
while len(b) & 7: b += b'\0'
open(os.path.join(D, "pass2_geom.bin"), "wb").write(bytes(b))
print("open book composed: %dv %dq (leaf verts 8-11), atlas %dx%d" % (len(V), len(quads), aw, ah))
