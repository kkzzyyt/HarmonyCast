#include "harmonycast/DeviceInspection.h"
#include "harmonycast/InspectionPage.h"
#include "harmonycast/MainWindow.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QCheckBox>
#include <QComboBox>
#include <QMessageBox>
#include <QTimer>
#include <iostream>
#include <stdexcept>

using namespace harmonycast;

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

template<class Predicate> static void waitUntil(Predicate predicate, int timeout = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    check(predicate(), "inspection did not finish in time");
}

static const char* snapshot = R"(<<<HC_INSPECTION:BEGIN>>>
<<<HC_INSPECTION:MODEL>>>
HR36
<<<HC_INSPECTION:ICU>>>
model: "YH-H36M01"
<<<HC_INSPECTION:SERIAL>>>
SN-0007
<<<HC_INSPECTION:SYSTEM>>>
OpenHarmony 5.0.1
<<<HC_INSPECTION:API>>>
13
<<<HC_INSPECTION:UPTIME>>>
90061.75 120000.0
<<<HC_INSPECTION:MEMORY>>>
MemTotal:        2097152 kB
MemFree:          131072 kB
MemAvailable:     524288 kB
<<<HC_INSPECTION:STORAGE>>>
Filesystem 1K-blocks Used Available Use% Mounted on
/dev/block/by-name/userdata 8388608 2097152 6291456 25% /data
<<<HC_INSPECTION:IP>>>
1: lo    inet 127.0.0.1/8 scope host lo
2: eth0    inet 192.0.2.1/24 brd 192.0.2.255 scope global eth0
3: wlan0    inet 198.51.100.7/24 scope global wlan0
<<<HC_INSPECTION:MAC>>>
lo 00:00:00:00:00:00
eth0 aa:bb:cc:dd:ee:01
wlan0 aa:bb:cc:dd:ee:02
<<<HC_INSPECTION:ARCH>>>
aarch64
<<<HC_INSPECTION:KERNEL>>>
5.10.184
<<<HC_INSPECTION:LOAD>>>
0.15 0.25 0.35 1/101 2345
<<<HC_INSPECTION:TIME>>>
2026-11-11 12:34:56 +0800
<<<HC_INSPECTION:END>>>
)";

static void testImport() {
    auto list = InspectionReport::parseTargets(QString::fromUtf8(
        "\xef\xbb\xbf# IP list\r\n192.0.2.1\r\n192.0.2.1:8710\n192.0.2.2:9000 # room\n"
        "192.0.2.3, 192.0.2.4;192.0.2.2:8710\n"
        "999.1.2.3\n192.0.2.1:0\n192.0.2.1:65536\n192.0.2.5;$(reboot)\n"));
    check(list.targets.size() == 6, "valid targets or custom ports lost");
    check(list.duplicates == 1 && list.errors.size() == 4, "duplicates or invalid addresses not reported");
    check(list.targets[0].endpoint() == "192.0.2.1:8710", "default port missing");
    check(list.targets[1].endpoint() == "192.0.2.2:9000", "custom port lost");
    check(InspectionReport::parseTargets("192.0.2.1:abc\nlocalhost\n-t\n192.0.2.01").errors.size() == 4,
          "invalid or ambiguous targets accepted");
}

