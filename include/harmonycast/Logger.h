#pragma once

#include "Types.h"
#include <QString>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>

namespace harmonycast {

class Logger {
public:
    static void init(const QString& customLogDir = QString());
    static void shutdown();

    static void log(LogLevel level, const char* file, int line, const char* func, const QString& message);
    static void log(LogLevel level, const QString& message);

    static QString logDirectory();
    static QString currentLogFilePath();
    static void openLogDirectory();

    // Export log bundle (app.log + crashes/ + sysinfo) into a zip or folder
    static QString exportDiagnosticBundle(const QString& targetPath = QString());

    // In-memory recent logs for UI / diagnostic inspection
    static std::vector<QString> getRecentLogs(size_t maxCount = 50);

    // Thread-safe and async-signal-safe circular trail buffer for crash reports
    static void writeToSignalSafeTrail(const char* data, size_t len);
    static void dumpTrailToFd(int fd);

    static void setLogLevel(LogLevel level);
    static LogLevel logLevel();
    static const char* levelToString(LogLevel level);

private:
    static void rotateLogsIfNeeded();
    static void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg);
};

} // namespace harmonycast

#define HC_LOG_DEBUG(msg) harmonycast::Logger::log(harmonycast::LogLevel::Debug, __FILE__, __LINE__, __FUNCTION__, msg)
#define HC_LOG_INFO(msg)  harmonycast::Logger::log(harmonycast::LogLevel::Info,  __FILE__, __LINE__, __FUNCTION__, msg)
#define HC_LOG_WARN(msg)  harmonycast::Logger::log(harmonycast::LogLevel::Warn,  __FILE__, __LINE__, __FUNCTION__, msg)
#define HC_LOG_ERROR(msg) harmonycast::Logger::log(harmonycast::LogLevel::Error, __FILE__, __LINE__, __FUNCTION__, msg)
#define HC_LOG_FATAL(msg) harmonycast::Logger::log(harmonycast::LogLevel::Fatal, __FILE__, __LINE__, __FUNCTION__, msg)
