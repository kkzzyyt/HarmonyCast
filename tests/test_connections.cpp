#include "harmonycast/HdcClient.h"
#include "harmonycast/MainWindow.h"
#include "harmonycast/NetworkDeviceDialog.h"
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>

using namespace harmonycast;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static const char* verboseTargets =
    "usb-offline\t\tUSB\tOffline\tConnected-terminal\r\n"
    "192.0.2.55:9000     TCP     Connected     HR36     hdc\r\n"
    "usb-unauthorized\tUSB\tUnauthorized\tConnected\n"
    "usb-ready\tUSB\tReady\tlocalhost\n"
    "usb-unknown\tUSB\tUnknown\tlocalhost\n"
    "c8:f5:d6:86:37:01\tUSB\tConnected\tlocalhost\n";

static void testDeviceStates() {
    const auto devices = HdcClient::parseDeviceList(verboseTargets);
    check(devices.size() == 6, "verbose target list lost devices");
    check(devices[0].state == DeviceState::Offline, "model name masked Offline state");
    check(devices[1].state == DeviceState::Connected, "connected TCP target was not recognized");
    check(devices[1].is_network && devices[1].ip == "192.0.2.55" && devices[1].port == 9000,
          "custom TCP endpoint changed");
    check(devices[2].state == DeviceState::Unauthorized, "model name masked Unauthorized state");
    check(devices[3].state != DeviceState::Connected, "Ready target must not start mirroring");
    check(devices[4].state != DeviceState::Connected, "Unknown target must not start mirroring");
    check(!devices[5].is_network, "USB serial containing colons was mistaken for TCP");

    const auto legacy = HdcClient::parseDeviceList("  usb-connected\r\nusb-denied Unauthorized\nusb-down\tOffline\n");
    check(legacy.size() == 3, "legacy target list is no longer supported");
    check(legacy[0].state == DeviceState::Connected, "bare legacy serial must remain connected");
    check(legacy[1].state == DeviceState::Unauthorized, "legacy Unauthorized state lost");
    check(legacy[2].state == DeviceState::Offline, "legacy Offline state lost");
    check(HdcClient::parseDeviceList("[Empty]\r\n").empty(), "empty marker became a device");
    check(HdcClient::parseDeviceList("[Fail]Failed to communicate with daemon.\n").empty(),
          "HDC error became a device");
}

static void testDeviceQuery() {
    HdcClient hdc(QCoreApplication::applicationFilePath().toStdString());
    qputenv("HC_CONNECTION_TEST_MODE", "query");
    auto devices = hdc.listDevicesChecked();
    check(devices && devices->size() == 6, "device discovery must request verbose states");
    check((*devices)[0].state == DeviceState::Offline, "query hid the offline device");
    check((*devices)[2].state == DeviceState::Unauthorized, "query hid the unauthorized device");

    qputenv("HC_CONNECTION_TEST_MODE", "query-failure");
    check(!hdc.listDevicesChecked(), "HDC failure with exit code zero must stay a failed query");
    qputenv("HC_CONNECTION_TEST_MODE", "empty");
    devices = hdc.listDevicesChecked();
    check(devices && devices->empty(), "successful empty list was treated as a query failure");
    qunsetenv("HC_CONNECTION_TEST_MODE");
}

template<class Predicate> static void waitUntil(Predicate predicate, int timeout = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    check(predicate(), "connection UI did not finish in time");
}

static QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

static void processFor(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    waitUntil([&] { return timer.elapsed() >= milliseconds; });
}

static QComboBox* deviceCombo(MainWindow& window, const QString& target) {
    for (auto* combo : window.findChildren<QComboBox*>()) {
        if (combo->findData(target) >= 0) return combo;
    }
    throw std::runtime_error("device selector missing");
}

