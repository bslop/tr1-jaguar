#!/usr/bin/env bash
# vsweep_game.sh - roll every A10 pad of a build set until one BOOTS.
#
#   tools/vsweep_game.sh <build_dir> <tag> <outdir> [secs] [pads...]
#
# Any layout change re-rolls the A10 boot lottery and typically only 2-3 of the
# 6 pads light, so a single dark roll means nothing at all - the set does.
# Stops at the first lit roll and leaves its stills for the decoder.
set -uo pipefail
cd "$(dirname "$0")/.."
DIR="${1:?usage: vsweep_game.sh <build_dir> <tag> <outdir> [secs] [pads...]}"
TAG="${2:?}"; OUT="${3:?}"; SECS="${4:-35}"; shift 4 || true
PADS=("$@"); [ ${#PADS[@]} -gt 0 ] || PADS=(0 272 544 816 136 408)

for p in "${PADS[@]}"; do
    rom="$DIR/${TAG}_p$p.cof"
    [ -f "$rom" ] || { echo "missing $rom"; continue; }
    echo "######## pad $p"
    tools/vroll_game.sh "$rom" "$OUT/p$p" "$SECS" | tail -3
    # lit = any second above the black floor; a whole run of zeros is either an
    # A10 miss or a dead Cam Link, and only the control roll can tell them apart
    # ☠️ LIT MEANS SUSTAINED, NOT A FLASH.  A build that shows 3 seconds of
    # boot flash and then dies read as LIT and cost a whole measurement roll:
    # require 5+ non-black seconds before believing it.
    if [ "$(tr ' ' '\n' < "$OUT/p$p/brightness.txt" 2>/dev/null |
            grep -cE ':[1-9][0-9]*$')" -ge 5 ]; then
        echo "==> pad $p LIT"; exit 0
    fi
done
echo "==> every pad dark - run a KNOWN-LIT control before believing the set"
exit 1
