#include "harmonycast/AdbClient.h"
#include "harmonycast/ScrcpyDemuxer.h"
#include "harmonycast/DeviceCatalog.h"
#include "harmonycast/ControlChannel.h"
#include "harmonycast/AndroidStreamSession.h"
#include "harmonycast/MainWindow.h"
#include "harmonycast/DeviceSelector.h"
#include "harmonycast/NetworkDeviceDialog.h"
#include <QApplication>
#include <QBuffer>
#include <QElapsedTimer>
#include <QDataStream>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif
#include <iostream>
#include <stdexcept>
#include <QTimer>
#include <QMouseEvent>

using namespace harmonycast;

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class Predicate> static void waitUntil(Predicate predicate, int timeout = 4000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(2); }
    check(predicate(), "Android asynchronous operation timed out");
}

static std::shared_ptr<AdbClient> fakeClient() {
    return std::make_shared<AdbClient>(QCoreApplication::applicationFilePath().toStdString());
}

static Device androidDevice() {
    Device device; device.serial = "android-test"; device.platform = DevicePlatform::Android; return device;
}

static QByteArray streamHeader(const QByteArray& codec = "h264") {
    QByteArray data(65, '\0');
    data.replace(1, 9, "Android 8");
    data += codec;
    return data;
}

static QByteArray sessionHeader(int width, int height) {
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out << quint32(0x80000000) << quint32(width) << quint32(height);
    return data;
}

static QByteArray mediaPacket(quint64 pts, const QByteArray& payload) {
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out << pts << quint32(payload.size());
    return data + payload;
}

static void testDeviceIdentity() {
    Device oh; oh.serial = "192.0.2.1:5555"; oh.is_network = true;
    Device android = oh; android.platform = DevicePlatform::Android;
    check(oh.id() != android.id(), "platforms share a device identity");
    check(!isSameDevice(oh.id(), android.id()), "platforms are merged by address");
    check(deviceSerial(android.id()) == android.serial, "ADB receives a platform prefix");
    check(normalizeEndpoint("android:192.0.2.1") == android.id(), "Android uses the HDC default port");
    check(!isSameDevice(android.id(), "android:192.0.2.1:5556"), "distinct ADB ports are merged");
    const auto entries = DeviceCatalog::merge({oh, android}, {});
    check(entries.size() == 2, "device catalog drops one platform");
    check(entries[1].label().contains("Android"), "device list does not identify Android");
    Device reserved; reserved.serial = "android:usb-board";
    check(!isAndroidDevice(reserved.id()) && deviceSerial(reserved.id()) == reserved.serial,
          "a Harmony USB serial can impersonate an Android device key");
}

static void testAdbDiscovery() {
    const auto devices = AdbClient::parseDeviceList(
        "List of devices attached\n"
        "usb-one\tdevice product:board model:Android_8_Panel device:board transport_id:1\n"
        "192.0.2.1:5555 device model:RK3288 transport_id:2\n"
        "denied unauthorized usb:2-1\n"
        "down offline\n"
        "* daemon started successfully *\n");
    check(devices.size() == 4, "ADB device states were not parsed");
    check(devices[0].platform == DevicePlatform::Android && !devices[0].is_network,
          "USB Android transport is incorrect");
    check(devices[0].model == "Android 8 Panel", "ADB model name was lost");
    check(devices[1].is_network && devices[1].port == 5555, "ADB TCP endpoint was lost");
    check(devices[2].state == DeviceState::Unauthorized && devices[3].state == DeviceState::Offline,
          "ADB authorization/offline states were hidden");
    check(AdbClient::parseDeviceList("error: no devices/emulators found\n").empty(), "ADB error became a device");
    check(AdbClient::commandArguments("adb shell getprop ro.product.model") ==
          std::vector<std::string>{"shell", "getprop ro.product.model"}, "pasted ADB shell command was changed");
    check(AdbClient::commandArguments("push \"/tmp/a b.apk\" /data/local/tmp/", false) ==
          std::vector<std::string>{"push", "/tmp/a b.apk", "/data/local/tmp/"}, "ADB paths containing spaces split");
}

