# How the level assets are generated (recovered the hard way, 2026-07-29)

`tools/build_cof.sh` is the source of truth, NOT the extractor's defaults.
Running `python3 tools/tr2jag_multiroom.py` with no environment silently
produces a DIFFERENT, much smaller level (5 rooms, no texture scaling) and
overwrites every mrt_* file including the generated headers. The .bin files are
gitignored (copyrighted game data), so there is no git to restore them from.

## Level 1 (the Caves) - the shipping asset set
```
TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=6144 LARA_MINAREA=0 \
FACE_PLANES=1 RAMP_PAL=1 STATICS=1 LARA_WINDFIX=0 \
TRLEVEL=<PSXDATA>/LEVEL1.PSX TRPREFIX=mrt python3 tools/tr2jag_multiroom.py
```
`build_cof.sh` lists only the first four; **FACE_PLANES / RAMP_PAL / STATICS
are also required** and are not in that script:
  - `FACE_PLANES=1` writes the 12-byte plane prefix every face record needs -
    `STAGEDIET` in the kernel reads it to backface-cull before staging.
  - `RAMP_PAL=1` bakes ONE full-bright tile copy instead of four shade levels
    (the runtime shade pass darkens); without it the atlas is 481280 instead
    of 280576.
  - `STATICS=1` bakes stalactites/plants into the room geometry. Also needs
    `make STATICS=1` (the kernel vertex cache goes 512 -> 768).
  - **`LARA_WINDFIX=0`** - the extractor DEFAULTS to 1, and 1 reverses the
    winding of ~190 of Lara's faces. Reversed winding flips the face normal,
    the runtime backface cull then drops those faces, and she renders
    SEE-THROUGH with no backpack and holes in her legs. The tell in the data is
    UV corners swapped in pairs (774 differing bytes in mrt_lara.bin, all of
    them corner order). WINDFIX=0 takes that to 8 bytes.
**`SUBDIV_MAX` must be >= 6144**, not the 3072 in build_cof.sh: at 3072 the
extractor still splits ~104 faces and mrt_geom.bin comes out 1248 bytes larger
than the shipped copy. At 6144 it is **byte-identical** to the shipped
mrt_geom.bin, and 12288 gives the same bytes again - i.e. the level is already
fully un-subdivided by 6144.

Verification: this recipe reproduces **mrt_atlas.bin AND mrt_geom.bin
byte-identically** and every other blob at its exact shipped size
(mrt_lara.bin is within 8 low bytes of rounding).

## Lara's Home (the Mansion) - same flags, different level
```
... TRLEVEL=<PSXDATA>/GYM.PSX TRPREFIX=gym python3 tools/tr2jag_multiroom.py
```

## Rule
Back the mrt_*/gym_* files up before running any extractor, and byte-compare
after. They are not in git and cannot be recovered from it.


## Lara FACE DIET - the 8.57 fps arm, RECIPE RECORDED (2026-08-02)

The -45% arm measured last night was recorded only as a RESULT, never as a
recipe, and was unreproducible the next day - the exact failure this file
exists to prevent.  Here it is, written down:

```
TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=6144 LARA_MINAREA=1600 \
FACE_PLANES=1 RAMP_PAL=1 STATICS=1 LARA_WINDFIX=0 \
LARA_HULLDIRS=3 LARA_HEADLOD=-1 \
TRLEVEL=<PSXDATA>/LEVEL1.PSX TRPREFIX=mrt python3 tools/tr2jag_multiroom.py
```
=> Lara 425 -> **221 faces (-48%)**.  Everything else byte-identical
(mrt_geom.bin 347296, atlas 274.0 KB, 38 rooms).

**LARA_MINAREA IS MODEL-SPACE AREA, SO THE SCALE IS HUNDREDS, NOT UNITS.**
6/12/24 all measured 425 faces (no effect) and looked like the knob was
dead.  The curve: 200->344  400->337  800->323  **1600->221**.

- `LARA_HULLDIRS=3` and `LARA_HEADLOD=-1` are DELIBERATE.  HULLDIRS is what
  wrecked her hair in the original arm and it only buys 425->409 anyway, so
  it is all damage and no win.
- The HEAD IS ALREADY SPARED: `LARA_KEEPMESH` defaults to `14` (the head),
  because its hair is tiny tris and dieting them opens skin-tone gaps.
- MINAREA damages LIMBS, not hair: it drops small faces on arms/legs and
  leaves boundary edges (holes).  That is the visible cost of this arm.
- Side effect: meshes 10 and 13 (the holsters, 6 faces each) are dropped
  ENTIRELY, which removes the mis-textured gold blocks at her hips.


## ☠️ LARA_COLRAMP=1 IS REQUIRED (2026-08-02) — the butt/limb blotches

Her flat-COLOURED faces (thighs, hands, hips) rendered as grey-green or BLACK
blotches.  Root cause was already written in the extractor and simply not
enabled: `LARA_COLRAMP` defaults to **0**.

Without it, each flat tone gets a reserved palette slot in the band 246..253.
That band is **NOT ramp-aligned**, so the runtime shade pass - which ORs a
darkness step `k` onto the palette index - walks straight into **254/255, the
UI black/white**.  Hence black blotches, and gold/grey ones at other shade
levels.  Mesh 1 and mesh 4 (the thighs) carry SIX such faces each, which is
exactly the size and symmetry of the blotches on her buttocks.

With `LARA_COLRAMP=1` every tone is pointed at the nearest RAMP BASE instead:
    6->ramp23  8->ramp0   10->ramp18  13->ramp23
    14->ramp23 21->ramp19 25->ramp8   34->ramp22
so `k` steps down a real ramp like every other surface in the game.

Requires `RAMP_PAL=1` (already in the recipe).  Add `LARA_COLRAMP=1` to the
Level 1 command line above.

## Title ring items (tr2jag_title.py)

```
PASS_DOUBLE=1 python3 tools/tr2jag_title.py                       # pass (71 open->closed pair)
PASS_TYPE=71 PASS_PREFIX=pass2 python3 tools/tr2jag_title.py
PASS_PREFIX=photo PASS_TYPE=73 PASS_FORCE_TEX=265 PASS_DOUBLE=1 python3 tools/tr2jag_title.py
PASS_PREFIX=sound PASS_TYPE=96 PASS_POSES=1 PASS_DOUBLE=1 python3 tools/tr2jag_title.py
PASS_PREFIX=detail PASS_TYPE=95 PASS_POSES=1 PASS_DOUBLE=1 PASS_GAIN=1.55 python3 tools/tr2jag_title.py
python3 tools/gen_polaroid.py
```

The sunglasses (detail, type 95) need PASS_GAIN=1.55 — the PS1's lit lens
reads (124,32,20) but the unbiased quantize picks a near-black entry and the
lenses vanish on a CRT. The gain also drags the FRAME swatch onto the neutral
grey ramp (246), which the PS1's frame is not — patch it back to the warm
dark entry after extraction:

```
python3 - <<'PYEOF'
b = bytearray(open("detail_atlas.bin","rb").read())
for i, v in enumerate(b):
    if v == 246: b[i] = 119        # grey -> warm dark (PS1 frame tone)
open("detail_atlas.bin","wb").write(bytes(b))
PYEOF
```
