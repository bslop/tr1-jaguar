#!/usr/bin/env bash
# build_cof.sh <disc> <outdir>
#
# The whole pipeline in one shot: extract a user's Tomb Raider PSX disc, convert
# every asset to Jaguar form, and build the GameDrive payload. Same script the
# Docker image runs as its entrypoint, so a local run and a container run are
# identical.  Reproducible: given the same disc it produces the same OPENLARA.COF.
#
#   tools/build_cof.sh  /path/to/TombRaider.(iso|bin|cue|chd|7z)  ./out
#
set -euo pipefail

DISC="${1:?usage: build_cof.sh <disc> <outdir>}"
OUT="${2:?usage: build_cof.sh <disc> <outdir>}"

HERE="$(cd "$(dirname "$0")/.." && pwd)"     # the jaguar/ tree
cd "$HERE"
RMAC="${RMAC:-$HOME/jaguar-tools/bin/rmac}"
# LOWRES (2026-07-25): render 320x120 and let the OP's hardware vertical
# scaler display it at 240, instead of HALFRES's Blitter line-double.  That
# deletes 2 Blitter passes / 76800 B per frame.  MATCHED silicon A/B, same
# session and route: 6.00 -> 7.50 fps (+25%), frames whole.  The path was
# unusable until the flip grew a Blitter COMPLETION barrier (video.c) — see
# the LOWRES BARRIER notes; do not switch back without reading them.
# ── QUALITY: pretty or playable ──────────────────────────────────────────────
# ONE knob, two values, because they are mutually exclusive - you cannot ship
# both resolutions in one ROM, and two independent PRETTY=/PLAYABLE= flags
# would need an "both set" rule nobody would remember.
#
#   QUALITY=pretty     (default)  120 render lines. Full vertical resolution.
#   QUALITY=playable              80 render lines through the OP 3.0x scaler.
#                                 Same full screen and field of view, coarser
#                                 vertically, but it holds the rock detail that
#                                 60 lines smears into bands - chosen at the
#                                 pad after seeing 120/96/80/60 on a TV.
#
# They differ ONLY in VRESN - identical feature set, identical assets - so a
# bug in one is a bug in both.  Measured on silicon 2026-08-11 with the demo
# feature set (see PLAY_BUILD.md for the full ladder and both columns):
#
#      120 lines  6.40 fps        80 lines  7.15 fps  (+11.7%)
#
QUALITY="${QUALITY:-pretty}"
case "$QUALITY" in
    pretty)   QUALITY_FLAGS="" ;;
    playable) QUALITY_FLAGS="VRESN=80" ;;
    *) echo "error: QUALITY must be 'pretty' or 'playable' (got '$QUALITY')" >&2
       exit 2 ;;
esac

# ☠️ THE SHIPPING FLAG SET.  This used to read
#   MULTIROOM=1 LOWRES=1 CFLAGS_EXTRA=-DJERRYPOSE
# which had drifted years behind the game: no entities, no doors, no enemies,
# no kernel stack, no sound.  A container build produced something that was not
# the game anyone was playing.  Keep this in step with PLAY_BUILD.md's CURRENT
# recipe - that file is the authority and records why each flag is here.
# ☠️ PADTEXT is deliberately ABSENT: it is an A10 boot-lottery roll tied to one
# exact code layout, and the layout a container produces is not this tree's.
# If a container ROM boots black, roll PADTEXT (see project_a10_reproducible).
# ☠️ AUTOSTART is deliberately ABSENT: with it the ROM drops straight into the
# caves, and this build is meant to come up on the TITLE RING like the real
# game. There is no MENU flag - "menu" IS the absence of AUTOSTART.
# ☠️ `make FOO=0` still DEFINES FOO in this Makefile, so a feature is turned off
# by OMITTING it, never by setting it to 0.
BUILD_FLAGS="${BUILD_FLAGS:-MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 \
STAGEDIET=1 PIPELINE=1 PIPESTAGE=2 HOPDIAL=1 HOPBOOT=1 XCULL=1 BEXIT=1 \
ROWDIET=1 STATICS=1 BANKDIET=1 LOWRES=1 FLIPASM=1 DIVZGUARD=1 MOVESET=1 \
SPANSHADE=1 SHADEEXCL=1 TIMESTEP=1 ANIMRATE=1 OPDBL=1 LPLANES=1 \
VCBIG=1 ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 ENEMYTEX=1 BLOBCACHE=1 \
JCENT=1 JOVL=1 SECTLONG=1 INLINEMUL=1 OFFHOIST=1 VPACK=1 NOPCLIP=1 \
TRAPFLOOR=1 GUNS=1 GYMSD=1 BOOTVID=1 JVFASTKICK=1 PROBE_AHEAD=256}"