static void testScrcpyProtocol() {
    ScrcpyDemuxer parser;
    QString error;
    std::vector<StreamPacket> packets;
    std::vector<QSize> sizes;
    parser.setErrorCallback([&](const QString& value) { error = value; });
    parser.setPacketCallback([&](const StreamPacket& packet) { packets.push_back(packet); });
    parser.setHeaderCallback([&](const std::string& name, int w, int h) {
        check(name == "Android 8", "scrcpy name metadata was corrupted");
        sizes.emplace_back(w, h);
    });
    const auto config = QByteArray::fromHex("000000016742001e");
    const auto idr = QByteArray::fromHex("00000001658884");
    const auto data = streamHeader() + sessionHeader(720, 1280) +
        mediaPacket(quint64(1) << 62, config) + mediaPacket((quint64(1) << 61) | 12345, idr) +
        sessionHeader(1280, 720) + mediaPacket(23456, idr);
    for (char byte : data) {
        const uint8_t value = static_cast<uint8_t>(byte);
        parser.feed(std::span<const uint8_t>(&value, 1));
    }
    check(error.isEmpty() && packets.size() == 3 && sizes.size() == 2, "fragmented scrcpy stream failed");
    check(packets[0].payload == std::vector<uint8_t>(config.begin(), config.end()), "codec config was discarded");
    check(packets[1].is_keyframe && packets[1].pts == 12345, "scrcpy keyframe/PTS flags were not decoded");
    check(sizes.back() == QSize(1280, 720), "rotation session metadata became a video packet");

    parser.reset(); error.clear();
    auto invalid = streamHeader("h265");
    parser.feed(std::span(reinterpret_cast<const uint8_t*>(invalid.constData()), size_t(invalid.size())));
    check(!error.isEmpty(), "unsupported codec was silently accepted");
    parser.reset(); error.clear();
    invalid = streamHeader() + sessionHeader(0, 1280);
    parser.feed(std::span(reinterpret_cast<const uint8_t*>(invalid.constData()), size_t(invalid.size())));
    check(!error.isEmpty(), "invalid dimensions were accepted");
    parser.reset(); error.clear();
    invalid = streamHeader() + sessionHeader(720, 1280);
    QDataStream out(&invalid, QIODevice::Append);
    out << quint64(0) << quint32(64 * 1024 * 1024);
    parser.feed(std::span(reinterpret_cast<const uint8_t*>(invalid.constData()), size_t(invalid.size())));
    check(!error.isEmpty(), "oversized scrcpy packet was buffered");
}

static void testControlMapping() {
    QTcpServer server; check(server.listen(QHostAddress::LocalHost), "control fixture unavailable");
    ControlChannel control(nullptr, DevicePlatform::Android);
    check(control.connectToHost("127.0.0.1", server.serverPort()), "Android control connection failed");
    waitUntil([&] { return server.hasPendingConnections(); });
    auto* peer = server.nextPendingConnection();
    check(control.sendTouch(TouchAction::Move, 20, 30, 720, 1280), "Android movement was not sent");
    check(control.sendTouch(TouchAction::Up, 20, 30, 720, 1280), "Android release was not sent");
    waitUntil([&] { return peer->bytesAvailable() >= 64; });
    const auto packets = peer->readAll();
    check(packets.size() == 64 && packets[1] == 2 && packets[33] == 1, "Android move/up action codes are reversed");
    check(packets[54] == 0 && packets[55] == 0, "released Android pointer retains pressure");
}

