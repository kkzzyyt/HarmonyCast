#include "harmonycast/MainWindow.h"
#include "harmonycast/FlowLayout.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QScrollBar>
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <iostream>

namespace harmonycast {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      m_hdc(std::make_shared<HdcClient>()),
      m_dev_mgr(std::make_shared<DeviceManager>(m_hdc, this)) {

    setupUi();
    applyTheme();

    connect(m_dev_mgr.get(), &DeviceManager::devicesUpdated, this, &MainWindow::onDevicesUpdated);
    m_dev_mgr->startPolling(1500);
}

MainWindow::~MainWindow() {
    onStopMirror();
    if (m_hilog) m_hilog->stop();
}

void MainWindow::setupUi() {
    setWindowTitle("HarmonyCast · 鸿蒙投屏工作台");
    setMinimumSize(960, 680);
    resize(1120, 780);

    auto* central = new QWidget(this);
    setCentralWidget(central);

    auto* root_layout = new QVBoxLayout(central);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    setupHeader();
    root_layout->addWidget(m_device_combo->parentWidget());

    auto* body_widget = new QWidget(central);
    auto* body_layout = new QHBoxLayout(body_widget);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);

    setupSidebar();
    body_layout->addWidget(m_nav_mirror->parentWidget());

    m_pages = new QStackedWidget(body_widget);
    setupMirrorPage();
    setupHiLogPage();
    setupInfoPage();
    setupHdcPage();

    m_pages->addWidget(m_mirror_page);
    m_pages->addWidget(m_hilog_page);
    m_pages->addWidget(m_info_page);
    m_pages->addWidget(m_hdc_page);

    body_layout->addWidget(m_pages, 1);
    root_layout->addWidget(body_widget, 1);
}

void MainWindow::setupHeader() {
    auto* header = new QWidget(this);
    header->setFixedHeight(50);
    header->setObjectName("headerWidget");

    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(16, 8, 16, 8);
    layout->setSpacing(12);

    auto* title = new QLabel("HarmonyCast", header);
    title->setObjectName("appTitle");
    layout->addWidget(title);

    m_device_combo = new QComboBox(header);
    m_device_combo->setMinimumWidth(220);
    m_device_combo->addItem("正在探测设备…");
    connect(m_device_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onDeviceSelected);
    layout->addWidget(m_device_combo);

    m_state_badge = new QLabel("未连接设备", header);
    m_state_badge->setObjectName("stateBadge");
    layout->addWidget(m_state_badge);

    m_refresh_btn = new QPushButton("刷新", header);
    m_refresh_btn->setFixedWidth(64);
    connect(m_refresh_btn, &QPushButton::clicked, this, &MainWindow::onRefreshClicked);
    layout->addWidget(m_refresh_btn);

    layout->addStretch();

    m_mode_combo = new QComboBox(header);
    m_mode_combo->addItems({"原生投屏 (低延迟)", "流畅投屏 (高码率)", "兼容预览 (截图)"});
    layout->addWidget(m_mode_combo);

    m_quality_combo = new QComboBox(header);
    m_quality_combo->addItems({"超清 8M 60F", "高清 4M 30F", "流畅 1.5M 15F"});
    layout->addWidget(m_quality_combo);
}

void MainWindow::setupSidebar() {
    auto* sidebar = new QWidget(this);
    sidebar->setFixedWidth(120);
    sidebar->setObjectName("sidebarWidget");

    auto* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(8, 16, 8, 16);
    layout->setSpacing(8);

    auto makeNavBtn = [sidebar](const QString& text) {
        auto* btn = new QPushButton(text, sidebar);
        btn->setCheckable(true);
        btn->setAutoExclusive(true);
        btn->setFixedHeight(40);
        btn->setObjectName("navButton");
        return btn;
    };

    m_nav_mirror = makeNavBtn("投屏工作台");
    m_nav_hilog = makeNavBtn("HiLog 日志");
    m_nav_info = makeNavBtn("设备信息");
    m_nav_hdc = makeNavBtn("常用命令");

    m_nav_mirror->setChecked(true);

    layout->addWidget(m_nav_mirror);
    layout->addWidget(m_nav_hilog);
    layout->addWidget(m_nav_info);
    layout->addWidget(m_nav_hdc);
    layout->addStretch();

    connect(m_nav_mirror, &QPushButton::clicked, this, [this]() { m_pages->setCurrentWidget(m_mirror_page); });
    connect(m_nav_hilog, &QPushButton::clicked, this, [this]() { m_pages->setCurrentWidget(m_hilog_page); });
    connect(m_nav_info, &QPushButton::clicked, this, [this]() { m_pages->setCurrentWidget(m_info_page); onRefreshDeviceInfo(); });
    connect(m_nav_hdc, &QPushButton::clicked, this, [this]() { m_pages->setCurrentWidget(m_hdc_page); });
}

