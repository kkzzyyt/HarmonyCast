#include "harmonycast/HiLogSession.h"
#include "harmonycast/HdcClient.h"
#include <QStringList>
#include <QCoreApplication>
#include <QPointer>
#include <QRegularExpression>
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
    m_finished_emitted = false;
    HdcClient::configureProcess(*m_process, m_hdc_path);
    auto* process = m_process;
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        if (process == m_process) onReadyRead();
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        if (process == m_process) m_stderr_tail = (m_stderr_tail + process->readAllStandardError()).right(8192);
    });
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int code, QProcess::ExitStatus status) {
        if (process == m_process) onProcessFinished(code, status);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process != m_process || error != QProcess::FailedToStart || m_finished_emitted) return;
        m_finished_emitted = true;
        const QPointer<HiLogSession> alive(this);
        emit errorOccurred(QString("HDC 日志采集启动失败：%1\n程序：%2")
            .arg(process->errorString(), QString::fromStdString(m_hdc_path)));
        // FailedToStart does not emit QProcess::finished(). Keep the session
        // completion signal consistent without reporting it twice.
        if (alive && alive->m_process == process) emit finished(-1);
    });

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
    m_stderr_tail.clear();
    m_stdout_tail.clear();
}

bool HiLogSession::isRunning() const {
    return m_process && m_process->state() == QProcess::Running;
}

bool HiLogSession::isActive() const {
    return m_process && m_process->state() != QProcess::NotRunning;
}

void HiLogSession::onReadyRead() {
    if (!m_process) return;

    const auto output = m_process->readAllStandardOutput();
    m_stdout_tail = (m_stdout_tail + output).right(8192);
    m_line_buffer.append(output);
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

void HiLogSession::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    if (!m_process || m_finished_emitted) return;
    const QPointer<HiLogSession> alive(this);
    auto* process = m_process;
    onReadyRead();
    if (!alive || alive->m_process != process || alive->m_finished_emitted) return;
    m_finished_emitted = true;
    m_stderr_tail = (m_stderr_tail + process->readAllStandardError()).right(8192);
    const bool hdcFailure = QRegularExpression(R"((?m)^\[(?:Fail|E[0-9a-f]+)\])", QRegularExpression::CaseInsensitiveOption)
        .match(QString::fromUtf8(m_stdout_tail)).hasMatch();
    if (status != QProcess::NormalExit || exitCode != 0 || hdcFailure) {
        QString detail = QString::fromUtf8(m_stderr_tail).trimmed();
        if (detail.isEmpty()) detail = QString::fromUtf8(m_stdout_tail).trimmed();
        if (detail.isEmpty() && status == QProcess::CrashExit) detail = process->errorString();
        QString error = QString("HDC 日志采集%1（退出码 %2）")
            .arg(status == QProcess::CrashExit ? "异常退出" : "退出失败").arg(exitCode);
        if (!detail.isEmpty()) error += "：" + detail.left(1500);
        emit errorOccurred(error);
    }
    if (!alive || alive->m_process != process) return;
    emit finished(exitCode == 0 && hdcFailure ? -1 : exitCode);
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