static void testParsing() {
    const InspectionTarget target{"192.0.2.1", 8710};
    auto row = InspectionReport::parseSnapshot(target, QString::fromUtf8(snapshot));
    check(row.status == InspectionStatus::Succeeded, "complete snapshot marked incomplete");
    check(row.uptimeSeconds == 90061 && InspectionReport::uptime(row).contains("1 天 1 小时 1 分 1 秒"),
          "uptime parsed incorrectly");
    check(row.memoryTotalKiB == 2097152 && row.memoryAvailableKiB == 524288,
          "memory must use MemAvailable instead of MemFree");
    check(row.storageTotalKiB == 8388608 && row.storageAvailableKiB == 6291456,
          "storage capacity units lost");
    check(row.networks.size() == 2 && InspectionReport::macs(row).contains("eth0: aa:bb:cc:dd:ee:01") &&
          !InspectionReport::ips(row).contains("127.0.0.1"), "network interfaces mismatched or loopback exported");
    check(row.model == "YH-H36M01" && row.architecture == "aarch64" && row.cpuLoad == "0.15 / 0.25 / 0.35",
          "common device information missing");
    auto noIcu = InspectionReport::parseSnapshot(target, QString::fromUtf8(snapshot).replace("model: \"YH-H36M01\"", ""));
    check(noIcu.model.isEmpty() && noIcu.notes.contains("看门狗硬件型号"), "product model must not replace the watchdog model");
    auto watchdogOnly = InspectionReport::parseSnapshot(target,
        QString::fromUtf8(snapshot).replace("model: \"YH-H36M01\"", "StopWatchDog: 1\nVersion: v1.9"));
    check(watchdogOnly.model.isEmpty(), "watchdog state or firmware must not become the hardware model");
    auto uppercaseModel = InspectionReport::parseSnapshot(target,
        QString::fromUtf8(snapshot).replace("model: \"YH-H36M01\"", "Model: 'YH-H35M01'\nStopWatchDog: 0"));
    check(uppercaseModel.model == "YH-H35M01", "watchdog hardware model casing or quoting was lost");
    auto aliases = InspectionReport::parseSnapshot(target,
        QString::fromUtf8(snapshot).replace("model: \"YH-H36M01\"", "model: fail\nMODEL: \"YH-H36M01\"\nmodel: root-alias"));
    check(aliases.model == "YH-H36M01", "lower-priority ICU aliases overwrote the hardware model");
    QString withoutUnusedFields = QString::fromUtf8(snapshot);
    withoutUnusedFields.replace(
        "<<<HC_INSPECTION:IP>>>\n1: lo    inet 127.0.0.1/8 scope host lo\n"
        "2: eth0    inet 192.0.2.1/24 brd 192.0.2.255 scope global eth0\n"
        "3: wlan0    inet 198.51.100.7/24 scope global wlan0\n", "");
    withoutUnusedFields.replace("6291456 25% /data", "6291456 - /data");
    auto noIpOrStoragePercent = InspectionReport::parseSnapshot(target, withoutUnusedFields);
    check(noIpOrStoragePercent.status == InspectionStatus::Succeeded && noIpOrStoragePercent.notes.isEmpty() &&
          noIpOrStoragePercent.storageTotalKiB == 8388608 && noIpOrStoragePercent.storageAvailableKiB == 6291456,
          "removed IP or storage usage fields must not be required for a complete inspection");
    auto partial = InspectionReport::parseSnapshot(target,
        "<<<HC_INSPECTION:BEGIN>>>\n<<<HC_INSPECTION:UPTIME>>>\n0.05 0\n"
        "<<<HC_INSPECTION:MEMORY>>>\nMemTotal: 1024 kB\nMemFree: 512 kB\n"
        "<<<HC_INSPECTION:END>>>\n");
    check(partial.uptimeSeconds == 0 && !partial.memoryAvailableKiB && partial.status == InspectionStatus::Partial,
          "missing available memory fabricated a usage value");
    check(!partial.notes.isEmpty(), "missing fields not explained");
    auto denied = InspectionReport::parseSnapshot(target, "[Fail]The device unauthorized.");
    check(denied.status == InspectionStatus::Failed && !denied.uptimeSeconds && !denied.memoryTotalKiB,
          "connection errors became zero-valued successful rows");
    auto invalid = InspectionReport::parseSnapshot(target,
        "<<<HC_INSPECTION:BEGIN>>>\n<<<HC_INSPECTION:UPTIME>>>\n-10 0\n"
        "<<<HC_INSPECTION:MEMORY>>>\nMemTotal: 1024 kB\nMemAvailable: 2048 kB\n<<<HC_INSPECTION:END>>>\n");
    check(!invalid.uptimeSeconds && !invalid.memoryAvailableKiB, "invalid counters caused underflow");
    const QString legacy = QString::fromUtf8(snapshot)
        .replace("<<<HC_INSPECTION:IP>>>\n", "<<<HC_INSPECTION:IP>>>\n<<<HC_INSPECTION:IFCONFIG>>>\n")
        .replace("2: eth0    inet 192.0.2.1/24 brd 192.0.2.255 scope global eth0",
                 "eth0      Link encap:Ethernet  HWaddr AA:BB:CC:DD:EE:01\n          inet addr:192.0.2.1  Bcast:192.0.2.255")
        .replace("3: wlan0    inet 198.51.100.7/24 scope global wlan0",
                 "wlan0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>\n        inet 198.51.100.7  netmask 255.255.255.0");
    auto legacyRow = InspectionReport::parseSnapshot(target, legacy);
    check(InspectionReport::ips(legacyRow).contains("eth0: 192.0.2.1") &&
          InspectionReport::ips(legacyRow).contains("wlan0: 198.51.100.7"), "ifconfig formats not supported");
}

