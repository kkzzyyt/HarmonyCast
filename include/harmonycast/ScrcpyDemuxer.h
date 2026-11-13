#pragma once

#include "PacketDemuxer.h"
#include <QString>

namespace harmonycast {

// Wire format pinned to scrcpy-server 5.0.1, with video metadata enabled.
class ScrcpyDemuxer {
public:
    void feed(std::span<const uint8_t> data);
    void reset();
    void setPacketCallback(PacketDemuxer::PacketCallback cb) { m_packet = std::move(cb); }
    void setHeaderCallback(PacketDemuxer::HeaderCallback cb) { m_header = std::move(cb); }
    void setErrorCallback(std::function<void(const QString&)> cb) { m_error = std::move(cb); }
    [[nodiscard]] int width() const { return m_width; }
    [[nodiscard]] int height() const { return m_height; }
private:
    std::vector<uint8_t> m_buffer;
    size_t m_offset{0};
    bool m_metadata{false};
    bool m_failed{false};
    int m_width{0}, m_height{0};
    std::string m_name;
    PacketDemuxer::PacketCallback m_packet;
    PacketDemuxer::HeaderCallback m_header;
    std::function<void(const QString&)> m_error;
    void fail(const QString& error);
};

} // namespace harmonycast
