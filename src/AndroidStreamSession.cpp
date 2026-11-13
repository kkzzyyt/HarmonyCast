#include "harmonycast/AndroidStreamSession.h"
#include "harmonycast/FramePresentation.h"
#include "harmonycast/HdcClient.h"
#include "harmonycast/Logger.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QNetworkProxy>
#include <QPointer>
#include <QRandomGenerator>
#include <algorithm>
#include <thread>

namespace harmonycast {
namespace {
void cleanupAndroid(const std::shared_ptr<AdbClient>& adb, const std::string& serial, int port,
                    const std::string& remote) {
    if (port > 0) adb->removeForward(serial, port);
    if (!remote.empty()) adb->run({"shell", "rm", "-f", remote}, serial, 2000);
}

void retireProcess(QProcess* process) {
    if (!process) return;
    process->disconnect();
    if (process->state() == QProcess::NotRunning) { process->deleteLater(); return; }
    process->setParent(QCoreApplication::instance());
    QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), process, &QObject::deleteLater);
    process->terminate();
    QTimer::singleShot(1000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
}
}

AndroidStreamSession::AndroidStreamSession(std::shared_ptr<AdbClient> adb, Device device, int bitrate,
                                         QObject* parent, StreamTimeouts timeouts, QString serverPath)
    : MirrorSession(parent), m_adb(std::move(adb)), m_device(std::move(device)),
      m_control(std::make_shared<ControlChannel>(nullptr, DevicePlatform::Android)), m_socket(new QTcpSocket(this)),
      m_timeouts{std::max(100, timeouts.startup_ms), std::max(100, timeouts.frame_ms)},
      m_server_path(serverPath.isEmpty() ? findServer() : std::move(serverPath)), m_bitrate(bitrate),
      m_delivery(new QObject, [](QObject* receiver) { receiver->deleteLater(); }) {
    m_device.platform = DevicePlatform::Android;
    m_socket->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    m_socket->setReadBufferSize(2 * 1024 * 1024);
    connect(m_socket, &QTcpSocket::connected, this, [this] {
        if (!m_running || m_fallback) return;
        m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    });
    connect(m_socket, &QTcpSocket::readyRead, this, &AndroidStreamSession::consumeVideo);
    connect(m_socket, &QTcpSocket::disconnected, this, [this] {
        if (!m_running || m_fallback) return;
        if (m_seen_dummy) startFallback("Android 视频连接已断开");
        else QTimer::singleShot(150, this, [this, generation = m_generation] { connectVideo(generation); });
    });
    m_watchdog.setInterval(100);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (!m_running) return;
        const int timeout = m_received_frame || m_fallback ? m_timeouts.frame_ms : m_timeouts.startup_ms;
        if (m_progress.elapsed() < timeout) return;
        if (!m_fallback) { startFallback("Android 视频流未返回画面"); return; }
        QString stage;
        if (m_snapshot) {
            std::lock_guard lock(m_snapshot->mutex); stage = m_snapshot->stage;
            if (!m_snapshot->last_failure.isEmpty()) stage += "\n最近失败：" + m_snapshot->last_failure;
        }
        finish("Android 兼容截图超时\n" + stage + "\n请检查设备截图权限与 ADB 连接");
    });
}

AndroidStreamSession::~AndroidStreamSession() { stop(); }

QString AndroidStreamSession::findServer() {
    const QString relative = "/servers/android/scrcpy-server-v5.0.1";
    const auto app = QCoreApplication::applicationDirPath();
    for (const auto& root : {app + "/../Resources", app + "/resources", QDir::currentPath() + "/resources"}) {
        if (QFile::exists(root + relative)) return root + relative;
    }
    return {};
}