void MainWindow::setupMirrorPage() {
    m_mirror_page = new QWidget(this);
    auto* layout = new QVBoxLayout(m_mirror_page);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // Toolbar with responsive FlowLayout
    auto* toolbar = new QWidget(m_mirror_page);
    auto* flow = new FlowLayout(toolbar, 0, 8, 8);

    m_btn_toggle_mirror = new QPushButton("开始投屏", toolbar);
    m_btn_toggle_mirror->setObjectName("primaryButton");
    connect(m_btn_toggle_mirror, &QPushButton::clicked, this, &MainWindow::onToggleMirror);
    flow->addWidget(m_btn_toggle_mirror);

    auto* btn_back = new QPushButton("返回", toolbar);
    connect(btn_back, &QPushButton::clicked, this, &MainWindow::onActionBack);
    flow->addWidget(btn_back);

    auto* btn_home = new QPushButton("主页", toolbar);
    connect(btn_home, &QPushButton::clicked, this, &MainWindow::onActionHome);
    flow->addWidget(btn_home);

    auto* btn_wake = new QPushButton("亮屏", toolbar);
    connect(btn_wake, &QPushButton::clicked, this, &MainWindow::onActionWakeup);
    flow->addWidget(btn_wake);

    auto* btn_rotate = new QPushButton("旋转", toolbar);
    connect(btn_rotate, &QPushButton::clicked, this, &MainWindow::onActionRotate);
    flow->addWidget(btn_rotate);

    auto* btn_shot = new QPushButton("截图保存", toolbar);
    connect(btn_shot, &QPushButton::clicked, this, &MainWindow::onActionScreenshot);
    flow->addWidget(btn_shot);

    m_fps_label = new QLabel("就绪", toolbar);
    m_fps_label->setObjectName("fpsLabel");
    flow->addWidget(m_fps_label);

    layout->addWidget(toolbar);

    m_screen_view = new ScreenView(m_mirror_page);
    m_screen_view->setTouchCallback([this](TouchAction action, int x, int y, int dev_w, int dev_h) {
        if (m_stream && m_stream->controlChannel()) {
            m_stream->controlChannel()->sendTouch(action, x, y, dev_w, dev_h);
        }
    });

    layout->addWidget(m_screen_view, 1);
}