static void testExport(const QString& directory) {
    auto row = InspectionReport::parseSnapshot({"192.0.2.1", 8710}, QString::fromUtf8(snapshot));
    const auto headers = InspectionReport::headers();
    const auto values = InspectionReport::cells(row);
    check(headers.size() == 22 && headers.size() == values.size(), "CSV columns do not match headers");
    check(!headers.contains("设备 IP") && !headers.contains("/data 使用率（%）"),
          "removed inspection columns remain in CSV");
    auto field = [&](const QString& header) {
        const int index = headers.indexOf(header);
        check(index >= 0, "CSV field missing");
        return values[index];
    };
    check(field("目标 IP") == "192.0.2.1" && field("HDC 端口") == "8710" &&
          field("设备型号") == "YH-H36M01" && field("序列号") == "SN-0007" &&
          field("系统版本") == "OpenHarmony 5.0.1" && field("CPU 架构") == "aarch64",
          "removing columns shifted target or system values");
    check(field("总内存（MiB）") == "2048.00" && field("已用内存（MiB）") == "1536.00" &&
          field("可用内存（MiB）") == "512.00" && field("内存使用率（%）") == "75.0" &&
          field("/data 总容量（GiB）") == "8.00" && field("/data 可用容量（GiB）") == "6.00" &&
          field("内核版本") == "5.10.184" && field("设备时间") == "2026-11-11 12:34:56 +0800",
          "removing columns shifted capacity or time values");
    row.model = "=HYPERLINK(\"http://example.invalid\",\"open\")";
    row.notes = "quote \" and comma,\nnext line 中文";
    InspectionResult failed;
    failed.target = {"192.0.2.2", 9000};
    failed.status = InspectionStatus::Failed;
    failed.notes = "连接超时";
    QString error;
    const QString path = directory + "/report.csv";
    check(InspectionReport::writeCsv(path, {row, failed}, &error), "CSV export failed");
    auto csv = readFile(path);
    check(csv.startsWith("\xef\xbb\xbf") && csv.contains("\r\n"), "Excel CSV encoding or line endings wrong");
    check(csv.contains("\"'=HYPERLINK(\"\""), "spreadsheet formula injection not escaped");
    check(csv.contains("quote \"\" and comma,\nnext line") && csv.contains("192.0.2.2"),
          "CSV quoting or failed row lost");
    check(!InspectionReport::writeCsv(directory + "/missing/report.csv", {row}, &error) && !error.isEmpty(),
          "export write failures hidden");
    const QString artifacts = QString::fromUtf8(qgetenv("HC_INSPECTION_ARTIFACTS"));
    if (!artifacts.isEmpty()) {
        QFile::remove(artifacts + "/inspection-sample.csv");
        check(QFile::copy(path, artifacts + "/inspection-sample.csv"), "CSV preview unavailable");
    }
}

