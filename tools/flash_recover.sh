#!/usr/bin/env bash
# flash_recover.sh <rom.cof> [jagq run args...]
#
# Flash a ROM and RECOVER FROM A WEDGE WITHOUT STOPPING.
#
# ☠️ THE RULE (the user, verbatim, jaguar-shared 075cdda 2026-08-18):
#   "if the unit is wedged, then a reboot happens via the remote power switch
#    for the Jaguar."
# A wedge is a STEP, NOT A STOP. No asking for hands, no parking the work, no
# handing the rig on broken. It costs ~16 s of your own turn - which is reason
# enough not to cause one, and no reason at all to stop.
#
# This exists because on 2026-08-18 I hit `exit -6` twice, power-cycled once
# (correctly, under a lease), and then STOPPED and asked the user whether to
# retry. That is the thing the rule forbids, and a script is how it stops being
# a matter of remembering.
#
# The escalation, cheapest first (hw/POWER.md §0a is a REFINEMENT of Rule 0b,
# not an exception to it):
#   1. upload
#   2. LIBUSB_ERROR_TIMEOUT / _PIPE / exit -6  ->  reset the USB LINK (~3 s).
#      Mains is shared regardless of cart, so a bounce resets whatever ANOTHER
#      project is running mid-capture; the link reset costs them nothing.
#   3. still failing  ->  MAINS cycle via hw/jagpower, inside our own turn
#   4. verify with `jaggd -r`: the GD MENU IS THE ORACLE. If it draws, the
#      console, the video chain and the cart are all alive, and a black screen
#      after the next upload is OUR ROM, not the rig.
#   5. escalate to the user ONLY for: plug unreachable, cart never
#      re-enumerating, menu that will not draw. Say WHICH ONE you saw.
#
# ☠️ And before blaming your own ROM: `jagq history`. The fault is rig-wide but
# every symptom arrives project-shaped - six turns failed across five projects
# on 2026-08-17 before anyone said so.
set -uo pipefail
SHARED="$HOME/Documents/Git/jaguar-shared"
GD_USB_ID='03eb:800e'
ROM="${1:?usage: flash_recover.sh <rom.cof> [jagq run args...]}"; shift

say() { printf '\n\033[1;36m==>\033[0m %s\n' "$*"; }

link_reset() {
    say "resetting the USB LINK (cheap, leaves other projects' runs alone)"
    jagq exec --lease 120 --note "unwedge: USB link reset" -- bash -c '
        node=$(lsusb -d '"$GD_USB_ID"' 2>/dev/null |
               sed -E "s|Bus ([0-9]+) Device ([0-9]+).*|/dev/bus/usb/\1/\2|" | head -1)
        [ -n "$node" ] || { echo "GameDrive not on the bus - not a wedged link"; exit 4; }
        python3 - "$node" <<PY
import fcntl, sys
USBDEVFS_RESET = 0x5514          # _IO("U", 20)
with open(sys.argv[1], "wb") as f:
    fcntl.ioctl(f, USBDEVFS_RESET, 0)
print("USB reset %s" % sys.argv[1])
PY
        sleep 3'
}

mains_cycle() {
    say "MAINS cycle (Rule 0b) - the harder reset, inside our own turn"
    jagq exec --lease 120 --note "wedge: remote power switch reboot" \
        -- "$SHARED/hw/jagpower" cycle
    sleep 12
}

menu_oracle() {
    say "asking the GD menu (the oracle: if it draws, the rig is alive)"
    jagq exec --lease 120 --note "verify: back to the GD menu" \
        -- "$HOME/Documents/Git/open_jaggd/jaggd" -r
}

for attempt in 1 2 3; do
    say "flash attempt $attempt: $(basename "$ROM")"
    OUT=$(jagq run "$ROM" "$@" 2>&1); RC=$?
    echo "$OUT" | grep -E "upload|frame|signal|state" | sed 's/^/    /'
    if [ $RC -eq 0 ] && ! echo "$OUT" | grep -q "exit -6"; then
        say "flashed. ☠️ 'signal content' is NOT a boot verdict - LOOK at the frame."
        exit 0
    fi
    case "$attempt" in
        1) link_reset ;;
        2) mains_cycle; menu_oracle ;;
        3) say "three attempts failed after a link reset AND a mains cycle."
           say "THIS is the escalation the rule allows - say which one you saw:"
           say "  plug unreachable / cart never re-enumerating / menu will not draw"
           lsusb -d "$GD_USB_ID" >/dev/null 2>&1 \
             && say "  cart IS on the bus, so it is not 'never re-enumerating'" \
             || say "  cart is ABSENT from lsusb"
           exit 3 ;;
    esac
done
