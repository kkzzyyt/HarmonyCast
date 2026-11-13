#pragma once

#include "AdbClient.h"
#include "MirrorSession.h"
#include "VideoPipeline.h"
#include "FrameMailbox.h"
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <mutex>

namespace harmonycast {

class AndroidStreamSession final : public MirrorSession {
    Q_OBJECT
public:
    AndroidStreamSession(std::shared_ptr<AdbClient> adb, Device device, int bitrate = 8000000,
                         QObject* parent = nullptr, StreamTimeouts timeouts = {}, QString serverPath = {});
    ~AndroidStreamSession() override;
    bool start() override;
    void stop() override;
    bool isRunning() const override { return m_running && (m_fallback || m_received_frame); }
    bool isActive() const override { return m_running; }
    int deviceWidth() const override { return m_width; }
    int deviceHeight() const override { return m_height; }
    bool isFallbackSnapshot() const override { return m_fallback; }
    std::shared_ptr<ControlChannel> controlChannel() const override { return m_control; }
    void setQuality(int quality, int bitrate) override;
    void setBitrate(int bitrate) override { m_bitrate = bitrate; }
    void setPresentation(QSize viewport, int rotation, qreal dpr) override;
    static QString findServer();
    static constexpr const char* serverVersion = "5.0.1";
    static constexpr const char* serverSha256 = "764eb6f79811d5211fe9df341120882ba9994c7a61b897d7bf3fb662e53bc536";
private:
    struct SnapshotState {
        std::mutex mutex;
        QSize viewport;
        int rotation{0};
        qreal dpr{1};
        QString stage;
        QString last_failure;
        int quality{0};
    };
    std::shared_ptr<AdbClient> m_adb;
    Device m_device;
    std::shared_ptr<ControlChannel> m_control;
    QTcpSocket* m_socket;
    QProcess* m_server{nullptr};
    QTimer m_watchdog;
    QElapsedTimer m_progress;
    StreamTimeouts m_timeouts;
    QString m_server_path;
    QString m_server_output;
    std::string m_remote_path;
    int m_local_port{0};
    int m_bitrate;
    int m_quality{0};
    int m_width{0}, m_height{0};
    bool m_running{false}, m_fallback{false}, m_received_frame{false}, m_decode_busy{false};
    bool m_seen_dummy{false};
    uint64_t m_generation{0};
    QThread* m_decode_thread{nullptr};
    VideoPipeline* m_pipeline{nullptr};
    QSize m_viewport;
    int m_rotation{0};
    qreal m_dpr{1};
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::shared_ptr<QObject> m_delivery;
    std::shared_ptr<FrameMailbox> m_mailbox;
    std::shared_ptr<SnapshotState> m_snapshot;
    void launchServer(const QString& scid);
    void connectVideo(uint64_t generation);
    void consumeVideo();
    void startPipeline();
    void releaseNative();
    void startFallback(const QString& reason);
    void deliver(const std::shared_ptr<FrameMailbox>& mailbox, uint64_t generation);
    void finish(const QString& error);
};

} // namespace harmonycast