static void testRunner(const QString& log) {
    auto hdc = std::make_shared<HdcClient>(QCoreApplication::applicationFilePath().toStdString());
    InspectionRunner runner(hdc, nullptr, 1000);
    auto targets = InspectionReport::parseTargets("192.0.2.1\n192.0.2.2:9000\n192.0.2.3\n192.0.2.4\n192.0.2.5\n192.0.2.6\n192.0.2.7\n192.0.2.8").targets;
    check(runner.start(targets), "batch refused valid input");
    check(!runner.start(targets), "concurrent inspection accepted");
    waitUntil([&] { return !runner.isRunning(); });
    const auto& rows = runner.results();
    check(rows.size() == 8 && rows[0].status == InspectionStatus::Succeeded, "successful device not inspected");
    check(rows[1].status == InspectionStatus::Failed && rows[1].notes.contains("unauthorized"), "HDC exit-zero failure ignored");
    check(rows[2].status == InspectionStatus::Failed && rows[2].notes.contains("超时"), "timeout not reported");
    check(rows[3].status == InspectionStatus::Partial && rows[3].notes.contains("断开失败"), "cleanup failure hidden");
    check(rows[4].status == InspectionStatus::Failed && rows[5].status == InspectionStatus::Succeeded,
          "shell failure stopped later targets");
    check(rows[6].status == InspectionStatus::Partial && rows[6].uptimeSeconds == 90061 && rows[6].notes.contains("超时"),
          "partial timeout lost collected values or transport error");
    check(rows[7].status == InspectionStatus::Partial && rows[7].notes.contains("transport closed"),
          "HDC stderr failure with exit code zero was hidden");
    const auto commands = QString::fromUtf8(readFile(log));
    int previousRemove = -1;
    for (const auto& target : targets) {
        const QString ep = target.endpoint();
        int connect = commands.indexOf("tconn " + ep + "\n", previousRemove + 1);
        int remove = commands.indexOf("tconn " + ep + " -remove\n", connect + 1);
        check(connect > previousRemove && remove > connect, "each target must disconnect before the next connection");
        previousRemove = remove;
    }
    check(!commands.contains("kill") && !commands.contains("tconn 198.51.100.99"), "batch touched unrelated connections");

    const auto cancellationStart = readFile(log).size();
    check(runner.start({{"192.0.2.3", 8710}, {"192.0.2.6", 8710}}), "runner could not restart");
    waitUntil([&] { return runner.results()[0].status == InspectionStatus::Collecting; });
    runner.stop();
    waitUntil([&] { return !runner.isRunning(); });
    check(runner.results()[0].status == InspectionStatus::Cancelled && runner.results()[1].status == InspectionStatus::Cancelled,
          "cancellation left pending results");
    auto cancellationLog = readFile(log).mid(cancellationStart);
    check(cancellationLog.contains("tconn 192.0.2.3:8710 -remove") && !cancellationLog.contains("192.0.2.6"),
          "cancellation skipped cleanup or connected a pending target");

    const auto destructionStart = readFile(log).size();
    {
        InspectionRunner closing(hdc, nullptr, 1000);
        check(closing.start({{"192.0.2.3", 8710}}), "destruction setup failed");
        waitUntil([&] { return closing.results()[0].status == InspectionStatus::Collecting; });
    }
    check(readFile(log).mid(destructionStart).contains("tconn 192.0.2.3:8710 -remove"), "closing left target connected");

    InspectionRunner missing(std::make_shared<HdcClient>("/missing/hdc"), nullptr, 300);
    check(missing.start({{"192.0.2.1", 8710}}), "missing executable setup failed");
    waitUntil([&] { return !missing.isRunning(); });
    check(missing.results()[0].status == InspectionStatus::Failed, "missing HDC left batch running");
}

