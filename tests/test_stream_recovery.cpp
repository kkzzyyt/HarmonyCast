#include "harmonycast/StreamSession.h"
#include "harmonycast/VideoDecoder.h"
#include "harmonycast/Logger.h"
#include "harmonycast/ScreenView.h"
#include "harmonycast/IconManager.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QImage>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QNetworkProxy>
#include <iostream>
#include <cstring>
#include <stdexcept>

using namespace harmonycast;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate> static void waitUntil(Predicate predicate, int timeout = 6000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    check(predicate(), "stream recovery did not finish in time");
}

static QByteArray rawFrame() {
    QByteArray data(32 + 16, '\0');
    std::memcpy(data.data(), "HCR2", 4);
    data[7] = 2; data[11] = 2; data[15] = 8; data[19] = 16; data[23] = 2; data[27] = 2;
    std::fill(data.begin() + 32, data.end(), char(127));
    return data;
}

static void testSoftwareMode() {
    const auto oldOpenGl = qgetenv("QT_OPENGL"), oldMode = qgetenv("HARMONYCAST_SOFTWARE_RENDERING");
    for (const auto* flag : {"QT_OPENGL", "HARMONYCAST_SOFTWARE_RENDERING"}) {
        qunsetenv("QT_OPENGL");
        qunsetenv("HARMONYCAST_SOFTWARE_RENDERING");
        qputenv(flag, QByteArray(flag) == "QT_OPENGL" ? "software" : "1");
        VideoDecoder decoder;
        check(decoder.init(64, 48, true), "software mode could not initialize decoder");
        check(!decoder.isHardwareAccelerated(), "software rendering mode still enables GPU video decoding");
    }
    if (oldOpenGl.isNull()) qunsetenv("QT_OPENGL"); else qputenv("QT_OPENGL", oldOpenGl);
    if (oldMode.isNull()) qunsetenv("HARMONYCAST_SOFTWARE_RENDERING"); else qputenv("HARMONYCAST_SOFTWARE_RENDERING", oldMode);
}

static std::shared_ptr<HdcClient> fakeClient() {
    return std::make_shared<HdcClient>(QCoreApplication::applicationFilePath().toStdString());
}

static Device testDevice() { Device device; device.serial = "stream-recovery-test"; return device; }

static QByteArray commandLog() {
    QFile file(QString::fromUtf8(qgetenv("HC_STREAM_TEST_CALLS")));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

static void resetCommands(const char* mode) {
    qputenv("HC_STREAM_TEST_MODE", mode);
    qputenv("HC_STREAM_TEST_CALLS", (QString::fromUtf8(qgetenv("HC_STREAM_TEST_DIR")) +
                                    "/" + QString::fromLatin1(mode) + ".log").toUtf8());
    QFile file(QString::fromUtf8(qgetenv("HC_STREAM_TEST_CALLS")));
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "command log unavailable");
}

static void testLocalProxyBypass() {
    const auto oldProxy = QNetworkProxy::applicationProxy();
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "192.0.2.1", 1));
    QTcpServer server;
    server.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    check(server.listen(QHostAddress::LocalHost, 0), "proxy bypass test server unavailable");
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto* peer = server.nextPendingConnection(); peer->setParent(&server); peer->write(rawFrame());
    });
    StreamSession session(fakeClient(), testDevice(), 60, 8000000, 0, nullptr, {3000, 1000});
    bool frame = false;
    QObject::connect(&session, &StreamSession::frameReady, &session, [&](const DecodedFrame&) { frame = true; });
    session.start();
    auto* socket = session.findChild<QTcpSocket*>();
    check(socket && socket->proxy().type() == QNetworkProxy::NoProxy, "video loopback inherits the system proxy");
    socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
    waitUntil([&] { return frame; });
    check(session.controlChannel()->connectToHost("127.0.0.1", server.serverPort()), "control loopback inherits the system proxy");
    session.stop();
    QNetworkProxy::setApplicationProxy(oldProxy);
}