bool AndroidStreamSession::start() {
    stop();
    m_running = true; m_fallback = false; m_received_frame = false;
    m_width = m_height = 0; m_server_output.clear(); m_seen_dummy = false;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_progress.start(); m_watchdog.start();
    if (!m_adb || !m_adb->isAvailable()) { finish("ADB 工具不可用，请检查安装包或 Android SDK 配置"); return false; }
    QFile server(m_server_path);
    if (!server.open(QIODevice::ReadOnly) ||
        QCryptographicHash::hash(server.readAll(), QCryptographicHash::Sha256).toHex() != serverSha256) {
        startFallback("Android 采屏服务缺失或校验失败，请使用完整安装包"); return true;
    }
    const auto generation = m_generation;
    const auto scid = QString::number(QRandomGenerator::global()->generate() & 0x7fffffff, 16).rightJustified(8, '0');
    const auto remote = "/data/local/tmp/harmonycast_scrcpy_" + scid.toStdString() + ".jar";
    const QPointer<AndroidStreamSession> guard(this);
    emit statusChanged("正在启动 Android 投屏…");
    if (!guard || !m_running || generation != m_generation) return false;
    std::thread([weak = guard, delivery = m_delivery, adb = m_adb, device = m_device.id(),
                  cancelled = m_cancel, generation, scid, remote, path = m_server_path]() {
        QString error;
        int port = 0;
        const auto sdk = adb->run({"shell", "getprop", "ro.build.version.sdk"}, device, 4000);
        bool valid = false;
        const int api = QString::fromStdString(sdk.out).trimmed().toInt(&valid);
        const bool unsupported = sdk.success() && valid && api < 26;
        if (unsupported) error = "需要 Android 8.0 或更高版本";
        else if (!sdk.success() || !valid) error = "无法读取 Android 系统版本，请检查 ADB 授权和连接";
        if (error.isEmpty() && !cancelled->load()) {
            const auto pushed = adb->run({"push", QDir::toNativeSeparators(path).toStdString(), remote}, device, 30000);
            if (!pushed.success()) error = "Android 采屏服务传输失败：" + QString::fromStdString(pushed.err).left(1000);
        }
        if (error.isEmpty() && !cancelled->load()) {
            const auto forwarded = adb->run({"forward", "tcp:0", "localabstract:scrcpy_" + scid.toStdString()}, device, 5000);
            port = QString::fromStdString(forwarded.out).trimmed().toInt(&valid);
            if (!forwarded.success() || !valid || port <= 0 || port > 65535) {
                port = 0; error = "Android 视频端口映射失败：" + QString::fromStdString(forwarded.err).left(1000);
            }
        }
        if (cancelled->load()) { cleanupAndroid(adb, device, port, remote); return; }
        QMetaObject::invokeMethod(delivery.get(), [weak, adb, device, generation, cancelled, scid, remote, port, error, unsupported] {
            if (!weak || cancelled->load() || weak->m_generation != generation || !weak->m_running) {
                std::thread([adb, device, port, remote] { cleanupAndroid(adb, device, port, remote); }).detach();
                return;
            }
            weak->m_local_port = port; weak->m_remote_path = remote;
            if (unsupported) weak->finish(error);
            else if (!error.isEmpty()) weak->startFallback(error);
            else weak->launchServer(scid);
        }, Qt::QueuedConnection);
    }).detach();
    return true;
}

void AndroidStreamSession::launchServer(const QString& scid) {
    startPipeline();
    const auto generation = m_generation;
    m_server = new QProcess(this);
    HdcClient::configureProcess(*m_server, m_adb->path());
    auto* process = m_server;
    const auto captureOutput = [this, process] {
        if (process != m_server) return;
        m_server_output = (m_server_output + QString::fromUtf8(process->readAllStandardOutput()) +
                           QString::fromUtf8(process->readAllStandardError())).right(16384);
    };
    connect(process, &QProcess::readyReadStandardOutput, this, captureOutput);
    connect(process, &QProcess::readyReadStandardError, this, captureOutput);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this, process, generation, captureOutput](int, QProcess::ExitStatus) {
            captureOutput();
            if (m_running && generation == m_generation && process == m_server && !m_fallback)
                startFallback("Android 采屏服务已结束\n" + m_server_output);
        });
    connect(process, &QProcess::errorOccurred, this, [this, process, generation](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_running && generation == m_generation && process == m_server)
            startFallback("Android 采屏服务启动失败：" + process->errorString());
    });
    const int sizes[] = {0, 1600, 1280, 960};
    const QString command = QString("CLASSPATH=%1 app_process / com.genymobile.scrcpy.Server %2 scid=%3 "
        "audio=false control=true tunnel_forward=true video_codec=h264 video_bit_rate=%4 max_fps=60 "
        "max_size=%5 cleanup=true clipboard_autosync=false send_device_meta=true send_frame_meta=true "
        "send_dummy_byte=true send_stream_meta=true")
        .arg(QString::fromStdString(m_remote_path), serverVersion, scid).arg(m_bitrate).arg(sizes[m_quality]);
    process->start(QString::fromStdString(m_adb->path()), {"-s", QString::fromStdString(m_device.serial), "shell", command});
    connectVideo(generation);
}