static void testBatchApplicationCleanup(const QString& log, const QString& directory) {
    auto hdc = std::make_shared<HdcClient>(QCoreApplication::applicationFilePath().toStdString());
    InspectionRunner runner(hdc, nullptr, 700);
    const auto packages = InspectionReport::cleanupPackages();
    check(packages == QStringList{"com.yarward.etnbedsideoh", "com.yarward.etnroomoh", "com.yarward.etndoorcontrol_a33"},
          "cleanup package allowlist does not match the requested applications");
    const auto offset = readFile(log).size();
    check(!runner.startApplicationCleanup({{"192.0.2.11", 8710}}, {}), "cleanup accepts no selected applications");
    check(!runner.startApplicationCleanup({{"192.0.2.11", 8710}}, {"com.other.app"}), "cleanup accepts an unrelated application");
    check(runner.startApplicationCleanup({{"192.0.2.11", 8710}, {"192.0.2.12", 9000}, {"192.0.2.13", 8710},
        {"192.0.2.14", 8710}, {"192.0.2.15", 8710}, {"192.0.2.16", 8710}}, packages), "cleanup did not start");
    waitUntil([&] { return !runner.isRunning(); });
    const auto& rows = runner.results();
    check(rows.size() == 6 && rows[0].status == InspectionStatus::Succeeded, "successful cleanup was not reported");
    check(rows[0].applications.size() == 3 && rows[0].applications[0].status == ApplicationCleanupStatus::Succeeded &&
          rows[0].applications[1].status == ApplicationCleanupStatus::NotInstalled, "missing applications are not skipped individually");
    check(rows[1].status == InspectionStatus::Partial && rows[1].applications[0].status == ApplicationCleanupStatus::Failed &&
          rows[1].applications[1].status == ApplicationCleanupStatus::Succeeded, "one application failure stops other applications");
    check(rows[2].status == InspectionStatus::Skipped, "device with no matching application reports cleanup success");
    check(rows[3].status == InspectionStatus::Failed && rows[3].notes.contains("连接失败"), "connection failure was hidden");
    check(rows[4].applications[0].status == ApplicationCleanupStatus::Failed && rows[4].applications[0].detail.contains("超时") &&
          rows[4].applications[1].status == ApplicationCleanupStatus::Succeeded, "timed-out application stops the batch");
    check(rows[5].applications[0].status == ApplicationCleanupStatus::Failed, "exit-zero unconfirmed cleanup reports success");
    const auto commands = QString::fromUtf8(readFile(log).mid(offset));
    int previous = -1;
    for (const auto& row : rows) {
        const auto endpoint = row.target.endpoint();
        const int connected = commands.indexOf("tconn " + endpoint + '\n', previous + 1);
        const int disconnected = commands.indexOf("tconn " + endpoint + " -remove\n", connected + 1);
        check(connected > previous && disconnected > connected, "cleanup opens the next device before disconnecting the current device");
        previous = disconnected;
    }
    check(commands.contains("-t 192.0.2.11:8710 shell bm clean -d -n com.yarward.etnbedsideoh\n") &&
          !commands.contains("-t 192.0.2.11:8710 shell bm clean -d -n com.yarward.etnroomoh\n"),
          "cleanup command differs from the requested command or targets an absent app");
    check(!commands.contains("kill-server") && !commands.contains("-c ") && !commands.contains("com.other"), "cleanup touches unrelated state");
    check(InspectionReport::writeCsv(directory + "/cleanup.csv", rows), "cleanup CSV export failed");
    const auto csv = readFile(directory + "/cleanup.csv");
    check(csv.contains("com.yarward.etndoorcontrol_a33") && csv.contains("未安装") && csv.contains("超时"),
          "cleanup export loses per-application results");

    const auto cancellationOffset = readFile(log).size();
    check(runner.startApplicationCleanup({{"192.0.2.15", 8710}, {"192.0.2.11", 8710}}, packages), "cleanup could not restart");
    waitUntil([&] { return readFile(log).mid(cancellationOffset).contains("bm clean -d -n"); });
    runner.stop(); waitUntil([&] { return !runner.isRunning(); });
    const auto cancelled = readFile(log).mid(cancellationOffset);
    check(cancelled.contains("tconn 192.0.2.15:8710 -remove") && !cancelled.contains("192.0.2.11"),
          "stopping cleanup leaves the current device connected or starts a pending device");
    check(runner.results()[0].status == InspectionStatus::Cancelled && runner.results()[1].status == InspectionStatus::Cancelled,
          "stopped cleanup rows remain pending");

    const auto beforeCommandOffset = readFile(log).size();
    InspectionRunner beforeCommand(hdc, nullptr, 1000);
    QObject::connect(&beforeCommand, &InspectionRunner::targetChanged, &beforeCommand, [&](int index) {
        const auto& applications = beforeCommand.results()[index].applications;
        if (!applications.empty() && applications[0].status == ApplicationCleanupStatus::Running) beforeCommand.stop();
    });
    check(beforeCommand.startApplicationCleanup({{"192.0.2.11", 8710}}, {packages[0]}), "pre-command cancellation setup failed");
    waitUntil([&] { return !beforeCommand.isRunning(); });
    check(!readFile(log).mid(beforeCommandOffset).contains("bm clean"), "cleanup executes a destructive command after cancellation");

    const auto closingOffset = readFile(log).size();
    {
        InspectionRunner closing(hdc, nullptr, 1000);
        check(closing.startApplicationCleanup({{"192.0.2.15", 8710}}, packages), "cleanup close setup failed");
        waitUntil([&] { return readFile(log).mid(closingOffset).contains("bm clean -d -n"); });
    }
    check(readFile(log).mid(closingOffset).contains("tconn 192.0.2.15:8710 -remove"), "closing cleanup leaves its target connected");

    InspectionRunner android(std::make_shared<AdbClient>(QCoreApplication::applicationFilePath().toStdString()));
    check(!android.startApplicationCleanup({{"192.0.2.11", 5555, DevicePlatform::Android}}, packages),
          "HDC-only cleanup can run on Android");
}