static void testSilentConnection() {
    qputenv("HC_STREAM_TEST_MODE", "normal");
    QTcpServer server;
    check(server.listen(QHostAddress::LocalHost, 0), "test stream server could not listen");
    QPointer<QTcpSocket> peer;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] { peer = server.nextPendingConnection(); peer->setParent(&server); });
    StreamSession session(fakeClient(), testDevice(), 60, 8000000, 0, nullptr, {3000, 250});
    QStringList statuses;
    QString failure;
    QObject::connect(&session, &StreamSession::statusChanged, &session, [&](const QString& text) { statuses << text; });
    QObject::connect(&session, &StreamSession::streamFinished, &session, [&](const QString& text) { failure = text; });
    session.start();
    auto* socket = session.findChild<QTcpSocket*>();
    check(socket, "video socket missing");
    socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
    waitUntil([&] { return session.isFallbackSnapshot(); });
    check(statuses.join('\n').contains("首帧"), "silent TCP connection has no first-frame explanation");
    waitUntil([&] { return !failure.isEmpty(); });
    check(!session.isActive() && failure.contains("兼容截图"), "failed snapshot recovery keeps the session connected forever");
}

static void testStalledFramesAndStop() {
    qputenv("HC_STREAM_TEST_MODE", "normal");
    QTcpServer server;
    check(server.listen(QHostAddress::LocalHost, 0), "frame server unavailable");
    QPointer<QTcpSocket> peer;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        peer = server.nextPendingConnection(); peer->setParent(&server); peer->write(rawFrame());
    });
    StreamSession session(fakeClient(), testDevice(), 60, 8000000, 0, nullptr, {3000, 350});
    int frames = 0;
    QObject::connect(&session, &StreamSession::frameReady, &session, [&](const DecodedFrame&) { ++frames; });
    session.start();
    auto* socket = session.findChild<QTcpSocket*>();
    socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
    waitUntil([&] { return frames == 1; });
    QTimer sender;
    sender.setInterval(70);
    QObject::connect(&sender, &QTimer::timeout, &server, [&] { if (peer) peer->write(rawFrame()); });
    sender.start();
    waitUntil([&] { return frames >= 8; });
    check(!session.isFallbackSnapshot(), "healthy frames triggered recovery");
    sender.stop();
    waitUntil([&] { return session.isFallbackSnapshot(); });
    session.stop();
    bool finishedAfterStop = false;
    QObject::connect(&session, &StreamSession::streamFinished, &session, [&](const QString&) { finishedAfterStop = true; });
    QElapsedTimer after;
    after.start();
    waitUntil([&] { return after.elapsed() > 450; });
    check(!finishedAfterStop && !session.isActive(), "stopped session emitted a stale recovery failure");
}

static void testLaunchFailure() {
    qputenv("HC_STREAM_TEST_MODE", "push-failed");
    StreamSession session(fakeClient(), testDevice(), 60, 8000000, 0, nullptr, {3000, 1200});
    QStringList statuses;
    QObject::connect(&session, &StreamSession::statusChanged, &session, [&](const QString& text) { statuses << text; });
    session.start();
    waitUntil([&] { return session.isFallbackSnapshot(); });
    check(statuses.join('\n').contains("采屏程序传输失败"), "capture binary upload failure was hidden");
    session.stop();
}

static void testCompatibilitySummaryAndRecovery() {
    resetCommands("snapshot-success");
    StreamSession session(fakeClient(), testDevice(), 60, 8000000, 0, nullptr, {3000, 1500});
    QStringList statuses;
    int frames = 0;
    QObject::connect(&session, &StreamSession::statusChanged, &session, [&](const QString& text) { statuses << text; });
    QObject::connect(&session, &StreamSession::frameReady, &session, [&](const DecodedFrame&) { ++frames; });
    session.start();
    waitUntil([&] { return frames >= 2; });
    const auto visible = statuses.join('\n');
    check(session.isFallbackSnapshot() && visible.contains("系统") && visible.contains("已启用兼容截图"),
          "missing device APIs do not lead to a confirmed snapshot recovery");
    check(!visible.contains("libnative_display_manager") && !visible.contains("symbol not found") &&
          !visible.contains("__HC_CAPTURE_FAILED__"), "raw loader diagnostics fill the user-facing status");
    QStringList diagnostics;
    for (const auto& line : Logger::getRecentLogs(100)) diagnostics << line;
    check(diagnostics.join('\n').contains("libnative_display_manager.so"), "native launch diagnostics were lost");
    check(commandLog().contains("&& snapshot_display"), "each capture does not remove the previous remote image");
    const QString artifacts = QString::fromUtf8(qgetenv("HC_STREAM_TEST_ARTIFACTS"));
    if (!artifacts.isEmpty()) {
        ScreenView view;
        view.setPalette(IconManager::getAppPalette());
        view.resize(960, 640);
        for (const auto& status : statuses) {
            if (status.contains("正在切换兼容截图")) view.setStatusMessage(status);
        }
        view.show();
        QCoreApplication::processEvents();
        check(view.grab().save(artifacts + "/capture-fallback.png"), "fallback preview unavailable");
    }
    session.stop();
}

