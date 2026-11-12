#include "harmonycast/StreamSession.h"
#include "harmonycast/Logger.h"
#include "harmonycast/FramePresentation.h"
#include <QTcpServer>
#include <QThread>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPointer>
#include <QRegularExpression>
#include <QImageReader>
#include <QNetworkProxy>
#include <iostream>
#include <thread>

namespace harmonycast {

namespace {

bool commandFailed(const CommandResult& result) {
    static const QRegularExpression error(R"(\[Fail\]|\[E[0-9a-f]+\]|\bfailed\b|\berror\b|permission denied)", QRegularExpression::CaseInsensitiveOption);
    return !result.success() || error.match(QString::fromStdString(result.out + "\n" + result.err)).hasMatch();
}

QString commandDetail(const CommandResult& result) {
    return QString::fromStdString(result.err + "\n" + result.out).trimmed().left(16384);
}

QString fallbackSummary(const QString& detail) {
    static const QRegularExpression unavailableApi(
        R"(Error loading shared library|symbol not found|undefined symbol|cannot locate symbol)",
        QRegularExpression::CaseInsensitiveOption);
    if (unavailableApi.match(detail).hasMatch()) return QStringLiteral("设备系统不支持当前原生采屏接口");
    if (detail.isEmpty()) return QStringLiteral("原生视频通道不可用");
    return detail.section('\n', 0, 0).section(QChar(0xff1a), 0, 0).left(120);
}

} // namespace

StreamSession::StreamSession(std::shared_ptr<HdcClient> hdc,
                             Device device,
                             int fps,
                             int bitrate,
                             int max_size,
                             QObject* parent,
                             StreamTimeouts timeouts)
    : QObject(parent),
      m_hdc(std::move(hdc)),
      m_device(std::move(device)),
      m_fps(fps),
      m_bitrate(bitrate),
      m_max_size(max_size),
      m_socket(new QTcpSocket(this)),
      m_heartbeat_timer(new QTimer(this)),
      m_progress_timer(new QTimer(this)),
      m_timeouts{std::max(100, timeouts.startup_ms), std::max(100, timeouts.frame_ms)},
      m_control(std::make_shared<ControlChannel>(this)) {

    // Workers retain this GUI-thread receiver until they finish. A session
    // can be destroyed while HDC is running; dereference its QPointer only
    // after delivery returns to the GUI thread.
    m_delivery_context = std::shared_ptr<QObject>(new QObject, [](QObject* receiver) { receiver->deleteLater(); });
    m_socket->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    connect(m_socket, &QTcpSocket::connected, this, &StreamSession::onSocketConnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &StreamSession::onSocketReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &StreamSession::onSocketDisconnected);
    connect(m_socket, &QAbstractSocket::errorOccurred, this, &StreamSession::onSocketError);
    connect(m_heartbeat_timer, &QTimer::timeout, this, &StreamSession::sendHeartbeat);
    connect(m_progress_timer, &QTimer::timeout, this, &StreamSession::checkProgress);
    m_progress_timer->setInterval(std::clamp(std::min(m_timeouts.startup_ms, m_timeouts.frame_ms) / 4, 25, 1000));
    qRegisterMetaType<DecodedFrame>("harmonycast::DecodedFrame");

    m_socket->setReadBufferSize(4 * 1024 * 1024);

}

StreamSession::~StreamSession() {
    stop();
}

int StreamSession::pickFreePort() {
    QTcpServer server;
    if (server.listen(QHostAddress(QStringLiteral("127.0.0.1")), 0)) {
        quint16 port = server.serverPort();
        server.close();
        return port;
    }
    return 8990;
}

bool StreamSession::launchServerAsync(const std::shared_ptr<HdcClient>& hdc,
                                      const std::string& serial,
                                      int remote_port,
                                      int fps,
                                      int bitrate,
                                      int max_size,
                                      const std::shared_ptr<std::atomic<bool>>& cancelled,
                                      QString& error) {
    if (!hdc || serial.empty() || cancelled->load()) return false;

    std::string arch = "arm64";
    auto res_uname = hdc->run({"shell", "uname", "-m"}, serial, 2000);
    if (cancelled->load()) return false;
    if (commandFailed(res_uname)) {
        error = "无法读取设备架构：" + commandDetail(res_uname);
        return false;
    }
    if (res_uname.out.find("riscv") != std::string::npos) {
        arch = "riscv64";
    } else if (res_uname.out.find("aarch64") == std::string::npos && res_uname.out.find("arm64") == std::string::npos) {
        error = "设备架构没有匹配的原生采屏程序：" + QString::fromStdString(res_uname.out).trimmed();
        return false;
    }

    QString bin_name = (arch == "riscv64") ? "harmonycast_capture_riscv64" : "harmonycast_capture_arm64";

    QStringList candidates = {
        QCoreApplication::applicationDirPath() + "/../Resources/capture/" + bin_name,
        QCoreApplication::applicationDirPath() + "/resources/capture/" + bin_name,
        QDir::currentPath() + "/resources/capture/" + bin_name,
        QDir::currentPath() + "/../resources/capture/" + bin_name
    };

    QString local_bin;
    for (const auto& path : candidates) {
        if (QFile::exists(path)) {
            local_bin = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
            break;
        }
    }

    std::string remote_bin = "/data/local/tmp/harmonycast_capture";
    if (local_bin.isEmpty()) {
        error = "缺少设备采屏程序 " + bin_name + "，请使用完整安装包";
        return false;
    }
    {
        QFileInfo linfo(local_bin);
        qint64 local_size = linfo.size();
        bool need_push = true;

        // Fast-path: check if binary already exists on remote device with matching file size
        auto check_res = hdc->run({"shell", "ls", "-l", remote_bin}, serial, 2000);
        if (cancelled->load()) return false;
        if (check_res.success() && !check_res.out.empty()) {
            if (check_res.out.find(std::to_string(local_size)) != std::string::npos) {
                need_push = false;
            }
        }

        if (need_push) {
            auto sent = hdc->run({"file", "send", local_bin.toStdString(), remote_bin}, serial, 30000);
            if (cancelled->load()) return false;
            if (commandFailed(sent)) {
                error = "采屏程序传输失败：" + commandDetail(sent);
                return false;
            }
        }
        auto chmod = hdc->run({"shell", "chmod", "755", remote_bin}, serial, 3000);
        if (cancelled->load()) return false;
        if (commandFailed(chmod)) {
            error = "采屏程序无法设置执行权限：" + commandDetail(chmod);
            return false;
        }
    }

    // Kill any previous capture instances
    hdc->run({"shell", "pkill -9 harmonycast_capture 2>/dev/null; killall -9 harmonycast_capture 2>/dev/null; pkill -9 harmonycast_capture_arm64 2>/dev/null; killall -9 harmonycast_capture_arm64 2>/dev/null"}, serial, 1500);
    if (cancelled->load()) return false;

    // Launch remote capture server in background with output redirected to avoid blocking hdc shell
    std::string cmd = QString("nohup %1 -p %2 -f %3 -b %4%5 >/data/local/tmp/harmonycast_capture_start.log 2>&1 & "
                             "pid=$!; sleep 1; if kill -0 \"$pid\" 2>/dev/null; then echo __HC_CAPTURE_READY__; "
                             "else echo __HC_CAPTURE_FAILED__; cat /data/local/tmp/harmonycast_capture_start.log; fi")
                          .arg(QString::fromStdString(remote_bin))
                          .arg(remote_port)
                          .arg(fps)
                          .arg(bitrate)
                          .arg(max_size > 0 ? QString(" -m %1").arg(max_size) : "")
                          .toStdString();
    auto res = hdc->run({"shell", cmd}, serial, 3000);
    if (cancelled->load()) return false;
    QStringList lines = QString::fromStdString(res.out).replace('\r', "").split('\n');
    if (commandFailed(res) || !lines.contains("__HC_CAPTURE_READY__")) {
        error = "设备采屏程序未能启动：" + commandDetail(res);
        return false;
    }
    return true;
}

bool StreamSession::start() {
    stop();
    m_running = true;
    m_socket_connected = false;
    m_received_frame = false;
    m_software_retry_requested = false;
    m_fallback_reason.clear();
    m_progress_clock.start();
    m_progress_timer->start();
    uint64_t current_gen = ++m_session_gen;
    HC_LOG_INFO(QString("StreamSession starting for device %1 (fps=%2, bitrate=%3)").arg(QString::fromStdString(m_device.serial)).arg(m_fps).arg(m_bitrate));
    emit statusChanged("正在准备投屏服务…");

    m_local_port = pickFreePort();
    m_remote_port = m_local_port;

    startPipeline();

    auto hdc = m_hdc;
    std::string serial = m_device.serial;
    int local_port = m_local_port;
    int remote_port = m_remote_port;
    int fps = m_fps;
    int bitrate = m_bitrate;
    int max_size = m_max_size;
    QPointer<StreamSession> weak_this(this);

    m_start_cancel = std::make_shared<std::atomic<bool>>(false);
    auto cancelled = m_start_cancel;
    auto delivery = m_delivery_context;
    std::thread([delivery, weak_this, current_gen, hdc, serial, local_port, remote_port, fps, bitrate, max_size, cancelled]() {
        if (cancelled->load()) return;
        // 1. Setup port forwarding in worker thread
        if (!hdc || serial.empty() || !hdc->forwardPort(serial, local_port, remote_port)) {
            QMetaObject::invokeMethod(delivery.get(), [weak_this, current_gen]() {
                if (!weak_this || !weak_this->m_running || weak_this->m_session_gen != current_gen || weak_this->m_fallback_snapshot) return;
                emit weak_this->statusChanged("端口映射失败，尝试兼容快照流…");
                weak_this->startSnapshotFallback();
            });
            return;
        }

        // If cancelled after forwardPort, remove forward task immediately
        if (cancelled->load()) {
            if (local_port > 0) hdc->removeForward(serial, local_port, remote_port);
            return;
        }

        // 2. Launch capture server on device in worker thread
        QString launch_error;
        const bool launched = launchServerAsync(hdc, serial, remote_port, fps, bitrate, max_size, cancelled, launch_error);

        if (cancelled->load()) {
            if (local_port > 0) hdc->removeForward(serial, local_port, remote_port);
            return;
        }

        // 3. Inform main thread to begin non-blocking socket connection attempts
        QMetaObject::invokeMethod(delivery.get(), [weak_this, current_gen, launched, launch_error]() {
            if (!weak_this || !weak_this->m_running || weak_this->m_session_gen != current_gen || weak_this->m_fallback_snapshot) return;
            weak_this->m_forwarded = true;
            if (!launched) {
                weak_this->startSnapshotFallback(launch_error);
                return;
            }
            emit weak_this->statusChanged("正在连接设备视频流…");
            weak_this->tryConnectSocket(0, current_gen);
        });
    }).detach();

    return true;
}

void StreamSession::tryConnectSocket(int attempt, uint64_t gen) {
    if (!m_running || m_session_gen != gen || m_fallback_snapshot) return;

    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket_connected = true;
        return;
    }

