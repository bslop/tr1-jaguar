#!/usr/bin/env bash
# vroll_game.sh - roll ONE game ROM at the rig and capture the boot clip.
#
#   tools/vroll_game.sh <rom.cof> <outdir> [record_secs]
#
# Sequence, and every step of it is a law paid for in rig hours:
#   1. POWER-CYCLE FIRST.  This ROM streams from the same cart the uploader
#      talks to, so an upload into a running clip races its gd_freads and
#      locks the console (feedback_sd_write_while_running); and an A10 miss
#      leaves a state the GameDrive cannot reset out of, which wedges the NEXT
#      upload's USB link and makes every roll after it read dark.
#   2. upload; success is jaggd printing OK!, never its exit status.
#   3. record a clip of the TV, then cut stills out of it - one ffmpeg open,
#      not twenty, and the video also dates the EIDOS->CORE (blue->gold)
#      transition, which is the ground truth for "Tom or the 68k?".
#   4. hand the stills to tools/panel_readout.py, which REFUSES to report
#      unless its calibration word decodes.
set -uo pipefail
cd "$(dirname "$0")/.."
GD="${GD:-$HOME/Documents/Git/jag_openlara/jag_gd.sh}"
JAGHW="${JAGHW:-$HOME/Documents/Git/jaguar-shared/hw/jaghw}"
export JAGHW_PROJECT=jag_openlara

ROM=$(readlink -f "${1:?usage: vroll_game.sh <rom.cof> <outdir> [secs]}")
OUT="${2:?}"; SECS="${3:-45}"
mkdir -p "$OUT"

# ☠️☠️ CYCLE + SETTLE + UPLOAD + RECORD ARE **ONE** ACQUISITION.
# This used to take three separate leases with an 18-SECOND UNLOCKED GAP in the
# middle (power-cycle, sleep, upload, then a lease only for the recording).
# PROTOCOL.md rule 2: "a measure cycle is one operation, INCLUDING its settle
# time... locking only the upload and leaving the waits unlocked is a real bug".
# Unlocked, another project can flash between our upload and our recording -
# and then we record THEIR program and report it as our result. The power cycle
# is the worse half: mains is shared by every cart, so an unleased cycle resets
# whatever anyone else is running. Found by jag_bubsy3d, 2026-08-16.
# jaghw is re-entrant (JAGHW_HELD), so $GD's own internal leases nest safely.
cycle_and_record() {
    echo "== power cycle"
    "$GD" power cycle >/dev/null 2>&1
    sleep 18                  # plug off/on + Jaguar boot + USB re-enumerate
    echo "== upload $(basename "$ROM")"
    if ! "$GD" upload "$ROM" > "$OUT/upload.log" 2>&1; then
        echo "UPLOAD FAILED:"; tail -3 "$OUT/upload.log"
        "$GD" power cycle >/dev/null 2>&1   # leave the rig in a clean state
        return 1
    fi
    echo "== record ${SECS}s"
    ffmpeg -nostdin -hide_banner -loglevel error -f v4l2 -i /dev/video0 \
           -t "$SECS" -y "$OUT/roll.mkv" 2>"$OUT/ffmpeg.log"
}
export -f cycle_and_record
export GD ROM OUT SECS

# ☠️ PROTOCOL.md caps a lease at 600 s. The cycle needs SECS + ~80 for the
# power/boot/enumerate/upload preamble, so a long -secs request cannot be
# honoured as one acquisition - refuse rather than silently split the cycle
# back into the racy shape this comment exists to prevent.
LEASE=$((SECS + 80))
if [ "$LEASE" -gt 600 ]; then
    echo "refusing: secs=$SECS needs a ${LEASE}s lease, over PROTOCOL.md's 600s cap."
    echo "  record in shorter takes - do NOT split the cycle across leases."
    exit 2
fi
"$JAGHW" run --lease "$LEASE" -- bash -c cycle_and_record </dev/null || exit 1

[ -s "$OUT/roll.mkv" ] || { echo "NO CAPTURE - see $OUT/ffmpeg.log"; exit 1; }
ffmpeg -hide_banner -loglevel error -i "$OUT/roll.mkv" \
       -vf fps=1 -y "$OUT/f%03d.png" 2>>"$OUT/ffmpeg.log"

# A dark roll is an A10 miss OR a dead capture - never guess which.  Print the
# per-second brightness so a run of zeros can be told from a lit frame.
python3 - "$OUT" <<'PY' | tee "$OUT/brightness.txt"
import sys, glob, os
from PIL import Image
d = sys.argv[1]
fs = sorted(glob.glob(os.path.join(d, "f*.png")))
print("  brightness by second:", " ".join(
    f"{i+1}:{int(sum(Image.open(f).convert('L').resize((32,24)).getdata())/768)}"
    for i, f in enumerate(fs)))
PY
echo "== stills in $OUT (decode: tools/panel_readout.py $OUT/f*.png)"