void AndroidStreamSession::connectVideo(uint64_t generation) {
    if (!m_running || m_fallback || generation != m_generation || m_socket->state() == QAbstractSocket::ConnectedState) return;
    if (m_socket->state() == QAbstractSocket::UnconnectedState) m_socket->connectToHost("127.0.0.1", quint16(m_local_port));
    QTimer::singleShot(150, this, [this, generation] { connectVideo(generation); });
}

void AndroidStreamSession::startPipeline() {
    m_mailbox = std::make_shared<FrameMailbox>();
    m_decode_thread = new QThread(this);
    m_pipeline = new VideoPipeline(nullptr, DevicePlatform::Android);
    m_pipeline->moveToThread(m_decode_thread);
    connect(m_decode_thread, &QThread::finished, m_pipeline, &QObject::deleteLater);
    const auto generation = m_generation;
    auto mailbox = m_mailbox;
    connect(m_pipeline, &VideoPipeline::frameReady, this, [this, mailbox, generation](const DecodedFrame& frame) {
        if (generation == m_generation && mailbox->publish(frame)) deliver(mailbox, generation);
    });
    connect(m_pipeline, &VideoPipeline::batchProcessed, this, [this, generation] {
        if (generation != m_generation) return;
        m_decode_busy = false; consumeVideo();
    });
    connect(m_pipeline, &VideoPipeline::protocolError, this, [this, generation](const QString& error) {
        if (generation == m_generation && m_running && !m_fallback) startFallback(error);
    });
    connect(m_pipeline, &VideoPipeline::decoderFallback, this, [this, generation](const QString& reason) {
        if (generation == m_generation && m_running && !m_fallback) { m_progress.restart(); emit statusChanged(reason); }
    });
    m_decode_thread->start();
    setPresentation(m_viewport, m_rotation, m_dpr);
}

void AndroidStreamSession::consumeVideo() {
    if (!m_running || m_fallback || m_decode_busy || !m_pipeline) return;
    if (!m_seen_dummy) {
        if (!m_socket->bytesAvailable()) return;
        if (m_socket->peek(1) != QByteArray(1, '\0')) { startFallback("Android 视频握手字节无效"); return; }
        m_seen_dummy = true;
        // A forwarded TCP connection can open before app_process is ready.
        // The dummy byte confirms the device accepted video; control is next.
        m_control->connectToHostAsync("127.0.0.1", quint16(m_local_port));
    }
    auto data = m_socket->read(256 * 1024);
    if (data.isEmpty()) return;
    m_decode_busy = true;
    auto* pipeline = m_pipeline;
    QMetaObject::invokeMethod(pipeline, [pipeline, data = std::move(data)] { pipeline->consume(data); }, Qt::QueuedConnection);
}

void AndroidStreamSession::releaseNative() {
    m_socket->abort(); m_control->disconnectFromHost();
    retireProcess(m_server); m_server = nullptr;
    if (m_decode_thread) {
        auto* thread = m_decode_thread; m_decode_thread = nullptr; m_pipeline = nullptr;
        thread->quit();
        if (thread->wait(1000)) delete thread;
        else { thread->setParent(nullptr); connect(thread, &QThread::finished, thread, &QObject::deleteLater); }
    }
    m_decode_busy = false;
    if (m_adb && (m_local_port || !m_remote_path.empty())) {
        std::thread([adb = m_adb, serial = m_device.id(), port = m_local_port, remote = m_remote_path] {
            cleanupAndroid(adb, serial, port, remote);
        }).detach();
    }
    m_local_port = 0; m_remote_path.clear();
}

void AndroidStreamSession::stop() {
    m_running = false; ++m_generation;
    if (m_cancel) m_cancel->store(true);
    m_watchdog.stop(); releaseNative();
    if (m_mailbox) m_mailbox->clear();
    m_mailbox.reset(); m_snapshot.reset();
}

