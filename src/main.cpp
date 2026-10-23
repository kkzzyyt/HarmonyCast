#include <QApplication>
#include <QIcon>
#include <QDir>
#include "harmonycast/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    qRegisterMetaType<harmonycast::DecodedFrame>("harmonycast::DecodedFrame");
    qRegisterMetaType<harmonycast::DecodedFrame>("DecodedFrame");
    app.setApplicationName("HarmonyCast");
    app.setApplicationDisplayName("HarmonyCast · 鸿蒙投屏工作台");
    app.setOrganizationName("HarmonyCast");
    app.setOrganizationDomain("local.harmonycast.desktop");

    QString icon_path = QDir::currentPath() + "/resources/app-icon.svg";
    if (QFile::exists(icon_path)) {
        app.setWindowIcon(QIcon(icon_path));
    }

    harmonycast::MainWindow window;
    window.show();

    return app.exec();
}
