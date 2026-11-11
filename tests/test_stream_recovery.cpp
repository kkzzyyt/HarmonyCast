#include "harmonycast/StreamSession.h"
#include "harmonycast/VideoDecoder.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
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
    check(statuses.join('\n').contains("file transfer denied"), "capture binary upload failure was hidden");
    session.stop();
}

static int fakeHdc(int argc, char** argv) {
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromUtf8(argv[i]);
    if (args.contains("fport")) std::cout << "Forwardport result:OK\n";
    else if (args.contains("send")) {
        if (qgetenv("HC_STREAM_TEST_MODE") == "push-failed") std::cout << "[Fail]file transfer denied\n";
        else std::cout << "FileTransfer finish\n";
    } else if (args.contains("uname")) std::cout << "aarch64\n";
    else if (args.contains("snapshot_display")) { std::cerr << "snapshot unavailable\n"; return 1; }
    else if (args.contains("shell") && args.last().contains("nohup")) std::cout << "__HC_CAPTURE_READY__\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return fakeHdc(argc, argv);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    try {
        testSoftwareMode();
        testLocalProxyBypass();
        testSilentConnection();
        testStalledFramesAndStop();
        testLaunchFailure();
        std::cout << "Stream recovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
