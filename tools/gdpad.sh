#!/usr/bin/env bash
# gdpad.sh - drive the running game from the host.
#
#   tools/gdpad.sh <mask> [hold_seconds]      one press
#   tools/gdpad.sh script <file>              a sequence: "<mask> <seconds>" per line
#
# The GameDrive writes INPUT.BIN over the control endpoint WITHOUT resetting
# the running game (that is the facility the Skunkboard lacks), and a GDPAD
# build reads it as a PAD_* mask every 4th frame.
#
#   UP 1  DOWN 2  LEFT 4  RIGHT 8  A 16  B 32  C 64  PAUSE 128  OPTION 256
#   X 512  Y 1024  Z 2048        (A = jump/select, B = action/grab, C = walk)
#
# ☠️ Never run this against a build that streams video or music from the cart:
# the write races the game's own gd_freads and locks the console.  FASTBOOT
# builds stream neither.
set -uo pipefail
GD="${GD:-./jag_gd.sh}"
TMP=$(mktemp -d)
press() {                       # $1 = mask, $2 = seconds
    printf "$(printf '\\x%02x\\x%02x' $(( ($1>>8)&255 )) $(( $1&255 )))" > "$TMP/INPUT.BIN"
    "$GD" run -wf "$TMP/INPUT.BIN" >/dev/null 2>&1
    sleep "$2"
}
if [ "${1:-}" = "script" ]; then
    while read -r m t; do
        case "$m" in ''|\#*) continue;; esac
        echo "  pad $m for ${t}s"
        press "$m" "$t"
    done < "$2"
    press 0 0.2                 # release
else
    press "${1:?usage: gdpad.sh <mask> [seconds] | gdpad.sh script <file>}" "${2:-0.5}"
    press 0 0.2
fi
rm -rf "$TMP"
