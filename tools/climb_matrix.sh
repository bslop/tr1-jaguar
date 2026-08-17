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

# ☠️ END THE TURN WITH A REBOOT (user, 2026-08-17). Whoever edits this loop:
# the last thing a rig cycle does must be `jag_gd.sh endturn`, so the next
# session does not inherit our ROM running on the board.
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
import sys, glob, os
from PIL import Image
import numpy as np
# ☠️ DO NOT DERIVE THE PICTURE BOX FROM ONE FRAME'S CONTENT. Rooms in this
# game can render almost entirely BLACK (see the open black-geometry bugs), and
# then the "bounding box of non-black pixels" collapses onto whatever is lit -
# so the readout cells get sampled in the wrong place and a perfectly good boot
# reports as unreadable. Take the box from the MAXIMUM over every frame of the
# recording (the OP's display area is fixed, so more frames = truer bounds).
src = sys.argv[1]
mkv = os.path.join(os.path.dirname(src), "g.mkv")
acc = None
if os.path.exists(mkv):
    os.system("ffmpeg -hide_banner -loglevel error -i '%s' -vf fps=4 -y '%s/_bx%%03d.png' 2>/dev/null"
              % (mkv, os.path.dirname(src)))
    for f in sorted(glob.glob(os.path.join(os.path.dirname(src), "_bx*.png"))):
        a = np.array(Image.open(f).convert('L')).astype(float)
        acc = a if acc is None else np.maximum(acc, a)
        os.remove(f)
im = np.array(Image.open(src).convert('L')).astype(float)
if acc is None: acc = im
# Locate the FULL-WIDTH calibration rule the ROM paints near the bottom: the
# widest near-solid run of bright pixels in the lower half. Its ends ARE the
# picture box, so this works even when the room itself renders black.
low = acc[acc.shape[0] // 2:, :]
best = 0; bx0 = bx1 = byr = 0
for r in range(low.shape[0]):
    lit = np.where(low[r] > 140)[0]
    if len(lit) > best and (lit.max() - lit.min() + 1) - len(lit) < 40:
        best, bx0, bx1, byr = len(lit), int(lit.min()), int(lit.max()), r
if best < 300: print(-1); raise SystemExit
x0, x1 = bx0, bx1
FBH0 = 80                                   # VRESN: the rule is at row FBH0-20
yrow = acc.shape[0] // 2 + byr
ph = yrow / ((FBH0 - 20) / float(FBH0))
y0 = 0; y1 = int(ph) - 1
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

# ☠️☠️☠️ THE WHOLE MEASURE CYCLE IS **ONE** ACQUISITION - INCLUDING THE WAITS.
# This loop used to take FIVE separate leases per pad: power-cycle (lease),
# enumeration wait (UNLOCKED), upload (lease), 14s settle (UNLOCKED), grab
# (lease). PROTOCOL.md rule 2 forbids exactly that, and it is the same bug that
# already shipped in jag_quake/scripts/flash.sh - here it was worse, because it
# sat inside a per-class loop that yanks the SHARED MAINS unattended on every
# pass. Two failures, both of which look like somebody ELSE's bug:
#   - another project mid-capture gets power-cycled out from under it, and
#     reads the result as a boot failure in THEIR code;
#   - our own grab, in the unlocked 14s window, can photograph ANOTHER
#     project's frame - and a capture that belongs to someone else is worse
#     than a failed capture, because it looks like a result.
# Caught by jag_bubsy3d reading this file, 2026-08-16. The build stays OUTSIDE
# the lock (it is minutes long and touches no hardware); jaghw is re-entrant
# via JAGHW_HELD, so grab()'s own inner lease nests instead of deadlocking.
cycle_one() {   # cycle_one <rom> <out.png>  - power, upload, settle, capture
  (cd "$ROOT" && ./jag_gd.sh power cycle >/dev/null 2>&1)
  until lsusb 2>/dev/null | grep -qi "03eb:800e"; do sleep 3; done
  (cd "$ROOT" && ./jag_gd.sh upload "$1" >/dev/null 2>&1 </dev/null)
  sleep 14
  grab "$2"
}
export -f cycle_one grab
export ROOT JAGHW OUT

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
        "$JAGHW" run --lease 300 -- \
            bash -c 'cycle_one "$1" "$2"' _ "$R" "$OUT/${CLS}_boot.png" </dev/null
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
    # ☠️ ONE SECOND, NOT SIX. Run speed is 47 units per 30Hz tick = ~1410
    # units/second, and the census stands her ONE CELL (512 units, ~0.36s) from
    # the ledge. Walking 6s carried her about EIGHT SECTORS past it, so the
    # "before" frame showed her in open ground with no ledge in sight and every
    # class reported a flat verdict. Walk just far enough to reach the wall.
    printf '0 2\n1 1\n0 2\n' > "$OUT/$CLS.walk.txt"
    # ☠️ DO NOT SWALLOW THE DRIVER'S OUTPUT. It failed silently for three runs
    # (a bad path from the release scrub) and every class read "REFUSED".
    "$HERE/tools/drive.sh" "$OUT/walk_$CLS" "$OUT/$CLS.walk.txt" >"$OUT/$CLS.walk.log" 2>&1 </dev/null
    grep -qi "NO CAPTURE\|No such file" "$OUT/$CLS.walk.log" && echo "  !! driver failed for $CLS - see $OUT/$CLS.walk.log"
    grab "$OUT/${CLS}_before.png"
    B=$(verdict "$OUT/${CLS}_before.png")
    printf '33 4\n0 3\n' > "$OUT/$CLS.climb.txt"
    "$HERE/tools/drive.sh" "$OUT/climb_$CLS" "$OUT/$CLS.climb.txt" >"$OUT/$CLS.climb.log" 2>&1 </dev/null
    grab "$OUT/${CLS}_after.png"
    A=$(verdict "$OUT/${CLS}_after.png")
    if [ "$A" = "0" ] && [ "$B" != "0" ]; then RES="CLIMBED"
    elif [ "$A" = "$B" ];                then RES="REFUSED (verdict unchanged)"
    else                                      RES="CHANGED $B->$A (inspect)"; fi
    printf '%-10s %-6s %-8s %-8s %s\n' "$CLS" "$RISE" "$B" "$A" "$RES" | tee -a "$OUT/report.txt"
done
echo; echo "frames + report in $OUT"
