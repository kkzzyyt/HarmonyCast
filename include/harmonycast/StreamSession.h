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
#include <thread>

namespace harmonycast {

struct SnapshotWorkerContext {
    std::atomic<bool> cancelled{false};
    std::shared_ptr<HdcClient> hdc;
    std::string serial;
    uintptr_t session_id{0};
};

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
    [[nodiscard]] bool isFallbackSnapshot() const { return m_fallback_snapshot; }

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
    uint64_t m_session_gen{0};

    QTcpSocket* m_socket{nullptr};
    QTimer* m_heartbeat_timer{nullptr};

    PacketDemuxer m_demuxer;
    std::unique_ptr<VideoDecoder> m_decoder;
    std::shared_ptr<ControlChannel> m_control;

    int m_dev_width{0};
    int m_dev_height{0};

    std::atomic<bool> m_fallback_snapshot{false};
    std::shared_ptr<SnapshotWorkerContext> m_snapshot_ctx;

    int pickFreePort();
    static bool launchServerAsync(const std::shared_ptr<HdcClient>& hdc,
                                  const std::string& serial,
                                  int remote_port,
                                  int fps,
                                  int bitrate,
                                  int max_size);
    void tryConnectSocket(int attempt, uint64_t gen);
    void cleanup();
    void startSnapshotFallback();
};

} // namespace harmonycast