void MainWindow::setupHiLogPage() {
    m_hilog_page = new QWidget(this);
    auto* layout = new QVBoxLayout(m_hilog_page);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // Toolbar with FlowLayout
    auto* tb = new QWidget(m_hilog_page);
    auto* flow = new FlowLayout(tb, 0, 8, 8);

    m_search_input = new QLineEdit(tb);
    m_search_input->setPlaceholderText("搜索日志内容、Tag 或 PID…");
    m_search_input->setMinimumWidth(200);
    connect(m_search_input, &QLineEdit::textChanged, this, &MainWindow::onHiLogFilterChanged);
    flow->addWidget(m_search_input);

    m_pkg_combo = new QComboBox(tb);
    m_pkg_combo->setFixedWidth(135);
    m_pkg_combo->addItems({"全部应用", "com.yarward.*"});
    connect(m_pkg_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onHiLogFilterChanged);
    flow->addWidget(m_pkg_combo);

    m_level_combo = new QComboBox(tb);
    m_level_combo->addItems({"全部级别", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"});
    connect(m_level_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onHiLogFilterChanged);
    flow->addWidget(m_level_combo);

    m_cat_combo = new QComboBox(tb);
    m_cat_combo->addItems({"全部日志", "应用日志", "系统日志"});
    connect(m_cat_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onHiLogFilterChanged);
    flow->addWidget(m_cat_combo);

    auto* btn_clear = new QPushButton("清空", tb);
    connect(btn_clear, &QPushButton::clicked, this, &MainWindow::onHiLogClear);
    flow->addWidget(btn_clear);

    m_chk_autoscroll = new QCheckBox("自动滚动", tb);
    m_chk_autoscroll->setChecked(true);
    flow->addWidget(m_chk_autoscroll);

    m_chk_pause = new QCheckBox("暂停", tb);
    flow->addWidget(m_chk_pause);

    auto* btn_export = new QPushButton("导出", tb);
    connect(btn_export, &QPushButton::clicked, this, &MainWindow::onHiLogExport);
    flow->addWidget(btn_export);

    layout->addWidget(tb);

    m_log_view = new QPlainTextEdit(m_hilog_page);
    m_log_view->setReadOnly(true);
    m_log_view->setMaximumBlockCount(10000);
    m_log_view->setFont(QFont("Menlo", 12));
    layout->addWidget(m_log_view, 1);

    m_log_count_label = new QLabel("缓存 0 条", m_hilog_page);
    m_log_count_label->setObjectName("mutedLabel");
    layout->addWidget(m_log_count_label);
}

void MainWindow::setupInfoPage() {
    m_info_page = new QWidget(this);
    auto* layout = new QVBoxLayout(m_info_page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    auto makeCard = [](const QString& label_text) {
        auto* card = new QWidget();
        card->setObjectName("infoCard");
        auto* l = new QVBoxLayout(card);
        l->setContentsMargins(16, 12, 16, 12);
        auto* title = new QLabel(label_text, card);
        title->setObjectName("cardTitle");
        l->addWidget(title);
        auto* val = new QLabel("—", card);
        val->setObjectName("cardValue");
        l->addWidget(val);
        return std::make_pair(card, val);
    };

    auto [c1, v1] = makeCard("设备型号");
    m_info_model = v1;
    layout->addWidget(c1);

    auto [c2, v2] = makeCard("设备序列号");
    m_info_serial = v2;
    layout->addWidget(c2);

    auto [c3, v3] = makeCard("系统版本");
    m_info_version = v3;
    layout->addWidget(c3);

    auto [c4, v4] = makeCard("开机运行时长");
    m_info_uptime = v4;
    layout->addWidget(c4);

    auto [c5, v5] = makeCard("屏幕规格与方向");
    m_info_display = v5;
    layout->addWidget(c5);

    auto* mem_card = new QWidget(m_info_page);
    mem_card->setObjectName("infoCard");
    auto* m_layout = new QVBoxLayout(mem_card);
    m_layout->setContentsMargins(16, 12, 16, 12);
    m_layout->addWidget(new QLabel("内存使用情况", mem_card));
    m_mem_bar = new QProgressBar(mem_card);
    m_mem_bar->setRange(0, 100);
    m_mem_bar->setValue(0);
    m_layout->addWidget(m_mem_bar);
    m_mem_label = new QLabel("总内存: —", mem_card);
    m_layout->addWidget(m_mem_label);
    layout->addWidget(mem_card);

    layout->addStretch();
}

void MainWindow::setupHdcPage() {
    m_hdc_page = new QWidget(this);
    auto* layout = new QVBoxLayout(m_hdc_page);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto* top = new QHBoxLayout();
    m_hdc_preset_combo = new QComboBox(m_hdc_page);
    m_hdc_preset_combo->addItems({
        "预置常用命令…",
        "shell param get",
        "shell bm dump -a",
        "shell hidumper -s DisplayManagerService -a -a",
        "shell power-shell wakeup"
    });
    connect(m_hdc_preset_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onHdcPresetSelected);
    top->addWidget(m_hdc_preset_combo);

    m_hdc_input = new QLineEdit(m_hdc_page);
    m_hdc_input->setPlaceholderText("输入 HDC 命令参数，如：shell getprop");
    top->addWidget(m_hdc_input, 1);

    auto* btn_run = new QPushButton("执行", m_hdc_page);
    btn_run->setObjectName("primaryButton");
    connect(btn_run, &QPushButton::clicked, this, &MainWindow::onHdcRunCommand);
    top->addWidget(btn_run);

    layout->addLayout(top);

    m_hdc_output = new QPlainTextEdit(m_hdc_page);
    m_hdc_output->setReadOnly(true);
    m_hdc_output->setFont(QFont("Menlo", 12));
    layout->addWidget(m_hdc_output, 1);
}

void MainWindow::applyTheme() {
    setStyleSheet(R"(
        QMainWindow, QWidget#headerWidget, QWidget#sidebarWidget {
            background-color: #0d1520;
            color: #dbe8f5;
        }
        QWidget#headerWidget {
            border-bottom: 1px solid #1e2c3d;
        }
        QWidget#sidebarWidget {
            border-right: 1px solid #1e2c3d;
        }
        QLabel#appTitle {
            font-size: 16px;
            font-weight: bold;
            color: #53d6b5;
        }
        QLabel#stateBadge {
            background-color: #102a24;
            color: #53d6b5;
            padding: 4px 10px;
            border-radius: 4px;
            font-size: 12px;
            font-weight: 500;
        }
        QLabel#fpsLabel {
            color: #53d6b5;
            font-size: 13px;
            font-weight: bold;
            padding: 2px 6px;
        }
        QPushButton {
            background-color: #1c2938;
            color: #dbe8f5;
            border: 1px solid #2d3f54;
            border-radius: 6px;
            padding: 6px 14px;
            font-size: 13px;
        }
        QPushButton:hover {
            background-color: #243548;
            border-color: #3b526d;
        }
        QPushButton#primaryButton {
            background-color: #53d6b5;
            color: #0b151e;
            font-weight: bold;
            border: none;
        }
        QPushButton#primaryButton:hover {
            background-color: #63e2c3;
        }
        QPushButton#navButton {
            text-align: left;
            padding-left: 12px;
            border: none;
            background: transparent;
        }
        QPushButton#navButton:checked {
            background-color: #172433;
            color: #53d6b5;
            font-weight: bold;
            border-left: 3px solid #53d6b5;
        }
        QComboBox, QLineEdit {
            background-color: #141f2c;
            color: #dbe8f5;
            border: 1px solid #233346;
            border-radius: 6px;
            padding: 5px 10px;
        }
        QPlainTextEdit {
            background-color: #09111a;
            color: #c9d8e8;
            border: 1px solid #1e2c3d;
            border-radius: 6px;
        }
        QWidget#infoCard {
            background-color: #121c27;
            border: 1px solid #1e2c3d;
            border-radius: 8px;
        }
        QLabel#cardTitle {
            color: #748ca2;
            font-size: 12px;
        }
        QLabel#cardValue {
            color: #ffffff;
            font-size: 15px;
            font-weight: bold;
        }
    )");
}

