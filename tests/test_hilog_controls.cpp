#include "harmonycast/MainWindow.h"
#include "harmonycast/HiLogSession.h"
#include "harmonycast/InspectionPage.h"
#include "harmonycast/NetworkDeviceDialog.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFileInfo>
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

template<class Predicate> static void waitUntil(Predicate predicate, int timeout = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    check(predicate(), "HiLog control did not finish in time");
}

static QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

static void settle() {
    QElapsedTimer timer;
    timer.start();
    waitUntil([&] { return timer.elapsed() >= 150; });
}

static void setDevices(MainWindow& window, const std::string& listing) {
    auto devices = HdcClient::parseDeviceList(listing);
    check(QMetaObject::invokeMethod(&window, "onDevicesUpdated", Qt::DirectConnection,
                                   Q_ARG(std::vector<Device>, devices)), "device update unavailable");
}

static void showLogs(MainWindow& window, bool only = true) {
    check(QMetaObject::invokeMethod(&window, "setWorkspaceLayout", Qt::DirectConnection, Q_ARG(QString, QString(only ? "logs_only" : "horizontal"))), "log layout unavailable");
    check(QMetaObject::invokeMethod(&window, "switchModule", Qt::DirectConnection, Q_ARG(int, 1)), "workspace unavailable");
    for (auto* combo : window.findChildren<QComboBox*>()) {
        if (combo->findText("全部应用") >= 0) combo->setCurrentText("全部应用");
    }
}

static int starts(const QString& serial) {
    return readFile(QString::fromUtf8(qgetenv("HC_HILOG_TEST_LOG"))).count(("hilog " + serial + '\n').toUtf8());
}

static int collectors(MainWindow& window) { return window.findChildren<HiLogSession*>().size(); }

static void testProcessEnvironment(const QString& directory) {
    const QString tools = directory + QStringLiteral("/工具 中文 空格");
    check(QDir().mkpath(tools), "Unicode tool directory unavailable");
#ifdef Q_OS_WIN
    const QString executable = tools + "/hdc.exe";
#else
    const QString executable = tools + "/hdc";
#endif
    QFile placeholder(executable);
    check(placeholder.open(QIODevice::WriteOnly), "HDC path fixture unavailable");
    placeholder.write("not an executable\n");
    placeholder.close();
    HiLogSession unavailable(executable.toStdString(), "设备 USB:01");
    QString startFailure;
    int failedCompletions = 0;
    QObject::connect(&unavailable, &HiLogSession::errorOccurred, &unavailable,
                     [&](const QString& error) { startFailure = error; });
    QObject::connect(&unavailable, &HiLogSession::finished, &unavailable, [&](int code) {
        check(code == -1, "failed launch does not report an unsuccessful completion");
        ++failedCompletions;
    });
    unavailable.start();
    auto* process = unavailable.findChild<QProcess*>();
    check(process, "HiLog process configuration missing");
    check(QDir(process->workingDirectory()).absolutePath() == QDir(tools).absolutePath(),
          "HiLog does not start in the bundled HDC directory");
    const auto environment = process->processEnvironment();
    const QChar separator = QDir::listSeparator();
    check(environment.value("PATH").startsWith(QDir(tools).absolutePath() + separator),
          "HiLog does not place the bundled HDC libraries first in PATH");
    check(process->program() == executable && process->arguments() == QStringList{"-t", "设备 USB:01", "shell", "hilog"},
          "Unicode or spaces in an HDC path or target are interpreted by a host shell");
#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(5, 7, 0)
    auto modifier = process->createProcessArgumentsModifier();
    check(bool(modifier), "HiLog is missing the Windows process creation policy");
    QProcess::CreateProcessArguments arguments{};
    modifier(&arguments);
    check((arguments.flags & 0x08000000) != 0, "HiLog creates a Windows console window");
#endif
    waitUntil([&] { return !unavailable.isActive() && !startFailure.isEmpty() && failedCompletions == 1; });
    check(startFailure.contains(executable), "failed launch loses the HDC path diagnostic");
    unavailable.stop();

    const QString report = directory + "/process-environment.json";
    qputenv("HC_HILOG_PROCESS_PROBE", report.toUtf8());
    HiLogSession session(QCoreApplication::applicationFilePath().toStdString(), "environment-probe");
    bool finished = false;
    bool normalError = false;
    QObject::connect(&session, &HiLogSession::errorOccurred, &session, [&](const QString&) { normalError = true; });
    QObject::connect(&session, &HiLogSession::finished, &session, [&](int code) {
        check(code == 0, "process environment probe exited with a failure");
        finished = true;
    });
    session.start();
    waitUntil([&] { return finished; });
    check(!normalError, "normal HiLog process completion emitted an error");
    const auto actual = QJsonDocument::fromJson(readFile(report)).object();
    const QString bundled = QFileInfo(QCoreApplication::applicationFilePath()).absolutePath();
    check(QDir(actual.value("cwd").toString()).absolutePath() == QDir(bundled).absolutePath(),
          "HiLog child did not receive the configured working directory");
    check(actual.value("path").toString().startsWith(bundled + separator),
          "HiLog child did not receive its bundled dependency search path");
    qunsetenv("HC_HILOG_PROCESS_PROBE");
}

