`video-64x48.h264` is a synthetic FFmpeg test pattern (four intra frames), generated locally without device data:

```sh
ffmpeg -f lavfi -i 'testsrc2=size=64x48:rate=10' -frames:v 4 \
  -c:v libx264 -preset ultrafast -tune zerolatency \
  -x264-params 'keyint=1:repeat-headers=1:aud=1' \
  -pix_fmt yuv420p -f h264 video-64x48.h264
```

The regression suite uses it to check hardware/software decoding, frame ownership,
buffer reuse, and safe decoder shutdown while frames remain visible.
