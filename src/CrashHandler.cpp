#include "harmonycast/CrashHandler.h"
#include "harmonycast/Logger.h"
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QStandardPaths>
#include <iostream>
#include <exception>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#if defined(__APPLE__) || defined(__linux__)
#include <execinfo.h>
#include <sys/utsname.h>
#endif

namespace harmonycast {

static char g_crash_dir[512] = {0};
static char g_marker_path[512] = {0};
static char g_app_info[256] = "HarmonyCast";

static const char* signalToName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (Segmentation Fault: Invalid memory access)";
        case SIGABRT: return "SIGABRT (Abort: Assertion failure or abort called)";
        case SIGBUS:  return "SIGBUS (Bus Error: Non-existent physical address or unaligned access)";
        case SIGFPE:  return "SIGFPE (Floating Point Exception: Division by zero or overflow)";
        case SIGILL:  return "SIGILL (Illegal Instruction)";
        default:      return "UNKNOWN_SIGNAL";
    }
}

static void writeSafeString(int fd, const char* str) {
    if (fd < 0 || !str) return;
    ::write(fd, str, std::strlen(str));
}

static void writeCrashReport(const char* reason, const char* details, void* fault_addr) {
    if (g_crash_dir[0] == '\0') return;

    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    char filename[512];
    snprintf(filename, sizeof(filename), "%s/crash_%04d%02d%02d_%02d%02d%02d.log",
             g_crash_dir,
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    int fd = ::open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;

    writeSafeString(fd, "================================================================================\n");
    writeSafeString(fd, "HarmonyCast Crash Report\n");
    writeSafeString(fd, "================================================================================\n");

    char time_str[64];
    snprintf(time_str, sizeof(time_str), "Timestamp:    %04d-%02d-%02d %02d:%02d:%02d\n",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    writeSafeString(fd, time_str);

    writeSafeString(fd, "Application:  ");
    writeSafeString(fd, g_app_info);
    writeSafeString(fd, "\n");

#if defined(__APPLE__) || defined(__linux__)
    struct utsname uts;
    if (uname(&uts) == 0) {
        char os_buf[256];
        snprintf(os_buf, sizeof(os_buf), "OS Info:      %s %s (%s, %s)\n",
                 uts.sysname, uts.release, uts.version, uts.machine);
        writeSafeString(fd, os_buf);
    }
#endif

    writeSafeString(fd, "Reason:       ");
    writeSafeString(fd, reason);
    writeSafeString(fd, "\n");

    if (details && details[0] != '\0') {
        writeSafeString(fd, "Details:      ");
        writeSafeString(fd, details);
        writeSafeString(fd, "\n");
    }

    if (fault_addr) {
        char addr_buf[64];
        snprintf(addr_buf, sizeof(addr_buf), "Fault Address: %p\n", fault_addr);
        writeSafeString(fd, addr_buf);
    }

    writeSafeString(fd, "\nThread Callstack (Most recent call first):\n");

#if defined(__APPLE__) || defined(__linux__)
    void* callstack[64];
    int frames = backtrace(callstack, 64);
    backtrace_symbols_fd(callstack, frames, fd);
#else
    writeSafeString(fd, "  [Stack backtrace not available on this platform]\n");
#endif

    writeSafeString(fd, "\n================================================================================\n");
    writeSafeString(fd, "Recent Activity Trail (Last events before crash):\n");
    writeSafeString(fd, "================================================================================\n");

    Logger::dumpTrailToFd(fd);

    writeSafeString(fd, "\n================================================================================\n");
    writeSafeString(fd, "End of Crash Report\n");
    writeSafeString(fd, "================================================================================\n");

    ::close(fd);

    // Create marker file for previous crash alert
    if (g_marker_path[0] != '\0') {
        int mfd = ::open(g_marker_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (mfd >= 0) {
            writeSafeString(mfd, filename);
            ::close(mfd);
        }
    }
}

static void posixSignalHandler(int sig, siginfo_t* info, void* ucontext) {
    (void)ucontext;
    void* fault_addr = (info ? info->si_addr : nullptr);
    writeCrashReport(signalToName(sig), "Fatal OS signal received", fault_addr);

    // Restore default handler and re-raise to produce core dump if enabled
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

static void onTerminate() {
    std::exception_ptr p = std::current_exception();
    char exc_buf[512] = {0};
    if (p) {
        try {
            std::rethrow_exception(p);
        } catch (const std::exception& e) {
            snprintf(exc_buf, sizeof(exc_buf), "std::exception: %s", e.what());
        } catch (...) {
            snprintf(exc_buf, sizeof(exc_buf), "Uncaught non-standard C++ exception");
        }
    } else {
        snprintf(exc_buf, sizeof(exc_buf), "std::terminate() called without active exception");
    }

    writeCrashReport("std::terminate", exc_buf, nullptr);
    std::abort();
}

void CrashHandler::install() {
    QString logDir = Logger::logDirectory();
    QDir dir(logDir);
    dir.mkpath("crashes");

    QString crashDirPath = logDir + "/crashes";
    std::strncpy(g_crash_dir, crashDirPath.toUtf8().constData(), sizeof(g_crash_dir) - 1);

    QString markerPath = crashDirPath + "/.unreviewed_crash";
    std::strncpy(g_marker_path, markerPath.toUtf8().constData(), sizeof(g_marker_path) - 1);

    snprintf(g_app_info, sizeof(g_app_info), "HarmonyCast v2.1.2 (Built %s %s)", __DATE__, __TIME__);

    // Install POSIX signal handlers
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = posixSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);

    // Install C++ terminate handler
    std::set_terminate(onTerminate);
}

bool CrashHandler::hasCrashFromPreviousRun() {
    if (g_marker_path[0] != '\0' && QFile::exists(QString::fromUtf8(g_marker_path))) {
        return true;
    }
    return false;
}

QString CrashHandler::latestCrashReportPath() {
    if (g_crash_dir[0] == '\0') {
        QString defaultPath = Logger::logDirectory() + "/crashes";
        std::strncpy(g_crash_dir, defaultPath.toUtf8().constData(), sizeof(g_crash_dir) - 1);
    }

    QDir dir(QString::fromUtf8(g_crash_dir));
    QStringList filters;
    filters << "crash_*.log";
    QFileInfoList list = dir.entryInfoList(filters, QDir::Files, QDir::Time);
    if (!list.isEmpty()) {
        return list.first().absoluteFilePath();
    }
    return QString();
}

void CrashHandler::markCrashAsReviewed() {
    if (g_marker_path[0] != '\0') {
        QFile::remove(QString::fromUtf8(g_marker_path));
    }
}

QString CrashHandler::readLatestCrashReport() {
    QString path = latestCrashReportPath();
    if (path.isEmpty()) return QString();

    QFile file(path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString::fromUtf8(file.readAll());
    }
    return QString();
}

} // namespace harmonycast
