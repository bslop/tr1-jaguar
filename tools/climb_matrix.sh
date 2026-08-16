#!/usr/bin/env bash
# climb_matrix.sh - build, boot and DRIVE one climb per TR1 ledge class, then
# say which classes work. The point is to stop judging the climb from wherever
# the player happens to be standing: ledge_census.py finds a real example of
# every class in the baked collision data, and this drives Lara at each one.
#
#   tools/climb_matrix.sh [outdir]
#
# Pass/fail is read from the TRAVDIAG verdict cell, not from "it looked right":
#   before the attempt the ledge must read 2 (VAULT) or 3 (JUMPGRAB)
#   after a successful climb she is standing ON it, so it reads 0 (flat)
# A class that never leaves its starting verdict did not climb.
#
# ☠️ Each class needs its own ROM (SPAWNAT is compile-time) and every ROM
# re-rolls the A10 boot lottery, so each one is walked until a pad lights.
# ☠️ The capture dies across repeated reboot-to-stub uploads and only recovers
# on a POWER CYCLE - so every attempt power-cycles first. Without that the walk
# reports phantom black boots (it did, six in a row, 2026-08-15).
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HERE/climbmatrix}"; mkdir -p "$OUT"
ROOT=/home/jvilla/Documents/Git/jag_openlara
JAGHW=/home/jvilla/Documents/Git/jaguar-shared/hw/jaghw
export JAGHW_PROJECT=jag_openlara

BASEFLAGS="ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 ENEMYTEX=1 BLOBCACHE=1 \
JCENT=1 JOVL=1 SECTLONG=1 NOPCLIP=1 VRESN=80 TRAPFLOOR=1 GYMSD=1 GUNS=1 \
PROBE_AHEAD=256 LARACOUNT=1 TRAVDIAG=1 FASTBOOT=1 GDPAD=1"

# read one test spot per class out of the census
spots() { python3 "$HERE/tools/ledge_census.py" --top 1 | grep -E "^   room" -B0; }

verdict() {   # verdict <png> -> the lit cell index, or -1
python3 - "$1" <<'EOF'
import sys
from PIL import Image
import numpy as np
im = np.array(Image.open(sys.argv[1]).convert('L')).astype(float)
cols = np.where(im.max(axis=0) > 18)[0]; rows = np.where(im.max(axis=1) > 18)[0]
if not len(cols) or not len(rows): print(-1); raise SystemExit
x0, x1, y0, y1 = cols.min(), cols.max(), rows.min(), rows.max()
pw, ph = x1-x0+1, y1-y0+1
FBH = 80                                  # VRESN
def px(fx, fy): return x0+pw*fx/320.0, y0+ph*fy/float(FBH)
def cell(fx, fy):
    cx, cy = px(fx+2.5, fy+2.5)
    b = im[int(cy-4):int(cy+5), int(cx-4):int(cx+5)]
    return float(b.mean()) if b.size else 0.0
calib = cell(170, FBH-16)                 # always-lit block anchors the grid
if calib < 40: print(-1); raise SystemExit # readout not up / capture dark
cells = [cell(180+c*7, FBH-16) for c in range(10)]
v = int(np.argmax(cells))
print(v if cells[v] > calib*0.5 else -1)
EOF
}

grab() {  # grab <out.png>  - a short recording, last frame (single grabs race the lock)
  "$JAGHW" run --lease 40 -- bash -c "
     ffmpeg -nostdin -hide_banner -loglevel error -f v4l2 -i /dev/video0 -t 4 -y '$OUT/g.mkv'" >/dev/null 2>&1
  ffmpeg -hide_banner -loglevel error -sseof -2 -i "$OUT/g.mkv" -frames:v 1 -y "$1" 2>/dev/null
}

printf '%-10s %-6s %-8s %-8s %s\n' CLASS RISE BEFORE AFTER RESULT | tee "$OUT/report.txt"
# ☠️ READ THE CENSUS INTO AN ARRAY FIRST, DO NOT PIPE IT INTO THE LOOP. The
# build, the upload and drive.sh all read stdin, and inside a `... | while read`
# they EAT the remaining lines: the first run silently lost CLIMB2 and CLIMB3
# entirely and delivered "WALL" as "LL". Same trap as ffmpeg needing -nostdin.
# Every child below also gets </dev/null so it cannot reach for stdin again.
mapfile -t SPOTS < <(python3 "$HERE/tools/ledge_census.py" --tsv --top 1)
for LINE in "${SPOTS[@]}"; do
    IFS=$'\t' read -r CLS ROOM RISE SX SY SZ YAW <<< "$LINE"
    [ "$CLS" = "WALL" ] && continue
    cd "$HERE"
    rm -rf build
    tools/gbuild.sh cm_$CLS $BASEFLAGS \
        SPAWNAT_ROOM=$ROOM SPAWNAT_X=$SX SPAWNAT_Y=$SY SPAWNAT_Z=$SZ SPAWNAT_YAW=$YAW \
        >/dev/null 2>&1 </dev/null
    LIT=""
    for P in 0 136 272 408 544 816; do
        R="$HERE/build_cm_$CLS/cm_${CLS}_p$P.cof"; [ -f "$R" ] || continue
        (cd "$ROOT" && ./jag_gd.sh power cycle >/dev/null 2>&1)
        until lsusb 2>/dev/null | grep -qi "03eb:800e"; do sleep 3; done
        (cd "$ROOT" && ./jag_gd.sh upload "$R" >/dev/null 2>&1 </dev/null)
        sleep 14
        grab "$OUT/${CLS}_boot.png"
        [ "$(verdict "$OUT/${CLS}_boot.png")" -ge 0 ] 2>/dev/null && { LIT=$P; break; }
    done
    if [ -z "$LIT" ]; then
        printf '%-10s %-6s %-8s %-8s %s\n' "$CLS" "$RISE" "-" "-" "NO LIT PAD" | tee -a "$OUT/report.txt"
        continue
    fi
    # ☠️ THE "BEFORE" READING MUST BE TAKEN AT THE WALL, NOT AT SPAWN. The
    # census stands her in the CELL CENTRE, 512 units from the boundary, and
    # the probe only reaches 256 - so at spawn the verdict is legitimately 0
    # (flat) no matter how good the ledge is. Walk her into it FIRST, read the
    # verdict there, and only then ask for the climb.
    printf '0 2\n1 6\n0 2\n' > "$OUT/$CLS.walk.txt"
    "$HERE/tools/drive.sh" "$OUT/walk_$CLS" "$OUT/$CLS.walk.txt" >/dev/null 2>&1 </dev/null
    grab "$OUT/${CLS}_before.png"
    B=$(verdict "$OUT/${CLS}_before.png")
    printf '33 7\n0 3\n' > "$OUT/$CLS.climb.txt"
    "$HERE/tools/drive.sh" "$OUT/climb_$CLS" "$OUT/$CLS.climb.txt" >/dev/null 2>&1 </dev/null
    grab "$OUT/${CLS}_after.png"
    A=$(verdict "$OUT/${CLS}_after.png")
    if [ "$A" = "0" ] && [ "$B" != "0" ]; then RES="CLIMBED"
    elif [ "$A" = "$B" ];                then RES="REFUSED (verdict unchanged)"
    else                                      RES="CHANGED $B->$A (inspect)"; fi
    printf '%-10s %-6s %-8s %-8s %s\n' "$CLS" "$RISE" "$B" "$A" "$RES" | tee -a "$OUT/report.txt"
done
echo; echo "frames + report in $OUT"
