#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
task_sdk="${HARMONYCAST_SDK:-/Applications/DevEco-Studio6.app/Contents/sdk/default/openharmony/native}"
task_libraries="${1:?传入用于链接的 RISC-V 系统库目录}"
mkdir -p resources/capture
"$task_sdk/llvm/bin/clang" --target=riscv64-linux-ohos -march=rv64gc -mabi=lp64d \
  --sysroot="$task_sdk/sysroot" \
  -isystem "$task_sdk/sysroot/usr/include/aarch64-linux-ohos" \
  -I"$task_sdk/sysroot/usr/include/multimedia/player_framework" \
  -I"$task_sdk/sysroot/usr/include/native_buffer" \
  -I"$task_sdk/sysroot/usr/include/native_window" \
  -I"$task_sdk/sysroot/usr/include/window_manager" \
  -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-ignored-attributes -fPIC -pie -nostdlib -Wl,-e,_start \
  -Wl,--dynamic-linker=/system/lib/ld-musl-riscv64.so.1 -Wl,--allow-shlib-undefined \
  -Wl,-rpath,/system/lib64:/system/lib64/platformsdk:/system/lib64/ndk \
  -L"$task_libraries" -lnative_avscreen_capture -lnative_media_core -lnative_media_venc -lnative_media_codecbase -lnative_buffer -lnative_display_manager -lc \
  native/riscv_start.S native/riscv_start.c native/capture_server.c \
  -o resources/capture/harmonycast_capture_riscv64
# The available SDK has no RISC-V libc headers. Only architecture-neutral C API
# types and musl LP64 scalar definitions are used; no ABI-specific structs are read.