    if (attempt >= 24) {
        emit statusChanged("硬件采屏未响应，自动切换为兼容快照流 (snapshot_display)…");
        startSnapshotFallback();
        return;
    }

    if (m_socket->state() != QAbstractSocket::UnconnectedState && m_socket->state() != QAbstractSocket::ConnectingState) {
        m_socket->abort();
    }

    if (m_socket->state() == QAbstractSocket::UnconnectedState) {
        m_socket->connectToHost(QHostAddress(QStringLiteral("127.0.0.1")), m_local_port);
    }

    QTimer::singleShot(300, this, [this, attempt, gen]() {
        if (!m_running || m_session_gen != gen) return;
        if (m_socket->state() != QAbstractSocket::ConnectedState) {
            tryConnectSocket(attempt + 1, gen);
        }
    });
}

void StreamSession::stop() {
    const bool needs_cleanup = m_running || m_forwarded || m_snapshot_ctx;
    if (m_running) {
        HC_LOG_INFO(QString("StreamSession stopped for %1").arg(QString::fromStdString(m_device.serial)));
    }
    m_running = false;
    m_socket_connected = false;
    if (m_start_cancel) m_start_cancel->store(true);
    m_fallback_snapshot = false;
    m_session_gen++;
    m_heartbeat_timer->stop();
    m_progress_timer->stop();
    m_progress_clock.invalidate();

    if (m_snapshot_ctx) {
        m_snapshot_ctx->cancelled = true;
        m_snapshot_ctx.reset();
    }

    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();
    }
    m_control->disconnectFromHost();

    stopPipeline();
    if (m_frame_mailbox) m_frame_mailbox->clear();
    m_frame_mailbox.reset();
    if (needs_cleanup) cleanup();
}

