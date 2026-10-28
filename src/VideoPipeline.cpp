#include "harmonycast/VideoPipeline.h"
#include "harmonycast/FramePresentation.h"

namespace harmonycast {

VideoPipeline::VideoPipeline(QObject* parent) : QObject(parent) {
    m_demuxer.setHandshakeCallback([this](const std::string&) { emit handshakeRequested(); });
    m_demuxer.setHeaderCallback([this](const std::string& name, int width, int height) {
        emit headerParsed(QString::fromStdString(name), width, height);
    });
    m_demuxer.setRawFrameCallback([this](const DecodedFrame& frame) { m_latest = frame; });
    m_decoder.setFrameCallback([this](const DecodedFrame& decoded) {
        DecodedFrame frame = decoded;
        frame.source_width = m_demuxer.sourceWidth();
        frame.source_height = m_demuxer.sourceHeight();
        m_latest = std::move(frame);
    });
    m_demuxer.setPacketCallback([this](const StreamPacket& packet) {
        if (packet.type < 1 || packet.type > 4 || m_decoder_failed) return;
        if (!m_decoder.isReady() && !m_decoder.init(0, 0, true) && !m_decoder.init(0, 0, false)) {
            m_decoder_failed = true;
            return;
        }
        m_decoder.decode(packet.payload, packet.is_keyframe, packet.pts);
    });
}

void VideoPipeline::consume(const QByteArray& data) {
    m_demuxer.feed(std::span(reinterpret_cast<const uint8_t*>(data.constData()), size_t(data.size())));
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
