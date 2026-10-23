#include "harmonycast/MainWindow.h"
#include "harmonycast/FlowLayout.h"
#include "harmonycast/IconManager.h"

#include <QApplication>
#include <QClipboard>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QFileDialog>
#include <QMessageBox>
#include <QScrollBar>
#include <QDateTime>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QHeaderView>
#include <QRegularExpression>
#include <QMenu>
#include <QShortcut>
#include <QInputDialog>
#include <iostream>

namespace harmonycast {

static const char* DEFAULT_PACKAGE_FILTER = "com.yarward.*";

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      m_hdc(std::make_shared<HdcClient>()),
      m_dev_mgr(std::make_shared<DeviceManager>(m_hdc, this)),
      m_webview(std::make_unique<WebViewDebugger>(m_hdc, this)),
      m_settings(std::make_unique<QSettings>("HarmonyCast", "HarmonyCast")) {

    m_auto_start_mirror = m_settings->value("auto_start_mirror", true).toBool();

    setWindowTitle("鸿蒙投屏 · HarmonyCast");
    resize(1120, 800);
    setMinimumSize(880, 600);
    setPalette(IconManager::getDarkPalette());

    setupUi();
    setupShortcuts();
    applyTheme();

    connect(m_dev_mgr.get(), &DeviceManager::devicesUpdated, this, &MainWindow::onDevicesUpdated);
    connect(m_webview.get(), &WebViewDebugger::consoleMessageReceived, this, &MainWindow::onWebConsoleMessage);
    connect(m_webview.get(), &WebViewDebugger::networkRequestReceived, this, &MainWindow::onWebNetworkRequest);
    connect(m_webview.get(), &WebViewDebugger::networkResponseReceived, this, &MainWindow::onWebNetworkResponse);
    connect(m_webview.get(), &WebViewDebugger::pagesLoaded, this, [this](const std::vector<WebViewPage>& pages){
        if (pages.empty()) {
            showToast("检测到 WebView 调试端口，但暂未发现活跃网页");
        } else {
            m_webview->connectPage(pages[0]);
            showToast(QString("已连接 WebView 页面: %1").arg(QString::fromStdString(pages[0].title.empty() ? pages[0].url : pages[0].title)));
        }
    });

    m_dev_mgr->startPolling(1500);
    switchModule(0);
}

MainWindow::~MainWindow() {
    stopMirror(false);
}

void MainWindow::applyTheme() {
    qApp->setPalette(IconManager::getDarkPalette());
    setStyleSheet(IconManager::getAppStyleSheet());
}

void MainWindow::setupUi() {
    auto* root = new QWidget(this);
    setCentralWidget(root);

    auto* horizontal = new QHBoxLayout(root);
    horizontal->setContentsMargins(0, 0, 0, 0);
    horizontal->setSpacing(0);

    // ==========================================
    // 1. Sidebar (侧边栏, 宽度 156px)
    // ==========================================
    m_sidebar = new QFrame(this);
    m_sidebar->setObjectName("Sidebar");
    m_sidebar->setFixedWidth(156);

    auto* side = new QVBoxLayout(m_sidebar);
    side->setContentsMargins(12, 20, 12, 16);
    side->setSpacing(8);

    // Brand
    auto* brand_box = new QHBoxLayout();
    brand_box->setContentsMargins(2, 0, 2, 0);
    brand_box->setSpacing(8);

    auto* brand_icon = new QLabel();
    brand_icon->setPixmap(IconManager::getIcon("workspace").pixmap(18, 18));
    brand_box->addWidget(brand_icon);

    auto* brand_title = IconManager::label("HarmonyCast", "Brand");
    brand_box->addWidget(brand_title);
    brand_box->addStretch();
    side->addLayout(brand_box);
    side->addSpacing(16);

    // Nav Items: 3 primary modules
    m_info_nav = IconManager::button("设备信息", "Navigation", "device");
    m_mirror_nav = IconManager::button("投屏工作台", "Navigation", "mirror");
    m_hdc_nav = IconManager::button("常用命令", "Navigation", "command");

    for (auto* item : {m_info_nav, m_mirror_nav, m_hdc_nav}) {
        item->setCheckable(true);
        item->setAutoExclusive(true);
        item->setFixedHeight(38);
        item->setIconSize(QSize(16, 16));
        side->addWidget(item);
    }

    connect(m_info_nav, &QPushButton::toggled, this, [this](bool sel){ if (sel) switchModule(0); });
    connect(m_mirror_nav, &QPushButton::toggled, this, [this](bool sel){ if (sel) switchModule(1); });
    connect(m_hdc_nav, &QPushButton::toggled, this, [this](bool sel){ if (sel) switchModule(2); });

    side->addStretch();

    // Sidebar Bottom
    auto* bottom_box = new QHBoxLayout();
    bottom_box->setContentsMargins(0, 0, 0, 0);
    bottom_box->setSpacing(6);

    auto* version_label = IconManager::label("HarmonyCast\nv2.0.0", "Muted");
    bottom_box->addWidget(version_label, 1);

    m_settings_btn = IconManager::iconButton("settings", "系统设置", "IconBtnSettings", 30, 30);
    connect(m_settings_btn, &QPushButton::clicked, this, [this] {
        auto* dlg = new QDialog(this);
        dlg->setWindowTitle("系统偏好设置 · HarmonyCast");
        dlg->resize(450, 260);
        auto* l = new QVBoxLayout(dlg);
        l->setSpacing(12);

        auto* cb_auto = new QCheckBox("连接设备后默认自动开启投屏", dlg);
        cb_auto->setChecked(m_auto_start_mirror);
        cb_auto->setStyleSheet("color: #e6edf3; font-size: 13px; font-weight: 500;");
        l->addWidget(cb_auto);

        auto* hint = IconManager::label("开启后，插拔 USB 或通过网络直连设备时，将自动建立连接并启动投屏。", "Muted");
        hint->setWordWrap(true);
        l->addWidget(hint);

        l->addSpacing(8);
        auto* engine_info = IconManager::label("投屏引擎: 高性能 C++20 + Qt6 原生引擎\n画面支持: 硬件加速 (VideoToolbox/D3D11) + 自动快照流降级 (snapshot_display)", "Muted");
        engine_info->setWordWrap(true);
        l->addWidget(engine_info);

        l->addStretch();
        auto* btn_box = new QHBoxLayout();
        btn_box->addStretch();
        auto* btn_ok = IconManager::button("确定", "Primary");
        connect(btn_ok, &QPushButton::clicked, dlg, [this, cb_auto, dlg]() {
            m_auto_start_mirror = cb_auto->isChecked();
            m_settings->setValue("auto_start_mirror", m_auto_start_mirror);
            dlg->accept();
            showToast(QString("设置已保存: %1").arg(m_auto_start_mirror ? "自动开启投屏" : "手动开启投屏"));
        });
        btn_box->addWidget(btn_ok);
        l->addLayout(btn_box);
        dlg->exec();
    });
    bottom_box->addWidget(m_settings_btn, 0, Qt::AlignVCenter);
    side->addLayout(bottom_box);

    horizontal->addWidget(m_sidebar);

    // ==========================================
    // 2. Main Area (主内容区)
    // ==========================================
    auto* main_area = new QWidget(this);
    auto* content = new QVBoxLayout(main_area);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);

    // Header
    m_header = new QFrame(this);
    m_header->setObjectName("Header");
    auto* heading = new QHBoxLayout(m_header);
    heading->setContentsMargins(18, 12, 18, 12);
    heading->setSpacing(8);

    m_page_title = IconManager::label("设备信息", "Title");
    heading->addWidget(m_page_title);
    heading->addStretch();

    // Layout Switcher (左右分屏 / 上下分屏 / 仅投屏 / 仅日志)
    m_layout_switcher = new QWidget(this);
    m_layout_switcher->setObjectName("LayoutSwitcher");
    m_layout_switcher->setFixedHeight(28);
    m_layout_switcher->setStyleSheet(
        "QWidget#LayoutSwitcher { background: #0c1219; border: 1px solid #1c2b3d; border-radius: 6px; padding: 2px; } "
        "QPushButton { background: transparent; border: none; border-radius: 4px; padding: 2px 7px; color: #7d8f9f; font-size: 12px; } "
        "QPushButton:hover { background: #162436; color: #c9d1d9; } "
        "QPushButton:checked { background: #1f3b60; color: #58a6ff; font-weight: bold; }"
    );
    auto* layout_box = new QHBoxLayout(m_layout_switcher);
    layout_box->setContentsMargins(2, 2, 2, 2);
    layout_box->setSpacing(2);

    m_btn_layout_h = new QPushButton("左右");
    m_btn_layout_h->setIcon(IconManager::getIcon("layout_h"));
    m_btn_layout_h->setIconSize(QSize(13, 13));
    m_btn_layout_h->setObjectName("LayoutSwitchBtn");
    m_btn_layout_h->setToolTip("左右分屏 (投屏在左，日志在右，可自由拖拽)");
    m_btn_layout_h->setCheckable(true);
    m_btn_layout_h->setFixedHeight(24);
    m_btn_layout_h->setCursor(Qt::PointingHandCursor);
    connect(m_btn_layout_h, &QPushButton::clicked, this, [this]{ setWorkspaceLayout("horizontal"); });
    layout_box->addWidget(m_btn_layout_h);

    m_btn_layout_v = new QPushButton("上下");
    m_btn_layout_v->setIcon(IconManager::getIcon("layout_v"));
    m_btn_layout_v->setIconSize(QSize(13, 13));
    m_btn_layout_v->setObjectName("LayoutSwitchBtn");
    m_btn_layout_v->setToolTip("上下分屏 (投屏在上，日志在下，可自由拖拽)");
    m_btn_layout_v->setCheckable(true);
    m_btn_layout_v->setFixedHeight(24);
    m_btn_layout_v->setCursor(Qt::PointingHandCursor);
    connect(m_btn_layout_v, &QPushButton::clicked, this, [this]{ setWorkspaceLayout("vertical"); });
    layout_box->addWidget(m_btn_layout_v);

    m_btn_layout_screen = new QPushButton("投屏");
    m_btn_layout_screen->setIcon(IconManager::getIcon("layout_screen"));
    m_btn_layout_screen->setIconSize(QSize(13, 13));
    m_btn_layout_screen->setObjectName("LayoutSwitchBtn");
    m_btn_layout_screen->setToolTip("仅投屏模式");
    m_btn_layout_screen->setCheckable(true);
    m_btn_layout_screen->setFixedHeight(24);
    m_btn_layout_screen->setCursor(Qt::PointingHandCursor);
    connect(m_btn_layout_screen, &QPushButton::clicked, this, [this]{ setWorkspaceLayout("mirror_only"); });
    layout_box->addWidget(m_btn_layout_screen);

    m_btn_layout_logs = new QPushButton("日志");
    m_btn_layout_logs->setIcon(IconManager::getIcon("layout_logs"));
    m_btn_layout_logs->setIconSize(QSize(13, 13));
    m_btn_layout_logs->setObjectName("LayoutSwitchBtn");
    m_btn_layout_logs->setToolTip("仅日志模式");
    m_btn_layout_logs->setCheckable(true);
    m_btn_layout_logs->setFixedHeight(24);
    m_btn_layout_logs->setCursor(Qt::PointingHandCursor);
    connect(m_btn_layout_logs, &QPushButton::clicked, this, [this]{ setWorkspaceLayout("logs_only"); });
    layout_box->addWidget(m_btn_layout_logs);

    heading->addWidget(m_layout_switcher);
    heading->addSpacing(4);

    // Device combo
    m_device_combo = new QComboBox();
    m_device_combo->setMinimumWidth(140);
    m_device_combo->setMaximumWidth(280);
    m_device_combo->setFixedHeight(28);
    IconManager::setupComboBoxPopup(m_device_combo, 180);
    connect(m_device_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onDeviceSelected);
    heading->addWidget(m_device_combo);

    m_refresh_btn = IconManager::button("刷新", "", "refresh");
    m_refresh_btn->setFixedHeight(28);
    m_refresh_btn->setShortcut(QKeySequence("F5"));
    connect(m_refresh_btn, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    heading->addWidget(m_refresh_btn);

    m_disconnect_btn = IconManager::button("断开", "Danger", "disconnect");
    m_disconnect_btn->setFixedHeight(28);
    m_disconnect_btn->setToolTip("断开当前设备连接");
    connect(m_disconnect_btn, &QPushButton::clicked, this, &MainWindow::disconnectCurrentDevice);
    heading->addWidget(m_disconnect_btn);

    m_device_hub_btn = IconManager::button("设备中心", "", "hub");
    m_device_hub_btn->setFixedHeight(28);
    m_device_hub_btn->setToolTip("设备维护中心：查看在线 targets、快速网络连接管理 (固定端口 8710)");
    connect(m_device_hub_btn, &QPushButton::clicked, this, &MainWindow::openDeviceCenter);
    heading->addWidget(m_device_hub_btn);

    content->addWidget(m_header);

    // Pages Stack
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildInfoPanel());      // 0: 设备信息
    m_pages->addWidget(buildWorkspacePage());  // 1: 投屏工作台
    m_pages->addWidget(buildHdcPanel());        // 2: 常用命令
    content->addWidget(m_pages, 1);

    horizontal->addWidget(main_area, 1);

    statusBar()->showMessage("连接设备后查看基本信息；点击「投屏」开始投屏");
    setWorkspaceLayout("horizontal");
    m_info_nav->setChecked(true);
}

void MainWindow::setupShortcuts() {
    auto* f11 = new QShortcut(QKeySequence("F11"), this);
    connect(f11, &QShortcut::activated, this, &MainWindow::toggleFullscreen);

    auto* altLeft = new QShortcut(QKeySequence("Alt+Left"), this);
    connect(altLeft, &QShortcut::activated, this, [this]{ sendKey("back"); });

    auto* altHome = new QShortcut(QKeySequence("Alt+Home"), this);
    connect(altHome, &QShortcut::activated, this, [this]{ sendKey("home"); });

    auto* esc = new QShortcut(QKeySequence("Escape"), this);
    connect(esc, &QShortcut::activated, this, [this] {
        if (isFullScreen()) {
            showNormal();
        } else if (m_projecting) {
            sendKey("back");
        }
    });

    // Font zoom shortcuts: ⌘+, ⌘=, ⌘-, ⌘0
    auto* zoomIn1 = new QShortcut(QKeySequence("Ctrl++"), this);
    connect(zoomIn1, &QShortcut::activated, this, [this]{ setLogFontSize(m_log_font_size_val + 1); });
    auto* zoomIn2 = new QShortcut(QKeySequence("Ctrl+="), this);
    connect(zoomIn2, &QShortcut::activated, this, [this]{ setLogFontSize(m_log_font_size_val + 1); });

    auto* zoomOut = new QShortcut(QKeySequence("Ctrl+-"), this);
    connect(zoomOut, &QShortcut::activated, this, [this]{ setLogFontSize(m_log_font_size_val - 1); });

    auto* zoomReset = new QShortcut(QKeySequence("Ctrl+0"), this);
    connect(zoomReset, &QShortcut::activated, this, [this]{ setLogFontSize(11); });
}

