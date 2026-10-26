#include <QApplication>
#include <QIcon>
#include <QDir>
#include "harmonycast/MainWindow.h"
#include "harmonycast/Logger.h"
#include "harmonycast/CrashHandler.h"

int main(int argc, char* argv[]) {
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
