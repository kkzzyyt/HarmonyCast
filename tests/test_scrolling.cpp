#include "harmonycast/MainWindow.h"
#include "harmonycast/InspectionPage.h"
#include "harmonycast/DeviceManager.h"
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QMouseEvent>
#include <QProcess>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleOptionSlider>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
#include <iostream>
#include <stdexcept>

using namespace harmonycast;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate> static void waitUntil(Predicate predicate, int timeout = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    check(predicate(), "UI operation timed out");
}

static int reentrantDiscovery(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    auto hdc = std::make_shared<HdcClient>(QCoreApplication::applicationFilePath().toStdString());
    DeviceManager manager(hdc);
    bool connected = false, updated = false;
    QObject::connect(&manager, &DeviceManager::deviceConnected, &manager, [&](const Device& device) {
        connected = !manager.devices().empty() && manager.findDevice(device.serial).has_value();
    });
    QObject::connect(&manager, &DeviceManager::devicesUpdated, &manager, [&](const std::vector<Device>& devices) {
        updated = manager.devices().size() == devices.size();
        app.quit();
    });
    manager.refresh(true);
    QTimer::singleShot(2000, &app, &QCoreApplication::quit);
    app.exec();
    return connected && updated ? 0 : 1;
}

static void testReentrantDiscovery() {
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {"--reentrant-discovery"});
    check(child.waitForStarted(), "reentrant discovery probe failed to start");
    const bool finished = child.waitForFinished(4000);
    if (!finished) { child.kill(); child.waitForFinished(); }
    check(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
          "device discovery holds its mutex while invoking UI callbacks");
}

static void testStartup() {
    qputenv("HC_SCROLL_TEST_SLOW_HDC", "1");
    QSettings("HarmonyCast", "HarmonyCast").setValue("auto_check_update", false);
    MainWindow window(nullptr, true, QCoreApplication::applicationFilePath().toStdString());
    auto* manager = window.findChild<DeviceManager*>();
    check(manager, "device manager missing");
    auto* polling = manager->findChild<QTimer*>();
    check(polling && !polling->isActive(), "device polling starts before the first window is shown");
    int ticks = 0;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    QObject::connect(&heartbeat, &QTimer::timeout, &window, [&] { ++ticks; });
    heartbeat.start();
    window.show();
    waitUntil([&] { return polling->isActive() && ticks >= 12; });
    check(QMetaObject::invokeMethod(&window, "switchModule", Qt::DirectConnection, Q_ARG(int, 3)), "navigation unavailable");
    check(window.findChild<InspectionPage*>()->isVisible(), "slow HDC prevents navigation after startup");
    // Let the fake query finish before destroying its callback context.
    waitUntil([&] { return ticks >= 70; });
    qunsetenv("HC_SCROLL_TEST_SLOW_HDC");
}

static QRect paintedHandle(QScrollBar* bar) {
    const auto pixels = bar->grab().toImage();
    const qreal scale = pixels.devicePixelRatio();
    const bool horizontal = bar->orientation() == Qt::Horizontal;
    const int length = horizontal ? bar->width() : bar->height();
    const QColor base = bar->palette().color(QPalette::Base);
    int first = -1, last = -1;
    for (int position = 0; position < length; ++position) {
        const QColor color = pixels.pixelColor(qRound((horizontal ? position : bar->width() / 2) * scale),
                                               qRound((horizontal ? bar->height() / 2 : position) * scale));
        if (qAbs(color.lightness() - base.lightness()) > 40) {
            if (first < 0) first = position;
            last = position;
        }
    }
    check(first >= 0 && last > first, "painted scrollbar thumb missing");
    return horizontal ? QRect(first, 0, last - first + 1, bar->height()) : QRect(0, first, bar->width(), last - first + 1);
}

static void mouse(QScrollBar* bar, QEvent::Type type, const QPoint& position) {
    QMouseEvent event(type, QPointF(position), QPointF(bar->mapToGlobal(position)), Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &event);
}

