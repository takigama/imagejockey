#!/usr/bin/env bash
# Build the firmware inside the pinned ESP-IDF Docker image.
# Run from WSL: ./scripts/build.sh
set -euo pipefail
cd "$(dirname "$0")/.."
docker compose build idf
docker compose run --rm idf idf.py build
