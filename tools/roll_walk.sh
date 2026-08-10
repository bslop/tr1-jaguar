#!/usr/bin/env bash
# walk.sh <arm> <padtext...> — roll each build until one boots LIT, then stop.
# Capture health is checked per roll: a 3840x2160 negotiation is the Cam Link's
# no-signal fallback, NOT a black boot, so it aborts instead of blaming the ROM.
set -uo pipefail
cd /home/jvilla/Documents/Git/jag_openlara/OpenLara-master/src/platform/jaguar
SP=/tmp/claude-1000/-home-jvilla-Documents-Git-jag-openlara/4ac6b7a2-22f8-49fd-9507-f1000322b659/scratchpad
ARM="$1"; shift
for pt in "$@"; do
    OUT=$SP/roll_${ARM}_p$pt
    rm -rf "$OUT"
    echo "===== $ARM PADTEXT=$pt"
    timeout 400 tools/vroll_game.sh "$SP/${ARM}_p$pt.cof" "$OUT" 50 >"$OUT.log" 2>&1
    MKV=$(ls "$OUT"/*.mkv 2>/dev/null | head -1)
    if [ -z "$MKV" ]; then echo "  no capture file"; continue; fi
    WH=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
         -of csv=p=0:s=x "$MKV" 2>/dev/null)
    LIT=$(grep -o 'brightness by second:.*' "$OUT.log" | grep -o '[0-9]*:[1-9][0-9]*' | wc -l)
    echo "  capture $WH   lit-seconds=$LIT"
    if [ "$WH" = "3840x2160" ]; then
        echo "  !! CAPTURE DEAD (no-signal fallback) — aborting walk, not a ROM verdict"
        exit 2
    fi
    if [ "$LIT" -ge 5 ]; then
        echo "  ==> LIT: $ARM PADTEXT=$pt   ($MKV)"
        echo "$pt" > "$SP/${ARM}_lit.txt"
        exit 0
    fi
done
echo "no roll lit for $ARM"
exit 1