void MainWindow::switchModule(int index) {
    if (!m_pages) return;
    m_pages->setCurrentIndex(index);
    if (index == 0) {
        if (m_page_title) m_page_title->setText("设备信息");
        if (m_layout_switcher) m_layout_switcher->hide();
        if (m_info_nav && !m_info_nav->isChecked()) {
            QSignalBlocker b(m_info_nav);
            m_info_nav->setChecked(true);
        }
        refreshDeviceInfo();
    } else if (index == 1) {
        if (m_page_title) m_page_title->setText("投屏工作台");
        if (m_layout_switcher) m_layout_switcher->show();
        if (m_mirror_nav && !m_mirror_nav->isChecked()) {
            QSignalBlocker b(m_mirror_nav);
            m_mirror_nav->setChecked(true);
        }
        if (!m_projecting && !currentSerial().empty()) {
            startMirror();
        }
    } else if (index == 2) {
        if (m_page_title) m_page_title->setText("常用命令");
        if (m_layout_switcher) m_layout_switcher->hide();
        if (m_hdc_nav && !m_hdc_nav->isChecked()) {
            QSignalBlocker b(m_hdc_nav);
            m_hdc_nav->setChecked(true);
        }
        std::string s = currentSerial();
        if (m_hdc_target_badge) m_hdc_target_badge->setText(s.empty() ? "目标: 未选择设备" : QString("目标: %1").arg(QString::fromStdString(s)));
        if (m_hdc_target_prefix) m_hdc_target_prefix->setText(s.empty() ? "-t <未选>" : QString("-t %1").arg(QString::fromStdString(s)));
    }
}

std::string MainWindow::currentSerial() const {
    if (!m_device_combo || m_device_combo->count() == 0) return {};
    return m_device_combo->currentData().toString().toStdString();
}