static void testTableScrolling(MainWindow& window) {
    QMetaObject::invokeMethod(&window, "switchModule", Qt::DirectConnection, Q_ARG(int, 3));
    auto* table = window.findChild<QTableWidget*>("InspectionResults");
    check(table && table->horizontalScrollMode() == QAbstractItemView::ScrollPerPixel &&
          table->verticalScrollMode() == QAbstractItemView::ScrollPerPixel, "table scrolls by whole columns or rows");
    table->setRowCount(200);
    for (int row = 0; row < table->rowCount(); ++row) {
        table->setItem(row, 0, new QTableWidgetItem(QString("192.0.2.%1:8710").arg(row + 1)));
        table->setItem(row, 1, new QTableWidgetItem("完成"));
    }
    QCoreApplication::processEvents();
    for (auto* bar : {table->horizontalScrollBar(), table->verticalScrollBar()}) {
        check(bar->isVisible() && bar->maximum() > 0, "overflow scrollbar missing");
        const auto handle = paintedHandle(bar);
        check((bar->orientation() == Qt::Horizontal ? handle.width() : handle.height()) >= 32, "scrollbar thumb too small to grab");
        const auto start = handle.center();
        const auto end = start + (bar->orientation() == Qt::Horizontal ? QPoint(80, 0) : QPoint(0, 80));
        mouse(bar, QEvent::MouseButtonPress, start);
        mouse(bar, QEvent::MouseMove, end);
        mouse(bar, QEvent::MouseButtonRelease, end);
        check(bar->value() > 0, "scrollbar thumb does not drag");
        const QPoint held = paintedHandle(bar).center();
        mouse(bar, QEvent::MouseButtonPress, held);
        check(bar->isSliderDown(), "painted thumb hit area does not match its position");
        QEvent hidden(QEvent::Hide);
        QApplication::sendEvent(bar, &hidden);
        check(!bar->isSliderDown(), "hidden scrollbar remains stuck in a drag");
    }
    table->horizontalScrollBar()->setValue(100);
    table->verticalScrollBar()->setValue(100);
    const QPoint local = table->viewport()->rect().center();
    auto wheel = [&](QPoint pixels, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QWheelEvent event(QPointF(local), QPointF(table->viewport()->mapToGlobal(local)), pixels, {},
                          Qt::NoButton, modifiers, Qt::ScrollUpdate, false);
        QApplication::sendEvent(table->viewport(), &event);
    };
    wheel(QPoint(-24, 0));
    check(table->horizontalScrollBar()->value() == 124 && table->verticalScrollBar()->value() == 100,
          "horizontal trackpad pixels are lost or change the vertical position");
    wheel(QPoint(0, -18));
    check(table->verticalScrollBar()->value() == 118, "vertical trackpad pixels are lost");
    wheel(QPoint(0, -20), Qt::ShiftModifier);
    check(table->horizontalScrollBar()->value() == 144 && table->verticalScrollBar()->value() == 118,
          "Shift-scroll does not move horizontally");
    const auto artifacts = QString::fromUtf8(qgetenv("HC_SCROLL_TEST_ARTIFACTS"));
    if (!artifacts.isEmpty()) {
        QStringList addresses;
        for (int row = 0; row < table->rowCount(); ++row) {
            addresses << QString("192.0.2.%1").arg(row + 1);
            table->setItem(row, 2, new QTableWidgetItem(QString("%1天 08:24:12").arg(2 + row % 15)));
            table->setItem(row, 3, new QTableWidgetItem(QString("02:18:7d:00:00:%1").arg(row + 1, 2, 16, QChar('0'))));
            table->setItem(row, 4, new QTableWidgetItem(QString("%1 / 2048").arg(780 + row * 3)));
            table->setItem(row, 5, new QTableWidgetItem("38.1%"));
        }
        window.findChild<QPlainTextEdit*>("InspectionTargets")->setPlainText(addresses.join('\n'));
        table->horizontalScrollBar()->setValue(0);
        table->verticalScrollBar()->setValue(0);
        for (auto* label : window.findChildren<QLabel*>()) {
            if (label->text() == "导入 IP 列表后开始巡检") label->setText("巡检结果预览（测试数据）");
            if (label->text().startsWith("采集运行时长")) label->setText("完成 200 · 部分完成 0 · 失败 0");
        }
        QCoreApplication::processEvents();
        check(window.grab().save(artifacts + "/scrollbars.png"), "scrollbar preview could not be saved");
    }
}