std::optional<Device> MainWindow::currentDevice() const {
    if (m_device_combo->currentIndex() < 0) return std::nullopt;
    QString serial = m_device_combo->currentData().toString();
    if (serial.isEmpty()) return std::nullopt;
    return m_dev_mgr->findDevice(serial.toStdString());
}

void MainWindow::onDevicesUpdated(const std::vector<Device>& devices) {
    QString current_serial = m_device_combo->currentData().toString();
    m_device_combo->blockSignals(true);
    m_device_combo->clear();

    if (devices.empty()) {
        m_device_combo->addItem("未检测到设备", "");
        m_state_badge->setText("未连接设备");
    } else {
        int select_idx = 0;
        for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
            const auto& dev = devices[i];
            QString text = QString::fromStdString(dev.serial);
            if (dev.is_network) text += " [Wi-Fi/LAN]";
            m_device_combo->addItem(text, QString::fromStdString(dev.serial));
            if (QString::fromStdString(dev.serial) == current_serial) {
                select_idx = i;
            }
        }
        m_device_combo->setCurrentIndex(select_idx);
        m_state_badge->setText(QString("已连接 %1 台设备").arg(devices.size()));
    }
    m_device_combo->blockSignals(false);
}

void MainWindow::onDeviceSelected(int) {
    auto dev = currentDevice();
    if (dev) {
        m_state_badge->setText(QString("目标: %1").arg(QString::fromStdString(dev->serial)));
    }
}

void MainWindow::onRefreshClicked() {
    m_dev_mgr->refresh();
}

