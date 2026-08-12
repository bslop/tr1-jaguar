#!/usr/bin/env bash
# drive.sh <outdir> <script> - drive Lara from the host AND capture, in ONE lease.
#
# Proven on silicon 2026-08-11: run, running jump, back, and both turns all
# respond (start-vs-end frame diff 28.8 against 0.7 when the presses are not
# landing - that number is the go/no-go, a filmstrip alone can look plausible
# while Lara stands still for 24 seconds).
#
#   tools/drive.sh out/ tools/drive/move.txt
#
# Script lines are "<mask> <seconds>"; masks are
#   UP 1  DOWN 2  LEFT 4  RIGHT 8  A=jump 16  B=action 32  C=walk 64
#
# ☠️ Both halves need the jaghw lock: tools/gdpad.sh takes it per press and
# vroll_game.sh takes it for the whole capture, so running them side by side
# deadlocks or interleaves badly. This holds the lock once, starts ffmpeg in the
# background inside it, and writes INPUT.BIN with jaggd directly.
#
# ☠️ Only ever point this at a FASTBOOT build: gdpad writes over the control
# endpoint WHILE the game runs, and a build that streams video/music from the
# cart will race its own gd_freads and lock the console.
set -uo pipefail
OUT="${1:?usage: drive.sh <outdir> <script>}"; SCRIPT="${2:?}"
JAGHW=/home/jvilla/Documents/Git/jaguar-shared/hw/jaghw
JAGGD=/home/jvilla/Documents/Git/open_jaggd/jaggd
export JAGHW_PROJECT=jag_openlara
mkdir -p "$OUT"; TMP=$(mktemp -d)
SECS=$(awk 'BEGIN{s=0} !/^#/ && NF==2 {s+=$2} END{printf "%d", s+3}' "$SCRIPT")
echo "== driving for ${SECS}s"
# ☠️ USE jag_gd.sh's OWN bus detection. A hand-rolled `lsusb -d 04b4:`
# matched NOTHING, so JAGGD_BUS was empty and every press silently failed
# while the capture looked fine - the run read as "Lara never moves".
GD_USB_ID=$(grep -m1 "^GD_USB_ID=" /home/jvilla/Documents/Git/jag_openlara/jag_gd.sh | cut -d= -f2- | tr -d "\"'")
B=$(lsusb 2>/dev/null | grep -i "$GD_USB_ID" | head -1 | sed -E 's/^Bus 0*([0-9]+).*/\1/')
[ -n "$B" ] || { echo "GameDrive not enumerated"; exit 1; }
echo "== GameDrive on bus $B"
"$JAGHW" run --lease $((SECS + 60)) -- bash -c '
    set -u
    OUT="'"$OUT"'"; TMP="'"$TMP"'"; SCRIPT="'"$SCRIPT"'"
    JAGGD="'"$JAGGD"'"; B="'"$B"'"; SECS="'"$SECS"'"
    ffmpeg -nostdin -hide_banner -loglevel error -f v4l2 -i /dev/video0 \
           -t "$SECS" -y "$OUT/drive.mkv" 2>"$OUT/ffmpeg.log" &
    FF=$!
    sleep 1
    while read -r m t; do
        case "$m" in ""|\#*) continue;; esac
        printf "$(printf "\\\\x%02x\\\\x%02x\\\\x00\\\\x00" $(( (m>>8)&255 )) $(( m&255 )))" > "$TMP/INPUT.BIN"
        env JAGGD_BUS="$B" "$JAGGD" -wf "$TMP/INPUT.BIN" >/dev/null 2>&1
        sleep "$t"
    done < "$SCRIPT"
    printf "\\x00\\x00" > "$TMP/INPUT.BIN"
    env JAGGD_BUS="$B" "$JAGGD" -wf "$TMP/INPUT.BIN" >/dev/null 2>&1
    wait $FF
'
rm -rf "$TMP"
[ -s "$OUT/drive.mkv" ] || { echo "NO CAPTURE"; exit 1; }
ffmpeg -hide_banner -loglevel error -i "$OUT/drive.mkv" -vf fps=2 -y "$OUT/f%03d.png"
echo "-> $OUT ($(ls "$OUT"/f*.png | wc -l) stills at 2fps)"