static void testOfflineDoesNotReconnect(const QString& commandLog) {
    qputenv("HC_CONNECTION_TEST_MODE", "empty");
    QSettings("HarmonyCast", "HarmonyCast").setValue("auto_start_mirror", true);
    MainWindow window(nullptr, false, QCoreApplication::applicationFilePath().toStdString());
    std::vector<Device> devices = HdcClient::parseDeviceList("192.0.2.55:9000 TCP Offline HR36\n");
    check(QMetaObject::invokeMethod(&window, "onDevicesUpdated", Qt::DirectConnection,
                                   Q_ARG(std::vector<Device>, devices)), "device update slot unavailable");
    processFor(350);
    // Let an unexpected attempt finish before destroying its window in the failing version.
    auto* screen = window.findChild<ScreenView*>();
    waitUntil([&] { return !screen->statusMessage().contains("正在连接设备"); });
    check(!readFile(commandLog).contains("tconn"), "offline polling triggered an unsolicited reconnect");
    check(!window.isProjecting(), "offline target started mirroring");
}

static void testWirelessDebounce() {
    auto hdc = std::make_shared<HdcClient>(QCoreApplication::applicationFilePath().toStdString());
    DeviceManager manager(hdc);
    int updates = 0;
    int disconnects = 0;
    QObject::connect(&manager, &DeviceManager::devicesUpdated, &manager, [&](const std::vector<Device>&) { ++updates; });
    QObject::connect(&manager, &DeviceManager::deviceDisconnected, &manager, [&](const Device&) { ++disconnects; });
    auto poll = [&](const char* mode) {
        qputenv("HC_CONNECTION_TEST_MODE", mode);
        const int previous = updates;
        manager.refresh(true);
        waitUntil([&] { return updates > previous; });
        auto devices = manager.devices();
        check(devices.size() == 1, "polling lost the wireless target");
        return devices.front().state;
    };
    check(poll("online") == DeviceState::Connected, "initial connection missing");
    check(poll("offline") == DeviceState::Connected, "first explicit Offline poll bypassed wireless debounce");
    check(poll("offline") == DeviceState::Connected, "second explicit Offline poll bypassed wireless debounce");
    check(poll("online") == DeviceState::Connected, "wireless recovery failed");
    check(poll("offline") == DeviceState::Connected, "recovery did not reset wireless debounce");
    check(poll("offline") == DeviceState::Connected, "wireless debounce ended too early");
    check(poll("offline") == DeviceState::Offline && disconnects == 1, "persistent Offline did not report one disconnect");
    check(poll("online") == DeviceState::Connected, "target did not reconnect");
    check(poll("denied") == DeviceState::Unauthorized && disconnects == 2,
          "authorization loss must be reported immediately");
}

static void testOnlineSelectionAndPort() {
    qputenv("HC_CONNECTION_TEST_MODE", "empty");
    QSettings("HarmonyCast", "HarmonyCast").setValue("auto_start_mirror", false);
    NetworkDeviceDialog::saveDevice("192.0.2.55", "", 9000);
    MainWindow window(nullptr, false, QCoreApplication::applicationFilePath().toStdString());
    const auto devices = HdcClient::parseDeviceList(verboseTargets);
    check(QMetaObject::invokeMethod(&window, "onDevicesUpdated", Qt::DirectConnection,
                                   Q_ARG(std::vector<Device>, devices)), "device update slot unavailable");
    auto* combo = deviceCombo(window, "192.0.2.55:9000");
    check(combo->currentData().toString() == "192.0.2.55:9000", "initial selection chose an offline target over an online target");
    const auto saved = NetworkDeviceDialog::loadSavedDevices();
    auto found = std::find_if(saved.begin(), saved.end(), [](const SavedNetworkDevice& device) { return device.ip == "192.0.2.55"; });
    check(found != saved.end() && found->port == 9000, "device refresh replaced the custom port with 8710");
}