void MainWindow::showToast(const QString& msg) {
    statusBar()->showMessage(msg, 3500);
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (m_projecting && m_pages->currentIndex() == 1) {
        if (event->key() == Qt::Key_Back || event->key() == Qt::Key_Backspace) {
            sendKey("back");
            return;
        }
    }
    QMainWindow::keyPressEvent(event);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == m_log_view || watched == m_wv_console_view) && event->type() == QEvent::Wheel) {
        auto* wheelEvent = static_cast<QWheelEvent*>(event);
        if (wheelEvent->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
            int delta = wheelEvent->angleDelta().y();
            if (delta > 0) {
                setLogFontSize(m_log_font_size_val + 1);
            } else if (delta < 0) {
                setLogFontSize(m_log_font_size_val - 1);
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// ==========================================
// Page 0: 设备信息 (Info Panel)
// ==========================================
QWidget* MainWindow::buildInfoPanel() {
    auto* panel = new QWidget();
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(18, 18, 18, 14);
    layout->setSpacing(12);

    auto* top_bar = new QHBoxLayout();
    m_info_badge = IconManager::label("未连接", "Badge");
    top_bar->addWidget(m_info_badge);

    m_info_device_label = IconManager::label("请选择设备以查看详情", "Muted");
    top_bar->addWidget(m_info_device_label);
    top_bar->addStretch();

    m_btn_refresh_info = IconManager::button("刷新设备信息", "", "refresh");
    connect(m_btn_refresh_info, &QPushButton::clicked, this, &MainWindow::refreshDeviceInfo);
    top_bar->addWidget(m_btn_refresh_info);

    m_btn_copy_all_info = IconManager::button("复制全部信息", "", "export");
    connect(m_btn_copy_all_info, &QPushButton::clicked, this, &MainWindow::copyAllDeviceInfo);
    top_bar->addWidget(m_btn_copy_all_info);

    layout->addLayout(top_bar);

    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* info_content = new QWidget();
    m_info_cards_layout = new QVBoxLayout(info_content);
    m_info_cards_layout->setContentsMargins(0, 0, 8, 0);
    m_info_cards_layout->setSpacing(14);

    // 0. 未连接状态提示卡片
    m_card_empty = new QFrame();
    m_card_empty->setObjectName("InfoCard");
    m_empty_layout = new QVBoxLayout(m_card_empty);
    m_empty_layout->setContentsMargins(20, 20, 20, 20);
    m_empty_layout->setSpacing(14);
    m_info_cards_layout->addWidget(m_card_empty);

    // 1. Yarward 专属应用卡片
    m_card_yarward = new QFrame();
    m_card_yarward->setObjectName("InfoCard");
    m_yarward_layout = new QVBoxLayout(m_card_yarward);
    m_yarward_layout->setContentsMargins(16, 16, 16, 16);
    m_yarward_layout->setSpacing(10);
    m_info_cards_layout->addWidget(m_card_yarward);

    // 2. ICU 硬件与看门狗卡片
    m_card_icu = new QFrame();
    m_card_icu->setObjectName("InfoCard");
    m_icu_layout = new QVBoxLayout(m_card_icu);
    m_icu_layout->setContentsMargins(16, 16, 16, 16);
    m_icu_layout->setSpacing(10);
    m_info_cards_layout->addWidget(m_card_icu);

    // 3. 网络与 MAC 地址卡片
    m_card_network = new QFrame();
    m_card_network->setObjectName("InfoCard");
    m_network_layout = new QVBoxLayout(m_card_network);
    m_network_layout->setContentsMargins(16, 16, 16, 16);
    m_network_layout->setSpacing(10);
    m_info_cards_layout->addWidget(m_card_network);

    // 4. 硬件与系统信息卡片
    m_card_device = new QFrame();
    m_card_device->setObjectName("InfoCard");
    m_device_layout = new QVBoxLayout(m_card_device);
    m_device_layout->setContentsMargins(16, 16, 16, 16);
    m_device_layout->setSpacing(10);
    m_info_cards_layout->addWidget(m_card_device);

    // 5. 全部应用包清单
    m_card_packages = new QFrame();
    m_card_packages->setObjectName("InfoCard");
    m_packages_layout = new QVBoxLayout(m_card_packages);
    m_packages_layout->setContentsMargins(16, 16, 16, 16);
    m_packages_layout->setSpacing(10);
    m_info_cards_layout->addWidget(m_card_packages);

    m_info_cards_layout->addStretch();
    scroll->setWidget(info_content);
    layout->addWidget(scroll, 1);

    renderEmptyInfo();
    return panel;
}

static void clearLayout(QLayout* layout) {
    if (!layout) return;
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        } else if (QLayout* childLayout = item->layout()) {
            clearLayout(childLayout);
        }
        delete item;
    }
}

void MainWindow::renderEmptyInfo(const QString& hint) {
    m_info_badge->setText("未连接设备");
    m_info_badge->setStyleSheet("color: #ff9f9f; background: #2b1419; border: 1px solid #572932; padding: 4px 10px; border-radius: 9px; font-size: 12px;");
    m_info_device_label->setText(hint);
    m_btn_refresh_info->setEnabled(false);
    m_btn_copy_all_info->setEnabled(false);

    clearLayout(m_yarward_layout);
    clearLayout(m_icu_layout);
    clearLayout(m_network_layout);
    clearLayout(m_device_layout);
    clearLayout(m_packages_layout);

    m_card_yarward->hide();
    m_card_icu->hide();
    m_card_network->hide();
    m_card_device->hide();
    m_card_packages->hide();

    m_card_empty->show();
    clearLayout(m_empty_layout);

    auto* hdr = new QVBoxLayout();
    hdr->setSpacing(6);
    auto* title = IconManager::label("未检测到已连接的设备", "CardTitleWarn");
    title->setStyleSheet("font-size: 17px; font-weight: 600; color: #f3c969;");
    auto* sub = IconManager::label("当前没有处于在线调试状态的 OpenHarmony / 鸿蒙设备。请先连接设备以查看硬件详情与应用信息。", "Muted");
    sub->setStyleSheet("font-size: 13px; color: #8ba2b9;");
    hdr->addWidget(title);
    hdr->addWidget(sub);
    m_empty_layout->addLayout(hdr);

    auto* guide_box = new QHBoxLayout();
    guide_box->setSpacing(14);

    // USB Guide
    auto* usb_card = new QFrame();
    usb_card->setObjectName("AppBox");
    auto* u_layout = new QVBoxLayout(usb_card);
    u_layout->setContentsMargins(16, 14, 16, 14);
    u_layout->setSpacing(8);
    auto* u_title = IconManager::label("🔌 USB 有线连接指引", "Brand");
    u_title->setStyleSheet("font-size: 14px; font-weight: bold; color: #79e6c9;");
    u_layout->addWidget(u_title);
    QString u_steps = "1. 使用 USB 数据线连接开发板或真机至电脑\n"
                      "2. 在设备上开启「开发者选项」与「USB 调试」\n"
                      "3. 屏幕弹窗勾选「始终允许此电脑调试」并确认\n"
                      "4. 点击下方「刷新设备列表」按钮或按 F5";
    auto* u_desc = IconManager::label(u_steps, "Muted");
    u_desc->setStyleSheet("line-height: 160%; color: #9bb0c3; font-size: 12px;");
    u_layout->addWidget(u_desc);
    guide_box->addWidget(usb_card, 1);

    // Network Guide
    auto* net_card = new QFrame();
    net_card->setObjectName("AppBox");
    auto* n_layout = new QVBoxLayout(net_card);
    n_layout->setContentsMargins(16, 14, 16, 14);
    n_layout->setSpacing(8);
    auto* n_title = IconManager::label("🌐 网络调试连接指引", "Brand");
    n_title->setStyleSheet("font-size: 14px; font-weight: bold; color: #58a6ff;");
    n_layout->addWidget(n_title);
    QString n_steps = "1. 确保设备与电脑接入同一局域网或直连网口\n"
                      "2. 确认设备的固定 IP 地址（默认调试端口 8710）\n"
                      "3. 点击右上角或下方「设备中心」\n"
                      "4. 输入目标 IP 点击连接即可自动同步";
    auto* n_desc = IconManager::label(n_steps, "Muted");
    n_desc->setStyleSheet("line-height: 160%; color: #9bb0c3; font-size: 12px;");
    n_layout->addWidget(n_desc);
    guide_box->addWidget(net_card, 1);

    m_empty_layout->addLayout(guide_box);

    auto* btn_bar = new QHBoxLayout();
    btn_bar->setSpacing(10);
    auto* btn_refresh = IconManager::button("刷新设备列表 (F5)", "Primary", "refresh");
    btn_refresh->setFixedHeight(30);
    connect(btn_refresh, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    auto* btn_hub = IconManager::button("打开设备中心", "", "hub");
    btn_hub->setFixedHeight(30);
    connect(btn_hub, &QPushButton::clicked, this, &MainWindow::openDeviceCenter);
    btn_bar->addWidget(btn_refresh);
    btn_bar->addWidget(btn_hub);
    btn_bar->addStretch();
    m_empty_layout->addLayout(btn_bar);
}

void MainWindow::refreshDeviceInfo() {
    std::string serial = currentSerial();
    if (serial.empty()) {
        renderEmptyInfo();
        return;
    }

    if (m_info_loading) return;
    m_info_loading = true;
    m_info_badge->setText("正在查询…");
    m_info_badge->setStyleSheet("color: #f3c969; background: #3d2f10; border: 1px solid #735515; padding: 4px 10px; border-radius: 9px; font-size: 12px;");
    m_btn_refresh_info->setEnabled(false);
    showToast("正在获取设备基本信息及应用版本…");

    m_dev_mgr->queryDeviceInfoAsync(serial, [this, serial](DeviceInfo info) {
        m_info_loading = false;
        m_btn_refresh_info->setEnabled(true);
        if (currentSerial() != serial) return;
        m_cached_info = info;
        renderDeviceInfo(info);
        showToast(QString("设备信息已更新 (%1)").arg(QString::fromStdString(serial).right(6)));
    });
}

void MainWindow::renderDeviceInfo(const DeviceInfo& data) {
    m_card_empty->hide();
    m_card_yarward->show();
    m_card_icu->show();
    m_card_network->show();
    m_card_device->show();
    m_card_packages->show();

    m_btn_refresh_info->setEnabled(true);
    m_btn_copy_all_info->setEnabled(true);
    m_info_badge->setStyleSheet("");

    QString model = QString::fromStdString(data.model.empty() ? "未知设备" : data.model);
    QString serial = QString::fromStdString(data.serial);
    m_info_badge->setText(QString("%1 · 已更新").arg(model));
    m_info_device_label->setText(QString("序列号: %1  |  更新于 %2").arg(serial, QDateTime::currentDateTime().toString("HH:mm:ss")));

    // 1. Yarward 专属应用卡片
    clearLayout(m_yarward_layout);
    auto* hdr_box = new QHBoxLayout();
    hdr_box->addWidget(IconManager::label("Yarward 专属应用", "CardTitle"));
    if (!data.yarward_apps.empty()) {
        hdr_box->addWidget(IconManager::label(QString("发现 %1 个应用").arg(data.yarward_apps.size()), "TagSuccess"));
    } else {
        hdr_box->addWidget(IconManager::label("未发现相关应用", "TagMuted"));
    }
    hdr_box->addStretch();
    m_yarward_layout->addLayout(hdr_box);

    if (data.yarward_apps.empty()) {
        m_yarward_layout->addWidget(IconManager::label("未检测到包名包含 'yarward' 的应用包 (例如 com.yarward.*)", "Muted"));
    } else {
        for (const auto& app : data.yarward_apps) {
            auto* app_box = new QFrame();
            app_box->setObjectName("AppBox");
            auto* box_layout = new QVBoxLayout(app_box);
            box_layout->setContentsMargins(14, 12, 14, 12);
            box_layout->setSpacing(8);

            QString bundle = QString::fromStdString(app.bundle);
            QString vName = QString::fromStdString(app.version_name.empty() ? "未知" : app.version_name);
            QString vCode = QString::number(app.version_code);
            QString ability = QString::fromStdString(app.main_ability);

            // Row 1
            auto* r1 = new QHBoxLayout();
            auto* pkg_lbl = IconManager::label(bundle, "Brand");
            pkg_lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            r1->addWidget(pkg_lbl);

            if (app.running) {
                QString pidsStr = QString::fromStdString(app.pids);
                r1->addWidget(IconManager::label(QString("● 运行中 (PID: %1)").arg(pidsStr.isEmpty() ? "活跃" : pidsStr), "TagSuccess"));
            } else {
                r1->addWidget(IconManager::label("○ 未运行", "TagMuted"));
            }
            r1->addStretch();

            auto* btn_start = IconManager::button("启动应用", "");
            btn_start->setFixedHeight(28);
            connect(btn_start, &QPushButton::clicked, this, [this, bundle, ability]{ startYarwardApp(bundle, ability); });

            auto* btn_stop = IconManager::button("停止应用", "Danger");
            btn_stop->setFixedHeight(28);
            connect(btn_stop, &QPushButton::clicked, this, [this, bundle]{ stopYarwardApp(bundle); });

            auto* btn_pull = IconManager::button("拉取日志", "");
            btn_pull->setFixedHeight(28);
            btn_pull->setToolTip("从设备拉取该应用的持久化运行日志及 HiAppEvent 异常事件");
            connect(btn_pull, &QPushButton::clicked, this, [this, bundle]{ fetchAppLogsAction(bundle); });

            r1->addWidget(btn_start);
            r1->addWidget(btn_stop);
            r1->addWidget(btn_pull);
            box_layout->addLayout(r1);

            // Row 2
            auto* r2 = new QHBoxLayout();
            r2->addWidget(IconManager::label("版本名称 (versionName):", "InfoKey"));
            auto* v_lbl = IconManager::label(vName, "AppVersion");
            v_lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            r2->addWidget(v_lbl);

            auto* btn_copy_v = IconManager::button("复制版本号", "");
            btn_copy_v->setFixedHeight(26);
            btn_copy_v->setStyleSheet("padding: 2px 8px; font-size: 11px;");
            connect(btn_copy_v, &QPushButton::clicked, this, [this, vName]{ copyToClipboard(vName, "版本号"); });
            r2->addWidget(btn_copy_v);
            r2->addSpacing(16);

            r2->addWidget(IconManager::label("内部构建号 (versionCode):", "InfoKey"));
            auto* v_code_lbl = IconManager::label(vCode, "InfoValMono");
            v_code_lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            r2->addWidget(v_code_lbl);
            r2->addSpacing(16);

            r2->addWidget(IconManager::label("目标 API:", "InfoKey"));
            auto* api_lbl = IconManager::label(QString::number(app.target_api), "InfoValMono");
            api_lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            r2->addWidget(api_lbl);
            r2->addStretch();
            box_layout->addLayout(r2);

            // Row 3
            auto* r3 = new QHBoxLayout();
            r3->addWidget(IconManager::label(QString("入口 Ability: %1").arg(ability.isEmpty() ? "默认" : ability), "Muted"));
            r3->addSpacing(16);
            r3->addWidget(IconManager::label(QString("更新时间: %1").arg(QString::fromStdString(app.update_time)), "Muted"));
            r3->addSpacing(16);
            r3->addWidget(IconManager::label(QString("安装时间: %1").arg(QString::fromStdString(app.install_time)), "Muted"));
            r3->addStretch();

            auto* btn_copy_detail = IconManager::button("复制应用详情", "");
            btn_copy_detail->setFixedHeight(26);
            btn_copy_detail->setStyleSheet("padding: 2px 8px; font-size: 11px;");
            connect(btn_copy_detail, &QPushButton::clicked, this, [this, bundle, vName, vCode, ability]{
                QString detail = QString("应用包名: %1\n版本名称: %2\n构建号: %3\n入口: %4").arg(bundle, vName, vCode, ability);
                copyToClipboard(detail, "应用详情");
            });
            r3->addWidget(btn_copy_detail);
            box_layout->addLayout(r3);

            m_yarward_layout->addWidget(app_box);
        }
    }

    // 2. ICU 硬件与看门狗卡片
    clearLayout(m_icu_layout);
    auto* icu_hdr = new QHBoxLayout();
    icu_hdr->addWidget(IconManager::label("亚华医疗 ICU 硬件与看门狗管控", "CardTitle"));
    if (!data.icu_available) {
        icu_hdr->addWidget(IconManager::label("通用设备 (未检测到 ICU 节点)", "TagMuted"));
    } else if (data.watchdog_stopped) {
        icu_hdr->addWidget(IconManager::label("看门狗已暂停 (调试防复位)", "TagWarning"));
    } else {
        icu_hdr->addWidget(IconManager::label("看门狗运行中 (超时将自动复位)", "TagSuccess"));
    }
    icu_hdr->addStretch();
    m_icu_layout->addLayout(icu_hdr);

    if (!data.icu_available) {
        m_icu_layout->addWidget(IconManager::label("未在 /sys/class/ICU/version 发现硬件控制节点 (非医疗终端硬件或节点未挂载)", "Muted"));
    } else {
        auto* icu_grid = new QGridLayout();
        icu_grid->setContentsMargins(0, 4, 0, 4);
        icu_grid->setHorizontalSpacing(24);
        icu_grid->setVerticalSpacing(8);

        std::vector<std::pair<QString, QString>> icu_items = {
            {"ICU 硬件型号", QString::fromStdString(data.icu_model.empty() ? "未知" : data.icu_model)},
            {"MCU 固件版本", QString::fromStdString(data.icu_version.empty() ? "未知" : data.icu_version)},
            {"硬件版本", QString::fromStdString(data.hardware_version.empty() ? "未知" : data.hardware_version)},
            {"关机延时", QString("%1 秒").arg(QString::fromStdString(data.power_delay.empty() ? "0" : data.power_delay))},
            {"看门狗状态", QString::fromStdString(data.watchdog_desc.empty() ? "未知" : data.watchdog_desc)},
        };

        for (size_t i = 0; i < icu_items.size(); ++i) {
            int row = static_cast<int>(i / 2);
            int col = static_cast<int>((i % 2) * 2);
            auto* k_lbl = IconManager::label(icu_items[i].first, "InfoKey");
            k_lbl->setFixedWidth(85);
            icu_grid->addWidget(k_lbl, row, col);
            auto* v_lbl = IconManager::label(icu_items[i].second, "InfoVal");
            v_lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            icu_grid->addWidget(v_lbl, row, col + 1);
        }
        m_icu_layout->addLayout(icu_grid);

        auto* icu_actions = new QHBoxLayout();
        auto* btn_stop_wd = IconManager::button("暂停看门狗 (调试防复位)", "", "pause");
        btn_stop_wd->setToolTip("写入 StopWatchDog=1，暂停看门狗计时，防止调试死机重启");
        connect(btn_stop_wd, &QPushButton::clicked, this, [this]{ setWatchdogStatusAction(true); });

        auto* btn_start_wd = IconManager::button("开启看门狗 (启用防护)", "", "play");
        btn_start_wd->setToolTip("写入 StopWatchDog=0，恢复看门狗监控");
        connect(btn_start_wd, &QPushButton::clicked, this, [this]{ setWatchdogStatusAction(false); });

        auto* btn_feed_wd = IconManager::button("手动喂狗一次", "", "bone");
        btn_feed_wd->setToolTip("写入 FeedWatchdog=1，执行单次手动喂狗重置计时器");
        connect(btn_feed_wd, &QPushButton::clicked, this, &MainWindow::feedWatchdogAction);

        auto* btn_refresh_icu = IconManager::button("刷新 ICU 状态", "", "refresh");
        connect(btn_refresh_icu, &QPushButton::clicked, this, &MainWindow::refreshDeviceInfo);

        icu_actions->addWidget(btn_stop_wd);
        icu_actions->addWidget(btn_start_wd);
        icu_actions->addWidget(btn_feed_wd);
        icu_actions->addWidget(btn_refresh_icu);
        icu_actions->addStretch();
        m_icu_layout->addLayout(icu_actions);
    }

    // 3. 网络与 MAC 地址卡片
    clearLayout(m_network_layout);
    auto* net_hdr = new QHBoxLayout();
    net_hdr->addWidget(IconManager::label("网络接口与 MAC 地址", "CardTitle"));
    net_hdr->addWidget(IconManager::label(QString("%1 个接口").arg(data.networks.size()), "Badge"));
    net_hdr->addStretch();
    m_network_layout->addLayout(net_hdr);

    if (data.networks.empty()) {
        m_network_layout->addWidget(IconManager::label("未检测到有效网络接口", "Muted"));
    } else {
        auto* net_grid = new QGridLayout();
        net_grid->setContentsMargins(0, 4, 0, 4);
        net_grid->setHorizontalSpacing(14);
        net_grid->setVerticalSpacing(8);

        for (size_t row = 0; row < data.networks.size(); ++row) {
            const auto& net = data.networks[row];
            QString ifc = QString::fromStdString(net.interface_name);
            QString name = QString::fromStdString(net.name);
            QString mac = QString::fromStdString(net.mac);
            QString ip = QString::fromStdString(net.ip);

            auto* ifc_lbl = IconManager::label(QString("● %1 (%2)").arg(name, ifc), net.connected ? "Brand" : "Muted");
            net_grid->addWidget(ifc_lbl, static_cast<int>(row), 0);

            auto* mac_box = new QHBoxLayout();
            auto* mac_lbl = IconManager::label(QString("MAC:  %1").arg(mac), "InfoValMono");
            mac_lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            mac_box->addWidget(mac_lbl);
            auto* btn_copy_mac = IconManager::button("复制 MAC", "");
            btn_copy_mac->setFixedHeight(24);
            btn_copy_mac->setStyleSheet("padding: 1px 6px; font-size: 11px;");
            connect(btn_copy_mac, &QPushButton::clicked, this, [this, mac]{ copyToClipboard(mac, "MAC 地址"); });
            mac_box->addWidget(btn_copy_mac);
            net_grid->addLayout(mac_box, static_cast<int>(row), 1);

            auto* ip_box = new QHBoxLayout();
            auto* ip_lbl = IconManager::label(QString("IP:  %1").arg(ip.isEmpty() ? "未连接" : ip), net.connected ? "InfoVal" : "Muted");
            ip_lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
            ip_box->addWidget(ip_lbl);
            if (net.connected && !ip.isEmpty()) {
                auto* btn_copy_ip = IconManager::button("复制 IP", "");
                btn_copy_ip->setFixedHeight(24);
                btn_copy_ip->setStyleSheet("padding: 1px 6px; font-size: 11px;");
                connect(btn_copy_ip, &QPushButton::clicked, this, [this, ip]{ copyToClipboard(ip, "IP 地址"); });
                ip_box->addWidget(btn_copy_ip);
            }
            net_grid->addLayout(ip_box, static_cast<int>(row), 2);

            auto* tag_lbl = IconManager::label(net.connected ? "已联网" : "未连接", net.connected ? "TagSuccess" : "TagMuted");
            net_grid->addWidget(tag_lbl, static_cast<int>(row), 3);
        }
        m_network_layout->addLayout(net_grid);
    }

    // 4. 硬件与系统信息卡片
    clearLayout(m_device_layout);
    auto* dev_hdr = new QHBoxLayout();
    dev_hdr->addWidget(IconManager::label("设备基本硬件与系统信息", "CardTitle"));
    dev_hdr->addStretch();
    m_device_layout->addLayout(dev_hdr);

    auto* grid = new QGridLayout();
    grid->setContentsMargins(0, 4, 0, 4);
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(8);

    std::vector<std::pair<QString, QString>> sys_items = {
        {"品牌厂商", QString::fromStdString(data.brand.empty() ? data.manufacturer : data.brand)},
        {"产品型号", QString::fromStdString(data.model)},
        {"硬件版本", QString::fromStdString(data.hardware_version.empty() ? "未知" : data.hardware_version)},
        {"软件版本", QString::fromStdString(data.software_version.empty() ? "未知" : data.software_version)},
        {"操作系统", QString::fromStdString(data.os_fullname.empty() ? "OpenHarmony" : data.os_fullname)},
        {"API 版本", QString("API %1").arg(QString::fromStdString(data.api_version.empty() ? "未知" : data.api_version))},
        {"CPU 架构", QString::fromStdString(data.cpu_abi.empty() ? "未知" : data.cpu_abi)},
        {"安全补丁", QString::fromStdString(data.security_patch.empty() ? "未知" : data.security_patch)},
        {"Linux 内核", QString::fromStdString(data.kernel.empty() ? "未知" : data.kernel)},
        {"屏幕分辨率", QString::fromStdString(data.display.empty() ? "未知" : data.display)},
        {"开机时长", QString::fromStdString(data.uptime.empty() ? "未知" : data.uptime)},
        {"内存使用", QString::fromStdString(data.memory.empty() ? "未知" : data.memory)},
        {"内部存储", QString::fromStdString(data.storage.empty() ? "未知" : data.storage)},
    };

    int row = 0;
    for (size_t i = 0; i < sys_items.size(); ++i) {
        int col = static_cast<int>((i % 2) * 2);
        row = static_cast<int>(i / 2);
        auto* k_lbl = IconManager::label(sys_items[i].first, "InfoKey");
        k_lbl->setFixedWidth(80);
        grid->addWidget(k_lbl, row, col);
        auto* v_lbl = IconManager::label(sys_items[i].second, "InfoVal");
        v_lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(v_lbl, row, col + 1);
    }

    QString udid = QString::fromStdString(data.udid.empty() ? "未知" : data.udid);
    row++;
    auto* k_udid = IconManager::label("设备 UDID", "InfoKey");
    k_udid->setFixedWidth(80);
    grid->addWidget(k_udid, row, 0);

    auto* udid_box = new QHBoxLayout();
    auto* v_udid = IconManager::label(udid, "InfoValMono");
    v_udid->setTextInteractionFlags(Qt::TextSelectableByMouse);
    udid_box->addWidget(v_udid);
    if (!udid.isEmpty() && udid != "未知") {
        auto* btn_copy_udid = IconManager::button("复制 UDID", "");
        btn_copy_udid->setFixedHeight(24);
        btn_copy_udid->setStyleSheet("padding: 1px 6px; font-size: 11px;");
        connect(btn_copy_udid, &QPushButton::clicked, this, [this, udid]{ copyToClipboard(udid, "设备 UDID"); });
        udid_box->addWidget(btn_copy_udid);
    }
    udid_box->addStretch();
    grid->addLayout(udid_box, row, 1, 1, 3);
    m_device_layout->addLayout(grid);

    // 5. 全部应用包清单
    clearLayout(m_packages_layout);
    auto* pkg_hdr = new QHBoxLayout();
    pkg_hdr->addWidget(IconManager::label(QString("已安装应用清单 (%1 个)").arg(data.all_packages.size()), "CardTitle"));
    pkg_hdr->addStretch();

    m_pkg_search = new QLineEdit();
    m_pkg_search->setPlaceholderText("过滤应用包名…");
    m_pkg_search->setFixedWidth(240);
    connect(m_pkg_search, &QLineEdit::textChanged, this, &MainWindow::filterPackageTable);
    pkg_hdr->addWidget(m_pkg_search);
    m_packages_layout->addLayout(pkg_hdr);

    m_pkg_table = new QTableWidget();
    m_pkg_table->setColumnCount(3);
    m_pkg_table->setHorizontalHeaderLabels({"序号", "应用包名", "操作"});
    m_pkg_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_pkg_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_pkg_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_pkg_table->verticalHeader()->setVisible(false);
    m_pkg_table->setFixedHeight(280);
    m_pkg_table->setRowCount(static_cast<int>(data.all_packages.size()));

    for (size_t idx = 0; idx < data.all_packages.size(); ++idx) {
        QString pkg = QString::fromStdString(data.all_packages[idx]);
        auto* item_num = new QTableWidgetItem(QString::number(idx + 1));
        item_num->setTextAlignment(Qt::AlignCenter);
        m_pkg_table->setItem(static_cast<int>(idx), 0, item_num);

        auto* item_name = new QTableWidgetItem(pkg);
        if (pkg.contains("yarward", Qt::CaseInsensitive)) {
            item_name->setForeground(QColor("#5ce0c1"));
            QFont f = item_name->font();
            f.setBold(true);
            item_name->setFont(f);
        }
        m_pkg_table->setItem(static_cast<int>(idx), 1, item_name);

        auto* btn_copy = IconManager::button("复制", "");
        btn_copy->setFixedHeight(22);
        btn_copy->setStyleSheet("padding: 0 6px; font-size: 11px;");
        connect(btn_copy, &QPushButton::clicked, this, [this, pkg]{ copyToClipboard(pkg, "包名"); });
        m_pkg_table->setCellWidget(static_cast<int>(idx), 2, btn_copy);
    }
    m_packages_layout->addWidget(m_pkg_table);

    // Sync packages to HiLog package filter combo
    if (m_package_combo) {
        QString curPkg = m_package_combo->currentText().trimmed();
        m_package_combo->blockSignals(true);
        m_package_combo->clear();
        m_package_combo->addItem(DEFAULT_PACKAGE_FILTER);
        for (const auto& pkg : data.all_packages) {
            QString qpkg = QString::fromStdString(pkg);
            if (qpkg.contains("yarward", Qt::CaseInsensitive)) {
                m_package_combo->addItem(qpkg);
            }
        }
        m_package_combo->addItem("全部应用");
        for (const auto& pkg : data.all_packages) {
            QString qpkg = QString::fromStdString(pkg);
            if (!qpkg.contains("yarward", Qt::CaseInsensitive)) {
                m_package_combo->addItem(qpkg);
            }
        }
        int idx = m_package_combo->findText(curPkg);
        if (idx >= 0) {
            m_package_combo->setCurrentIndex(idx);
        } else {
            m_package_combo->setCurrentIndex(0);
        }
        m_package_combo->blockSignals(false);
    }
}

void MainWindow::filterPackageTable(const QString& query) {
    if (!m_pkg_table) return;
    QString q = query.trimmed().toLower();
    for (int row = 0; row < m_pkg_table->rowCount(); ++row) {
        auto* item = m_pkg_table->item(row, 1);
        if (!item) continue;
        bool match = q.isEmpty() || item->text().toLower().contains(q);
        m_pkg_table->setRowHidden(row, !match);
    }
}

void MainWindow::copyAllDeviceInfo() {
    if (m_cached_info.serial.empty()) {
        showToast("无可用设备信息可复制");
        return;
    }
    const auto& d = m_cached_info;
    QStringList lines;
    lines << QString("# 设备基本信息 (%1)").arg(QString::fromStdString(d.serial))
          << QString("- 品牌厂商: %1").arg(QString::fromStdString(d.brand))
          << QString("- 产品型号: %1").arg(QString::fromStdString(d.model))
          << QString("- 硬件版本: %1").arg(QString::fromStdString(d.hardware_version))
          << QString("- 软件版本: %1").arg(QString::fromStdString(d.software_version))
          << QString("- 操作系统: %1 (API %2)").arg(QString::fromStdString(d.os_fullname), QString::fromStdString(d.api_version))
          << QString("- CPU 架构: %1").arg(QString::fromStdString(d.cpu_abi))
          << QString("- 设备 UDID: %1").arg(QString::fromStdString(d.udid))
          << QString("- 屏幕分辨率: %1").arg(QString::fromStdString(d.display))
          << QString("- 运行时间: %1").arg(QString::fromStdString(d.uptime))
          << QString("- 内存使用: %1").arg(QString::fromStdString(d.memory))
          << QString("- 存储空间: %1").arg(QString::fromStdString(d.storage))
          << "";

    if (d.icu_available) {
        lines << "## ICU 硬件与看门狗"
              << QString("- ICU 模块型号: %1").arg(QString::fromStdString(d.icu_model))
              << QString("- MCU 固件版本: %1").arg(QString::fromStdString(d.icu_version))
              << QString("- 看门狗状态: %1").arg(QString::fromStdString(d.watchdog_desc))
              << QString("- 关机延时: %1").arg(QString::fromStdString(d.power_delay))
              << "";
    }

    lines << "## 网络接口与 MAC 地址";
    for (const auto& net : d.networks) {
        lines << QString("- %1 (%2): MAC=%3, IP=%4")
                     .arg(QString::fromStdString(net.name), QString::fromStdString(net.interface_name),
                          QString::fromStdString(net.mac), QString::fromStdString(net.ip));
    }
    lines << "" << QString("已安装全部应用数: %1").arg(d.all_packages.size());

    copyToClipboard(lines.join("\n"), "全部设备信息");
}

void MainWindow::copyToClipboard(const QString& text, const QString& hint) {
    QApplication::clipboard()->setText(text);
    showToast(QString("已复制到剪贴板: %1").arg(hint));
}

void MainWindow::startYarwardApp(const QString& bundle, const QString& ability) {
    std::string s = currentSerial();
    if (s.empty()) return;
    showToast(QString("正在启动应用 %1…").arg(bundle));
    m_hdc->executeAsync({"shell", "aa", "start", "-b", bundle.toStdString(), "-a", ability.isEmpty() ? "EntryAbility" : ability.toStdString()}, s, [this, bundle](const CommandResult& r){
        if (r.success()) {
            showToast(QString("应用已启动: %1").arg(bundle));
            QTimer::singleShot(1000, this, &MainWindow::refreshDeviceInfo);
        } else {
            showToast(QString("启动应用失败: %1").arg(QString::fromStdString(r.err.empty() ? r.out : r.err)));
        }
    });
}

void MainWindow::stopYarwardApp(const QString& bundle) {
    std::string s = currentSerial();
    if (s.empty()) return;
    showToast(QString("正在停止应用 %1…").arg(bundle));
    m_hdc->executeAsync({"shell", "aa", "force-stop", bundle.toStdString()}, s, [this, bundle](const CommandResult& r){
        if (r.success()) {
            showToast(QString("应用已停止: %1").arg(bundle));
            QTimer::singleShot(500, this, &MainWindow::refreshDeviceInfo);
        } else {
            showToast(QString("停止应用失败: %1").arg(QString::fromStdString(r.err.empty() ? r.out : r.err)));
        }
    });
}

void MainWindow::fetchAppLogsAction(const QString& bundle) {
    std::string s = currentSerial();
    if (s.empty()) {
        showToast("请先选择已连接的设备");
        return;
    }

    QString realBundle = bundle;
    if (realBundle.contains("*") || realBundle.isEmpty()) {
        if (!m_cached_info.yarward_apps.empty()) {
            realBundle = QString::fromStdString(m_cached_info.yarward_apps[0].bundle);
        } else {
            for (const auto& pkg : m_cached_info.all_packages) {
                QString qpkg = QString::fromStdString(pkg);
                if (qpkg.startsWith("com.yarward.")) {
                    realBundle = qpkg;
                    break;
                }
            }
        }
    }

    if (realBundle.contains("*") || realBundle.isEmpty()) {
        bool ok = false;
        realBundle = QInputDialog::getText(this, "拉取应用日志", "请输入目标应用真实包名:", QLineEdit::Normal, "com.yarward.etnroomoh", &ok);
        if (!ok || realBundle.isEmpty()) return;
    }

    QString destDir = QFileDialog::getExistingDirectory(this, "选择保存应用日志的本地目录", QDir::homePath() + "/Downloads");
    if (destDir.isEmpty()) return;

    showToast(QString("正在拉取应用数据与日志: %1…").arg(realBundle));
    std::string remote_dir = "/data/app/el2/100/base/" + realBundle.toStdString() + "/";
    m_hdc->executeAsync({"file", "recv", remote_dir, destDir.toStdString()}, s, [this, destDir, realBundle](const CommandResult& r){
        if (r.success()) {
            int ret = QMessageBox::information(this, "应用日志拉取成功", QString("应用 [%1] 的本地数据与日志已保存到:\n%2\n\n是否打开所在目录？").arg(realBundle, destDir), QMessageBox::Open | QMessageBox::Ok);
            if (ret == QMessageBox::Open) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(destDir));
            }
        } else {
            showToast("拉取日志失败或设备端该应用尚未生成持久化日志");
        }
    });
}

