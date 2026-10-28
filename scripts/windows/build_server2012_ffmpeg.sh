#!/usr/bin/env bash
# Run in MSYS2 with the MSVC v142 x64 developer environment inherited.
set -euo pipefail

task_source="$(cygpath -u "${1:?FFmpeg 6.1.3 source directory required}")"
task_prefix="$(cygpath -u "${2:?Install prefix required}")"
task_build="$(cygpath -u "${3:?Build directory required}")"
task_msvc="$(cygpath -u "${VCToolsInstallDir:?MSVC v142 developer environment required}")/bin/Hostx64/x64"
# MSYS also ships a link.exe; FFmpeg must find Microsoft's linker first.
export PATH="$task_msvc:$PATH"
mkdir -p "$task_build"
cd "$task_build"

"$task_source/configure" \
    --prefix="$task_prefix" --toolchain=msvc --arch=x86_64 --target-os=win32 \
    --enable-shared --disable-static --disable-programs --disable-doc --disable-debug \
    --disable-autodetect --disable-everything --disable-network \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-postproc --disable-swresample \
    --disable-d3d11va --disable-dxva2 --disable-vulkan \
    --enable-decoder=h264 --enable-parser=h264 --enable-swscale \
    --extra-cflags="-D_WIN32_WINNT=0x0602 -DWINVER=0x0602" \
    --extra-ldflags="-SUBSYSTEM:CONSOLE,6.02"
make -j"${NUMBER_OF_PROCESSORS:-2}"
make install
