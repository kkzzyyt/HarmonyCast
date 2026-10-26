#include "harmonycast/Logger.h"
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QThread>
#include <QProcess>
#include <QDesktopServices>
#include <QUrl>
#include <iostream>
#include <mutex>
#include <deque>
#include <atomic>
#include <unistd.h>
#include <sys/stat.h>

#if defined(__APPLE__)
#include <sys/utsname.h>
#endif

namespace harmonycast {

static QString s_log_dir;
static QString s_log_file;
static std::mutex s_log_mutex;
static std::deque<QString> s_recent_logs;
static std::atomic<LogLevel> s_level{LogLevel::Debug};
static std::atomic<bool> s_initialized{false};

// Async-signal-safe 32KB circular trail buffer for crash reports
static constexpr size_t TRAIL_SIZE = 32768;
static char s_trail_buffer[TRAIL_SIZE];
static std::atomic<size_t> s_trail_head{0};
static std::atomic<size_t> s_trail_total{0};

const char* Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default:              return "UNKNOWN";
    }
}

void Logger::setLogLevel(LogLevel level) {
    s_level = level;
}

LogLevel Logger::logLevel() {
    return s_level.load();
}

QString Logger::logDirectory() {
    if (s_log_dir.isEmpty()) {
        QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (base.isEmpty() || !base.contains("HarmonyCast")) {
#if defined(Q_OS_MAC)
            base = QDir::homePath() + "/Library/Application Support/HarmonyCast";
#else
            base = QDir::homePath() + "/.harmonycast";
#endif
        }
        return base + "/logs";
    }
    return s_log_dir;
}

QString Logger::currentLogFilePath() {
    return s_log_file.isEmpty() ? (logDirectory() + "/harmonycast.log") : s_log_file;
}

void Logger::writeToSignalSafeTrail(const char* data, size_t len) {
    if (!data || len == 0) return;
    for (size_t i = 0; i < len; ++i) {
        size_t idx = s_trail_head.fetch_add(1, std::memory_order_relaxed) % TRAIL_SIZE;
        s_trail_buffer[idx] = data[i];
        s_trail_total.fetch_add(1, std::memory_order_relaxed);
    }
}

void Logger::dumpTrailToFd(int fd) {
    if (fd < 0) return;
    size_t total = s_trail_total.load(std::memory_order_relaxed);
    if (total == 0) return;

    if (total < TRAIL_SIZE) {
        ::write(fd, s_trail_buffer, total);
    } else {
        size_t head = s_trail_head.load(std::memory_order_relaxed) % TRAIL_SIZE;
        ::write(fd, s_trail_buffer + head, TRAIL_SIZE - head);
        if (head > 0) {
            ::write(fd, s_trail_buffer, head);
        }
    }
}

void Logger::rotateLogsIfNeeded() {
    QFileInfo fi(s_log_file);
    if (!fi.exists() || fi.size() < 5 * 1024 * 1024) { // 5 MB threshold
        return;
    }

    // Keep up to 3 backup logs: harmonycast.log.1, harmonycast.log.2, harmonycast.log.3
    QString backup3 = s_log_file + ".3";
    QString backup2 = s_log_file + ".2";
    QString backup1 = s_log_file + ".1";

    if (QFile::exists(backup3)) QFile::remove(backup3);
    if (QFile::exists(backup2)) QFile::rename(backup2, backup3);
    if (QFile::exists(backup1)) QFile::rename(backup1, backup2);
    QFile::rename(s_log_file, backup1);
}

void Logger::init(const QString& customLogDir) {
    std::lock_guard<std::mutex> lock(s_log_mutex);
    if (s_initialized) return;

    if (!customLogDir.isEmpty()) {
        s_log_dir = customLogDir;
    } else {
        s_log_dir = logDirectory();
    }

    QDir dir(s_log_dir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    dir.mkpath("crashes");

    s_log_file = s_log_dir + "/harmonycast.log";
    rotateLogsIfNeeded();

    // Hook Qt Message Handler
    qInstallMessageHandler(qtMessageHandler);
    s_initialized = true;

    // Write start banner
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    QString banner = QString(
        "================================================================================\n"
        "HarmonyCast Log Session Started\n"
        "Time: %1\n"
        "PID: %2 | Path: %3\n"
        "Log Dir: %4\n"
        "================================================================================\n"
    ).arg(timestamp).arg(QCoreApplication::applicationPid()).arg(QCoreApplication::applicationFilePath()).arg(s_log_dir);

    QFile file(s_log_file);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        file.write(banner.toUtf8());
        file.close();
    }
    writeToSignalSafeTrail(banner.toUtf8().constData(), banner.toUtf8().size());
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(s_log_mutex);
    if (!s_initialized) return;

    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    QString footer = QString("\n[Session End: %1]\n\n").arg(timestamp);

    QFile file(s_log_file);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        file.write(footer.toUtf8());
        file.close();
    }

    qInstallMessageHandler(nullptr);
    s_initialized = false;
}

