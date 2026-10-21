/* HarmonyCast continuous capture service. Uses OpenHarmony public C APIs.
 * No system installation or persistent service is required.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <native_avscreen_capture.h>
#include <native_avbuffer.h>
#include <native_buffer.h>
#include <oh_display_manager.h>
#include <native_avcodec_videoencoder.h>
#include <native_avformat.h>

static _Atomic int stopped;
static volatile int sending;
static int client = -1;
static int source_width, source_height;
static int display_rotation;
static int target_fps = 30;
static int target_bitrate = 4000000;
static int64_t last_timestamp;
static _Atomic unsigned long frame_count;
static int max_size = 0;

static void stop_signal(int number) { stopped = 1; }

static int send_all(const void *data, size_t length) {
    const char *position = data;
    int retry_eagain = 0;
    while (length && !stopped) {
        ssize_t count = send(client, position, length, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++retry_eagain < 200 && !stopped) {
                usleep(20000);
                continue;
            }
        }
        if (count <= 0) { stopped = 1; return -1; }
        position += count;
        length -= (size_t)count;
        retry_eagain = 0;
    }
    return length == 0 ? 0 : -1;
}

static void send_packet(uint32_t type, const void *data, uint32_t size) {
    while (__sync_lock_test_and_set(&sending, 1)) {
        if (stopped) return;
        usleep(1000);
    }
    uint32_t header[] = { htonl(type), htonl(size) };
    if (send_all(header, sizeof(header)) == 0 && size) send_all(data, size);
    __sync_lock_release(&sending);
}

static int start_code(const uint8_t *data, int size, int offset) {
    if (offset + 3 < size && !data[offset] && !data[offset + 1] && !data[offset + 2] && data[offset + 3] == 1) return 4;
    if (offset + 2 < size && !data[offset] && !data[offset + 1] && data[offset + 2] == 1) return 3;
    return 0;
}

static void encoder_error(OH_AVCodec *codec, int32_t code, void *context) {
    fprintf(stderr, "Video encoder error: %d\n", code);
    if (code == 10) {
        return;
    }
    stopped = 1;
}

static void encoder_changed(OH_AVCodec *codec, OH_AVFormat *format, void *context) {}
static void encoder_input(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *context) {}

static void encoder_output(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *context) {
    OH_AVCodecBufferAttr attr = {0};
    if (stopped || !buffer || OH_AVBuffer_GetBufferAttr(buffer, &attr) != 0) goto release;
    uint8_t *address = OH_AVBuffer_GetAddr(buffer);
    int capacity = OH_AVBuffer_GetCapacity(buffer);
    if (!address || attr.offset < 0 || attr.size <= 0 || attr.size > capacity - attr.offset) goto release;
    const uint8_t *data = address + attr.offset;
    int picture = 0;
    for (int position = 0; position + 3 < attr.size;) {
        int prefix = start_code(data, attr.size, position);
        if (!prefix) { ++position; continue; }
        int next = position + prefix + 1;
        while (next < attr.size && !start_code(data, attr.size, next)) ++next;
        int type = data[position + prefix] & 31;
        if (type == 7) send_packet(1, data + position, next - position);
        if (type == 8) send_packet(2, data + position, next - position);
        if (type == 1 || type == 5) picture = 1;
        position = next;
    }
    if (picture) {
        send_packet((attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) ? 3 : 4, data, attr.size);
        ++frame_count;
    }
release:
    OH_VideoEncoder_FreeOutputBuffer(codec, index);
}

static OH_AVCodec *create_encoder(int width, int height, OHNativeWindow **surface) {
    OH_AVCodec *codec = OH_VideoEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    if (!codec) return NULL;
    OH_AVCodecCallback callback = { encoder_error, encoder_changed, encoder_input, encoder_output };
    int result = OH_VideoEncoder_RegisterCallback(codec, callback, NULL);
    OH_AVFormat *format = OH_AVFormat_Create();
    if (!format) { OH_VideoEncoder_Destroy(codec); return NULL; }
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height);
    OH_AVFormat_SetDoubleValue(format, OH_MD_KEY_FRAME_RATE, target_fps);
    OH_AVFormat_SetLongValue(format, OH_MD_KEY_BITRATE, target_bitrate);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_RGBA);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_I_FRAME_INTERVAL, 1000);
    if (result == 0) result = OH_VideoEncoder_Configure(codec, format);
    OH_AVFormat_Destroy(format);
    if (result == 0) result = OH_VideoEncoder_GetSurface(codec, surface);
    if (result == 0) result = OH_VideoEncoder_Prepare(codec);
    if (result != 0) {
        fprintf(stderr, "Video encoder unavailable: %d, using RGBA transport\n", result);
        OH_VideoEncoder_Destroy(codec);
        *surface = NULL;
        return NULL;
    }
    return codec;
}

static void on_data(OH_AVScreenCapture *capture, OH_AVBuffer *buffer,
                    OH_AVScreenCaptureBufferType type, int64_t timestamp, void *context) {
    if (type != OH_SCREEN_CAPTURE_BUFFERTYPE_VIDEO || stopped) return;
    if (__sync_lock_test_and_set(&sending, 1)) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t timestamp_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    if (last_timestamp && timestamp_ns - last_timestamp < 1000000000LL / target_fps) goto done;
    OH_NativeBuffer *native = OH_AVBuffer_GetNativeBuffer(buffer);
    void *pixels = NULL;
    OH_NativeBuffer_Config config = {0};
    if (!native) goto done;
    OH_NativeBuffer_GetConfig(native, &config);
    if (config.format != 12 || config.width <= 0 || config.height <= 0 ||
        config.width > 4096 || config.height > 4096 ||
        config.stride < config.width * 4 || config.stride > config.width * 4 + 4096 ||
        (int64_t)config.stride * config.height > 16 * 1024 * 1024) {
        fprintf(stderr, "Unsupported capture buffer: %dx%d stride=%d format=%d\n",
                config.width, config.height, config.stride, config.format);
        stopped = 1;
        goto done;
    }
    if (OH_NativeBuffer_Map(native, &pixels) != 0 || !pixels) goto done;
    uint32_t header[8] = {
        htonl(0x48435232), htonl(config.width), htonl(config.height), htonl(config.stride),
        htonl(config.stride * config.height), htonl(source_width), htonl(source_height), htonl(display_rotation)
    };
    if (send_all(header, sizeof(header)) == 0 && send_all(pixels, (size_t)config.stride * config.height) == 0) {
        last_timestamp = timestamp_ns;
        ++frame_count;
    }
    OH_NativeBuffer_Unmap(native);
done:
    __sync_lock_release(&sending);
}

static void on_error(OH_AVScreenCapture *capture, int32_t code, void *context) {
    fprintf(stderr, "Native capture error: %d\n", code);
    stopped = 1;
}

static void on_state(OH_AVScreenCapture *capture, OH_AVScreenCaptureStateCode code, void *context) {
    fprintf(stderr, "Native capture state: %d\n", code);
    if (code == OH_SCREEN_CAPTURE_STATE_CANCELED || code == OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER ||
        code == OH_SCREEN_CAPTURE_STATE_STOPPED_BY_CALL || code == OH_SCREEN_CAPTURE_STATE_INTERRUPTED_BY_OTHER) stopped = 1;
}

int main(int argc, char **argv) {
    int port = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-p")) port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-f")) target_fps = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-b")) target_bitrate = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-m")) max_size = atoi(argv[i + 1]);
        else return 2;
    }
    if (port < 1024 || port > 65535 || target_fps < 1 || target_fps > 60 ||
        target_bitrate < 100000 || target_bitrate > 20000000 || max_size < 0 || max_size > 8192) return 2;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    if (OH_NativeDisplayManager_GetDefaultDisplayWidth(&source_width) != 0 ||
        OH_NativeDisplayManager_GetDefaultDisplayHeight(&source_height) != 0 ||
        source_width < 1 || source_height < 1 || source_width > 8192 || source_height > 8192) {
        fprintf(stderr, "Unable to read the device display size\n");
        return 3;
    }
    /* This device's capture API uses the native display canvas. Capture its
     * full size, then report rotation so the desktop can present logical pixels. */
    int width = source_width, height = source_height;
    NativeDisplayManager_Rotation rotation = DISPLAY_MANAGER_ROTATION_0;
    OH_NativeDisplayManager_GetDefaultDisplayRotation(&rotation);
    display_rotation = rotation;
    fprintf(stderr, "Display size %dx%d rotation %d\n", source_width, source_height, display_rotation);
    if (display_rotation % 2) {
        int temporary_width = width;
        width = height;
        height = temporary_width;
    }
    /* The device encoder reads RGBA with a 64-byte row alignment. Matching
     * capture width to that alignment prevents diagonal stride corruption. */
    width = (width + 15) & ~15;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 4;
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 1)) {
        perror("capture socket"); close(listener); return 4;
    }
    fprintf(stderr, "Native capture ready: %dx%d -> %dx%d, %d FPS\n", source_width, source_height, width, height, target_fps);
    struct pollfd incoming = { .fd = listener, .events = POLLIN };
    if (poll(&incoming, 1, 15000) <= 0 || stopped) { close(listener); return 5; }
    client = accept(listener, NULL, NULL);
    close(listener);
    if (client < 0) return 5;
    struct timeval send_timeout = { .tv_sec = 8, .tv_usec = 0 };
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    int sndbuf = 2 * 1024 * 1024;
    setsockopt(client, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    OHNativeWindow *surface = NULL;
    OH_AVCodec *encoder = create_encoder(width, height, &surface);
    if (encoder) {
        char greeting[128];
        int size = snprintf(greeting, sizeof(greeting), "SCREEN_INFO:%d:%d:%d:%d:h264\n", width, height, target_fps, target_bitrate);
        send_all(greeting, size);
        char ack[32] = {0};
        struct pollfd handshake = { .fd = client, .events = POLLIN };
        int offset = 0;
        while (offset < (int)sizeof(ack) - 1 && poll(&handshake, 1, 5000) > 0) {
            if (recv(client, ack + offset, 1, 0) <= 0) break;
            if (ack[offset++] == '\n') break;
        }
        if (strcmp(ack, "CONFIG_ACK\n")) {
            fprintf(stderr, "Client configuration acknowledgment failed\n");
            OH_VideoEncoder_Destroy(encoder); close(client); return 7;
        }
        uint32_t dimensions[] = { htonl(width), htonl(height), htonl(target_fps), htonl(target_bitrate), htonl(source_width), htonl(source_height), htonl(display_rotation) };
        send_packet(5, dimensions, sizeof(dimensions));
        fprintf(stderr, "Using H.264 video transport\n");
    }
    OH_AVScreenCapture *capture = OH_AVScreenCapture_Create();
    if (!capture) { if (encoder) OH_VideoEncoder_Destroy(encoder); close(client); return 6; }
    OH_AVScreenCaptureConfig config = {0};
    config.captureMode = OH_CAPTURE_HOME_SCREEN;
    config.dataType = OH_ORIGINAL_STREAM;
    config.videoInfo.videoCapInfo.videoFrameWidth = width;
    config.videoInfo.videoCapInfo.videoFrameHeight = height;
    config.videoInfo.videoCapInfo.videoSource = OH_VIDEO_SOURCE_SURFACE_RGBA;
    int result = OH_AVScreenCapture_Init(capture, config);
    if (result == 0) {
        int rotation_result = OH_AVScreenCapture_SetCanvasRotation(capture, false);
        fprintf(stderr, "Capture canvas rotation: %d\n", rotation_result);
    }
    if (result == 0) result = OH_AVScreenCapture_SetMicrophoneEnabled(capture, false);
    if (result == 0 && !encoder) result = OH_AVScreenCapture_SetDataCallback(capture, on_data, NULL);
    if (result == 0) result = OH_AVScreenCapture_SetErrorCallback(capture, on_error, NULL);
    if (result == 0) result = OH_AVScreenCapture_SetStateCallback(capture, on_state, NULL);
    if (result == 0 && encoder) result = OH_VideoEncoder_Start(encoder);
    if (result == 0) result = encoder ? OH_AVScreenCapture_StartScreenCaptureWithSurface(capture, surface) : OH_AVScreenCapture_StartScreenCapture(capture);
    if (result != 0) {
        fprintf(stderr, "Capture initialization failed: %d\n", result);
    } else {
        struct pollfd state = { .fd = client, .events = POLLIN | POLLHUP | POLLERR };
        while (!stopped) {
            int available = poll(&state, 1, 250);
            if (available > 0) {
                char bytes[128];
                if (state.revents & (POLLHUP | POLLERR)) break;
                if (state.revents & POLLIN) {
                    ssize_t r = recv(client, bytes, sizeof(bytes), 0);
                    if (r == 0) break;
                    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
                    if (encoder) send_packet(0, NULL, 0);
                }
            }
        }
        OH_AVScreenCapture_StopScreenCapture(capture);
    }
    stopped = 1;
    shutdown(client, SHUT_RDWR);
    OH_AVScreenCapture_Release(capture);
    if (encoder) { OH_VideoEncoder_Stop(encoder); OH_VideoEncoder_Destroy(encoder); }
    close(client);
    unsigned long frames_sent = frame_count;
    fprintf(stderr, "Native capture stopped after %lu frames\n", frames_sent);
    return result == 0 ? 0 : 6;
}
