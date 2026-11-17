#include "harmonycast/DeviceInspection.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <iostream>
#include <stdexcept>

using namespace harmonycast;

static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> static void waitUntil(Predicate predicate, int timeout = 25000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(2); }
    check(predicate(), "parallel inspection did not finish in time");
}
static std::shared_ptr<HdcClient> client() { return std::make_shared<HdcClient>(QCoreApplication::applicationFilePath().toStdString()); }
static QList<QStringList> calls() {
    QFile log(qEnvironmentVariable("HC_CONCURRENCY_LOG"));
    QList<QStringList> result;
    if (!log.open(QIODevice::ReadOnly)) return result;
    for (const auto& line : log.readAll().split('\n')) {
        if (line.isEmpty()) continue;
        QStringList args;
        for (const auto& value : QJsonDocument::fromJson(line).array()) args << value.toString();
        result << args;
    }
    return result;
}
static void reset(const char* mode) {
    qputenv("HC_CONCURRENCY_MODE", mode);
    QFile log(qEnvironmentVariable("HC_CONCURRENCY_LOG"));
    check(log.open(QIODevice::WriteOnly | QIODevice::Truncate), "fake HDC log unavailable");
    const QDir directory(qEnvironmentVariable("HC_CONCURRENCY_DIR"));
    for (const auto& name : directory.entryList({"attempt_*"}, QDir::Files)) QFile::remove(directory.filePath(name));
}
static std::vector<InspectionTarget> targets(int count) {
    std::vector<InspectionTarget> result;
    for (int index = 0; index < count; ++index) result.push_back({QString("192.0.2.%1").arg(index + 1), 8710});
    return result;
}
static void testParallelLimitAndOrder() {
    reset("normal");
    InspectionRunner runner(client(), nullptr, 5000, 120000, 8, 10);
    int completed = 0;
    QObject::connect(&runner, &InspectionRunner::progressChanged, &runner, [&](int value, int) {
        check(value >= completed, "parallel progress moved backwards"); completed = value;
    });
    auto list = targets(18);
    list[2].bed = "3床"; list[2].department = "神经科";
    check(runner.start(list), "parallel run refused input");
    waitUntil([&] { return !runner.isRunning(); });
    check(completed == 18, "parallel progress does not include every completed target");
    QSet<QString> active;
    int peak = 0;
    for (const auto& args : calls()) {
        if (args.value(0) == "tconn") {
            if (args.contains("-remove")) { check(active.remove(args[1]), "disconnected a target not owned by the batch"); }
            else { check(!active.contains(args[1]), "same endpoint connected twice concurrently"); active.insert(args[1]); peak = std::max(peak, int(active.size())); }
        }
    }
    check(peak >= 3 && peak <= 8 && active.isEmpty(), "inspection is serial, exceeds its limit or leaks connections");
    for (size_t index = 0; index < list.size(); ++index) {
        check(runner.results()[index].target.endpoint() == list[index].endpoint(), "out-of-order worker results shifted target rows");
        check(runner.results()[index].uptimeSeconds.has_value(), "parallel worker lost its collected fields");
    }
    check(runner.results()[2].target.bed == "3床" && runner.results()[2].target.department == "神经科", "parallel collection lost inventory metadata");
}
static void testReadOnlyOnlyAndDuplicates() {
    reset("normal");
    InspectionRunner runner(client(), nullptr, 5000, 120000, 8, 10);
    auto list = targets(3); list.push_back(list.front()); list.back().bed = "共享IP的另一床位";
    check(runner.start(list), "duplicate target run refused"); waitUntil([&] { return !runner.isRunning(); });
    QSet<QString> active;
    for (const auto& args : calls()) if (args.first() == "tconn") {
        if (args.contains("-remove")) active.remove(args[1]);
        else { check(!active.contains(args[1]), "duplicate inventory rows race the same endpoint"); active.insert(args[1]); }
    }
    int duplicateConnections = 0;
    for (const auto& args : calls()) if (args == QStringList{"tconn", "192.0.2.1:8710"}) ++duplicateConnections;
    check(duplicateConnections == 1 && runner.results()[3].target.bed == "共享IP的另一床位" &&
          runner.results()[3].uptimeSeconds == runner.results()[0].uptimeSeconds, "shared terminal was inspected twice or its bed row was lost");
    check(!runner.startApplicationUninstall(list, {InspectionReport::cleanupPackages()[0]}), "application mutation accepts duplicated endpoints");
    reset("normal");
    check(runner.startApplicationCleanup(targets(3), {InspectionReport::cleanupPackages()[0]}), "cleanup setup failed");
    waitUntil([&] { return !runner.isRunning(); });
    int activeCount = 0;
    for (const auto& args : calls()) if (args.first() == "tconn") {
        activeCount += args.contains("-remove") ? -1 : 1;
        check(activeCount >= 0 && activeCount <= 1, "destructive cleanup unexpectedly became concurrent");
    }
    check(activeCount == 0, "cleanup leaves its last target connected");
}
static void testRetryAndStrictHealth() {
    reset("retry");
    InspectionRunner runner(client(), nullptr, 3000, 120000, 1, 10);
    check(runner.start(targets(1)), "retry setup failed"); waitUntil([&] { return !runner.isRunning(); });
    check(runner.results()[0].uptimeSeconds.has_value(), "health failure did not recover on a later connection");
    int attempts = 0, removed = 0, health = 0;
    for (const auto& args : calls()) {
        if (args.first() == "tconn") { if (args.contains("-remove")) ++removed; else ++attempts; }
        if (args.contains("__HC_INSPECTION_READY__")) ++health;
    }
    check(attempts == 3 && removed == 3 && health == 3, "retry count or disconnect-per-attempt is wrong");
    reset("invalid-health");
    check(runner.start(targets(1)), "strict health setup failed"); waitUntil([&] { return !runner.isRunning(); });
    check(runner.results()[0].status == InspectionStatus::Failed && runner.results()[0].notes.contains("3 次"), "empty exit-zero health check counts as connected");
    for (const auto& args : calls()) check(!args.join(' ').contains("HC_INSPECTION:BEGIN"), "collected data after a failed health check");
}
static void testStopAndDestruction() {
    reset("slow");
    InspectionRunner runner(client(), nullptr, 5000, 120000, 3, 10);
    check(runner.start(targets(20)), "parallel cancellation setup failed");
    waitUntil([&] { int reads = 0; for (const auto& args : calls()) if (args.join(' ').contains("HC_INSPECTION:BEGIN")) ++reads; return reads >= 3; });
    runner.stop(); waitUntil([&] { return !runner.isRunning(); });
    QSet<QString> opened, removed;
    for (const auto& args : calls()) if (args.first() == "tconn") { if (args.contains("-remove")) removed.insert(args[1]); else opened.insert(args[1]); }
    check(opened.size() == 3 && removed == opened, "stop opened another target or missed active cleanup");
    for (const auto& row : runner.results()) check(row.status == InspectionStatus::Cancelled, "stop left a pending or active parallel row");

    reset("invalid-health");
    InspectionRunner delayed(client(), nullptr, 3000, 120000, 1, 1000);
    check(delayed.start(targets(1)), "retry cancellation setup failed");
    waitUntil([&] { return delayed.results()[0].activity == "等待重连"; });
    delayed.stop(); waitUntil([&] { return !delayed.isRunning(); });
    QElapsedTimer timer; timer.start(); waitUntil([&] { return timer.elapsed() > 1100; });
    int connections = 0; for (const auto& args : calls()) if (args.first() == "tconn" && !args.contains("-remove")) ++connections;
    check(connections == 1, "retry timer reconnects after stopping the batch");

    reset("normal");
    InspectionRunner beforeRead(client(), nullptr, 3000, 120000, 1, 10);
    QObject::connect(&beforeRead, &InspectionRunner::targetChanged, &beforeRead, [&](int index) {
        if (beforeRead.results()[index].status == InspectionStatus::Collecting) beforeRead.stop();
    });
    check(beforeRead.start(targets(1)), "pre-read cancellation setup failed");
    waitUntil([&] { return !beforeRead.isRunning(); });
    for (const auto& args : calls()) check(!args.join(' ').contains("HC_INSPECTION:BEGIN"), "collection began after a synchronous stop request");

    reset("slow");
    {
        InspectionRunner closing(client(), nullptr, 5000, 120000, 2, 10);
        check(closing.start(targets(4)), "destruction setup failed");
        waitUntil([&] { int reads = 0; for (const auto& args : calls()) if (args.join(' ').contains("HC_INSPECTION:BEGIN")) ++reads; return reads >= 2; });
    }
    opened.clear(); removed.clear();
    for (const auto& args : calls()) if (args.first() == "tconn") { if (args.contains("-remove")) removed.insert(args[1]); else opened.insert(args[1]); }
    check(opened.size() == 2 && removed == opened, "destroying the parent leaks active worker connections");
}
static void testSynchronousStopAndRestart() {
    reset("invalid-health");
    InspectionRunner runner(client(), nullptr, 3000, 120000, 8, 1000);
    auto aliases = targets(1); aliases.push_back(aliases.front()); aliases.back().bed = "alias";
    bool stoppedFromRow = false, restarted = false;
    QObject::connect(&runner, &InspectionRunner::targetChanged, &runner, [&](int index) {
        if (!stoppedFromRow && index == 0 && runner.results()[index].activity == "等待重连") {
            stoppedFromRow = true;
            runner.stop();
        }
    });
    QObject::connect(&runner, &InspectionRunner::finished, &runner, [&](bool stopped) {
        if (stopped && !restarted) {
            restarted = true;
            qputenv("HC_CONCURRENCY_MODE", "normal");
            check(runner.start({{"192.0.2.90", 8710}}), "synchronous restart from finished failed");
        }
    });
    check(runner.start(aliases), "reentrant alias stop setup failed");
    waitUntil([&] { return restarted && !runner.isRunning(); });
    check(stoppedFromRow && runner.results().size() == 1 && runner.results()[0].target.ip == "192.0.2.90" &&
          runner.results()[0].uptimeSeconds.has_value(), "old alias callbacks overwrote the restarted batch");
    int aliasAttempts = 0, newAttempts = 0;
    for (const auto& args : calls()) if (args.first() == "tconn" && !args.contains("-remove")) {
        if (args[1] == "192.0.2.1:8710") ++aliasAttempts;
        if (args[1] == "192.0.2.90:8710") ++newAttempts;
    }
    check(aliasAttempts == 1 && newAttempts == 1, "old retry timer or worker callback reopened a target after restart");

    reset("normal");
    InspectionRunner queued(client(), nullptr, 3000, 120000, 8, 10);
    bool nextStarted = false;
    QObject::connect(&queued, &InspectionRunner::finished, &queued, [&](bool stopped) {
        if (stopped && !nextStarted) {
            nextStarted = true;
            check(queued.start(targets(12)), "immediate queued restart failed");
        }
    });
    check(queued.start(targets(10)), "queued stop setup failed");
    queued.stop();
    waitUntil([&] { return nextStarted && !queued.isRunning(); });
    check(queued.results().size() == 12, "old queued start changed the restarted batch");
    int connections = 0;
    for (const auto& args : calls()) if (args.first() == "tconn" && !args.contains("-remove")) ++connections;
    check(connections == 12, "queued start from the stopped batch initialized a second worker pool");

    reset("normal");
    InspectionRunner onCompleted(client(), nullptr, 3000, 120000, 8, 10);
    bool stoppedOnCompleted = false;
    QObject::connect(&onCompleted, &InspectionRunner::targetChanged, &onCompleted, [&](int index) {
        const auto& row = onCompleted.results()[index];
        if (!stoppedOnCompleted && row.uptimeSeconds && row.activity.isEmpty()) {
            stoppedOnCompleted = true; onCompleted.stop();
        }
    });
    check(onCompleted.start(aliases), "stop during completed alias notification setup failed");
    waitUntil([&] { return !onCompleted.isRunning(); });
    check(stoppedOnCompleted && onCompleted.results()[0].uptimeSeconds && onCompleted.results()[1].uptimeSeconds,
          "synchronous stop during completed fan-out lost an alias result");
}
static int fakeTool(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments().mid(1);
    QFile log(qEnvironmentVariable("HC_CONCURRENCY_LOG"));
    if (log.open(QIODevice::WriteOnly | QIODevice::Append)) log.write(QJsonDocument(QJsonArray::fromStringList(args)).toJson(QJsonDocument::Compact) + '\n');
    log.close();
    const auto mode = qEnvironmentVariable("HC_CONCURRENCY_MODE");
    if (args.first() == "tconn") {
        if (!args.contains("-remove")) {
            QFile attempt(QDir(qEnvironmentVariable("HC_CONCURRENCY_DIR")).filePath("attempt_" + args.value(1).replace(':', '_')));
            if (attempt.open(QIODevice::WriteOnly | QIODevice::Append)) attempt.write("x");
        }
        std::cout << "Connect OK\n";
    } else if (args.contains("__HC_INSPECTION_READY__")) {
        QFile attempt(QDir(qEnvironmentVariable("HC_CONCURRENCY_DIR")).filePath("attempt_" + args.value(1).replace(':', '_')));
        const auto count = attempt.size();
        if (mode == "invalid-health" || (mode == "retry" && count < 3)) std::cout << "echo accepted but no device response\n";
        else std::cout << "__HC_INSPECTION_READY__\n";
    } else if (args.contains("dump")) std::cout << "com.yarward.etnbedsideoh\n";
    else if (args.contains("clean")) { QThread::msleep(80); std::cout << "clean bundle data files successfully.\n"; }
    else if (args.join(' ').contains("HC_INSPECTION:BEGIN")) {
        QThread::msleep(mode == "slow" ? 1800 : 120);
        std::cout << "<<<HC_INSPECTION:BEGIN>>>\n<<<HC_INSPECTION:ICU>>>\nmodel: TEST\nVersion: 1.0\n"
                     "<<<HC_INSPECTION:SYSTEM>>>\nOpenHarmony-test\n<<<HC_INSPECTION:UPTIME>>>\n42 0\n"
                     "<<<HC_INSPECTION:MEMORY>>>\nMemTotal: 1024 kB\nMemAvailable: 512 kB\n"
                     "<<<HC_INSPECTION:MAC>>>\neth0 aa:bb:cc:dd:ee:ff\n<<<HC_INSPECTION:LOAD>>>\n0 0 0\n<<<HC_INSPECTION:END>>>\n";
    }
    return 0;
}
int main(int argc, char** argv) {
    if (argc > 1) return fakeTool(argc, argv);
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    qputenv("HC_CONCURRENCY_LOG", temporary.filePath("commands.jsonl").toUtf8());
    qputenv("HC_CONCURRENCY_DIR", temporary.path().toUtf8());
    try {
        std::cout << "parallel limit and metadata\n" << std::flush; testParallelLimitAndOrder();
        std::cout << "read-only isolation and deduplication\n" << std::flush; testReadOnlyOnlyAndDuplicates();
        std::cout << "strict health and retries\n" << std::flush; testRetryAndStrictHealth();
        std::cout << "stop, retries and destruction\n" << std::flush; testStopAndDestruction();
        std::cout << "synchronous stop and immediate restart\n" << std::flush; testSynchronousStopAndRestart();
        std::cout << "Inspection concurrency tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