static void testCleanupPage(const QString& log, const QString& directory) {
    auto hdc = std::make_shared<HdcClient>(QCoreApplication::applicationFilePath().toStdString());
    InspectionPage page(hdc);
    auto* task = page.findChild<QComboBox*>("InspectionTask");
    auto* input = page.findChild<QPlainTextEdit*>("InspectionTargets");
    auto* start = page.findChild<QPushButton*>("InspectionStart");
    auto* table = page.findChild<QTableWidget*>("InspectionResults");
    check(task && input && start && table, "batch cleanup task selector missing");
    task->setCurrentIndex(task->findData(int(InspectionTask::ClearApplicationData)));
    check(table->columnCount() == 7, "cleanup columns do not follow the selected task immediately");
    const auto boxes = page.findChildren<QCheckBox*>();
    check(boxes.size() == 3 && start->text() == "开始清理", "cleanup application choices missing");
    input->setPlainText("192.0.2.11");
    check(table->rowCount() == 1 && table->item(0, 1)->text() == "待处理", "cleanup target list is not synchronized before starting");
    for (auto* box : boxes) box->setChecked(false);
    check(!start->isEnabled(), "cleanup enabled without an application selection");
    boxes[0]->setChecked(true);
    check(start->isEnabled(), "selected cleanup application cannot run");
    bool confirmed = false;
    QTimer::singleShot(0, &page, [&] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (dialog) { confirmed = dialog->text().contains("com.yarward.etnbedsideoh") && dialog->text().contains("数据"); dialog->button(QMessageBox::No)->click(); }
    });
    const auto offset = readFile(log).size();
    start->click();
    check(confirmed && !page.isRunning() && readFile(log).size() == offset, "declining cleanup still executes commands");
    QTimer::singleShot(0, &page, [&] {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) dialog->button(QMessageBox::Yes)->click();
    });
    start->click();
    check(page.isRunning() && !task->isEnabled() && !boxes[0]->isEnabled() && input->isReadOnly(),
          "running cleanup allows its targets or applications to change");
    waitUntil([&] { return !page.isRunning(); });
    check(table->columnCount() == 7 && table->rowCount() == 1 && table->item(0, 3)->text() == "已清理" &&
          table->item(0, 4)->text() == "未选择", "cleanup UI loses per-application results or shows unselected apps as cleared");
    check(page.exportCsv(directory + "/cleanup-page.csv"), "cleanup UI export unavailable");
    check(!readFile(log).mid(offset).contains("bm clean -d -n com.yarward.etndoorcontrol_a33"), "unchecked application was cleaned");
    const auto artifacts = qEnvironmentVariable("HC_CLEANUP_ARTIFACTS");
    if (!artifacts.isEmpty()) {
        page.resize(1200, 760); page.setStyleSheet(IconManager::getAppStyleSheet()); page.setPalette(IconManager::getAppPalette());
        page.show(); QCoreApplication::processEvents();
        check(page.grab().save(artifacts + "/batch-application-cleanup.png"), "cleanup UI preview unavailable");
    }
    task->setCurrentIndex(task->findData(int(InspectionTask::Inspect)));
    check(table->columnCount() == 10 && page.exportCsv(directory + "/cleanup-after-mode-switch.csv") &&
          readFile(directory + "/cleanup-after-mode-switch.csv").contains("清理结果"),
          "changing the next task corrupts the previous cleanup export");
}

static void testCleanupLiveStatus() {
    InspectionPage page(std::make_shared<HdcClient>(QCoreApplication::applicationFilePath().toStdString()));
    auto* task = page.findChild<QComboBox*>("InspectionTask");
    auto* input = page.findChild<QPlainTextEdit*>("InspectionTargets");
    auto* table = page.findChild<QTableWidget*>("InspectionResults");
    task->setCurrentIndex(task->findData(int(InspectionTask::ClearApplicationData)));
    input->setPlainText("192.0.2.15\n192.0.2.13");
    const auto boxes = page.findChildren<QCheckBox*>();
    boxes[1]->setChecked(false); boxes[2]->setChecked(false);
    bool sawQuery = false, sawDisconnect = false;
    auto* runner = page.findChild<InspectionRunner*>();
    QObject::connect(runner, &InspectionRunner::targetChanged, &page, [&](int index) {
        const auto text = table->item(index, 1)->text();
        sawQuery |= text == "查询应用";
        sawDisconnect |= text == "断开中";
    });
    QTimer::singleShot(0, &page, [&] {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) dialog->button(QMessageBox::Yes)->click();
    });
    page.findChild<QPushButton*>("InspectionStart")->click();
    waitUntil([&] { return table->item(0, 3)->text() == "清理中"; });
    check(page.isRunning() && table->item(0, 1)->text() == "清理中" && table->item(1, 1)->text() == "待处理" &&
        table->item(0, 4)->text() == "未选择", "per-application progress is only shown after the batch finishes");
    const auto artifacts = qEnvironmentVariable("HC_CLEANUP_STATUS_ARTIFACTS");
    if (!artifacts.isEmpty()) {
        page.resize(1200, 760); page.setStyleSheet(IconManager::getAppStyleSheet()); page.setPalette(IconManager::getAppPalette());
        page.show(); QCoreApplication::processEvents();
        check(page.grab().save(artifacts + "/cleanup-live-status.png"), "live cleanup status preview unavailable");
    }
    waitUntil([&] { return !page.isRunning(); });
    check(sawQuery && sawDisconnect && table->item(0, 3)->text().startsWith("失败") &&
        table->item(1, 3)->text().contains("未安装"), "query, disconnect, failure or skipped status was not synchronized");
}

