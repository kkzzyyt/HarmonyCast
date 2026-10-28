#pragma once

#include "Types.h"
#include "HiLogParser.h"
#include <QObject>
#include <QProcess>
#include <vector>
#include <deque>
#include <mutex>
#include <string>

namespace harmonycast {

class HiLogSession : public QObject {
    Q_OBJECT
public:
    explicit HiLogSession(std::string hdc_path,
                          std::string serial,
                          std::string bundle = "",
                          std::string category = "all",
                          QObject* parent = nullptr);
    ~HiLogSession() override;

    void start();
    void stop();
    [[nodiscard]] bool isRunning() const;

    void setBundle(std::string bundle) { m_bundle = std::move(bundle); }
    void setCategory(std::string cat) { m_category = std::move(cat); }
    void setRetentionEnabled(bool enabled) { m_retain_entries = enabled; if (!enabled) clear(); }

    std::vector<HiLogEntry> snapshotEntries() const;
    void clear();

signals:
    void entriesReady(const std::vector<HiLogEntry>& entries);
    void finished(int exit_code);

private slots:
    void onReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    std::string m_hdc_path;
    std::string m_serial;
    std::string m_bundle;
    std::string m_category;

    QProcess* m_process{nullptr};
    HiLogParser m_parser;
    QByteArray m_line_buffer;

    mutable std::mutex m_mutex;
    std::deque<HiLogEntry> m_entries;
    bool m_retain_entries{true};
    static constexpr size_t MAX_ENTRIES = 10000;
};

} // namespace harmonycast
