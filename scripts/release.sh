#!/usr/bin/env bash
# Build the firmware and publish it as a GitHub release with the .bin
# attached as an asset -- this is what the device's OTA update pulls from
# (see main/ota.c's OTA_URL, which always points at the "latest" release).
#
# Requires `gh auth login` once beforehand.
#
# Run from WSL: ./scripts/release.sh v0.3.0 ["release notes"]
set -euo pipefail
cd "$(dirname "$0")/.."

TAG="${1:?usage: release.sh <tag> [notes]}"
NOTES="${2:-Release $TAG}"

# Tag *before* building -- main/ota.c's FIRMWARE_VERSION comes from `git
# describe` at build time (see top-level CMakeLists.txt), so the binary's
# own idea of its version has to see this tag already exist, or it embeds
# "N commits past the previous tag" instead of the tag it's actually being
# released under. That mismatch makes /ota/check report "update available"
# forever, even right after updating to the latest release.
git tag "$TAG"
git push origin "$TAG"

./scripts/build.sh

BIN="build/imagejockey.bin"
if [[ ! -f "$BIN" ]]; then
    echo "error: $BIN not found after build" >&2
    exit 1
fi

gh release create "$TAG" "$BIN" --title "$TAG" --notes "$NOTES"

echo "Released $TAG with $BIN attached -- devices on real WiFi can now OTA to it."