static void testStopOnRecoveryStatus() {
    resetCommands("snapshot-success");
    StreamSession session(fakeClient(), testDevice());
    bool recovered = false;
    int frames = 0;
    QObject::connect(&session, &StreamSession::statusChanged, &session, [&](const QString& status) {
        if (status.contains("已启用兼容截图")) {
            recovered = true;
            session.stop();
        }
    });
    QObject::connect(&session, &StreamSession::frameReady, &session, [&](const DecodedFrame&) { ++frames; });
    session.start();
    waitUntil([&] { return recovered; });
    check(!session.isActive() && frames == 0, "stopping from the recovery notification still delivers a frame");
}

static void testUnavailableHdc() {
    StreamSession session(nullptr, testDevice());
    QString error;
    QObject::connect(&session, &StreamSession::streamFinished, &session, [&](const QString& text) { error = text; });
    session.start();
    waitUntil([&] { return !error.isEmpty(); });
    check(!session.isActive() && error.contains("HDC"), "missing HDC leaves a snapshot worker running");
}

static void testSnapshotFailures(const char* mode, const QString& expected, int validFrames = 0) {
    resetCommands(mode);
    StreamSession session(fakeClient(), testDevice(), 60, 8000000, 0, nullptr, {3000, 1500});
    QString failure;
    int frames = 0;
    QObject::connect(&session, &StreamSession::frameReady, &session, [&](const DecodedFrame&) { ++frames; });
    QObject::connect(&session, &StreamSession::streamFinished, &session, [&](const QString& text) { failure = text; });
    session.start();
    waitUntil([&] { return !failure.isEmpty(); }, 5000);
    check(!session.isActive() && failure.contains(expected), "snapshot failure loses its stage or leaves the session running");
    check(validFrames == 0 ? frames == 0 : frames > 0 && frames <= validFrames,
          "failed capture or transfer reused a stale local/remote image");
    if (QByteArray(mode) == "snapshot-permission") {
        check(!commandLog().contains("file recv"), "exit-zero snapshot failure still attempts a transfer");
    }
}

static void testLegacySnapshotResize(const char* mode = "snapshot-no-resize") {
    resetCommands(mode);
    StreamSession session(fakeClient(), testDevice(), 60, 8000000, 0, nullptr, {3000, 1500});
    session.setQuality(3, 2500000);
    int frames = 0;
    QString failure;
    QObject::connect(&session, &StreamSession::frameReady, &session, [&](const DecodedFrame&) { ++frames; });
    QObject::connect(&session, &StreamSession::streamFinished, &session, [&](const QString& text) { failure = text; });
    session.start();
    waitUntil([&] { return frames >= 3; });
    check(failure.isEmpty() && session.isActive(), "legacy screenshot tool cannot recover without resize options");
    check(commandLog().count(" -w ") == 1, "unsupported resize options keep being sent to the device");
    session.stop();
}

static void testSnapshotTransferTimeout() {
    resetCommands("snapshot-slow-transfer");
    StreamSession session(fakeClient(), testDevice(), 60, 8000000, 0, nullptr, {3000, 350});
    QString failure;
    QObject::connect(&session, &StreamSession::streamFinished, &session, [&](const QString& error) { failure = error; });
    session.start();
    waitUntil([&] { return !failure.isEmpty(); });
    check(failure.contains("截图传输"), "timeout during a transfer reports the original native API failure");
}

static void testDestroyDuringSnapshotTransfer() {
    resetCommands("snapshot-destroy-transfer");
    auto session = std::make_unique<StreamSession>(fakeClient(), testDevice());
    session->start();
    waitUntil([&] { return commandLog().contains("file recv"); });
    session.reset();
    // The worker still holds an HDC process. Let its completion and queued
    // deliveries run after the session itself has been destroyed.
    QElapsedTimer elapsed;
    elapsed.start();
    waitUntil([&] { return elapsed.elapsed() > 800; });
}