void MainWindow::setWatchdogStatusAction(bool stop) {
    std::string s = currentSerial();
    if (s.empty()) return;
    QString cmd = stop ? "echo -n 1 > /sys/class/ICU/version/StopWatchDog" : "echo -n 0 > /sys/class/ICU/version/StopWatchDog";
    m_hdc->executeAsync({"shell", cmd.toStdString()}, s, [this, stop](const CommandResult& r){
        if (r.success()) {
            showToast(stop ? "已暂停看门狗 (调试防复位)" : "已恢复看门狗监控");
            refreshDeviceInfo();
        } else {
            showToast("设置看门狗失败");
        }
    });
}

void MainWindow::feedWatchdogAction() {
    std::string s = currentSerial();
    if (s.empty()) return;
    m_hdc->executeAsync({"shell", "echo -n 1 > /sys/class/ICU/version/FeedWatchdog"}, s, [this](const CommandResult& r){
        if (r.success()) {
            showToast("看门狗手动喂狗成功 (FeedWatchdog=1)");
        } else {
            showToast("手动喂狗失败");
        }
    });
}

// ==========================================
// Page 1: 投屏工作台 (Workspace Page)
// ==========================================
QWidget* MainWindow::buildWorkspacePage() {
    auto* page = new QWidget();
    auto* ws_layout = new QVBoxLayout(page);
    ws_layout->setContentsMargins(0, 0, 0, 0);
    ws_layout->setSpacing(0);

    m_workspace_splitter = new QSplitter(Qt::Horizontal, page);
    m_workspace_splitter->setObjectName("WorkspaceSplitter");
    m_workspace_splitter->setHandleWidth(8);
    m_workspace_splitter->setChildrenCollapsible(false);

    // Left: Screen Mirroring Area
    m_mirror_page = new QWidget();
    m_mirror_page->setMinimumSize(80, 80);
    auto* mirror_layout = new QVBoxLayout(m_mirror_page);
    mirror_layout->setContentsMargins(0, 0, 0, 0);
    mirror_layout->setSpacing(0);

    // Toolbar (FlowLayout)
    m_mirror_toolbar = new QFrame();
    m_mirror_toolbar->setObjectName("MirrorToolbar");
    m_mirror_toolbar->setMinimumHeight(40);
    auto* controls = new FlowLayout(m_mirror_toolbar, 6, 5, 5);

    m_start_btn = IconManager::button("开始投屏", "Primary", "play");
    m_start_btn->setFixedHeight(28);
    m_start_btn->setShortcut(QKeySequence("Ctrl+Return"));
    connect(m_start_btn, &QPushButton::clicked, this, &MainWindow::startMirror);

    m_stop_btn = IconManager::button("停止投屏", "Danger", "stop");
    m_stop_btn->setFixedHeight(28);
    m_stop_btn->hide();
    connect(m_stop_btn, &QPushButton::clicked, this, [this]{ stopMirror(true); });

    controls->addWidget(m_start_btn);
    controls->addWidget(m_stop_btn);

    m_quality_combo = new QComboBox();
    m_quality_combo->addItems({"标准 (4M)", "流畅 (1.5M)", "超清 (8M)"});
    m_quality_combo->setToolTip("投屏画质：1080P 完整全屏，支持动态调节视频流码率");
    m_quality_combo->setFixedHeight(28);
    m_quality_combo->setFixedWidth(112);
    IconManager::setupComboBoxPopup(m_quality_combo, 112);
    connect(m_quality_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onQualityChanged);
    controls->addWidget(m_quality_combo);

    m_rotate_btn = IconManager::button("旋转", "", "rotate");
    m_rotate_btn->setFixedHeight(28);
    m_rotate_btn->setShortcut(QKeySequence("Ctrl+R"));
    m_rotate_btn->setToolTip("旋转画面 0° / 90° / 180° / 270° (快捷键: ⌘R)");
    connect(m_rotate_btn, &QPushButton::clicked, this, &MainWindow::rotateDisplay);
    controls->addWidget(m_rotate_btn);

    m_screenshot_btn = IconManager::button("截图", "", "camera");
    m_screenshot_btn->setFixedHeight(28);
    m_screenshot_btn->setShortcut(QKeySequence("Ctrl+Shift+S"));
    m_screenshot_btn->setToolTip("保存高清截图 (快捷键: ⌘⇧S)");
    connect(m_screenshot_btn, &QPushButton::clicked, this, &MainWindow::saveScreenshot);
    controls->addWidget(m_screenshot_btn);

    m_btn_key_back = IconManager::button("返回", "", "back");
    m_btn_key_back->setFixedHeight(28);
    m_btn_key_back->setToolTip("返回键 (Back, 快捷键: Alt+Left)");
    connect(m_btn_key_back, &QPushButton::clicked, this, [this]{ sendKey("back"); });
    controls->addWidget(m_btn_key_back);

    m_btn_key_home = IconManager::button("主页", "", "home");
    m_btn_key_home->setFixedHeight(28);
    m_btn_key_home->setToolTip("回到主屏幕 (Home, 快捷键: Alt+Home)");
    connect(m_btn_key_home, &QPushButton::clicked, this, [this]{ sendKey("home"); });
    controls->addWidget(m_btn_key_home);

    m_restore_primary_btn = IconManager::button("主程序", "", "app");
    m_restore_primary_btn->setFixedHeight(28);
    m_restore_primary_btn->setToolTip("拉起业务主程序");
    connect(m_restore_primary_btn, &QPushButton::clicked, this, &MainWindow::launchPrimaryAppShortcut);
    controls->addWidget(m_restore_primary_btn);

    m_fault_report_btn = IconManager::button("打包诊断", "", "package");
    m_fault_report_btn->setFixedHeight(28);
    m_fault_report_btn->setToolTip("打包导出诊断现场数据包");
    connect(m_fault_report_btn, &QPushButton::clicked, this, &MainWindow::exportFaultReport);
    controls->addWidget(m_fault_report_btn);

    m_fullscreen_btn = IconManager::button("全屏", "", "fullscreen");
    m_fullscreen_btn->setFixedHeight(28);
    m_fullscreen_btn->setToolTip("全屏演示模式 (F11)");
    connect(m_fullscreen_btn, &QPushButton::clicked, this, &MainWindow::toggleFullscreen);
    controls->addWidget(m_fullscreen_btn);

    m_state_badge = IconManager::label("待连接", "Muted");
    m_state_badge->setFixedHeight(28);
    m_state_badge->setAlignment(Qt::AlignCenter);
    controls->addWidget(m_state_badge);

    mirror_layout->addWidget(m_mirror_toolbar);

    // Screen
    m_screen = new ScreenView(m_mirror_page);
    connect(m_screen, &ScreenView::startRequested, this, &MainWindow::startMirror);
    mirror_layout->addWidget(m_screen, 1);

    m_workspace_splitter->addWidget(m_mirror_page);

    // Right: Log Panel (HiLog + WebView)
    m_log_panel = new QWidget();
    m_log_panel->setObjectName("LogPanel");
    m_log_panel->setMinimumSize(80, 80);
    auto* log_panel_layout = new QVBoxLayout(m_log_panel);
    log_panel_layout->setContentsMargins(6, 6, 6, 6);
    log_panel_layout->setSpacing(4);

    auto* log_tab_bar = new QHBoxLayout();
    log_tab_bar->setContentsMargins(0, 0, 0, 0);
    log_tab_bar->setSpacing(4);

    m_btn_log_tab_device = new QPushButton("设备日志");
    m_btn_log_tab_device->setIcon(IconManager::getIcon("logs"));
    m_btn_log_tab_device->setIconSize(QSize(14, 14));
    m_btn_log_tab_device->setObjectName("LogSubTab");
    m_btn_log_tab_device->setCheckable(true);
    m_btn_log_tab_device->setChecked(true);
    m_btn_log_tab_device->setFixedHeight(28);
    m_btn_log_tab_device->setCursor(Qt::PointingHandCursor);
    connect(m_btn_log_tab_device, &QPushButton::clicked, this, [this]{ switchLogSubtab(0); });
    log_tab_bar->addWidget(m_btn_log_tab_device);

    m_btn_log_tab_webview = new QPushButton("WebView 控制台");
    m_btn_log_tab_webview->setIcon(IconManager::getIcon("web"));
    m_btn_log_tab_webview->setIconSize(QSize(14, 14));
    m_btn_log_tab_webview->setObjectName("LogSubTab");
    m_btn_log_tab_webview->setCheckable(true);
    m_btn_log_tab_webview->setFixedHeight(28);
    m_btn_log_tab_webview->setCursor(Qt::PointingHandCursor);
    connect(m_btn_log_tab_webview, &QPushButton::clicked, this, [this]{ switchLogSubtab(1); });
    log_tab_bar->addWidget(m_btn_log_tab_webview);

    log_tab_bar->addStretch(1);
    log_panel_layout->addLayout(log_tab_bar);

    m_log_subtab_stack = new QStackedWidget();
    m_log_subtab_stack->addWidget(buildHiLogWidget());
    m_log_subtab_stack->addWidget(buildWebViewPanel());
    log_panel_layout->addWidget(m_log_subtab_stack, 1);

    m_workspace_splitter->addWidget(m_log_panel);
    m_workspace_splitter->setStretchFactor(0, 5);
    m_workspace_splitter->setStretchFactor(1, 5);

    ws_layout->addWidget(m_workspace_splitter, 1);
    return page;
}