static void testPage(const QString& directory) {
    auto hdc = std::make_shared<HdcClient>(QCoreApplication::applicationFilePath().toStdString());
    InspectionPage page(hdc);
    auto* input = page.findChild<QPlainTextEdit*>("InspectionTargets");
    auto* start = page.findChild<QPushButton*>("InspectionStart");
    auto* exportButton = page.findChild<QPushButton*>("InspectionExport");
    auto* table = page.findChild<QTableWidget*>("InspectionResults");
    check(input && start && exportButton && table, "inspection controls missing");
    check(!start->isEnabled() && !exportButton->isEnabled(), "empty inspection actions enabled");
    input->setPlainText("192.0.2.1\ninvalid");
    check(!start->isEnabled(), "invalid import silently skipped");
    input->setPlainText("192.0.2.1\n192.0.2.6:9000\n192.0.2.1:8710");
    check(start->isEnabled(), "valid import did not enable inspection");
    start->click();
    check(!start->isEnabled() && input->isReadOnly(), "running batch inputs not locked");
    waitUntil([&] { return !page.isRunning(); });
    check(table->rowCount() == 2 && exportButton->isEnabled(), "UI results or export state missing");
    check(table->columnCount() == 10 && table->item(0, 6)->text() == "YH-H36M01", "inspection columns or watchdog model are incorrect");
    check(table->item(0, 3)->text() == "aa:bb:cc:dd:ee:01" && table->item(0, 4)->text() == "1536 / 2048" &&
          table->item(0, 5)->text() == "75.0%" && table->item(0, 7)->text() == "OpenHarmony 5.0.1" &&
          !table->item(0, 8)->text().isEmpty() && table->item(0, 9)->text().isEmpty(),
          "removing columns shifted inspection table values");
    for (int column = 0; column < table->columnCount(); ++column) {
        const QString header = table->horizontalHeaderItem(column)->text();
        check(header != "设备 IP" && header != "/data 使用率", "removed inspection table columns remain visible");
    }
    check(page.exportCsv(directory + "/page.csv"), "UI CSV export failed");
    QString error;
    const auto previous = input->toPlainText();
    check(!page.importFile(directory + "/missing.txt", &error) && !error.isEmpty() && input->toPlainText() == previous,
          "failed import erased existing input");
    const QString utf16Path = directory + "/utf16.txt";
    QFile utf16(utf16Path);
    check(utf16.open(QIODevice::WriteOnly), "UTF-16 fixture unavailable");
    utf16.write(QByteArray::fromHex("fffe3100390032002e0030002e0032002e0031000d000a00"));
    utf16.close();
    check(page.importFile(utf16Path, &error) && input->toPlainText().trimmed() == "192.0.2.1", "Windows UTF-16 TXT unsupported");

    const QString artifactDirectory = QString::fromUtf8(qgetenv("HC_INSPECTION_ARTIFACTS"));
    if (!artifactDirectory.isEmpty()) {
        page.resize(1120, 720);
        page.setStyleSheet(IconManager::getAppStyleSheet());
        page.setPalette(IconManager::getAppPalette());
        page.show();
        QCoreApplication::processEvents();
        check(page.grab().save(artifactDirectory + "/inspection-results.png"), "UI screenshot unavailable");
    }
    QSettings("HarmonyCast", "HarmonyCast").setValue("auto_start_mirror", true);
    MainWindow window(nullptr, false, hdc->path(), "/missing/adb");
    check(QMetaObject::invokeMethod(&window, "switchModule", Qt::DirectConnection, Q_ARG(int, 3)), "inspection navigation missing");
    auto* batch = window.findChild<InspectionPage*>();
    check(batch && !batch->isHidden(), "inspection page not selected");
    batch->findChild<QPlainTextEdit*>("InspectionTargets")->setPlainText("192.0.2.3\n192.0.2.6");
    batch->findChild<QPushButton*>("InspectionStart")->click();
    auto discovered = HdcClient::parseDeviceList("192.0.2.3:8710 TCP Connected HR36\n");
    check(QMetaObject::invokeMethod(&window, "onDevicesUpdated", Qt::DirectConnection,
          Q_ARG(std::vector<Device>, discovered)), "discovery simulation unavailable");
    check(QMetaObject::invokeMethod(&window, "startMirror", Qt::DirectConnection), "mirror guard unavailable");
    check(!window.isProjecting(), "batch inspection triggered screen mirroring");
    for (auto* navigation : window.findChildren<QPushButton*>()) {
        if (navigation->text() == "投屏工作台") check(!navigation->isEnabled(), "debug connection controls conflict with batch");
    }
    batch->findChild<InspectionRunner*>()->stop();
    waitUntil([&] { return !batch->isRunning(); });
    batch->findChild<QPlainTextEdit*>("InspectionTargets")->setPlainText("192.0.2.1\n192.0.2.6:9000");
    batch->findChild<QPushButton*>("InspectionStart")->click();
    waitUntil([&] { return !batch->isRunning(); });
    if (!artifactDirectory.isEmpty()) {
        window.show();
        QCoreApplication::processEvents();
        check(window.grab().save(artifactDirectory + "/inspection-workbench.png"), "workbench screenshot unavailable");
    }
}