static void testAdbProcessAndInfo() {
    const auto result = fakeClient()->run({"exec-out", "screencap", "-p"}, "android:android-test");
    check(result.success() && !QImage::fromData(reinterpret_cast<const uchar*>(result.out.data()), int(result.out.size()), "PNG").isNull(),
          "ADB binary screenshot was corrupted or target prefix leaked");
    const std::string fixture =
        "<<<PROP>>>\n[ro.product.model]: [Android 8 Panel]\n[ro.product.brand]: [Test]\n"
        "[ro.build.version.sdk]: [26]\n[ro.build.version.release]: [8.0]\n[ro.product.cpu.abi]: [armeabi-v7a]\n"
        "<<<UPTIME>>>\n90061.2 0\n<<<MEM>>>\nMemTotal: 2097152 kB\nMemAvailable: 1048576 kB\n"
        "<<<DISPLAY>>>\nPhysical size: 720x1280\nOverride size: 540x960\nSurfaceOrientation: 1\n"
        "<<<NET>>>\n2: eth0 inet 192.0.2.4/24 brd 192.0.2.255 scope global eth0\n"
        "<<<MAC>>>\neth0 02:00:00:00:00:01\n<<<PKGS>>>\npackage:com.yarward.demo\npackage:com.android.settings\n"
        "<<<PS>>>\n12 com.yarward.demo\n<<<DETAILS>>>\n===PACKAGE:com.yarward.demo===\nversionCode=12 targetSdk=26\nversionName=1.2\n";
    const auto info = AdbClient::parseDeviceInfo("android:android-test", fixture);
    check(info.os_fullname == "Android 8.0" && info.api_version == "26" && info.cpu_abi == "armeabi-v7a", "Android properties were lost");
    check(info.sn.empty() && info.udid.empty(), "ADB transport identity is reported as a hardware identifier");
    check(info.display_width == 540 && info.display_height == 960 && info.display_rotation == 90, "Android display override ignored");
    check(info.uptime_seconds == 90061 && info.mem_free_kb == 1048576, "Android metrics were lost");
    check(info.networks.size() == 1 && info.networks[0].mac == "02:00:00:00:00:01", "Android interfaces were lost");
    check(info.yarward_apps.size() == 1 && info.yarward_apps[0].running && info.yarward_apps[0].version_name == "1.2", "Android applications were lost");
    check(AdbClient::screenshotImage({1, result.out, {}}).isNull(), "failed screenshot command is treated as successful");
    check(AdbClient::screenshotImage({0, "not a PNG", {}}).isNull(), "invalid screenshot was accepted");
    auto truncated = result; truncated.out.resize(truncated.out.size() - 5);
    check(AdbClient::screenshotImage(truncated).isNull(), "truncated screenshot is treated as a complete frame");
    check(fakeClient()->run({"test-timeout"}, "android:android-test", 50).exit_code == -2, "ADB timeout was not enforced");
    check(AdbClient("/missing/adb").run({"devices"}).exit_code != 0, "missing ADB executable reports success");
    const auto client = fakeClient();
    check(!AdbClient::findAdb().empty(), "ADB discovery does not return a tool path");
    check(client->forwardPort("android:android-test", 40000, 5555) && client->removeForward("android:android-test", 40000),
          "ADB port lifecycle failed");
    check(client->wakeup("android:android-test") && client->home("android:android-test") && client->back("android:android-test"),
          "Android shell navigation failed");
    check(client->fileSend("android:android-test", "/tmp/a b.apk", "/data/local/tmp/") &&
          client->fileRecv("android:android-test", "/sdcard/a b.txt", "/tmp/a b.txt"), "ADB file transfer failed");
    check(client->enableTcpMode("android:android-test", 5555) && client->restartServer(), "ADB TCP setup failed");
    check(client->getDeviceInfo("android:android-test").has_value(), "Android info query failed");
}