void MainWindow::onToggleMirror() {
    if (m_stream && m_stream->isRunning()) {
        onStopMirror();
    } else {
        auto dev = currentDevice();
        if (!dev) {
            QMessageBox::warning(this, "投屏提示", "请先在上方连接并选择目标 OpenHarmony 设备");
            return;
        }

        int fps = (m_quality_combo->currentIndex() == 0) ? 60 : ((m_quality_combo->currentIndex() == 1) ? 30 : 15);
        int bitrate = (m_quality_combo->currentIndex() == 0) ? 8000000 : ((m_quality_combo->currentIndex() == 1) ? 4000000 : 1500000);

        m_stream = std::make_unique<StreamSession>(m_hdc, *dev, fps, bitrate, 0, this);
        connect(m_stream.get(), &StreamSession::frameReady, this, &MainWindow::onFrameReady);
        connect(m_stream.get(), &StreamSession::statusChanged, this, &MainWindow::onStreamStatusChanged);
        connect(m_stream.get(), &StreamSession::streamFinished, this, &MainWindow::onStreamFinished);

        m_screen_view->setActive(true);
        m_btn_toggle_mirror->setText("停止投屏");
        m_fps_counter = 0;
        m_fps_last_time = QDateTime::currentMSecsSinceEpoch();

        m_stream->start();
    }
}

void MainWindow::onStopMirror() {
    if (m_stream) {
        m_stream->stop();
        m_stream.reset();
    }
    m_screen_view->setActive(false);
    m_screen_view->clear();
    m_btn_toggle_mirror->setText("开始投屏");
    m_fps_label->setText("已断开");
}

void MainWindow::onFrameReady(const DecodedFrame& frame) {
    m_screen_view->updateFrame(frame);

    m_fps_counter++;
    int64_t now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_fps_last_time >= 1000) {
        double fps = (m_fps_counter * 1000.0) / (now - m_fps_last_time);
        m_fps_label->setText(QString("%1 × %2 · %3 FPS")
                                 .arg(frame.width).arg(frame.height).arg(QString::number(fps, 'f', 1)));
        m_fps_counter = 0;
        m_fps_last_time = now;
    }
}

void MainWindow::onStreamStatusChanged(const QString& status) {
    m_screen_view->setStatusMessage(status);
}

void MainWindow::onStreamFinished(const QString& error) {
    onStopMirror();
    if (!error.isEmpty()) {
        m_screen_view->setStatusMessage(error);
    }
}

void MainWindow::onActionBack() {
    auto dev = currentDevice();
    if (dev) m_hdc->back(dev->serial);
}

void MainWindow::onActionHome() {
    auto dev = currentDevice();
    if (dev) m_hdc->home(dev->serial);
}

void MainWindow::onActionWakeup() {
    auto dev = currentDevice();
    if (dev) m_hdc->wakeup(dev->serial);
}

void MainWindow::onActionPower() {
    auto dev = currentDevice();
    if (dev) m_hdc->run({"shell", "uinput -K -d 18 -u 18"}, dev->serial);
}

void MainWindow::onActionRotate() {
    int next_rot = (m_screen_view->rotation() + 90) % 360;
    m_screen_view->setRotation(next_rot);
}

void MainWindow::onActionScreenshot() {
    QImage img = m_screen_view->grab().toImage();
    if (img.isNull()) return;

    QString default_path = QDir::homePath() + QString("/Desktop/HarmonyCast_%1.png")
                               .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    QString path = QFileDialog::getSaveFileName(this, "保存截图", default_path, "PNG Images (*.png)");
    if (!path.isEmpty()) {
        img.save(path);
        QMessageBox::information(this, "保存成功", "截图已成功保存至：\n" + path);
    }
}

void MainWindow::onHiLogEntriesReady(const std::vector<HiLogEntry>& entries) {
    if (m_chk_pause->isChecked()) return;

    QStringList lines;
    lines.reserve(entries.size());

    std::string kw = m_search_input->text().toStdString();
    std::string pkg = (m_pkg_combo->currentIndex() == 1) ? "com.yarward.*" : "";
    std::string cat = (m_cat_combo->currentIndex() == 1) ? "app" : ((m_cat_combo->currentIndex() == 2) ? "system" : "");
    LogLevel min_lvl = static_cast<LogLevel>(std::max(0, m_level_combo->currentIndex() - 1));

    for (const auto& e : entries) {
        if (m_cached_entries.size() >= 10000) {
            m_cached_entries.erase(m_cached_entries.begin());
        }
        m_cached_entries.push_back(e);

        if (HiLogParser::matches(e, kw, min_lvl, cat, pkg)) {
            lines.append(QString::fromStdString(e.format()));
        }
    }

    if (!lines.isEmpty()) {
        m_log_view->appendPlainText(lines.join("\n"));
        if (m_chk_autoscroll->isChecked()) {
            m_log_view->verticalScrollBar()->setValue(m_log_view->verticalScrollBar()->maximum());
        }
    }
    m_log_count_label->setText(QString("缓存 %1 条").arg(m_cached_entries.size()));
}

