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
BUILD_FLAGS="${BUILD_FLAGS:-MULTIROOM=1 HALFRES=1 CFLAGS_EXTRA=-DJERRYPOSE}"

say() { printf '\n\033[1;36m==>\033[0m %s\n' "$*"; }

# ── 1. disc -> a private extraction dir (never touches the repo's assets) ─────
ASSETS="$(mktemp -d)"; trap 'rm -rf "$ASSETS"' EXIT
say "Reading disc: $DISC"
python3 tools/extract_disc.py "$DISC" "$ASSETS"
PSX="$ASSETS/PSXDATA"

# ── 2. convert every asset (order matters: gen_titlebg writes title_pal.bin,
#       which tr2jag_title.py then maps the passport/photo textures onto) ──────
say "Extracting levels + Lara (Caves)"
TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=3072 LARA_MINAREA=800 \
    TRLEVEL="$PSX/LEVEL1.PSX" TRPREFIX=mrt python3 tools/tr2jag_multiroom.py
say "Extracting Lara's Home (Mansion)"
TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=3072 LARA_MINAREA=800 \
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
say "Compiling for Atari Jaguar"
make clean >/dev/null
make RMAC="$RMAC" $BUILD_FLAGS

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
