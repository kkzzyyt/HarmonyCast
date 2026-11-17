#pragma once

#include "HdcClient.h"
#include <QDateTime>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QMap>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace harmonycast {

struct InspectionTarget {
    QString ip;
    int port{8710};
    DevicePlatform platform{DevicePlatform::OpenHarmony};
    QString deviceType;
    QString inventoryModel;
    QString department;
    QString bed;
    QString room;
    [[nodiscard]] QString endpoint() const { return ip + ":" + QString::number(port); }
    bool operator==(const InspectionTarget&) const = default;
};

struct InspectionImport {
    std::vector<InspectionTarget> targets;
    QStringList errors;
    int duplicates{0};
};

enum class InspectionTask { Inspect, ClearApplicationData, UninstallApplications, InstallApplications };
enum class InspectionStatus { Pending, Connecting, Collecting, Succeeded, Partial, Failed, Cancelled, Cleaning, Skipped, Installing, Uninstalling };
enum class ApplicationCleanupStatus { Pending, Running, Succeeded, NotInstalled, Failed, Cancelled };

struct ApplicationCleanupResult {
    QString package;
    ApplicationCleanupStatus status{ApplicationCleanupStatus::Pending};
    QString detail;
    QString sourcePath;
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
    QString mcuVersion;
    QString serial;
    QString systemVersion;
    QString cpuLoad;
    std::optional<qint64> uptimeSeconds;
    std::optional<quint64> memoryTotalKiB;
    std::optional<quint64> memoryAvailableKiB;
    std::vector<InspectionNetwork> networks;
    QString notes;
    InspectionTask task{InspectionTask::Inspect};
    std::vector<ApplicationCleanupResult> applications;
    QString activity;
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
    static QString taskName(InspectionTask task);
    static QStringList installationFiles(const QStringList& paths, DevicePlatform platform, QString* error = nullptr);
    static QString applicationCleanupText(const ApplicationCleanupResult& result, InspectionTask task = InspectionTask::ClearApplicationData);
    static QStringList headers(InspectionTask task = InspectionTask::Inspect);
    static QStringList cells(const InspectionResult& result);
    static bool writeCsv(const QString& path, const std::vector<InspectionResult>& results, QString* error = nullptr);
};

// HDC processes run asynchronously on the owning Qt event loop. Read-only
// inspection can overlap targets; application mutations stay serial. Cleanup
// removes only connections attempted by this batch, including after cancellation.
class InspectionRunner : public QObject {
    Q_OBJECT
public:
    explicit InspectionRunner(std::shared_ptr<DeviceClient> hdc, QObject* parent = nullptr, int commandTimeoutMs = 15000,
                              int installationTimeoutMs = 120000, int maxConcurrency = 8, int retryDelayMs = 2000);
    void setClient(std::shared_ptr<DeviceClient> client) { if (!m_running) m_hdc = std::move(client); }
    ~InspectionRunner() override;
    bool start(const std::vector<InspectionTarget>& targets);
    bool startApplicationCleanup(const std::vector<InspectionTarget>& targets, const QStringList& packages);
    bool startApplicationUninstall(const std::vector<InspectionTarget>& targets, const QStringList& packages);
    bool startApplicationInstall(const std::vector<InspectionTarget>& targets, const QStringList& files);
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
    bool startSelectedApplications(const std::vector<InspectionTarget>& targets, InspectionTask task, const QStringList& packages);
    void nextTarget();
    void connectCurrent();
    void verifyConnection();
    void connectionFailed(const CommandResult& result, const QString& stage);
    void startParallelInspection();
    void dispatchParallel();
    void finishParallelInspection();
    void collectCurrent();
    void queryApplications();
    void executeNextApplication();
    void prepareInstallation(const QString& source, std::function<void(QString, QString)> callback);
    void finishApplications();
    void disconnectCurrent();
    void finishBatch();
    void queueAction(std::function<void()> action);
    [[nodiscard]] bool currentBatch(quint64 generation) const { return m_running && m_batch_generation == generation; }
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
    int m_installation_timeout_ms;
    int m_max_concurrency;
    int m_retry_delay_ms;
    QTimer* m_retry_timer{nullptr};
    quint64 m_batch_generation{0};
    quint64 m_retry_generation{0};
    int m_connect_attempt{0};
    bool m_retry_after_disconnect{false};
    struct WorkerSlot { InspectionRunner* runner; int index{-1}; };
    std::vector<WorkerSlot> m_workers;
    std::vector<bool> m_assigned;
    std::vector<std::vector<int>> m_target_groups;
    int m_completed{0};
    bool m_parallel{false};
    std::shared_ptr<QTemporaryDir> m_install_directory;
    QMap<QString, QString> m_prepared_installations;
    InspectionTask m_task{InspectionTask::Inspect};
    size_t m_application_index{0};
    bool m_running{false};
    bool m_stopping{false};
    bool m_disconnecting{false};
    bool m_connection_attempted{false};
};

} // namespace harmonycast
