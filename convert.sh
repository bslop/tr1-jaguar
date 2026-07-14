#!/usr/bin/env bash
# convert.sh <Tomb Raider PSX disc> [output folder]
#
# Turn your own Tomb Raider 1 (USA) PlayStation disc into an Atari Jaguar
# GameDrive build.  The only thing you need installed is Docker.
#
#   ./convert.sh "Tomb Raider (USA) (v1.6).cue"
#   ./convert.sh TombRaider.chd  ~/Desktop/tr-jaguar
#
# Accepts .iso, .bin (+.cue), .cue, .chd, .7z/.zip, or a folder that already
# holds the disc's PSXDATA.  Works on Linux, macOS and Windows (run it inside
# WSL2 or Git Bash, with Docker Desktop running).  No game data is uploaded
# anywhere: everything happens locally, inside the container.
set -euo pipefail

usage() {
    echo "usage: ./convert.sh <TombRaider.(iso|bin|cue|chd|7z)|folder> [output folder]"
    echo "       output folder defaults to ./TombRaider-Jaguar"
    exit 1
}
[ $# -ge 1 ] || usage
SRC="$1"; OUT="${2:-./TombRaider-Jaguar}"

if ! command -v docker >/dev/null 2>&1; then
    echo "error: Docker is required but not found."
    echo "       Install Docker Desktop (Windows/macOS) or Docker Engine (Linux):"
    echo "       https://docs.docker.com/get-docker/"
    exit 1
fi
[ -e "$SRC" ] || { echo "error: no such file or folder: $SRC"; exit 1; }

here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"; OUT_ABS="$(cd "$OUT" && pwd)"

# Mount the disc's FOLDER read-only so a .cue can reach its sibling .bin tracks.
if [ -d "$SRC" ]; then
    DISC_DIR="$(cd "$SRC" && pwd)"; DISC_NAME=""
else
    DISC_DIR="$(cd "$(dirname "$SRC")" && pwd)"; DISC_NAME="$(basename "$SRC")"
fi

echo "==> Preparing the converter (first run builds the toolchain image; later runs are instant)…"
docker build -t tr-jaguar "$here"

echo "==> Converting ${DISC_NAME:-$SRC} …"
# MSYS_NO_PATHCONV keeps Git Bash on Windows from rewriting the /disc paths.
MSYS_NO_PATHCONV=1 docker run --rm \
    -v "$DISC_DIR:/disc:ro" -v "$OUT_ABS:/out" \
    -e DISC_NAME="$DISC_NAME" \
    -e OUT_UID="$(id -u 2>/dev/null || echo 0)" -e OUT_GID="$(id -g 2>/dev/null || echo 0)" \
    tr-jaguar

echo
echo "Done!  Copy EVERY file below to the ROOT of your GameDrive SD card, then boot OPENLARA.COF."
echo "(MUSIC.PCM streams from the card -- don't skip it, or the title screen hisses."
echo " See COPY-THESE-TO-SD-ROOT.txt in the folder.)"
echo
ls -1 "$OUT_ABS" | sed 's/^/   /'
