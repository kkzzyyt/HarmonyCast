#include "harmonycast/PacketDemuxer.h"
#include <cstring>
#include <algorithm>

namespace harmonycast {

static uint32_t readBigEndian32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}

PacketDemuxer::PacketDemuxer() {
    m_buffer.reserve(256 * 1024);
}

void PacketDemuxer::reset() {
    m_buffer.clear();
    m_header_parsed = false;
    m_device_name.clear();
    m_width = 0;
    m_height = 0;
}

void PacketDemuxer::feed(std::span<const uint8_t> data) {
    if (data.empty()) return;
    m_buffer.insert(m_buffer.end(), data.begin(), data.end());
    processBuffer();
}

void PacketDemuxer::processBuffer() {
    // 1. Initial Device Header (64 bytes device name + 4 bytes width + 4 bytes height = 72 bytes)
    if (!m_header_parsed) {
        constexpr size_t HEADER_SIZE = 64 + 4 + 4;
        if (m_buffer.size() < HEADER_SIZE) {
            return;
        }

        // Parse 64 bytes null-terminated name
        const char* name_ptr = reinterpret_cast<const char*>(m_buffer.data());
        size_t len = strnlen(name_ptr, 64);
        m_device_name.assign(name_ptr, len);

        m_width = static_cast<int>(readBigEndian32(m_buffer.data() + 64));
        m_height = static_cast<int>(readBigEndian32(m_buffer.data() + 68));
        m_header_parsed = true;

        if (m_header_cb) {
            m_header_cb(m_device_name, m_width, m_height);
        }

        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + HEADER_SIZE);
    }

    // 2. Continuous Packet Loop
    // Format: [1 byte Type] [4 bytes Size] [N bytes Payload]
    while (m_buffer.size() >= 5) {
        uint8_t type = m_buffer[0];
        uint32_t payload_size = readBigEndian32(m_buffer.data() + 1);

        // Sanity check payload size (< 32MB) to prevent buffer attacks or corrupted stream
        if (payload_size > 32 * 1024 * 1024) {
            // Stream sync lost; skip 1 byte to seek next packet
            m_buffer.erase(m_buffer.begin());
            continue;
        }

        size_t total_packet_size = 5 + payload_size;
        if (m_buffer.size() < total_packet_size) {
            // Need more data from socket
            break;
        }

        if (m_packet_cb) {
            StreamPacket pkt;
            pkt.type = type;
            pkt.is_keyframe = (type == 3);
            pkt.payload.assign(m_buffer.begin() + 5, m_buffer.begin() + total_packet_size);
            m_packet_cb(pkt);
        }

        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + total_packet_size);
    }

    // Shrink buffer if excess capacity
    if (m_buffer.empty() && m_buffer.capacity() > 1024 * 1024) {
        m_buffer.shrink_to_fit();
        m_buffer.reserve(256 * 1024);
    }
}

} // namespace harmonycast
