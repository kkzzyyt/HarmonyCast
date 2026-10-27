#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$DIR"

echo "=== 1. Building HarmonyCast.app ==="
cmake --build build --target HarmonyCast

echo "=== 2. Generating DMG background ==="
if [ ! -f "resources/dmg_background.png" ]; then
    python3 scripts/generate_dmg_background.py resources/dmg_background.png || true
fi

echo "=== 3. Preparing DMG staging folder & deploying standalone frameworks ==="
STAGING_DIR="build/dmg_pack"
python3 -c "import shutil, os; shutil.rmtree('$STAGING_DIR', ignore_errors=True)"
mkdir -p "$STAGING_DIR"
cp -R "build/HarmonyCast.app" "$STAGING_DIR/"

APP_IN_STAGING="$STAGING_DIR/HarmonyCast.app"

echo "=== 4. Running macdeployqt to bundle Qt and FFmpeg dependencies ==="
MACDEPLOYQT_BIN="/opt/homebrew/bin/macdeployqt"
if [ ! -x "$MACDEPLOYQT_BIN" ]; then
    MACDEPLOYQT_BIN="$(which macdeployqt || true)"
fi

if [ -x "$MACDEPLOYQT_BIN" ]; then
    "$MACDEPLOYQT_BIN" "$APP_IN_STAGING" \
        -always-overwrite \
        -verbose=1 \
        -libpath=/opt/homebrew/lib \
        -libpath=/opt/homebrew/opt/ffmpeg/lib
else
    echo "ERROR: macdeployqt not found!"
    exit 1
fi

echo "=== 5. Configuring standalone qt.conf ==="
cat << 'EOF' > "$APP_IN_STAGING/Contents/Resources/qt.conf"
[Paths]
Plugins = PlugIns
EOF

echo "=== 6. Sanitizing library IDs and references ==="
python3 -c "
import os, subprocess

app_path = '$APP_IN_STAGING'
for root, dirs, files in os.walk(app_path):
    for f in files:
        p = os.path.join(root, f)
        if os.path.islink(p): continue
        try:
            res = subprocess.run(['otool', '-D', p], capture_output=True, text=True)
            if res.returncode == 0 and res.stdout.strip():
                lines = res.stdout.strip().splitlines()
                if len(lines) >= 2:
                    cur_id = lines[1].strip()
                    if '/opt/homebrew' in cur_id:
                        new_id = '@rpath/' + os.path.basename(cur_id)
                        subprocess.run(['install_name_tool', '-id', new_id, p], check=False)
        except Exception:
            pass
"

echo "=== 7. Re-signing application bundle with ad-hoc signature ==="
codesign --force --deep --sign - "$APP_IN_STAGING"
codesign --verify --deep --strict "$APP_IN_STAGING"

echo "=== 8. Verifying zero /opt/homebrew dependency ==="
python3 -c "
import subprocess, os, sys

app = '$APP_IN_STAGING'
bad = []
for root, dirs, files in os.walk(app):
    for f in files:
        p = os.path.join(root, f)
        if os.path.islink(p): continue
        try:
            res = subprocess.run(['otool', '-L', p], capture_output=True, text=True)
            if res.returncode == 0:
                for line in res.stdout.splitlines()[1:]:
                    dep = line.strip().split()[0]
                    if '/opt/homebrew' in dep:
                        bad.append((p, dep))
        except Exception:
            pass

if bad:
    print(f'ERROR: Found {len(bad)} unbundled references to /opt/homebrew:')
    for p, dep in bad:
        print(f'  {p} -> {dep}')
    sys.exit(1)
else:
    print('SUCCESS: Application bundle is 100% self-contained! 0 external homebrew dependencies.')
"

VERSION=$(grep -m1 "project(HarmonyCast VERSION" CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/')
DMG_OUTPUT="build/HarmonyCast-${VERSION}-macOS-arm64.dmg"
python3 -c "import os; os.remove('$DMG_OUTPUT') if os.path.exists('$DMG_OUTPUT') else None"

echo "=== 9. Creating fancy DMG with create-dmg ==="
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

python3 -c "import shutil, os; shutil.rmtree('$STAGING_DIR', ignore_errors=True)"
python3 -c "import shutil, os; shutil.rmtree('build/test_bundle', ignore_errors=True)"

echo "=== DMG Build Succeeded: $DMG_OUTPUT ==="
ls -lh "$DMG_OUTPUT"
