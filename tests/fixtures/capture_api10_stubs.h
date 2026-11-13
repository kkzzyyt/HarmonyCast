#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct OH_AVScreenCapture { int unused; } OH_AVScreenCapture;
typedef struct OH_NativeBuffer { int unused; } OH_NativeBuffer;
typedef int OH_AudioCaptureSourceType;
typedef struct { int32_t width, height, format, usage, stride; } OH_NativeBuffer_Config;
struct OH_Rect { int32_t x, y, width, height; };
typedef struct {
    void (*onError)(OH_AVScreenCapture *, int32_t);
    void (*onAudioBufferAvailable)(OH_AVScreenCapture *, bool, OH_AudioCaptureSourceType);
    void (*onVideoBufferAvailable)(OH_AVScreenCapture *, bool);
} OH_AVScreenCaptureCallback;
typedef struct {
    int captureMode, dataType;
    struct { int unused[8]; } audioInfo;
    struct { struct { int videoFrameWidth, videoFrameHeight, videoSource; } videoCapInfo; } videoInfo;
} OH_AVScreenCaptureConfig;
enum { OH_CAPTURE_HOME_SCREEN = 0, OH_ORIGINAL_STREAM = 0, OH_VIDEO_SOURCE_SURFACE_RGBA = 0 };
OH_AVScreenCapture *OH_AVScreenCapture_Create(void);
int OH_AVScreenCapture_SetCallback(OH_AVScreenCapture *, OH_AVScreenCaptureCallback);
int OH_AVScreenCapture_Init(OH_AVScreenCapture *, OH_AVScreenCaptureConfig);
int OH_AVScreenCapture_SetMicrophoneEnabled(OH_AVScreenCapture *, bool);
int OH_AVScreenCapture_StartScreenCapture(OH_AVScreenCapture *);
int OH_AVScreenCapture_StopScreenCapture(OH_AVScreenCapture *);
int OH_AVScreenCapture_Release(OH_AVScreenCapture *);
OH_NativeBuffer *OH_AVScreenCapture_AcquireVideoBuffer(OH_AVScreenCapture *, int32_t *, int64_t *, struct OH_Rect *);
int OH_AVScreenCapture_ReleaseVideoBuffer(OH_AVScreenCapture *);
void OH_NativeBuffer_GetConfig(OH_NativeBuffer *, OH_NativeBuffer_Config *);
int OH_NativeBuffer_Map(OH_NativeBuffer *, void **);
int OH_NativeBuffer_Unmap(OH_NativeBuffer *);
