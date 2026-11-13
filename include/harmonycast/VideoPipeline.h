#pragma once

#include "PacketDemuxer.h"
#include "VideoDecoder.h"
#include "ScrcpyDemuxer.h"
#include <QByteArray>
#include <QObject>
#include <optional>

namespace harmonycast {

// All parsing, decoding and conversion is confined to the owning worker thread.
class VideoPipeline : public QObject {
    Q_OBJECT
public:
    explicit VideoPipeline(QObject* parent = nullptr, DevicePlatform platform = DevicePlatform::OpenHarmony);
    void consume(const QByteArray& data);
    void setPresentation(QSize viewport, int rotation, qreal dpr);
    // Call on the owning worker thread. Each encoded stream gets one CPU retry.
    bool useSoftwareDecoder();
    void requestSoftwareDecoder();

signals:
    void frameReady(const DecodedFrame& frame);
    void batchProcessed();
    void handshakeRequested();
    void headerParsed(const QString& name, int width, int height);
    void decoderFallback(const QString& reason);
    void softwareRetryCompleted(bool recovered);
    void protocolError(const QString& error);

private:
    PacketDemuxer m_demuxer;
    ScrcpyDemuxer m_scrcpy;
    DevicePlatform m_platform;
    VideoDecoder m_decoder;
    bool m_decoder_failed{false};
    bool m_encoded_stream{false};
    bool m_raw_stream{false};
    bool m_software_attempted{false};
    bool m_waiting_keyframe{true};
    QByteArray m_sps;
    QByteArray m_pps;
    QSize m_viewport;
    int m_rotation{0};
    qreal m_dpr{1};
    std::optional<DecodedFrame> m_latest;

    void decodePacket(const StreamPacket& packet);
};

} // namespace harmonycast
