#!/usr/bin/env bash
# Drop into an interactive shell inside the container, IDF environment
# already sourced (the espressif/idf image's entrypoint does this).
# Run from WSL: ./scripts/shell.sh
set -euo pipefail
cd "$(dirname "$0")/.."
docker compose build idf
docker compose run --rm idf bash
