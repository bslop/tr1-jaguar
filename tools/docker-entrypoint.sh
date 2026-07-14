#!/usr/bin/env bash
# Container entrypoint: find the disc the user mounted at /disc and run the
# full build.  DISC_NAME picks a specific file inside /disc (set by convert.sh);
# if unset we treat /disc itself as the disc (a folder or a single-file mount).
set -euo pipefail

D="/disc"
[ -n "${DISC_NAME:-}" ] && D="/disc/$DISC_NAME"

if [ ! -e "$D" ]; then
    echo "error: no Tomb Raider disc found at $D"
    echo "mount one and set DISC_NAME, e.g.:"
    echo "  docker run --rm -v /path/to/discfolder:/disc:ro -v \"\$PWD/out:/out\" \\"
    echo "             -e DISC_NAME='Tomb Raider (USA) (v1.6).cue' tr-jaguar"
    exit 2
fi

bash tools/build_cof.sh "$D" /out
status=$?

# Docker writes /out as root; hand the finished files back to the user who
# launched the run (convert.sh passes their uid/gid).  Harmless elsewhere.
if [ -n "${OUT_UID:-}" ] && [ "$status" -eq 0 ]; then
    chown -R "$OUT_UID:${OUT_GID:-$OUT_UID}" /out 2>/dev/null || true
fi
exit $status
