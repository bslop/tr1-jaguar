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
#   QUALITY=playable              60 render lines through the OP's 4x scaler.
#                                 Same full screen and field of view, coarser
#                                 vertically; measurably the fastest build.
#
# They differ ONLY in VRESN - identical feature set, identical assets - so a
# bug in one is a bug in both.  Measured on silicon 2026-08-11 with the demo
# feature set (see PLAY_BUILD.md for the full ladder and both columns):
#
#      120 lines  6.40 fps      60 lines  ~7.6 fps (extrapolated from the
#                                          enemy-free 7.92; the 60-line
#                                          no-HUD arm was not rolled)
#
QUALITY="${QUALITY:-pretty}"
case "$QUALITY" in
    pretty)   QUALITY_FLAGS="" ;;
    playable) QUALITY_FLAGS="VRESN=60" ;;
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
BUILD_FLAGS="${BUILD_FLAGS:-MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 \
STAGEDIET=1 PIPELINE=1 PIPESTAGE=2 HOPDIAL=1 HOPBOOT=1 XCULL=1 BEXIT=1 \
ROWDIET=1 STATICS=1 BANKDIET=1 LOWRES=1 FLIPASM=1 DIVZGUARD=1 MOVESET=1 \
SPANSHADE=1 SHADEEXCL=1 TIMESTEP=1 ANIMRATE=1 OPDBL=1 LPLANES=1 AUTOSTART=1 \
VCBIG=1 ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 BLOBCACHE=1 JCENT=1 JOVL=1 \
SECTLONG=1 INLINEMUL=1 OFFHOIST=1 VPACK=1 NOPCLIP=1}"

say() { printf '\n\033[1;36m==>\033[0m %s\n' "$*"; }

# ── 1. disc -> a private extraction dir (never touches the repo's assets) ─────
ASSETS="$(mktemp -d)"; trap 'rm -rf "$ASSETS"' EXIT
say "Reading disc: $DISC"
python3 tools/extract_disc.py "$DISC" "$ASSETS"
PSX="$ASSETS/PSXDATA"

# ── 2. convert every asset (order matters: gen_titlebg writes title_pal.bin,
#       which tr2jag_title.py then maps the passport/photo textures onto) ──────
say "Extracting levels + Lara (Caves)"
TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=3072 LARA_MINAREA=0 \
    TRLEVEL="$PSX/LEVEL1.PSX" TRPREFIX=mrt python3 tools/tr2jag_multiroom.py
say "Extracting Lara's Home (Mansion)"
TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=3072 LARA_MINAREA=0 \
    TRLEVEL="$PSX/GYM.PSX" TRPREFIX=gym python3 tools/tr2jag_multiroom.py
say "Title + loading backgrounds"
TR_DELDATA="$ASSETS" python3 tools/gen_titlebg.py
say "Title passport + Lara's-Home photo"
TRTITLE="$PSX/TITLE.PSX" python3 tools/tr2jag_title.py
TRTITLE="$PSX/TITLE.PSX" PASS_PREFIX=photo PASS_TYPE=73 PASS_FORCE_TEX=265 PASS_DOUBLE=1 \
    python3 tools/tr2jag_title.py
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
# The title theme STREAMS from MUSIC.PCM on the SD card; the ROM embeds only a
# 4-byte stub so the .incbin links without baking ~370KB of audio into DRAM
# (that overflows the 2MB budget). This matches the shipped build.
printf '\0\0\0\0' > music.bin

# ── 3. build the ROM ─────────────────────────────────────────────────────────
say "Compiling for Atari Jaguar ($QUALITY)"
make clean >/dev/null
make RMAC="$RMAC" $BUILD_FLAGS $QUALITY_FLAGS

# ── 4. stage the GameDrive payload (everything the SD card needs, together) ───
mkdir -p "$OUT"
cp build/openlara.cof "$OUT/OPENLARA.COF"
[ -f MUSIC.PCM ] && cp MUSIC.PCM "$OUT/MUSIC.PCM" || true
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