static int fakeHdc(int argc, char** argv) {
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromUtf8(argv[i]);
    const QByteArray mode = qgetenv("HC_STREAM_TEST_MODE");
    if (!qgetenv("HC_STREAM_TEST_CALLS").isEmpty()) {
        QFile file(QString::fromUtf8(qgetenv("HC_STREAM_TEST_CALLS")));
        if (file.open(QIODevice::WriteOnly | QIODevice::Append)) file.write(args.join(' ').toUtf8() + '\n');
    }
    if (args.contains("fport")) std::cout << "Forwardport result:OK\n";
    else if (args.contains("send")) {
        if (qgetenv("HC_STREAM_TEST_MODE") == "push-failed") std::cout << "[Fail]file transfer denied\n";
        else std::cout << "FileTransfer finish\n";
    } else if (args.contains("uname")) std::cout << "aarch64\n";
    else if (args.contains("recv") && mode.startsWith("snapshot-")) {
        const int index = args.indexOf("recv");
        if (index + 2 >= args.size()) return 1;
        if (mode == "snapshot-recv-denied") { std::cout << "[Fail] FileTransfer rejected\n"; return 0; }
        if (mode == "snapshot-slow-transfer" || mode == "snapshot-destroy-transfer") QThread::msleep(600);
        if (mode == "snapshot-stale" && commandLog().count("file recv") > 2) return 0;
        if (mode == "snapshot-invalid-image") {
            QFile file(args[index + 2]);
            if (file.open(QIODevice::WriteOnly)) file.write("incomplete JPEG");
        } else QFile::copy(QString::fromUtf8(qgetenv("HC_STREAM_TEST_JPEG")), args[index + 2]);
    } else if (args.join(' ').contains("snapshot_display")) {
        if (mode == "snapshot-permission") { std::cout << "[Fail] snapshot permission denied\n"; return 0; }
        if (mode == "snapshot-denied-zero") { std::cerr << "snapshot_display: Operation not permitted\n"; return 0; }
        if (mode.startsWith("snapshot-missing")) {
            std::cerr << "/bin/sh: snapshot_display: not found\n"; return mode.endsWith("zero") ? 0 : 127;
        }
        if (mode.startsWith("snapshot-no-resize") && args.join(' ').contains(" -w ")) {
            std::cerr << "snapshot_display: invalid option -- w\n"; return mode.endsWith("zero") ? 0 : 1;
        }
        if (mode.startsWith("snapshot-")) std::cout << "snapshot success\n";
        else { std::cerr << "snapshot unavailable\n"; return 1; }
    } else if (args.contains("shell") && args.last().contains("nohup")) {
        if (mode.startsWith("snapshot-")) {
            std::cout << "__HC_CAPTURE_FAILED__\nError loading shared library libnative_display_manager.so: No such file or directory\n"
                         "Error relocating harmonycast_capture: OH_AVScreenCapture_SetDataCallback: symbol not found\n";
        } else std::cout << "__HC_CAPTURE_READY__\n";
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return fakeHdc(argc, argv);
    if (!qEnvironmentVariableIsSet("HC_STREAM_TEST_NATIVE")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    qputenv("HC_STREAM_TEST_DIR", temporary.path().toUtf8());
    qputenv("HC_STREAM_TEST_CALLS", temporary.filePath("commands.log").toUtf8());
    qputenv("HC_STREAM_TEST_JPEG", temporary.filePath("snapshot.jpeg").toUtf8());
    QImage fixture(720, 1280, QImage::Format_RGB32);
    fixture.fill(Qt::gray);
    check(fixture.save(QString::fromUtf8(qgetenv("HC_STREAM_TEST_JPEG")), "JPEG"), "snapshot fixture unavailable");
    Logger::init(temporary.filePath("logs"));
    try {
        testSoftwareMode();
        testLocalProxyBypass();
        testSilentConnection();
        testStalledFramesAndStop();
        testLaunchFailure();
        testCompatibilitySummaryAndRecovery();
        testSnapshotFailures("snapshot-permission", "截图权限");
        testSnapshotFailures("snapshot-missing", "截图命令不可用");
        testSnapshotFailures("snapshot-missing-zero", "截图命令不可用");
        testSnapshotFailures("snapshot-denied-zero", "截图权限");
        testSnapshotFailures("snapshot-recv-denied", "截图传输失败");
        testSnapshotFailures("snapshot-invalid-image", "截图图片");
        testSnapshotFailures("snapshot-stale", "截图传输失败", 2);
        testLegacySnapshotResize();
        testLegacySnapshotResize("snapshot-no-resize-zero");
        testStopOnRecoveryStatus();
        testUnavailableHdc();
        testSnapshotTransferTimeout();
        testDestroyDuringSnapshotTransfer();
        std::cout << "Stream recovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