bool StreamSession::isRunning() const {
    return m_running && (m_fallback_snapshot || m_socket->state() == QAbstractSocket::ConnectedState);
}

void StreamSession::cleanup() {
    if (!m_hdc || m_device.serial.empty()) return;

    auto hdc = m_hdc;
    std::string serial = m_device.serial;
    int local_port = m_local_port;
    int remote_port = m_remote_port;
    m_forwarded = false;

    std::thread([hdc, serial, local_port, remote_port]() {
        if (local_port > 0) {
            hdc->removeForward(serial, local_port, remote_port);
        }
        hdc->run({"shell", "killall", "-9", "harmonycast_capture", "2>/dev/null"}, serial, 1000);
    }).detach();
}

void StreamSession::onSocketConnected() {
    if (!m_running || m_fallback_snapshot) return;
    m_socket_connected = true;
    m_progress_clock.restart();
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 2 * 1024 * 1024);
    m_heartbeat_timer->start(2000);
    emit statusChanged("投屏连接就绪，等待首帧…");
}

void StreamSession::onSocketReadyRead() {
    if (!m_running || m_fallback_snapshot || m_decode_busy || !m_pipeline) return;
    QByteArray data = m_socket->read(256 * 1024);
    if (data.isEmpty()) return;
    m_decode_busy = true;
    auto* pipeline = m_pipeline;
    QMetaObject::invokeMethod(pipeline, [pipeline, data = std::move(data)] { pipeline->consume(data); }, Qt::QueuedConnection);
}

