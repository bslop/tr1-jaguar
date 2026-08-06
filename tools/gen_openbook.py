#!/usr/bin/env python3
# gen_openbook.py - author the OPEN passport spread directly (user refs
# 11-42-22 PSX vs 12-34-24). Model 71 is a nearly-closed FAN: its pages
# face inward and no orientation shows a flat two-page spread (offline
# sorted-painter sweep peaked at ~3% page pixels). The PSX plays an
# opening animation our pose bake doesn't capture, so - like the
# polaroid (gen_polaroid.py) - the spread is composed here instead:
# two page quads in a shallow V toward the camera + a dark spine strip,
# textured from the tiles the pass2 extraction already put in
# pass2_atlas.bin. Runs AFTER:
#   PASS_TYPE=71 PASS_PREFIX=pass2 PASS_DOUBLE=1 tr2jag_title.py
# and REWRITES pass2_geom.bin only (the atlas is used as-is).
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
# The PSX spread reads as PLAIN PALE pages with light marks: left =
# the plain page, right = the text page (faint lines), like the ref.
LPAGE = (0, 96, 71, 191)
RPAGE = (144, 96, 215, 191)
SPINE = (241, 289, 249, 292)    # clean dark-grey strip painted below

HW, HH, VZ, SW = 94, 60, 8, 4   # half-width, half-height, V depth, spine half-w
V = [
    (-HW, -HH, -VZ), (-SW, -HH,  VZ), ( SW, -HH,  VZ), ( HW, -HH, -VZ),
    (-HW,  HH, -VZ), (-SW,  HH,  VZ), ( SW,  HH,  VZ), ( HW,  HH, -VZ),
]
def uvq(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
quads = [
    ((0, 1, 5, 4), uvq(*LPAGE)),        # left page
    ((2, 3, 7, 6), uvq(*RPAGE)),        # right page
    ((1, 2, 6, 5), uvq(*SPINE)),        # spine gutter
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
print("open book composed: %dv %dq, atlas %dx%d untouched" % (len(V), len(quads), aw, ah))
