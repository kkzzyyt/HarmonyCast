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
    int m_width{0};
    int m_height{0};
    uint64_t m_decoded_count{0};
    int m_source_format{-1};
    std::array<QImage, 2> m_images;
    FrameCallback m_frame_cb;

    bool initHwContext();
    void emitFrame(AVFrame* frame);
};

} // namespace harmonycast