static QByteArray commandLog() {
    QFile file(qEnvironmentVariable("HC_ANDROID_TEST_CALLS"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

static void resetCommands() {
    QFile file(qEnvironmentVariable("HC_ANDROID_TEST_CALLS"));
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "ADB command log unavailable");
}

static void testSnapshotFallback() {
    AndroidStreamSession session(fakeClient(), androidDevice(), 8000000, nullptr, {1000, 1500}, "/missing/server");
    int frames = 0; QStringList statuses;
    QObject::connect(&session, &MirrorSession::frameReady, &session, [&](const DecodedFrame& frame) {
        check(frame.width == 64 && frame.height == 48, "Android snapshot dimensions changed"); ++frames;
    });
    QObject::connect(&session, &MirrorSession::statusChanged, &session, [&](const QString& status) { statuses << status; });
    session.start(); waitUntil([&] { return frames >= 2; });
    check(session.isFallbackSnapshot() && statuses.join('\n').contains("已启用 Android"), "Android fallback was not confirmed");
    session.stop(); const auto stoppedFrames = frames;
    QElapsedTimer timer; timer.start(); waitUntil([&] { return timer.elapsed() > 300; });
    check(frames == stoppedFrames, "stopped Android fallback still delivers frames");
}

static void testNativeSession(bool workbench = false) {
    qputenv("HC_ANDROID_TEST_MODE", "native");
    QTcpServer server; check(server.listen(QHostAddress::LocalHost), "video fixture unavailable");
    qputenv("HC_ANDROID_TEST_PORT", QByteArray::number(server.serverPort()));
    QTcpSocket* video = nullptr; QTcpSocket* control = nullptr;
    bool rejectedEarlyConnection = false;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            auto* socket = server.nextPendingConnection();
            if (!rejectedEarlyConnection) {
                rejectedEarlyConnection = true; socket->disconnectFromHost(); socket->deleteLater(); continue;
            }
            if (!video) { video = socket; video->write(QByteArray(1, '\0')); }
            else control = socket;
            if (video && control) {
                QFile fixture(QStringLiteral(HARMONYCAST_TEST_FIXTURES "/video-64x48.h264"));
                check(fixture.open(QIODevice::ReadOnly), "H264 fixture unavailable");
                const auto encoded = fixture.readAll();
                QByteArray wire = streamHeader().mid(1) + sessionHeader(64, 48);
                std::vector<int> starts;
                for (int i = 0; i + 3 < encoded.size(); ++i) {
                    if (encoded.mid(i, 3) == QByteArray::fromHex("000001")) { starts.push_back(i); i += 2; }
                }
                starts.push_back(encoded.size());
                for (size_t i = 0; i + 1 < starts.size(); ++i) {
                    const auto nal = encoded.mid(starts[i], starts[i + 1] - starts[i]);
                    const auto type = uint8_t(nal[3]) & 31;
                    const quint64 flags = type == 7 || type == 8 ? quint64(1) << 62 :
                        type == 5 ? quint64(1) << 61 : 0;
                    wire += mediaPacket(flags | quint64(i * 1000), nal);
                }
                video->write(wire);
            }
        }
    });
    if (workbench) {
        QSettings settings("HarmonyCast", "HarmonyCast"); settings.setValue("auto_start_mirror", false);
        MainWindow window(nullptr, false, "/missing/hdc", QCoreApplication::applicationFilePath().toStdString());
        std::vector<Device> devices{androidDevice()};
        check(QMetaObject::invokeMethod(&window, "onDevicesUpdated", Qt::DirectConnection,
            Q_ARG(std::vector<Device>, devices)), "workbench discovery unavailable");
        check(QMetaObject::invokeMethod(&window, "switchModule", Qt::DirectConnection, Q_ARG(int, 1)), "mirror module unavailable");
        window.show();
        check(QMetaObject::invokeMethod(&window, "startMirror", Qt::DirectConnection), "mirror action unavailable");
        auto* screen = window.findChild<ScreenView*>();
        check(screen, "workbench screen unavailable");
        waitUntil([&] { return !screen->image().isNull(); });
        check(window.isProjecting(), "workbench does not project Android frames");
        const QPointF center(screen->rect().center());
        const QPointF global(screen->mapToGlobal(center.toPoint()));
        QMouseEvent down(QEvent::MouseButtonPress, center, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease, center, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(screen, &down); QApplication::sendEvent(screen, &up);
        check(QMetaObject::invokeMethod(&window, "sendKey", Qt::DirectConnection, Q_ARG(QString, QString("home"))), "navigation action unavailable");
        waitUntil([&] { return control && control->bytesAvailable() >= 78; });
        const auto input = control->readAll();
        check(input[1] == 0 && input[33] == 1 && input.mid(66, 4) == QByteArray::fromHex("00000003"),
              "workbench sends Harmony touch/key codes to Android");
        const auto artifacts = qEnvironmentVariable("HC_ANDROID_TEST_ARTIFACTS");
        if (!artifacts.isEmpty()) check(window.grab().save(artifacts + "/android-workbench.png"), "workbench preview unavailable");
        const auto streams = window.findChildren<MirrorSession*>();
        check(streams.size() == 1 && QMetaObject::invokeMethod(streams.front(), "streamFinished", Qt::DirectConnection,
            Q_ARG(QString, QString("old session failure"))), "queued failure fixture unavailable");
        check(QMetaObject::invokeMethod(&window, "stopMirror", Qt::DirectConnection, Q_ARG(bool, true)), "stop action unavailable");
        check(!window.isProjecting(), "Android workbench cannot stop mirroring");
        check(QMetaObject::invokeMethod(&window, "startMirror", Qt::DirectConnection), "restart action unavailable");
        QCoreApplication::processEvents();
        check(window.isProjecting(), "a queued old session failure stopped the replacement session");
        QMetaObject::invokeMethod(&window, "stopMirror", Qt::DirectConnection, Q_ARG(bool, true));
    } else {
        AndroidStreamSession session(fakeClient(), androidDevice(), 8000000, nullptr, {3000, 1500});
        int frames = 0;
        QObject::connect(&session, &MirrorSession::frameReady, &session, [&](const DecodedFrame& frame) {
            check(frame.width == 64 && frame.height == 48, "Android native frame was not decoded"); ++frames;
        });
        session.start(); waitUntil([&] { return frames > 0; });
        check(!session.isFallbackSnapshot() && session.controlChannel()->isConnected(), "native Android video/control handshake failed");
        session.stop();
    }
    waitUntil([] {
        for (auto* process : QCoreApplication::instance()->findChildren<QProcess*>())
            if (process->state() != QProcess::NotRunning) return false;
        return true;
    });
    qputenv("HC_ANDROID_TEST_MODE", "");
}

