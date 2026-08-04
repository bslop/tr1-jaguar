# RESTORE POINT — 2026-08-01, "head fixed + floors fixed"

The last state verified good on silicon by the user. **Everything below is
reproducible; nothing depends on a file that only exists in `build/`.**

## What this state is

- Lara's head is solid: hull restored to `LARA_HULLDIRS=3` + `LARA_HEADLOD=-1`
  (425 faces), and `HEADLEAK` now darkens flat-COLOUR head faces too.
- Floors are clean: `SPANSHADE=1 SHADEEXCL=1` plus the face-end blit wait, so
  the grey/transparent trapezoid-overshoot slabs are gone and the black
  flickering tiles they brought with them are gone too.
- Measured: **7.55 fps** (mean gap 7.95 fields), i.e. the floor fix is free.

## Git

    commit  a1aefbb   ROOMAUDIT: extractor-side face reduction is a DEAD END
    branch  title-passport-open

Source is fully in git. **The assets and ROMs are NOT** — `.gitignore` excludes
`*.bin` and `*.cof`, so a bad extractor run cannot be undone with git and
`git status` will look clean while the level is wrong.

## The exact build

    make MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 STAGEDIET=1 \
         PIPELINE=1 PIPESTAGE=2 HOPDIAL=1 HOPBOOT=1 XCULL=1 BEXIT=1 \
         ROWDIET=1 STATICS=1 BANKDIET=1 LOWRES=1 FLIPASM=1 NOSOUND=1 \
         DIVZGUARD=1 MOVESET=1 LPLANES=1 SPANSHADE=1 SHADEEXCL=1 PADTEXT=544

Produces `build/openlara.cof`, 1645664 bytes,
**md5 `66eefbbc0a265919e9ee2a92dab8cc83`** — verified byte-identical to the ROM
the user played. If your rebuild does not match that md5, the assets are wrong;
do not flash it, restore them first (below).

☠️ `PADTEXT=544` is load-bearing at this size. 272 and 816 are both black.
☠️ `NOSOUND=1` is in this recipe. Dropping it re-rolls the A10 lottery.
☠️ `rm -rf build` before any A/B — make tracks timestamps, not flags.

## The assets

Regenerate with (ASSET_BUILD.md, all eight variables, `SUBDIV_MAX=6144`):

    TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=6144 LARA_MINAREA=0 FACE_PLANES=1 \
    RAMP_PAL=1 STATICS=1 LARA_WINDFIX=0 \
    TRLEVEL=<PSXDATA>/LEVEL1.PSX TRPREFIX=mrt python3 tools/tr2jag_multiroom.py

Sanity: `head -8 mrt.h` must say **38 rooms** and **`MRT_FACE_PLANES 1`**;
`mrt_atlas.bin` must be **280576** bytes; `mrt_lara.bin` **8568**;
`mrt_lplanes.h` must total **151 quads + 274 tris = 425 faces**.

## Restore, if an experiment goes wrong

A byte-exact copy of every generated asset AND the verified ROM lives OUTSIDE
the repo (so a `git clean` cannot take it):

    /home/jvilla/Documents/Git/jag_openlara/RESTORE_2026-08-01_headfix_spanshade/

    GOOD_ROM.cof   the exact ROM the user played
    MD5SUMS.txt    checksums for every file here
    mrt_*          every generated asset + header

To get back:

    cd .../src/platform/jaguar
    git checkout a1aefbb -- .              # source
    cp $RP/mrt_* .                         # assets (NOT in git)
    cd $RP && md5sum -c MD5SUMS.txt        # prove the copy is intact

To just re-flash the known-good ROM without rebuilding anything:

    jcp -r ; sleep 3 ; jcp $RP/GOOD_ROM.cof

★ Uploads go to **RAM**, never `jcp -f`. Success prints
`Jag accepted start request at $00004000`. Full-screen green = no ROM running.
