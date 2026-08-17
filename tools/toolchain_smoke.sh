#!/usr/bin/env bash
# toolchain_smoke.sh - does the CURRENT toolchain still build a ROM that DRAWS?
#
# ☠️☠️ WHY THIS EXISTS. cobweb_check.sh --update used to declare "renderer
# byte-identical - safe" and that was TRUE AND USELESS: it only re-assembles
# gpu_geotex.gas. cobweb bf31dee changed **jcc68k** - moving a 16-bit parameter
# read by +2 bytes - which broke the gcc/jcc68k ABI boundary this project links
# across, and produced a ROM that runs perfectly (GPU 218M instrs, no illegal
# instruction, vblank vector intact) and renders a COMPLETELY BLACK SCREEN.
# Four runs were spent bisecting assets that were never at fault.
#
# ★ A TOOLCHAIN-SAFETY CHECK MUST EXERCISE WHAT THE TOOLCHAIN BUILDS.
#   Comparing one hand-picked artefact proves only that that artefact is stable.
#
#   tools/toolchain_smoke.sh              build + screenshot + compare
#   tools/toolchain_smoke.sh --baseline   record the CURRENT result as good
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
JE="${JE:-/home/jvilla/Documents/Git/jag_openlara/cobweb/sim/target/release/jagemu}"
BASE="$HERE/tools/.toolchain_baseline"
OUT=$(mktemp -d)
# Small, fast, representative: the caves with the mansion left out.
FLAGS="MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 STAGEDIET=1 PIPELINE=1 \
PIPESTAGE=2 HOPDIAL=1 HOPBOOT=1 XCULL=1 BEXIT=1 ROWDIET=1 STATICS=1 BANKDIET=1 \
LOWRES=1 FLIPASM=1 DIVZGUARD=1 MOVESET=1 SPANSHADE=1 SHADEEXCL=1 TIMESTEP=1 \
ANIMRATE=1 OPDBL=1 LPLANES=1 AUTOSTART=1 VCBIG=1 BOOTVID=1 JVFASTKICK=1 \
INLINEMUL=1 OFFHOIST=1 VPACK=1 ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 \
ENEMYTEX=1 BLOBCACHE=1 JCENT=1 JOVL=1 SECTLONG=1 NOPCLIP=1 VRESN=80 \
TRAPFLOOR=1 GYMSD=1 GUNS=1 PROBE_AHEAD=256 RCLIPFIX=1"

cd "$HERE" || exit 1
rm -rf build
if ! make $FLAGS PADTEXT=0 > "$OUT/build.log" 2>&1; then
    echo "☠️ TOOLCHAIN SMOKE: THE BUILD FAILED"; tail -5 "$OUT/build.log"; exit 1
fi
"$JE" screenshot build/openlara.cof --frames 1500 -o "$OUT/s.png" >/dev/null 2>&1
BLK=$(python3 -c "
from PIL import Image
im=Image.open('$OUT/s.png').convert('L'); px=list(im.getdata())
print('%.1f' % (100.0*sum(1 for p in px if p<8)/len(px)))" 2>/dev/null)
LUM=$(python3 -c "
from PIL import Image
print(max(Image.open('$OUT/s.png').convert('L').getdata()))" 2>/dev/null)
SZ=$(stat -c%s build/openlara.cof)
echo "  smoke: ROM $SZ B, black ${BLK}%, maxluma $LUM"

if [ "${1:-}" = "--baseline" ]; then
    echo "$BLK $SZ" > "$BASE"; echo "  baseline recorded: black ${BLK}%, $SZ B"; exit 0
fi
[ -f "$BASE" ] || { echo "  (no baseline yet - run --baseline on a KNOWN-GOOD toolchain)"; exit 0; }
read -r OLDBLK OLDSZ < "$BASE"
# ☠️ The failure mode is TOTAL: a broken toolchain gave 100% black / maxluma 0,
# a good one 1.1% / 217. A generous threshold still catches it, and generous is
# right - normal scene variation moves black% by a few points.
BAD=$(python3 -c "print(1 if (float('$BLK') > float('$OLDBLK') + 25 or int('$LUM') < 32) else 0)")
if [ "$BAD" = "1" ]; then
    echo "☠️☠️ TOOLCHAIN SMOKE FAILED: black ${BLK}% (baseline ${OLDBLK}%), maxluma $LUM"
    echo "   The ROM builds and runs but does not DRAW. Suspect the C compiler:"
    echo "   see jaguar-shared/COBWEB_ISSUES_JCC68K_ABI.md. Pin cobweb and bisect"
    echo "   in a git worktree - never checkout an old rev in the shared tree."
    exit 1
fi
echo "  smoke PASSED (baseline black ${OLDBLK}%, $OLDSZ B)"
