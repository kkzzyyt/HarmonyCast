#pragma once

#include "Types.h"
#include "HdcClient.h"
#include "PacketDemuxer.h"
#include "VideoDecoder.h"
#include "ControlChannel.h"
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <memory>
#include <atomic>

namespace harmonycast {

class StreamSession : public QObject {
    Q_OBJECT
public:
    explicit StreamSession(std::shared_ptr<HdcClient> hdc,
                           Device device,
                           int fps = 60,
                           int bitrate = 8000000,
                           int max_size = 0,
                           QObject* parent = nullptr);
    ~StreamSession() override;

    bool start();
    void stop();
    [[nodiscard]] bool isRunning() const;

    [[nodiscard]] const Device& device() const { return m_device; }
    [[nodiscard]] std::shared_ptr<ControlChannel> controlChannel() const { return m_control; }
    [[nodiscard]] int deviceWidth() const { return m_dev_width; }
    [[nodiscard]] int deviceHeight() const { return m_dev_height; }

    void setBitrate(int b) { m_bitrate = b; }
    void setFps(int f) { m_fps = f; }

signals:
    void frameReady(const DecodedFrame& frame);
    void statusChanged(const QString& status);
    void streamFinished(const QString& error);

private slots:
    void onSocketConnected();
    void onSocketReadyRead();
    void onSocketDisconnected();
    void sendHeartbeat();

private:
    std::shared_ptr<HdcClient> m_hdc;
    Device m_device;
    int m_fps{60};
    int m_bitrate{8000000};
    int m_max_size{0};

    int m_local_port{0};
    int m_remote_port{0};
    bool m_forwarded{false};
    std::atomic<bool> m_running{false};

    QTcpSocket* m_socket{nullptr};
    QTimer* m_heartbeat_timer{nullptr};

    PacketDemuxer m_demuxer;
    std::unique_ptr<VideoDecoder> m_decoder;
    std::shared_ptr<ControlChannel> m_control;

    int m_dev_width{0};
    int m_dev_height{0};

    int pickFreePort();
    bool launchServer();
    void cleanup();
};

} // namespace harmonycast