void StreamSession::startPipeline() {
    m_frame_mailbox = std::make_shared<FrameMailbox>();
    m_decode_thread = new QThread(this);
    m_pipeline = new VideoPipeline;
    m_pipeline->moveToThread(m_decode_thread);
    connect(m_decode_thread, &QThread::finished, m_pipeline, &QObject::deleteLater);
    const uint64_t generation = m_session_gen;
    auto mailbox = m_frame_mailbox;
    // consume() is serialized by m_decode_busy, so only one owning frame can be
    // queued per batch. A receiver context also drops delivery after shutdown.
    connect(m_pipeline, &VideoPipeline::frameReady, this, [this, mailbox, generation](const DecodedFrame& frame) {
        if (mailbox->publish(frame)) deliverLatestFrame(mailbox, generation);
    }, Qt::QueuedConnection);
    connect(m_pipeline, &VideoPipeline::batchProcessed, this, [this, generation] {
        if (generation != m_session_gen) return;
        m_decode_busy = false;
        onSocketReadyRead();
    });
    connect(m_pipeline, &VideoPipeline::handshakeRequested, this, [this, generation] {
        if (generation != m_session_gen || !m_running || m_fallback_snapshot) return;
        m_socket->write("CONFIG_ACK\n");
        emit statusChanged("设备握手成功，开始接收流");
    });
    connect(m_pipeline, &VideoPipeline::decoderFallback, this, [this, generation](const QString& reason) {
        if (generation != m_session_gen || !m_running || m_fallback_snapshot) return;
        m_software_retry_requested = true;
        m_progress_clock.restart();
        HC_LOG_WARN("Video decoder recovery: " + reason);
        emit statusChanged(reason);
    });
    connect(m_pipeline, &VideoPipeline::softwareRetryCompleted, this, [this, generation](bool recovered) {
        if (generation != m_session_gen || !m_running || m_fallback_snapshot) return;
        if (!recovered) startSnapshotFallback(m_software_retry_reason);
    });
    m_decode_thread->start();
    setPresentation(m_viewport, m_rotation, m_dpr);
}

void StreamSession::setPresentation(QSize viewport, int rotation, qreal dpr) {
    m_viewport = viewport;
    m_rotation = rotation;
    m_dpr = dpr;
    if (m_pipeline) {
        auto* pipeline = m_pipeline;
        QMetaObject::invokeMethod(pipeline, [pipeline, viewport, rotation, dpr] { pipeline->setPresentation(viewport, rotation, dpr); }, Qt::QueuedConnection);
    }
    if (m_snapshot_ctx) {
        std::lock_guard lock(m_snapshot_ctx->presentation_mutex);
        m_snapshot_ctx->viewport = viewport;
        m_snapshot_ctx->rotation = rotation;
        m_snapshot_ctx->dpr = dpr;
    }
}

void StreamSession::stopPipeline() {
    if (!m_decode_thread) return;
    auto* thread = m_decode_thread;
    m_decode_thread = nullptr;
    m_pipeline = nullptr;
    m_decode_busy = false;
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->requestInterruption();
    thread->quit();
    if (thread->wait(1000)) {
        delete thread;
    } else {
        thread->setParent(nullptr);
        HC_LOG_WARN("Video decoder thread did not stop in time; UI recovery will continue without waiting for the driver");
    }
}