QWidget* MainWindow::buildHiLogWidget() {
    auto* widget = new QWidget();
    auto* log_layout = new QVBoxLayout(widget);
    log_layout->setContentsMargins(0, 2, 0, 2);
    log_layout->setSpacing(6);

    // Toolbar Container: 2-row structured layout with clear hierarchy & generous spacing
    auto* toolbar_container = new QWidget();
    auto* tb_vlayout = new QVBoxLayout(toolbar_container);
    tb_vlayout->setContentsMargins(0, 2, 0, 4);
    tb_vlayout->setSpacing(6);

    // Row 1: Filters & Search (Data Querying)
    auto* row1 = new QHBoxLayout();
    row1->setContentsMargins(0, 0, 0, 0);
    row1->setSpacing(6);

    // 1. Package combo
    m_package_combo = new QComboBox();
    m_package_combo->setEditable(true);
    m_package_combo->addItems({DEFAULT_PACKAGE_FILTER, "全部应用"});
    m_package_combo->setFixedWidth(135);
    m_package_combo->setFixedHeight(28);
    IconManager::setupComboBoxPopup(m_package_combo, 260);
    connect(m_package_combo, &QComboBox::currentTextChanged, this, &MainWindow::renderHiLog);
    row1->addWidget(m_package_combo);

    // 2. Level combo
    m_level_combo = new QComboBox();
    m_level_combo->addItems({"全部级别", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"});
    m_level_combo->setFixedHeight(28);
    m_level_combo->setFixedWidth(96);
    IconManager::setupComboBoxPopup(m_level_combo, 120);
    connect(m_level_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::renderHiLog);
    row1->addWidget(m_level_combo);

    // 3. Search lineEdit
    m_search_input = new QLineEdit();
    m_search_input->setPlaceholderText("搜索日志内容、Tag 或 PID…");
    m_search_input->addAction(IconManager::getIcon("search"), QLineEdit::LeadingPosition);
    m_search_input->setClearButtonEnabled(true);
    m_search_input->setMinimumWidth(100);
    m_search_input->setFixedHeight(28);
    connect(m_search_input, &QLineEdit::textChanged, this, &MainWindow::renderHiLog);
    row1->addWidget(m_search_input, 1);

    tb_vlayout->addLayout(row1);

    // Row 2: Stream Controls & Action Buttons (Generous spacing & breathing room)
    auto* row2 = new QHBoxLayout();
    row2->setContentsMargins(0, 0, 0, 0);
    row2->setSpacing(14);

    m_autoscroll_cb = new QCheckBox("自动滚动");
    m_autoscroll_cb->setChecked(true);
    m_autoscroll_cb->setToolTip("新日志到达时自动滚动到底部");
    m_autoscroll_cb->setFixedHeight(26);
    m_autoscroll_cb->setCursor(Qt::PointingHandCursor);

    m_pause_cb = new QCheckBox("暂停刷新");
    m_pause_cb->setToolTip("暂停日志刷新显示");
    m_pause_cb->setFixedHeight(26);
    m_pause_cb->setCursor(Qt::PointingHandCursor);

    row2->addWidget(m_autoscroll_cb);
    row2->addWidget(m_pause_cb);

    row2->addStretch(1);

    m_clear_log_btn = IconManager::button("清空", "", "clear");
    m_clear_log_btn->setFixedHeight(26);
    m_clear_log_btn->setToolTip("清空当前所有日志缓存");
    connect(m_clear_log_btn, &QPushButton::clicked, this, &MainWindow::clearHiLog);

    m_export_log_btn = IconManager::button("导出日志", "", "export");
    m_export_log_btn->setFixedHeight(26);
    m_export_log_btn->setToolTip("导出当前日志到本地文本文件");
    connect(m_export_log_btn, &QPushButton::clicked, this, &MainWindow::exportHiLog);

    row2->addWidget(m_clear_log_btn);
    row2->addWidget(m_export_log_btn);

    tb_vlayout->addLayout(row2);
    log_layout->addWidget(toolbar_container);

    // Log View
    m_log_view = new QPlainTextEdit();
    m_log_view->setReadOnly(true);
    m_log_view->setFont(QFont("Menlo", 11));
    m_log_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log_view->setMaximumBlockCount(10000);
    m_log_view->installEventFilter(this);
    log_layout->addWidget(m_log_view, 1);

    // Footer
    auto* log_footer = new QHBoxLayout();
    log_footer->setContentsMargins(0, 0, 0, 0);
    log_footer->setSpacing(8);

    m_log_count = IconManager::label("0 条日志", "Muted");
    log_footer->addWidget(m_log_count);
    log_footer->addStretch(1);

    auto* font_box = new QHBoxLayout();
    font_box->setContentsMargins(0, 0, 0, 0);
    font_box->setSpacing(4);

    auto* font_lbl = IconManager::label("字号", "Muted");
    font_lbl->setToolTip("快捷键: ⌘ + 放大 · ⌘ − 缩小 · ⌘ 0 恢复默认字号 (支持 ⌘ + 滚轮缩放)");
    font_box->addWidget(font_lbl);

    auto* btn_zoom_out = new QPushButton("−");
    btn_zoom_out->setFixedSize(26, 26);
    btn_zoom_out->setToolTip("缩小字体 (快捷键: ⌘ −)");
    btn_zoom_out->setCursor(Qt::PointingHandCursor);
    connect(btn_zoom_out, &QPushButton::clicked, this, [this]{ setLogFontSize(m_log_font_size_val - 1); });
    font_box->addWidget(btn_zoom_out);

    m_log_font_size = new QSpinBox();
    m_log_font_size->setRange(8, 32);
    m_log_font_size->setSuffix(" pt");
    m_log_font_size->setFixedWidth(78);
    m_log_font_size->setFixedHeight(26);
    m_log_font_size->setAlignment(Qt::AlignCenter);
    m_log_font_size->setValue(11);
    connect(m_log_font_size, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::setLogFontSize);
    font_box->addWidget(m_log_font_size);

    auto* btn_zoom_in = new QPushButton("+");
    btn_zoom_in->setFixedSize(26, 26);
    btn_zoom_in->setToolTip("放大字体 (快捷键: ⌘ +)");
    btn_zoom_in->setCursor(Qt::PointingHandCursor);
    connect(btn_zoom_in, &QPushButton::clicked, this, [this]{ setLogFontSize(m_log_font_size_val + 1); });
    font_box->addWidget(btn_zoom_in);

    auto* btn_reset_font = new QPushButton("默认");
    btn_reset_font->setFixedHeight(26);
    btn_reset_font->setToolTip("恢复默认字号 11 pt (快捷键: ⌘ 0)");
    btn_reset_font->setCursor(Qt::PointingHandCursor);
    connect(btn_reset_font, &QPushButton::clicked, this, [this]{ setLogFontSize(11); });
    font_box->addWidget(btn_reset_font);

    log_footer->addLayout(font_box);

    log_layout->addLayout(log_footer);
    return widget;
}

QWidget* MainWindow::buildWebViewPanel() {
    auto* panel = new QWidget();
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(6);

    // Toolbar Container: 2-row clean structured layout
    auto* wv_toolbar_container = new QWidget();
    auto* wv_tb_vlayout = new QVBoxLayout(wv_toolbar_container);
    wv_tb_vlayout->setContentsMargins(0, 2, 0, 4);
    wv_tb_vlayout->setSpacing(6);

    // Row 1: Subtabs & Search
    auto* wv_row1 = new QHBoxLayout();
    wv_row1->setContentsMargins(0, 0, 0, 0);
    wv_row1->setSpacing(6);

    m_btn_subtab_console = new QPushButton("控制台");
    m_btn_subtab_console->setObjectName("Primary");
    m_btn_subtab_console->setFixedHeight(28);
    m_btn_subtab_console->setCursor(Qt::PointingHandCursor);
    connect(m_btn_subtab_console, &QPushButton::clicked, this, [this]{ setWebViewSubtab(0); });
    wv_row1->addWidget(m_btn_subtab_console);

    m_btn_subtab_network = new QPushButton("接口网络");
    m_btn_subtab_network->setFixedHeight(28);
    m_btn_subtab_network->setCursor(Qt::PointingHandCursor);
    connect(m_btn_subtab_network, &QPushButton::clicked, this, [this]{ setWebViewSubtab(1); });
    wv_row1->addWidget(m_btn_subtab_network);

    m_wv_filter_combo = new QComboBox();
    m_wv_filter_combo->addItems({"全部级别", "日志", "警告", "错误"});
    m_wv_filter_combo->setFixedHeight(28);
    m_wv_filter_combo->setFixedWidth(100);
    IconManager::setupComboBoxPopup(m_wv_filter_combo, 112);
    wv_row1->addWidget(m_wv_filter_combo);

    m_wv_filter_input = new QLineEdit();
    m_wv_filter_input->setPlaceholderText("过滤日志内容或请求 URL…");
    m_wv_filter_input->addAction(IconManager::getIcon("search"), QLineEdit::LeadingPosition);
    m_wv_filter_input->setFixedHeight(28);
    m_wv_filter_input->setClearButtonEnabled(true);
    wv_row1->addWidget(m_wv_filter_input, 1);

    wv_tb_vlayout->addLayout(wv_row1);

    // Row 2: Controls & Actions (Generous spacing & breathing room)
    auto* wv_row2 = new QHBoxLayout();
    wv_row2->setContentsMargins(0, 0, 0, 0);
    wv_row2->setSpacing(14);

    m_wv_autoscroll = new QCheckBox("自动滚动");
    m_wv_autoscroll->setChecked(true);
    m_wv_autoscroll->setFixedHeight(26);
    m_wv_autoscroll->setCursor(Qt::PointingHandCursor);

    m_wv_pause = new QCheckBox("暂停刷新");
    m_wv_pause->setFixedHeight(26);
    m_wv_pause->setCursor(Qt::PointingHandCursor);

    wv_row2->addWidget(m_wv_autoscroll);
    wv_row2->addWidget(m_wv_pause);

    wv_row2->addStretch(1);

    m_wv_clear_btn = IconManager::button("清空", "", "clear");
    m_wv_clear_btn->setFixedHeight(26);
    connect(m_wv_clear_btn, &QPushButton::clicked, this, &MainWindow::clearWebViewConsole);

    m_wv_export_btn = IconManager::button("导出日志", "", "export");
    m_wv_export_btn->setFixedHeight(26);
    connect(m_wv_export_btn, &QPushButton::clicked, this, [this]{
        QString content = m_wv_console_view->toPlainText();
        QString path = QFileDialog::getSaveFileName(this, "导出 WebView 日志", QDir::homePath() + "/Downloads/webview.log", "Log Files (*.log)");
        if (!path.isEmpty()) {
            QFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                file.write(content.toUtf8());
                showToast("WebView 日志已导出");
            }
        }
    });

    m_wv_chrome_btn = IconManager::button("Chrome DevTools", "", "chrome");
    m_wv_chrome_btn->setFixedHeight(26);
    m_wv_chrome_btn->setToolTip("在系统 Chrome 浏览器中打开 DevTools 调试");
    connect(m_wv_chrome_btn, &QPushButton::clicked, this, &MainWindow::openChromeInspect);

    wv_row2->addWidget(m_wv_clear_btn);
    wv_row2->addWidget(m_wv_export_btn);
    wv_row2->addWidget(m_wv_chrome_btn);

    wv_tb_vlayout->addLayout(wv_row2);
    layout->addWidget(wv_toolbar_container);

    m_wv_right_stack = new QStackedWidget();

    // Subtab 0: Console
    auto* console_page = new QWidget();
    auto* c_layout = new QVBoxLayout(console_page);
    c_layout->setContentsMargins(0, 0, 0, 0);
    c_layout->setSpacing(4);

    m_wv_console_view = new QPlainTextEdit();
    m_wv_console_view->setReadOnly(true);
    m_wv_console_view->setFont(QFont("Menlo", 11));
    m_wv_console_view->setStyleSheet(
        "background: #090e15; color: #c2d1df; border: 1px solid #1e2c3d; border-radius: 7px; padding: 8px; font-family: 'Menlo', monospace;"
    );
    m_wv_console_view->installEventFilter(this);
    c_layout->addWidget(m_wv_console_view, 1);

    auto* js_bar = new QHBoxLayout();
    js_bar->setContentsMargins(0, 0, 0, 0);
    js_bar->setSpacing(6);
    auto* js_prompt = IconManager::label("JS >", "TagSuccess");
    js_prompt->setStyleSheet("color: #53d6b5; font-weight: 700; font-family: 'Menlo'; padding: 2px 4px;");
    js_bar->addWidget(js_prompt);

    m_wv_js_input = new QLineEdit();
    m_wv_js_input->setPlaceholderText("输入 JavaScript 执行 (按 Enter 运行)…");
    m_wv_js_input->setFixedHeight(26);
    connect(m_wv_js_input, &QLineEdit::returnPressed, this, &MainWindow::executeWebViewJs);
    js_bar->addWidget(m_wv_js_input, 1);

    m_btn_wv_eval = IconManager::button("运行", "Primary");
    m_btn_wv_eval->setFixedHeight(26);
    connect(m_btn_wv_eval, &QPushButton::clicked, this, &MainWindow::executeWebViewJs);
    js_bar->addWidget(m_btn_wv_eval);
    c_layout->addLayout(js_bar);
    m_wv_right_stack->addWidget(console_page);

    // Subtab 1: Network
    auto* net_page = new QWidget();
    auto* n_layout = new QVBoxLayout(net_page);
    n_layout->setContentsMargins(0, 0, 0, 0);
    n_layout->setSpacing(4);

    m_wv_network_table = new QTableWidget(0, 5);
    m_wv_network_table->setHorizontalHeaderLabels({"状态", "方式", "接口 / URL", "类型", "耗时"});
    m_wv_network_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_wv_network_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_wv_network_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_wv_network_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_wv_network_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_wv_network_table->setSelectionBehavior(QTableWidget::SelectRows);
    m_wv_network_table->setEditTriggers(QTableWidget::NoEditTriggers);
    m_wv_network_table->setAlternatingRowColors(true);
    m_wv_network_table->verticalHeader()->setVisible(false);
    m_wv_network_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_wv_network_table, &QTableWidget::customContextMenuRequested, this, &MainWindow::onNetworkTableContextMenu);
    m_wv_network_table->setStyleSheet(
        "QTableWidget { background: #090e15; alternate-background-color: #0d141e; color: #c2d1df; border: 1px solid #1e2c3d; border-radius: 7px; font-family: 'Menlo', monospace; font-size: 11px; } "
        "QHeaderView::section { background: #121c29; color: #8ba2b9; padding: 4px; border: none; font-weight: bold; font-size: 11px; }"
    );
    n_layout->addWidget(m_wv_network_table, 1);
    m_wv_right_stack->addWidget(net_page);

    layout->addWidget(m_wv_right_stack, 1);

    // Footer
    auto* net_bottom_bar = new QHBoxLayout();
    net_bottom_bar->setContentsMargins(0, 0, 0, 0);
    net_bottom_bar->setSpacing(8);

    m_wv_net_status_label = IconManager::label("0 条日志 · 0 个请求", "Muted");
    net_bottom_bar->addWidget(m_wv_net_status_label);
    net_bottom_bar->addStretch(1);

    auto* wv_font_box = new QHBoxLayout();
    wv_font_box->setContentsMargins(0, 0, 0, 0);
    wv_font_box->setSpacing(4);

    auto* wv_font_lbl = IconManager::label("字号", "Muted");
    wv_font_lbl->setToolTip("快捷键: ⌘ + 放大 · ⌘ − 缩小 · ⌘ 0 恢复默认字号 (支持 ⌘ + 滚轮缩放)");
    wv_font_box->addWidget(wv_font_lbl);

    auto* btn_wv_zoom_out = new QPushButton("−");
    btn_wv_zoom_out->setFixedSize(26, 26);
    btn_wv_zoom_out->setToolTip("缩小字体 (快捷键: ⌘ −)");
    btn_wv_zoom_out->setCursor(Qt::PointingHandCursor);
    connect(btn_wv_zoom_out, &QPushButton::clicked, this, [this]{ setLogFontSize(m_log_font_size_val - 1); });
    wv_font_box->addWidget(btn_wv_zoom_out);

    m_wv_font_size = new QSpinBox();
    m_wv_font_size->setRange(8, 32);
    m_wv_font_size->setSuffix(" pt");
    m_wv_font_size->setFixedWidth(78);
    m_wv_font_size->setFixedHeight(26);
    m_wv_font_size->setAlignment(Qt::AlignCenter);
    m_wv_font_size->setValue(11);
    connect(m_wv_font_size, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::setLogFontSize);
    wv_font_box->addWidget(m_wv_font_size);

    auto* btn_wv_zoom_in = new QPushButton("+");
    btn_wv_zoom_in->setFixedSize(26, 26);
    btn_wv_zoom_in->setToolTip("放大字体 (快捷键: ⌘ +)");
    btn_wv_zoom_in->setCursor(Qt::PointingHandCursor);
    connect(btn_wv_zoom_in, &QPushButton::clicked, this, [this]{ setLogFontSize(m_log_font_size_val + 1); });
    wv_font_box->addWidget(btn_wv_zoom_in);

    auto* btn_wv_reset_font = new QPushButton("默认");
    btn_wv_reset_font->setFixedHeight(26);
    btn_wv_reset_font->setToolTip("恢复默认字号 11 pt (快捷键: ⌘ 0)");
    btn_wv_reset_font->setCursor(Qt::PointingHandCursor);
    connect(btn_wv_reset_font, &QPushButton::clicked, this, [this]{ setLogFontSize(11); });
    wv_font_box->addWidget(btn_wv_reset_font);

    net_bottom_bar->addLayout(wv_font_box);

    layout->addLayout(net_bottom_bar);
    return panel;
}