static void testSessionFailures() {
    qputenv("HC_ANDROID_TEST_MODE", "sdk-old");
    AndroidStreamSession old(fakeClient(), androidDevice(), 8000000, nullptr, {2000, 600});
    QString error;
    QObject::connect(&old, &MirrorSession::streamFinished, &old, [&](const QString& text) { error = text; });
    old.start(); waitUntil([&] { return !error.isEmpty(); });
    check(error.contains("Android 8.0") && !old.isActive(), "unsupported Android version continues mirroring");
    qputenv("HC_ANDROID_TEST_MODE", "permission");
    AndroidStreamSession denied(fakeClient(), androidDevice(), 8000000, nullptr, {1000, 700}, "/missing/server");
    error.clear();
    QObject::connect(&denied, &MirrorSession::streamFinished, &denied, [&](const QString& text) { error = text; });
    denied.start(); waitUntil([&] { return !error.isEmpty(); });
    check(error.contains("截图权限") && !denied.isActive(), "Android screenshot permission failure is hidden");
    qputenv("HC_ANDROID_TEST_MODE", "bad-png");
    // Leave time for the instrumented child to report the invalid image first.
    AndroidStreamSession invalid(fakeClient(), androidDevice(), 8000000, nullptr, {1000, 1500}, "/missing/server");
    error.clear();
    QObject::connect(&invalid, &MirrorSession::streamFinished, &invalid, [&](const QString& text) { error = text; });
    invalid.start(); waitUntil([&] { return !error.isEmpty(); });
    check(error.contains("incomplete PNG"), "Android fallback timeout loses the last failure");
    qputenv("HC_ANDROID_TEST_MODE", "slow-png");
    resetCommands();
    auto destroyed = std::make_unique<AndroidStreamSession>(fakeClient(), androidDevice(), 8000000, nullptr,
        StreamTimeouts{1000, 1500}, "/missing/server");
    destroyed->start(); waitUntil([] { return commandLog().contains("exec-out"); }); destroyed.reset();
    QElapsedTimer timer; timer.start(); waitUntil([&] { return timer.elapsed() > 700; });
    qputenv("HC_ANDROID_TEST_MODE", "");
}

