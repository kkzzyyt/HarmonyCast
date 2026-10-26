#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$DIR"

echo "=== 1. Building HarmonyCast.app ==="
cmake --build build --target HarmonyCast

echo "=== 2. Generating DMG background ==="
python3 scripts/generate_dmg_background.py resources/dmg_background.png

echo "=== 3. Preparing DMG staging folder ==="
STAGING_DIR="build/dmg_pack"
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"
cp -R "build/HarmonyCast.app" "$STAGING_DIR/"

VERSION=$(grep -m1 "project(HarmonyCast VERSION" CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/')
DMG_OUTPUT="build/HarmonyCast-${VERSION}-macOS-arm64.dmg"
rm -f "$DMG_OUTPUT"

echo "=== 4. Creating fancy DMG with create-dmg ==="
create-dmg \
  --volname "HarmonyCast" \
  --volicon "resources/app-icon.icns" \
  --background "resources/dmg_background.png" \
  --window-pos 200 120 \
  --window-size 660 400 \
  --icon-size 128 \
  --icon "HarmonyCast.app" 170 190 \
  --app-drop-link 490 190 \
  --hide-extension "HarmonyCast.app" \
  --format UDZO \
  --overwrite \
  "$DMG_OUTPUT" \
  "$STAGING_DIR"

rm -rf "$STAGING_DIR"
echo "=== DMG Build Succeeded: $DMG_OUTPUT ==="
ls -lh "$DMG_OUTPUT"