# ☠️ PROBE_AHEAD IS NOT OPTIONAL. The default is WALK_SPEED*2 = 94 units against
# a 1024-unit sector, and collision holds Lara ~100 units off a wall - so the
# forward probe never leaves the cell she stands in, finds her own floor, and
# EVERY climb refuses (standing climb, running vault and the airborne grab all
# feed off that one probe). Verified on silicon 2026-08-16: at 94 the on-screen
# verdict reads "flat ground ahead" while she faces an obvious ledge; at 256 a
# 512 and a 768 ledge both read VAULT and she climbs. Leaving this out of the
# container would have shipped a demo where nothing can be climbed.

say() { printf '\n\033[1;36m==>\033[0m %s\n' "$*"; }

# ── 1. disc -> a private extraction dir (never touches the repo's assets) ─────
ASSETS="$(mktemp -d)"; trap 'rm -rf "$ASSETS"' EXIT
say "Reading disc: $DISC"
WANT_FMV="${WANT_FMV:-CORELOGO.FMV,CAFE.FMV,SNOW.FMV,INTRO.STR}" \
    python3 tools/extract_disc.py "$DISC" "$ASSETS"
PSX="$ASSETS/PSXDATA"

# ── 2. convert every asset (order matters: gen_titlebg writes title_pal.bin,
#       which tr2jag_title.py then maps the passport/photo textures onto) ──────
# ☠️☠️ THE FULL RECIPE, NOT THE FIRST FOUR FLAGS. ASSET_BUILD.md spells this
# out and this script had only ever passed TEXSCALE/MRT_ROOMS/SUBDIV_MAX/
# LARA_MINAREA, which is why container ROMs STREAKED: without RAMP_PAL the
# atlas is baked with four shade levels per tile (481280 B) instead of one
# full-bright copy (280576 B) that the runtime shade pass darkens - the kernel
# samples a layout that is not there. The others matter just as much:
#   FACE_PLANES=1  the 12-byte plane prefix STAGEDIET backface-culls from
#   STATICS=1      stalactites/plants baked into the rooms (kernel: STATICS=1)
#   LARA_WINDFIX=0 the extractor DEFAULTS to 1, which reverses ~190 of Lara's
#                  faces and renders her SEE-THROUGH
#   SUBDIV_MAX=6144 at 3072 it still splits ~104 faces; 6144 is byte-identical
#                  to the shipped mrt_geom.bin
MRTENV="TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=6144 LARA_MINAREA=0 \
FACE_PLANES=1 RAMP_PAL=1 STATICS=1 LARA_WINDFIX=0"

say "Extracting levels + Lara (Caves)"
env $MRTENV TRLEVEL="$PSX/LEVEL1.PSX" TRPREFIX=mrt python3 tools/tr2jag_multiroom.py

# ☠️☠️☠️ COLLISION PATCH. mrt_sect.bin is baked by the extractor and is WRONG
# until this runs: TR1's GetHeight DESCENDS into the room below at a seam and
# uses its floor, while the bake stores the seam height as solid ground - so
# Lara runs off a ledge and keeps running on air - and some floor==-127
# doorcells come out classified as WALL, which SEALS OFF parts of the level.
# 403 such sectors in the Caves. The file is gitignored, so it must be
# re-patched after EVERY regeneration; the container regenerates it every run
# and never did this (user 2026-08-15: "an issue with both wall boundaries as
# well as being able to access the area up above for the rest of the level").
say "Patching collision boundaries (phantom seam floors + doorcells)"
TRLEVEL="$PSX/LEVEL1.PSX" python3 tools/mrt_boundary_audit.py --patch 2>&1 \
    | tail -4 || echo "   note: boundary patch failed"

