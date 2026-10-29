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
#include <iostream>
#include <thread>

namespace harmonycast {

StreamSession::StreamSession(std::shared_ptr<HdcClient> hdc,
                             Device device,
                             int fps,
                             int bitrate,
                             int max_size,
                             QObject* parent)
    : QObject(parent),
      m_hdc(std::move(hdc)),
      m_device(std::move(device)),
      m_fps(fps),
      m_bitrate(bitrate),
      m_max_size(max_size),
      m_socket(new QTcpSocket(this)),
      m_heartbeat_timer(new QTimer(this)),
      m_control(std::make_shared<ControlChannel>(this)) {

    connect(m_socket, &QTcpSocket::connected, this, &StreamSession::onSocketConnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &StreamSession::onSocketReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &StreamSession::onSocketDisconnected);
    connect(m_socket, &QAbstractSocket::errorOccurred, this, &StreamSession::onSocketError);
    connect(m_heartbeat_timer, &QTimer::timeout, this, &StreamSession::sendHeartbeat);

    m_socket->setReadBufferSize(4 * 1024 * 1024);

}

StreamSession::~StreamSession() {
    stop();
}

int StreamSession::pickFreePort() {
    QTcpServer server;
    if (server.listen(QHostAddress::LocalHost, 0)) {
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
                                      int max_size) {
    if (!hdc || serial.empty()) return false;

    std::string arch = "arm64";
    auto res_uname = hdc->run({"shell", "uname", "-m"}, serial, 2000);
    if (res_uname.success() && res_uname.out.find("riscv") != std::string::npos) {
        arch = "riscv64";
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
            local_bin = QFileInfo(path).canonicalFilePath();
            break;
        }
    }

    std::string remote_bin = "/data/local/tmp/harmonycast_capture";
    if (!local_bin.isEmpty()) {
        QFileInfo linfo(local_bin);
        qint64 local_size = linfo.size();
        bool need_push = true;

        // Fast-path: check if binary already exists on remote device with matching file size
        auto check_res = hdc->run({"shell", "ls", "-l", remote_bin}, serial, 2000);
        if (check_res.success() && !check_res.out.empty()) {
            if (check_res.out.find(std::to_string(local_size)) != std::string::npos) {
                need_push = false;
            }
        }

        if (need_push) {
            hdc->fileSend(serial, local_bin.toStdString(), remote_bin);
        }
        hdc->run({"shell", "chmod", "755", remote_bin}, serial, 3000);
    }

    // Kill any previous capture instances
    hdc->run({"shell", "killall", "-9", "harmonycast_capture", "harmonycast_capture_arm64", "2>/dev/null"}, serial, 1000);

    // Launch remote capture server in background
    std::string cmd = QString("%1 -p %2 -f %3 -b %4%5 &")
                          .arg(QString::fromStdString(remote_bin))
                          .arg(remote_port)
                          .arg(fps)
                          .arg(bitrate)
                          .arg(max_size > 0 ? QString(" -m %1").arg(max_size) : "")
                          .toStdString();
    auto res = hdc->run({"shell", cmd}, serial, 3000);
    return res.success();
}

bool StreamSession::start() {
    stop();
    m_running = true;
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
    std::thread([weak_this, current_gen, hdc, serial, local_port, remote_port, fps, bitrate, max_size, cancelled]() {
        if (cancelled->load()) return;
        // 1. Setup port forwarding in worker thread
        if (!hdc || serial.empty() || !hdc->forwardPort(serial, local_port, remote_port)) {
            if (weak_this) {
                QMetaObject::invokeMethod(weak_this.data(), [weak_this, current_gen]() {
                    if (!weak_this || !weak_this->m_running || weak_this->m_session_gen != current_gen) return;
                    emit weak_this->statusChanged("端口映射失败，尝试兼容快照流…");
                    weak_this->startSnapshotFallback();
                });
            }
            return;
        }

        // 2. Launch capture server on device in worker thread
        if (cancelled->load()) return;
        launchServerAsync(hdc, serial, remote_port, fps, bitrate, max_size);

        // 3. Inform main thread to begin non-blocking socket connection attempts
        if (weak_this) {
            QMetaObject::invokeMethod(weak_this.data(), [weak_this, current_gen]() {
                if (!weak_this || !weak_this->m_running || weak_this->m_session_gen != current_gen) return;
                weak_this->m_forwarded = true;
                emit weak_this->statusChanged("正在连接设备视频流…");
                weak_this->tryConnectSocket(0, current_gen);
            });
        }
    }).detach();

    return true;
}

void StreamSession::tryConnectSocket(int attempt, uint64_t gen) {
    if (!m_running || m_session_gen != gen) return;

    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        return;
    }

    if (attempt >= 16) {
        emit statusChanged("硬件采屏未响应，自动切换为兼容快照流 (snapshot_display)…");
        startSnapshotFallback();
        return;
    }

    if (m_socket->state() != QAbstractSocket::UnconnectedState && m_socket->state() != QAbstractSocket::ConnectingState) {
        m_socket->abort();
    }

    if (m_socket->state() == QAbstractSocket::UnconnectedState) {
        m_socket->connectToHost(QHostAddress::LocalHost, m_local_port);
    }

