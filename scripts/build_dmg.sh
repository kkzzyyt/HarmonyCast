#!/bin/bash
set -euo pipefail

TASK_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$TASK_ROOT"
BUILD_DIR="${HARMONYCAST_BUILD_DIR:-build}"
QT_PREFIX="${HARMONYCAST_QT_PREFIX:-${QT_ROOT_DIR:-}}"
if [ -z "$QT_PREFIX" ]; then QT_PREFIX="$(brew --prefix qt)"; fi
FFMPEG_PREFIX="${HARMONYCAST_FFMPEG_PREFIX:-}"
if [ -z "$FFMPEG_PREFIX" ]; then FFMPEG_PREFIX="$(brew --prefix ffmpeg)"; fi
MACDEPLOYQT_BIN="$QT_PREFIX/bin/macdeployqt"
QT_PATHS="$QT_PREFIX/bin/qtpaths"
if [ ! -x "$QT_PATHS" ]; then QT_PATHS="$QT_PREFIX/bin/qtpaths6"; fi
[ -x "$MACDEPLOYQT_BIN" ] && [ -x "$QT_PATHS" ] || { echo "Qt deployment tools missing under $QT_PREFIX" >&2; exit 1; }

cmake --build "$BUILD_DIR" --target HarmonyCast
APP_BINARY="$BUILD_DIR/HarmonyCast.app/Contents/MacOS/HarmonyCast"
MAC_ARCH="${HARMONYCAST_MAC_ARCH:-$(lipo -archs "$APP_BINARY")}"
case "$MAC_ARCH" in arm64|x86_64) ;; *) echo "Expected a native arm64 or x86_64 app, found $MAC_ARCH" >&2; exit 1 ;; esac
lipo "$APP_BINARY" -verify_arch "$MAC_ARCH"

if [ ! -f resources/dmg_background.png ]; then
    python3 scripts/generate_dmg_background.py resources/dmg_background.png
fi
STAGING_DIR="$BUILD_DIR/dmg_pack"
python3 -c 'import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)' "$STAGING_DIR"
mkdir -p "$STAGING_DIR"
cp -R "$BUILD_DIR/HarmonyCast.app" "$STAGING_DIR/"
APP_IN_STAGING="$STAGING_DIR/HarmonyCast.app"
QT_PLUGIN_ROOT="$("$QT_PATHS" --query QT_INSTALL_PLUGINS)"
PLUGIN_ARGS=()
for plugin in \
    platforms/libqcocoa.dylib \
    platforms/libqoffscreen.dylib \
    imageformats/libqjpeg.dylib \
    imageformats/libqsvg.dylib \
    iconengines/libqsvgicon.dylib \
    styles/libqmacstyle.dylib \
    tls/libqsecuretransportbackend.dylib \
    tls/libqcertonlybackend.dylib \
    networkinformation/libqapplenetworkinformation.dylib; do
    if [ -f "$QT_PLUGIN_ROOT/$plugin" ]; then
        mkdir -p "$APP_IN_STAGING/Contents/PlugIns/$(dirname "$plugin")"
        cp -L "$QT_PLUGIN_ROOT/$plugin" "$APP_IN_STAGING/Contents/PlugIns/$plugin"
        PLUGIN_ARGS+=("-executable=$APP_IN_STAGING/Contents/PlugIns/$plugin")
    fi
done
"$MACDEPLOYQT_BIN" "$APP_IN_STAGING" -no-plugins "${PLUGIN_ARGS[@]}" \
    -always-overwrite -verbose=1 \
    "-libpath=$FFMPEG_PREFIX/lib" "-libpath=$QT_PREFIX/lib" 2>&1 | tee "$BUILD_DIR/macdeployqt.log"
python3 - "$BUILD_DIR/macdeployqt.log" <<'PYCODE'
from pathlib import Path
import sys
errors = [line for line in Path(sys.argv[1]).read_text().splitlines() if line.startswith('ERROR:')]
if errors:
    raise SystemExit('Qt deployment failed: ' + '\n'.join(errors))
PYCODE
cat > "$APP_IN_STAGING/Contents/Resources/qt.conf" <<'QTCONF'
[Paths]
Plugins = PlugIns
QTCONF

# Check every Mach-O, including helper executables and plugins, before signing.
CHECK_ARGS=("$APP_IN_STAGING" --arch "$MAC_ARCH" --write-minimum)
if [ -n "${MACOSX_DEPLOYMENT_TARGET:-}" ]; then CHECK_ARGS+=(--minimum-os "$MACOSX_DEPLOYMENT_TARGET"); fi
python3 scripts/check_macos_bundle.py "${CHECK_ARGS[@]}"
codesign --force --deep --sign - "$APP_IN_STAGING"
codesign --verify --deep --strict "$APP_IN_STAGING"

VERSION="$(python3 -c 'from pathlib import Path; import re; print(re.search(r"project\(HarmonyCast VERSION ([0-9.]+)", Path("CMakeLists.txt").read_text()).group(1))')"
DMG_OUTPUT="$BUILD_DIR/HarmonyCast-$VERSION-macOS-$MAC_ARCH.dmg"
create-dmg \
    --volname HarmonyCast --volicon resources/app-icon.icns \
    --background resources/dmg_background.png \
    --window-pos 200 120 --window-size 660 400 --icon-size 128 \
    --icon HarmonyCast.app 170 190 --app-drop-link 490 190 \
    --hide-extension HarmonyCast.app --format UDZO --overwrite \
    "$DMG_OUTPUT" "$STAGING_DIR"
hdiutil verify -quiet "$DMG_OUTPUT"
python3 -c 'import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)' "$STAGING_DIR"
echo "DMG build succeeded: $DMG_OUTPUT"
ls -lh "$DMG_OUTPUT"
