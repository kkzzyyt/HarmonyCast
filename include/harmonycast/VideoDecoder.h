#pragma once

#include "Types.h"
#include <vector>
#include <cstdint>
#include <span>
#include <memory>
#include <functional>
#include <array>

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;

namespace harmonycast {

namespace detail {

// Keep error accounting independent of FFmpeg resource lifetime. Only the
// pipeline may rebuild the decoder after decode() has returned.
class DecoderRecoveryPolicy {
public:
    void reset(bool hardware) {
        m_hardware = hardware;
        m_failures = 0;
        m_fallback_requested = false;
    }
    void frameDecoded(bool hardware) {
        m_hardware = hardware;
        m_failures = 0;
    }
    void decodeFailed() {
        if (m_hardware && m_failures < 3 && ++m_failures == 3) m_fallback_requested = true;
    }
    void transferFailed() {
        decodeFailed();
    }
    [[nodiscard]] bool fallbackRequested() const { return m_fallback_requested; }

private:
    bool m_hardware{false};
    unsigned int m_failures{0};
    bool m_fallback_requested{false};
};

} // namespace detail

class VideoDecoder {
public:
    using FrameCallback = std::function<void(const DecodedFrame&)>;

    VideoDecoder();
    ~VideoDecoder();

    bool init(int width = 0, int height = 0, bool enable_hardware = true);
    void close();

    void setFrameCallback(FrameCallback cb) { m_frame_cb = std::move(cb); }

    // Decode a packet (H.264 NALUs)
    bool decode(std::span<const uint8_t> data, bool is_keyframe = false, int64_t pts = 0);
    void flush();

    [[nodiscard]] bool isReady() const { return m_codec_ctx != nullptr; }
    [[nodiscard]] bool isHardwareAccelerated() const { return m_hw_active; }
    [[nodiscard]] bool softwareFallbackRequested() const { return m_recovery.fallbackRequested(); }
    [[nodiscard]] int width() const { return m_width; }
    [[nodiscard]] int height() const { return m_height; }
    [[nodiscard]] uint64_t totalFramesDecoded() const { return m_decoded_count; }

private:
    AVCodecContext* m_codec_ctx{nullptr};
    AVFrame* m_av_frame{nullptr};
    AVFrame* m_sw_frame{nullptr};
    AVPacket* m_av_packet{nullptr};
    AVBufferRef* m_hw_device_ctx{nullptr};
    SwsContext* m_sws_ctx{nullptr};

    bool m_hw_active{false};
    detail::DecoderRecoveryPolicy m_recovery;
    int m_width{0};
    int m_height{0};
    uint64_t m_decoded_count{0};
    int m_source_format{-1};
    std::array<QImage, 2> m_images;
    FrameCallback m_frame_cb;

    bool initHwContext();
    void recordDecodeFailure(const char* operation, int error);
    bool emitFrame(AVFrame* frame);
};

} // namespace harmonycast
