#include "harmonycast/InspectionPage.h"
#include "harmonycast/AdbClient.h"
#include "harmonycast/IconManager.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <iostream>
#include <stdexcept>
using namespace harmonycast;
static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> static void waitUntil(Predicate predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 8000) { QCoreApplication::processEvents(); QThread::msleep(5); }
    check(predicate(), "application batch timed out");
}
static QByteArray readFile(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
static std::shared_ptr<HdcClient> client() { return std::make_shared<HdcClient>(QCoreApplication::applicationFilePath().toStdString()); }
static QString fixture(const QString& folder, const QString& name) {
    const auto path = folder + "/" + name; QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write("test application archive") > 0, "fixture unavailable");
    file.close(); return QDir::toNativeSeparators(QFileInfo(path).canonicalFilePath());
}
static QList<QStringList> commands() {
    QList<QStringList> result;
    for (const auto& line : readFile(qEnvironmentVariable("HC_BATCH_APP_LOG")).split('\n')) {
        if (line.isEmpty()) continue;
        QStringList args;
        for (const auto& value : QJsonDocument::fromJson(line).array()) args << value.toString();
        result << args;
    }
    return result;
}
static void resetCommands() { QFile file(qEnvironmentVariable("HC_BATCH_APP_LOG")); check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "log unavailable"); }
static void testUninstall(const QString& folder) {
    InspectionRunner runner(client(), nullptr, 250);
    const auto packages = InspectionReport::cleanupPackages();
    check(!runner.startApplicationUninstall({{"192.0.2.21"}}, {"com.other.app"}), "uninstall accepts unrelated packages");
    check(!runner.startApplicationUninstall({{"192.0.2.21"}}, {}), "empty uninstall accepted");
    resetCommands();
    check(runner.startApplicationUninstall({{"192.0.2.21"}, {"192.0.2.22"}, {"192.0.2.23"}, {"192.0.2.24"}}, packages), "uninstall did not start");
    waitUntil([&] { return !runner.isRunning(); });
    const auto& rows = runner.results();
    check(rows[0].status == InspectionStatus::Succeeded && rows[0].applications[1].status == ApplicationCleanupStatus::NotInstalled, "uninstall did not skip missing package");
    check(rows[1].status == InspectionStatus::Partial && rows[1].applications[1].status == ApplicationCleanupStatus::Succeeded, "uninstall stops after a failed package");
    check(rows[2].status == InspectionStatus::Skipped, "device without matching packages not skipped");
    check(rows[3].applications[0].status == ApplicationCleanupStatus::Failed, "unconfirmed uninstall reported success");
    const auto calls = commands();
    check(calls.contains({"-t", "192.0.2.21:8710", "uninstall", packages[0]}), "uninstall command incorrect");
    check(!calls.contains({"-t", "192.0.2.21:8710", "uninstall", packages[1]}), "absent package uninstalled");
    int previous = -1;
    for (const auto& row : rows) {
        const auto connected = calls.indexOf({"tconn", row.target.endpoint()});
        const auto disconnected = calls.indexOf({"tconn", row.target.endpoint(), "-remove"});
        check(connected > previous && disconnected > connected, "targets overlap during uninstall"); previous = disconnected;
    }
    check(InspectionReport::writeCsv(folder + "/uninstall.csv", rows) && readFile(folder + "/uninstall.csv").contains("已卸载"), "uninstall CSV wrong");
}
static void testInstall(const QString& folder) {
    const auto first = fixture(folder, "床旁 安装包.hap"), second = fixture(folder, "room.app");
    InspectionRunner runner(client(), nullptr, 300, 250);
    QString error;
    check(InspectionReport::installationFiles({first, first}, DevicePlatform::OpenHarmony, &error).size() == 1, "duplicate install files not removed");
    check(!runner.startApplicationInstall({{"192.0.2.21"}}, {}), "empty installation accepted");
    check(!runner.startApplicationInstall({{"192.0.2.21"}}, {folder + "/missing.hap"}), "missing installation accepted");
    check(!runner.startApplicationInstall({{"192.0.2.21"}}, {fixture(folder, "wrong.apk")}), "APK accepted for Harmony");
    resetCommands();
    check(runner.startApplicationInstall({{"192.0.2.21"}, {"192.0.2.22"}, {"192.0.2.24"}, {"192.0.2.25"}}, {first, second}), "installation did not start");
    waitUntil([&] { return !runner.isRunning(); });
    const auto& rows = runner.results();
    if (rows[0].status != InspectionStatus::Succeeded) {
        std::cerr << rows[0].notes.toStdString() << '\n';
        for (const auto& app : rows[0].applications) std::cerr << app.detail.toStdString() << '\n';
    }
    check(rows[0].status == InspectionStatus::Succeeded, "successful installation missing");
    check(rows[1].status == InspectionStatus::Partial && rows[1].applications[1].status == ApplicationCleanupStatus::Succeeded, "failed install stops remaining files");
    check(rows[2].status == InspectionStatus::Failed, "unconfirmed install reported success");
    check(rows[3].applications[0].status == ApplicationCleanupStatus::Failed && rows[3].applications[0].detail.contains("超时"), "install timeout hidden");
    check(commands().contains({"-t", "192.0.2.21:8710", "install", first}), "installation path was split or command incorrect");
    check(InspectionReport::writeCsv(folder + "/install.csv", rows) && readFile(folder + "/install.csv").contains("床旁 安装包.hap"), "install CSV loses filenames");
    const auto errorName = fixture(folder, "error failed.hap");
    check(runner.startApplicationInstall({{"192.0.2.21"}}, {errorName}), "error filename setup failed");
    waitUntil([&] { return !runner.isRunning(); });
    check(runner.results()[0].status == InspectionStatus::Succeeded, "successful installation path misread as failure");
    auto adb = std::make_shared<AdbClient>(QCoreApplication::applicationFilePath().toStdString());
    InspectionRunner android(adb);
    const auto apk = fixture(folder, "Android 应用.apk");
    check(android.startApplicationInstall({{"192.0.2.21", 5555, DevicePlatform::Android}}, {apk}), "Android install unavailable");
    waitUntil([&] { return !android.isRunning(); });
    check(android.results()[0].status == InspectionStatus::Succeeded && commands().contains({"-s", "192.0.2.21:5555", "install", "-r", apk}), "Android command or success parse incorrect");
    check(!android.startApplicationUninstall({{"192.0.2.21", 5555, DevicePlatform::Android}}, InspectionReport::cleanupPackages()), "Harmony uninstall can target Android");
}
static void testCancellation(const QString& folder) {
    const auto path = fixture(folder, "cancel.hap"); resetCommands();
    InspectionRunner runner(client());
    check(runner.startApplicationInstall({{"192.0.2.25"}, {"192.0.2.21"}}, {path}), "cancellation setup failed");
    waitUntil([&] { for (const auto& args : commands()) if (args.contains("install")) return true; return false; });
    runner.stop(); waitUntil([&] { return !runner.isRunning(); });
    check(runner.results()[0].applications[0].status == ApplicationCleanupStatus::Cancelled && runner.results()[1].status == InspectionStatus::Cancelled, "cancellation leaves pending apps");
    check(!commands().contains({"tconn", "192.0.2.21:8710"}) && commands().contains({"tconn", "192.0.2.25:8710", "-remove"}), "cancel connects next target or skips disconnection");
    resetCommands();
    InspectionRunner before(client());
    QObject::connect(&before, &InspectionRunner::targetChanged, &before, [&](int i) {
        if (before.results()[i].applications[0].status == ApplicationCleanupStatus::Running) before.stop();
    });
    check(before.startApplicationUninstall({{"192.0.2.21"}}, {InspectionReport::cleanupPackages()[0]}), "precommand cancellation setup failed");
    waitUntil([&] { return !before.isRunning(); });
    for (const auto& args : commands()) check(!args.contains("uninstall"), "uninstall ran after cancellation");
}
static void confirm() {
    QTimer::singleShot(0, [] { if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) dialog->button(QMessageBox::Yes)->click(); });
}
static void testPage(const QString& folder) {
    InspectionPage page(client());
    auto* task = page.findChild<QComboBox*>("InspectionTask");
    auto* input = page.findChild<QPlainTextEdit*>("InspectionTargets");
    auto* start = page.findChild<QPushButton*>("InspectionStart");
    auto* table = page.findChild<QTableWidget*>("InspectionResults");
    task->setCurrentIndex(task->findData(int(InspectionTask::InstallApplications)));
    input->setPlainText("192.0.2.21\n192.0.2.22");
    check(!start->isEnabled(), "installation starts without packages");
    const auto path = folder + "/床旁 安装包.hap";
    check(page.setInstallationFiles({path}), "UI cannot select install files");
    check(start->isEnabled() && table->rowCount() == 2 && table->columnCount() == 5, "install preview not synchronized");
    resetCommands();
    QTimer::singleShot(0, [] { if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) dialog->button(QMessageBox::No)->click(); });
    start->click(); check(!page.isRunning() && commands().isEmpty(), "declined installation still ran");
    bool live = false;
    auto* runner = page.findChild<InspectionRunner*>();
    QObject::connect(runner, &InspectionRunner::targetChanged, &page, [&](int index) {
        if (runner->results()[index].applications[0].status == ApplicationCleanupStatus::Running)
            live = table->item(index, 3)->text().contains("安装中");
    });
    confirm(); start->click();
    check(page.isRunning() && !page.findChild<QPushButton*>("InspectionSelectPackages")->isEnabled(), "install input changes mid-run");
    waitUntil([&] { return !page.isRunning(); });
    check(live && table->item(0, 3)->text() == "已安装" && table->item(1, 3)->text().contains("失败"), "install table stale");
    task->setCurrentIndex(task->findData(int(InspectionTask::UninstallApplications)));
    check(table->columnCount() == 7 && page.exportCsv(folder + "/last-install.csv") && readFile(folder + "/last-install.csv").contains("安装包路径"), "switching tasks corrupts old export");
    const auto artifacts = qEnvironmentVariable("HC_BATCH_APP_ARTIFACTS");
    if (!artifacts.isEmpty()) {
        page.resize(1280, 800); page.setStyleSheet(IconManager::getAppStyleSheet()); page.setPalette(IconManager::getAppPalette());
        page.show(); QCoreApplication::processEvents(); check(page.grab().save(artifacts + "/batch-uninstall.png"), "UI screenshot unavailable");
        task->setCurrentIndex(task->findData(int(InspectionTask::InstallApplications)));
        QCoreApplication::processEvents(); check(page.grab().save(artifacts + "/batch-install.png"), "install screenshot unavailable");
    }
}
static int fakeTool(int argc, char** argv) {
    QCoreApplication command(argc, argv);
    const auto args = command.arguments().mid(1);
    QFile log(qEnvironmentVariable("HC_BATCH_APP_LOG"));
    if (log.open(QIODevice::WriteOnly | QIODevice::Append)) log.write(QJsonDocument(QJsonArray::fromStringList(args)).toJson(QJsonDocument::Compact) + '\n');
    log.close();
    const auto endpoint = args.value(1);
    if (args.contains("dump")) {
        if (endpoint.startsWith("192.0.2.23:")) std::cout << "com.yarward.etnbedsideoh.extra\n";
        else if (endpoint.startsWith("192.0.2.21:")) std::cout << "com.yarward.etnbedsideoh\ncom.yarward.etndoorcontrol_a33\n";
        else for (const auto& package : InspectionReport::cleanupPackages()) std::cout << package.toStdString() << '\n';
    } else if (args.contains("install") || args.contains("uninstall")) {
        const bool first = args.last().contains("床旁") || args.last().contains("etnbedsideoh");
        if (endpoint.startsWith("192.0.2.25:")) QThread::msleep(1800);
        else if (endpoint.startsWith("192.0.2.22:") && first) std::cout << "error: operation failed\n";
        else if (endpoint.startsWith("192.0.2.24:")) std::cout << "request sent\n";
        else if (args.first() == "-s") std::cout << "Success\n";
        else if (args.contains("uninstall")) std::cout << "uninstall bundle successfully.\n";
        else std::cout << "[Info]App install path:" << args.last().toStdString() << " msg:install bundle successfully.\nAppMod finish\n";
    } else std::cout << "Connect OK\n";
    return 0;
}
int main(int argc, char** argv) {
    if (argc > 1) return fakeTool(argc, argv);
#if defined(Q_OS_WIN) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    qputenv("QT_QPA_PLATFORM", "windows");
#else
    qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    QTemporaryDir temporary; qputenv("HC_BATCH_APP_LOG", temporary.filePath("commands.jsonl").toUtf8());
    try {
        testUninstall(temporary.path()); testInstall(temporary.path()); testCancellation(temporary.path()); testPage(temporary.path());
        std::cout << "Batch application tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
