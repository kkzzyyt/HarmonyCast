#include "harmonycast/VideoDecoder.h"
#include <iostream>
#include <thread>
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace harmonycast {

static enum AVPixelFormat s_hw_pix_fmt = AV_PIX_FMT_NONE;

static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    const enum AVPixelFormat* p;
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == s_hw_pix_fmt) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

VideoDecoder::VideoDecoder() {
    m_av_frame = av_frame_alloc();
    m_sw_frame = av_frame_alloc();
    m_av_packet = av_packet_alloc();
}

VideoDecoder::~VideoDecoder() {
    close();
    if (m_av_frame) av_frame_free(&m_av_frame);
    if (m_sw_frame) av_frame_free(&m_sw_frame);
    if (m_av_packet) av_packet_free(&m_av_packet);
}

void VideoDecoder::close() {
    if (m_sws_ctx) {
        sws_freeContext(m_sws_ctx);
        m_sws_ctx = nullptr;
    }
    if (m_codec_ctx) {
        avcodec_free_context(&m_codec_ctx);
        m_codec_ctx = nullptr;
    }
    if (m_hw_device_ctx) {
        av_buffer_unref(&m_hw_device_ctx);
        m_hw_device_ctx = nullptr;
    }
    m_hw_active = false;
    m_decoded_count = 0;
}

bool VideoDecoder::initHwContext() {
#if defined(__APPLE__)
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
    s_hw_pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
#elif defined(_WIN32)
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_D3D11VA;
    s_hw_pix_fmt = AV_PIX_FMT_D3D11;
#else
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_VAAPI;
    s_hw_pix_fmt = AV_PIX_FMT_VAAPI;
#endif

    int ret = av_hwdevice_ctx_create(&m_hw_device_ctx, type, nullptr, nullptr, 0);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        std::cerr << "[VideoDecoder] Hardware accel init failed (" << err << "), falling back to CPU." << std::endl;
        return false;
    }
    return true;
}

bool VideoDecoder::init(int width, int height, bool enable_hardware) {
    close();
    m_width = width;
    m_height = height;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        std::cerr << "[VideoDecoder] H.264 decoder not found in FFmpeg!" << std::endl;
        return false;
    }

    m_codec_ctx = avcodec_alloc_context3(codec);
    if (!m_codec_ctx) return false;

    // Fast decoding settings
    m_codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (enable_hardware && initHwContext()) {
        m_codec_ctx->hw_device_ctx = av_buffer_ref(m_hw_device_ctx);
        m_codec_ctx->get_format = get_hw_format;
        m_hw_active = true;
    } else {
        // Multi-threaded CPU decoding
        unsigned int cores = std::thread::hardware_concurrency();
        m_codec_ctx->thread_count = std::clamp(cores, 2u, 4u);
        m_codec_ctx->thread_type = FF_THREAD_SLICE;
        m_hw_active = false;
    }

    if (avcodec_open2(m_codec_ctx, codec, nullptr) < 0) {
        close();
        return false;
    }

    return true;
}

bool VideoDecoder::decode(std::span<const uint8_t> data, bool is_keyframe, int64_t pts) {
    if (!m_codec_ctx || data.empty()) return false;

    m_av_packet->data = const_cast<uint8_t*>(data.data());
    m_av_packet->size = static_cast<int>(data.size());
    m_av_packet->pts = pts;
    if (is_keyframe) {
        m_av_packet->flags |= AV_PKT_FLAG_KEY;
    }

    int ret = avcodec_send_packet(m_codec_ctx, m_av_packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        return false;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(m_codec_ctx, m_av_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            return false;
        }

        emitFrame(m_av_frame);
    }

    return true;
}

void VideoDecoder::emitFrame(AVFrame* frame) {
    if (!m_frame_cb) return;

    DecodedFrame out;
    out.width = frame->width;
    out.height = frame->height;
    out.pts = frame->pts;
    out.is_keyframe = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
    out.format = frame->format;

    // 1. Hardware texture pass-through (Zero-Copy!)
    if (frame->format == s_hw_pix_fmt) {
        // macOS: frame->data[3] is CVPixelBufferRef
        out.hw_surface = reinterpret_cast<void*>(frame->data[3]);
        m_decoded_count++;
        m_frame_cb(out);
        return;
    }

    // 2. CPU RGB24 fallback conversion
    if (!m_sws_ctx || m_width != frame->width || m_height != frame->height) {
        if (m_sws_ctx) sws_freeContext(m_sws_ctx);
        m_width = frame->width;
        m_height = frame->height;
        m_sws_ctx = sws_getContext(
            frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
            frame->width, frame->height, AV_PIX_FMT_RGB24,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    }

    if (!m_sws_ctx) return;

    out.stride = frame->width * 3;
    out.data.resize(out.stride * frame->height);

    uint8_t* dst_data[4] = { out.data.data(), nullptr, nullptr, nullptr };
    int dst_linesize[4] = { out.stride, 0, 0, 0 };

    sws_scale(m_sws_ctx, frame->data, frame->linesize, 0, frame->height, dst_data, dst_linesize);

    m_decoded_count++;
    m_frame_cb(out);
}

void VideoDecoder::flush() {
    if (m_codec_ctx) {
        avcodec_flush_buffers(m_codec_ctx);
    }
}

} // namespace harmonycast
