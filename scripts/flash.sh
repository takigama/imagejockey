#!/usr/bin/env bash
# Flash the built firmware over USB.
#
# The T-Dongle-S3's serial port has to be attached from Windows into WSL
# first (one-time per plug-in), from a Windows PowerShell/cmd prompt with
# admin rights:
#   usbipd list                          # find the BUSID of the dongle
#   usbipd bind   --busid <BUSID>        # one-time, persists across reboots
#   usbipd attach --wsl --busid <BUSID>  # do this each time you plug it in
#
# Then from WSL, this device shows up as /dev/ttyACM0 (or /dev/ttyUSB0) —
# check with `ls /dev/tty*` if the default below doesn't match.
#
# Run from WSL: ./scripts/flash.sh [/dev/ttyACM0]
set -euo pipefail
cd "$(dirname "$0")/.."

DEVICE="${1:-/dev/ttyACM0}"

if [[ ! -e "$DEVICE" ]]; then
  echo "error: $DEVICE not found." >&2
  echo "Attach the board into WSL first — see the comment at the top of this script." >&2
  echo "Currently visible serial devices:" >&2
  ls /dev/tty* 2>/dev/null | grep -E 'ACM|USB' || echo "  (none found)" >&2
  exit 1
fi

docker build -t tdisplay-media-idf:v5.5.5 ./docker
docker run --rm -it \
  -v "$PWD":/project -w /project \
  --device="$DEVICE" \
  tdisplay-media-idf:v5.5.5 \
  idf.py -p "$DEVICE" flash
