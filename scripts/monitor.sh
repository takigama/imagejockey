#!/usr/bin/env bash
# Open the serial monitor. Same USB-attach prerequisite as scripts/flash.sh.
# Run from WSL: ./scripts/monitor.sh [/dev/ttyACM0]
# Exit with Ctrl+] .
set -euo pipefail
cd "$(dirname "$0")/.."

DEVICE="${1:-/dev/ttyACM0}"

if [[ ! -e "$DEVICE" ]]; then
  echo "error: $DEVICE not found." >&2
  echo "Attach the board into WSL first — see the comment at the top of scripts/flash.sh." >&2
  exit 1
fi

docker build -t tdisplay-media-idf:v5.5.5 ./docker
docker run --rm -it \
  -v "$PWD":/project -w /project \
  --device="$DEVICE" \
  tdisplay-media-idf:v5.5.5 \
  idf.py -p "$DEVICE" monitor