static void testDiscoveryIsolation() {
    auto hdc = std::make_shared<HdcClient>(QCoreApplication::applicationFilePath().toStdString());
    DeviceManager manager(hdc, nullptr, fakeClient());
    bool updated = false;
    QObject::connect(&manager, &DeviceManager::devicesUpdated, &manager, [&](const auto&) { updated = true; });
    manager.refresh(true); waitUntil([&] { return updated; });
    check(manager.findDevice("android:android-test").has_value() && manager.findDevice("android-test").has_value(),
          "combined discovery loses one device platform");
}

static void testAndroidLogcat() {
    HiLogSession session(QCoreApplication::applicationFilePath().toStdString(), "android:android-test");
    bool resolved = false;
    QObject::connect(&session, &HiLogSession::entriesReady, &session, [&](const std::vector<HiLogEntry>& entries) {
        for (const auto& entry : entries) if (entry.bundle == "com.yarward.demo" && entry.level == LogLevel::Debug) resolved = true;
    });
    session.start(); waitUntil([&] { return resolved; }); session.stop();
}

static void testAndroidInspection() {
    const auto imported = InspectionReport::parseTargets("192.0.2.8\n192.0.2.9:6000", DevicePlatform::Android);
    check(imported.errors.isEmpty() && imported.targets[0].port == 5555 && imported.targets[1].port == 6000,
          "Android inspection uses HDC ports");
    const auto script = InspectionReport::collectionScript(DevicePlatform::Android);
    check(script.contains("getprop ro.build.version.sdk") && !script.contains("param get"), "Android inspection sends Harmony commands");
    InspectionRunner runner(fakeClient(), nullptr, 1500);
    check(runner.start({imported.targets[0]}), "Android inspection did not start");
    waitUntil([&] { return !runner.isRunning(); });
    check(runner.results().size() == 1 && runner.results()[0].systemVersion == "Android 8.0" &&
          runner.results()[0].status == InspectionStatus::Partial, "Android inspection discarded readable device data");
}

