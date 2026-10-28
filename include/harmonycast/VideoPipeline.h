#pragma once

#include "PacketDemuxer.h"
#include "VideoDecoder.h"
#include <QByteArray>
#include <QObject>
#include <optional>

namespace harmonycast {

// All parsing, decoding and conversion is confined to the owning worker thread.
class VideoPipeline : public QObject {
    Q_OBJECT
public:
    explicit VideoPipeline(QObject* parent = nullptr);
    void consume(const QByteArray& data);
    void setPresentation(QSize viewport, int rotation, qreal dpr);

signals:
    void frameReady(const DecodedFrame& frame);
    void batchProcessed();
    void handshakeRequested();
    void headerParsed(const QString& name, int width, int height);

private:
    PacketDemuxer m_demuxer;
    VideoDecoder m_decoder;
    bool m_decoder_failed{false};
    QSize m_viewport;
    int m_rotation{0};
    qreal m_dpr{1};
    std::optional<DecodedFrame> m_latest;
};

} // namespace harmonycast
