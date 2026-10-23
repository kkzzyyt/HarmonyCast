#include "harmonycast/StreamSession.h"
#include <QTcpServer>
#include <QThread>
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
    std::string cmd = QString("/system/bin/ohscrcpy_server -p %1 -f %2 -b %3%4 --log &")
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
    for (int attempt = 0; attempt < 25 && m_running; ++attempt) {
        m_socket->connectToHost(QHostAddress::LocalHost, m_local_port);
        if (m_socket->waitForConnected(300)) {
            break;
        }
        QThread::msleep(150);
    }

    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        emit streamFinished("连接视频流超时，请检查设备权限或投屏服务");
        stop();
        return false;
    }

    // 5. Connect control channel
    m_control->connectToHost("127.0.0.1", m_local_port);

    m_heartbeat_timer->start(2000);
    return true;
}

void StreamSession::stop() {
    m_running = false;
    m_heartbeat_timer->stop();

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
    return m_running && m_socket->state() == QAbstractSocket::ConnectedState;
}

void StreamSession::cleanup() {
    if (m_forwarded && m_hdc && !m_device.serial.empty()) {
        m_hdc->removeForward(m_device.serial, m_local_port, m_remote_port);
        m_forwarded = false;
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
    if (m_running) {
        emit streamFinished("设备端投屏服务已断开");
        stop();
    }
}

void StreamSession::sendHeartbeat() {
    if (m_socket && m_socket->isOpen()) {
        uint32_t hb[2] = {0, 0};
        m_socket->write(reinterpret_cast<const char*>(hb), sizeof(hb));
        m_socket->flush();
    }
}

} // namespace harmonycast
