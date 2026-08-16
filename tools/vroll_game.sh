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
GD="${GD:-./jag_gd.sh}"
JAGHW="${JAGHW:-$HOME/jaguar-shared/hw/jaghw}"
export JAGHW_PROJECT=jag_openlara

ROM=$(readlink -f "${1:?usage: vroll_game.sh <rom.cof> <outdir> [secs]}")
OUT="${2:?}"; SECS="${3:-45}"
mkdir -p "$OUT"

echo "== power cycle"
"$GD" power cycle >/dev/null 2>&1
sleep 18                      # plug off/on + Jaguar boot + USB re-enumerate

echo "== upload $(basename "$ROM")"
if ! "$GD" upload "$ROM" > "$OUT/upload.log" 2>&1; then
    echo "UPLOAD FAILED:"; tail -3 "$OUT/upload.log"
    "$GD" power cycle >/dev/null 2>&1     # leave the rig in a clean state
    exit 1
fi

echo "== record ${SECS}s"
"$JAGHW" run --lease $((SECS + 40)) -- \
    ffmpeg -hide_banner -loglevel error -f v4l2 -i /dev/video0 \
           -t "$SECS" -y "$OUT/roll.mkv" 2>"$OUT/ffmpeg.log"

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
