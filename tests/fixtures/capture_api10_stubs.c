#include "capture_api10_stubs.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static OH_AVScreenCapture capture;
static OH_NativeBuffer buffer;
static OH_AVScreenCaptureCallback callbacks;
static int acquired, released, mapped, unmapped, fence = -1, fence_writer = -1;
static unsigned char pixels[24] = {255,0,0,0, 0,255,0,0, 123,123,123,123, 0,0,255,0, 255,255,255,0, 123,123,123,123};
static int mode(const char *value) { const char *actual = getenv("HC_API10_MODE"); return actual && !strcmp(actual, value); }
OH_AVScreenCapture *OH_AVScreenCapture_Create(void) { return mode("create-fail") ? NULL : &capture; }
int OH_AVScreenCapture_SetCallback(OH_AVScreenCapture *c, OH_AVScreenCaptureCallback cb) {
    (void)c; callbacks = cb; return mode("callback-fail") ? -1 : 0;
}
int OH_AVScreenCapture_Init(OH_AVScreenCapture *c, OH_AVScreenCaptureConfig config) {
    (void)c;
    assert(callbacks.onError && callbacks.onVideoBufferAvailable && callbacks.onAudioBufferAvailable);
    assert(config.videoInfo.videoCapInfo.videoFrameWidth == 720 && config.videoInfo.videoCapInfo.videoFrameHeight == 1280);
    for (unsigned i = 0; i < sizeof(config.audioInfo.unused)/sizeof(int); ++i) assert(!config.audioInfo.unused[i]);
    return mode("init-fail") ? -2 : 0;
}
int OH_AVScreenCapture_SetMicrophoneEnabled(OH_AVScreenCapture *c, bool enabled) { (void)c; assert(!enabled); return mode("mic-fail") ? -3 : 0; }
int OH_AVScreenCapture_StartScreenCapture(OH_AVScreenCapture *c) {
    if (mode("start-fail")) return -4;
    if (mode("error-callback")) { callbacks.onError(c, 999); return 0; }
    callbacks.onAudioBufferAvailable(c, false, 0);
    callbacks.onVideoBufferAvailable(c, false);
    callbacks.onVideoBufferAvailable(c, true);
    if (mode("rate-limit")) callbacks.onVideoBufferAvailable(c, true);
    return 0;
}
int OH_AVScreenCapture_StopScreenCapture(OH_AVScreenCapture *c) { (void)c; return 0; }
int OH_AVScreenCapture_Release(OH_AVScreenCapture *c) {
    (void)c;
    assert(acquired == released);
    assert(mapped == unmapped);
    if (fence >= 0) assert(fcntl(fence, F_GETFD) == -1);
    if (fence_writer >= 0) close(fence_writer);
    fprintf(stderr, "leases=%d/%d maps=%d/%d\n", acquired, released, mapped, unmapped);
    return 0;
}
OH_NativeBuffer *OH_AVScreenCapture_AcquireVideoBuffer(OH_AVScreenCapture *c, int32_t *fd, int64_t *timestamp, struct OH_Rect *region) {
    (void)c; (void)timestamp; (void)region;
    if (mode("no-buffer")) return NULL;
    ++acquired;
    if (mode("fence-ready") || mode("fence-timeout")) {
        int descriptors[2]; assert(!pipe(descriptors));
        *fd = fence = descriptors[0]; fence_writer = descriptors[1];
        if (mode("fence-ready")) assert(write(fence_writer, "x", 1) == 1);
    }
    return &buffer;
}
int OH_AVScreenCapture_ReleaseVideoBuffer(OH_AVScreenCapture *c) { (void)c; ++released; return 0; }
void OH_NativeBuffer_GetConfig(OH_NativeBuffer *b, OH_NativeBuffer_Config *config) {
    (void)b;
    *config = (OH_NativeBuffer_Config){2, 2, 12, 0, 12};
    if (mode("bgra")) { config->format = 20; pixels[0] = 0; pixels[2] = 255; }
    if (mode("rgbx")) config->format = 11;
    if (mode("bad-format")) config->format = 3;
    if (mode("bad-stride")) config->stride = 1;
    if (mode("oversize")) config->height = 8193;
}
int OH_NativeBuffer_Map(OH_NativeBuffer *b, void **data) { (void)b; if (mode("map-fail")) return -1; ++mapped; *data = mode("map-null") ? NULL : pixels; return 0; }
int OH_NativeBuffer_Unmap(OH_NativeBuffer *b) { (void)b; ++unmapped; return 0; }
