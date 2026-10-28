#include "harmonycast/PacketDemuxer.h"
#include "harmonycast/ScreenView.h"
#include "harmonycast/FrameMailbox.h"
#include "harmonycast/MainWindow.h"
#include "harmonycast/VideoPipeline.h"
#include "harmonycast/FramePresentation.h"
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <QFile>
#include <QTableWidget>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QClipboard>
#include <QTcpServer>
#include <QTcpSocket>

using namespace harmonycast;

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

template<class Predicate> static void waitUntil(Predicate predicate, int timeout = 2000) {
    QElapsedTimer clock;
    clock.start();
    while (!predicate() && clock.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    check(predicate(), "asynchronous operation timed out");
}

static QByteArray rawFrame(int width, int height, int value) {
    QByteArray bytes(32 + width * height * 4, '\0');
    auto write32 = [&](int offset, quint32 v) {
        for (int i = 0; i < 4; ++i) bytes[offset + i] = char(v >> (24 - 8 * i));
    };
    std::memcpy(bytes.data(), "HCR2", 4);
    write32(4, width);
    write32(8, height);
    write32(12, width * 4);
    write32(16, width * height * 4);
    write32(20, width * 2);
    write32(24, height * 2);
    std::fill(bytes.begin() + 32, bytes.end(), char(value));
    return bytes;
}

static void testFragmentedFrames() {
    PacketDemuxer demuxer;
    std::vector<DecodedFrame> received;
    demuxer.setRawFrameCallback([&](const DecodedFrame& frame) { received.push_back(frame); });
    QByteArray bytes = rawFrame(8, 6, 17) + rawFrame(4, 10, 29);
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        demuxer.feed(std::span(reinterpret_cast<const uint8_t*>(bytes.constData() + i), 1));
    }
    check(received.size() == 2, "fragmented headers/payloads must survive TCP boundaries");
    check(received[0].image.constBits()[0] == 17, "retained frame changed after next packet");
    check(received[1].image.size() == QSize(4, 10), "resolution change lost");
    check(received[1].source_width == 8 && received[1].source_height == 20, "device coordinates must retain source dimensions");
    demuxer.reset();
    check(received[0].image.constBits()[0] == 17, "frame must outlive parser reset");
    QByteArray rotated = rawFrame(6, 8, 41);
    rotated[23] = 16; rotated[27] = 12; rotated[31] = 1;
    demuxer.feed(std::span(reinterpret_cast<const uint8_t*>(rotated.constData()), size_t(rotated.size())));
    check(received.back().source_width == 12 && received.back().source_height == 16, "native display rotation changed touch coordinates");
}

static void testFrameLifetimeAndMailbox() {
    FrameMailbox mailbox;
    ScreenView screen;
    screen.resize(200, 200);
    for (int i = 0; i < 1000; ++i) {
        DecodedFrame frame;
        frame.image = QImage(32, 16, QImage::Format_RGB32);
        frame.image.fill(QColor(i % 256, 42, 17));
        frame.width = 32;
        frame.height = 16;
        frame.source_width = 320;
        frame.source_height = 160;
        check(mailbox.publish(frame) == (i == 0), "only one GUI notification may be pending");
    }
    auto latest = mailbox.take();
    check(latest.has_value(), "latest frame missing");
    screen.updateFrame(*latest);
    check(screen.image().pixelColor(0, 0).red() == 999 % 256, "stale frame displayed");
    check(screen.image().constBits() == latest->image.constBits(), "display must share the owned frame");
    latest.reset();
    check(screen.image().pixelColor(0, 0).green() == 42, "display must own pixel lifetime");
    check(screen.deviceWidth() == 320, "scaled frame changed touch coordinates");
    DecodedFrame next;
    next.image = QImage(16, 32, QImage::Format_RGB32);
    next.image.fill(Qt::blue);
    next.width = 16;
    next.height = 32;
    screen.updateFrame(next);
    check(screen.targetRect().height() > screen.targetRect().width(), "target rectangle must track resolution changes");
    mailbox.clear();
    check(!mailbox.take(), "cleared mailbox retained frame");
}

