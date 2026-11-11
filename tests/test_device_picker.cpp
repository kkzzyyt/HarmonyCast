#include "harmonycast/DeviceCatalog.h"
#include "harmonycast/DeviceSelector.h"
#include "harmonycast/IconManager.h"
#include "harmonycast/MainWindow.h"
#include "harmonycast/NetworkDeviceDialog.h"

#include <QApplication>
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QFile>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QSettings>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <iostream>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace harmonycast;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

static void press(QWidget* widget, int key) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

static int visibleRows(QTableWidget* table) {
    int visible = 0;
    for (int row = 0; row < table->rowCount(); ++row) if (!table->isRowHidden(row)) ++visible;
    return visible;
}

static QTableWidgetItem* checkedItem(QTableWidget* table, int row, int column) {
    auto* item = table->item(row, column);
    if (!item) throw std::runtime_error("hub table item missing at row " + std::to_string(row) +
                                      ", column " + std::to_string(column));
    return item;
}

#ifdef Q_OS_WIN
static void printCrashAddress(const char* label, const void* address) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module) &&
        GetModuleFileNameW(module, path, MAX_PATH)) {
        const auto* slash = std::wcsrchr(path, L'\\');
        std::fprintf(stderr, "%s %p %ls+0x%llx\n", label, address, slash ? slash + 1 : path,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
    } else std::fprintf(stderr, "%s %p (module unavailable)\n", label, address);
}

static LONG WINAPI reportWindowsCrash(EXCEPTION_POINTERS* exception) {
    const auto* record = exception ? exception->ExceptionRecord : nullptr;
    if (record) {
        std::fprintf(stderr, "[CRASH] Windows exception 0x%08lx\n", record->ExceptionCode);
        printCrashAddress("[CRASH] instruction", record->ExceptionAddress);
        for (DWORD i = 0; i < record->NumberParameters; ++i)
            std::fprintf(stderr, "[CRASH] parameter[%lu]=0x%llx\n", i,
                         static_cast<unsigned long long>(record->ExceptionInformation[i]));
    }
    void* stack[32]{};
    const auto count = CaptureStackBackTrace(0, 32, stack, nullptr);
    for (USHORT i = 0; i < count; ++i) printCrashAddress("[CRASH] stack", stack[i]);
    std::fflush(stderr);
    std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static const std::vector<Device> liveDevices = HdcClient::parseDeviceList(
    "192.0.2.55:9000 TCP Connected HR36\n"
    "192.0.2.55:8710 TCP Unauthorized HR36\n"
    "192.0.2.56:8710 TCP Offline HR36\n"
    "aa:bb:cc:dd:ee:01 USB Connected Board\n");

static void seedDevices() {
    QJsonArray array{
        QJsonObject{{"ip", "192.0.2.55"}, {"name", "诊室终端"}, {"port", 9000}},
        QJsonObject{{"ip", "192.0.2.55"}, {"name", "旧调试端口"}, {"port", 8710}},
        QJsonObject{{"ip", "192.0.2.77"}, {"name", "备用设备"}, {"port", 8710}}
    };
    QFile file(NetworkDeviceDialog::savedDevicesFilePath());
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "test storage unavailable");
    file.write(QJsonDocument(array).toJson());
}

