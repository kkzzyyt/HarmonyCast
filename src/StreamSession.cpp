#include "harmonycast/StreamSession.h"
#include <QTcpServer>
#include <QThread>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
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

bool StreamSession::launchServer() {
    std::string arch = "arm64";
    auto res_uname = m_hdc->run({"shell", "uname", "-m"}, m_device.serial, 2000);
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
        m_hdc->fileSend(m_device.serial, local_bin.toStdString(), remote_bin);
        m_hdc->run({"shell", "chmod", "755", remote_bin}, m_device.serial, 3000);
    }

    // Kill any previous capture instances
    m_hdc->run({"shell", "killall", "-9", "harmonycast_capture", "harmonycast_capture_arm64", "2>/dev/null"}, m_device.serial, 1000);

    // Launch remote capture server in background
    std::string cmd = QString("%1 -p %2 -f %3 -b %4%5 &")
                          .arg(QString::fromStdString(remote_bin))
                          .arg(m_remote_port)
                          .arg(m_fps)
                          .arg(m_bitrate)
                          .arg(m_max_size > 0 ? QString(" -m %1").arg(m_max_size) : "")
                          .toStdString();
    auto res = m_hdc->run({"shell", cmd}, m_device.serial, 3000);
    return res.success();
}

bool StreamSession::start() {
    stop();
    m_running = true;
    emit statusChanged("正在准备投屏服务…");

    m_local_port = pickFreePort();
    m_remote_port = m_local_port;

    // 1. Setup port forwarding
    if (!m_hdc->forwardPort(m_device.serial, m_local_port, m_remote_port)) {
        emit streamFinished("端口映射失败，请检查设备连接");
        stop();
        return false;
    }
    m_forwarded = true;

    // 2. Launch device-side server
    launchServer();

    // 3. Initialize hardware-accelerated video decoder
    if (!m_decoder->init(1080, 1920, true)) {
        emit statusChanged("硬件加速初始化降级为多线程软解");
        m_decoder->init(1080, 1920, false);
    }

    // 4. Connect video socket with retry loop
    emit statusChanged("正在连接设备视频流…");
    for (int attempt = 0; attempt < 15 && m_running; ++attempt) {
        m_socket->connectToHost(QHostAddress::LocalHost, m_local_port);
        if (m_socket->waitForConnected(250)) {
            break;
        }
        QThread::msleep(100);
    }

    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        emit statusChanged("硬件采屏未响应，自动切换为兼容快照流 (snapshot_display)…");
        startSnapshotFallback();
        return true;
    }

    m_heartbeat_timer->start(2000);
    return true;
}

void StreamSession::stop() {
    m_running = false;
    m_fallback_snapshot = false;
    m_heartbeat_timer->stop();

    if (m_snapshot_thread && m_snapshot_thread->joinable()) {
        m_snapshot_thread->join();
    }
    m_snapshot_thread.reset();

    if (m_socket->isOpen()) {
        m_socket->disconnectFromHost();
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
    if (m_forwarded && m_hdc && !m_device.serial.empty()) {
        m_hdc->removeForward(m_device.serial, m_local_port, m_remote_port);
        m_forwarded = false;
    }
    if (m_hdc && !m_device.serial.empty()) {
        m_hdc->run({"shell", "killall", "-9", "harmonycast_capture", "2>/dev/null"}, m_device.serial, 1000);
        m_hdc->run({"shell", "rm", "-f", "/data/local/tmp/harmonycast_stream.jpeg"}, m_device.serial, 1000);
    }
}

void StreamSession::onSocketConnected() {
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 2 * 1024 * 1024);
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

void StreamSession::startSnapshotFallback() {
    if (m_fallback_snapshot) return;
    m_fallback_snapshot = true;

    if (m_snapshot_thread && m_snapshot_thread->joinable()) {
        m_snapshot_thread->join();
    }

    m_snapshot_thread = std::make_unique<std::thread>([this]() {
        std::string remote_path = "/data/local/tmp/harmonycast_stream.jpeg";
        QString local_temp = QDir::tempPath() + QString("/harmonycast_stream_%1_%2.jpeg")
                                .arg(QString::fromStdString(m_device.serial).replace(':', '_').replace('.', '_'))
                                .arg(reinterpret_cast<uintptr_t>(this));
        std::string local_path = local_temp.toStdString();

        m_hdc->wakeup(m_device.serial);

        int consecutive_errors = 0;
        while (m_running && m_fallback_snapshot) {
            auto t0 = std::chrono::steady_clock::now();

            auto res_snap = m_hdc->run({"shell", "snapshot_display", "-f", remote_path}, m_device.serial, 3000);
            if (!m_running || !m_fallback_snapshot) break;

            if (res_snap.success()) {
                bool res_recv = m_hdc->fileRecv(m_device.serial, remote_path, local_path);
                if (!m_running || !m_fallback_snapshot) break;

                if (res_recv && QFile::exists(local_temp)) {
                    QImage img;
                    if (img.load(local_temp)) {
                        consecutive_errors = 0;
                        m_dev_width = img.width();
                        m_dev_height = img.height();

                        QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
                        DecodedFrame frame;
                        frame.width = rgba.width();
                        frame.height = rgba.height();
                        frame.stride = rgba.bytesPerLine();
                        frame.format = 12; // AV_PIX_FMT_RGBA
                        frame.is_keyframe = true;
                        frame.data.assign(rgba.constBits(), rgba.constBits() + rgba.sizeInBytes());

                        emit frameReady(frame);
                        emit statusChanged(QString("兼容快照流 · %1x%2").arg(frame.width).arg(frame.height));
                    }
                    QFile::remove(local_temp);
                }
            } else {
                consecutive_errors++;
                if (consecutive_errors % 4 == 0) {
                    m_hdc->wakeup(m_device.serial);
                }
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            int target_delay = std::clamp(100 - static_cast<int>(elapsed), 10, 100);
            for (int i = 0; i < target_delay / 10 && m_running && m_fallback_snapshot; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        m_hdc->run({"shell", "rm", "-f", remote_path}, m_device.serial, 1000);
        QFile::remove(local_temp);
    });
}

void StreamSession::sendHeartbeat() {
    if (m_socket && m_socket->isOpen()) {
        uint32_t hb[2] = {0, 0};
        m_socket->write(reinterpret_cast<const char*>(hb), sizeof(hb));
        m_socket->flush();
    }
}

} // namespace harmonycast