// ==========================================
// Page 2: 常用命令 (HDC Presets Panel)
// ==========================================
QWidget* MainWindow::buildHdcPanel() {
    auto* panel = new QWidget();
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    auto* top_bar = new QHBoxLayout();
    m_hdc_target_badge = IconManager::label("目标: 未选择设备", "Badge");
    top_bar->addWidget(m_hdc_target_badge);
    m_hdc_desc_label = IconManager::label("设备命令自动指定目标设备；支持一键常用操作与自定义交互命令。", "Muted");
    top_bar->addWidget(m_hdc_desc_label);
    top_bar->addStretch();
    layout->addLayout(top_bar);

    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->setChildrenCollapsible(false);

    auto* top_widget = new QWidget();
    auto* top_layout = new QVBoxLayout(top_widget);
    top_layout->setContentsMargins(0, 0, 0, 0);
    top_layout->setSpacing(10);

    auto* cards_grid = new QGridLayout();
    cards_grid->setSpacing(10);

    // 1. 设备与系统控制
    auto* card_sys = new QFrame();
    card_sys->setObjectName("InfoCard");
    auto* sys_layout = new QVBoxLayout(card_sys);
    sys_layout->setContentsMargins(14, 12, 14, 12);
    sys_layout->setSpacing(8);
    sys_layout->addWidget(IconManager::label("设备与系统控制", "CardTitle"));

    auto* btn_grid_sys = new QGridLayout();
    btn_grid_sys->setSpacing(6);

    auto* btn_home = IconManager::button("桌面", "");
    connect(btn_home, &QPushButton::clicked, this, [this]{ sendKey("home"); });
    auto* btn_back = IconManager::button("返回", "");
    connect(btn_back, &QPushButton::clicked, this, [this]{ sendKey("back"); });
    auto* btn_power = IconManager::button("电源 / 息屏", "");
    connect(btn_power, &QPushButton::clicked, this, [this]{ sendKey("power"); });

    auto* btn_mount = IconManager::button("挂载读写", "");
    connect(btn_mount, &QPushButton::clicked, this, [this]{
        m_hdc_input->setText("target mount");
        executeCustomHdc();
    });

    auto* btn_screenshot = IconManager::button("屏幕截图", "");
    connect(btn_screenshot, &QPushButton::clicked, this, &MainWindow::saveScreenshot);

    auto* btn_reboot = IconManager::button("重启设备", "Danger");
    connect(btn_reboot, &QPushButton::clicked, this, [this]{
        int ret = QMessageBox::warning(this, "重启确认", "确定要重启目标设备吗？", QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            m_hdc_input->setText("shell reboot");
            executeCustomHdc();
        }
    });

    auto* btn_stop_wd = IconManager::button("禁用看门狗", "");
    connect(btn_stop_wd, &QPushButton::clicked, this, [this]{ setWatchdogStatusAction(true); });
    auto* btn_start_wd = IconManager::button("开启看门狗", "");
    connect(btn_start_wd, &QPushButton::clicked, this, [this]{ setWatchdogStatusAction(false); });
    auto* btn_feed_wd = IconManager::button("手动喂狗", "");
    connect(btn_feed_wd, &QPushButton::clicked, this, &MainWindow::feedWatchdogAction);

    btn_grid_sys->addWidget(btn_home, 0, 0);
    btn_grid_sys->addWidget(btn_back, 0, 1);
    btn_grid_sys->addWidget(btn_power, 0, 2);
    btn_grid_sys->addWidget(btn_mount, 1, 0);
    btn_grid_sys->addWidget(btn_screenshot, 1, 1);
    btn_grid_sys->addWidget(btn_reboot, 1, 2);
    btn_grid_sys->addWidget(btn_stop_wd, 2, 0);
    btn_grid_sys->addWidget(btn_start_wd, 2, 1);
    btn_grid_sys->addWidget(btn_feed_wd, 2, 2);
    sys_layout->addLayout(btn_grid_sys);
    cards_grid->addWidget(card_sys, 0, 0);

    // 2. 应用与文件传输
    auto* card_app = new QFrame();
    card_app->setObjectName("InfoCard");
    auto* app_layout = new QVBoxLayout(card_app);
    app_layout->setContentsMargins(14, 12, 14, 12);
    app_layout->setSpacing(8);
    app_layout->addWidget(IconManager::label("应用与文件", "CardTitle"));

    auto* btn_grid_app = new QGridLayout();
    btn_grid_app->setSpacing(6);

    auto* btn_install = IconManager::button("安装应用...", "");
    connect(btn_install, &QPushButton::clicked, this, [this]{
        QString path = QFileDialog::getOpenFileName(this, "选择 HAP 安装包", QDir::homePath(), "OpenHarmony Package (*.hap)");
        if (!path.isEmpty()) {
            m_hdc_input->setText(QString("app install -r \"%1\"").arg(path));
            executeCustomHdc();
        }
    });

    auto* btn_uninstall = IconManager::button("卸载应用...", "");
    connect(btn_uninstall, &QPushButton::clicked, this, [this]{
        bool ok = false;
        QString bundle = QInputDialog::getText(this, "卸载应用", "请输入要卸载的 Bundle 名称:", QLineEdit::Normal, DEFAULT_PACKAGE_FILTER, &ok);
        if (ok && !bundle.isEmpty()) {
            m_hdc_input->setText(QString("app uninstall %1").arg(bundle));
            executeCustomHdc();
        }
    });

    auto* btn_clean = IconManager::button("清除缓存...", "");
    connect(btn_clean, &QPushButton::clicked, this, [this]{
        bool ok = false;
        QString bundle = QInputDialog::getText(this, "清除缓存", "请输入 Bundle 名称:", QLineEdit::Normal, DEFAULT_PACKAGE_FILTER, &ok);
        if (ok && !bundle.isEmpty()) {
            m_hdc_input->setText(QString("shell bm clean -c -b %1").arg(bundle));
            executeCustomHdc();
        }
    });

    auto* btn_send = IconManager::button("推送文件...", "");
    connect(btn_send, &QPushButton::clicked, this, [this]{
        QString path = QFileDialog::getOpenFileName(this, "选择推送的本地文件", QDir::homePath());
        if (!path.isEmpty()) {
            m_hdc_input->setText(QString("file send \"%1\" /data/local/tmp/").arg(path));
            executeCustomHdc();
        }
    });

    auto* btn_recv = IconManager::button("拉取文件...", "");
    connect(btn_recv, &QPushButton::clicked, this, [this]{
        bool ok = false;
        QString remote = QInputDialog::getText(this, "拉取文件", "设备端文件完整路径:", QLineEdit::Normal, "/data/local/tmp/", &ok);
        if (ok && !remote.isEmpty()) {
            m_hdc_input->setText(QString("file recv %1 %2/Downloads/").arg(remote, QDir::homePath()));
            executeCustomHdc();
        }
    });

    auto* btn_net = IconManager::button("网络配置...", "");
    connect(btn_net, &QPushButton::clicked, this, [this]{
        m_hdc_input->setText("shell ifconfig");
        executeCustomHdc();
    });

    auto* btn_pull_disk_log = IconManager::button("拉取应用日志...", "");
    connect(btn_pull_disk_log, &QPushButton::clicked, this, [this]{ fetchAppLogsAction(DEFAULT_PACKAGE_FILTER); });

    btn_grid_app->addWidget(btn_install, 0, 0);
    btn_grid_app->addWidget(btn_uninstall, 0, 1);
    btn_grid_app->addWidget(btn_clean, 0, 2);
    btn_grid_app->addWidget(btn_send, 1, 0);
    btn_grid_app->addWidget(btn_recv, 1, 1);
    btn_grid_app->addWidget(btn_net, 1, 2);
    btn_grid_app->addWidget(btn_pull_disk_log, 2, 0, 1, 3);
    app_layout->addLayout(btn_grid_app);
    cards_grid->addWidget(card_app, 0, 1);

    top_layout->addLayout(cards_grid);

    // Command Interactive Input Box
    auto* cmd_box = new QFrame();
    cmd_box->setObjectName("AppBox");
    auto* cmd_box_layout = new QVBoxLayout(cmd_box);
    cmd_box_layout->setContentsMargins(12, 10, 12, 10);
    cmd_box_layout->setSpacing(8);

    // Row 1: Template Combo
    auto* template_row = new QHBoxLayout();
    template_row->setSpacing(8);
    template_row->addWidget(IconManager::label("常用模板:"));

    m_hdc_preset_combo = new QComboBox();
    m_hdc_preset_combo->addItems({
        "—— 快速选择常用命令模板 (点击填入) ——",
        "target mount",
        "shell ifconfig",
        "shell cat /sys/class/ICU/version/StopWatchDog",
        "shell echo -n 1 > /sys/class/ICU/version/StopWatchDog",
        "shell echo -n 0 > /sys/class/ICU/version/StopWatchDog",
        "shell echo -n 1 > /sys/class/ICU/version/FeedWatchdog",
        "shell ls -l /data/app/el2/100/base/com.yarward.*/haps/*/files/log/",
        "shell param get",
        "shell bm dump -a",
        "shell ps -A",
        "shell reboot",
        "list targets",
    });
    IconManager::setupComboBoxPopup(m_hdc_preset_combo, 260);
    connect(m_hdc_preset_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onHdcPresetSelected);
    template_row->addWidget(m_hdc_preset_combo, 1);
    cmd_box_layout->addLayout(template_row);

    // Row 2: Input + Run + Kill
    auto* input_row = new QHBoxLayout();
    input_row->setSpacing(8);
    input_row->addWidget(IconManager::label("hdc", "Brand"));

    m_hdc_target_prefix = IconManager::label("-t <未选>", "TagMuted");
    input_row->addWidget(m_hdc_target_prefix);

    m_hdc_input = new QLineEdit();
    m_hdc_input->setPlaceholderText("输入命令参数，例如 shell ls -la /data 或 target mount (回车直接执行)...");
    connect(m_hdc_input, &QLineEdit::returnPressed, this, &MainWindow::executeCustomHdc);
    input_row->addWidget(m_hdc_input, 1);

    m_btn_run_hdc = IconManager::button("执行", "Primary");
    connect(m_btn_run_hdc, &QPushButton::clicked, this, &MainWindow::executeCustomHdc);
    m_btn_kill_hdc = IconManager::button("中断", "Danger");
    m_btn_kill_hdc->setEnabled(false);
    connect(m_btn_kill_hdc, &QPushButton::clicked, this, &MainWindow::interruptHdcCommand);

    input_row->addWidget(m_btn_run_hdc);
    input_row->addWidget(m_btn_kill_hdc);
    cmd_box_layout->addLayout(input_row);

    top_layout->addWidget(cmd_box);
    splitter->addWidget(top_widget);

    // Bottom Terminal
    auto* term_widget = new QWidget();
    auto* term_layout = new QVBoxLayout(term_widget);
    term_layout->setContentsMargins(0, 0, 0, 0);
    term_layout->setSpacing(8);

    auto* term_bar = new QHBoxLayout();
    term_bar->addWidget(IconManager::label("终端控制台输出", "CardTitle"));
    m_hdc_status_label = IconManager::label("就绪", "Muted");
    term_bar->addWidget(m_hdc_status_label);
    term_bar->addStretch();

    auto* btn_copy_term = IconManager::button("复制输出", "");
    connect(btn_copy_term, &QPushButton::clicked, this, &MainWindow::copyHdcOutput);
    auto* btn_clear_term = IconManager::button("清空终端", "");
    connect(btn_clear_term, &QPushButton::clicked, this, &MainWindow::clearHdcTerminal);
    auto* btn_export_term = IconManager::button("导出为文件...", "");
    connect(btn_export_term, &QPushButton::clicked, this, &MainWindow::exportHdcOutput);

    term_bar->addWidget(btn_copy_term);
    term_bar->addWidget(btn_clear_term);
    term_bar->addWidget(btn_export_term);
    term_layout->addLayout(term_bar);

    m_hdc_terminal = new QTextEdit();
    m_hdc_terminal->setReadOnly(true);
    m_hdc_terminal->setFont(QFont("Menlo", 11));
    m_hdc_terminal->setLineWrapMode(QTextEdit::NoWrap);
    m_hdc_terminal->setStyleSheet("background: #080e15; color: #e7edf4; border: 1px solid #1f2d3d; border-radius: 6px; padding: 10px; font-family: 'Menlo', 'Courier New', monospace;");
    term_layout->addWidget(m_hdc_terminal, 1);

    splitter->addWidget(term_widget);
    splitter->setSizes({330, 320});
    layout->addWidget(splitter, 1);

    return panel;
}