static void testConnectionFailure(const QByteArray& mode, const QString& expected) {
    qputenv("HC_CONNECTION_TEST_MODE", mode);
    QSettings("HarmonyCast", "HarmonyCast").setValue("auto_start_mirror", false);
    MainWindow window(nullptr, false, QCoreApplication::applicationFilePath().toStdString());
    check(QMetaObject::invokeMethod(&window, "switchModule", Qt::DirectConnection, Q_ARG(int, 1)), "mirror navigation unavailable");
    const std::string endpoint = "192.0.2.55:9000";
    check(QMetaObject::invokeMethod(&window, "switchToSingleDevice", Qt::DirectConnection,
                                   Q_ARG(std::string, endpoint)), "connection slot unavailable");
    auto* screen = window.findChild<ScreenView*>();
    check(screen != nullptr, "mirror view missing");
    waitUntil([&] {
        for (auto* combo : window.findChildren<QComboBox*>()) {
            if (combo->findData("192.0.2.55:9000") >= 0 && combo->isEnabled()) {
                return !screen->statusMessage().contains("正在连接设备");
            }
        }
        return false;
    }, 15000); // Multiple child processes are slower under sanitizer builds.
    processFor(350);
    check(screen->statusMessage().contains(expected), "connection failure reason was hidden by device refresh");
    check(!window.isProjecting(), "failed connection started mirroring");
    HdcClient hdc(QCoreApplication::applicationFilePath().toStdString());
    const auto devices = hdc.listDevices();
    check(QMetaObject::invokeMethod(&window, "onDevicesUpdated", Qt::DirectConnection,
                                   Q_ARG(std::vector<Device>, devices)), "device update slot unavailable");
    check(screen->statusMessage().contains(expected), "later polling erased the connection failure reason");
    check(!readFile(QString::fromUtf8(qgetenv("HC_CONNECTION_TEST_LOG"))).contains("tconn 192.0.2.1:8710 -remove"),
          "connecting a target tried to disconnect unrelated cached Offline devices");
    const QString artifacts = QString::fromUtf8(qgetenv("HC_CONNECTION_TEST_ARTIFACTS"));
    if (!artifacts.isEmpty()) {
        window.show();
        processFor(50);
        check(window.grab().save(artifacts + "/" + QString::fromUtf8(mode) + ".png"), "failure screenshot could not be saved");
    }
}

int main(int argc, char** argv) {
    // Stand in for HDC without contacting or changing a real device.
    if (argc > 1) {
        QFile log(QString::fromUtf8(qgetenv("HC_CONNECTION_TEST_LOG")));
        if (log.open(QIODevice::WriteOnly | QIODevice::Append)) {
            for (int i = 1; i < argc; ++i) { log.write(argv[i]); log.write(" "); }
            log.write("\n");
        }
        const QByteArray mode = qgetenv("HC_CONNECTION_TEST_MODE");
        if (argc > 2 && std::string(argv[1]) == "list" && std::string(argv[2]) == "targets") {
            if (mode == "query-failure") std::cout << "[Fail]Failed to communicate with daemon.\n";
            else if (mode == "query" && argc > 3 && std::string(argv[3]) == "-v") std::cout << verboseTargets;
            else if (mode == "unauthorized") std::cout << "192.0.2.1:8710 TCP Offline old-target\n192.0.2.55:9000 TCP Unauthorized HR36\n";
            else if (mode == "denied") std::cout << "192.0.2.55:9000 TCP Unauthorized HR36\n";
            else if (mode == "online") std::cout << "192.0.2.55:9000 TCP Connected HR36\n";
            else if (mode == "offline") std::cout << "192.0.2.55:9000 TCP Offline HR36\n";
            else std::cout << "[Empty]\n";
        } else if (std::string(argv[1]) == "tconn") {
            if (mode == "unauthorized") std::cout << "[E000002] The device unauthorized.\n";
            else std::cerr << "connect failed status:-4078\n";
        }
        return 0;
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    const QString savedPath = NetworkDeviceDialog::savedDevicesFilePath();
    const bool savedExisted = QFile::exists(savedPath);
    const QByteArray savedContents = readFile(savedPath);
    QFile savedFile(savedPath);
    check(savedFile.open(QIODevice::WriteOnly | QIODevice::Truncate), "test device storage unavailable");
    savedFile.write("[]\n");
    savedFile.close();
    const QString commandLog = temporary.filePath("hdc-commands.log");
    qputenv("HC_CONNECTION_TEST_LOG", commandLog.toUtf8());
    int result = 0;
    try {
        testDeviceStates();
        testDeviceQuery();
        testOfflineDoesNotReconnect(commandLog);
        testWirelessDebounce();
        testOnlineSelectionAndPort();
        testConnectionFailure("connect-failure", "connect failed status:-4078");
        testConnectionFailure("unauthorized", "E000002");
        std::cout << "Connection regression tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (savedExisted) {
        check(savedFile.open(QIODevice::WriteOnly | QIODevice::Truncate), "test storage restore failed");
        savedFile.write(savedContents);
        savedFile.close();
    } else {
        QFile::remove(savedPath);
    }
    return result;
}
