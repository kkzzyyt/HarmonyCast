#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
capture_sdk="${HARMONYCAST_SDK:-/Applications/DevEco-Studio6.app/Contents/sdk/default/openharmony/native}"
for capture_arch in arm64 armv7; do
    capture_flags=(-march=armv8-a)
    if [ "$capture_arch" = arm64 ]; then capture_target=aarch64-linux-ohos; else capture_target=arm-linux-ohos; fi
    if [ "$capture_arch" = armv7 ]; then capture_flags=(-march=armv7-a -mfloat-abi=softfp); fi
    "$capture_sdk/llvm/bin/clang" --target="$capture_target" --sysroot="$capture_sdk/sysroot" \
        "${capture_flags[@]}" \
        -std=c11 -O2 -Wall -Wextra -Werror -fPIE -pie \
        -I"$capture_sdk/sysroot/usr/include/multimedia/player_framework" \
        -I"$capture_sdk/sysroot/usr/include/native_buffer" \
        native/capture_server_api10.c -lnative_avscreen_capture -lnative_buffer -lc \
        -o "resources/capture/harmonycast_capture_api10_$capture_arch"
done
python3 scripts/check_capture_api10.py