// ==========================================
// Workspace Layout Switching
// ==========================================
void MainWindow::setWorkspaceLayout(const QString& mode) {
    m_current_layout = mode;
    m_btn_layout_h->setChecked(mode == "horizontal");
    m_btn_layout_v->setChecked(mode == "vertical");
    m_btn_layout_screen->setChecked(mode == "mirror_only");
    m_btn_layout_logs->setChecked(mode == "logs_only");

    if (mode == "horizontal") {
        m_workspace_splitter->setOrientation(Qt::Horizontal);
        m_mirror_page->show();
        m_log_panel->show();
        int w = m_workspace_splitter->width();
        if (w <= 0) w = 1000;
        m_workspace_splitter->setSizes({static_cast<int>(w * 0.45), static_cast<int>(w * 0.55)});
    } else if (mode == "vertical") {
        m_workspace_splitter->setOrientation(Qt::Vertical);
        m_mirror_page->show();
        m_log_panel->show();
        int h = m_workspace_splitter->height();
        if (h <= 0) h = 800;
        m_workspace_splitter->setSizes({static_cast<int>(h * 0.5), static_cast<int>(h * 0.5)});
    } else if (mode == "mirror_only") {
        m_mirror_page->show();
        m_log_panel->hide();
    } else if (mode == "logs_only") {
        m_mirror_page->hide();
        m_log_panel->show();
    }
}

void MainWindow::switchLogSubtab(int index) {
    m_log_subtab_stack->setCurrentIndex(index);
    m_btn_log_tab_device->setChecked(index == 0);
    m_btn_log_tab_webview->setChecked(index == 1);
    if (index == 1) {
        autoConnectWebView();
    }
}

void MainWindow::autoConnectWebView() {
    std::string s = currentSerial();
    if (s.empty()) {
        showToast("请先选择已连接的设备");
        return;
    }

    showToast("正在检测设备中的 WebView 实例…");
    std::thread([this, s]() {
        auto instances = m_webview->scanInstances(s);
        QMetaObject::invokeMethod(this, [this, s, instances]() {
            if (instances.empty()) {
                showToast("当前设备未检测到运行中的 WebView 界面");
                return;
            }

            int localPort = 9222;
            if (m_webview->forwardInstance(s, instances[0], localPort)) {
                showToast(QString("已建立 WebView 调试端口映射 (PID %1)").arg(instances[0].pid));
                m_webview->fetchPages(localPort);
            } else {
                showToast("WebView 调试端口映射失败");
            }
        });
    }).detach();
}

void MainWindow::setWebViewSubtab(int index) {
    m_wv_right_stack->setCurrentIndex(index);
    m_btn_subtab_console->setObjectName(index == 0 ? "Primary" : "");
    m_btn_subtab_network->setObjectName(index == 1 ? "Primary" : "");
    m_btn_subtab_console->style()->unpolish(m_btn_subtab_console);
    m_btn_subtab_console->style()->polish(m_btn_subtab_console);
    m_btn_subtab_network->style()->unpolish(m_btn_subtab_network);
    m_btn_subtab_network->style()->polish(m_btn_subtab_network);
}

// ==========================================
// Device Discovery & Status Events
// ==========================================
void MainWindow::onDevicesUpdated(const std::vector<Device>& devices) {
    m_cached_devices = devices;
    QString currentSel = m_device_combo->currentData().toString();
    m_device_combo->blockSignals(true);
    m_device_combo->clear();

    int foundIdx = -1;
    for (size_t i = 0; i < devices.size(); ++i) {
        QString s = QString::fromStdString(devices[i].serial);
        QString m = QString::fromStdString(devices[i].model.empty() ? s.toStdString() : devices[i].model);
        m_device_combo->addItem(QString("%1 (%2)").arg(m, s), s);
        if (!m_pending_auto_start_serial.empty() && s.toStdString() == m_pending_auto_start_serial) {
            foundIdx = static_cast<int>(i);
        } else if (foundIdx < 0 && s == currentSel) {
            foundIdx = static_cast<int>(i);
        }
    }

    if (foundIdx >= 0) {
        m_device_combo->setCurrentIndex(foundIdx);
    } else if (!devices.empty()) {
        m_device_combo->setCurrentIndex(0);
    }
    m_device_combo->blockSignals(false);

    if (!devices.empty()) {
        onDeviceSelected(m_device_combo->currentIndex());
    }

    if (devices.empty()) {
        renderEmptyInfo();
        m_state_badge->setText("待连接");
    } else {
        if (m_pages->currentIndex() == 0 && !m_info_loading) {
            refreshDeviceInfo();
        }

        bool pending = !m_pending_auto_start_serial.empty();
        m_pending_auto_start_serial.clear();

        if ((pending || (m_auto_start_mirror && !m_user_stopped_mirror)) && !m_projecting) {
            QTimer::singleShot(150, this, [this]() {
                if (!m_projecting && !currentSerial().empty() && !m_user_stopped_mirror) {
                    startMirror();
                }
            });
        }
    }
}

void MainWindow::onDeviceSelected(int index) {
    if (index >= 0) {
        std::string s = currentSerial();
        m_hdc_target_badge->setText(QString("目标: %1").arg(QString::fromStdString(s)));
        m_hdc_target_prefix->setText(QString("-t %1").arg(QString::fromStdString(s)));
        if (m_pages->currentIndex() == 0) {
            refreshDeviceInfo();
        }
    }
}

void MainWindow::refreshDevices() {
    m_dev_mgr->refresh();
    showToast("正在扫描在线设备…");
}

void MainWindow::disconnectCurrentDevice() {
    std::string s = currentSerial();
    if (s.empty()) return;
    stopMirror(true);
    m_hdc->executeAsync({"tconn", s, "-remove"}, "", [this](const CommandResult&){
        refreshDevices();
        showToast("已请求断开设备");
    });
}

void MainWindow::openDeviceCenter() {
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle("设备维护中心 · targets & 网络直连");
    dlg->resize(540, 360);
    auto* l = new QVBoxLayout(dlg);

    auto* ip_box = new QHBoxLayout();
    ip_box->addWidget(IconManager::label("目标 IP:"));
    auto* ip_in = new QLineEdit();
    ip_in->setPlaceholderText("192.168.1.100");
    ip_box->addWidget(ip_in);

    ip_box->addWidget(IconManager::label("端口:"));
    auto* port_in = new QLineEdit("8710");
    port_in->setFixedWidth(60);
    ip_box->addWidget(port_in);

    auto* btn_conn = IconManager::button("连接", "Primary");
    connect(btn_conn, &QPushButton::clicked, dlg, [this, ip_in, port_in, dlg]{
        QString target = QString("%1:%2").arg(ip_in->text().trimmed(), port_in->text().trimmed());
        if (ip_in->text().trimmed().isEmpty()) return;
        m_hdc->executeAsync({"tconn", target.toStdString()}, "", [this, target, dlg](const CommandResult& r){
            if (r.success()) {
                showToast(QString("成功连接到设备: %1，正在开启投屏…").arg(target));
                m_pending_auto_start_serial = target.toStdString();
                m_user_stopped_mirror = false;
                m_dev_mgr->refresh();
                dlg->accept();
            } else {
                showToast(QString("连接失败: %1").arg(QString::fromStdString(r.err.empty() ? r.out : r.err)));
            }
        });
    });
    ip_box->addWidget(btn_conn);
    l->addLayout(ip_box);

    auto* dev_list = new QTableWidget(0, 3);
    dev_list->setHorizontalHeaderLabels({"序列号 / 地址", "型号", "状态"});
    dev_list->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    dev_list->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    dev_list->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    for (const auto& d : m_cached_devices) {
        int row = dev_list->rowCount();
        dev_list->insertRow(row);
        dev_list->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(d.serial)));
        dev_list->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(d.model)));
        dev_list->setItem(row, 2, new QTableWidgetItem(d.state == DeviceState::Connected ? "已连接" : "离线"));
    }
    l->addWidget(dev_list, 1);
    dlg->exec();
    dlg->deleteLater();
}

// ==========================================
// Mirroring Session Control
// ==========================================
void MainWindow::startMirror() {
    std::string s = currentSerial();
    if (s.empty()) {
        showToast("请先选择已连接的设备");
        return;
    }

    m_user_stopped_mirror = false;

    if (m_projecting) return;
    m_projecting = true;
    m_start_btn->hide();
    m_stop_btn->show();
    m_state_badge->setText("连接中…");
    m_screen->setActive(true);

    // Wake up device screen
    m_hdc->wakeup(s);

    int bitrates[] = {4000000, 1500000, 8000000};
    int br = bitrates[m_bitrate_idx];

    Device dev;
    dev.serial = s;
    for (const auto& d : m_cached_devices) {
        if (d.serial == s) {
            dev = d;
            break;
        }
    }

    m_stream = std::make_unique<StreamSession>(m_hdc, dev, 60, br, 0, this);

    connect(m_stream.get(), &StreamSession::frameReady, this, [this](const DecodedFrame& f){
        m_screen->updateFrame(f);
        m_state_badge->setText(QString("投屏中 · %1x%2").arg(f.width).arg(f.height));
    });

    connect(m_stream.get(), &StreamSession::statusChanged, this, [this](const QString& status){
        m_state_badge->setText(status);
        statusBar()->showMessage(status, 4000);
    });

    connect(m_stream.get(), &StreamSession::streamFinished, this, [this](const QString& error){
        if (m_projecting) {
            showToast(QString("投屏断开: %1").arg(error));
            stopMirror(false);
        }
    });

    m_screen->setTouchCallback([this, s](TouchAction act, int x, int y, int dw, int dh){
        static QPoint down_pt;
        static int64_t down_time = 0;

        if (act == TouchAction::Down) {
            down_pt = QPoint(x, y);
            down_time = QDateTime::currentMSecsSinceEpoch();
        } else if (act == TouchAction::Up) {
            int64_t dur = std::max(50LL, QDateTime::currentMSecsSinceEpoch() - down_time);
            int dx = std::abs(x - down_pt.x());
            int dy = std::abs(y - down_pt.y());

            if (dx < 10 && dy < 10) {
                // Single touch click
                m_hdc->executeAsync({"shell", "uinput", "-T", "-c", std::to_string(x), std::to_string(y)}, s, nullptr);
            } else {
                // Drag or swipe gesture
                m_hdc->executeAsync({"shell", "uinput", "-T", "-m",
                    std::to_string(down_pt.x()), std::to_string(down_pt.y()),
                    std::to_string(x), std::to_string(y),
                    std::to_string(std::clamp(static_cast<int>(dur), 100, 800))}, s, nullptr);
            }
        }
    });

    m_stream->start();

    // Start HiLog simultaneously
    m_hilog = std::make_unique<HiLogSession>(m_hdc->path(), s, "", "all", this);
    connect(m_hilog.get(), &HiLogSession::entriesReady, this, &MainWindow::onHiLogEntries);
    m_hilog->start();

    showToast("已启动投屏引擎");
}

void MainWindow::stopMirror(bool userInitiated) {
    if (!m_projecting) return;
    m_projecting = false;
    m_start_btn->show();
    m_stop_btn->hide();
    m_state_badge->setText("待连接");
    m_screen->setActive(false);
    m_screen->clear();

    if (m_stream) {
        m_stream->stop();
        m_stream.reset();
    }
    if (m_hilog) {
        m_hilog->stop();
        m_hilog.reset();
    }
    if (userInitiated) {
        m_user_stopped_mirror = true;
        showToast("投屏已停止");
    }
}

void MainWindow::onQualityChanged(int index) {
    m_bitrate_idx = std::clamp(index, 0, 2);
    if (m_stream) {
        int bitrates[] = {4000000, 1500000, 8000000};
        m_stream->setBitrate(bitrates[m_bitrate_idx]);
    }
}

void MainWindow::rotateDisplay() {
    m_user_rotation = (m_user_rotation + 90) % 360;
    m_screen->setRotation(m_user_rotation);
    showToast(QString("旋转画面: %1°").arg(m_user_rotation));
}

void MainWindow::saveScreenshot() {
    std::string s = currentSerial();
    if (s.empty()) return;
    QString filename = QString("screenshot_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    QString dest = QDir::homePath() + "/Downloads/" + filename;
    m_hdc->executeAsync({"shell", "snapshot_display", "-f", "/data/local/tmp/snap.png"}, s, [this, s, dest](const CommandResult& r){
        if (r.success()) {
            m_hdc->executeAsync({"file", "recv", "/data/local/tmp/snap.png", dest.toStdString()}, s, [this, dest](const CommandResult& r2){
                if (r2.success()) {
                    showToast(QString("截图已保存: %1").arg(dest));
                }
            });
        }
    });
}

void MainWindow::toggleFullscreen() {
    if (isFullScreen()) {
        showNormal();
        m_fullscreen_btn->setText("全屏");
    } else {
        showFullScreen();
        m_fullscreen_btn->setText("退出全屏");
    }
}

void MainWindow::sendKey(const QString& key) {
    std::string s = currentSerial();
    if (s.empty()) return;
    int keyCode = 0;
    if (key == "back") keyCode = 2;
    else if (key == "home") keyCode = 1;
    else if (key == "power") keyCode = 18;

    if (keyCode > 0) {
        m_hdc->executeAsync({"shell", "uinput", "-K", "-d", std::to_string(keyCode), "-u", std::to_string(keyCode)}, s, [](const CommandResult&){});
    }
}

void MainWindow::launchPrimaryAppShortcut() {
    std::string s = currentSerial();
    if (s.empty()) {
        showToast("请先选择已连接的设备");
        return;
    }

    QString targetBundle;
    QString targetAbility = "EntryAbility";

    if (!m_cached_info.yarward_apps.empty()) {
        targetBundle = QString::fromStdString(m_cached_info.yarward_apps[0].bundle);
        if (!m_cached_info.yarward_apps[0].main_ability.empty()) {
            targetAbility = QString::fromStdString(m_cached_info.yarward_apps[0].main_ability);
        }
    } else {
        for (const auto& pkg : m_cached_info.all_packages) {
            QString qpkg = QString::fromStdString(pkg);
            if (qpkg.startsWith("com.yarward.") && !qpkg.contains("test")) {
                targetBundle = qpkg;
                break;
            }
        }
    }

    if (targetBundle.isEmpty()) {
        bool ok = false;
        targetBundle = QInputDialog::getText(this, "启动主程序", "未在设备上检测到 Yarward 业务包，请输入 Bundle 名称:", QLineEdit::Normal, "com.yarward.etnroomoh", &ok);
        if (!ok || targetBundle.isEmpty()) return;
    }

    startYarwardApp(targetBundle, targetAbility);
}

void MainWindow::exportFaultReport() {
    std::string s = currentSerial();
    if (s.empty()) {
        showToast("请先选择已连接的设备");
        return;
    }

    QString defaultName = QString("HarmonyCast_Fault_%1_%2.md")
                              .arg(QString::fromStdString(s).replace(':', '_').replace('.', '_'))
                              .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    QString dest = QFileDialog::getSaveFileName(this, "导出故障现场诊断报告", QDir::homePath() + "/Downloads/" + defaultName, "Markdown 报告 (*.md);;所有文件 (*.*)");
    if (dest.isEmpty()) return;

    QFile file(dest);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        showToast("无法创建目标文件");
        return;
    }

    QTextStream out(&file);
    out << "# HarmonyCast 故障现场诊断报告\n\n";
    out << "- **导出时间**: " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n";
    out << "- **设备序列号**: `" << QString::fromStdString(s) << "`\n";
    out << "- **设备型号**: " << QString::fromStdString(m_cached_info.model.empty() ? "未知" : m_cached_info.model) << "\n";
    out << "- **系统版本**: " << QString::fromStdString(m_cached_info.os_fullname.empty() ? "OpenHarmony" : m_cached_info.os_fullname) << "\n";
    out << "- **开机时长**: " << QString::fromStdString(m_cached_info.uptime.empty() ? "未知" : m_cached_info.uptime) << "\n";
    out << "- **内存情况**: " << QString::fromStdString(m_cached_info.memory.empty() ? "未知" : m_cached_info.memory) << "\n";
    out << "- **存储空间**: " << QString::fromStdString(m_cached_info.storage.empty() ? "未知" : m_cached_info.storage) << "\n\n";

    out << "## 业务应用状态\n\n";
    if (m_cached_info.yarward_apps.empty()) {
        out << "未发现 Yarward 业务应用。\n\n";
    } else {
        out << "| 包名 | 版本 | 运行状态 | PID |\n";
        out << "| :--- | :--- | :--- | :--- |\n";
        for (const auto& app : m_cached_info.yarward_apps) {
            out << "| `" << QString::fromStdString(app.bundle) << "` | "
                << QString::fromStdString(app.version_name) << " | "
                << (app.running ? "**运行中**" : "未运行") << " | "
                << QString::fromStdString(app.pids) << " |\n";
        }
        out << "\n";
    }

    out << "## 最近 HiLog 日志快照 (共 " << m_cached_logs.size() << " 条)\n\n";
    out << "```text\n";
    for (const auto& log : m_cached_logs) {
        out << QString::fromStdString(log.format()) << "\n";
    }
    out << "```\n";
    file.close();

    // Save screenshot if available
    QString shotPath = dest;
    shotPath.replace(QRegularExpression("\\.md$"), ".png");
    if (m_screen && !m_screen->image().isNull()) {
        m_screen->image().save(shotPath, "PNG");
    }

    int ret = QMessageBox::information(this, "诊断报告导出完成", QString("故障现场报告已保存至:\n%1\n\n是否打开所在目录？").arg(dest), QMessageBox::Open | QMessageBox::Ok);
    if (ret == QMessageBox::Open) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(dest).absolutePath()));
    }
}