# Atlas PATCH passes. Each re-reads the shipping palette+atlas, APPENDS its rows
# and exits, touching nothing else - so they must run after the base extraction
# and before the build. Without them the doors, medikits, pistols and wolf fur
# sample rows that do not exist.
say "Atlas patches (doors, pickups, pistols, enemy skins)"
for patch in MRT_DOORPATCH MRT_PICKPATCH MRT_GUNPATCH; do
    env $MRTENV $patch=1 TRLEVEL="$PSX/LEVEL1.PSX" TRPREFIX=mrt \
        python3 tools/tr2jag_multiroom.py >/dev/null || echo "   note: $patch pass failed"
    echo "   $patch -> atlas $(stat -c%s mrt_atlas.bin) B"
done
env $MRTENV MRT_ENEMYTEX=1 MRT_ENEMYTEX_STEP=2 TRLEVEL="$PSX/LEVEL1.PSX" TRPREFIX=mrt \
    python3 tools/tr2jag_multiroom.py >/dev/null || echo "   note: MRT_ENEMYTEX pass failed"
echo "   MRT_ENEMYTEX -> atlas $(stat -c%s mrt_atlas.bin) B"
env $MRTENV MRT_GUNONLY=1 TRLEVEL="$PSX/LEVEL1.PSX" TRPREFIX=mrt \
    python3 tools/tr2jag_multiroom.py >/dev/null || echo "   note: MRT_GUNONLY pass failed"
# The pistol ARM animations (LARA_PISTOLS' arm joints). ☠️ Disc-derived, so it
# is generated here rather than committed - this repo ships no game data.
say "Pistol arm animations"
env $MRTENV MRT_GUNANIM=1 TRLEVEL="$PSX/LEVEL1.PSX" TRPREFIX=mrt python3 tools/tr2jag_multiroom.py

# Lara's Home is only extracted when the ROM actually carries it. GYMSD leaves
# it out (mrt_data.S stubs the gym_* incbins), so extracting it is pure cost -
# and under the full RAMP_PAL recipe the mansion's palette overflows 256 entries
# and the extractor dies, taking the whole build with it. That is a real
# extractor bug, but it is in a level this ROM does not ship.
# ☠️ main.c includes gym_spawn.h unconditionally, so the mansion must be
# extracted even under GYMSD - the ROM does not LINK its data, but the build
# still needs its headers. Under GYMSD we drop RAMP_PAL for this level only:
# with it the mansion's palette overflows 256 entries and the extractor dies
# (a real bug, in a level this ROM does not ship), and since none of its atlas
# is linked, how it was shaded cannot matter.
case "$BUILD_FLAGS" in
  *GYMSD*)
     say "Lara's Home (headers only - GYMSD leaves its data out of the ROM)"
     # minimal flags on purpose: RAMP_PAL and STATICS each push the mansion's
     # palette past 256 entries and the extractor raises IndexError. This pass
     # exists only to emit gym_spawn.h/gym_lara.h, and none of its data is
     # linked under GYMSD, so the shading flags are irrelevant here.
     env TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=6144 LARA_MINAREA=0 LARA_WINDFIX=0 \
         TRLEVEL="$PSX/GYM.PSX" TRPREFIX=gym python3 tools/tr2jag_multiroom.py ;;
  *) say "Extracting Lara's Home (Mansion)"
     env $MRTENV TRLEVEL="$PSX/GYM.PSX" TRPREFIX=gym python3 tools/tr2jag_multiroom.py ;;
