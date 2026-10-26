#pragma once

#include <QString>

namespace harmonycast {

class CrashHandler {
public:
    // Install POSIX signal handlers and C++ terminate handler
    static void install();

    // Check if there is an unreviewed crash from a previous execution
    static bool hasCrashFromPreviousRun();

    // Get the path of the latest crash report
    static QString latestCrashReportPath();

    // Mark the previous crash report as reviewed so user is not prompted again
    static void markCrashAsReviewed();

    // Read the contents of the latest crash report
    static QString readLatestCrashReport();
};

} // namespace harmonycast