    QTimer::singleShot(250, this, [this, attempt, gen]() {
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
    if (m_start_cancel) m_start_cancel->store(true);
    m_fallback_snapshot = false;
    m_session_gen++;
    m_heartbeat_timer->stop();

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
    connect(m_pipeline, &VideoPipeline::frameReady, m_pipeline, [this, mailbox, generation](const DecodedFrame& frame) {
        if (mailbox->publish(frame)) {
            QMetaObject::invokeMethod(this, [this, mailbox, generation] { deliverLatestFrame(mailbox, generation); }, Qt::QueuedConnection);
        }
    }, Qt::DirectConnection);
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
    m_decode_thread->requestInterruption();
    m_decode_thread->quit();
    m_decode_thread->wait();
    delete m_decode_thread;
    m_decode_thread = nullptr;
    m_pipeline = nullptr;
    m_decode_busy = false;
}

void StreamSession::deliverLatestFrame(const std::shared_ptr<FrameMailbox>& mailbox, uint64_t generation) {
    auto frame = mailbox->take();
    if (!frame || generation != m_session_gen || !m_running) return;
    const int width = frame->source_width > 0 ? frame->source_width : frame->width;
    const int height = frame->source_height > 0 ? frame->source_height : frame->height;
    bool changed = m_dev_width != width || m_dev_height != height;
    m_dev_width = width;
    m_dev_height = height;
    if (changed) updateSnapshotTargetSize();
    emit frameReady(*frame);
}

void StreamSession::onSocketDisconnected() {
    if (m_running && !m_fallback_snapshot) {
        HC_LOG_WARN("StreamSession TCP socket disconnected, attempting snapshot fallback");
        emit statusChanged("设备端硬件投屏长连接中断，自动切换为兼容快照流…");
        startSnapshotFallback();
    }
}

void StreamSession::onSocketError(QAbstractSocket::SocketError socketError) {
    if (!m_running || m_fallback_snapshot) return;
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

void StreamSession::startSnapshotFallback() {
    if (m_fallback_snapshot) return;
    m_fallback_snapshot = true;
    m_heartbeat_timer->stop();
    m_socket->abort();
    stopPipeline();
    if (m_frame_mailbox) m_frame_mailbox->clear();
    m_frame_mailbox = std::make_shared<FrameMailbox>();
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
    std::thread([weak_this, ctx, mailbox, generation]() {
        std::string remote_path = "/data/local/tmp/harmonycast_stream_" + std::to_string(ctx->session_id) + ".jpeg";
        QString local_temp = QDir::tempPath() + QString("/harmonycast_stream_%1_%2.jpeg")
                                .arg(QString::fromStdString(ctx->serial).replace(':', '_').replace('.', '_'))
                                .arg(ctx->session_id);
        std::string local_path = local_temp.toStdString();

        ctx->hdc->wakeup(ctx->serial);

        int consecutive_errors = 0;
        int source_width = 0, source_height = 0;
        while (!ctx->cancelled.load()) {
            auto t0 = std::chrono::steady_clock::now();

            std::vector<std::string> snap_cmd = {"shell", "snapshot_display", "-f", remote_path};
            int tw = ctx->target_width.load();
            int th = ctx->target_height.load();
            if (source_width > 0 && tw > 0 && th > 0) {
                snap_cmd.push_back("-w");
                snap_cmd.push_back(std::to_string(tw));
                snap_cmd.push_back("-h");
                snap_cmd.push_back(std::to_string(th));
            }
            auto res_snap = ctx->hdc->run(snap_cmd, ctx->serial, 3000);
            if (ctx->cancelled.load()) break;

            if (res_snap.success()) {
                bool res_recv = ctx->hdc->fileRecv(ctx->serial, remote_path, local_path);
                if (ctx->cancelled.load()) break;

                if (res_recv && QFile::exists(local_temp)) {
                    QImage img;
                    if (img.load(local_temp)) {
                        consecutive_errors = 0;

                        if (source_width == 0 || tw == 0) {
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
                        if (weak_this && mailbox->publish(frame)) {
                            QMetaObject::invokeMethod(weak_this.data(), [weak_this, ctx, mailbox, generation]() {
                                if (weak_this && !ctx->cancelled.load()) weak_this->deliverLatestFrame(mailbox, generation);
                            });
                        }
                    } else {
                        consecutive_errors++;
                    }
                    QFile::remove(local_temp);
                } else {
                    consecutive_errors++;
                }
            } else {
                consecutive_errors++;
                if (consecutive_errors % 4 == 0) {
                    ctx->hdc->wakeup(ctx->serial);
                }
            }

            if (consecutive_errors >= 5) {
                HC_LOG_ERROR(QString("Snapshot fallback failed %1 consecutive times, terminating session for %2")
                                 .arg(consecutive_errors)
                                 .arg(QString::fromStdString(ctx->serial)));
                if (weak_this) {
                    QMetaObject::invokeMethod(weak_this.data(), [weak_this]() {
                        if (weak_this && weak_this->m_running) {
                            emit weak_this->streamFinished("设备画面流传输异常中断，已自动停止投屏");
                            weak_this->stop();
                        }
                    });
                }
                break;
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            int target_delay = std::clamp(100 - static_cast<int>(elapsed), 10, 100);
            for (int i = 0; i < target_delay / 10 && !ctx->cancelled.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        ctx->hdc->run({"shell", "rm", "-f", remote_path}, ctx->serial, 1000);
        QFile::remove(local_temp);
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