esac
# ☠️ GUARD FOR THE gym_lskin ALIAS. mrt_data.S no longer .incbin's a second
# copy of Lara's skeleton for the mansion - gym_lskin is a .set alias onto
# mrt_lskin, which saves 110,016 B in EVERY ROM (the old copy sat outside the
# GYMSD #endif, so even builds that ship no mansion carried it). That alias is
# only correct while the two extractions agree. They do today, by construction:
# _lskin is skeleton + all-animation joint angles, with no atlas dependence.
# If a future extractor change makes them diverge, Lara must get her own blob
# back - so FAIL LOUDLY here rather than render her wrong in Lara's Home.
if [ -f gym_lskin.bin ] && [ -f mrt_lskin.bin ]; then
    if cmp -s mrt_lskin.bin gym_lskin.bin; then
        echo "   gym_lskin == mrt_lskin (alias valid, saves $(stat -c%s gym_lskin.bin) B)"
    else
        echo "☠️ BUILD STOPPED: gym_lskin.bin and mrt_lskin.bin DIVERGED."
        echo "   mrt_data.S aliases gym_lskin onto mrt_lskin; that is now WRONG."
        echo "   Restore the .incbin for gym_lskin in mrt_data.S before shipping."
        exit 1
    fi
fi

say "Title + loading backgrounds"
TR_DELDATA="$ASSETS" python3 tools/gen_titlebg.py
say "Title passport + Lara's-Home photo"
TRTITLE="$PSX/TITLE.PSX" python3 tools/tr2jag_title.py
TRTITLE="$PSX/TITLE.PSX" PASS_PREFIX=photo PASS_TYPE=73 PASS_FORCE_TEX=265 PASS_DOUBLE=1 \
    python3 tools/tr2jag_title.py
# The OPEN passport spread (ring item 2). ☠️ These passes were missing, so the
# container died at `No rule to make target pass2_geom.bin` the moment the
# title ring was switched on - the ROM links the ring items whether or not the
# build knows how to make them. Recipe is ASSET_BUILD.md's.
TRTITLE="$PSX/TITLE.PSX" PASS_TYPE=71 PASS_PREFIX=pass2 PASS_DOUBLE=1 \
    python3 tools/tr2jag_title.py
# Sound + Detail ring items (ASSET_BUILD.md). ☠️ mrt_data.S .incbin's these
# whether or not anything generated them, and a missing .incbin is an ASSEMBLER
# error, not a link error - which is why it surfaces late and cryptically.
TRTITLE="$PSX/TITLE.PSX" PASS_PREFIX=sound PASS_TYPE=96 PASS_POSES=1 PASS_DOUBLE=1 \
    python3 tools/tr2jag_title.py
TRTITLE="$PSX/TITLE.PSX" PASS_PREFIX=detail PASS_TYPE=95 PASS_POSES=1 PASS_DOUBLE=1 \
    PASS_GAIN=1.55 python3 tools/tr2jag_title.py
# PASS_GAIN rescues the lit lens but drags the FRAME swatch onto the neutral
# grey ramp (246); the PS1's frame is warm. Patch it back.
python3 - <<'PYEOF'
b = bytearray(open("detail_atlas.bin","rb").read())
for i, v in enumerate(b):
    if v == 246: b[i] = 119
open("detail_atlas.bin","wb").write(bytes(b))
PYEOF
python3 tools/gen_polaroid.py    # photo_geom -> a real polaroid
python3 tools/gen_openbook.py    # model 71 is a near-closed fan; author the spread
say "UI font"
TRLEVEL="$PSX/LEVEL1.PSX" python3 tools/tr2jag_font.py
say "Sound effects"
TRLEVEL="$PSX/LEVEL1.PSX" python3 tools/tr2jag_sound.py
AUDIO="$(ls "$ASSETS"/TRACK02.* 2>/dev/null | head -1 || true)"
if [ -n "$AUDIO" ]; then
    say "Title music (streams from MUSIC.PCM on the SD card)"
    MUSIC_SRC="$AUDIO" python3 tools/tr2jag_music.py   # -> music.bin + MUSIC.PCM
else
    say "No CD-audio track on this disc -> silent title (game unaffected)"
    rm -f MUSIC.PCM