static void testPlatformSelection() {
    QSettings settings("HarmonyCast", "HarmonyCast"); settings.setValue("auto_start_mirror", false);
    MainWindow window(nullptr, false, "/missing/hdc", QCoreApplication::applicationFilePath().toStdString());
    Device oh; oh.serial = "same-usb";
    Device android = oh; android.platform = DevicePlatform::Android;
    std::vector<Device> devices{oh, android};
    check(QMetaObject::invokeMethod(&window, "onDevicesUpdated", Qt::DirectConnection,
        Q_ARG(std::vector<Device>, devices)), "device discovery slot unavailable");
    auto* picker = window.findChild<DeviceSelector*>("DeviceSelector");
    auto* mode = window.findChild<QComboBox*>("HdcCommandMode");
    check(picker && mode && picker->findData("same-usb") >= 0 && picker->findData("android:same-usb") >= 0,
          "workbench drops an Android device with the same serial");
    picker->setCurrentIndex(picker->findData("android:same-usb"));
    check(mode->itemText(1) == "ADB 命令", "Android selection still exposes HDC commands");
    picker->setCurrentIndex(picker->findData("same-usb"));
    check(mode->itemText(1) == "HDC 命令", "switching back to Harmony leaves the Android backend active");
    resetCommands();
    const std::string target = "android:192.0.2.44:6000";
    qputenv("HC_ANDROID_TEST_NETWORK", "192.0.2.44:6000");
    check(QMetaObject::invokeMethod(&window, "switchToSingleDevice", Qt::DirectConnection,
        Q_ARG(std::string, target)), "Android network connection action unavailable");
    waitUntil([&] { return picker->isEnabled() && picker->currentData().toString() == QString::fromStdString(target); });
    check(commandLog().contains("connect 192.0.2.44:6000") && !commandLog().contains("tconn"),
          "Android network connection uses an HDC command");
    check(picker->findData("same-usb") >= 0, "Android connection removes cached Harmony devices");
    check(QMetaObject::invokeMethod(&window, "stopMirror", Qt::DirectConnection, Q_ARG(bool, true)), "cancel pending mirror action unavailable");
    qputenv("HC_ANDROID_TEST_NETWORK", "");
}

static void testNetworkPersistence() {
    {
        QFile legacy(NetworkDeviceDialog::savedDevicesFilePath());
        check(legacy.open(QIODevice::WriteOnly), "legacy device fixture unavailable");
        legacy.write(R"([{"ip":"192.0.2.9","port":5555,"name":"OH"}])");
    }
    const auto legacy = NetworkDeviceDialog::loadSavedDevices();
    check(legacy.size() == 1 && legacy[0].platform == DevicePlatform::OpenHarmony,
          "legacy saved devices are reclassified as Android");
    NetworkDeviceDialog::saveDevice("192.0.2.9", "OH", 5555);
    NetworkDeviceDialog::saveDevice("192.0.2.9", "Android", 5555, DevicePlatform::Android);
    auto saved = NetworkDeviceDialog::loadSavedDevices();
    check(saved.size() == 2, "saving Android overwrites Harmony at the same endpoint");
    NetworkDeviceDialog dialog;
    auto* platform = dialog.findChild<QComboBox*>("DeviceHubPlatform");
    auto* port = dialog.findChild<QLineEdit*>("DeviceHubPort");
    check(platform && port, "device hub lacks a platform choice");
    platform->setCurrentIndex(1);
    check(port->text() == "5555", "Android default network port is wrong");
    const auto artifacts = qEnvironmentVariable("HC_ANDROID_TEST_ARTIFACTS");
    if (!artifacts.isEmpty()) { dialog.show(); QCoreApplication::processEvents(); check(dialog.grab().save(artifacts + "/android-device-hub.png"), "Android hub preview failed"); }
    NetworkDeviceDialog::deleteSavedDevice("android:192.0.2.9:5555");
    saved = NetworkDeviceDialog::loadSavedDevices();
    check(saved.size() == 1 && saved[0].platform == DevicePlatform::OpenHarmony, "Android deletion removes a Harmony record");
}