void StreamSession::deliverLatestFrame(const std::shared_ptr<FrameMailbox>& mailbox, uint64_t generation) {
    auto frame = mailbox->take();
    if (!frame || generation != m_session_gen || !m_running || mailbox != m_frame_mailbox) return;
    const bool first_frame = !m_received_frame;
    m_received_frame = true;
    m_progress_clock.restart();
    const int width = frame->source_width > 0 ? frame->source_width : frame->width;
    const int height = frame->source_height > 0 ? frame->source_height : frame->height;
    if (first_frame) {
        HC_LOG_INFO(QString("First displayable frame received: %1x%2 (%3)")
            .arg(width).arg(height).arg(m_fallback_snapshot ? "snapshot" : "native video"));
        if (m_fallback_snapshot) {
            const QPointer<StreamSession> guard(this);
            emit statusChanged("已启用兼容截图投屏");
            if (!guard || !m_running || generation != m_session_gen || mailbox != m_frame_mailbox) return;
        }
    }
    bool changed = m_dev_width != width || m_dev_height != height;
    m_dev_width = width;
    m_dev_height = height;
    if (changed) updateSnapshotTargetSize();
    emit frameReady(*frame);
}

void StreamSession::onSocketDisconnected() {
    if (m_running && !m_fallback_snapshot && m_socket_connected) {
        HC_LOG_WARN("StreamSession TCP socket disconnected, attempting snapshot fallback");
        emit statusChanged("设备端硬件投屏长连接中断，自动切换为兼容快照流…");
        startSnapshotFallback();
    }
}

void StreamSession::onSocketError(QAbstractSocket::SocketError socketError) {
    if (!m_running || m_fallback_snapshot) return;

    // During initial connection attempts before socket is connected, ConnectionRefused
    // and host errors are expected while remote harmonycast_capture server initializes.
    // tryConnectSocket will retry until connection succeeds or max attempts (16) is reached.
    if (!m_socket_connected && (socketError == QAbstractSocket::ConnectionRefusedError ||
                               socketError == QAbstractSocket::HostNotFoundError ||
                               socketError == QAbstractSocket::RemoteHostClosedError)) {
        return;
    }

    QString errStr = m_socket ? m_socket->errorString() : "未知网络套接字错误";
    HC_LOG_WARN(QString("StreamSession socket error (%1): %2").arg(socketError).arg(errStr));
    emit statusChanged(QString("视频流连接异常 (%1)，尝试降级至兼容快照流…").arg(errStr));
    startSnapshotFallback();
}

void StreamSession::setQuality(int quality_idx, int bitrate) {
    m_quality_idx = std::clamp(quality_idx, 0, 3);
    m_bitrate = bitrate;
    updateSnapshotTargetSize();
}

void StreamSession::setBitrate(int b) {
    m_bitrate = b;
    if (b >= 20000000) m_quality_idx = 0;
    else if (b >= 12000000) m_quality_idx = 1;
    else if (b >= 6000000) m_quality_idx = 2;
    else m_quality_idx = 3;
    updateSnapshotTargetSize();
}

