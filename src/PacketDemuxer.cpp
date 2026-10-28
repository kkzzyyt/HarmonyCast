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
    std::vector<uint8_t>().swap(m_buffer);
    m_buffer.reserve(256 * 1024);
    m_read_offset = 0;
    m_source_width = m_source_height = 0;
    m_header_parsed = false;
    m_device_name.clear();
    m_width = 0;
    m_height = 0;
}

void PacketDemuxer::feed(std::span<const uint8_t> data) {
    if (data.empty()) return;
    // Feed in bounded chunks and compact only when consumed bytes are significant.
    while (!data.empty()) {
        if (m_read_offset && (m_read_offset >= 256 * 1024 || m_read_offset == m_buffer.size())) {
            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + m_read_offset);
            m_read_offset = 0;
        }
        size_t count = std::min(data.size(), size_t(256 * 1024));
        m_buffer.insert(m_buffer.end(), data.begin(), data.begin() + count);
        data = data.subspan(count);
        processBuffer();
    }
}

void PacketDemuxer::processBuffer() {
    while (m_read_offset < m_buffer.size()) {
        std::span<const uint8_t> bytes(m_buffer.data() + m_read_offset, m_buffer.size() - m_read_offset);
        // Headers can be split at any byte by TCP.
        auto partial = [&](const char* magic, size_t length) {
            return bytes.size() < length && std::memcmp(bytes.data(), magic, bytes.size()) == 0;
        };
        if (partial("SCREEN_INFO:", 12) || partial("HCR1", 4) || partial("HCR2", 4)) break;
        if (bytes.size() < 5) break;
        // 1. Check for SCREEN_INFO handshake (terminated with \n)
        if (bytes.size() >= 12 && std::memcmp(bytes.data(), "SCREEN_INFO:", 12) == 0) {
            auto it = std::find(bytes.begin(), bytes.end(), '\n');
            if (it == bytes.end()) {
                if (bytes.size() > 512) { ++m_read_offset; continue; }
                break;
            }
            std::string line(bytes.begin(), it);
            if (m_handshake_cb) {
                m_handshake_cb(line);
            }
            m_header_parsed = true;
            m_read_offset += size_t(it - bytes.begin()) + 1;
            continue;
        }

        // 2. Check for Native Raw Frame (HCR1 or HCR2)
        if (bytes.size() >= 4 && bytes[0] == 'H' && bytes[1] == 'C' && bytes[2] == 'R' &&
            (bytes[3] == '1' || bytes[3] == '2')) {
            bool is_hcr2 = (bytes[3] == '2');
            size_t header_size = is_hcr2 ? 32 : 28;
            if (bytes.size() < header_size) {
                break; // wait for full header
            }

            uint32_t width = readBigEndian32(bytes.data() + 4);
            uint32_t height = readBigEndian32(bytes.data() + 8);
            uint32_t stride = readBigEndian32(bytes.data() + 12);
            uint32_t length = readBigEndian32(bytes.data() + 16);

            // Validation
            if (width < 1 || width > 4096 || height < 1 || height > 4096 ||
                stride < width * 4 || stride > width * 4 + 4096 ||
                length != stride * height || length > 32 * 1024 * 1024) {
                // Invalid frame header, drop 1 byte to re-sync
                ++m_read_offset;
                continue;
            }

            size_t total_size = header_size + length;
            if (bytes.size() < total_size) {
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
                QImage wrapped(bytes.data() + header_size, int(width), int(height), int(stride), QImage::Format_RGBA8888);
                frame.image = wrapped.copy();
                frame.source_width = is_hcr2 ? int(readBigEndian32(bytes.data() + 20)) : int(width);
                frame.source_height = is_hcr2 ? int(readBigEndian32(bytes.data() + 24)) : int(height);
                if (is_hcr2 && (readBigEndian32(bytes.data() + 28) & 1)) std::swap(frame.source_width, frame.source_height);
                if (frame.source_width <= 0 || frame.source_width > 8192) frame.source_width = int(width);
                if (frame.source_height <= 0 || frame.source_height > 8192) frame.source_height = int(height);
                m_raw_frame_cb(frame);
            }

            m_read_offset += total_size;
            continue;
        }

        // The legacy device-name header starts with printable text.
        if (!m_header_parsed && bytes[0] >= 32 && bytes[0] < 127 && bytes.size() < 72) break;

        // 3. Check for 72-byte Android Scrcpy header (device name 64B + W 4B + H 4B)
        if (!m_header_parsed && bytes.size() >= 72) {
            uint32_t cand_w = readBigEndian32(bytes.data() + 64);
            uint32_t cand_h = readBigEndian32(bytes.data() + 68);
            if (cand_w >= 100 && cand_w <= 8192 && cand_h >= 100 && cand_h <= 8192) {
                const char* name_ptr = reinterpret_cast<const char*>(bytes.data());
                size_t len = strnlen(name_ptr, 64);
                m_device_name.assign(name_ptr, len);
                m_width = static_cast<int>(cand_w);
                m_height = static_cast<int>(cand_h);
                m_header_parsed = true;
                if (m_header_cb) {
                    m_header_cb(m_device_name, m_width, m_height);
                }
                m_read_offset += 72;
                continue;
            }
        }

        // 4. OpenHarmony 8-byte packet header: [uint32 type (4B)] [uint32 size (4B)] [payload]
        if (bytes.size() >= 8) {
            uint32_t type = readBigEndian32(bytes.data());
            uint32_t size = readBigEndian32(bytes.data() + 4);

            if (type <= 8 && size <= 32 * 1024 * 1024) {
                size_t total_pkt = 8 + size;
                if (bytes.size() < total_pkt) {
                    break; // wait for full packet
                }

                if (type == 5 && size >= 16) {
                    // Dimensions packet: width, height, fps, bitrate...
                    m_width = static_cast<int>(readBigEndian32(bytes.data() + 8));
                    m_height = static_cast<int>(readBigEndian32(bytes.data() + 12));
                    if (size >= 24) {
                        m_source_width = int(readBigEndian32(bytes.data() + 24));
                        m_source_height = int(readBigEndian32(bytes.data() + 28));
                        if (size >= 28 && (readBigEndian32(bytes.data() + 32) & 1)) std::swap(m_source_width, m_source_height);
                        if (m_source_width <= 0 || m_source_width > 8192) m_source_width = m_width;
                        if (m_source_height <= 0 || m_source_height > 8192) m_source_height = m_height;
                    }
                    m_header_parsed = true;
                    if (m_header_cb) {
                        m_header_cb("OpenHarmony", m_width, m_height);
                    }
                } else if (type == 1 || type == 2 || type == 3 || type == 4 || type == 6) {
                    if (m_packet_cb) {
                        StreamPacket pkt;
                        pkt.type = static_cast<uint8_t>(type);
                        pkt.is_keyframe = (type == 3);
                        pkt.payload.assign(bytes.begin() + 8, bytes.begin() + total_pkt);
                        m_packet_cb(pkt);
                    }
                }

                m_read_offset += total_pkt;
                continue;
            }
        }

        if (bytes[0] == 0 && bytes.size() < 8) break;

        // 5. 5-byte Scrcpy packet format: [uint8 type (1B)] [uint32 size (4B)] [payload]
        if (bytes.size() >= 5) {
            uint8_t type = bytes[0];
            uint32_t payload_size = readBigEndian32(bytes.data() + 1);

            if (type <= 8 && payload_size <= 32 * 1024 * 1024) {
                size_t total_packet_size = 5 + payload_size;
                if (bytes.size() < total_packet_size) {
                    break;
                }

                if (m_packet_cb) {
                    StreamPacket pkt;
                    pkt.type = type;
                    pkt.is_keyframe = (type == 3);
                    pkt.payload.assign(bytes.begin() + 5, bytes.begin() + total_packet_size);
                    m_packet_cb(pkt);
                }

                m_read_offset += total_packet_size;
                continue;
            }
        }

        // Cannot match any packet header; drop 1 byte to re-sync
        ++m_read_offset;
    }

    // Keep allocated capacity during streaming; reset() releases it on stop.
    if (m_read_offset == m_buffer.size()) {
        m_buffer.clear();
        m_read_offset = 0;
    }
}

} // namespace harmonycast