static void testProcessFailureReporting() {
    HiLogSession session(QCoreApplication::applicationFilePath().toStdString(), "process-exit-failure");
    QString error;
    int errors = 0, completions = 0;
    QObject::connect(&session, &HiLogSession::errorOccurred, &session, [&](const QString& value) {
        error = value;
        ++errors;
    });
    QObject::connect(&session, &HiLogSession::finished, &session, [&](int code) {
        check(code == 7, "HiLog completion loses the HDC exit code");
        ++completions;
    });
    session.start();
    waitUntil([&] { return completions == 1; });
    check(errors == 1 && error.contains("permission denied") && error.contains("7") && !session.isActive(),
          "HiLog failure did not preserve stderr or emitted duplicate completion");
    settle();
    check(completions == 1 && errors == 1, "HiLog failure was reported more than once");

    for (const auto* serial : {"process-exit-zero-failure", "process-log-marker"}) {
        HiLogSession probe(QCoreApplication::applicationFilePath().toStdString(), serial);
        QString diagnostic;
        int completion = 99;
        QObject::connect(&probe, &HiLogSession::errorOccurred, &probe, [&](const QString& value) { diagnostic = value; });
        QObject::connect(&probe, &HiLogSession::finished, &probe, [&](int code) { completion = code; });
        probe.start();
        waitUntil([&] { return completion != 99; });
        if (std::string(serial) == "process-exit-zero-failure") {
            check(completion == -1 && diagnostic.contains("unauthorized"), "exit-zero HDC failure was ignored");
        } else {
            check(completion == 0 && diagnostic.isEmpty(), "device log text was mistaken for an HDC failure");
        }
    }
}

static void testWirelessControls() {
    MainWindow window(nullptr, false, QCoreApplication::applicationFilePath().toStdString());
    window.setAttribute(Qt::WA_ShowWithoutActivating);
    window.show();
    auto* toggle = window.findChild<QCheckBox*>("WirelessHiLog");
    check(toggle && !toggle->isChecked(), "wireless HiLog must have an unchecked control by default");
    const QString first = "192.0.2.51:8710", second = "192.0.2.52:9000";
    showLogs(window, false);
    setDevices(window, "192.0.2.51:8710 TCP Connected TEST\n");
    settle();
    check(toggle->isEnabled() && starts(first) == 0 && collectors(window) == 0,
          "opening a wireless log panel starts collection without consent");
    check(!window.isProjecting(), "logs-only layout starts screen mirroring");
    toggle->setChecked(true);
    auto* logs = window.findChild<QPlainTextEdit*>("LogView");
    waitUntil([&] { return starts(first) == 1 && logs->toPlainText().contains(first); });
    check(collectors(window) == 1, "enabling wireless logs did not create one collector");
    const auto captured = logs->toPlainText();
    toggle->setChecked(false);
    check(collectors(window) == 0 && logs->toPlainText() == captured, "disabling collection erases captured logs or leaves collection active");
    showLogs(window);
    check(toggle->isChecked(), "entering the logs-only layout does not enable wireless collection");
    waitUntil([&] { return starts(first) == 2; });
    setDevices(window, "192.0.2.51:8710 TCP Connected TEST\n");
    showLogs(window);
    settle();
    check(starts(first) == 2 && collectors(window) == 1, "refreshing the panel duplicates HiLog processes");

    setDevices(window, "192.0.2.51:8710 TCP Connected TEST\n192.0.2.52:9000 TCP Connected TEST\n");
    for (auto* combo : window.findChildren<QComboBox*>()) {
        const int target = combo->findData(second);
        if (target >= 0) combo->setCurrentIndex(target);
    }
    waitUntil([&] { return starts(second) == 1 && logs->toPlainText().contains(second); });
    check(collectors(window) == 1 && !logs->toPlainText().contains(first), "switching targets mixes logs or leaves two collectors");
    setDevices(window, "192.0.2.52:9000 TCP Offline TEST\n");
    check(collectors(window) == 0 && !toggle->isEnabled(), "disconnection leaves wireless collection active");
    setDevices(window, "192.0.2.52:9000 TCP Connected TEST\n");
    waitUntil([&] { return starts(second) == 2; });
    check(toggle->isChecked() && collectors(window) == 1, "reconnection forgets the current-session choice");

    auto* inspection = window.findChild<InspectionPage*>();
    QMetaObject::invokeMethod(&window, "switchModule", Qt::DirectConnection, Q_ARG(int, 3));
    inspection->findChild<QPlainTextEdit*>("InspectionTargets")->setPlainText("192.0.2.100");
    inspection->findChild<QPushButton*>("InspectionStart")->click();
    check(inspection->isRunning() && collectors(window) == 0 && !toggle->isEnabled(), "batch inspection conflicts with HiLog collection");
    waitUntil([&] { return !inspection->isRunning(); });
    showLogs(window);
    waitUntil([&] { return starts(second) == 3 && collectors(window) == 1; });
    QMetaObject::invokeMethod(&window, "stopMirror", Qt::DirectConnection, Q_ARG(bool, true));
    check(collectors(window) == 1, "stopping mirroring also disables explicitly enabled wireless logs");

    const QString artifacts = QString::fromUtf8(qgetenv("HC_HILOG_TEST_ARTIFACTS"));
    if (!artifacts.isEmpty()) {
        QMetaObject::invokeMethod(&window, "setWorkspaceLayout", Qt::DirectConnection, Q_ARG(QString, QString("horizontal")));
        QCoreApplication::processEvents();
        check(window.grab().save(artifacts + "/wireless-hilog.png"), "wireless HiLog preview unavailable");
    }
    QMetaObject::invokeMethod(&window, "disconnectCurrentDevice", Qt::DirectConnection);
    setDevices(window, "192.0.2.52:9000 TCP Connected TEST\n");
    settle();
    check(collectors(window) == 0 && starts(second) == 3, "a stale device poll restarts HiLog after an explicit disconnect");
}

