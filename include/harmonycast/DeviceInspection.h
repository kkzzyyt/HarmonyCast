#pragma once

#include "HdcClient.h"
#include <QDateTime>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace harmonycast {

struct InspectionTarget {
    QString ip;
    int port{8710};
    DevicePlatform platform{DevicePlatform::OpenHarmony};
    [[nodiscard]] QString endpoint() const { return ip + ":" + QString::number(port); }
};

struct InspectionImport {
    std::vector<InspectionTarget> targets;
    QStringList errors;
    int duplicates{0};
};

enum class InspectionTask { Inspect, ClearApplicationData };
enum class InspectionStatus { Pending, Connecting, Collecting, Succeeded, Partial, Failed, Cancelled, Cleaning, Skipped };
enum class ApplicationCleanupStatus { Pending, Running, Succeeded, NotInstalled, Failed, Cancelled };

struct ApplicationCleanupResult {
    QString package;
    ApplicationCleanupStatus status{ApplicationCleanupStatus::Pending};
    QString detail;
};

struct InspectionNetwork {
    QString name;
    QStringList ips;
    QString mac;
};

struct InspectionResult {
    InspectionTarget target;
    InspectionStatus status{InspectionStatus::Pending};
    QDateTime inspectedAt;
    QString model;
    QString serial;
    QString systemVersion;
    QString apiVersion;
    QString architecture;
    QString kernel;
    QString cpuLoad;
    QString deviceTime;
    std::optional<qint64> uptimeSeconds;
    std::optional<quint64> memoryTotalKiB;
    std::optional<quint64> memoryAvailableKiB;
    std::optional<quint64> storageTotalKiB;
    std::optional<quint64> storageAvailableKiB;
    std::vector<InspectionNetwork> networks;
    QString notes;
    InspectionTask task{InspectionTask::Inspect};
    std::vector<ApplicationCleanupResult> applications;
};

class InspectionReport {
public:
    static constexpr int MaxTargets = 10000;
    static constexpr int MaxImportBytes = 1024 * 1024;
    static InspectionImport parseTargets(const QString& text, DevicePlatform platform = DevicePlatform::OpenHarmony);
    static QString collectionScript(DevicePlatform platform = DevicePlatform::OpenHarmony);
    static InspectionResult parseSnapshot(const InspectionTarget& target, const QString& output);
    static QString statusText(InspectionStatus status);
    static QString uptime(const InspectionResult& result);
    static QString ips(const InspectionResult& result);
    static QString macs(const InspectionResult& result);
    static QStringList cleanupPackages();
    static QString applicationCleanupText(const ApplicationCleanupResult& result);
    static QStringList headers(InspectionTask task = InspectionTask::Inspect);
    static QStringList cells(const InspectionResult& result);
    static bool writeCsv(const QString& path, const std::vector<InspectionResult>& results, QString* error = nullptr);
};

// All HDC processes run asynchronously on the owning Qt event loop. Only the
// current target is removed, including after failures, timeouts and cancellation.
class InspectionRunner : public QObject {
    Q_OBJECT
public:
    explicit InspectionRunner(std::shared_ptr<DeviceClient> hdc, QObject* parent = nullptr, int commandTimeoutMs = 15000);
    void setClient(std::shared_ptr<DeviceClient> client) { if (!m_running) m_hdc = std::move(client); }
    ~InspectionRunner() override;
    bool start(const std::vector<InspectionTarget>& targets);
    bool startApplicationCleanup(const std::vector<InspectionTarget>& targets, const QStringList& packages);
    void stop();
    [[nodiscard]] bool isRunning() const { return m_running; }
    [[nodiscard]] const std::vector<InspectionResult>& results() const { return m_results; }

signals:
    void runningChanged(bool running);
    void targetChanged(int index);
    void progressChanged(int completed, int total);
    void activityChanged(const QString& activity);
    void finished(bool stopped);

private:
    bool startBatch(const std::vector<InspectionTarget>& targets, InspectionTask task, const QStringList& packages);
    void nextTarget();
    void collectCurrent();
    void queryCleanupApplications();
    void cleanNextApplication();
    void finishApplicationCleanup();
    void disconnectCurrent();
    void finishBatch();
    void runCommand(const std::vector<std::string>& args, const QString& serial, int timeoutMs,
                    std::function<void(CommandResult)> callback);
    void completeCommand(QProcess* process, int exitCode, const QString& error = {});
    void readOutput(QProcess* process);

    std::shared_ptr<DeviceClient> m_hdc;
    std::string m_hdc_path;
    QProcess* m_process{nullptr};
    std::function<void(CommandResult)> m_callback;
    QByteArray m_output;
    QByteArray m_errors;
    QString m_command_error;
    std::vector<InspectionResult> m_results;
    int m_index{0};
    int m_command_timeout_ms;
    InspectionTask m_task{InspectionTask::Inspect};
    size_t m_application_index{0};
    bool m_running{false};
    bool m_stopping{false};
    bool m_disconnecting{false};
    bool m_connection_attempted{false};
};

} // namespace harmonycast