void Logger::log(LogLevel level, const char* file, int line, const char* func, const QString& message) {
    if (level < s_level.load()) return;

    QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    const char* lvlStr = levelToString(level);
    quintptr tid = reinterpret_cast<quintptr>(QThread::currentThreadId());

    // Extract file basename
    QString fileName = file ? QString::fromUtf8(file) : "unknown";
    int lastSlash = std::max(fileName.lastIndexOf('/'), fileName.lastIndexOf('\\'));
    if (lastSlash >= 0) fileName = fileName.mid(lastSlash + 1);

    QString formatted = QString("[%1] [0x%2] [%3] [%4:%5] %6\n")
                            .arg(ts)
                            .arg(QString::number(tid, 16).rightJustified(8, '0'))
                            .arg(lvlStr)
                            .arg(fileName)
                            .arg(line)
                            .arg(message);

    QByteArray bytes = formatted.toUtf8();

    // 1. Thread-safe file & memory list write
    {
        std::lock_guard<std::mutex> lock(s_log_mutex);
        if (s_initialized) {
            rotateLogsIfNeeded();
            QFile logFile(s_log_file);
            if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
                logFile.write(bytes);
                logFile.close();
            }
        }

        s_recent_logs.push_back(formatted.trimmed());
        if (s_recent_logs.size() > 200) {
            s_recent_logs.pop_front();
        }
    }

    // 2. Async-signal-safe circular trail write
    writeToSignalSafeTrail(bytes.constData(), bytes.size());

    // 3. Console output in interactive terminal
    if (level >= LogLevel::Warn) {
        std::cerr << bytes.constData();
    } else {
        std::cout << bytes.constData();
    }
}

void Logger::log(LogLevel level, const QString& message) {
    log(level, nullptr, 0, nullptr, message);
}

std::vector<QString> Logger::getRecentLogs(size_t maxCount) {
    std::lock_guard<std::mutex> lock(s_log_mutex);
    size_t count = std::min(maxCount, s_recent_logs.size());
    std::vector<QString> result;
    result.reserve(count);
    auto it = s_recent_logs.end() - count;
    while (it != s_recent_logs.end()) {
        result.push_back(*it++);
    }
    return result;
}

void Logger::openLogDirectory() {
    QString dir = logDirectory();
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

QString Logger::exportDiagnosticBundle(const QString& targetPath) {
    QString logDir = logDirectory();
    QString exportFile = targetPath;

    if (exportFile.isEmpty()) {
        QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
        exportFile = QDir::homePath() + QString("/Downloads/HarmonyCast_Diagnostics_%1.zip").arg(ts);
    }

    // Write system snapshot info before export
    QString sysInfoPath = logDir + "/system_info.txt";
    QFile sysFile(sysInfoPath);
    if (sysFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&sysFile);
        out << "========================================\n";
        out << "HarmonyCast Diagnostic System Info\n";
        out << "========================================\n";
        out << "Date: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
        out << "PID: " << QCoreApplication::applicationPid() << "\n";
        out << "Application: " << QCoreApplication::applicationName() << " " << QCoreApplication::applicationVersion() << "\n";
        out << "Binary: " << QCoreApplication::applicationFilePath() << "\n";
#if defined(__APPLE__)
        struct utsname uts;
        if (uname(&uts) == 0) {
            out << "Kernel: " << uts.sysname << " " << uts.release << " " << uts.version << "\n";
            out << "Machine: " << uts.machine << "\n";
        }
#endif
        sysFile.close();
    }

    // Zip logs directory using system zip if available
    QProcess zip;
    QStringList args;
    args << "-rq" << exportFile << ".";
    zip.setWorkingDirectory(logDir);
    zip.start("/usr/bin/zip", args);

    if (zip.waitForFinished(10000) && zip.exitCode() == 0) {
        return exportFile;
    }

    // Fallback: return folder path if zip tool failed
    return logDir;
}

void Logger::qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    LogLevel level = LogLevel::Info;
    switch (type) {
        case QtDebugMsg:    level = LogLevel::Debug; break;
        case QtInfoMsg:     level = LogLevel::Info;  break;
        case QtWarningMsg:  level = LogLevel::Warn;  break;
        case QtCriticalMsg: level = LogLevel::Error; break;
        case QtFatalMsg:    level = LogLevel::Fatal; break;
    }

    log(level, context.file, context.line, context.function, msg);
}

} // namespace harmonycast