fi
# ── 2b. the FRONT END: four clips, all off the user's own disc ───────────────
# EIDOS.JV  MOVIES/INTRO.STR  - that file OPENS with the Eidos logo (152 frames)
# CORE.JV   FMV/CORELOGO.FMV  - the Core Design logo
# INTRO.JV  FMV/CAFE.FMV      - the attract cinematic
# CAVES.JV  FMV/SNOW.FMV      - the trek that plays before the Caves
# Matched by frame count against the shipped clips (152/193 exact; the two long
# ones land within the encoder's audio-pad). Nothing here ships with the repo
# and no third-party footage is involved - it is all on the disc you mounted.
# Set VIDEO=0 to skip (much faster build; the game then boots to the title).
if [ "${VIDEO:-1}" = "1" ]; then
    say "Front-end videos (four clips off the disc - this is the slow part)"
    FMVDIR="$ASSETS/FMV"        # dumped by extract_disc.py via WANT_FMV
    conv() {   # conv <source> <OUT.JV>
        [ -f "$FMVDIR/$1" ] || { echo "   (no $1 on this disc - skipping $2)"; return 0; }
        JV_VQ=1 JV_CROP="0,0,320,208" python3 tools/tr2jag_video.py \
              "$FMVDIR/$1" "$2" 15 >/dev/null 2>&1 \
            && echo "   $2 <- $1  ($(stat -c%s "$2") B)" \
            || echo "   !! $2 failed to convert - the build continues without it"
    }
    conv INTRO.STR    EIDOS.JV
    conv CORELOGO.FMV CORE.JV
    conv CAFE.FMV     INTRO.JV
    conv SNOW.FMV     CAVES.JV
else
    say "VIDEO=0 - skipping the front-end clips"
fi

# The title theme STREAMS from MUSIC.PCM on the SD card; the ROM embeds only a
# 4-byte stub so the .incbin links without baking ~370KB of audio into DRAM
# (that overflows the 2MB budget). This matches the shipped build.
printf '\0\0\0\0' > music.bin

# ── 3. build the ROM ─────────────────────────────────────────────────────────
say "Compiling for Atari Jaguar ($QUALITY)"
make clean >/dev/null
# PADTEXT: the A10 boot lottery. It pads .text, and whether a given layout
# boots at all is positional - ANY change to code or assets re-rolls it. It is
# NOT a setting you can carry between builds. If your ROM comes up black, run
# the same build again with -e PADTEXT=0/136/272/408/544/816 until one boots.
# ★ The .JV clips are not linked into the ROM, so VIDEO=0 produces a
# BYTE-IDENTICAL ROM in a fraction of the time - roll with VIDEO=0, then do one
# full run with the pad that booted.
make RMAC="$RMAC" ${JAS:+JAS="$JAS"} ${JCC68K:+JCC68K="$JCC68K"} \
     ${PADTEXT:+PADTEXT="$PADTEXT"} $BUILD_FLAGS $QUALITY_FLAGS

# ── 4. stage the GameDrive payload (everything the SD card needs, together) ───
mkdir -p "$OUT"
cp build/openlara.cof "$OUT/OPENLARA.COF"
[ -f MUSIC.PCM ] && cp MUSIC.PCM "$OUT/MUSIC.PCM" || true
# the front-end clips and the Lara's-Home loading art stream from the card too
for v in EIDOS.JV CORE.JV INTRO.JV CAVES.JV; do
    [ -f "$v" ] && cp "$v" "$OUT/$v"
done
[ -f gymload.bin ] && cp gymload.bin "$OUT/GYMLOAD.DAT"
cat > "$OUT/COPY-THESE-TO-SD-ROOT.txt" <<'NOTE'
Copy EVERY file in this folder to the ROOT of your RetroHQ GameDrive SD card
(the top level, NOT a subfolder), then boot OPENLARA.COF from the GameDrive menu.

  OPENLARA.COF   the game
  MUSIC.PCM      the title theme -- the game STREAMS it from the SD card, so if
                 this file is missing you will hear a loud hiss on the title
                 screen. Copy it too.

Level loads take 10-15 seconds -- that's the 68000 doing its thing.
NOTE
say "Done!  ->  $OUT"
echo "   Copy EVERY file below to the ROOT of your GameDrive SD card, then boot OPENLARA.COF:"
ls -1 "$OUT" | sed 's/^/     /'
