#!/usr/bin/env bash
# Interactive sdkconfig editor inside the container.
# Run from WSL: ./scripts/menuconfig.sh
set -euo pipefail
cd "$(dirname "$0")/.."
docker compose build idf
docker compose run --rm idf idf.py menuconfig