static void testPresentationAndTouchCoordinates() {
    DecodedFrame frame;
    frame.image = QImage(40, 20, QImage::Format_RGB32);
    for (int y = 0; y < 20; ++y) for (int x = 0; x < 40; ++x) {
        frame.image.setPixelColor(x, y, y < 10 ? (x < 20 ? Qt::red : Qt::green) : (x < 20 ? Qt::blue : Qt::yellow));
    }
    frame.width = 40; frame.height = 20;
    frame.source_width = 400; frame.source_height = 200;
    prepareFramePresentation(frame, QSize(100, 200), 90, 2);
    check(frame.presentation_image.size() == QSize(200, 400), "Retina presentation size incorrect");
    check(frame.presentation_image.pixelColor(20, 20) == QColor(Qt::blue), "rotation changed quadrant colors");
    check(frame.presentation_image.pixelColor(180, 20) == QColor(Qt::red), "rotation direction incorrect");
    ScreenView screen;
    screen.resize(120, 240);
    screen.setRotation(90);
    screen.updateFrame(frame);
    check(screen.image().size() == QSize(40, 20), "presentation scaling changed screenshot resolution");
    auto [x, y] = screen.mapToDevice(screen.targetRect().center());
    check(x >= 190 && x <= 210 && y >= 90 && y <= 110, "rotation/scale changed touch mapping");
}

static void testWorkerPipeline() {
    QThread thread;
    auto* pipeline = new VideoPipeline;
    pipeline->moveToThread(&thread);
    QObject::connect(&thread, &QThread::finished, pipeline, &QObject::deleteLater);
    FrameMailbox mailbox;
    std::atomic<bool> workerThread{false};
    std::atomic<int> processed{0};
    QObject::connect(pipeline, &VideoPipeline::frameReady, pipeline, [&](const DecodedFrame& frame) {
        workerThread = QThread::currentThread() == &thread;
        mailbox.publish(frame);
    }, Qt::DirectConnection);
    QObject::connect(pipeline, &VideoPipeline::batchProcessed, pipeline, [&] { ++processed; }, Qt::DirectConnection);
    thread.start();
    QByteArray bytes = rawFrame(64, 32, 37);
    QMetaObject::invokeMethod(pipeline, [pipeline, bytes] { pipeline->consume(bytes); }, Qt::QueuedConnection);
    waitUntil([&] { return processed.load() == 1; });
    thread.quit();
    thread.wait();
    check(workerThread, "frame processing ran on GUI thread");
    auto frame = mailbox.take();
    check(frame && frame->image.constBits()[0] == 37, "worker frame lost during shutdown");
}

static std::vector<QByteArray> encodedFrames() {
    QFile fixture(QStringLiteral(HARMONYCAST_TEST_FIXTURES "/video-64x48.h264"));
    check(fixture.open(QIODevice::ReadOnly), "H.264 fixture missing");
    QByteArray bytes = fixture.readAll();
    std::vector<QByteArray> frames;
    const QByteArray delimiter = QByteArray::fromHex("0000000109");
    qsizetype start = bytes.indexOf(delimiter);
    while (start >= 0) {
        qsizetype next = bytes.indexOf(delimiter, start + delimiter.size());
        frames.push_back(bytes.mid(start, next < 0 ? -1 : next - start));
        start = next;
    }
    check(frames.size() == 4, "fixture access unit split failed");
    return frames;
}

static void testEncodedFrameOwnership() {
    const auto packets = encodedFrames();
    for (bool hardware : {false, true}) {
        VideoDecoder decoder;
        check(decoder.init(64, 48, hardware), "decoder initialization failed");
        std::vector<DecodedFrame> frames;
        decoder.setFrameCallback([&](const DecodedFrame& frame) { frames.push_back(frame); });
        for (const auto& packet : packets) {
            check(decoder.decode(std::span(reinterpret_cast<const uint8_t*>(packet.constData()), size_t(packet.size())), true), "H.264 decoding failed");
        }
        check(frames.size() == 4, "decoder lost frames");
        QImage first = frames.front().image.copy();
        // Retain images through pool exhaustion, reuse and decoder shutdown.
        for (const auto& packet : packets) decoder.decode(std::span(reinterpret_cast<const uint8_t*>(packet.constData()), size_t(packet.size())), true);
        decoder.close();
        check(frames.front().image == first, "buffer reuse changed an outstanding decoded frame");
        check(first.size() == QSize(64, 48), "decoded size incorrect");
        check(first.pixelColor(20, 20) != first.pixelColor(40, 20), "decoded test pattern lost colors");
    }
}