void MainWindow::onHiLogClear() {
    m_cached_entries.clear();
    m_log_view->clear();
    m_log_count_label->setText("缓存 0 条");
}

void MainWindow::onHiLogFilterChanged() {
    renderHiLog();
}

void MainWindow::renderHiLog() {
    std::string kw = m_search_input->text().toStdString();
    std::string pkg = (m_pkg_combo->currentIndex() == 1) ? "com.yarward.*" : "";
    std::string cat = (m_cat_combo->currentIndex() == 1) ? "app" : ((m_cat_combo->currentIndex() == 2) ? "system" : "");
    LogLevel min_lvl = static_cast<LogLevel>(std::max(0, m_level_combo->currentIndex() - 1));

    QStringList filtered;
    for (const auto& e : m_cached_entries) {
        if (HiLogParser::matches(e, kw, min_lvl, cat, pkg)) {
            filtered.append(QString::fromStdString(e.format()));
        }
    }
    m_log_view->setPlainText(filtered.join("\n"));
}

void MainWindow::onHiLogExport() {
    QString path = QFileDialog::getSaveFileName(this, "导出日志", QDir::homePath() + "/hilog.txt", "Text Files (*.txt)");
    if (!path.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << m_log_view->toPlainText();
            QMessageBox::information(this, "导出完成", "日志已保存至：\n" + path);
        }
    }
}

void MainWindow::onRefreshDeviceInfo() {
    auto dev = currentDevice();
    if (!dev) return;

    auto info = m_hdc->getDeviceInfo(dev->serial);
    if (!info) return;

    m_info_model->setText(QString::fromStdString(info->model.empty() ? "OpenHarmony Device" : info->model));
    m_info_serial->setText(QString::fromStdString(info->serial));
    m_info_version->setText(QString::fromStdString(info->os_version.empty() ? "OpenHarmony 5.0" : info->os_version));

    // Format uptime into Days / Hours / Minutes / Seconds
    int64_t up = info->uptime_seconds;
    int days = up / 86400;
    int hours = (up % 86400) / 3600;
    int mins = (up % 3600) / 60;
    int secs = up % 60;
    if (days > 0) {
        m_info_uptime->setText(QString("%1 天 %2 小时 %3 分钟 %4 秒").arg(days).arg(hours).arg(mins).arg(secs));
    } else {
        m_info_uptime->setText(QString("%1 小时 %2 分钟 %3 秒").arg(hours).arg(mins).arg(secs));
    }

    m_info_display->setText(QString("%1 × %2 (方向: %3°)")
                                .arg(info->display_width).arg(info->display_height).arg(info->display_rotation));

    if (info->mem_total_kb > 0) {
        uint64_t used_kb = info->mem_total_kb - info->mem_free_kb;
        int pct = static_cast<int>((used_kb * 100) / info->mem_total_kb);
        m_mem_bar->setValue(pct);
        m_mem_label->setText(QString("已用: %1 MB / 总计: %2 MB (%3%)")
                                 .arg(used_kb / 1024).arg(info->mem_total_kb / 1024).arg(pct));
    }
}

void MainWindow::onHdcPresetSelected(int index) {
    if (index > 0) {
        m_hdc_input->setText(m_hdc_preset_combo->currentText());
    }
}

void MainWindow::onHdcRunCommand() {
    auto dev = currentDevice();
    if (!dev) {
        QMessageBox::warning(this, "提示", "请选择设备");
        return;
    }

    QString text = m_hdc_input->text().trimmed();
    if (text.isEmpty()) return;

    QStringList parts = text.split(" ", Qt::SkipEmptyParts);
    std::vector<std::string> args;
    for (const auto& p : parts) args.push_back(p.toStdString());

    m_hdc_output->appendPlainText("> hdc " + text);
    auto res = m_hdc->run(args, dev->serial);
    if (!res.out.empty()) m_hdc_output->appendPlainText(QString::fromStdString(res.out));
    if (!res.err.empty()) m_hdc_output->appendPlainText("[STDERR] " + QString::fromStdString(res.err));
}

} // namespace harmonycast
