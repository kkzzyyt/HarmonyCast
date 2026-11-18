#!/bin/bash
# Shared H.264 decoder libraries without Homebrew runtime dependencies.
set -euo pipefail
cd "$(dirname "$0")/.."
ffmpeg_source="${1:?FFmpeg 6.1.3 source directory required}"
ffmpeg_prefix="${2:?Install prefix required}"
ffmpeg_build="${3:?Build directory required}"
ffmpeg_arch="${4:-$(uname -m)}"
ffmpeg_target="${MACOSX_DEPLOYMENT_TARGET:-15.0}"
case "$ffmpeg_arch" in arm64|x86_64) ;; *) echo "Unsupported macOS architecture: $ffmpeg_arch" >&2; exit 1 ;; esac
mkdir -p "$ffmpeg_build" "$ffmpeg_prefix"
ffmpeg_source="$(cd "$ffmpeg_source" && pwd)"
ffmpeg_prefix="$(cd "$ffmpeg_prefix" && pwd)"
cd "$ffmpeg_build"
export MACOSX_DEPLOYMENT_TARGET="$ffmpeg_target"
"$ffmpeg_source/configure" \
    --prefix="$ffmpeg_prefix" --arch="$ffmpeg_arch" --target-os=darwin \
    --enable-shared --disable-static --disable-programs --disable-doc --disable-debug \
    --disable-autodetect --disable-everything --disable-network \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-postproc --disable-swresample \
    --enable-decoder=h264 --enable-parser=h264 --enable-swscale \
    --enable-videotoolbox --enable-hwaccel=h264_videotoolbox \
    --extra-cflags="-arch $ffmpeg_arch -mmacosx-version-min=$ffmpeg_target" \
    --extra-ldflags="-arch $ffmpeg_arch -mmacosx-version-min=$ffmpeg_target"
make -j"$(sysctl -n hw.ncpu)"
make install