static void sendEvent(WebViewDebugger& debugger, const QJsonObject& event) {
    QString text = QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact));
    check(QMetaObject::invokeMethod(&debugger, "onWsTextMessage", Qt::DirectConnection, Q_ARG(QString, text)), "CDP event dispatch failed");
}

static QJsonObject requestEvent(const QString& id) {
    return {{"method", "Network.requestWillBeSent"}, {"params", QJsonObject{
        {"requestId", id}, {"type", "Fetch"}, {"request", QJsonObject{{"url", "https://example.test/" + id}, {"method", "GET"}}}}}};
}

static void testNetworkRetentionAndBodies() {
    WebViewDebugger debugger(std::make_shared<HdcClient>());
    for (int i = 0; i < 2500; ++i) sendEvent(debugger, requestEvent(QString::number(i)));
    check(debugger.allRequests().size() == 1000, "request cache exceeded limit");
    check(!debugger.getRequest("0") && debugger.getRequest("2499"), "request cache must evict oldest, not lexical IDs");
    debugger.clearRequests();
    sendEvent(debugger, {{"method", "Network.responseReceived"}, {"params", QJsonObject{{"requestId", "0"}, {"response", QJsonObject{{"status", 200}}}}}});
    check(debugger.allRequests().empty(), "late response resurrected cleared data");

    QWebSocketServer server("test", QWebSocketServer::NonSecureMode);
    check(server.listen(QHostAddress::LocalHost, 0), "local CDP test server failed");
    QWebSocket* peer = nullptr;
    QObject::connect(&server, &QWebSocketServer::newConnection, &server, [&] {
        peer = server.nextPendingConnection();
        peer->setParent(&server);
        QObject::connect(peer, &QWebSocket::textMessageReceived, peer, [&](const QString& text) {
            QJsonObject command = QJsonDocument::fromJson(text.toUtf8()).object();
            if (command["method"].toString() != "Network.getResponseBody") return;
            QString id = command["params"].toObject()["requestId"].toString();
            QJsonObject result{{"body", id == "large" ? QString(3 * 1024 * 1024, 'a') : QString("aGVsbG8=")}, {"base64Encoded", id != "large"}};
            peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"id", command["id"]}, {"result", result}}).toJson(QJsonDocument::Compact)));
        });
    });
    WebViewPage page;
    page.ws_url = QString("ws://127.0.0.1:%1").arg(server.serverPort()).toStdString();
    debugger.connectPage(page);
    waitUntil([&] { return debugger.isConnected() && peer; });
    sendEvent(debugger, requestEvent("body"));
    bool received = false;
    debugger.getResponseBody("body", [&](const QString& body, bool base64) {
        check(body == "aGVsbG8=" && base64, "base64 response corrupted"); received = true;
    });
    waitUntil([&] { return received; });
    received = false;
    debugger.getResponseBody("body", [&](const QString& body, bool base64) {
        check(body == "aGVsbG8=" && base64, "cached response lost base64 encoding"); received = true;
    });
    check(received, "response body cache missed");
    sendEvent(debugger, requestEvent("large"));
    received = false;
    debugger.getResponseBody("large", [&](const QString& body, bool) {
        check(body.size() == 3 * 1024 * 1024, "large response was truncated"); received = true;
    });
    waitUntil([&] { return received; });
    check(debugger.getRequest("large")->response_body.empty(), "large response remained in RAM cache");
    QString cacheFile = debugger.getRequest("large")->response_body_file;
    check(QFile::exists(cacheFile), "large response missing disk cache");
    received = false;
    debugger.getResponseBody("large", [&](const QString& body, bool base64) {
        check(body.size() == 3 * 1024 * 1024 && !base64, "disk cache lost response contents"); received = true;
    });
    check(received, "disk cache unnecessarily refetched response");
    bool disconnected = false;
    QObject::connect(debugger.findChild<QWebSocket*>(), &QWebSocket::disconnected, &server, [&] { disconnected = true; });
    debugger.disconnectPage();
    waitUntil([&] { return disconnected; });
    received = false;
    debugger.getResponseBody("large", [&](const QString& body, bool) {
        check(body.size() == 3 * 1024 * 1024, "disconnect lost the captured response"); received = true;
    });
    check(received, "captured response unavailable offline");
    debugger.clearRequests();
    check(!QFile::exists(cacheFile), "clear did not release temporary response files");
    debugger.disconnectPage();
}