static void testUsbAndFreshDefault() {
    MainWindow window(nullptr, false, QCoreApplication::applicationFilePath().toStdString());
    window.setAttribute(Qt::WA_ShowWithoutActivating);
    window.show();
    auto* toggle = window.findChild<QCheckBox*>("WirelessHiLog");
    check(toggle && !toggle->isChecked(), "new window inherits wireless collection opt-in");
    const QString usb = "aa:bb:cc:dd:ee:01";
    setDevices(window, "aa:bb:cc:dd:ee:01 USB Connected TEST\n");
    showLogs(window);
    waitUntil([&] { return starts(usb) == 1 && collectors(window) == 1; });
    check(!toggle->isEnabled() && !toggle->isChecked(), "USB serial containing colons is mistaken for a network endpoint");
}

static int fakeHdc(int argc, char** argv) {
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromUtf8(argv[i]);
    if (args.size() >= 4 && args[0] == "-t" && args[2] == "shell" && args[3] == "hilog") {
        if (args[1] == "process-exit-zero-failure") { std::cout << "[Fail]device unauthorized\n"; return 0; }
        if (args[1] == "process-log-marker") { std::cout << "10-23 11:24:45.913 1162 1162 I A06666/TEST: [Fail] application result\n"; return 0; }
        if (args[1] == "process-exit-failure") {
            std::cerr << "Read hilog failed: permission denied\n";
            return 7;
        }
        if (args[1] == "environment-probe") {
            QFile report(QString::fromUtf8(qgetenv("HC_HILOG_PROCESS_PROBE")));
            if (!report.open(QIODevice::WriteOnly)) return 1;
            report.write(QJsonDocument(QJsonObject{{"cwd", QDir::currentPath()},
                                                 {"path", QString::fromUtf8(qgetenv("PATH"))}}).toJson());
            return 0;
        }
        QFile file(QString::fromUtf8(qgetenv("HC_HILOG_TEST_LOG")));
        if (file.open(QIODevice::WriteOnly | QIODevice::Append)) { file.write(("hilog " + args[1] + '\n').toUtf8()); file.close(); }
        std::cout << "10-23 11:24:45.913 1162 1162 I A06666/TEST: " << args[1].toStdString() << std::endl;
        for (int i = 0; i < 100; ++i) QThread::msleep(100);
    } else if (args.value(0) == "list") std::cout << "[Empty]\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return fakeHdc(argc, argv);
    if (!qEnvironmentVariableIsSet("HC_HILOG_TEST_NATIVE")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QSettings("HarmonyCast", "HarmonyCast").setValue("auto_start_mirror", false);
    QSettings("HarmonyCast", "HarmonyCast").setValue("auto_check_update", false);
    qputenv("HC_HILOG_TEST_LOG", temporary.filePath("hilog.log").toUtf8());
    const QString savedPath = NetworkDeviceDialog::savedDevicesFilePath();
    const bool savedExisted = QFile::exists(savedPath);
    const QByteArray savedContents = readFile(savedPath);
    QFile saved(savedPath);
    check(saved.open(QIODevice::WriteOnly | QIODevice::Truncate), "test device storage unavailable");
    saved.write("[]\n"); saved.close();
    int result = 0;
    try {
        testProcessEnvironment(temporary.path());
        testProcessFailureReporting();
        testWirelessControls();
        testUsbAndFreshDefault();
        waitUntil([&] {
            for (auto* process : app.findChildren<QProcess*>()) {
                if (process->state() != QProcess::NotRunning) return false;
            }
            return true;
        });
        std::cout << "HiLog control tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; result = 1;
    }
    if (savedExisted) {
        check(saved.open(QIODevice::WriteOnly | QIODevice::Truncate), "test storage restore failed");
        saved.write(savedContents); saved.close();
    } else QFile::remove(savedPath);
    return result;
}