static void testMergeAndPersistence() {
    auto entries = DeviceCatalog::merge(liveDevices, NetworkDeviceDialog::loadSavedDevices());
    check(entries.size() == 5, "catalog omitted a port, saved record or USB target");
    check(entries[0].device.serial == "192.0.2.55:9000" && entries[0].name == "诊室终端" && entries[0].saved,
          "catalog lost saved alias or live endpoint");
    check(entries[1].device.serial == "192.0.2.55:8710" && entries[1].device.state == DeviceState::Unauthorized,
          "two TCP ports shared a connection state");
    check(!entries[3].device.is_network && entries[3].device.serial == "aa:bb:cc:dd:ee:01", "USB serial was treated as TCP");
    check(entries[4].device.state == DeviceState::Disconnected, "saved target was reported online");
    check(DeviceCatalog::matchesSearchText(entries[0].searchText(), "9000 诊室 已连接"), "shared search does not match endpoint, alias and state");
    check(!DeviceCatalog::matchesSearchText(entries[0].searchText(), "9000 未授权"), "shared search accepts nonmatching states");
    NetworkDeviceDialog::saveDevice("192.0.2.55:9000");
    const auto saved = NetworkDeviceDialog::loadSavedDevices();
    check(saved.size() == 3, "saving one port removed another port");
    check(saved.front().name == "诊室终端", "automatic saves clear a stored name");
    NetworkDeviceDialog::deleteSavedDevice("192.0.2.55:8710");
    const auto remaining = NetworkDeviceDialog::loadSavedDevices();
    check(remaining.size() == 2 && remaining.front().port == 9000, "deleting one endpoint erased another port");
    seedDevices();
}

static void testSelector() {
    std::vector<SavedNetworkDevice> saved;
    for (int i = 0; i < 500; ++i) {
        SavedNetworkDevice device;
        device.ip = "198.51." + std::to_string(i / 250) + "." + std::to_string(i % 250 + 1);
        device.name = "Room " + std::to_string(i);
        saved.push_back(device);
    }
    const auto entries = DeviceCatalog::merge({}, saved);
    DeviceSelector selector;
    selector.setStyleSheet(IconManager::getAppStyleSheet());
    selector.setPalette(IconManager::getAppPalette());
    selector.resize(260, 32);
    selector.setEntries(entries, QString::fromStdString(entries[10].device.serial));
    selector.show();
    QCoreApplication::processEvents();
    const QString original = selector.currentData().toString();
    int activations = 0, selections = 0;
    QObject::connect(&selector, QOverload<int>::of(&QComboBox::activated), &selector, [&](int) { ++activations; });
    QObject::connect(&selector, QOverload<int>::of(&QComboBox::currentIndexChanged), &selector, [&](int) { ++selections; });
    selector.showPopup();
    QCoreApplication::processEvents();
    auto* popup = selector.findChild<QFrame*>("DeviceSelectorPopup");
    auto* search = selector.findChild<QLineEdit*>("DeviceSelectorSearch");
    auto* results = selector.findChild<QListView*>("DeviceSelectorResults");
    check(popup && search && results, "selector search popup missing");
    const int height = popup->height();
    check(height <= DeviceSelector::PopupHeight && height >= 240 && results->model()->rowCount() == 500,
          "large list expands the popup to the screen height");
    search->setText("rOoM 349");
    check(results->model()->rowCount() == 1, "case-insensitive multi-part device search failed");
    check(selector.currentData().toString() == original && activations == 0 && selections == 0,
          "typing a filter changes the target or activates a connection");
    check(popup->height() == height, "filtering resizes the popup");
    press(search, Qt::Key_Return);
    check(activations == 1 && selections == 1 && !popup->isVisible() &&
          selector.currentData().toString() == QString::fromStdString(entries[349].device.serial), "Enter activates the wrong target or activates more than once");
    selector.showPopup();
    search->setText("not a device");
    press(search, Qt::Key_Return);
    check(activations == 1 && popup->isVisible(), "empty search activates a target");
    press(search, Qt::Key_Escape);
    check(!popup->isVisible() && activations == 1, "Escape changes selection or leaves popup open");
    selector.setEntries(entries);
    check(activations == 1 && selections == 1 && selector.currentData().toString() == QString::fromStdString(entries[349].device.serial),
          "polling resets selection or causes a connection");
    selector.showPopup();
    search->clear();
    const int row = results->currentIndex().row();
    press(search, Qt::Key_Down);
    check(results->currentIndex().row() == row + 1 && activations == 1, "arrow navigation selects prematurely");
    press(search, Qt::Key_Escape);
    const QString artifacts = QString::fromUtf8(qgetenv("HC_DEVICE_PICKER_ARTIFACTS"));
    if (!artifacts.isEmpty()) {
        selector.showPopup();
        search->setText("Room 3");
        QCoreApplication::processEvents();
        check(popup->grab().save(artifacts + "/device-picker.png"), "selector preview unavailable");
        selector.hidePopup();
    }
}

