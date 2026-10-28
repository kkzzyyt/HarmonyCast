#include "harmonycast/HiLogSession.h"
#include <QStringList>
#include <QCoreApplication>
#include <QTimer>
#include <utility>

namespace harmonycast {

HiLogSession::HiLogSession(std::string hdc_path,
                           std::string serial,
                           std::string bundle,
                           std::string category,
                           QObject* parent)
    : QObject(parent),
      m_hdc_path(std::move(hdc_path)),
      m_serial(std::move(serial)),
      m_bundle(std::move(bundle)),
      m_category(std::move(category)) {
}

HiLogSession::~HiLogSession() {
    stop();
}

void HiLogSession::start() {
    stop();

    m_process = new QProcess(this);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &HiLogSession::onReadyRead);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &HiLogSession::onProcessFinished);

    QStringList args;
    if (!m_serial.empty()) {
        args << "-t" << QString::fromStdString(m_serial);
    }
    args << "shell" << "hilog";

    m_process->start(QString::fromStdString(m_hdc_path), args);
}

void HiLogSession::stop() {
    if (auto* process = std::exchange(m_process, nullptr)) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() == QProcess::NotRunning) {
            delete process;
        } else {
            // Let the event loop finish shutdown instead of blocking the UI for 1.5s.
            process->setParent(QCoreApplication::instance());
            connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), process, &QObject::deleteLater);
            process->terminate();
            QTimer::singleShot(1000, process, [process] {
                if (process->state() != QProcess::NotRunning) process->kill();
                else process->deleteLater();
            });
        }
    }
    m_line_buffer.clear();
}

bool HiLogSession::isRunning() const {
    return m_process && m_process->state() == QProcess::Running;
}

void HiLogSession::onReadyRead() {
    if (!m_process) return;

    m_line_buffer.append(m_process->readAllStandardOutput());
    std::vector<HiLogEntry> batch;

    qsizetype offset = 0;
    qsizetype idx = 0;
    while ((idx = m_line_buffer.indexOf('\n', offset)) >= 0) {
        QByteArray line = m_line_buffer.mid(offset, idx - offset);
        offset = idx + 1;

        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) continue;

        std::string raw_str = line.toStdString();
        HiLogEntry entry = m_parser.parseLine(raw_str, m_bundle);

        if (m_retain_entries) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_entries.size() >= MAX_ENTRIES) {
                m_entries.pop_front();
            }
            m_entries.push_back(entry);
        }

        batch.push_back(std::move(entry));
    }

    m_line_buffer.remove(0, offset);
    if (!batch.empty()) {
        emit entriesReady(batch);
    }
}

void HiLogSession::onProcessFinished(int exitCode, QProcess::ExitStatus) {
    emit finished(exitCode);
}

std::vector<HiLogEntry> HiLogSession::snapshotEntries() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<HiLogEntry>(m_entries.begin(), m_entries.end());
}

void HiLogSession::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

} // namespace harmonycast