static void testCommandOrderAndSessionShutdown() {
    QObject context;
    auto client = std::make_shared<HdcClient>(QCoreApplication::applicationFilePath().toStdString(), &context);
    std::vector<std::string> completed;
    for (const std::string value : {"one", "two", "three"}) {
        client->executeAsync({"shell", "uinput", value}, "performance-test-target", [&](const CommandResult& result) {
            check(result.success(), "queued control command failed");
            completed.push_back(result.out);
        });
    }
    waitUntil([&] { return completed.size() == 3; }, 10000);
    check(completed == std::vector<std::string>{"one", "two", "three"}, "control command ordering changed");
    Device device;
    device.serial = "performance-test-target";
    {
        QTcpServer server;
        check(server.listen(QHostAddress::LocalHost, 0), "local capture test server failed");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* peer = server.nextPendingConnection();
            peer->setParent(&server);
            peer->write(rawFrame(512, 1024, 17));
            peer->write(rawFrame(16, 32, 29));
        });
        StreamSession session(client, device);
        DecodedFrame latest;
        QObject::connect(&session, &StreamSession::frameReady, &context, [&](const DecodedFrame& frame) { latest = frame; });
        session.start();
        auto* socket = session.findChild<QTcpSocket*>();
        check(socket != nullptr, "capture socket missing");
        socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
        waitUntil([&] { return latest.width == 16; }, 10000);
        check(latest.image.constBits()[0] == 29 && session.deviceHeight() == 64, "bounded TCP pipeline lost final frame/source size");
        session.stop();
        check(!session.isRunning() && latest.image.constBits()[0] == 29, "stopping capture invalidated the retained image");
    }
    for (int i = 0; i < 10; ++i) {
        StreamSession session(client, device);
        session.start();
        session.stop();
        check(!session.isRunning(), "stream stayed active after stop");
        QCoreApplication::processEvents();
    }
}