static void testSelectorLifetime() {
    const auto entries = DeviceCatalog::merge(liveDevices, {});
    for (int iteration = 0; iteration < 40; ++iteration) {
        auto selector = std::make_unique<DeviceSelector>();
        selector->setEntries(entries);
        selector->setEntries({});
        selector->setEntries(entries, "192.0.2.55:9000");
        auto* search = selector->findChild<QLineEdit*>("DeviceSelectorSearch");
        auto* results = selector->findChild<QListView*>("DeviceSelectorResults");
        check(search && results, "lifetime fixture is missing popup children");
        search->setText("USB");
        check(results->model()->rowCount() == 1, "repeated catalog refresh corrupts the filter");

        // QComboBox owns its default model and can replace it while the search
        // proxy still references it. Both that reset and final teardown must
        // avoid callbacks into an invalid model or destroyed popup children.
        auto* replacement = new QStandardItemModel(selector.get());
        replacement->appendRow(new QStandardItem("replacement-device"));
        selector->setModel(replacement);
        selector->setEntries(entries);
        if (iteration % 10 == 0) {
            selector->show();
            selector->showPopup();
            QCoreApplication::processEvents();
            search->setText("9000");
            check(results->model()->rowCount() == 1, "replacement model does not reconnect to popup filtering");
            // Deliberately destroy the selector with the popup still open.
        }
        selector.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

static void testHubAndMainConsistency() {
    std::cout << "[RUN] Hub construct" << std::endl;
    NetworkDeviceDialog hub;
    std::cout << "[PASS] Hub construct" << std::endl;
    std::cout << "[RUN] Hub style" << std::endl;
    hub.setStyleSheet(IconManager::getAppStyleSheet());
    std::cout << "[PASS] Hub style" << std::endl;
    std::cout << "[RUN] Hub setDevices" << std::endl;
    hub.setDevices(liveDevices, "192.0.2.55:9000");
    std::cout << "[PASS] Hub setDevices" << std::endl;
    std::cout << "[RUN] Hub show" << std::endl;
    hub.show();
    std::cout << "[PASS] Hub show" << std::endl;
    auto* table = hub.findChild<QTableWidget*>("DeviceHubTable");
    auto* search = hub.findChild<QLineEdit*>("DeviceHubSearch");
    check(table && search && table->rowCount() == 5, "hub does not use the shared catalog");
    for (auto* button : hub.findChildren<QPushButton*>()) {
        check(!button->text().contains("重启 HDC") && !button->text().contains("开启 USB"), "removed service buttons remain visible");
    }
    std::cout << "[RUN] Hub filter by IP" << std::endl;
    search->setText("192.0.2.55");
    check(visibleRows(table) == 2, "hub merged distinct ports");
    std::cout << "[PASS] Hub filter by IP" << std::endl;
    std::cout << "[RUN] Hub filter by port, name and state" << std::endl;
    search->setText("9000 诊室 已连接");
    check(visibleRows(table) == 1, "hub alias, port or status filtering differs");
    std::cout << "[PASS] Hub filter by port, name and state" << std::endl;
    std::cout << "[RUN] Hub filter USB" << std::endl;
    search->setText("USB");
    check(visibleRows(table) == 1 && checkedItem(table, 3, 1)->text().isEmpty(), "hub USB identity is incorrect");
    std::cout << "[PASS] Hub filter USB" << std::endl;
    std::cout << "[RUN] Hub empty filter and live refresh" << std::endl;
    search->setText("不存在");
    check(visibleRows(table) == 0, "hub shows unmatched rows");
    hub.setDevices(liveDevices, "192.0.2.55:9000");
    check(search->text() == "不存在" && visibleRows(table) == 0, "live updates discard the search");
    search->setText("9000 已连接");
    auto offline = liveDevices;
    offline.front().state = DeviceState::Offline;
    hub.setDevices(offline, "192.0.2.55:9000");
    check(visibleRows(table) == 0, "live state changes do not update a filtered hub");
    hub.setDevices(liveDevices, "192.0.2.55:9000");
    check(visibleRows(table) == 1, "live state recovery does not update a filtered hub");
    search->clear();
    std::cout << "[PASS] Hub empty filter and live refresh" << std::endl;

    std::cout << "[RUN] MainWindow construct" << std::endl;
    MainWindow main(nullptr, false, QCoreApplication::applicationFilePath().toStdString());
    std::cout << "[PASS] MainWindow construct" << std::endl;
    std::cout << "[RUN] MainWindow device update and list consistency" << std::endl;
    auto devices = liveDevices;
    check(QMetaObject::invokeMethod(&main, "onDevicesUpdated", Qt::DirectConnection,
                                   Q_ARG(std::vector<Device>, devices)), "main device update unavailable");
    auto* selector = main.findChild<DeviceSelector*>();
    check(selector && selector->count() == table->rowCount(), "main selector is not integrated with the shared catalog");
    for (int row = 0; row < table->rowCount(); ++row) {
        check(selector->findData(checkedItem(table, row, 0)->data(Qt::UserRole)) >= 0, "hub and main list different endpoints");
    }
    std::cout << "[PASS] MainWindow device update and list consistency" << std::endl;
    const QString artifacts = QString::fromUtf8(qgetenv("HC_DEVICE_PICKER_ARTIFACTS"));
    if (!artifacts.isEmpty()) {
        QCoreApplication::processEvents();
        check(hub.grab().save(artifacts + "/device-center.png"), "hub preview unavailable");
    }

    int changes = 0;
    QObject::connect(&hub, &NetworkDeviceDialog::devicesChanged, &main, [&] {
        ++changes;
        auto current = liveDevices;
        QMetaObject::invokeMethod(&main, "onDevicesUpdated", Qt::DirectConnection,
                                  Q_ARG(std::vector<Device>, current));
    });
    auto* input = hub.findChild<QLineEdit*>("DeviceHubAddress");
    auto* port = hub.findChild<QLineEdit*>("DeviceHubPort");
    check(input && port, "hub connection inputs missing");
    QString selectedTarget;
    hub.setOnConnect([&](const std::string& endpoint) { selectedTarget = QString::fromStdString(endpoint); });
    std::cout << "[RUN] Hub save and synchronize target" << std::endl;
    input->setText("203.0.113.9:9020");
    check(QMetaObject::invokeMethod(&hub, "onConnectAndSave", Qt::DirectConnection), "hub connection slot unavailable");
    check(changes == 1 && selectedTarget == "203.0.113.9:9020" && selector->findData(selectedTarget) >= 0,
          "adding a saved target does not synchronize the main selector");
    std::cout << "[PASS] Hub save and synchronize target" << std::endl;
    std::cout << "[RUN] Hub rebuild action widgets" << std::endl;
    hub.refreshData();
    hub.show();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QPushButton* remove = nullptr;
    for (int row = 0; row < table->rowCount(); ++row) {
        if (checkedItem(table, row, 0)->data(Qt::UserRole).toString() != selectedTarget) continue;
        auto* actions = table->cellWidget(row, 3);
        if (!actions) continue;
        for (auto* button : actions->findChildren<QPushButton*>()) {
            if (button->toolTip() == "删除保存记录 203.0.113.9:9020") remove = button;
        }
    }
    check(remove, "saved endpoint delete action missing");
    check(remove->isEnabled(), "saved endpoint delete action is disabled");
    std::cout << "[PASS] Hub rebuild action widgets" << std::endl;
    std::cout << "[RUN] Hub delete confirmation and synchronization" << std::endl;
    bool clicked = false;
    QObject::connect(remove, &QPushButton::clicked, &hub, [&clicked] { clicked = true; });
    bool confirmed = false;
    QTimer confirmer;
    confirmer.setInterval(10);
    QObject::connect(&confirmer, &QTimer::timeout, &hub, [&confirmed, &confirmer] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* question = qobject_cast<QMessageBox*>(widget);
            // macOS intentionally omits message-box window titles. Match its
            // actual question text instead of the platform-dependent title.
            if (question && question->isVisible() && question->text().contains("203.0.113.9:9020")) {
                if (auto* yes = question->button(QMessageBox::Yes)) {
                    confirmed = true;
                    confirmer.stop();
                    yes->click();
                }
            }
        }
    });
    QTimer confirmationDeadline;
    confirmationDeadline.setSingleShot(true);
    QObject::connect(&confirmationDeadline, &QTimer::timeout, &hub, [&confirmer] {
        confirmer.stop();
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* question = qobject_cast<QMessageBox*>(widget)) question->reject();
        }
    });
    confirmer.start();
    confirmationDeadline.start(2000);
    remove->click();
    confirmer.stop();
    confirmationDeadline.stop();
    check(clicked, "saved endpoint delete button did not emit clicked");
    check(confirmed, "saved target deletion confirmation did not select Yes");
    check(changes == 2, "saved target deletion did not emit a storage change");
    const auto stored = NetworkDeviceDialog::loadSavedDevices();
    check(std::none_of(stored.begin(), stored.end(), [](const SavedNetworkDevice& device) {
        return device.ip == "203.0.113.9" && device.port == 9020;
    }), "saved target deletion left the endpoint in storage");
    check(selector->findData(selectedTarget) < 0, "saved target deletion did not update the main selector");
    std::cout << "[PASS] Hub delete confirmation and synchronization" << std::endl;
    std::cout << "[RUN] Hub and MainWindow teardown" << std::endl;
}

