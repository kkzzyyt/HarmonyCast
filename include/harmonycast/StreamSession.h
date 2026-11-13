#pragma once

#include "Types.h"
#include "HdcClient.h"
#include "PacketDemuxer.h"
#include "VideoDecoder.h"
#include "ControlChannel.h"
#include "FrameMailbox.h"
#include "VideoPipeline.h"
#include "MirrorSession.h"
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>
#include <atomic>
#include <thread>
#include <QThread>

namespace harmonycast {

struct SnapshotWorkerContext {
    std::atomic<bool> cancelled{false};
    std::atomic<int> target_width{0};
    std::atomic<int> target_height{0};
    std::shared_ptr<DeviceClient> hdc;
    std::string serial;
    uintptr_t session_id{0};
    std::mutex presentation_mutex;
    std::mutex diagnostic_mutex;
    QString progress_detail;
    QString last_failure;
    QSize viewport;
    int rotation{0};
    qreal dpr{1};
};

class StreamSession : public MirrorSession {
    Q_OBJECT
public:
    explicit StreamSession(std::shared_ptr<DeviceClient> hdc,
                           Device device,
                           int fps = 60,
                           int bitrate = 8000000,
                           int max_size = 0,
                           QObject* parent = nullptr,
                           StreamTimeouts timeouts = {});
    ~StreamSession() override;

    bool start() override;
    void stop() override;
    [[nodiscard]] bool isRunning() const override;
    [[nodiscard]] bool isConnected() const { return isRunning(); }
    [[nodiscard]] bool isActive() const override { return m_running; }

    [[nodiscard]] const Device& device() const { return m_device; }
    [[nodiscard]] std::shared_ptr<ControlChannel> controlChannel() const override { return m_control; }
    [[nodiscard]] int deviceWidth() const override { return m_dev_width; }
    [[nodiscard]] int deviceHeight() const override { return m_dev_height; }
    [[nodiscard]] bool isFallbackSnapshot() const override { return m_fallback_snapshot; }

    void setQuality(int quality_idx, int bitrate) override;
    [[nodiscard]] int qualityIndex() const { return m_quality_idx; }
    void setBitrate(int b) override;
    void updateSnapshotTargetSize();
    void setFps(int f) { m_fps = f; }
    void setPresentation(QSize viewport, int rotation, qreal dpr) override;

private slots:
    void onSocketConnected();
    void onSocketReadyRead();
    void onSocketDisconnected();
    void onSocketError(QAbstractSocket::SocketError error);
    void sendHeartbeat();

private:
    std::shared_ptr<DeviceClient> m_hdc;
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
    bool m_socket_connected{false};
    QTimer* m_heartbeat_timer{nullptr};
    QTimer* m_progress_timer{nullptr};
    QElapsedTimer m_progress_clock;
    StreamTimeouts m_timeouts;
    bool m_received_frame{false};
    bool m_software_retry_requested{false};
    QString m_software_retry_reason;
    QString m_fallback_reason;

    QThread* m_decode_thread{nullptr};
    VideoPipeline* m_pipeline{nullptr};
    bool m_decode_busy{false};
    std::shared_ptr<FrameMailbox> m_frame_mailbox;
    std::shared_ptr<std::atomic<bool>> m_start_cancel;
    std::shared_ptr<QObject> m_delivery_context;
    QSize m_viewport;
    int m_rotation{0};
    qreal m_dpr{1};
    std::shared_ptr<ControlChannel> m_control;

    int m_dev_width{0};
    int m_dev_height{0};
    int m_quality_idx{0};

    std::atomic<bool> m_fallback_snapshot{false};
    std::shared_ptr<SnapshotWorkerContext> m_snapshot_ctx;

    int pickFreePort();
    static bool launchServerAsync(const std::shared_ptr<DeviceClient>& hdc,
                                  const std::string& serial,
                                  int remote_port,
                                  int fps,
                                  int bitrate,
                                  int max_size,
                                  const std::shared_ptr<std::atomic<bool>>& cancelled,
                                  QString& error);
    void tryConnectSocket(int attempt, uint64_t gen);
    void cleanup();
    void startSnapshotFallback(const QString& reason = {});
    void checkProgress();
    void startPipeline();
    void stopPipeline();
    void deliverLatestFrame(const std::shared_ptr<FrameMailbox>& mailbox, uint64_t generation);
};

} // namespace harmonycast