static void testLogScrollPosition(MainWindow& window) {
    // The production default is screen-only; opt into the split workspace so
    // log viewport scrolling is exercised on a visible panel.
    check(QMetaObject::invokeMethod(&window, "setWorkspaceLayout", Qt::DirectConnection,
                                    Q_ARG(QString, QString("horizontal"))), "log layout unavailable");
    QMetaObject::invokeMethod(&window, "switchModule", Qt::DirectConnection, Q_ARG(int, 1));
    auto* logs = window.findChild<QPlainTextEdit*>("LogView");
    std::vector<HiLogEntry> entries(400);
    for (int i = 0; i < 400; ++i) {
        entries[i].bundle = "com.yarward.test";
        entries[i].timestamp = "12:00:00.000";
        entries[i].message = "scroll-test-" + std::to_string(i) + std::string(200, 'x');
    }
    QMetaObject::invokeMethod(&window, "onHiLogEntries", Qt::DirectConnection, Q_ARG(std::vector<HiLogEntry>, entries));
    waitUntil([&] { return logs->toPlainText().contains("scroll-test-399"); });
    auto* bar = logs->verticalScrollBar();
    check(bar->maximum() > 30, "log scrolling fixture did not overflow");
    bar->setValue(bar->maximum() - 10);
    const int oldPosition = bar->value();
    std::vector<HiLogEntry> next{entries.front()};
    next.front().message = "new-log-line";
    QMetaObject::invokeMethod(&window, "onHiLogEntries", Qt::DirectConnection, Q_ARG(std::vector<HiLogEntry>, next));
    waitUntil([&] { return logs->toPlainText().contains("new-log-line"); });
    check(bar->value() == oldPosition, "new logs pull the user back to the bottom");
    bar->setValue(bar->maximum());
    bar->setSliderDown(true);
    const int heldPosition = bar->value();
    next.front().message = "held-drag-log";
    QMetaObject::invokeMethod(&window, "onHiLogEntries", Qt::DirectConnection, Q_ARG(std::vector<HiLogEntry>, next));
    waitUntil([&] { return logs->toPlainText().contains("held-drag-log"); });
    check(bar->value() == heldPosition, "new logs move the scrollbar during a drag");
    bar->setSliderDown(false);
    bar->setValue(oldPosition);
    QMetaObject::invokeMethod(&window, "renderHiLog", Qt::DirectConnection);
    check(bar->value() == oldPosition, "log rebuilding discards the reading position");
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--reentrant-discovery") return reentrantDiscovery(argc, argv);
    if (argc > 1) {
        if (argc > 2 && std::string(argv[1]) == "list" && std::string(argv[2]) == "targets") {
            if (qEnvironmentVariableIsSet("HC_SCROLL_TEST_SLOW_HDC")) {
                QThread::msleep(600);
                std::cout << "[Empty]\n";
            } else std::cout << "scroll-test-usb USB Connected HR36\n";
        }
        return 0;
    }
    if (!qEnvironmentVariableIsSet("HC_SCROLL_TEST_NATIVE")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QSettings("HarmonyCast", "HarmonyCast").setValue("auto_start_mirror", false);
    try {
        testReentrantDiscovery();
        testStartup();
        MainWindow window(nullptr, false, QCoreApplication::applicationFilePath().toStdString());
        window.show();
        testTableScrolling(window);
        testLogScrollPosition(window);
        std::cout << "Startup and scrolling regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
