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
    while (!m_buffer.empty()) {
        // 1. Check for SCREEN_INFO handshake (terminated with \n)
        if (m_buffer.size() >= 12 && std::memcmp(m_buffer.data(), "SCREEN_INFO:", 12) == 0) {
            auto it = std::find(m_buffer.begin(), m_buffer.end(), '\n');
            if (it == m_buffer.end()) {
                break; // wait for full line
            }
            std::string line(m_buffer.begin(), it);
            if (m_handshake_cb) {
                m_handshake_cb(line);
            }
            m_header_parsed = true;
            m_buffer.erase(m_buffer.begin(), it + 1);
            continue;
        }

        // 2. Check for Native Raw Frame (HCR1 or HCR2)
        if (m_buffer.size() >= 4 && m_buffer[0] == 'H' && m_buffer[1] == 'C' && m_buffer[2] == 'R' &&
            (m_buffer[3] == '1' || m_buffer[3] == '2')) {
            bool is_hcr2 = (m_buffer[3] == '2');
            size_t header_size = is_hcr2 ? 32 : 28;
            if (m_buffer.size() < header_size) {
                break; // wait for full header
            }

            uint32_t width = readBigEndian32(m_buffer.data() + 4);
            uint32_t height = readBigEndian32(m_buffer.data() + 8);
            uint32_t stride = readBigEndian32(m_buffer.data() + 12);
            uint32_t length = readBigEndian32(m_buffer.data() + 16);

            // Validation
            if (width < 1 || width > 4096 || height < 1 || height > 4096 ||
                stride < width * 4 || stride > width * 4 + 4096 ||
                length != stride * height || length > 32 * 1024 * 1024) {
                // Invalid frame header, drop 1 byte to re-sync
                m_buffer.erase(m_buffer.begin());
                continue;
            }

            size_t total_size = header_size + length;
            if (m_buffer.size() < total_size) {
                break; // wait for full frame payload
            }

            if (!m_header_parsed) {
                m_header_parsed = true;
                m_width = static_cast<int>(width);
                m_height = static_cast<int>(height);
                m_device_name = "OpenHarmony";
                if (m_header_cb) {
                    m_header_cb(m_device_name, m_width, m_height);
                }
            }

            if (m_raw_frame_cb) {
                DecodedFrame frame;
                frame.width = static_cast<int>(width);
                frame.height = static_cast<int>(height);
                frame.stride = static_cast<int>(stride);
                frame.format = 12; // AV_PIX_FMT_RGBA
                frame.is_keyframe = true;
                frame.data.assign(m_buffer.begin() + header_size, m_buffer.begin() + total_size);
                m_raw_frame_cb(frame);
            }

            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + total_size);
            continue;
        }

        // 3. Check for 72-byte Android Scrcpy header (device name 64B + W 4B + H 4B)
        if (!m_header_parsed && m_buffer.size() >= 72) {
            uint32_t cand_w = readBigEndian32(m_buffer.data() + 64);
            uint32_t cand_h = readBigEndian32(m_buffer.data() + 68);
            if (cand_w >= 100 && cand_w <= 8192 && cand_h >= 100 && cand_h <= 8192) {
                const char* name_ptr = reinterpret_cast<const char*>(m_buffer.data());
                size_t len = strnlen(name_ptr, 64);
                m_device_name.assign(name_ptr, len);
                m_width = static_cast<int>(cand_w);
                m_height = static_cast<int>(cand_h);
                m_header_parsed = true;
                if (m_header_cb) {
                    m_header_cb(m_device_name, m_width, m_height);
                }
                m_buffer.erase(m_buffer.begin(), m_buffer.begin() + 72);
                continue;
            }
        }

        // 4. OpenHarmony 8-byte packet header: [uint32 type (4B)] [uint32 size (4B)] [payload]
        if (m_buffer.size() >= 8) {
            uint32_t type = readBigEndian32(m_buffer.data());
            uint32_t size = readBigEndian32(m_buffer.data() + 4);

            if (type <= 8 && size <= 32 * 1024 * 1024) {
                size_t total_pkt = 8 + size;
                if (m_buffer.size() < total_pkt) {
                    break; // wait for full packet
                }

                if (type == 5 && size >= 16) {
                    // Dimensions packet: width, height, fps, bitrate...
                    m_width = static_cast<int>(readBigEndian32(m_buffer.data() + 8));
                    m_height = static_cast<int>(readBigEndian32(m_buffer.data() + 12));
                    m_header_parsed = true;
                    if (m_header_cb) {
                        m_header_cb("OpenHarmony", m_width, m_height);
                    }
                } else if (type == 1 || type == 2 || type == 3 || type == 4 || type == 6) {
                    if (m_packet_cb) {
                        StreamPacket pkt;
                        pkt.type = static_cast<uint8_t>(type);
                        pkt.is_keyframe = (type == 3);
                        pkt.payload.assign(m_buffer.begin() + 8, m_buffer.begin() + total_pkt);
                        m_packet_cb(pkt);
                    }
                }

                m_buffer.erase(m_buffer.begin(), m_buffer.begin() + total_pkt);
                continue;
            }
        }

        // 5. 5-byte Scrcpy packet format: [uint8 type (1B)] [uint32 size (4B)] [payload]
        if (m_buffer.size() >= 5) {
            uint8_t type = m_buffer[0];
            uint32_t payload_size = readBigEndian32(m_buffer.data() + 1);

            if (type <= 8 && payload_size <= 32 * 1024 * 1024) {
                size_t total_packet_size = 5 + payload_size;
                if (m_buffer.size() < total_packet_size) {
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
                continue;
            }
        }

        // Cannot match any packet header; drop 1 byte to re-sync
        m_buffer.erase(m_buffer.begin());
    }

    // Shrink buffer if excess capacity
    if (m_buffer.empty() && m_buffer.capacity() > 1024 * 1024) {
        m_buffer.shrink_to_fit();
        m_buffer.reserve(256 * 1024);
    }
}

} // namespace harmonycast
