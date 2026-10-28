#include <QApplication>
#include <QIcon>
#include <QDir>
#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cstring>
#include <windows.h>
#include <versionhelpers.h>
#endif
#include "harmonycast/MainWindow.h"
#include "harmonycast/Logger.h"
#include "harmonycast/CrashHandler.h"

int main(int argc, char* argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

#if defined(Q_OS_WIN)
    bool use_software_rendering = false;
#if defined(HARMONYCAST_SERVER2012_COMPAT)
    use_software_rendering = true;
#endif

    for (int i = 1; i < argc; ++i) {
        if (argv[i] && (strcmp(argv[i], "--software-rendering") == 0 ||
                        strcmp(argv[i], "-software") == 0 ||
                        strcmp(argv[i], "/software") == 0)) {
            use_software_rendering = true;
            break;
        }
    }

    if (qEnvironmentVariableIsSet("HARMONYCAST_SOFTWARE_RENDERING") ||
        qEnvironmentVariableIsSet("QT_OPENGL")) {
        if (qgetenv("QT_OPENGL") == "software" ||
            qgetenv("HARMONYCAST_SOFTWARE_RENDERING") == "1") {
            use_software_rendering = true;
        }
    }

    // Windows Server 2012 / 2012 R2 and Windows 8.x (< Windows 10) lack native Direct3D 12
    // and modern GPU hardware acceleration in server VM environments. Automatically
    // default to software rasterization fallback.
    if (!IsWindows10OrGreater()) {
        use_software_rendering = true;
    }

    if (use_software_rendering) {
        qputenv("QT_OPENGL", "software");
        qputenv("QSG_RHI_BACKEND", "software");
        qputenv("QT_QUICK_BACKEND", "software");
        QApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
    }
#elif defined(HARMONYCAST_SERVER2012_COMPAT)
    QApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
#endif

    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    qRegisterMetaType<harmonycast::DecodedFrame>("harmonycast::DecodedFrame");
    qRegisterMetaType<harmonycast::DecodedFrame>("DecodedFrame");
    app.setApplicationName("HarmonyCast");
    app.setApplicationDisplayName("HarmonyCast · 鸿蒙投屏工作台");
    app.setOrganizationName("HarmonyCast");
    app.setOrganizationDomain("local.harmonycast.desktop");

    // 1. Install crash handler
    harmonycast::CrashHandler::install();

    // 2. Initialize global logging
    harmonycast::Logger::init();
    HC_LOG_INFO("HarmonyCast main entry initialized");
#if defined(HARMONYCAST_SERVER2012_COMPAT)
    HC_LOG_INFO("Windows Server 2012 compatibility edition: Qt 5, software video decoding, software rasterization");
#endif
#if defined(Q_OS_WIN)
    if (use_software_rendering) {
        HC_LOG_INFO("Software rendering active (QT_OPENGL=software, AA_UseSoftwareOpenGL)");
    }
#endif

    QString icon_path = QDir::currentPath() + "/resources/app-icon.svg";
    if (QFile::exists(icon_path)) {
        app.setWindowIcon(QIcon(icon_path));
    }

    harmonycast::MainWindow window;
    window.show();

    int ret = app.exec();
    harmonycast::Logger::shutdown();
    return ret;
}
