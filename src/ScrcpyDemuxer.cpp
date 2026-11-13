#include "harmonycast/ScrcpyDemuxer.h"
#include <algorithm>

namespace harmonycast {
namespace {
uint32_t read32(const uint8_t* data) {
    return uint32_t(data[0]) << 24 | uint32_t(data[1]) << 16 | uint32_t(data[2]) << 8 | data[3];
}
uint64_t read64(const uint8_t* data) { return uint64_t(read32(data)) << 32 | read32(data + 4); }
constexpr size_t maxPacketSize = 32 * 1024 * 1024;
}

void ScrcpyDemuxer::reset() {
    m_buffer = {}; m_offset = 0; m_metadata = false; m_failed = false;
    m_width = m_height = 0; m_name.clear();
}

void ScrcpyDemuxer::fail(const QString& error) {
    m_failed = true;
    m_buffer.clear(); m_offset = 0;
    if (m_error) m_error(error);
}

void ScrcpyDemuxer::feed(std::span<const uint8_t> data) {
    if (m_failed || data.empty()) return;
    if (data.size() > maxPacketSize + 128 || m_buffer.size() - m_offset > maxPacketSize + 128 - data.size()) {
        fail("Android 视频数据超过缓冲上限"); return;
    }
    if (m_offset && (m_offset > 1024 * 1024 || m_offset == m_buffer.size())) {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + m_offset); m_offset = 0;
    }
    m_buffer.insert(m_buffer.end(), data.begin(), data.end());
    if (!m_metadata) {
        if (m_buffer.size() - m_offset < 69) return;
        const auto* bytes = m_buffer.data() + m_offset;
        if (bytes[0] != 0 || read32(bytes + 65) != 0x68323634) {
            fail("Android 视频握手失败或编码格式不是 H.264，请检查采屏服务版本"); return;
        }
        const auto* name = bytes + 1;
        m_name.assign(reinterpret_cast<const char*>(name), std::find(name, name + 64, uint8_t(0)) - name);
        m_offset += 69; m_metadata = true;
    }
    while (m_buffer.size() - m_offset >= 12) {
        const auto* bytes = m_buffer.data() + m_offset;
        const uint32_t first = read32(bytes);
        if (first & 0x80000000u) {
            const uint32_t width = read32(bytes + 4), height = read32(bytes + 8);
            if ((first & 0x7ffffffeu) || !width || width > 16384 || !height || height > 16384) {
                fail("Android 视频尺寸信息无效"); return;
            }
            m_width = int(width); m_height = int(height); m_offset += 12;
            if (m_header) m_header(m_name, m_width, m_height);
            continue;
        }
        const uint64_t flags = read64(bytes);
        const uint32_t size = read32(bytes + 8);
        if (size > maxPacketSize || m_width == 0) {
            fail("Android 视频帧长度或会话信息无效"); return;
        }
        if (m_buffer.size() - m_offset < 12 + size) break;
        StreamPacket packet;
        packet.type = (flags & (uint64_t(1) << 62)) ? 1 : 4;
        packet.is_keyframe = (flags & (uint64_t(1) << 61)) != 0;
        packet.pts = int64_t(flags & ((uint64_t(1) << 61) - 1));
        packet.payload.assign(bytes + 12, bytes + 12 + size);
        m_offset += 12 + size;
        if (m_packet && size) m_packet(packet);
    }
    if (m_offset == m_buffer.size()) { m_buffer.clear(); m_offset = 0; }
}

} // namespace harmonycast
