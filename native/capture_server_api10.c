/* API 10 continuous capture. No API 12 display, AVBuffer or Surface imports. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef HC_CAPTURE_API10_TEST
#include "capture_api10_stubs.h"
#else
#include <native_avscreen_capture.h>
#include <native_buffer.h>
#endif

static atomic_int stopped;
static atomic_flag reading = ATOMIC_FLAG_INIT;
static int client = -1, source_width, source_height, fps = 10, max_size = 1280;
static int64_t last_frame;
static unsigned long frames_sent;
static uint8_t *scratch;
static size_t scratch_size;

static void stop_signal(int number) { (void)number; atomic_store(&stopped, 1); }

static int send_all(const void *data, size_t size) {
    const char *position = data;
    while (size && !atomic_load(&stopped)) {
        const ssize_t count = send(client, position, size, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { atomic_store(&stopped, 1); return -1; }
        position += count; size -= (size_t)count;
    }
    return size ? -1 : 0;
}

static void capture_error(OH_AVScreenCapture *capture, int32_t code) {
    (void)capture;
    fprintf(stderr, "API 10 capture error: %d\n", code);
    atomic_store(&stopped, 1);
}

static void audio_ready(OH_AVScreenCapture *capture, bool ready, OH_AudioCaptureSourceType type) {
    (void)capture; (void)ready; (void)type;
}

static void video_ready(OH_AVScreenCapture *capture, bool ready) {
    if (!ready || atomic_load(&stopped) || atomic_flag_test_and_set(&reading)) return;
    int32_t fence = -1;
    int64_t timestamp = 0;
    struct OH_Rect region = {0};
    OH_NativeBuffer *buffer = OH_AVScreenCapture_AcquireVideoBuffer(capture, &fence, &timestamp, &region);
    void *pixels = NULL;
    if (!buffer) goto done;
    if (fence >= 0) {
        struct pollfd sync = { .fd = fence, .events = POLLIN };
        int status;
        do { status = poll(&sync, 1, 500); } while (status < 0 && errno == EINTR && !atomic_load(&stopped));
        close(fence); fence = -1;
        if (status <= 0 || !(sync.revents & POLLIN)) goto release;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) goto release;
    const int64_t tick = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    if (last_frame && tick - last_frame < 1000000000LL / fps) goto release;
    OH_NativeBuffer_Config config = {0};
    OH_NativeBuffer_GetConfig(buffer, &config);
    // Pixel format values are stable since API 9: RGBX/RGBA/BGRX/BGRA.
    if (config.width < 1 || config.height < 1 || config.width > 8192 || config.height > 8192 ||
        config.stride < config.width * 4 || (int64_t)config.stride * config.height > 64 * 1024 * 1024 ||
        (config.format != 11 && config.format != 12 && config.format != 19 && config.format != 20)) {
        fprintf(stderr, "Unsupported API 10 buffer: %dx%d stride=%d format=%d\n",
                config.width, config.height, config.stride, config.format);
        atomic_store(&stopped, 1); goto release;
    }
    if (OH_NativeBuffer_Map(buffer, &pixels) != 0) goto release;
    if (!pixels) goto unmap;
    int width = config.width, height = config.height;
    const int edge = width > height ? width : height;
    if (edge > max_size) { width = width * max_size / edge; height = height * max_size / edge; }
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    const size_t size = (size_t)width * height * 4;
    if (scratch_size < size) {
        uint8_t *next = realloc(scratch, size);
        if (!next) { atomic_store(&stopped, 1); goto unmap; }
        scratch = next; scratch_size = size;
    }
    const int blue_first = config.format == 19 || config.format == 20;
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = (const uint8_t *)pixels + (size_t)(y * config.height / height) * config.stride;
        for (int x = 0; x < width; ++x) {
            const uint8_t *pixel = row + (size_t)(x * config.width / width) * 4;
            uint8_t *out = scratch + ((size_t)y * width + x) * 4;
            out[0] = pixel[blue_first ? 2 : 0]; out[1] = pixel[1]; out[2] = pixel[blue_first ? 0 : 2]; out[3] = 255;
        }
    }
    const uint32_t header[] = { htonl(0x48435232), htonl(width), htonl(height), htonl(width * 4),
        htonl((uint32_t)size), htonl(source_width), htonl(source_height), 0 };
    if (!send_all(header, sizeof(header)) && !send_all(scratch, size)) { last_frame = tick; ++frames_sent; }
unmap:
    OH_NativeBuffer_Unmap(buffer);
release:
    OH_AVScreenCapture_ReleaseVideoBuffer(capture);
done:
    if (fence >= 0) close(fence);
    atomic_flag_clear(&reading);
}

static int number(const char *text) {
    char *end = NULL;
    errno = 0;
    const long value = strtol(text, &end, 10);
    return errno || !*text || *end || value < 0 || value > INT_MAX ? -1 : (int)value;
}

int main(int argc, char **argv) {
    int port = 0, bitrate = 0;
    if (!(argc & 1)) return 2;
    for (int i = 1; i + 1 < argc; i += 2) {
        const int value = number(argv[i + 1]);
        if (value < 0) return 2;
        if (!strcmp(argv[i], "-p")) port = value;
        else if (!strcmp(argv[i], "-w")) source_width = value;
        else if (!strcmp(argv[i], "-h")) source_height = value;
        else if (!strcmp(argv[i], "-f")) fps = value;
        else if (!strcmp(argv[i], "-b")) bitrate = value;
        else if (!strcmp(argv[i], "-m")) max_size = value;
        else return 2;
    }
    if (port < 1024 || port > 65535 || source_width < 1 || source_height < 1 || source_width > 8192 || source_height > 8192 ||
        fps < 1 || fps > 60 || bitrate < 0 || bitrate > 20000000 || max_size < 1 || max_size > 8192) return 2;
    if (fps > 10) fps = 10;
    if (max_size > 1280) max_size = 1280;
    signal(SIGPIPE, SIG_IGN); signal(SIGTERM, stop_signal); signal(SIGINT, stop_signal);
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 4;
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(0x7f000001U);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 1)) { close(listener); return 4; }
    fprintf(stderr, "API 10 RGBA capture ready: %dx%d, up to %d FPS, max edge %d\n", source_width, source_height, fps, max_size);
    struct pollfd incoming = { .fd = listener, .events = POLLIN };
    if (poll(&incoming, 1, 15000) <= 0 || atomic_load(&stopped)) { close(listener); return 5; }
    client = accept(listener, NULL, NULL); close(listener);
    if (client < 0) return 5;
    struct timeval timeout = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    OH_AVScreenCapture *capture = OH_AVScreenCapture_Create();
    if (!capture) { close(client); return 6; }
    const OH_AVScreenCaptureCallback callbacks = { capture_error, audio_ready, video_ready };
    int result = OH_AVScreenCapture_SetCallback(capture, callbacks);
    OH_AVScreenCaptureConfig config = {0};
    config.captureMode = OH_CAPTURE_HOME_SCREEN; config.dataType = OH_ORIGINAL_STREAM;
    config.videoInfo.videoCapInfo.videoFrameWidth = source_width;
    config.videoInfo.videoCapInfo.videoFrameHeight = source_height;
    config.videoInfo.videoCapInfo.videoSource = OH_VIDEO_SOURCE_SURFACE_RGBA;
    if (!result) result = OH_AVScreenCapture_Init(capture, config);
    if (!result) result = OH_AVScreenCapture_SetMicrophoneEnabled(capture, false);
    if (!result) result = OH_AVScreenCapture_StartScreenCapture(capture);
    const int started = result == 0;
    if (result) fprintf(stderr, "API 10 capture initialization failed: %d\n", result);
    while (!result && !atomic_load(&stopped)) {
        struct pollfd connection = { .fd = client, .events = POLLIN | POLLHUP | POLLERR };
        if (poll(&connection, 1, 250) > 0) {
            if (connection.revents & (POLLHUP | POLLERR)) break;
            if (connection.revents & POLLIN) {
                char data[128];
                if (recv(client, data, sizeof(data), 0) <= 0) break;
            }
        }
    }
    atomic_store(&stopped, 1); shutdown(client, SHUT_RDWR);
    if (started) OH_AVScreenCapture_StopScreenCapture(capture);
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
    while (atomic_flag_test_and_set(&reading)) nanosleep(&pause, NULL);
    OH_AVScreenCapture_Release(capture);
    free(scratch); close(client);
    fprintf(stderr, "API 10 capture stopped after %lu frames\n", frames_sent);
    return result ? 6 : 0;
}
