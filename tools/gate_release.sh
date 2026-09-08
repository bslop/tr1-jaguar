#!/bin/sh
# gate_release.sh - the four cheap checks that must pass before a ROM goes near
# the rig.  None of them needs the console; all four have caught a real defect.
#
#   1. HRESN reached ALL THREE define paths (gcc / jcc68k / jas) and did NOT
#      reach the HQ title kernel.  The ladder sat inside `ifdef VRESN` once and
#      reached two of the three -- the ROM still differed, so "the flag landed"
#      read TRUE while the game rendered 309 columns.
#   2. Zero debug/instrument flags.  FPSBEACON paints a block over the picture;
#      PADMUTE/FASTBOOT/AUTOSTART change what the user is playing.
#   3. No `move.l abs.l,abs.l` storing below $4000 -- the jas dropped-DEST-reloc
#      bug (cobweb ba9c680) wrote 24 corrupt exception vectors and went black on
#      every pad.
#   4. __bss_end under the 68k stack floor, with the headroom printed.
#
# usage: tools/gate_release.sh <payload-dir> [build.log]
set -eu
DIR=${1:?usage: gate_release.sh <payload-dir> [build.log]}
LOG=${2:-}
fail=0
skip=0
say() { printf '  %-6s %s\n' "$1" "$2"; }
# ☠️ A SKIP IS NOT A PASS. The first version of this printed "GATES PASS" with
# three of its four checks skipped for want of a build log -- a green light that
# had verified one thing out of four. Skips are counted and the verdict says
# INCOMPLETE, because a gate that can pass without running is the exact defect
# class this file exists to catch.

COF=$(ls "$DIR"/*.COF "$DIR"/*.cof 2>/dev/null | head -1) || true
[ -n "${COF:-}" ] || { echo "!!! no .COF in $DIR"; exit 1; }
echo "ROM: $COF ($(stat -c%s "$COF") B)"

echo
echo "[1] HRESN on all three define paths"
if [ -n "$LOG" ] && [ -f "$LOG" ]; then
    gccn=$(grep -E '(^| )m68k[^ ]*gcc | gcc .*-c ' "$LOG" | grep -c 'DHRESN=160' || true)
    jccn=$(grep -E 'jcc68k' "$LOG" | grep -c 'HRESN=160' || true)
    jasn=$(grep -E ' jas | jas$' "$LOG" | grep -c 'HRESN=160' || true)
    hqn=$(grep -E 'gpu_geotex_hq|GEOTEX_HQ' "$LOG" | grep -c 'HRESN' || true)
    say "gcc"    "$gccn compile lines carry -DHRESN=160"
    say "jcc68k" "$jccn lines carry HRESN=160"
    say "jas"    "$jasn lines carry HRESN=160"
    say "HQ"     "$hqn HQ-kernel lines carry HRESN (must be 0)"
    [ "$gccn" -gt 0 ] && [ "$jccn" -gt 0 ] && [ "$jasn" -gt 0 ] && [ "$hqn" -eq 0 ] || {
        echo "  !!! HRESN did not reach every path (or leaked into the HQ kernel)"; fail=1; }
else
    say "SKIP" "no build log given - cannot verify the compile lines"; skip=$((skip+1))
fi

echo
echo "[2] debug flags"
if [ -n "$LOG" ] && [ -f "$LOG" ]; then
    bad=""
    for f in FPSBEACON PADMUTE FASTBOOT AUTOSTART SPAWNAT DBGROOM HOLEVIS GUNDIAG \
             CRUMB ROOMTOUR OTLIST NOFILL NOCLEAR HALFW; do
        if grep -qE "[-D ]$f=1|[-D ]$f\b" "$LOG" 2>/dev/null; then bad="$bad $f"; fi
    done
    if [ -n "$bad" ]; then say "FAIL" "debug flags present:$bad"; fail=1
    else say "ok" "none of the 14 instrument flags appear on any compile line"; fi
else
    say "SKIP" "no build log given"; skip=$((skip+1))
fi

echo
echo "[3] jas dropped-DEST-reloc scan (move.l abs.l,abs.l below \$4000)"
n=$(python3 - "$COF" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
hits = 0
i = 0
while True:
    i = b.find(b'\x23\xf9', i)
    if i < 0 or i + 10 > len(b): break
    dest = int.from_bytes(b[i+6:i+10], 'big')
    if dest < 0x4000: hits += 1
    i += 2
print(hits)
PY
)
if [ "$n" -eq 0 ]; then say "ok" "0 such stores"; else say "FAIL" "$n stores below \$4000"; fail=1; fi

echo
echo "[4] DRAM budget"
if [ -n "$LOG" ] && [ -f "$LOG" ]; then
    line=$(grep -E 'DRAM budget' "$LOG" | tail -1 || true)
    if [ -n "$line" ]; then say "ok" "$(echo "$line" | sed 's/^ *//')"
    else say "FAIL" "the guard did not print - it did not run"; fail=1; fi
else
    say "SKIP" "no build log given"; skip=$((skip+1))
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "GATES FAIL"
    exit 1
elif [ "$skip" -ne 0 ]; then
    echo "GATES INCOMPLETE - $skip of 4 checks did not run (pass the build log)"
    exit 2
fi
echo "GATES PASS (4/4 ran)"
