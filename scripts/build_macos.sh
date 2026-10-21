#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "$(uname -m)" != "arm64" ]]; then
  echo "此构建脚本适用于 Apple Silicon Mac。" >&2
  exit 1
fi
if [[ ! -x .venv/bin/python ]]; then
  python3.12 -m venv .venv
  .venv/bin/python -m pip install -r requirements-dev.txt
fi
.venv/bin/python -m pytest -q
.venv/bin/python scripts/make_icon.py
iconutil -c icns build/HarmonyCast.iconset -o resources/app-icon.icns
.venv/bin/python -m PyInstaller --noconfirm HarmonyCast.spec
codesign --verify --deep --strict dist/HarmonyCast.app
ditto -c -k --sequesterRsrc --keepParent dist/HarmonyCast.app dist/HarmonyCast-macOS-arm64.zip
shasum -a 256 dist/HarmonyCast-macOS-arm64.zip > dist/SHA256SUMS.txt
echo "已生成 dist/HarmonyCast.app 和 dist/HarmonyCast-macOS-arm64.zip"