void AndroidStreamSession::startFallback(const QString& reason) {
    if (!m_running || m_fallback) return;
    HC_LOG_WARN("Android capture fallback: " + reason);
    m_fallback = true; ++m_generation;
    if (m_cancel) m_cancel->store(true);
    releaseNative();
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_mailbox = std::make_shared<FrameMailbox>(); m_received_frame = false;
    m_snapshot = std::make_shared<SnapshotState>();
    m_snapshot->quality = m_quality;
    setPresentation(m_viewport, m_rotation, m_dpr);
    m_progress.restart();
    const auto generation = m_generation;
    const QPointer<AndroidStreamSession> guard(this);
    emit statusChanged("正在切换 Android 兼容截图，请稍候…");
    if (!guard || !m_running || generation != m_generation) return;
    std::thread([weak = guard, delivery = m_delivery, adb = m_adb, serial = m_device.id(),
                  cancelled = m_cancel, mailbox = m_mailbox, state = m_snapshot, generation] {
        while (!cancelled->load()) {
            { std::lock_guard lock(state->mutex); state->stage = "正在获取 Android 截图"; }
            const auto result = adb->run({"exec-out", "screencap", "-p"}, serial, 8000);
            if (cancelled->load()) break;
            const auto image = AdbClient::screenshotImage(result);
            if (result.success() && !image.isNull()) {
                DecodedFrame frame; frame.width = image.width(); frame.height = image.height();
                frame.source_width = frame.width; frame.source_height = frame.height; frame.image = image;
                { std::lock_guard lock(state->mutex);
                    const int maxSizes[] = {0, 1600, 1280, 960};
                    const int maxSize = maxSizes[state->quality];
                    if (maxSize && std::max(frame.width, frame.height) > maxSize) {
                        frame.image = image.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                        frame.width = frame.image.width(); frame.height = frame.image.height();
                    }
                    prepareFramePresentation(frame, state->viewport, state->rotation, state->dpr);
                    state->stage.clear(); state->last_failure.clear(); }
                if (mailbox->publish(frame)) QMetaObject::invokeMethod(delivery.get(), [weak, mailbox, generation] {
                    if (weak) weak->deliver(mailbox, generation);
                }, Qt::QueuedConnection);
            } else {
                auto detail = QString::fromStdString(result.err).trimmed().left(800);
                if (detail.isEmpty() && result.out.size() < 1024 &&
                    std::all_of(result.out.begin(), result.out.end(), [](unsigned char c) {
                        return c == '\n' || c == '\r' || c == '\t' || (c >= 32 && c < 127);
                    })) detail = QString::fromStdString(result.out).trimmed();
                { std::lock_guard lock(state->mutex); state->last_failure = "Android 截图失败：" +
                    (detail.isEmpty() ? QStringLiteral("未收到完整 PNG 图片") : detail); }
                if (detail.contains("Permission denied", Qt::CaseInsensitive) || detail.contains("not permitted", Qt::CaseInsensitive) ||
                    detail.contains("Permission denial", Qt::CaseInsensitive)) {
                    QMetaObject::invokeMethod(delivery.get(), [weak, generation] {
                        if (weak && weak->m_running && weak->m_generation == generation)
                            weak->finish("设备未授予 Android 截图权限");
                    }, Qt::QueuedConnection);
                    break;
                }
            }
            for (int delay = 0; delay < 200 && !cancelled->load(); delay += 10)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }).detach();
}

void AndroidStreamSession::deliver(const std::shared_ptr<FrameMailbox>& mailbox, uint64_t generation) {
    const auto frame = mailbox->take();
    if (!frame || !m_running || generation != m_generation || mailbox != m_mailbox) return;
    const bool first = !m_received_frame;
    m_received_frame = true; m_progress.restart();
    m_width = frame->source_width; m_height = frame->source_height;
    const QPointer<AndroidStreamSession> guard(this);
    if (first) emit statusChanged(m_fallback ? "已启用 Android 兼容截图投屏" : "Android 投屏已连接");
    if (guard && m_running && generation == m_generation) emit frameReady(*frame);
}

void AndroidStreamSession::finish(const QString& error) {
    stop(); emit streamFinished(error);
}

void AndroidStreamSession::setQuality(int quality, int bitrate) {
    m_quality = std::clamp(quality, 0, 3); m_bitrate = std::clamp(bitrate, 100000, 100000000);
    if (m_snapshot) { std::lock_guard lock(m_snapshot->mutex); m_snapshot->quality = m_quality; }
}

void AndroidStreamSession::setPresentation(QSize viewport, int rotation, qreal dpr) {
    m_viewport = viewport; m_rotation = rotation; m_dpr = dpr;
    if (m_pipeline) {
        auto* pipeline = m_pipeline;
        QMetaObject::invokeMethod(pipeline, [pipeline, viewport, rotation, dpr] { pipeline->setPresentation(viewport, rotation, dpr); }, Qt::QueuedConnection);
    }
    if (m_snapshot) { std::lock_guard lock(m_snapshot->mutex); m_snapshot->viewport = viewport; m_snapshot->rotation = rotation; m_snapshot->dpr = dpr; }
}

} // namespace harmonycast
