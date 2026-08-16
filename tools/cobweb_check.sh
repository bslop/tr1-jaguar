#!/usr/bin/env bash
# cobweb_check.sh - is the toolchain we build with behind bslop/cobweb?
#
#   tools/cobweb_check.sh            report only (safe, read-only, ~1s)
#   tools/cobweb_check.sh --update   pull, rebuild, and re-verify the kernels
#
# WHY THIS EXISTS: cobweb is not a frozen dependency - it is actively worked on,
# and its updates have repeatedly changed what is POSSIBLE here rather than just
# fixing bugs. Recent examples: GD_FSeek appeared (our video converter still
# says "the GD BIOS has no seek"), and the Blitter's Z-buffer path was
# implemented (relevant to the 2.07x overdraw that is this renderer's standing
# frame-rate lever). Work done against a stale copy can be work done around a
# limitation that no longer exists.
#
# ☠️ --update RE-VERIFIES rather than trusting: after rebuilding it assembles
# the shipping renderer and byte-compares against the previous binary, because
# a toolchain change that silently alters the ROM is exactly what must not slip
# through unnoticed.
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
CW="${COBWEB_DIR:-$(cd "$HERE/../../../../" && pwd)/cobweb}"
UPDATE=0; [ "${1:-}" = "--update" ] && UPDATE=1

if [ ! -d "$CW/.git" ]; then
    echo "cobweb: no checkout at $CW"
    echo "  git clone https://github.com/bslop/cobweb.git \"$CW\""
    exit 2
fi

git -C "$CW" fetch -q origin 2>/dev/null || { echo "cobweb: fetch failed (offline?)"; exit 1; }
LOCAL=$(git -C "$CW" rev-parse --short HEAD)
REMOTE=$(git -C "$CW" rev-parse --short origin/main)
BEHIND=$(git -C "$CW" rev-list --count HEAD..origin/main 2>/dev/null || echo 0)
AHEAD=$(git -C "$CW" rev-list --count origin/main..HEAD 2>/dev/null || echo 0)

if [ "$BEHIND" = "0" ] && [ "$AHEAD" = "0" ]; then
    echo "cobweb: UP TO DATE ($LOCAL)"
    exit 0
fi

[ "$AHEAD" != "0" ] && echo "cobweb: $AHEAD local commit(s) NOT pushed - push them before they are lost"
if [ "$BEHIND" != "0" ]; then
    echo "cobweb: $BEHIND NEW commit(s) upstream ($LOCAL -> $REMOTE)"
    git -C "$CW" log --oneline --no-decorate HEAD..origin/main | sed 's/^/    /'
    echo "  read these before assuming a limitation still exists; run with --update to take them."
fi
[ "$UPDATE" = "0" ] && exit 0

# ---- update, rebuild, and prove the ROM did not move underneath us ----------
BEFORE=$(mktemp); AFTER=$(mktemp)
KFLAGS="--gpu -d LOWRES=1 -d VRESN=80 -d SHADEPASS=1 -d STAGEDIET=1 -d XCULL=1 -d BEXIT=1 \
-d ROWDIET=1 -d BANKDIET=1 -d SHADEEXCL=1 -d DIVZGUARD=1 -d SPANSHADE=1 -d INLINEMUL=1 \
-d OFFHOIST=1 -d VPACK=1 -d NEARCLIP=0 -d VIEWH=0 -d UVNEG=0 -d UVFIX=0 -d NOFILL=0 -d NOSPAN=0 \
-d HALFSPAN=0 -d VRES60=0 -d PROFGPU=0 -d NOMUL=0 -d NODIV=0 -d NOSTORE=0 -d NOCULL=0 -d NOBLIT=0 \
-d ALLCULL=0 -d RUNHIST=0 -d TRAPEZOID=0 -d DRIFTLOOSE=0 -d PHRASESHADE=0 -d DIVHIDE=0 \
-d RUNBATCH=0 -d RBNOUV=0 -d CULLCOUNT=0 -d NOBFCULL=0 -d BEXCNT=0 -d SDPROBE=0 -d NOSDCULL=0 \
-d WCCNT=0 -d NOEMPTYY=0 -d GPUBG=0 -d NEARLOW=0 -d PREPASSONLY=0 -d MMULTX=0 -d MMXDIAG=0 \
-d UVCLAMP=0 -d UVPROBE=0 -d TINYCULL=0 -d JMPDIET=0 -d LARACOUNT=0 -d KEEPDEGEN=0 -d TINYKEEP=0 \
-d BWOVER=0 -d ODRAW=0 -d PHRASEDST=0 -d IMULPROBE=0 -d FOURBPP=0 -d VCJDIET=0 -d SYNCDRAIN=0 -d ODRAWS=0"
JAS="$CW/sim/target/release/jas"
[ -x "$JAS" ] && $JAS "$HERE/gpu_geotex.gas" -o "$BEFORE" $KFLAGS >/dev/null 2>&1

echo "cobweb: updating..."
git -C "$CW" pull --rebase -q origin main || { echo "  pull failed - resolve by hand"; exit 1; }
cargo build --release --manifest-path "$CW/sim/Cargo.toml" 2>&1 | grep -E "^error|Finished" | tail -1

if [ -s "$BEFORE" ] && $JAS "$HERE/gpu_geotex.gas" -o "$AFTER" $KFLAGS >/dev/null 2>&1; then
    if cmp -s "$BEFORE" "$AFTER"; then
        echo "  renderer byte-identical after the update - safe"
    else
        echo "  ☠️ THE RENDERER CHANGED ($(stat -c%s "$BEFORE") -> $(stat -c%s "$AFTER") bytes)."
        echo "     Do not ship without re-testing on silicon: a toolchain update moved the ROM."
    fi
fi
rm -f "$BEFORE" "$AFTER"
echo "cobweb: now at $(git -C "$CW" rev-parse --short HEAD)"