// ==========================================
// HiLog Management
// ==========================================
bool MainWindow::isLogMatch(const HiLogEntry& e, const QString& filterPkg, const QString& filterLvl, const QString& query) const {
    if (filterLvl != "全部级别" && !filterLvl.isEmpty()) {
        if (QString::fromStdString(e.levelString()) != filterLvl) return false;
    }

    if (filterPkg != "全部应用" && !filterPkg.isEmpty()) {
        std::string pkgStr = filterPkg.toStdString();
        bool match = false;
        if (!e.bundle.empty()) {
            match = HiLogParser::packageMatches(e.bundle, pkgStr);
        } else if (pkgStr == "com.yarward.*" || pkgStr.find("yarward") != std::string::npos) {
            if (e.category == "app" || (!e.domain.empty() && e.domain[0] == 'A')) {
                match = true;
            } else if (QString::fromStdString(e.tag).contains("yarward", Qt::CaseInsensitive) ||
                       QString::fromStdString(e.message).contains("yarward", Qt::CaseInsensitive) ||
                       QString::fromStdString(e.tag).contains("MVP", Qt::CaseInsensitive) ||
                       QString::fromStdString(e.tag).contains("ETN", Qt::CaseInsensitive)) {
                match = true;
            }
        } else {
            match = QString::fromStdString(e.tag).contains(filterPkg, Qt::CaseInsensitive) ||
                    QString::fromStdString(e.message).contains(filterPkg, Qt::CaseInsensitive);
        }
        if (!match) return false;
    }

    if (!query.isEmpty()) {
        bool queryMatch = QString::fromStdString(e.tag).contains(query, Qt::CaseInsensitive) ||
                          QString::fromStdString(e.message).contains(query, Qt::CaseInsensitive) ||
                          QString::number(e.pid).contains(query) ||
                          QString::fromStdString(e.bundle).contains(query, Qt::CaseInsensitive);
        if (!queryMatch) return false;
    }

    return true;
}

QString MainWindow::formatLogLine(const HiLogEntry& e) {
    QString tag = QString::fromStdString(e.tag);
    QString msg = QString::fromStdString(e.message);
    if (!tag.isEmpty() && msg.startsWith("[" + tag + "] ")) {
        msg = msg.mid(tag.length() + 3);
    }
    return QString("%1  %2  %3/%4 %5: %6\n")
        .arg(QString::fromStdString(e.timestamp),
             QString::fromStdString(e.levelString()),
             QString::number(e.pid),
             QString::number(e.tid),
             tag,
             msg);
}

void MainWindow::onHiLogEntries(const std::vector<HiLogEntry>& entries) {
    if (m_pause_cb && m_pause_cb->isChecked()) return;

    QString filterPkg = m_package_combo ? m_package_combo->currentText().trimmed() : "";
    QString filterLvl = m_level_combo ? m_level_combo->currentText() : "";
    QString query = m_search_input ? m_search_input->text().trimmed() : "";

    QString newLines;
    int matchedNew = 0;

    for (const auto& e : entries) {
        m_cached_logs.push_back(e);
        if (m_cached_logs.size() > 10000) {
            m_cached_logs.erase(m_cached_logs.begin(), m_cached_logs.begin() + 1000);
        }

        if (isLogMatch(e, filterPkg, filterLvl, query)) {
            newLines += formatLogLine(e);
            matchedNew++;
        }
    }

    if (!newLines.isEmpty() && m_log_view) {
        bool wasAtBottom = (m_log_view->verticalScrollBar()->value() >= m_log_view->verticalScrollBar()->maximum() - 20);
        m_log_view->appendPlainText(newLines.trimmed());
        if (m_autoscroll_cb && m_autoscroll_cb->isChecked() && wasAtBottom) {
            m_log_view->verticalScrollBar()->setValue(m_log_view->verticalScrollBar()->maximum());
        }
        m_visible_log_count += matchedNew;
        if (m_log_count) {
            m_log_count->setText(QString("%1 条日志").arg(m_visible_log_count));
        }
    }
}

void MainWindow::renderHiLog() {
    if (!m_log_view) return;

    QString filterPkg = m_package_combo ? m_package_combo->currentText().trimmed() : "";
    QString filterLvl = m_level_combo ? m_level_combo->currentText() : "";
    QString query = m_search_input ? m_search_input->text().trimmed() : "";

    QString buffer;
    int visibleCount = 0;

    for (const auto& e : m_cached_logs) {
        if (isLogMatch(e, filterPkg, filterLvl, query)) {
            buffer += formatLogLine(e);
            visibleCount++;
        }
    }

    m_visible_log_count = visibleCount;
    m_log_view->setPlainText(buffer);
    if (m_autoscroll_cb && m_autoscroll_cb->isChecked()) {
        m_log_view->verticalScrollBar()->setValue(m_log_view->verticalScrollBar()->maximum());
    }
    if (m_log_count) {
        m_log_count->setText(QString("%1 条日志").arg(visibleCount));
    }
}

void MainWindow::clearHiLog() {
    m_cached_logs.clear();
    m_visible_log_count = 0;
    m_log_view->clear();
    m_log_count->setText("0 条日志");
}

void MainWindow::exportHiLog() {
    QString path = QFileDialog::getSaveFileName(this, "导出 HiLog", QDir::homePath() + "/Downloads/hilog.txt", "Text Files (*.txt)");
    if (!path.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write(m_log_view->toPlainText().toUtf8());
            showToast("HiLog 已成功导出");
        }
    }
}

void MainWindow::setLogFontSize(int size) {
    size = std::clamp(size, 8, 32);
    m_log_font_size_val = size;
    QFont f("Menlo", size);
    if (m_log_view) m_log_view->setFont(f);
    if (m_wv_console_view) m_wv_console_view->setFont(f);
    if (m_log_font_size && m_log_font_size->value() != size) {
        QSignalBlocker b(m_log_font_size);
        m_log_font_size->setValue(size);
    }
    if (m_wv_font_size && m_wv_font_size->value() != size) {
        QSignalBlocker b(m_wv_font_size);
        m_wv_font_size->setValue(size);
    }
}

// ==========================================
// WebView Debugger Events
// ==========================================
void MainWindow::onWebConsoleMessage(const WebConsoleMessage& msg) {
    m_wv_console_count++;
    QString formatted = QString("[%1] %2 (%3:%4)\n").arg(
        QString::fromStdString(msg.level).toUpper(),
        QString::fromStdString(msg.text),
        QString::fromStdString(msg.source_url),
        QString::number(msg.line_number));
    m_wv_console_view->appendPlainText(formatted);
    if (m_wv_autoscroll->isChecked()) {
        m_wv_console_view->verticalScrollBar()->setValue(m_wv_console_view->verticalScrollBar()->maximum());
    }
    m_wv_net_status_label->setText(QString("%1 条日志 · %2 个请求").arg(m_wv_console_count).arg(m_wv_network_count));
}

void MainWindow::onWebNetworkRequest(const WebNetworkRequest& req) {
    m_wv_network_count++;
    int row = m_wv_network_table->rowCount();
    m_wv_network_table->insertRow(row);
    m_wv_network_table->setItem(row, 0, new QTableWidgetItem("..."));
    m_wv_network_table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(req.method)));
    m_wv_network_table->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(req.url)));
    m_wv_network_table->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(req.mime_type)));
    m_wv_network_table->setItem(row, 4, new QTableWidgetItem("进行中"));
    m_wv_network_table->item(row, 2)->setData(Qt::UserRole, QString::fromStdString(req.request_id));
    m_wv_net_status_label->setText(QString("%1 条日志 · %2 个请求").arg(m_wv_console_count).arg(m_wv_network_count));
}

void MainWindow::onWebNetworkResponse(const QString& reqId, int status, const QString& mimeType) {
    for (int r = 0; r < m_wv_network_table->rowCount(); ++r) {
        auto* item = m_wv_network_table->item(r, 2);
        if (item && item->data(Qt::UserRole).toString() == reqId) {
            m_wv_network_table->setItem(r, 0, new QTableWidgetItem(QString::number(status)));
            m_wv_network_table->setItem(r, 3, new QTableWidgetItem(mimeType));
            m_wv_network_table->setItem(r, 4, new QTableWidgetItem("完成"));
            if (status >= 400) {
                m_wv_network_table->item(r, 0)->setForeground(QColor("#ff8080"));
            } else {
                m_wv_network_table->item(r, 0)->setForeground(QColor("#5ce0c1"));
            }
            break;
        }
    }
}

void MainWindow::clearWebViewConsole() {
    m_wv_console_count = 0;
    m_wv_network_count = 0;
    m_wv_console_view->clear();
    m_wv_network_table->setRowCount(0);
    m_wv_net_status_label->setText("0 条日志 · 0 个请求");
}

void MainWindow::executeWebViewJs() {
    QString js = m_wv_js_input->text().trimmed();
    if (js.isEmpty()) return;
    m_wv_console_view->appendPlainText(QString("JS > %1").arg(js));
    m_wv_js_input->clear();

    if (m_webview->isConnected()) {
        m_webview->evaluateJs(js);
        showToast("已向页面发送 JS 指令");
    } else {
        showToast("WebView 调试器未连接，请先确保应用中有加载网页");
    }
}

void MainWindow::openChromeInspect() {
    QString targetUrl = "chrome://inspect/#devices";
    copyToClipboard(targetUrl, "Chrome 调试地址");

    bool opened = false;
#if defined(Q_OS_MACOS)
    // On macOS, chrome:// is an internal browser protocol and not registered with OS LaunchServices.
    // Launch Google Chrome (or Edge/Chromium) directly via open -a.
    if (QProcess::startDetached("open", {"-a", "Google Chrome", targetUrl})) {
        opened = true;
    } else if (QProcess::startDetached("open", {"-a", "Microsoft Edge", targetUrl})) {
        opened = true;
    } else if (QProcess::startDetached("open", {"-a", "Chromium", targetUrl})) {
        opened = true;
    }
#elif defined(Q_OS_WIN)
    if (QProcess::startDetached("cmd", {"/c", "start", "chrome", targetUrl})) {
        opened = true;
    } else if (QProcess::startDetached("cmd", {"/c", "start", "msedge", targetUrl})) {
        opened = true;
    }
#else
    if (QProcess::startDetached("google-chrome", {targetUrl})) {
        opened = true;
    } else if (QProcess::startDetached("chromium-browser", {targetUrl})) {
        opened = true;
    }
#endif

    if (!opened) {
        QDesktopServices::openUrl(QUrl(targetUrl));
    }
    showToast("已唤起 Chrome 调试页 (chrome://inspect/#devices 已复制)");
}

void MainWindow::onNetworkTableContextMenu(const QPoint& pos) {
    int row = m_wv_network_table->rowAt(pos.y());
    if (row < 0) return;
    QMenu menu(this);
    menu.addAction("复制为 cURL 命令", this, [this, row]{
        auto* urlItem = m_wv_network_table->item(row, 2);
        auto* methodItem = m_wv_network_table->item(row, 1);
        if (urlItem && methodItem) {
            QString curl = QString("curl -X %1 \"%2\"").arg(methodItem->text(), urlItem->text());
            copyToClipboard(curl, "cURL 命令");
        }
    });
    menu.exec(m_wv_network_table->viewport()->mapToGlobal(pos));
}

// ==========================================
// HDC Command Runner Events
// ==========================================
void MainWindow::onHdcPresetSelected(int index) {
    if (index <= 0) return;
    QString cmd = m_hdc_preset_combo->itemText(index);
    m_hdc_input->setText(cmd);
}

void MainWindow::executeCustomHdc() {
    QString input = m_hdc_input->text().trimmed();
    if (input.isEmpty()) return;

    std::string s = currentSerial();
    m_hdc_status_label->setText("执行中…");
    m_btn_run_hdc->setEnabled(false);
    m_btn_kill_hdc->setEnabled(true);

    m_hdc_terminal->append(QString("\n$ hdc %1 %2").arg(s.empty() ? "" : QString("-t %1").arg(QString::fromStdString(s)), input));

    std::vector<std::string> args;
    if (input.startsWith("shell ") || input.startsWith("shell\t")) {
        args = {"shell", input.mid(6).trimmed().toStdString()};
    } else {
        QStringList tokens = QProcess::splitCommand(input);
        for (const auto& t : tokens) args.push_back(t.toStdString());
    }

    m_hdc->executeAsync(args, s, [this](const CommandResult& r){
        m_btn_run_hdc->setEnabled(true);
        m_btn_kill_hdc->setEnabled(false);
        m_hdc_status_label->setText("就绪");

        if (r.success()) {
            m_hdc_terminal->append(QString::fromStdString(r.out));
        } else {
            m_hdc_terminal->append(QString("<ERROR> %1").arg(QString::fromStdString(r.err.empty() ? r.out : r.err)));
        }
        m_hdc_terminal->verticalScrollBar()->setValue(m_hdc_terminal->verticalScrollBar()->maximum());
    });
}

void MainWindow::interruptHdcCommand() {
    showToast("已中断命令执行");
    m_btn_run_hdc->setEnabled(true);
    m_btn_kill_hdc->setEnabled(false);
    m_hdc_status_label->setText("已中断");
}

void MainWindow::clearHdcTerminal() {
    m_hdc_terminal->clear();
    m_hdc_status_label->setText("就绪");
}

void MainWindow::copyHdcOutput() {
    copyToClipboard(m_hdc_terminal->toPlainText(), "终端控制台输出");
}

void MainWindow::exportHdcOutput() {
    QString path = QFileDialog::getSaveFileName(this, "导出终端输出", QDir::homePath() + "/Downloads/hdc_terminal.log", "Log Files (*.log)");
    if (!path.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write(m_hdc_terminal->toPlainText().toUtf8());
            showToast("终端输出已导出");
        }
    }
}

} // namespace harmonycast