void StreamSession::updateSnapshotTargetSize() {
    if (!m_snapshot_ctx) return;

    // Index 0: 超清原画 (点对点无损) -> Native resolution
    if (m_quality_idx == 0 || m_bitrate >= 20000000) {
        m_snapshot_ctx->target_width.store(0);
        m_snapshot_ctx->target_height.store(0);
        HC_LOG_INFO("Snapshot fallback mode target size set to Native full resolution (0x0)");
        return;
    }

    bool portrait = (m_dev_width > 0 && m_dev_height > 0) ? (m_dev_width < m_dev_height) : true;
    int dev_short = (m_dev_width > 0 && m_dev_height > 0) ? std::min(m_dev_width, m_dev_height) : 1080;
    int dev_long = (m_dev_width > 0 && m_dev_height > 0) ? std::max(m_dev_width, m_dev_height) : 1920;

    // Index 1: 超清 900P (清晰均衡)
    if (m_quality_idx == 1 || m_bitrate >= 12000000) {
        int short_side = (dev_short > 1080) ? 1080 : 900;
        int long_side = static_cast<int>(static_cast<double>(short_side) * dev_long / dev_short);
        int tw = portrait ? short_side : long_side;
        int th = portrait ? long_side : short_side;
        m_snapshot_ctx->target_width.store(tw);
        m_snapshot_ctx->target_height.store(th);
        HC_LOG_INFO(QString("Snapshot fallback mode target size set to %1x%2 (%3P)").arg(tw).arg(th).arg(short_side));
        return;
    }

    // Index 2: 高清 720P (流畅提速)
    if (m_quality_idx == 2 || m_bitrate >= 6000000) {
        int short_side = 720;
        int long_side = static_cast<int>(static_cast<double>(short_side) * dev_long / dev_short);
        int tw = portrait ? short_side : long_side;
        int th = portrait ? long_side : short_side;
        m_snapshot_ctx->target_width.store(tw);
        m_snapshot_ctx->target_height.store(th);
        HC_LOG_INFO(QString("Snapshot fallback mode target size set to %1x%2 (720P)").arg(tw).arg(th));
        return;
    }

    // Index 3: 标清 540P (极速省流)
    {
        int short_side = 540;
        int long_side = static_cast<int>(static_cast<double>(short_side) * dev_long / dev_short);
        int tw = portrait ? short_side : long_side;
        int th = portrait ? long_side : short_side;
        m_snapshot_ctx->target_width.store(tw);
        m_snapshot_ctx->target_height.store(th);
        HC_LOG_INFO(QString("Snapshot fallback mode target size set to %1x%2 (540P)").arg(tw).arg(th));
    }
}

void StreamSession::checkProgress() {
    if (!m_running || !m_progress_clock.isValid()) return;
    const int timeout = !m_fallback_snapshot && !m_socket_connected ? m_timeouts.startup_ms : m_timeouts.frame_ms;
    if (m_progress_clock.elapsed() < timeout) return;
    if (m_fallback_snapshot) {
        QString stage;
        if (m_snapshot_ctx) {
            std::lock_guard lock(m_snapshot_ctx->diagnostic_mutex);
            stage = m_snapshot_ctx->progress_detail;
            if (!m_snapshot_ctx->last_failure.isEmpty() && m_snapshot_ctx->last_failure != stage) {
                stage += "\n最近失败：" + m_snapshot_ctx->last_failure;
            }
        }
        const QString error = QString("兼容截图在 %1 秒内未返回画面，请检查设备截图权限和 HDC 文件传输\n%2")
            .arg(timeout / 1000.0, 0, 'g', 3).arg(stage.isEmpty() ? m_fallback_reason : stage);
        HC_LOG_WARN(error);
        stop();
        emit streamFinished(error);
        return;
    }
    const QString reason = !m_socket_connected ? QStringLiteral("投屏服务准备超时，采屏程序或端口映射未就绪") :
        m_received_frame ? QStringLiteral("视频通道没有继续返回画面") : QStringLiteral("视频通道已连接，但一直未收到可显示的首帧");
    if (m_socket_connected && m_pipeline && !m_software_retry_requested) {
        m_software_retry_requested = true;
        m_progress_clock.restart();
        m_software_retry_reason = reason;
        auto* pipeline = m_pipeline;
        QMetaObject::invokeMethod(pipeline, [pipeline] { pipeline->requestSoftwareDecoder(); });
        return;
    }
    startSnapshotFallback(reason);
}

