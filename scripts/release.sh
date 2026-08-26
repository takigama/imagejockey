#!/usr/bin/env bash
# Build the firmware and publish it as a GitHub release with all four flash
# images attached -- imagejockey.bin is what the device's OTA update pulls
# from (see main/ota.c's OTA_URL, which always points at the "latest"
# release), and all four together are what a first-time flash from a
# downloaded release needs (see build.md) -- bootloader/partition-table/
# ota_data_initial rarely change between releases, but attaching them every
# time is cheap and keeps "download the release and flash it" always correct.
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

ASSETS=(
    "build/bootloader/bootloader.bin"
    "build/partition_table/partition-table.bin"
    "build/ota_data_initial.bin"
    "build/imagejockey.bin"
)
for f in "${ASSETS[@]}"; do
    if [[ ! -f "$f" ]]; then
        echo "error: $f not found after build" >&2
        exit 1
    fi
done

gh release create "$TAG" "${ASSETS[@]}" --title "$TAG" --notes "$NOTES"

# Keep the GitHub Pages web-flasher (docs/) in sync with what was just
# released -- ESP Web Tools needs the four images same-origin as the page
# itself (GitHub release assets aren't reliably CORS-enabled), so these are
# a checked-in copy, not fetched live from Releases.
cp build/bootloader/bootloader.bin docs/bootloader.bin
cp build/partition_table/partition-table.bin docs/partition-table.bin
cp build/ota_data_initial.bin docs/ota_data_initial.bin
cp build/imagejockey.bin docs/imagejockey.bin
jq --arg v "$TAG" '.version = $v' docs/manifest.json > docs/manifest.json.tmp
mv docs/manifest.json.tmp docs/manifest.json

git add docs/
git commit -m "Update web-flasher assets for $TAG"
git push origin master

echo "Released $TAG with ${ASSETS[*]} attached -- devices on real WiFi can now OTA to it,"
echo "and a first-time flash can now use just the downloaded release assets (see build.md)"
echo "or the web flasher at docs/ (once GitHub Pages is enabled for this repo)."
