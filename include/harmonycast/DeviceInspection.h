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
    [[nodiscard]] QString endpoint() const { return ip + ":" + QString::number(port); }
};

struct InspectionImport {
    std::vector<InspectionTarget> targets;
    QStringList errors;
    int duplicates{0};
};

enum class InspectionStatus { Pending, Connecting, Collecting, Succeeded, Partial, Failed, Cancelled };

struct InspectionNetwork {
    QString name;
    QStringList ips;
    QString mac;
};

struct InspectionResult {
    InspectionTarget target;
    InspectionStatus status{InspectionStatus::Pending};
    QDateTime inspectedAt;
    QString model; // Hardware model from the ICU/watchdog sysfs nodes.
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
};

class InspectionReport {
public:
    static constexpr int MaxTargets = 10000;
    static constexpr int MaxImportBytes = 1024 * 1024;
    static InspectionImport parseTargets(const QString& text);
    static QString collectionScript();
    static InspectionResult parseSnapshot(const InspectionTarget& target, const QString& output);
    static QString statusText(InspectionStatus status);
    static QString uptime(const InspectionResult& result);
    static QString ips(const InspectionResult& result);
    static QString macs(const InspectionResult& result);
    static QStringList headers();
    static QStringList cells(const InspectionResult& result);
    static bool writeCsv(const QString& path, const std::vector<InspectionResult>& results, QString* error = nullptr);
};

// All HDC processes run asynchronously on the owning Qt event loop. Only the
// current target is removed, including after failures, timeouts and cancellation.
class InspectionRunner : public QObject {
    Q_OBJECT
public:
    explicit InspectionRunner(std::shared_ptr<HdcClient> hdc, QObject* parent = nullptr, int commandTimeoutMs = 15000);
    ~InspectionRunner() override;
    bool start(const std::vector<InspectionTarget>& targets);
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
    void nextTarget();
    void collectCurrent();
    void disconnectCurrent();
    void finishBatch();
    void runCommand(const std::vector<std::string>& args, const QString& serial, int timeoutMs,
                    std::function<void(CommandResult)> callback);
    void completeCommand(QProcess* process, int exitCode, const QString& error = {});
    void readOutput(QProcess* process);

    std::shared_ptr<HdcClient> m_hdc;
    std::string m_hdc_path;
    QProcess* m_process{nullptr};
    std::function<void(CommandResult)> m_callback;
    QByteArray m_output;
    QByteArray m_errors;
    QString m_command_error;
    std::vector<InspectionResult> m_results;
    int m_index{0};
    int m_command_timeout_ms;
    bool m_running{false};
    bool m_stopping{false};
    bool m_disconnecting{false};
    bool m_connection_attempted{false};
};

} // namespace harmonycast
