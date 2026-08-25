#!/usr/bin/env bash
# Wipe the build/ directory inside the container (use if a build gets into a
# bad state after e.g. an sdkconfig.defaults or CMakeLists change).
# Run from WSL: ./scripts/fullclean.sh
set -euo pipefail
cd "$(dirname "$0")/.."
docker compose run --rm idf idf.py fullclean