int main(int argc, char** argv) {
    if (argc > 1) { std::cout << "[Empty]\n"; return 0; }
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(reportWindowsCrash);
#endif
    if (!qEnvironmentVariableIsSet("HC_DEVICE_PICKER_NATIVE")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    // This test opens/closes independent top-level fixtures without app.exec().
    app.setQuitOnLastWindowClosed(false);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QSettings("HarmonyCast", "HarmonyCast").setValue("auto_start_mirror", false);
    const QString savedPath = NetworkDeviceDialog::savedDevicesFilePath();
    const bool existed = QFile::exists(savedPath);
    const auto backup = readFile(savedPath);
    int result = 0;
    try {
        seedDevices();
        std::cout << "[RUN] DeviceCatalog merge and persistence" << std::endl;
        testMergeAndPersistence();
        std::cout << "[PASS] DeviceCatalog merge and persistence" << std::endl;
        std::cout << "[RUN] DeviceSelector search and activation" << std::endl;
        testSelector();
        std::cout << "[PASS] DeviceSelector search and activation" << std::endl;
        std::cout << "[RUN] DeviceSelector model replacement and teardown" << std::endl;
        testSelectorLifetime();
        std::cout << "[PASS] DeviceSelector model replacement and teardown" << std::endl;
        std::cout << "[RUN] Device center and main selector consistency" << std::endl;
        testHubAndMainConsistency();
        std::cout << "[PASS] Device center and main selector consistency" << std::endl;
        std::cout << "Device picker tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (existed) {
        QFile file(savedPath);
        check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "test storage restore failed");
        file.write(backup);
    } else QFile::remove(savedPath);
    return result;
}
