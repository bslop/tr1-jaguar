#!/usr/bin/env bash
# build_conf.sh — build the ROMs tools/conformance.py drives.
#
#   tools/build_conf.sh caves     -> /tmp/conf.cof + /tmp/conf.elf
#   tools/build_conf.sh gym       -> /tmp/gym.cof  + /tmp/gym.elf
#   tools/build_conf.sh both
#
# WHY THIS FILE EXISTS: runs 40-45 built these ROMs by hand and recorded only
# their PATHS in AUTORUN_STATE.md, never their FLAGS. That is the "a value only
# in shell history is not a setting" trap, and it bit immediately — run 46
# wanted to re-measure the jump-grab fix and could not reproduce the ROM the
# earlier numbers came from. The flag set IS the experiment; if it is not in a
# file, the numbers are not comparable to anything.
#
# Differences from the shipping set (tools/build_cof.sh) and why each is needed:
#   AUTOSTART=1   skip the title ring and drop into the level. The release comes
#                 up on the ring on purpose; a sweep cannot sit through it.
#   BOOTVID=1     ☠️ KEEP THIS EVEN THOUGH THE SWEEP WANTS NO VIDEOS. Run 46 left
#                 it out as "nothing to gain" and the ROM sat on a flat blue
#                 screen for 2600+ fields — all 25 spots came back UNTESTABLE
#                 with garbage floor reads. Substituting FASTBOOT (which does
#                 skip the GameDrive wait) was worse: it renders to frame ~400
#                 and then goes 100% BLACK. Neither was diagnosed further,
#                 because the flag set below is the one PROVEN to render.
#   GYMSD=1       caves only — leaves Lara's Home out of the image, which the
#                 caves sweep never visits and which keeps the ROM small.
#   AUTOGYM=1     gym only — selects Lara's Home, a different level SET
#                 (g_useset=1), not a different spawn point. It must be built
#                 WITHOUT GYMSD or the level is not in the image to select.
#   no PADMUTE    ☠️ THE HARNESS DRIVES THE PAD. PADMUTE rewrites `pad` to zero
#                 and every driven input silently does nothing — this already
#                 cost one full sweep that read as a level-wide climb failure.
#
# ☠️ THIS SET IS COPIED FROM tools/toolchain_smoke.sh, NOT from build_cof.sh, and
# that is deliberate. The smoke set is the only flag combination with a RECORDED
# RENDERING BASELINE (tools/.toolchain_baseline: 1.1% black, maxluma 217 at
# frame 1500), so a conformance ROM built this way can be checked against a
# number before anyone trusts a sweep run on it. It differs from the shipping
# set mainly in VRESN=80; climb geometry does not depend on render height.
#
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE" || exit 1

# ☠️ TOOLCHAIN PIN — same reason as build_cof.sh: cobweb bf31dee+ ships a jcc68k
# whose 16-bit parameter read moved +2 bytes, breaking the gcc/jcc68k mixed link
# this project depends on. The ROM builds and renders a BLACK SCREEN.
if [ -z "${COBWEB_DIR:-}" ] && [ -x /tmp/cobweb-old/sim/target/release/jas ]; then
    export COBWEB_DIR=/tmp/cobweb-old
fi

# Kept deliberately in step with build_cof.sh's BUILD_FLAGS. A conformance ROM
# that does not match the shipping feature set measures a game nobody plays.
BASE="MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 STAGEDIET=1 PIPELINE=1 \
PIPESTAGE=2 HOPDIAL=1 HOPBOOT=1 XCULL=1 BEXIT=1 ROWDIET=1 STATICS=1 BANKDIET=1 \
LOWRES=1 FLIPASM=1 DIVZGUARD=1 MOVESET=1 SPANSHADE=1 SHADEEXCL=1 TIMESTEP=1 \
ANIMRATE=1 OPDBL=1 LPLANES=1 AUTOSTART=1 VCBIG=1 BOOTVID=1 JVFASTKICK=1 \
INLINEMUL=1 OFFHOIST=1 VPACK=1 ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 \
ENEMYTEX=1 BLOBCACHE=1 JCENT=1 JOVL=1 SECTLONG=1 NOPCLIP=1 VRESN=80 \
TRAPFLOOR=1 GUNS=1 PROBE_AHEAD=256 RCLIPFIX=1 FITSTEP=1"

build_one() {
    local name="$1" extra="$2"
    echo "==> building $name ROM"
    rm -rf build            # ☠️ stale objects survive a flag change otherwise
    # EXTRA=... appends flags for an A/B without editing this file. They are
    # verified below like every other flag, so a typo shows up as "NOT in the
    # compile line" instead of quietly measuring the baseline twice.
    # SKIP="XCULL BEXIT" REMOVES flags from BASE for an A/B. ☠️ It has to remove
    # them: `make XCULL=0` still DEFINES XCULL in this Makefile, so the obvious
    # way to turn a feature off actually leaves it on.
    local use="$BASE"
    for k in ${SKIP:-}; do
        use=$(echo "$use" | sed -E "s/(^| )$k=[^ ]*/ /g")
        echo "    SKIP: $k removed from the flag set"
    done
    if ! make $use $extra ${EXTRA:-} > "/tmp/build_$name.log" 2>&1; then
        echo "☠️ $name build FAILED — tail of /tmp/build_$name.log:"
        tail -12 "/tmp/build_$name.log"
        return 1
    fi
    cp build/openlara.cof "/tmp/$name.cof"
    cp build/openlara.elf "/tmp/$name.elf"
    echo "    /tmp/$name.cof  $(stat -c%s /tmp/$name.cof) B"
    # Verify the flag actually landed rather than trusting the command line.
    for k in ${SKIP:-}; do
        grep -q -- "-D$k" "/tmp/build_$name.log" \
            && echo "    ☠️ $k STILL in the compile line - the A/B is invalid" \
            || echo "    $k: confirmed absent"
    done
    for f in $extra ${EXTRA:-}; do
        case "$f" in
            *=1) k="${f%=1}"
                 # ☠️ CHECK THE ASSEMBLER TOO. Some flags are jas symbols, not C
                 # defines - NEARLOW is `.if NEARLOW=1` inside gpu_geotex.gas and
                 # reaches the build as `-d NEARLOW=1`. Looking only for -D
                 # reported a correctly-built ROM as "NOT in the compile line",
                 # which would have thrown away a valid A/B as invalid.
                 if grep -q -- "-D$k" "/tmp/build_$name.log"; then
                     echo "    $k: in the compile line"
                 elif grep -q -- "-d *$k=1" "/tmp/build_$name.log"; then
                     echo "    $k: passed to the assembler (-d $k=1)"
                 else
                     echo "    ☠️ $k reached NEITHER the compiler nor the assembler"
                 fi ;;
        esac
    done
}

rc=0
case "${1:-both}" in
    caves) build_one conf "GYMSD=1" || rc=1 ;;
    gym)   build_one gym  "AUTOGYM=1" || rc=1 ;;
    both)  build_one conf "GYMSD=1" || rc=1; build_one gym "AUTOGYM=1" || rc=1 ;;
    *) echo "usage: build_conf.sh {caves|gym|both}"; exit 2 ;;
esac
exit $rc
