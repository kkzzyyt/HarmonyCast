#include "harmonycast/VideoPipeline.h"
#include "harmonycast/FramePresentation.h"
#include "harmonycast/Logger.h"

namespace {

constexpr size_t kMaxParameterSetBytes = 64 * 1024;

size_t startCodeSize(std::span<const uint8_t> bytes, size_t offset) {
    if (offset + 3 > bytes.size() || bytes[offset] != 0 || bytes[offset + 1] != 0) return 0;
    if (bytes[offset + 2] == 1) return 3;
    if (offset + 4 <= bytes.size() && bytes[offset + 2] == 0 && bytes[offset + 3] == 1) return 4;
    return 0;
}

template<class Visitor>
void visitNalus(std::span<const uint8_t> bytes, Visitor visitor) {
    size_t start = 0;
    while (start < bytes.size() && !startCodeSize(bytes, start)) ++start;
    if (start == bytes.size()) {
        if (!bytes.empty()) visitor(bytes[0] & 31, bytes);
        return;
    }
    while (start < bytes.size()) {
        const size_t payload = start + startCodeSize(bytes, start);
        size_t end = payload;
        while (end < bytes.size() && !startCodeSize(bytes, end)) ++end;
        if (payload < end) visitor(bytes[payload] & 31, bytes.subspan(payload, end - payload));
        start = end;
    }
}

} // namespace

namespace harmonycast {

VideoPipeline::VideoPipeline(QObject* parent, DevicePlatform platform) : QObject(parent), m_platform(platform) {
    m_demuxer.setHandshakeCallback([this](const std::string&) { emit handshakeRequested(); });
    m_demuxer.setHeaderCallback([this](const std::string& name, int width, int height) {
        emit headerParsed(QString::fromStdString(name), width, height);
    });
    m_demuxer.setRawFrameCallback([this](const DecodedFrame& frame) {
        m_raw_stream = true;
        m_latest = frame;
    });
    m_decoder.setFrameCallback([this](const DecodedFrame& decoded) {
        DecodedFrame frame = decoded;
        frame.source_width = m_platform == DevicePlatform::Android ? m_scrcpy.width() : m_demuxer.sourceWidth();
        frame.source_height = m_platform == DevicePlatform::Android ? m_scrcpy.height() : m_demuxer.sourceHeight();
        m_latest = std::move(frame);
    });
    m_demuxer.setPacketCallback([this](const StreamPacket& packet) { decodePacket(packet); });
    m_scrcpy.setPacketCallback([this](const StreamPacket& packet) { decodePacket(packet); });
    m_scrcpy.setHeaderCallback([this](const std::string& name, int w, int h) {
        // A new capture session carries new SPS/PPS after rotation.
        m_decoder.close(); m_latest.reset(); m_decoder_failed = false;
        m_waiting_keyframe = true; m_sps.clear(); m_pps.clear();
        emit headerParsed(QString::fromStdString(name), w, h);
    });
    m_scrcpy.setErrorCallback([this](const QString& error) { emit protocolError(error); });
}

bool VideoPipeline::useSoftwareDecoder() {
    if (!m_encoded_stream || m_raw_stream || m_software_attempted) return false;
    m_software_attempted = true;
    m_waiting_keyframe = true;
    m_latest.reset();
    // decodePacket invokes this only after decode() returns; no FFmpeg frame,
    // packet or context is released from inside its receive/transfer loop.
    m_decoder_failed = !m_decoder.init(0, 0, false);
    if (m_decoder_failed) {
        HC_LOG_ERROR("Could not initialize CPU video decoder");
        return false;
    }
    const QString reason = QStringLiteral("已切换软件解码，等待关键帧恢复画面");
    HC_LOG_WARN(reason);
    emit decoderFallback(reason);
    return true;
}

void VideoPipeline::requestSoftwareDecoder() {
    // An automatic fallback may finish just before a queued watchdog request.
    // Reuse that CPU decoder instead of treating the overlap as a failed retry.
    const bool alreadyRecovered = m_software_attempted && !m_decoder_failed && !m_raw_stream && m_decoder.isReady();
    emit softwareRetryCompleted(alreadyRecovered || useSoftwareDecoder());
}

void VideoPipeline::decodePacket(const StreamPacket& packet) {
    if (packet.type < 1 || packet.type > 4 || m_raw_stream || m_decoder_failed || packet.payload.empty()) return;
    m_encoded_stream = true;
    bool has_picture = false;
    bool has_idr = false;
    visitNalus(packet.payload, [&](int type, std::span<const uint8_t> nalu) {
        has_picture |= type == 1 || type == 5;
        has_idr |= type == 5;
        if ((type == 7 || type == 8) && nalu.size() + 4 <= kMaxParameterSetBytes) {
            QByteArray parameter_set("\0\0\0\1", 4);
            parameter_set.append(reinterpret_cast<const char*>(nalu.data()), qsizetype(nalu.size()));
            (type == 7 ? m_sps : m_pps) = std::move(parameter_set);
        }
    });
    // Configuration packets may contain SPS/PPS alone, or both configuration
    // and a picture. Never rely on their wire type to identify an IDR frame.
    if (!has_picture) return;
    const bool keyframe = has_idr || packet.is_keyframe;
    if (m_waiting_keyframe && !keyframe) return;
    if (!m_decoder.isReady() && !m_decoder.init(0, 0, !m_software_attempted) && !useSoftwareDecoder()) {
        m_decoder_failed = true;
        return;
    }

    QByteArray configured;
    std::span<const uint8_t> payload = packet.payload;
    if (m_waiting_keyframe) {
        // Feed cached parameter sets together with the first IDR. Sending them
        // as standalone packets can itself be reported as invalid H.264 data.
        // Always preserve SPS -> PPS ordering, including when this packet
        // repeats just one of them. Duplicate parameter sets are valid H.264.
        configured = m_sps + m_pps;
        if (!configured.isEmpty()) {
            configured.append(reinterpret_cast<const char*>(payload.data()), qsizetype(payload.size()));
            payload = std::span(reinterpret_cast<const uint8_t*>(configured.constData()), size_t(configured.size()));
        }
    }
    const bool decoded = m_decoder.decode(payload, keyframe, packet.pts);
    if (m_decoder.softwareFallbackRequested()) {
        useSoftwareDecoder();
    } else if (decoded) {
        m_waiting_keyframe = false;
    }
}

void VideoPipeline::consume(const QByteArray& data) {
    const auto bytes = std::span(reinterpret_cast<const uint8_t*>(data.constData()), size_t(data.size()));
    if (m_platform == DevicePlatform::Android) m_scrcpy.feed(bytes);
    else m_demuxer.feed(bytes);
    if (m_latest) {
        prepareFramePresentation(*m_latest, m_viewport, m_rotation, m_dpr);
        emit frameReady(*m_latest);
        m_latest.reset();
    }
    emit batchProcessed();
}

void VideoPipeline::setPresentation(QSize viewport, int rotation, qreal dpr) {
    m_viewport = viewport;
    m_rotation = rotation;
    m_dpr = dpr;
}

} // namespace harmonycast