static void testPausedLogsAndBoundedConsole() {
    MainWindow window(nullptr, false);
    window.show();
    auto* logs = window.findChild<QPlainTextEdit*>("LogView");
    auto* pause = window.findChild<QCheckBox*>("PauseHiLog");
    auto* console = window.findChild<QPlainTextEdit*>("WebConsoleView");
    check(logs && pause && console, "log controls missing");
    QMetaObject::invokeMethod(&window, "switchModule", Qt::DirectConnection, Q_ARG(int, 1));
    pause->setChecked(true);
    HiLogEntry entry;
    entry.timestamp = "12:00:00.000";
    entry.bundle = "com.yarward.test";
    entry.message = "received-while-paused";
    std::vector<HiLogEntry> entries{entry};
    QMetaObject::invokeMethod(&window, "onHiLogEntries", Qt::DirectConnection, Q_ARG(std::vector<HiLogEntry>, entries));
    check(!logs->toPlainText().contains("received-while-paused"), "pause must freeze visible log");
    pause->setChecked(false);
    waitUntil([&] { return logs->toPlainText().contains("received-while-paused"); });
    check(console->maximumBlockCount() > 0 && console->maximumBlockCount() <= 10000, "WebView document must have a memory bound");
    QMetaObject::invokeMethod(&window, "clearHiLog", Qt::DirectConnection);
    check(logs->toPlainText().isEmpty(), "clear left stale pending logs");

    QMetaObject::invokeMethod(&window, "switchLogSubtab", Qt::DirectConnection, Q_ARG(int, 1));
    auto* webPause = window.findChild<QCheckBox*>("PauseWebConsole");
    webPause->setChecked(true);
    WebConsoleMessage message;
    message.level = "info";
    message.timestamp = "12:00:00.000";
    message.text = "paused-web-console";
    QMetaObject::invokeMethod(&window, "onWebConsoleMessage", Qt::DirectConnection, Q_ARG(WebConsoleMessage, message));
    check(console->toPlainText().isEmpty(), "WebView pause did not freeze display");
    webPause->setChecked(false);
    waitUntil([&] { return console->toPlainText().contains("paused-web-console"); });
    for (int i = 0; i < 12000; ++i) {
        message.text = "console-entry-" + std::to_string(i);
        QMetaObject::invokeMethod(&window, "onWebConsoleMessage", Qt::DirectConnection, Q_ARG(WebConsoleMessage, message));
    }
    waitUntil([&] { return console->toPlainText().contains("console-entry-11999"); });
    check(console->document()->blockCount() <= 10000, "streaming console document grew without bound");
    QMetaObject::invokeMethod(&window, "clearWebViewConsole", Qt::DirectConnection);
    check(console->toPlainText().isEmpty(), "WebView clear left pending text");

    QMetaObject::invokeMethod(&window, "setWebViewSubtab", Qt::DirectConnection, Q_ARG(int, 1));
    auto* table = window.findChild<QTableWidget*>("NetworkTable");
    for (int i = 0; i < 1100; ++i) {
        WebNetworkRequest request;
        request.request_id = std::to_string(i);
        request.method = "GET";
        request.url = "https://example.test/" + request.request_id;
        QMetaObject::invokeMethod(&window, "onWebNetworkRequest", Qt::DirectConnection, Q_ARG(WebNetworkRequest, request));
    }
    check(table->rowCount() == 1000, "network rows exceeded retention limit");
    QString lastId = "1099", firstId = "0", mime = "application/json";
    QMetaObject::invokeMethod(&window, "onWebNetworkResponse", Qt::DirectConnection, Q_ARG(QString, lastId), Q_ARG(int, 200), Q_ARG(QString, mime));
    check(table->item(999, 0)->text() == "200", "indexed response updated wrong row after eviction");
    QMetaObject::invokeMethod(&window, "onWebNetworkResponse", Qt::DirectConnection, Q_ARG(QString, firstId), Q_ARG(int, 500), Q_ARG(QString, mime));
    check(table->rowCount() == 1000 && table->item(999, 0)->text() == "200", "late response corrupted retained rows");
    QMetaObject::invokeMethod(&window, "clearWebViewConsole", Qt::DirectConnection);
    check(table->rowCount() == 0, "clear retained network rows");

    QString output = "first-terminal-line\n";
    for (int i = 0; i < 11000; ++i) output += QString("terminal line %1\n").arg(i);
    output += "last-terminal-line";
    QMetaObject::invokeMethod(&window, "appendHdcOutput", Qt::DirectConnection, Q_ARG(QString, output));
    auto* terminal = window.findChild<QTextEdit*>("HdcTerminal");
    check(terminal->document()->blockCount() <= 10000, "terminal document exceeded memory bound");
    QMetaObject::invokeMethod(&window, "copyHdcOutput", Qt::DirectConnection);
    QString copied = QApplication::clipboard()->text();
    check(copied.contains("first-terminal-line") && copied.contains("last-terminal-line"), "terminal history lost content outside display limit");
    QMetaObject::invokeMethod(&window, "clearHdcTerminal", Qt::DirectConnection);
    QMetaObject::invokeMethod(&window, "copyHdcOutput", Qt::DirectConnection);
    check(QApplication::clipboard()->text().isEmpty(), "clear retained terminal history");
}

int main(int argc, char** argv) {
    // A deterministic HDC stand-in: never connects to or changes a real device.
    if (argc > 1 && std::string(argv[1]) == "fport") {
        std::cout << "OK";
        return 0;
    }
    if (argc > 3 && std::string(argv[1]) == "-t" && std::string(argv[2]) == "performance-test-target") {
        std::cout << (std::string(argv[3]) == "fport" ? "OK" : argv[argc - 1]);
        return 0;
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    try {
        testFragmentedFrames();
        testFrameLifetimeAndMailbox();
        testPresentationAndTouchCoordinates();
        testWorkerPipeline();
        testEncodedFrameOwnership();
        testNetworkRetentionAndBodies();
        testCommandOrderAndSessionShutdown();
        testPausedLogsAndBoundedConsole();
        std::cout << "Performance regression tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
