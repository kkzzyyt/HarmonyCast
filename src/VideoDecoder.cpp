#include "harmonycast/VideoDecoder.h"
#include <iostream>
#include <thread>
#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace harmonycast {

#if defined(__APPLE__)
static constexpr AVPixelFormat s_hw_pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
#elif defined(_WIN32)
static constexpr AVPixelFormat s_hw_pix_fmt = AV_PIX_FMT_D3D11;
#else
static constexpr AVPixelFormat s_hw_pix_fmt = AV_PIX_FMT_VAAPI;
#endif

static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    const enum AVPixelFormat* p;
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == s_hw_pix_fmt) {
            return *p;
        }
    }
    // FFmpeg retries format selection without the hardware format when the
    // actual stream is unsupported. Keep software decoding available then.
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const auto* descriptor = av_pix_fmt_desc_get(*p);
        if (descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
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
    if (m_av_frame) av_frame_unref(m_av_frame);
    if (m_sw_frame) av_frame_unref(m_sw_frame);
    if (m_av_packet) av_packet_unref(m_av_packet);
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
    m_hw_transfer_failures = 0;
    m_decoded_count = 0;
    m_source_format = -1;
    m_images = {};
}

bool VideoDecoder::initHwContext() {
#if defined(__APPLE__)
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#elif defined(_WIN32)
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_D3D11VA;
#else
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_VAAPI;
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
#if defined(HARMONYCAST_SERVER2012_COMPAT)
    // The compatibility FFmpeg build contains a CPU-only H.264 decoder.
    enable_hardware = false;
#endif
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

    av_packet_unref(m_av_packet);
    if (av_new_packet(m_av_packet, static_cast<int>(data.size())) < 0) return false;
    std::memcpy(m_av_packet->data, data.data(), data.size());
    m_av_packet->pts = pts;
    m_av_packet->flags = is_keyframe ? AV_PKT_FLAG_KEY : 0;

    int ret = avcodec_send_packet(m_codec_ctx, m_av_packet);
    if (ret == AVERROR(EAGAIN)) {
        while (avcodec_receive_frame(m_codec_ctx, m_av_frame) == 0) emitFrame(m_av_frame);
        ret = avcodec_send_packet(m_codec_ctx, m_av_packet);
    }
    if (ret < 0) return false;

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

    AVFrame* render_frame = frame;
    m_hw_active = frame->format == s_hw_pix_fmt;
    if (frame->format == s_hw_pix_fmt) {
        if (m_sw_frame->width != frame->width || m_sw_frame->height != frame->height) av_frame_unref(m_sw_frame);
        int ret = av_hwframe_transfer_data(m_sw_frame, frame, 0);
        if (ret < 0) {
            char err[128];
            av_strerror(ret, err, sizeof(err));
            std::cerr << "[VideoDecoder] av_hwframe_transfer_data failed: " << err << std::endl;
            m_hw_transfer_failures++;
            if (m_hw_transfer_failures >= 3) {
                std::cerr << "[VideoDecoder] Persistent hardware transfer failure, falling back to software decoding." << std::endl;
                int w = m_width;
                int h = m_height;
                init(w, h, false);
            }
            return;
        }
        m_hw_transfer_failures = 0;
        render_frame = m_sw_frame;
    }

    DecodedFrame out;
    out.width = render_frame->width;
    out.height = render_frame->height;
    out.pts = frame->pts;
    out.is_keyframe = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
    out.format = 4; // QImage::Format_RGB32, also used by the snapshot path.

    // Convert directly into reusable painter-native images.
    if (!m_sws_ctx || m_width != render_frame->width || m_height != render_frame->height || m_source_format != render_frame->format) {
        if (m_sws_ctx) sws_freeContext(m_sws_ctx);
        m_width = render_frame->width;
        m_height = render_frame->height;
        m_source_format = render_frame->format;
        m_sws_ctx = sws_getContext(
            render_frame->width, render_frame->height, static_cast<AVPixelFormat>(render_frame->format),
            render_frame->width, render_frame->height, AV_PIX_FMT_RGB32,
            SWS_BICUBIC | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
    }

    if (!m_sws_ctx) return;

    for (auto& image : m_images) {
        if (image.isNull() || image.isDetached()) {
            if (image.size() != QSize(out.width, out.height)) image = QImage(out.width, out.height, QImage::Format_RGB32);
            if (image.isNull()) return;
            // The pool slot is detached, so writing cannot change a displayed frame.
            uint8_t* dst_data[4] = { image.bits(), nullptr, nullptr, nullptr };
            int dst_linesize[4] = { int(image.bytesPerLine()), 0, 0, 0 };
            sws_scale(m_sws_ctx, render_frame->data, render_frame->linesize, 0, render_frame->height, dst_data, dst_linesize);
            out.image = image;
            break;
        }
    }
    if (out.image.isNull()) {
        out.image = QImage(out.width, out.height, QImage::Format_RGB32);
        if (out.image.isNull()) return;
        uint8_t* dst_data[4] = { out.image.bits(), nullptr, nullptr, nullptr };
        int dst_linesize[4] = { int(out.image.bytesPerLine()), 0, 0, 0 };
        sws_scale(m_sws_ctx, render_frame->data, render_frame->linesize, 0, render_frame->height, dst_data, dst_linesize);
    }
    out.stride = int(out.image.bytesPerLine());

    m_decoded_count++;
    m_frame_cb(out);
}

void VideoDecoder::flush() {
    if (m_codec_ctx) {
        avcodec_flush_buffers(m_codec_ctx);
    }
}

} // namespace harmonycast