void StreamSession::startSnapshotFallback(const QString& reason) {
    if (m_fallback_snapshot) return;
    if (!reason.isEmpty()) HC_LOG_WARN("Native capture fallback details: " + reason);
    m_fallback_reason = fallbackSummary(reason);
    m_fallback_snapshot = true;
    if (m_start_cancel) m_start_cancel->store(true);
    m_received_frame = false;
    m_progress_clock.restart();
    m_heartbeat_timer->stop();
    m_socket->abort();
    stopPipeline();
    if (m_frame_mailbox) m_frame_mailbox->clear();
    m_frame_mailbox = std::make_shared<FrameMailbox>();
    emit statusChanged(m_fallback_reason + "\n正在切换兼容截图，请稍候…");
    if (m_forwarded) cleanup();
    if (!QImageReader::supportedImageFormats().contains("jpeg")) {
        const QString error = "缺少 JPEG 图像插件，无法启用兼容截图，请使用完整安装包";
        stop();
        emit streamFinished(error);
        return;
    }
    if (!m_hdc) {
        stop();
        emit streamFinished("HDC 工具不可用，无法获取兼容截图");
        return;
    }
    HC_LOG_WARN(QString("StreamSession starting snapshot fallback mode for %1").arg(QString::fromStdString(m_device.serial)));

    if (m_snapshot_ctx) {
        m_snapshot_ctx->cancelled = true;
        m_snapshot_ctx.reset();
    }

    auto ctx = std::make_shared<SnapshotWorkerContext>();
    ctx->hdc = m_hdc;
    ctx->serial = m_device.serial;
    static std::atomic<uintptr_t> next_snapshot_id{0};
    ctx->session_id = ++next_snapshot_id;
    m_snapshot_ctx = ctx;
    setPresentation(m_viewport, m_rotation, m_dpr);
    updateSnapshotTargetSize();
    QPointer<StreamSession> weak_this(this);

    auto mailbox = m_frame_mailbox;
    const uint64_t generation = m_session_gen;
    auto delivery = m_delivery_context;
    std::thread([delivery, weak_this, ctx, mailbox, generation]() {
        std::string remote_path = "/data/local/tmp/harmonycast_stream_" + std::to_string(ctx->session_id) + ".jpeg";
        QString local_temp[2] = {
            QDir::toNativeSeparators(QDir::tempPath() + QString("/hc_snap_%1_%2_0.jpeg")
                .arg(QString::fromStdString(ctx->serial).replace(':', '_').replace('.', '_'))
                .arg(ctx->session_id)),
            QDir::toNativeSeparators(QDir::tempPath() + QString("/hc_snap_%1_%2_1.jpeg")
                .arg(QString::fromStdString(ctx->serial).replace(':', '_').replace('.', '_'))
                .arg(ctx->session_id))
        };
        int buf_idx = 0;

        auto stage = [ctx](const QString& detail) {
            std::lock_guard lock(ctx->diagnostic_mutex);
            ctx->progress_detail = detail;
        };
        stage("正在唤醒设备以获取兼容截图");
        ctx->hdc->wakeup(ctx->serial);

        auto finish = [delivery, weak_this, ctx, generation](const QString& error) {
            QMetaObject::invokeMethod(delivery.get(), [weak_this, ctx, generation, error]() {
                if (!weak_this || !weak_this->m_running || ctx->cancelled.load() ||
                    weak_this->m_session_gen != generation || weak_this->m_snapshot_ctx != ctx) return;
                weak_this->stop();
                if (weak_this) emit weak_this->streamFinished(error);
            });
        };
        const QRegularExpression resizeUnsupported(
            R"(invalid option|unknown option|unrecognized option|unsupported option)",
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression missingCommand(R"(snapshot_display[^\n]*(not found|inaccessible|no such file or directory))",
                                                 QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression denied(R"(permission denied|access denied|operation not permitted)",
                                        QRegularExpression::CaseInsensitiveOption);
        int consecutive_errors = 0;
        int source_width = 0, source_height = 0;
        bool resize_supported = true;
        while (!ctx->cancelled.load()) {
            auto t0 = std::chrono::steady_clock::now();
            // Delete the previous device image in the same shell invocation.
            // A failing capture must never look successful by reusing that file.
            std::string command = "rm -f " + remote_path + " && snapshot_display -f " + remote_path;
            const int tw = ctx->target_width.load();
            const int th = ctx->target_height.load();
            const bool scaled = resize_supported && source_width > 0 && tw > 0 && th > 0;
            if (scaled) command += " -w " + std::to_string(tw) + " -h " + std::to_string(th);
            stage("正在执行设备截图命令");
            const auto res_snap = ctx->hdc->run({"shell", command}, ctx->serial, 8000);
            if (ctx->cancelled.load()) break;

            QString failure;
            QString detail = commandDetail(res_snap);
            bool permanent = false;
            bool loaded = false;
            if (commandFailed(res_snap) || resizeUnsupported.match(detail).hasMatch() ||
                missingCommand.match(detail).hasMatch() || denied.match(detail).hasMatch()) {
                if (scaled && resizeUnsupported.match(detail).hasMatch()) {
                    resize_supported = false;
                    HC_LOG_WARN("Device screenshot scaling unsupported; retrying at native size: " + detail);
                    continue;
                }
                if (denied.match(detail).hasMatch()) {
                    failure = "设备未授予截图权限，兼容截图无法继续";
                    permanent = true;
                } else if (missingCommand.match(detail).hasMatch()) {
                    failure = "设备截图命令不可用，无法启用兼容截图";
                    permanent = true;
                } else {
                    failure = "兼容截图命令执行失败";
                }
            } else {
                const QString current_temp = local_temp[buf_idx];
                buf_idx = 1 - buf_idx;
                // HDC can report success without creating a destination file.
                // Remove each reused buffer before receiving a fresh image.
                QFile destination(current_temp);
                if (destination.exists() && !destination.remove()) {
                    failure = "兼容截图本地旧文件无法清理";
                    detail = destination.errorString();
                } else {
                    stage("兼容截图传输中");
                    const auto received = ctx->hdc->run({"file", "recv", remote_path, current_temp.toStdString()},
                                                        ctx->serial, 8000);
                    if (ctx->cancelled.load()) break;
                    if (commandFailed(received) || !QFile::exists(current_temp)) {
                        failure = "兼容截图传输失败，未收到新的截图文件";
                        detail = commandDetail(received);
                    } else {
                        QFile file(current_temp);
                        if (!file.open(QIODevice::ReadOnly)) {
                            failure = "兼容截图文件无法读取";
                            detail = file.errorString();
                        } else {
                            const auto data = file.readAll();
                            file.close();
                            QImage img;
                            const bool complete = data.size() >= 4 &&
                                static_cast<uint8_t>(data[0]) == 0xFF && static_cast<uint8_t>(data[1]) == 0xD8 &&
                                static_cast<uint8_t>(data[data.size() - 2]) == 0xFF &&
                                static_cast<uint8_t>(data[data.size() - 1]) == 0xD9;
                            if (!complete || !img.loadFromData(data, "JPEG")) {
                                failure = "兼容截图图片不完整或无法解码";
                                detail = QString("Received %1 bytes from %2").arg(data.size()).arg(current_temp);
                            } else {
                                loaded = true;
                                if (source_width == 0 || !scaled) {
                                    source_width = img.width();
                                    source_height = img.height();
                                }
                                DecodedFrame frame;
                                frame.image = img.format() == QImage::Format_RGB32 ? img : img.convertToFormat(QImage::Format_RGB32);
                                frame.width = frame.image.width();
                                frame.height = frame.image.height();
                                frame.stride = int(frame.image.bytesPerLine());
                                frame.format = 4;
                                frame.source_width = source_width;
                                frame.source_height = source_height;
                                frame.is_keyframe = true;
                                QSize viewport;
                                int rotation;
                                qreal dpr;
                                {
                                    std::lock_guard lock(ctx->presentation_mutex);
                                    viewport = ctx->viewport;
                                    rotation = ctx->rotation;
                                    dpr = ctx->dpr;
                                }
                                prepareFramePresentation(frame, viewport, rotation, dpr);
                                if (mailbox->publish(frame)) {
                                    QMetaObject::invokeMethod(delivery.get(), [weak_this, ctx, mailbox, generation]() {
                                        if (weak_this && !ctx->cancelled.load()) weak_this->deliverLatestFrame(mailbox, generation);
                                    });
                                }
                            }
                        }
                    }
                }
            }
            stage(loaded ? QString{} : failure);
            {
                std::lock_guard lock(ctx->diagnostic_mutex);
                ctx->last_failure = loaded ? QString{} : failure;
            }
            if (loaded) {
                consecutive_errors = 0;
            } else {
                ++consecutive_errors;
                HC_LOG_WARN(QString("%1 for %2 (consecutive=%3): %4")
                    .arg(failure, QString::fromStdString(ctx->serial)).arg(consecutive_errors).arg(detail));
                if (permanent || consecutive_errors >= 25) {
                    finish(failure + "\n请检查设备和 HDC 连接，详细原因见诊断日志");
                    break;
                }
                const int backoff_ms = std::min(800, 150 + consecutive_errors * 30);
                for (int delay = 0; delay < backoff_ms && !ctx->cancelled.load(); delay += 10)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            const int target_delay = std::clamp(100 - static_cast<int>(elapsed), 10, 100);
            for (int delay = 0; delay < target_delay && !ctx->cancelled.load(); delay += 10)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ctx->hdc->run({"shell", "rm", "-f", remote_path}, ctx->serial, 1000);
        QFile::remove(local_temp[0]);
        QFile::remove(local_temp[1]);
    }).detach();
}

void StreamSession::sendHeartbeat() {
    if (m_socket && m_socket->isOpen()) {
        uint32_t hb[2] = {0, 0};
        m_socket->write(reinterpret_cast<const char*>(hb), sizeof(hb));
        m_socket->flush();
    }
}

} // namespace harmonycast
