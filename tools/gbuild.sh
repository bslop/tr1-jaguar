#!/usr/bin/env bash
# gbuild.sh - build the GAME with the video read-out panel, one ROM per A10
# layout roll.  (The previous copy of this harness lived in a scratchpad that
# was cleaned; it lives in tools/ now so it cannot be lost again.)
#
#   tools/gbuild.sh <tag> [extra make flags...]
#
# Produces build/<tag>_p<PAD>.cof for every PADTEXT roll.  ALWAYS build the
# whole set: any layout change re-rolls the A10 boot lottery, and typically
# only 2-3 of the 6 pads light (project_a10_reproducible).
#
# ☠️ rm -rf build between sets - make tracks timestamps, and a stale
# dsp_pose.bin once "proved" the mixer innocent in an arm that never rebuilt it.
set -euo pipefail
cd "$(dirname "$0")/.."

TAG="${1:?usage: gbuild.sh <tag> [extra make flags...]}"; shift || true

# The demo recipe minus ENTITIES: the panel CODE plus doors/switches overflows
# the 16KB stack headroom check, and none of it is live while a boot clip
# plays.  JVFASTKICK is in every video build (8.31 -> 13.78 fps on its own).
BASE=(MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 STAGEDIET=1
      PIPELINE=1 PIPESTAGE=2 HOPDIAL=1 HOPBOOT=1 XCULL=1 BEXIT=1
      ROWDIET=1 STATICS=1 BANKDIET=1 LOWRES=1 FLIPASM=1
      DIVZGUARD=1 MOVESET=1 SPANSHADE=1 SHADEEXCL=1
      TIMESTEP=1 ANIMRATE=1 OPDBL=1 LPLANES=1 AUTOSTART=1
      VCBIG=1 BOOTVID=1 JVFASTKICK=1 INLINEMUL=1 OFFHOIST=1 VPACK=1)

# MENU=1 (as an argument): come up on the TITLE RING instead of dropping
# straight into the caves.  ☠️ AUTOSTART must be OMITTED, never set to 0 -
# `make FOO=0` still defines FOO in this Makefile.
if [ "${*#*MENU=1}" != "$*" ]; then
    NEW=(); for f in "${BASE[@]}"; do
        [ "$f" = "AUTOSTART=1" ] || NEW+=("$f"); done
    BASE=("${NEW[@]}")
    NEW=(); for f in "$@"; do
        [ "$f" = "MENU=1" ] || NEW+=("$f"); done
    set -- "${NEW[@]}"
fi

PADS=(0 272 544 816 136 408)
OUT=build_$TAG
rm -rf build "$OUT"; mkdir -p "$OUT"
for p in "${PADS[@]}"; do
    rm -rf build
    # A pad that overflows the 16KB stack-headroom check is not a build error,
    # it is a pad this feature set cannot use: PADTEXT pads TEXT, and BSS sits
    # above it.  Skip it and keep the rest of the lottery - but SAY SO, because
    # a silently short set is a silently worse chance of booting.
    make "${BASE[@]}" "$@" PADTEXT=$p >"$OUT/${TAG}_p$p.log" 2>&1 || {
        if grep -q "stack headroom" "$OUT/${TAG}_p$p.log"; then
            echo "pad=$p  SKIPPED (BSS over the stack-headroom guard)"; continue
        fi
        echo "BUILD FAILED pad=$p"; tail -5 "$OUT/${TAG}_p$p.log"; exit 1; }
    cp build/openlara.cof "$OUT/${TAG}_p$p.cof"
    printf '%-6s %s bytes\n' "pad=$p" "$(stat -c%s "$OUT/${TAG}_p$p.cof")"
done
echo "-> $OUT/${TAG}_p*.cof"
