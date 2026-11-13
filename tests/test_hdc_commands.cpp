#include "harmonycast/MainWindow.h"
#include "harmonycast/HdcClient.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>

using namespace harmonycast;

static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

int main(int argc, char** argv) {
    if (argc > 1) {
        QJsonArray args;
        for (int i = 1; i < argc; ++i) args.append(QString::fromUtf8(argv[i]));
        std::cout << "ARGS:" << QJsonDocument(args).toJson(QJsonDocument::Compact).toStdString();
        return 0;
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temp;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    try {
        check(HdcClient::commandArguments("ls -la /data") == std::vector<std::string>{"shell", "ls -la /data"}, "default is not hdc shell");
        const QString expression = "echo 'hello world' | cat > /data/local/tmp/test.txt";
        check(HdcClient::commandArguments(expression) == std::vector<std::string>{"shell", expression.toStdString()}, "shell expression was split or unquoted");
        check(HdcClient::commandArguments("shell ifconfig") == std::vector<std::string>{"shell", "ifconfig"}, "explicit shell prefix duplicated");
        check(HdcClient::commandArguments("hdc shell ifconfig") == std::vector<std::string>{"shell", "ifconfig"}, "pasted hdc shell prefix duplicated");
        check(HdcClient::commandArguments("hdc list targets") == std::vector<std::string>{"list", "targets"}, "explicit HDC command treated as shell");
        check(HdcClient::commandArguments("file send \"C:/test dir/file.hap\" /data/local/tmp/", false) ==
              std::vector<std::string>{"file", "send", "C:/test dir/file.hap", "/data/local/tmp/"}, "host command quoting lost");
        check(HdcClient::commandArguments("  ").empty() && HdcClient::commandArguments("hdc shell").empty(), "empty input starts interactive shell");
        MainWindow window(nullptr, false, QCoreApplication::applicationFilePath().toStdString(), "/missing/adb");
        auto* input = window.findChild<QLineEdit*>("HdcCommandInput");
        auto* mode = window.findChild<QComboBox*>("HdcCommandMode");
        auto* presets = window.findChild<QComboBox*>("HdcPresets");
        auto* terminal = window.findChild<QTextEdit*>("HdcTerminal");
        check(input && mode && presets && terminal && mode->currentData().toBool(), "shell command UI not default");
        presets->setCurrentIndex(presets->findText("target mount"));
        check(!mode->currentData().toBool(), "target mount preset runs through shell");
        presets->setCurrentIndex(presets->findText("ifconfig"));
        check(mode->currentData().toBool() && input->text() == "ifconfig", "shell preset does not reset default mode");
        check(QMetaObject::invokeMethod(&window, "executeCustomHdc", Qt::DirectConnection), "command execution unavailable");
        QElapsedTimer timer; timer.start();
        while (!terminal->toPlainText().contains("ARGS:") && timer.elapsed() < 4000) { app.processEvents(); QThread::msleep(2); }
        check(terminal->toPlainText().contains("ARGS:[\"shell\",\"ifconfig\"]"), "actual HDC arguments differ from UI default");

        ScreenView screen;
        screen.setPalette(IconManager::getAppPalette());
        screen.resize(300, 200);
        check(screen.grab().toImage().pixelColor(0, 0) == screen.palette().color(QPalette::Window), "empty mirror background remains black");
        QImage video(100, 100, QImage::Format_RGB32); video.fill(QColor("#be3246"));
        screen.setImage(video);
        auto rendered = screen.grab().toImage();
        const auto dpr = rendered.devicePixelRatio();
        check(rendered.pixelColor(0, 0) == screen.palette().color(QPalette::Window), "video margin remains black");
        check(rendered.pixelColor(qRound(150 * dpr), qRound(100 * dpr)) == QColor("#be3246"), "video pixels changed with the canvas theme");
        int displayUpdates = 0;
        QObject::connect(&screen, &ScreenView::presentationChanged, &screen, [&](QSize, int, qreal) { ++displayUpdates; });
        QEvent monitorChange(QEvent::ScreenChangeInternal);
        QApplication::sendEvent(&screen, &monitorChange);
        check(displayUpdates == 1 && screen.image() == video, "monitor changes do not refresh the presentation or alter captured pixels");
        std::cout << "HDC default shell and mirror canvas tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