static int fakeHdc(int argc, char** argv) {
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromUtf8(argv[i]);
    QFile log(QString::fromUtf8(qgetenv("HC_INSPECTION_LOG")));
    if (log.open(QIODevice::WriteOnly | QIODevice::Append)) {
        // Keep the command log compact; the collection script is fixed and read only.
        const bool applicationCommand = args.contains("bm") && (args.contains("clean") || args.contains("dump"));
        log.write((args.contains("shell") && !applicationCommand ? args.mid(0, 3).join(' ') : args.join(' ')).toUtf8() + '\n');
        log.close();
    }
    if (args.contains("bm") && args.contains("dump") && args.contains("-a")) {
        if (args.value(1).startsWith("192.0.2.13:")) std::cout << "com.other.app\ncom.yarward.etnbedsideoh.extra\n";
        else if (args.value(1).startsWith("192.0.2.11:")) std::cout << "com.yarward.etnbedsideoh\ncom.yarward.etndoorcontrol_a33\n";
        else std::cout << "com.yarward.etnbedsideoh\ncom.yarward.etnroomoh\ncom.yarward.etndoorcontrol_a33\n";
    } else if (args.contains("bm") && args.contains("clean")) {
        if (args.value(1).startsWith("192.0.2.12:") && args.last() == "com.yarward.etnbedsideoh")
            std::cout << "error: clean bundle data files failed\n";
        else if (args.value(1).startsWith("192.0.2.15:") && args.last() == "com.yarward.etnbedsideoh") QThread::msleep(1800);
        else if (args.value(1).startsWith("192.0.2.16:")) std::cout << "request sent\n";
        else std::cout << "clean bundle data files successfully.\n";
    } else if (args.value(0) == "tconn") {
        if (args.contains("-remove")) {
            std::cout << (args[1].startsWith("192.0.2.4:") ? "[Fail]remove failed\n" : "Remove target finish\n");
        } else {
            std::cout << ((args[1].startsWith("192.0.2.2:") || args[1].startsWith("192.0.2.14:")) ? "[Fail]The device unauthorized.\n" : "Connect OK\n");
        }
    } else if (args.contains("shell")) {
        if (args.value(1).startsWith("192.0.2.3:")) {
            QThread::msleep(4500);
        } else if (args.value(1).startsWith("192.0.2.5:")) {
            std::cout << "[Fail]shell transport closed\n";
        } else if (args.value(1).startsWith("192.0.2.7:")) {
            std::cout << snapshot << std::flush;
            QThread::msleep(4500);
        } else if (args.value(1).startsWith("192.0.2.8:")) {
            std::cout << snapshot;
            std::cerr << "[Fail]transport closed\n";
        } else {
            std::cout << snapshot;
        }
    } else if (args.value(0) == "list") {
        std::cout << "[Empty]\n";
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return fakeHdc(argc, argv);
#if defined(Q_OS_WIN) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Match DevicePickerTests: Qt 5 QMessageBox needs a Windows native interface.
    qputenv("QT_QPA_PLATFORM", "windows");
#else
    qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    qputenv("HC_INSPECTION_LOG", temporary.filePath("commands.log").toUtf8());
    try {
        testImport();
        testParsing();
        testExport(temporary.path());
        testRunner(temporary.filePath("commands.log"));
        testBatchApplicationCleanup(temporary.filePath("commands.log"), temporary.path());
        testPage(temporary.path());
        testCleanupPage(temporary.filePath("commands.log"), temporary.path());
        testCleanupLiveStatus();
        std::cout << "Inspection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
