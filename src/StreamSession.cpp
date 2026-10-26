#include "harmonycast/StreamSession.h"
#include "harmonycast/Logger.h"
#include <QTcpServer>
#include <QThread>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPointer>
#include <iostream>

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
      m_decoder(std::make_unique<VideoDecoder>()),
      m_control(std::make_shared<ControlChannel>(this)) {

    connect(m_socket, &QTcpSocket::connected, this, &StreamSession::onSocketConnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &StreamSession::onSocketReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &StreamSession::onSocketDisconnected);
    connect(m_heartbeat_timer, &QTimer::timeout, this, &StreamSession::sendHeartbeat);

    m_demuxer.setHeaderCallback([this](const std::string& name, int w, int h) {
        m_dev_width = w;
        m_dev_height = h;
        emit statusChanged(QString("画面已同步 · %1 × %2 (%3)")
                               .arg(w).arg(h).arg(QString::fromStdString(name)));
    });

    m_demuxer.setRawFrameCallback([this](const DecodedFrame& frame) {
        m_dev_width = frame.width;
        m_dev_height = frame.height;
        emit frameReady(frame);
    });

    m_demuxer.setHandshakeCallback([this](const std::string&) {
        if (m_socket && m_socket->isOpen()) {
            m_socket->write("CONFIG_ACK\n");
            m_socket->flush();
        }
        emit statusChanged("设备握手成功，开始接收流");
    });

    m_demuxer.setPacketCallback([this](const StreamPacket& pkt) {
        if (!m_decoder || !m_decoder->isReady()) return;
        if (pkt.type == 3 || pkt.type == 4 || pkt.type == 1 || pkt.type == 2) {
            m_decoder->decode(pkt.payload, pkt.is_keyframe, pkt.pts);
        }
    });

    m_decoder->setFrameCallback([this](const DecodedFrame& frame) {
        emit frameReady(frame);
    });
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
        hdc->fileSend(serial, local_bin.toStdString(), remote_bin);
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

    // Initialize hardware-accelerated video decoder
    if (!m_decoder->init(1080, 1920, true)) {
        emit statusChanged("硬件加速初始化降级为多线程软解");
        m_decoder->init(1080, 1920, false);
    }

    auto hdc = m_hdc;
    std::string serial = m_device.serial;
    int local_port = m_local_port;
    int remote_port = m_remote_port;
    int fps = m_fps;
    int bitrate = m_bitrate;
    int max_size = m_max_size;
    QPointer<StreamSession> weak_this(this);

    std::thread([weak_this, current_gen, hdc, serial, local_port, remote_port, fps, bitrate, max_size]() {
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

    if (attempt >= 8) {
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
    if (m_running) {
        HC_LOG_INFO(QString("StreamSession stopped for %1").arg(QString::fromStdString(m_device.serial)));
    }
    m_running = false;
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

    cleanup();
    if (m_decoder) {
        m_decoder->close();
    }
    m_demuxer.reset();
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
    bool forwarded = m_forwarded;
    m_forwarded = false;

    std::thread([hdc, serial, local_port, remote_port, forwarded]() {
        if (forwarded) {
            hdc->removeForward(serial, local_port, remote_port);
        }
        hdc->run({"shell", "killall", "-9", "harmonycast_capture", "2>/dev/null"}, serial, 1000);
        hdc->run({"shell", "rm", "-f", "/data/local/tmp/harmonycast_stream.jpeg"}, serial, 1000);
    }).detach();
}

void StreamSession::onSocketConnected() {
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 2 * 1024 * 1024);
    m_heartbeat_timer->start(2000);
    emit statusChanged("投屏连接就绪，等待首帧…");
}

void StreamSession::onSocketReadyRead() {
    if (!m_running) return;
    QByteArray data = m_socket->readAll();
    if (!data.isEmpty()) {
        m_demuxer.feed(std::span(reinterpret_cast<const uint8_t*>(data.constData()), data.size()));
    }
}

void StreamSession::onSocketDisconnected() {
    if (m_running && !m_fallback_snapshot) {
        emit statusChanged("设备端硬件投屏中断，自动切换为兼容快照流…");
        startSnapshotFallback();
    }
}

void StreamSession::setBitrate(int b) {
    m_bitrate = b;
    updateSnapshotTargetSize();
}

void StreamSession::updateSnapshotTargetSize() {
    if (!m_snapshot_ctx) return;

    if (m_bitrate >= 12000000) {
        m_snapshot_ctx->target_width.store(0);
        m_snapshot_ctx->target_height.store(0);
        HC_LOG_INFO("Snapshot fallback mode target size set to Native full resolution (0x0)");
        return;
    }

    bool portrait = (m_dev_width > 0 && m_dev_height > 0) ? (m_dev_width < m_dev_height) : true;

    if (m_bitrate >= 6000000) {
        int tw = portrait ? 720 : 1280;
        int th = portrait ? 1280 : 720;
        m_snapshot_ctx->target_width.store(tw);
        m_snapshot_ctx->target_height.store(th);
        HC_LOG_INFO(QString("Snapshot fallback mode target size set to %1x%2 (720P)").arg(tw).arg(th));
    } else {
        int tw = portrait ? 540 : 960;
        int th = portrait ? 960 : 540;
        m_snapshot_ctx->target_width.store(tw);
        m_snapshot_ctx->target_height.store(th);
        HC_LOG_INFO(QString("Snapshot fallback mode target size set to %1x%2 (540P)").arg(tw).arg(th));
    }
}

void StreamSession::startSnapshotFallback() {
    if (m_fallback_snapshot) return;
    m_fallback_snapshot = true;
    HC_LOG_WARN(QString("StreamSession starting snapshot fallback mode for %1").arg(QString::fromStdString(m_device.serial)));

    if (m_snapshot_ctx) {
        m_snapshot_ctx->cancelled = true;
        m_snapshot_ctx.reset();
    }

    auto ctx = std::make_shared<SnapshotWorkerContext>();
    ctx->hdc = m_hdc;
    ctx->serial = m_device.serial;
    ctx->session_id = reinterpret_cast<uintptr_t>(this);
    m_snapshot_ctx = ctx;
    updateSnapshotTargetSize();
    QPointer<StreamSession> weak_this(this);

    std::thread([weak_this, ctx]() {
        std::string remote_path = "/data/local/tmp/harmonycast_stream.jpeg";
        QString local_temp = QDir::tempPath() + QString("/harmonycast_stream_%1_%2.jpeg")
                                .arg(QString::fromStdString(ctx->serial).replace(':', '_').replace('.', '_'))
                                .arg(ctx->session_id);
        std::string local_path = local_temp.toStdString();

        ctx->hdc->wakeup(ctx->serial);

        int consecutive_errors = 0;
        while (!ctx->cancelled.load()) {
            auto t0 = std::chrono::steady_clock::now();

            std::vector<std::string> snap_cmd = {"shell", "snapshot_display", "-f", remote_path};
            int tw = ctx->target_width.load();
            int th = ctx->target_height.load();
            if (tw > 0 && th > 0) {
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

                        QImage native_img = (img.format() == QImage::Format_RGB32) ? img : img.convertToFormat(QImage::Format_RGB32);
                        DecodedFrame frame;
                        frame.width = native_img.width();
                        frame.height = native_img.height();
                        frame.stride = native_img.bytesPerLine();
                        frame.format = 4; // QImage::Format_RGB32
                        frame.is_keyframe = true;
                        frame.data.assign(native_img.constBits(), native_img.constBits() + native_img.sizeInBytes());

                        if (weak_this) {
                            QMetaObject::invokeMethod(weak_this.data(), [weak_this, ctx, frame]() {
                                if (weak_this && !ctx->cancelled.load() && weak_this->m_running) {
                                    bool size_changed = (weak_this->m_dev_width != frame.width || weak_this->m_dev_height != frame.height);
                                    weak_this->m_dev_width = frame.width;
                                    weak_this->m_dev_height = frame.height;
                                    if (size_changed) {
                                        weak_this->updateSnapshotTargetSize();
                                    }
                                    emit weak_this->frameReady(frame);
                                    QString tier = (frame.width >= 1080 || frame.height >= 1080) ? "原生原画" : (frame.width >= 720 || frame.height >= 720 ? "720P高清" : "540P流畅");
                                    emit weak_this->statusChanged(QString("兼容快照流 · %1x%2 (%3)").arg(frame.width).arg(frame.height).arg(tier));
                                }
                            });
                        }
                    }
                    QFile::remove(local_temp);
                }
            } else {
                consecutive_errors++;
                if (consecutive_errors % 4 == 0) {
                    ctx->hdc->wakeup(ctx->serial);
                }
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