static int fakeAdb(int argc, char** argv) {
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromUtf8(argv[i]);
    if (!qgetenv("HC_ANDROID_TEST_CALLS").isEmpty()) {
        QFile log(qEnvironmentVariable("HC_ANDROID_TEST_CALLS"));
        if (log.open(QIODevice::WriteOnly | QIODevice::Append)) log.write(args.join(' ').toUtf8() + '\n');
    }
    if (args.contains("-s") && args.value(args.indexOf("-s") + 1).startsWith("android:")) return 2;
    if (args.contains("exec-out")) {
        if (qgetenv("HC_ANDROID_TEST_MODE") == "permission") { std::cerr << "Permission denied"; return 1; }
        if (qgetenv("HC_ANDROID_TEST_MODE") == "bad-png") { std::cout << "incomplete PNG"; return 0; }
        if (qgetenv("HC_ANDROID_TEST_MODE") == "slow-png") QThread::msleep(600);
        QFile image(qEnvironmentVariable("HC_ANDROID_TEST_PNG"));
        if (!image.open(QIODevice::ReadOnly)) return 1;
        const auto data = image.readAll();
#ifdef Q_OS_WIN
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::cout.write(data.constData(), data.size());
    } else if (args.contains("devices")) {
        std::cout << "List of devices attached\nandroid-test device model:Android_8\n";
        if (!qgetenv("HC_ANDROID_TEST_NETWORK").isEmpty()) std::cout << qgetenv("HC_ANDROID_TEST_NETWORK").constData() << " device model:Network_Android_8\n";
    }
    else if (args.contains("targets")) std::cout << "android-test USB Connected Harmony_Board\n";
    else if (args.contains("getprop")) std::cout << (qgetenv("HC_ANDROID_TEST_MODE") == "sdk-old" ? "25\n" : "26\n");
    else if (args.contains("test-timeout")) QThread::sleep(2);
    else if (args.contains("logcat")) {
        for (int i = 0; i < 10; ++i) {
            std::cout << "11-12 12:00:01.123 12 12 V Demo: Android verbose log\n" << std::flush;
            QThread::msleep(100);
        }
    } else if (args.join(' ').contains("<<<HC_INSPECTION:BEGIN>>>")) {
        std::cout << "<<<HC_INSPECTION:BEGIN>>>\n<<<HC_INSPECTION:ICU>>>\nmodel: Android panel\n"
            "<<<HC_INSPECTION:SYSTEM>>>\nAndroid 8.0\n<<<HC_INSPECTION:API>>>\n26\n"
            "<<<HC_INSPECTION:UPTIME>>>\n100 0\n<<<HC_INSPECTION:END>>>\n";
    } else if (args.join(' ').contains("ps -A -o PID,NAME")) std::cout << "PID NAME\n12 com.yarward.demo\n";
    else if (args.contains("forward") && args.contains("tcp:0")) std::cout << qgetenv("HC_ANDROID_TEST_PORT").constData() << '\n';
    else if (args.join(' ').contains("app_process")) QThread::sleep(10);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return fakeAdb(argc, argv);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("HARMONYCAST_SOFTWARE_RENDERING", "1");
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    const auto savedPath = NetworkDeviceDialog::savedDevicesFilePath();
    QFile previous(savedPath); QByteArray oldSaved;
    const bool existed = previous.open(QIODevice::ReadOnly);
    if (existed) { oldSaved = previous.readAll(); previous.close(); }
    { QFile file(savedPath); check(file.open(QIODevice::WriteOnly), "test device storage unavailable"); file.write("[]"); }
    qputenv("HC_ANDROID_TEST_PNG", temporary.filePath("snapshot.png").toUtf8());
    qputenv("HC_ANDROID_TEST_CALLS", temporary.filePath("commands.log").toUtf8());
    QImage image(64, 48, QImage::Format_RGB32); image.fill(Qt::red);
    check(image.save(temporary.filePath("snapshot.png"), "PNG"), "PNG fixture unavailable");
    int result = 0;
    try {
        testDeviceIdentity();
        testAdbDiscovery();
        testDiscoveryIsolation();
        testScrcpyProtocol();
        testControlMapping();
        testAdbProcessAndInfo();
        testSnapshotFallback();
        testNativeSession();
        testNativeSession(true);
        testSessionFailures();
        testAndroidLogcat();
        testAndroidInspection();
        testPlatformSelection();
        testNetworkPersistence();
        std::cout << "Android compatibility tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (existed) { QFile file(savedPath); if (file.open(QIODevice::WriteOnly)) file.write(oldSaved); }
    else QFile::remove(savedPath);
    return result;
}
