#pragma once

#include "Types.h"
#include <vector>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace harmonycast {

struct StreamPacket {
    uint8_t type{0};
    bool is_keyframe{false};
    int64_t pts{0};
    std::vector<uint8_t> payload;
};

class PacketDemuxer {
public:
    using PacketCallback = std::function<void(const StreamPacket&)>;
    using HeaderCallback = std::function<void(const std::string& device_name, int width, int height)>;
    using RawFrameCallback = std::function<void(const DecodedFrame&)>;
    using HandshakeCallback = std::function<void(const std::string&)>;

    PacketDemuxer();
    ~PacketDemuxer() = default;

    void setPacketCallback(PacketCallback cb) { m_packet_cb = std::move(cb); }
    void setHeaderCallback(HeaderCallback cb) { m_header_cb = std::move(cb); }
    void setRawFrameCallback(RawFrameCallback cb) { m_raw_frame_cb = std::move(cb); }
    void setHandshakeCallback(HandshakeCallback cb) { m_handshake_cb = std::move(cb); }

    // Feeds incoming raw TCP bytes from socket
    void feed(std::span<const uint8_t> data);
    void reset();

    [[nodiscard]] bool hasHeader() const { return m_header_parsed; }
    [[nodiscard]] int deviceWidth() const { return m_width; }
    [[nodiscard]] int deviceHeight() const { return m_height; }
    [[nodiscard]] const std::string& deviceName() const { return m_device_name; }

private:
    std::vector<uint8_t> m_buffer;
    bool m_header_parsed{false};
    std::string m_device_name;
    int m_width{0};
    int m_height{0};

    PacketCallback m_packet_cb;
    HeaderCallback m_header_cb;
    RawFrameCallback m_raw_frame_cb;
    HandshakeCallback m_handshake_cb;

    void processBuffer();
};

} // namespace harmonycast
