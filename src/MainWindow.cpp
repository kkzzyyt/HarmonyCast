#include "harmonycast/MainWindow.h"
#include "harmonycast/FlowLayout.h"
#include "harmonycast/IconManager.h"
#include "harmonycast/NetworkDeviceDialog.h"
#include "harmonycast/LogHighlighter.h"
#include "harmonycast/Logger.h"
#include "harmonycast/CrashHandler.h"
#include "harmonycast/UpdateDialog.h"

#include <unordered_set>

#include <QApplication>
#include <QClipboard>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QFileDialog>
#include <QMessageBox>
#include <QScrollBar>
#include <QDateTime>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QUrlQuery>
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
      m_updater(std::make_unique<UpdateChecker>(this)),
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
    connect(m_updater.get(), &UpdateChecker::updateAvailable, this, &MainWindow::onUpdateAvailable);
    connect(m_updater.get(), &UpdateChecker::noUpdateAvailable, this, &MainWindow::onNoUpdateAvailable);
    connect(m_updater.get(), &UpdateChecker::checkFailed, this, &MainWindow::onUpdateCheckFailed);
    connect(m_webview.get(), &WebViewDebugger::consoleMessageReceived, this, &MainWindow::onWebConsoleMessage);
    connect(m_webview.get(), &WebViewDebugger::networkRequestReceived, this, &MainWindow::onWebNetworkRequest);
    connect(m_webview.get(), &WebViewDebugger::networkResponseReceived, this, &MainWindow::onWebNetworkResponse);
    connect(m_webview.get(), &WebViewDebugger::pagesLoaded, this, [this](const std::vector<WebViewPage>& pages){
        if (pages.empty()) {
            showToast("当前设备未检测到运行中的 WebView 界面");
            statusBar()->showMessage("未检测到运行中的 WebView 界面，请确认应用已启动并在前台加载网页", 5000);
            if (m_wv_net_status_label) {
                m_wv_net_status_label->setText("未检测到 WebView 界面 · 请在设备上打开 Web 界面后点击「重新检测」");
            }
            if (m_wv_console_view && m_wv_console_view->toPlainText().isEmpty()) {
                m_wv_console_view->setPlaceholderText(
                    "【未检测到运行中的 WebView 界面】\n\n"
                    "1. 请确认设备上已打开并运行包含 Web/ArkWeb 组件的应用；\n"
                    "2. 确保页面已在前台加载完成；\n"
                    "3. 打开页面后，点击上方工具栏中的「重新检测」按钮即可自动建立调试连接。"
                );
            }
        } else {
            m_webview->connectPage(pages[0]);
            QString title = QString::fromStdString(pages[0].title.empty() ? pages[0].url : pages[0].title);
            showToast(QString("已成功连接 Web 调试页面: %1").arg(title));
            statusBar()->showMessage(QString("已成功连接 Web 调试页面: %1").arg(title), 4000);
            if (m_wv_net_status_label) {
                m_wv_net_status_label->setText(QString("已连接: %1 · %2 条日志 · %3 个请求").arg(title).arg(m_wv_console_count).arg(m_wv_network_count));
            }
        }
    });

    m_dev_mgr->startPolling(1500);
    switchModule(0);

    QTimer::singleShot(800, this, &MainWindow::checkPreviousCrash);

    // Silent background update check after window displays
    if (m_settings->value("auto_check_update", true).toBool()) {
        QTimer::singleShot(2500, this, [this]{ checkForUpdates(true); });
    }
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

    auto* version_label = IconManager::label("HarmonyCast\nv2.1.2", "Muted");
    bottom_box->addWidget(version_label, 1);

    m_settings_btn = IconManager::iconButton("settings", "系统设置", "IconBtnSettings", 30, 30);
    connect(m_settings_btn, &QPushButton::clicked, this, &MainWindow::openSettingsDialog);
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
    connect(m_device_combo, QOverload<int>::of(&QComboBox::activated), this, &MainWindow::onDeviceComboActivated);
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

    // 2. ICU 硬件信息卡片
    clearLayout(m_icu_layout);
    auto* icu_hdr = new QHBoxLayout();
    icu_hdr->addWidget(IconManager::label("亚华医疗 ICU 硬件信息", "CardTitle"));
    if (!data.icu_available) {
        icu_hdr->addWidget(IconManager::label("通用设备 (未检测到 ICU 节点)", "TagMuted"));
    } else {
        icu_hdr->addWidget(IconManager::label("ICU 硬件就绪", "TagSuccess"));
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
        auto* btn_refresh_icu = IconManager::button("刷新 ICU 状态", "", "refresh");
        connect(btn_refresh_icu, &QPushButton::clicked, this, &MainWindow::refreshDeviceInfo);

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
        {"品牌厂商", QString::fromStdString(data.brand.empty() ? (data.manufacturer.empty() ? "未知" : data.manufacturer) : data.brand)},
        {"产品型号", QString::fromStdString(data.model.empty() ? "未知" : data.model)},
        {"主控芯片", QString::fromStdString(data.chip.empty() ? "未知" : data.chip)},
        {"设备类型", QString::fromStdString(data.device_type.empty() ? "标准终端" : data.device_type)},
        {"硬件版本", QString::fromStdString(data.hardware_version.empty() ? "未知" : data.hardware_version)},
        {"软件版本", QString::fromStdString(data.software_version.empty() ? "未知" : data.software_version)},
        {"操作系统", QString::fromStdString(data.os_fullname.empty() ? "OpenHarmony" : data.os_fullname)},
        {"API 版本", QString("API %1").arg(QString::fromStdString(data.api_version.empty() ? "未知" : data.api_version))},
        {"CPU 架构", QString::fromStdString(data.cpu_abi.empty() ? "未知" : data.cpu_abi)},
        {"安全补丁", QString::fromStdString(data.security_patch.empty() ? "未知" : data.security_patch)},
        {"Linux 内核", QString::fromStdString(data.kernel.empty() ? "未知" : data.kernel)},
        {"屏幕分辨率", QString::fromStdString(data.display.empty() ? "未知" : data.display)},
        {"开机时长", QString::fromStdString(data.uptime.empty() ? "未知" : data.uptime)},
        {"引导版本", QString::fromStdString(data.bootloader.empty() ? "未知" : data.bootloader)},
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

    // 序列号 (SN) 行
    QString sn_str = QString::fromStdString(data.sn.empty() ? "未知" : data.sn);
    row++;
    auto* k_sn = IconManager::label("序列号 (SN)", "InfoKey");
    k_sn->setFixedWidth(80);
    grid->addWidget(k_sn, row, 0);

    auto* sn_box = new QHBoxLayout();
    auto* v_sn = IconManager::label(sn_str, "InfoValMono");
    v_sn->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sn_box->addWidget(v_sn);
    if (!sn_str.isEmpty() && sn_str != "未知") {
        auto* btn_copy_sn = IconManager::button("复制 SN", "");
        btn_copy_sn->setFixedHeight(24);
        btn_copy_sn->setStyleSheet("padding: 1px 6px; font-size: 11px;");
        connect(btn_copy_sn, &QPushButton::clicked, this, [this, sn_str]{ copyToClipboard(sn_str, "设备序列号 (SN)"); });
        sn_box->addWidget(btn_copy_sn);
    }
    sn_box->addStretch();
    grid->addLayout(sn_box, row, 1, 1, 3);

    // 设备 UUID / UDID 行
    QString udid = QString::fromStdString(data.udid.empty() ? (data.sn.empty() ? "未知" : data.sn) : data.udid);
    row++;
    auto* k_udid = IconManager::label("设备 UUID", "InfoKey");
    k_udid->setFixedWidth(80);
    grid->addWidget(k_udid, row, 0);

    auto* udid_box = new QHBoxLayout();
    auto* v_udid = IconManager::label(udid, "InfoValMono");
    v_udid->setTextInteractionFlags(Qt::TextSelectableByMouse);
    udid_box->addWidget(v_udid);
    if (!udid.isEmpty() && udid != "未知") {
        auto* btn_copy_udid = IconManager::button("复制 UUID", "");
        btn_copy_udid->setFixedHeight(24);
        btn_copy_udid->setStyleSheet("padding: 1px 6px; font-size: 11px;");
        connect(btn_copy_udid, &QPushButton::clicked, this, [this, udid]{ copyToClipboard(udid, "设备 UUID"); });
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
          << QString("- 品牌厂商: %1").arg(QString::fromStdString(d.brand.empty() ? d.manufacturer : d.brand))
          << QString("- 产品型号: %1").arg(QString::fromStdString(d.model))
          << QString("- 主控芯片: %1").arg(QString::fromStdString(d.chip.empty() ? "未知" : d.chip))
          << QString("- 设备类型: %1").arg(QString::fromStdString(d.device_type.empty() ? "标准终端" : d.device_type))
          << QString("- 硬件版本: %1").arg(QString::fromStdString(d.hardware_version))
          << QString("- 软件版本: %1").arg(QString::fromStdString(d.software_version))
          << QString("- 操作系统: %1 (API %2)").arg(QString::fromStdString(d.os_fullname), QString::fromStdString(d.api_version))
          << QString("- CPU 架构: %1").arg(QString::fromStdString(d.cpu_abi))
          << QString("- Linux 内核: %1").arg(QString::fromStdString(d.kernel))
          << QString("- 屏幕分辨率: %1").arg(QString::fromStdString(d.display))
          << QString("- 序列号 (SN): %1").arg(QString::fromStdString(d.sn))
          << QString("- 设备 UUID: %1").arg(QString::fromStdString(d.udid.empty() ? d.sn : d.udid))
          << QString("- 引导版本: %1").arg(QString::fromStdString(d.bootloader))
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
    m_quality_combo->addItems({
        "超清原画 (点对点)",
        "超清 900P (清晰均衡)",
        "高清 720P (流畅提速)",
        "标清 540P (极速省流)"
    });
    m_quality_combo->setToolTip(
        "投屏画质：\n"
        "• 超清原画：100% 原始分辨率点对点渲染，文字最清晰 (推荐)\n"
        "• 超清 900P：高清晰度与流畅度均衡推荐\n"
        "• 高清 720P：降低分辨率以提升快照传输帧率\n"
        "• 标清 540P：极速低延迟低带宽模式"
    );
    m_quality_combo->setFixedHeight(28);
    m_quality_combo->setFixedWidth(146);
    IconManager::setupComboBoxPopup(m_quality_combo, 146);
    int savedQuality = m_settings->value("mirror_quality_idx", 0).toInt();
    m_bitrate_idx = std::clamp(savedQuality, 0, 3);
    m_quality_combo->setCurrentIndex(m_bitrate_idx);
    connect(m_quality_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onQualityChanged);
    controls->addWidget(m_quality_combo);

    m_rotate_btn = IconManager::button("旋转", "", "rotate");
    m_rotate_btn->setFixedHeight(28);
    m_rotate_btn->setShortcut(QKeySequence("Ctrl+R"));
    m_rotate_btn->setToolTip("旋转画面 0° / 90° / 180° / 270° (快捷键: ⌘R)");
    connect(m_rotate_btn, &QPushButton::clicked, this, &MainWindow::rotateDisplay);
    controls->addWidget(m_rotate_btn);

    m_btn_pixel_perfect = IconManager::button("1:1 原画", "", "1to1");
    m_btn_pixel_perfect->setFixedHeight(28);
    m_btn_pixel_perfect->setShortcut(QKeySequence("Ctrl+1"));
    m_btn_pixel_perfect->setToolTip("适配 1:1 点对点原生像素尺寸，享受最极致清晰度 (快捷键: ⌘1)");
    connect(m_btn_pixel_perfect, &QPushButton::clicked, this, &MainWindow::resizeToPixelPerfect);
    controls->addWidget(m_btn_pixel_perfect);

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

    // Toolbar Card Frame: styled with subtle border & background
    auto* toolbar_frame = new QFrame();
    toolbar_frame->setObjectName("LogToolbarFrame");
    toolbar_frame->setStyleSheet(
        "QFrame#LogToolbarFrame {"
        "  background: #0f1722;"
        "  border: 1px solid #1c2b3d;"
        "  border-radius: 6px;"
        "}"
    );
    auto* tb_vlayout = new QVBoxLayout(toolbar_frame);
    tb_vlayout->setContentsMargins(8, 7, 8, 7);
    tb_vlayout->setSpacing(7);

    // Row 1: Filters & Search (Data Querying)
    auto* row1 = new QHBoxLayout();
    row1->setContentsMargins(0, 0, 0, 0);
    row1->setSpacing(6);

    // 1. Package combo
    row1->addWidget(IconManager::label("应用:", "Muted"));
    m_package_combo = new QComboBox();
    m_package_combo->setEditable(true);
    m_package_combo->addItem(DEFAULT_PACKAGE_FILTER, DEFAULT_PACKAGE_FILTER);
    m_package_combo->addItem("全部应用", "全部应用");
    m_package_combo->setFixedWidth(155);
    m_package_combo->setFixedHeight(28);
    IconManager::setupComboBoxPopup(m_package_combo, 260);
    connect(m_package_combo, &QComboBox::currentTextChanged, this, &MainWindow::renderHiLog);
    row1->addWidget(m_package_combo);

    row1->addSpacing(4);

    // 2. Level combo
    row1->addWidget(IconManager::label("级别:", "Muted"));
    m_level_combo = new QComboBox();
    m_level_combo->addItem("全部级别", "");
    m_level_combo->addItem("⚪ DEBUG", "DEBUG");
    m_level_combo->addItem("🟢 INFO", "INFO");
    m_level_combo->addItem("🟡 WARN", "WARN");
    m_level_combo->addItem("🔴 ERROR", "ERROR");
    m_level_combo->addItem("🟣 FATAL", "FATAL");
    m_level_combo->setFixedHeight(28);
    m_level_combo->setFixedWidth(112);
    IconManager::setupComboBoxPopup(m_level_combo, 130);
    connect(m_level_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::renderHiLog);
    row1->addWidget(m_level_combo);

    row1->addSpacing(4);

    // 3. Search lineEdit
    m_search_input = new QLineEdit();
    m_search_input->setPlaceholderText("搜索日志内容、Tag、PID、关键词…");
    m_search_input->addAction(IconManager::getIcon("search"), QLineEdit::LeadingPosition);
    m_search_input->setClearButtonEnabled(true);
    m_search_input->setMinimumWidth(100);
    m_search_input->setFixedHeight(28);
    connect(m_search_input, &QLineEdit::textChanged, this, &MainWindow::renderHiLog);
    row1->addWidget(m_search_input, 1);

    tb_vlayout->addLayout(row1);

    // Row 2: Stream Controls & Action Buttons
    auto* row2 = new QHBoxLayout();
    row2->setContentsMargins(0, 0, 0, 0);
    row2->setSpacing(20);

    m_autoscroll_cb = new QCheckBox("自动滚动");
    m_autoscroll_cb->setChecked(true);
    m_autoscroll_cb->setToolTip("新日志到达时自动滚动到底部");
    m_autoscroll_cb->setFixedHeight(26);
    m_autoscroll_cb->setCursor(Qt::PointingHandCursor);

    m_pause_cb = new QCheckBox("暂停刷新");
    m_pause_cb->setToolTip("暂停日志刷新显示");
    m_pause_cb->setFixedHeight(26);
    m_pause_cb->setCursor(Qt::PointingHandCursor);
    connect(m_pause_cb, &QCheckBox::toggled, this, [this](bool paused) {
        if (!paused) renderHiLog();
    });

    row2->addWidget(m_autoscroll_cb);
    row2->addSpacing(8);
    row2->addWidget(m_pause_cb);

    row2->addStretch(1);

    m_clear_log_btn = IconManager::button("清空", "", "clear");
    m_clear_log_btn->setFixedHeight(26);
    m_clear_log_btn->setToolTip("清空当前所有日志缓存 (快捷键: ⌘ K)");
    connect(m_clear_log_btn, &QPushButton::clicked, this, &MainWindow::clearHiLog);

    m_export_log_btn = IconManager::button("导出日志", "", "export");
    m_export_log_btn->setFixedHeight(26);
    m_export_log_btn->setToolTip("导出当前筛选日志到本地文本文件");
    connect(m_export_log_btn, &QPushButton::clicked, this, &MainWindow::exportHiLog);

    row2->addWidget(m_clear_log_btn);
    row2->addWidget(m_export_log_btn);

    tb_vlayout->addLayout(row2);
    log_layout->addWidget(toolbar_frame);

    // Log View
    m_log_view = new QPlainTextEdit();
    m_log_view->setObjectName("LogView");
    m_log_view->setReadOnly(true);
    m_log_view->setFont(QFont("Menlo", 11));
    m_log_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log_view->setMaximumBlockCount(10000);
    m_log_view->setStyleSheet(
        "QPlainTextEdit#LogView {"
        "  background-color: #070c13;"
        "  color: #dbe5ee;"
        "  border: 1px solid #1a2737;"
        "  border-radius: 6px;"
        "  padding: 6px 8px;"
        "  selection-background-color: #17382f;"
        "  selection-color: #79e6c9;"
        "  font-family: 'Menlo', 'JetBrains Mono', 'SF Mono', 'Consolas', monospace;"
        "}"
    );
    m_log_view->installEventFilter(this);
    m_log_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_log_view, &QPlainTextEdit::customContextMenuRequested, this, &MainWindow::onHiLogContextMenu);
    m_log_highlighter = std::make_unique<LogHighlighter>(m_log_view->document());
    log_layout->addWidget(m_log_view, 1);

    // Footer Frame
    auto* footer_frame = new QFrame();
    footer_frame->setObjectName("LogFooterFrame");
    footer_frame->setStyleSheet(
        "QFrame#LogFooterFrame {"
        "  background: #090e15;"
        "  border: 1px solid #192534;"
        "  border-radius: 5px;"
        "  padding: 2px 6px;"
        "}"
    );
    auto* log_footer = new QHBoxLayout(footer_frame);
    log_footer->setContentsMargins(6, 3, 6, 3);
    log_footer->setSpacing(8);

    m_log_count = IconManager::label("显示 0 条 · 缓存 0 条", "Muted");
    log_footer->addWidget(m_log_count);
    log_footer->addStretch(1);

    auto* font_box = new QHBoxLayout();
    font_box->setContentsMargins(0, 0, 0, 0);
    font_box->setSpacing(4);

    auto* font_lbl = IconManager::label("字号", "Muted");
    font_lbl->setToolTip("快捷键: ⌘ + 放大 · ⌘ − 缩小 · ⌘ 0 恢复默认字号 (支持 ⌘ + 滚轮缩放)");
    font_box->addWidget(font_lbl);

    auto* btn_zoom_out = new QPushButton("−");
    btn_zoom_out->setFixedSize(24, 24);
    btn_zoom_out->setToolTip("缩小字体 (快捷键: ⌘ −)");
    btn_zoom_out->setCursor(Qt::PointingHandCursor);
    connect(btn_zoom_out, &QPushButton::clicked, this, [this]{ setLogFontSize(m_log_font_size_val - 1); });
    font_box->addWidget(btn_zoom_out);

    m_log_font_size = new QSpinBox();
    m_log_font_size->setRange(8, 32);
    m_log_font_size->setSuffix(" pt");
    m_log_font_size->setFixedWidth(72);
    m_log_font_size->setFixedHeight(24);
    m_log_font_size->setAlignment(Qt::AlignCenter);
    m_log_font_size->setValue(11);
    connect(m_log_font_size, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::setLogFontSize);
    font_box->addWidget(m_log_font_size);

    auto* btn_zoom_in = new QPushButton("+");
    btn_zoom_in->setFixedSize(24, 24);
    btn_zoom_in->setToolTip("放大字体 (快捷键: ⌘ +)");
    btn_zoom_in->setCursor(Qt::PointingHandCursor);
    connect(btn_zoom_in, &QPushButton::clicked, this, [this]{ setLogFontSize(m_log_font_size_val + 1); });
    font_box->addWidget(btn_zoom_in);

    auto* btn_reset_font = new QPushButton("默认");
    btn_reset_font->setFixedHeight(24);
    btn_reset_font->setToolTip("恢复默认字号 11 pt (快捷键: ⌘ 0)");
    btn_reset_font->setCursor(Qt::PointingHandCursor);
    connect(btn_reset_font, &QPushButton::clicked, this, [this]{ setLogFontSize(11); });
    font_box->addWidget(btn_reset_font);

    log_footer->addLayout(font_box);
    log_layout->addWidget(footer_frame);
    return widget;
}

QWidget* MainWindow::buildWebViewPanel() {
    auto* panel = new QWidget();
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(6);

    // Toolbar Container: 2-row clean structured layout
    auto* wv_toolbar_container = new QFrame();
    wv_toolbar_container->setObjectName("LogToolbarFrame");
    wv_toolbar_container->setStyleSheet(
        "QFrame#LogToolbarFrame {"
        "  background: #0f1722;"
        "  border: 1px solid #1c2b3d;"
        "  border-radius: 6px;"
        "}"
    );
    auto* wv_tb_vlayout = new QVBoxLayout(wv_toolbar_container);
    wv_tb_vlayout->setContentsMargins(8, 7, 8, 7);
    wv_tb_vlayout->setSpacing(7);

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
    connect(m_wv_filter_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::renderWebViewConsole);
    wv_row1->addWidget(m_wv_filter_combo);

    m_wv_filter_input = new QLineEdit();
    m_wv_filter_input->setPlaceholderText("过滤日志内容或请求 URL…");
    m_wv_filter_input->addAction(IconManager::getIcon("search"), QLineEdit::LeadingPosition);
    m_wv_filter_input->setFixedHeight(28);
    m_wv_filter_input->setClearButtonEnabled(true);
    connect(m_wv_filter_input, &QLineEdit::textChanged, this, [this]{
        renderWebViewConsole();
        filterNetworkTable();
    });
    wv_row1->addWidget(m_wv_filter_input, 1);

    wv_tb_vlayout->addLayout(wv_row1);

    // Row 2: Controls & Actions (Generous spacing & breathing room)
    auto* wv_row2 = new QHBoxLayout();
    wv_row2->setContentsMargins(0, 0, 0, 0);
    wv_row2->setSpacing(20);

    m_wv_autoscroll = new QCheckBox("自动滚动");
    m_wv_autoscroll->setChecked(true);
    m_wv_autoscroll->setFixedHeight(26);
    m_wv_autoscroll->setCursor(Qt::PointingHandCursor);

    m_wv_pause = new QCheckBox("暂停刷新");
    m_wv_pause->setFixedHeight(26);
    m_wv_pause->setCursor(Qt::PointingHandCursor);

    wv_row2->addWidget(m_wv_autoscroll);
    wv_row2->addSpacing(8);
    wv_row2->addWidget(m_wv_pause);

    wv_row2->addStretch(1);

    auto* btn_refresh_wv = IconManager::button("重新检测", "", "refresh");
    btn_refresh_wv->setFixedHeight(26);
    btn_refresh_wv->setToolTip("重新检测设备中运行的 WebView 页面并连接");
    connect(btn_refresh_wv, &QPushButton::clicked, this, &MainWindow::autoConnectWebView);
    wv_row2->addWidget(btn_refresh_wv);

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
        "background: #070c13; color: #dbe5ee; border: 1px solid #1a2737; border-radius: 6px; padding: 6px 8px; font-family: 'Menlo', monospace;"
    );
    m_wv_console_view->installEventFilter(this);
    m_wv_console_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_wv_console_view, &QPlainTextEdit::customContextMenuRequested, this, &MainWindow::onWebViewConsoleContextMenu);
    m_wv_log_highlighter = std::make_unique<LogHighlighter>(m_wv_console_view->document());
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

    // Type Filter Bar (Chrome DevTools style)
    auto* type_filter_bar = new QHBoxLayout();
    type_filter_bar->setContentsMargins(4, 2, 4, 4);
    type_filter_bar->setSpacing(5);

    auto* filter_label = IconManager::label("资源类型:", "Muted");
    filter_label->setStyleSheet("color: #798d9f; font-size: 11px; font-weight: 600;");
    type_filter_bar->addWidget(filter_label);

    const std::vector<std::pair<QString, QString>> type_filters = {
        {"全部", "ALL"},
        {"Fetch/XHR", "XHR"},
        {"JS 脚本", "JS"},
        {"CSS 样式", "CSS"},
        {"图片", "IMG"},
        {"文档", "DOC"},
        {"其他", "OTHER"}
    };

    m_net_type_btn_group = new QButtonGroup(this);
    m_net_type_btn_group->setExclusive(true);

    for (const auto& [title, key] : type_filters) {
        auto* btn = new QPushButton(title);
        btn->setCheckable(true);
        btn->setFixedHeight(22);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setProperty("filterKey", key);
        btn->setStyleSheet(
            "QPushButton {"
            "  background: #0d1622; color: #8ba2b9; border: 1px solid #1a2737; border-radius: 4px; padding: 2px 8px; font-size: 11px;"
            "}"
            "QPushButton:hover {"
            "  background: #152438; color: #c2d1df; border-color: #263c54;"
            "}"
            "QPushButton:checked {"
            "  background: #17384e; color: #5ce0c1; border-color: #2d6b91; font-weight: bold;"
            "}"
        );
        if (key == "ALL") btn->setChecked(true);
        m_net_type_btn_group->addButton(btn);
        type_filter_bar->addWidget(btn);
    }

    connect(m_net_type_btn_group, &QButtonGroup::buttonClicked, this, [this](QAbstractButton* btn) {
        m_current_net_type_filter = btn->property("filterKey").toString();
        filterNetworkTable();
    });

    type_filter_bar->addStretch(1);
    n_layout->addLayout(type_filter_bar);

    m_wv_network_table = new QTableWidget(0, 5);
    m_wv_network_table->setHorizontalHeaderLabels({"状态", "方式", "接口 / URL", "类型", "耗时"});
    m_wv_network_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_wv_network_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_wv_network_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_wv_network_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_wv_network_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_wv_network_table->setSelectionBehavior(QTableWidget::SelectRows);
    m_wv_network_table->setSelectionMode(QTableWidget::SingleSelection);
    m_wv_network_table->setEditTriggers(QTableWidget::NoEditTriggers);
    m_wv_network_table->setAlternatingRowColors(true);
    m_wv_network_table->verticalHeader()->setVisible(false);
    m_wv_network_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_wv_network_table, &QTableWidget::customContextMenuRequested, this, &MainWindow::onNetworkTableContextMenu);
    connect(m_wv_network_table, &QTableWidget::itemSelectionChanged, this, &MainWindow::onNetworkRowSelected);
    m_wv_network_table->setStyleSheet(
        "QTableWidget { background: #090e15; alternate-background-color: #0d141e; color: #c2d1df; border: 1px solid #1e2c3d; border-radius: 7px; font-family: 'Menlo', monospace; font-size: 11px; } "
        "QHeaderView::section { background: #121c29; color: #8ba2b9; padding: 4px; border: none; font-weight: bold; font-size: 11px; }"
    );

    // Splitter: Table (Top) & Detail Panel (Bottom)
    m_wv_net_splitter = new QSplitter(Qt::Vertical);
    m_wv_net_splitter->setChildrenCollapsible(false);
    m_wv_net_splitter->addWidget(m_wv_network_table);

    m_wv_net_detail_panel = qobject_cast<QFrame*>(buildNetworkDetailPanel());
    m_wv_net_detail_panel->setVisible(false);
    m_wv_net_splitter->addWidget(m_wv_net_detail_panel);

    n_layout->addWidget(m_wv_net_splitter, 1);
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
        if (m_wv_net_status_label) {
            m_wv_net_status_label->setText("未选择设备 · 请先选择已连接的设备");
        }
        return;
    }

    if (m_wv_net_status_label) {
        m_wv_net_status_label->setText("正在检测设备中的 WebView 实例…");
    }
    statusBar()->showMessage("正在检测设备中的 WebView 实例…", 3000);

    std::thread([this, s]() {
        // 1. 先确保设备开启了 ArkWeb DevTools 系统参数
        {
            std::string ignored;
            m_hdc->run({"shell", "param set web.debug.devtools true 2>/dev/null"}, s, 2000);
        }

        // 2. 扫描 /proc/net/unix 中的 WebView unix socket 实例
        auto instances = m_webview->scanInstances(s);

        // 3. 构建候选目标列表：unix socket 优先，tcp:9222 兜底
        std::vector<std::string> candidates;
        for (const auto& inst : instances) {
            candidates.push_back(inst.target);
        }
        // OpenHarmony ArkWeb 默认监听 tcp:9222
        bool hasTcp9222 = false;
        for (const auto& c : candidates) {
            if (c == "tcp:9222") { hasTcp9222 = true; break; }
        }
        if (!hasTcp9222) candidates.push_back("tcp:9222");

        int localPort = 9222;
        bool forwarded = false;

        for (const auto& target : candidates) {
            if (m_webview->forwardTarget(s, target, localPort)) {
                forwarded = true;
                break;
            }
        }

        QMetaObject::invokeMethod(this, [this, forwarded, localPort]() {
            if (!forwarded) {
                showToast("当前设备未检测到运行中的 WebView 界面");
                statusBar()->showMessage("未检测到运行中的 WebView 界面，请确认应用已启动并在前台加载网页", 5000);
                if (m_wv_net_status_label) {
                    m_wv_net_status_label->setText("未检测到 WebView 界面 · 请在设备上打开 Web 界面后点击「重新检测」");
                }
                return;
            }

            m_webview->fetchPages(localPort);
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

    if (m_wv_filter_combo) {
        m_wv_filter_combo->setVisible(index == 0);
    }
    if (m_wv_filter_input) {
        m_wv_filter_input->setPlaceholderText(index == 0 ? "过滤日志内容或关键词…" : "过滤请求 URL、方法或路径…");
    }
    if (index == 1) {
        filterNetworkTable();
    }
}

// ==========================================
// Device Discovery & Status Events
// ==========================================
void MainWindow::onDevicesUpdated(const std::vector<Device>& devices) {
    m_cached_devices = devices;
    QString currentSel = m_device_combo->currentData().toString();
    m_device_combo->blockSignals(true);
    m_device_combo->clear();

    std::unordered_set<std::string> connected_serials;
    int foundIdx = -1;
    for (size_t i = 0; i < devices.size(); ++i) {
        std::string s = devices[i].serial;
        connected_serials.insert(s);
        QString text;
        if (s.find(':') != std::string::npos) {
            std::string host = s.substr(0, s.find(':'));
            connected_serials.insert(host);
            NetworkDeviceDialog::saveDevice(host, "", 8710);
            text = QString("🟢 %1").arg(QString::fromStdString(s));
        } else {
            std::string id_part = s.length() > 8 ? s.substr(s.length() - 6) : s;
            text = QString("🟢 USB: %1").arg(QString::fromStdString(id_part));
        }
        m_device_combo->addItem(text, QString::fromStdString(s));
        m_device_combo->setItemData(static_cast<int>(i),
            QString("%1\n型号: %2").arg(
                QString::fromStdString(s),
                QString::fromStdString(devices[i].model.empty() ? s : devices[i].model)
            ), Qt::ToolTipRole);

        if (!m_pending_auto_start_serial.empty() && s == m_pending_auto_start_serial) {
            foundIdx = static_cast<int>(i);
        } else if (foundIdx < 0 && QString::fromStdString(s) == currentSel) {
            foundIdx = static_cast<int>(i);
        }
    }

    // Append saved offline network devices from saved_devices.json
    auto saved_list = NetworkDeviceDialog::loadSavedDevices();
    for (const auto& saved : saved_list) {
        if (saved.ip.empty()) continue;
        std::string ep = saved.ip + ":" + std::to_string(saved.port);
        if (connected_serials.find(saved.ip) == connected_serials.end() &&
            connected_serials.find(ep) == connected_serials.end()) {
            int idx = m_device_combo->count();
            m_device_combo->addItem(QString("⚪ %1").arg(QString::fromStdString(ep)), QString::fromStdString(ep));
            m_device_combo->setItemData(idx,
                QString("%1\n选择此项将切换至 %1 (自动断开其他设备)").arg(QString::fromStdString(ep)),
                Qt::ToolTipRole);
            if (!m_pending_auto_start_serial.empty() && ep == m_pending_auto_start_serial) {
                foundIdx = idx;
            } else if (foundIdx < 0 && QString::fromStdString(ep) == currentSel) {
                foundIdx = idx;
            }
        }
    }

    if (foundIdx >= 0) {
        m_device_combo->setCurrentIndex(foundIdx);
    } else if (m_device_combo->count() > 0) {
        m_device_combo->setCurrentIndex(0);
    }
    m_device_combo->blockSignals(false);

    if (devices.empty()) {
        renderEmptyInfo();
        m_state_badge->setText("待连接");
        m_hdc_target_badge->setText("目标: 无");
        m_hdc_target_prefix->setText("");
        m_screen->clear();
        m_screen->setStatusMessage("未连接设备，请点击右上角设备维护中心连接设备");
        stopMirror(false);
        return;
    }

    if (m_device_combo->count() > 0) {
        onDeviceSelected(m_device_combo->currentIndex());
    }

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

void MainWindow::onDeviceSelected(int index) {
    if (index < 0) return;
    std::string s = currentSerial();
    if (s.empty()) {
        m_hdc_target_badge->setText("目标: 无");
        m_hdc_target_prefix->setText("");
        clearHiLog();
        if (m_hilog) {
            m_hilog->stop();
            m_hilog.reset();
        }
        return;
    }

    m_hdc_target_badge->setText(QString("目标: %1").arg(QString::fromStdString(s)));
    m_hdc_target_prefix->setText(QString("-t %1").arg(QString::fromStdString(s)));

    bool is_connected = false;
    for (const auto& d : m_cached_devices) {
        if (d.serial == s || (s.find(':') != std::string::npos && d.serial.rfind(s.substr(0, s.find(':')), 0) == 0)) {
            is_connected = true;
            break;
        }
    }

    if (is_connected) {
        bool device_changed = (m_active_device_serial != s);
        m_active_device_serial = s;

        if (device_changed) {
            // 切换设备后，缓存日志清空，从新展示当前设备的日志信息
            restartHiLogForCurrentDevice();

            if (m_projecting) {
                stopMirror(false);
                QTimer::singleShot(100, this, [this]() {
                    startMirror();
                });
            }
        }

        if (m_pages->currentIndex() == 0 && !m_info_loading) {
            refreshDeviceInfo();
        }
    }
}

void MainWindow::onDeviceComboActivated(int index) {
    if (index < 0) return;
    std::string s = currentSerial();
    if (s.empty()) return;

    bool is_connected = false;
    for (const auto& d : m_cached_devices) {
        if (d.serial == s || (s.find(':') != std::string::npos && d.serial.rfind(s.substr(0, s.find(':')), 0) == 0)) {
            is_connected = true;
            break;
        }
    }

    if (!is_connected && s.find(':') != std::string::npos) {
        m_active_device_serial = s;
        switchToSingleDevice(s);
        return;
    }

    if (!m_active_device_serial.empty() && m_active_device_serial != s &&
        (s.find(':') != std::string::npos || m_active_device_serial.find(':') != std::string::npos)) {
        m_active_device_serial = s;
        switchToSingleDevice(s);
        return;
    }

    onDeviceSelected(index);
}

void MainWindow::switchToSingleDevice(const std::string& endpoint) {
    if (!m_hdc) return;
    std::string target = endpoint;
    while (!target.empty() && (target.front() == ' ' || target.front() == '\t')) target.erase(0, 1);
    while (!target.empty() && (target.back() == ' ' || target.back() == '\t')) target.pop_back();
    if (target.empty()) return;

    std::string clean_target = target;
    if (size_t col = target.find(':'); col != std::string::npos) {
        clean_target = target.substr(0, col);
    }

    m_switch_generation++;
    uint64_t gen = m_switch_generation;
    m_active_device_serial = target;

    // 1. Immediately stop mirror session and display switching message
    m_user_stopped_mirror = false;
    stopMirror(false);
    m_screen->clear();
    m_screen->setStatusMessage(QString("正在连接设备 %1…").arg(QString::fromStdString(target)));

    // 2. Status badge and status bar
    m_state_badge->setText("正在切换…");
    m_state_badge->setStyleSheet("color: #f3c969; background: #3d2f10; border: 1px solid #735515; padding: 4px 10px; border-radius: 9px; font-size: 12px;");
    statusBar()->showMessage(QString("正在断开旧设备并连接至 %1，请稍候…").arg(QString::fromStdString(target)));
    m_device_combo->setEnabled(false);

    // 3. Clear old device info
    m_cached_info = DeviceInfo{};
    m_info_badge->setText("正在加载…");
    renderEmptyInfo(QString("正在查询设备 %1 的硬件与应用信息…").arg(QString::fromStdString(target)));
    m_btn_refresh_info->setEnabled(false);

    // 4. Reset logs & HDC target
    clearHiLog();
    if (m_hilog) {
        m_hilog->stop();
        m_hilog.reset();
    }
    m_hdc_target_badge->setText(QString("目标: %1 (连接中…)").arg(QString::fromStdString(target)));

    // Execute in background worker thread
    std::thread([this, target, clean_target, gen]() {
        if (gen != m_switch_generation) return;

        // Disconnect other network devices (single device exclusive policy)
        auto current_devs = m_hdc->listDevices();
        for (const auto& d : current_devs) {
            if (d.serial.find(':') != std::string::npos) {
                std::string clean_d = d.serial.substr(0, d.serial.find(':'));
                if (clean_d != clean_target && d.serial != target) {
                    m_hdc->run({"tconn", d.serial, "-remove"});
                }
            }
        }

        if (gen != m_switch_generation) return;

        // Connect to target
        if (target.find(':') != std::string::npos) {
            m_hdc->run({"tconn", target});
            NetworkDeviceDialog::saveDevice(clean_target, "", 8710);
        }

        if (gen != m_switch_generation) return;

        // Query freshly updated devices right in the worker thread
        auto fresh_devs = m_hdc->listDevices();

        QMetaObject::invokeMethod(this, [this, target, fresh_devs, gen]() {
            if (gen != m_switch_generation) return;

            m_device_combo->setEnabled(true);
            m_state_badge->setStyleSheet("");
            m_pending_auto_start_serial = target;
            m_user_stopped_mirror = false;

            bool is_online = false;
            for (const auto& d : fresh_devs) {
                if (d.serial == target || (target.find(':') != std::string::npos && d.serial.rfind(target.substr(0, target.find(':')), 0) == 0)) {
                    is_online = true;
                    break;
                }
            }

            if (!is_online) {
                m_screen->setStatusMessage(QString("连接失败：无法连接至 %1，请检查设备 IP 及网络").arg(QString::fromStdString(target)));
                statusBar()->showMessage(QString("连接设备 %1 失败，请检查 IP 是否正确且处于同一局域网").arg(QString::fromStdString(target)), 4000);
                showToast(QString("连接失败: 无法连接至 %1").arg(QString::fromStdString(target)));
                m_pending_auto_start_serial.clear();
            } else {
                m_screen->setStatusMessage(QString("已连接 %1，正在同步画面…").arg(QString::fromStdString(target)));
                statusBar()->showMessage(QString("已稳定连接设备 %1，正在投屏…").arg(QString::fromStdString(target)), 3500);
            }

            onDevicesUpdated(fresh_devs);
        });
    }).detach();
}

void MainWindow::refreshDevices() {
    m_dev_mgr->refresh(true);
    showToast("正在扫描在线设备…");
}

void MainWindow::disconnectCurrentDevice() {
    std::string s = currentSerial();
    if (s.empty()) return;
    m_user_stopped_mirror = true;
    m_pending_auto_start_serial.clear();
    m_active_device_serial.clear();
    stopMirror(true);
    m_hdc->executeAsync({"tconn", s, "-remove"}, "", [this, s](const CommandResult&){
        refreshDevices();
        showToast(QString("已断开设备: %1").arg(QString::fromStdString(s)));
    });
}

void MainWindow::openDeviceCenter() {
    std::vector<std::string> connected_serials;
    for (const auto& d : m_cached_devices) {
        connected_serials.push_back(d.serial);
    }
    std::string cur = currentSerial();

    auto* dlg = new NetworkDeviceDialog(this, m_hdc, connected_serials, cur);
    dlg->setOnConnect([this](const std::string& ep) {
        switchToSingleDevice(ep);
    });
    dlg->setOnSelect([this](const std::string& ep) {
        switchToSingleDevice(ep);
    });
    dlg->setOnDisconnect([this](const std::string& ep) {
        std::string cur = currentSerial();
        if (!cur.empty() && (cur == ep || cur.rfind(ep, 0) == 0 || ep.rfind(cur, 0) == 0)) {
            m_user_stopped_mirror = true;
            m_pending_auto_start_serial.clear();
            m_active_device_serial.clear();
            stopMirror(true);
        }
        m_hdc->executeAsync({"tconn", ep, "-remove"}, "", [this](const CommandResult&) {
            refreshDevices();
        });
    });
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

    bool is_connected = false;
    for (const auto& d : m_cached_devices) {
        if (d.serial == s || (s.find(':') != std::string::npos && d.serial.rfind(s.substr(0, s.find(':')), 0) == 0)) {
            is_connected = true;
            break;
        }
    }
    if (!is_connected) {
        showToast("所选设备当前未连接，请先连接设备");
        return;
    }

    m_user_stopped_mirror = false;

    if (m_projecting) return;
    m_projecting = true;
    m_start_btn->hide();
    m_stop_btn->show();
    m_state_badge->setText("连接中…");
    m_screen->setActive(true);

    // Wake up device screen asynchronously
    m_hdc->wakeupAsync(s);

    int bitrates[] = {20000000, 12000000, 6000000, 2500000};
    int br = bitrates[std::clamp(m_bitrate_idx, 0, 3)];

    Device dev;
    dev.serial = s;
    for (const auto& d : m_cached_devices) {
        if (d.serial == s) {
            dev = d;
            break;
        }
    }

    m_stream = std::make_unique<StreamSession>(m_hdc, dev, 60, br, 0, this);
    m_stream->setQuality(m_bitrate_idx, br);

    connect(m_stream.get(), &StreamSession::frameReady, this, [this](const DecodedFrame& f){
        m_screen->updateFrame(f);
        updateStreamStatusBadge(f.width, f.height);
    });

    connect(m_stream.get(), &StreamSession::statusChanged, this, [this](const QString& status){
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

    // Start HiLog simultaneously if not already running for this device
    if (!m_hilog) {
        m_hilog = std::make_unique<HiLogSession>(m_hdc->path(), s, "", "all", this);
        connect(m_hilog.get(), &HiLogSession::entriesReady, this, &MainWindow::onHiLogEntries);
        m_hilog->start();
    }

    showToast("已启动投屏引擎");
}

void MainWindow::stopMirror(bool userInitiated) {
    if (userInitiated) {
        m_user_stopped_mirror = true;
        m_pending_auto_start_serial.clear();
    }
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
        showToast("投屏已停止");
    }
}

void MainWindow::onQualityChanged(int index) {
    m_bitrate_idx = std::clamp(index, 0, 3);
    m_settings->setValue("mirror_quality_idx", m_bitrate_idx);
    int bitrates[] = {20000000, 12000000, 6000000, 2500000};
    int br = bitrates[m_bitrate_idx];

    QString qualityName = (m_quality_combo ? m_quality_combo->currentText() : QString::number(br / 1000000) + "M");

    if (m_projecting && m_stream) {
        m_stream->setQuality(m_bitrate_idx, br);
        if (!m_stream->isFallbackSnapshot()) {
            showToast(QString("正在实时热重载视频流码率: %1…").arg(qualityName));
            statusBar()->showMessage(QString("正在实时热重载视频流码率: %1…").arg(qualityName), 3000);
            m_stream->start();
        } else {
            QString tierDesc;
            if (m_bitrate_idx == 0) {
                tierDesc = "原生原画 (100% 原始分辨率点对点无损输出，最高清晰度)";
            } else if (m_bitrate_idx == 1) {
                tierDesc = "超清 900P (清晰度与流畅度均衡推荐)";
            } else if (m_bitrate_idx == 2) {
                tierDesc = "高清 720P (缩放加速传输，延迟更低)";
            } else {
                tierDesc = "标清 540P (540x960 极速传输，极限流畅)";
            }
            showToast(QString("快照流画质已实时切换: %1\n%2").arg(qualityName, tierDesc));
            statusBar()->showMessage(QString("兼容快照流: 已实时应用 %1 (%2)").arg(qualityName, tierDesc), 4000);
        }
    } else {
        showToast(QString("已设置默认画质: %1").arg(qualityName));
    }
}

void MainWindow::rotateDisplay() {
    m_user_rotation = (m_user_rotation + 90) % 360;
    m_screen->setRotation(m_user_rotation);
    showToast(QString("旋转画面: %1°").arg(m_user_rotation));
}

void MainWindow::resizeToPixelPerfect() {
    int dev_w = m_screen->deviceWidth();
    int dev_h = m_screen->deviceHeight();
    if (dev_w <= 0 || dev_h <= 0) {
        if (!m_screen->image().isNull()) {
            dev_w = m_screen->image().width();
            dev_h = m_screen->image().height();
        }
    }
    if (dev_w <= 0 || dev_h <= 0) {
        showToast("暂无画面，请先连接并开始投屏");
        return;
    }

    if (m_user_rotation % 180 != 0) {
        std::swap(dev_w, dev_h);
    }

    const qreal dpr = m_screen->devicePixelRatioF();
    int target_w = static_cast<int>(dev_w / dpr);
    int target_h = static_cast<int>(dev_h / dpr);

    QScreen* scr = screen();
    if (scr) {
        QRect avail = scr->availableGeometry();
        int max_h = avail.height() - 140;
        if (target_h > max_h) {
            double ratio = static_cast<double>(max_h) / target_h;
            target_h = max_h;
            target_w = static_cast<int>(target_w * ratio);
        }
    }

    if (m_current_layout == "mirror_only") {
        int chrome_h = height() - m_screen->height();
        int chrome_w = width() - m_screen->width();
        resize(target_w + std::max(0, chrome_w), target_h + std::max(0, chrome_h));
    } else {
        if (m_workspace_splitter && m_workspace_splitter->orientation() == Qt::Horizontal) {
            int total_w = m_workspace_splitter->width();
            if (total_w < target_w + 300) {
                resize(target_w + 420 + (width() - m_workspace_splitter->width()), height());
            }
            m_workspace_splitter->setSizes({target_w, std::max(300, width() - target_w - 200)});
        }
    }

    showToast(QString("已适配 1:1 点对点显示: %1 × %2").arg(dev_w).arg(dev_h));
}

void MainWindow::updateStreamStatusBadge(int w, int h) {
    if (!m_projecting) return;
    QString streamType = (m_stream && m_stream->isFallbackSnapshot()) ? "快照流" : "高清流";
    QString qualityLabel = "原画";
    int minDim = std::min(w, h);
    if (minDim >= 1080) {
        qualityLabel = "原画点对点";
    } else if (minDim >= 900) {
        qualityLabel = "超清 900P";
    } else if (minDim >= 720) {
        qualityLabel = "高清 720P";
    } else {
        qualityLabel = "标清 540P";
    }
    m_state_badge->setText(QString("%1 · %2x%3 (%4)").arg(streamType).arg(w).arg(h).arg(qualityLabel));
    m_state_badge->setStyleSheet("color: #53d6b5; background: #0c2724; border: 1px solid #1a4d46; padding: 4px 10px; border-radius: 9px; font-size: 12px;");
}

void MainWindow::saveScreenshot() {
    std::string s = currentSerial();
    if (s.empty()) {
        showToast("请先选择已连接的设备");
        return;
    }

    QString filename = QString("screenshot_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    QString destDir = QDir::homePath() + "/Downloads";
    QDir().mkpath(destDir);
    QString dest = destDir + "/" + filename;

    auto onSaveSuccess = [this, dest, destDir]() {
        HC_LOG_INFO(QString("Screenshot saved successfully to %1").arg(dest));
        showToast(QString("截图已保存至: %1").arg(dest));
        statusBar()->showMessage(QString("截图成功保存至: %1").arg(dest), 6000);

#ifdef Q_OS_MAC
        QProcess::startDetached("open", {"-R", dest});
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(destDir));
#endif

        auto* dlg = new QDialog(this);
        dlg->setWindowTitle("截图保存成功 · HarmonyCast");
        dlg->setFixedWidth(520);
        dlg->setStyleSheet("QDialog { background: #0c1219; color: #dbe5ee; }");
        auto* l = new QVBoxLayout(dlg);
        l->setContentsMargins(20, 20, 20, 16);
        l->setSpacing(12);

        auto* title = IconManager::label("设备屏幕截图已保存", "CardTitle");
        title->setStyleSheet("font-size: 15px; font-weight: bold; color: #5ce0c1;");
        l->addWidget(title);

        auto* desc = IconManager::label("截图文件已成功保存到本地目录，并已在访达 (Finder) 中自动定位该文件：", "Muted");
        desc->setWordWrap(true);
        l->addWidget(desc);

        auto* path_edit = new QLineEdit(dest, dlg);
        path_edit->setReadOnly(true);
        path_edit->setStyleSheet("QLineEdit { background: #070c13; color: #8ee2ff; border: 1px solid #233446; border-radius: 4px; padding: 6px 8px; font-family: 'Menlo', monospace; font-size: 12px; }");
        l->addWidget(path_edit);

        auto* btn_bar = new QHBoxLayout();
        auto* btn_copy = IconManager::button("复制路径", "", "copy");
        connect(btn_copy, &QPushButton::clicked, dlg, [this, dest]{
            copyToClipboard(dest, "截图本地绝对路径");
        });

        auto* btn_open = IconManager::button("打开所在文件夹", "Primary", "folder");
        connect(btn_open, &QPushButton::clicked, dlg, [dest, destDir]{
#ifdef Q_OS_MAC
            QProcess::startDetached("open", {"-R", dest});
#else
            QDesktopServices::openUrl(QUrl::fromLocalFile(destDir));
#endif
        });

        auto* btn_close = IconManager::button("完成", "");
        connect(btn_close, &QPushButton::clicked, dlg, &QDialog::accept);

        btn_bar->addWidget(btn_copy);
        btn_bar->addWidget(btn_open);
        btn_bar->addStretch();
        btn_bar->addWidget(btn_close);
        l->addLayout(btn_bar);

        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    };

    if (m_projecting && m_screen && !m_screen->image().isNull()) {
        QImage shot = m_screen->image();
        if (m_user_rotation != 0) {
            QTransform t;
            t.rotate(m_user_rotation);
            shot = shot.transformed(t, Qt::SmoothTransformation);
        }
        if (shot.save(dest, "PNG")) {
            onSaveSuccess();
            return;
        }
    }

    // Fallback: capture directly via device snapshot_display (OpenHarmony requires .jpeg extension)
    std::string remote_jpeg = "/data/local/tmp/harmonycast_snap.jpeg";
    QString local_temp = QDir::tempPath() + QString("/snap_%1.jpeg").arg(QDateTime::currentMSecsSinceEpoch());

    m_hdc->executeAsync({"shell", "snapshot_display", "-f", remote_jpeg}, s, [this, s, remote_jpeg, local_temp, dest, onSaveSuccess](const CommandResult& r){
        if (r.success()) {
            m_hdc->executeAsync({"file", "recv", remote_jpeg, local_temp.toStdString()}, s, [this, s, remote_jpeg, local_temp, dest, onSaveSuccess](const CommandResult& r2){
                bool ok = false;
                if (r2.success() && QFile::exists(local_temp)) {
                    QImage img;
                    if (img.load(local_temp)) {
                        ok = img.save(dest, "PNG");
                    }
                    QFile::remove(local_temp);
                }
                m_hdc->executeAsync({"shell", "rm", "-f", remote_jpeg}, s, nullptr);

                if (ok) {
                    onSaveSuccess();
                } else {
                    showToast("保存截图文件失败");
                }
            });
        } else {
            showToast("设备端截屏执行失败");
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
    if (!filterLvl.isEmpty()) {
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
    QString lvl = QString::fromStdString(e.levelString()).leftJustified(5, ' ');

    if (!tag.isEmpty()) {
        return QString("%1  %2  [%3] %4\n")
            .arg(QString::fromStdString(e.timestamp),
                 lvl,
                 tag,
                 msg);
    } else {
        return QString("%1  %2  %3\n")
            .arg(QString::fromStdString(e.timestamp),
                 lvl,
                 msg);
    }
}

void MainWindow::onHiLogEntries(const std::vector<HiLogEntry>& entries) {
    if (m_pause_cb && m_pause_cb->isChecked()) return;

    QString filterPkg = m_package_combo ? m_package_combo->currentText().trimmed() : "";
    QString filterLvl = m_level_combo ? m_level_combo->currentData().toString() : "";
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
        bool wasAtBottom = (m_log_view->verticalScrollBar()->value() >= m_log_view->verticalScrollBar()->maximum() - 25);
        m_log_view->appendPlainText(newLines.trimmed());
        if (m_autoscroll_cb && m_autoscroll_cb->isChecked() && wasAtBottom) {
            m_log_view->verticalScrollBar()->setValue(m_log_view->verticalScrollBar()->maximum());
        }
        m_visible_log_count += matchedNew;
        if (m_log_count) {
            m_log_count->setText(QString("显示 %1 条 · 缓存 %2 条")
                .arg(m_visible_log_count)
                .arg(m_cached_logs.size()));
        }
    }
}

void MainWindow::renderHiLog() {
    if (!m_log_view) return;

    QString filterPkg = m_package_combo ? m_package_combo->currentText().trimmed() : "";
    QString filterLvl = m_level_combo ? m_level_combo->currentData().toString() : "";
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
        m_log_count->setText(QString("显示 %1 条 · 缓存 %2 条")
            .arg(visibleCount)
            .arg(m_cached_logs.size()));
    }
}

void MainWindow::clearHiLog() {
    m_cached_logs.clear();
    m_visible_log_count = 0;
    if (m_log_view) m_log_view->clear();
    if (m_log_count) m_log_count->setText("显示 0 条 · 缓存 0 条");
}

void MainWindow::restartHiLogForCurrentDevice() {
    clearHiLog();
    std::string s = currentSerial();
    if (s.empty() || !m_hdc) return;

    if (m_hilog) {
        m_hilog->stop();
        m_hilog.reset();
    }

    m_hilog = std::make_unique<HiLogSession>(m_hdc->path(), s, "", "all", this);
    connect(m_hilog.get(), &HiLogSession::entriesReady, this, &MainWindow::onHiLogEntries);
    m_hilog->start();
    HC_LOG_INFO(QString("HiLog session freshly restarted for device %1").arg(QString::fromStdString(s)));
}

void MainWindow::onHiLogContextMenu(const QPoint& pos) {
    if (!m_log_view) return;

    QMenu menu(this);
    menu.setStyleSheet(
        "QMenu { background-color: #0c131c; border: 1px solid #1c2b3d; border-radius: 6px; padding: 4px; color: #cfdfeb; font-size: 13px; } "
        "QMenu::item { padding: 6px 22px 6px 12px; border-radius: 4px; } "
        "QMenu::item:selected { background-color: #1a2f47; color: #5ce0c1; } "
        "QMenu::separator { height: 1px; background: #1c2b3d; margin: 4px 6px; }"
    );

    auto* clearAction = menu.addAction(IconManager::getIcon("clear"), "清空日志 (Clear)");
    connect(clearAction, &QAction::triggered, this, &MainWindow::clearHiLog);

    menu.addSeparator();

    auto* copyAction = menu.addAction(IconManager::getIcon("copy"), "复制选中 (Copy)");
    copyAction->setEnabled(m_log_view->textCursor().hasSelection());
    connect(copyAction, &QAction::triggered, m_log_view, &QPlainTextEdit::copy);

    auto* selectAllAction = menu.addAction("全选 (Select All)");
    connect(selectAllAction, &QAction::triggered, m_log_view, &QPlainTextEdit::selectAll);

    menu.addSeparator();

    auto* exportAction = menu.addAction(IconManager::getIcon("export"), "导出日志 (Export)…");
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportHiLog);

    menu.exec(m_log_view->mapToGlobal(pos));
}

void MainWindow::onWebViewConsoleContextMenu(const QPoint& pos) {
    if (!m_wv_console_view) return;

    QMenu menu(this);
    menu.setStyleSheet(
        "QMenu { background-color: #0c131c; border: 1px solid #1c2b3d; border-radius: 6px; padding: 4px; color: #cfdfeb; font-size: 13px; } "
        "QMenu::item { padding: 6px 22px 6px 12px; border-radius: 4px; } "
        "QMenu::item:selected { background-color: #1a2f47; color: #5ce0c1; } "
        "QMenu::separator { height: 1px; background: #1c2b3d; margin: 4px 6px; }"
    );

    auto* clearAction = menu.addAction(IconManager::getIcon("clear"), "清空控制台 (Clear)");
    connect(clearAction, &QAction::triggered, this, &MainWindow::clearWebViewConsole);

    menu.addSeparator();

    auto* copyAction = menu.addAction(IconManager::getIcon("copy"), "复制选中 (Copy)");
    copyAction->setEnabled(m_wv_console_view->textCursor().hasSelection());
    connect(copyAction, &QAction::triggered, m_wv_console_view, &QPlainTextEdit::copy);

    auto* selectAllAction = menu.addAction("全选 (Select All)");
    connect(selectAllAction, &QAction::triggered, m_wv_console_view, &QPlainTextEdit::selectAll);

    menu.exec(m_wv_console_view->mapToGlobal(pos));
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
    if (m_wv_pause && m_wv_pause->isChecked()) return;

    m_cached_wv_logs.push_back(msg);
    if (m_cached_wv_logs.size() > 5000) {
        m_cached_wv_logs.erase(m_cached_wv_logs.begin(), m_cached_wv_logs.begin() + 500);
    }
    m_wv_console_count++;

    // Apply current filter before appending
    QString levelFilter = m_wv_filter_combo ? m_wv_filter_combo->currentText() : "";
    QString query = m_wv_filter_input ? m_wv_filter_input->text().trimmed() : "";

    bool levelMatch = true;
    if (levelFilter == "警告") {
        std::string l = msg.level;
        levelMatch = (l == "warn" || l == "warning");
    } else if (levelFilter == "错误") {
        levelMatch = (msg.level == "error");
    } else if (levelFilter == "日志") {
        std::string l = msg.level;
        levelMatch = (l == "log" || l == "info" || l == "debug");
    }

    bool queryMatch = query.isEmpty() ||
        QString::fromStdString(msg.text).contains(query, Qt::CaseInsensitive) ||
        QString::fromStdString(msg.source_url).contains(query, Qt::CaseInsensitive);

    if (levelMatch && queryMatch) {
        QString lvl = QString::fromStdString(msg.level).toUpper();
        QString ts = QString::fromStdString(msg.timestamp);
        QString text = QString::fromStdString(msg.text);
        QString src = QString::fromStdString(msg.source_url);
        QString formatted;
        if (!src.isEmpty()) {
            formatted = QString("%1 [%2] %3 (%4:%5)").arg(ts, lvl, text, src, QString::number(msg.line_number));
        } else {
            formatted = QString("%1 [%2] %3").arg(ts, lvl, text);
        }

        if (m_wv_console_view) {
            bool wasAtBottom = (m_wv_console_view->verticalScrollBar()->value() >=
                                m_wv_console_view->verticalScrollBar()->maximum() - 25);
            m_wv_console_view->appendPlainText(formatted);
            if (m_wv_autoscroll && m_wv_autoscroll->isChecked() && wasAtBottom) {
                m_wv_console_view->verticalScrollBar()->setValue(m_wv_console_view->verticalScrollBar()->maximum());
            }
        }
    }

    if (m_wv_net_status_label) {
        m_wv_net_status_label->setText(QString("%1 条日志 · %2 个请求").arg(m_wv_console_count).arg(m_wv_network_count));
    }
}

void MainWindow::renderWebViewConsole() {
    if (!m_wv_console_view) return;

    QString levelFilter = m_wv_filter_combo ? m_wv_filter_combo->currentText() : "";
    QString query = m_wv_filter_input ? m_wv_filter_input->text().trimmed() : "";

    QStringList lines;
    for (const auto& msg : m_cached_wv_logs) {
        bool levelMatch = true;
        if (levelFilter == "警告") {
            std::string l = msg.level;
            levelMatch = (l == "warn" || l == "warning");
        } else if (levelFilter == "错误") {
            levelMatch = (msg.level == "error");
        } else if (levelFilter == "日志") {
            std::string l = msg.level;
            levelMatch = (l == "log" || l == "info" || l == "debug");
        }

        bool queryMatch = query.isEmpty() ||
            QString::fromStdString(msg.text).contains(query, Qt::CaseInsensitive) ||
            QString::fromStdString(msg.source_url).contains(query, Qt::CaseInsensitive);

        if (levelMatch && queryMatch) {
            QString lvl = QString::fromStdString(msg.level).toUpper();
            QString ts = QString::fromStdString(msg.timestamp);
            QString text = QString::fromStdString(msg.text);
            QString src = QString::fromStdString(msg.source_url);
            if (!src.isEmpty()) {
                lines << QString("%1 [%2] %3 (%4:%5)").arg(ts, lvl, text, src, QString::number(msg.line_number));
            } else {
                lines << QString("%1 [%2] %3").arg(ts, lvl, text);
            }
        }
    }

    m_wv_console_view->setPlainText(lines.join('\n'));
    if (m_wv_autoscroll && m_wv_autoscroll->isChecked()) {
        m_wv_console_view->verticalScrollBar()->setValue(m_wv_console_view->verticalScrollBar()->maximum());
    }
    if (m_wv_net_status_label) {
        m_wv_net_status_label->setText(QString("%1 条日志 · %2 个请求").arg(m_wv_console_count).arg(m_wv_network_count));
    }
}

QString MainWindow::normalizeResourceType(const std::string& type, const std::string& mime, const std::string& url) {
    QString t = QString::fromStdString(type).toUpper();
    QString m = QString::fromStdString(mime).toLower();
    QString u = QString::fromStdString(url).toLower();

    if (t == "XHR" || t == "FETCH" || t == "FETCH/XHR") return "Fetch/XHR";
    if (t == "SCRIPT") return "JS";
    if (t == "STYLESHEET") return "CSS";
    if (t == "IMAGE") return "Img";
    if (t == "DOCUMENT") return "Doc";
    if (t == "FONT") return "Font";

    if (m.contains("json") || m.contains("xml") || u.contains("/api/") || u.contains("/ajax/")) return "Fetch/XHR";
    if (m.contains("javascript") || u.endsWith(".js") || u.contains(".js?")) return "JS";
    if (m.contains("css") || u.endsWith(".css") || u.contains(".css?")) return "CSS";
    if (m.startsWith("image/") || u.endsWith(".png") || u.endsWith(".jpg") || u.endsWith(".jpeg") || u.endsWith(".svg") || u.endsWith(".gif") || u.endsWith(".webp") || u.endsWith(".ico")) return "Img";
    if (m.contains("html") || u.endsWith(".html") || u.endsWith(".htm")) return "Doc";
    if (m.contains("font") || u.endsWith(".woff") || u.endsWith(".woff2") || u.endsWith(".ttf")) return "Font";

    return t.isEmpty() ? "Other" : QString::fromStdString(type);
}

QWidget* MainWindow::buildNetworkDetailPanel() {
    auto* panel = new QFrame();
    panel->setObjectName("NetDetailPanel");
    panel->setStyleSheet(
        "QFrame#NetDetailPanel {"
        "  background: #090e15;"
        "  border-top: 1px solid #1e2c3d;"
        "}"
    );

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    // Header bar: Method Badge + URL + Status Badge + Copy URL + Close Button
    auto* top_bar = new QHBoxLayout();
    top_bar->setContentsMargins(2, 0, 2, 2);
    top_bar->setSpacing(8);

    m_detail_method_badge = new QLabel("GET");
    m_detail_method_badge->setStyleSheet(
        "background: #17384e; color: #5ce0c1; font-weight: bold; font-size: 11px; padding: 2px 7px; border-radius: 4px;"
    );
    top_bar->addWidget(m_detail_method_badge);

    m_detail_url_label = new QLabel();
    m_detail_url_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detail_url_label->setStyleSheet("color: #dbe5ee; font-weight: 500; font-size: 11px; font-family: 'Menlo', monospace;");
    top_bar->addWidget(m_detail_url_label, 1);

    m_detail_status_badge = new QLabel("200 OK");
    m_detail_status_badge->setStyleSheet(
        "background: #112e23; color: #5ce0c1; font-weight: bold; font-size: 11px; padding: 2px 7px; border-radius: 4px;"
    );
    top_bar->addWidget(m_detail_status_badge);

    auto* btn_copy_url = IconManager::button("复制 URL", "", "copy");
    btn_copy_url->setFixedHeight(24);
    connect(btn_copy_url, &QPushButton::clicked, this, [this]{
        copyToClipboard(m_detail_url_label->text(), "请求 URL");
    });
    top_bar->addWidget(btn_copy_url);

    auto* btn_close = new QPushButton("✕");
    btn_close->setFixedSize(24, 24);
    btn_close->setCursor(Qt::PointingHandCursor);
    btn_close->setStyleSheet(
        "QPushButton { background: transparent; color: #798d9f; border: none; font-size: 13px; font-weight: bold; }"
        "QPushButton:hover { background: #3d1a1f; color: #ff7b72; border-radius: 3px; }"
    );
    connect(btn_close, &QPushButton::clicked, this, [this]{
        m_wv_net_detail_panel->setVisible(false);
        m_wv_network_table->clearSelection();
    });
    top_bar->addWidget(btn_close);

    layout->addLayout(top_bar);

    // Tab Widget
    m_net_detail_tabs = new QTabWidget();
    m_net_detail_tabs->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #1a2737; background: #070c13; border-radius: 4px; } "
        "QTabBar::tab { background: #0f1722; color: #8ba2b9; padding: 5px 12px; border: 1px solid #1a2737; border-bottom: none; border-top-left-radius: 4px; border-top-right-radius: 4px; font-size: 11px; } "
        "QTabBar::tab:selected { background: #070c13; color: #5ce0c1; font-weight: bold; border-color: #2d6b91; } "
        "QTabBar::tab:hover:!selected { background: #162232; color: #dbe5ee; }"
    );

    // Tab 1: 标头 (Headers)
    m_net_headers_view = new QTextEdit();
    m_net_headers_view->setReadOnly(true);
    m_net_headers_view->setFont(QFont("Menlo", 11));
    m_net_headers_view->setStyleSheet("background: #070c13; color: #dbe5ee; border: none; font-family: 'Menlo', monospace; padding: 6px;");
    m_net_detail_tabs->addTab(m_net_headers_view, "标头 (Headers)");

    // Tab 2: 载荷 (Payload)
    auto* payload_page = new QWidget();
    auto* p_layout = new QVBoxLayout(payload_page);
    p_layout->setContentsMargins(6, 6, 6, 6);
    p_layout->setSpacing(4);

    auto* p_toolbar = new QHBoxLayout();
    p_toolbar->setContentsMargins(0, 0, 0, 0);
    p_toolbar->addWidget(IconManager::label("请求载荷 / 查询参数", "Muted"));
    p_toolbar->addStretch(1);
    auto* btn_copy_payload = IconManager::button("复制载荷", "", "copy");
    btn_copy_payload->setFixedHeight(22);
    connect(btn_copy_payload, &QPushButton::clicked, this, [this]{
        copyToClipboard(m_net_payload_view->toPlainText(), "请求载荷");
    });
    p_toolbar->addWidget(btn_copy_payload);
    p_layout->addLayout(p_toolbar);

    m_net_payload_view = new QPlainTextEdit();
    m_net_payload_view->setReadOnly(true);
    m_net_payload_view->setFont(QFont("Menlo", 11));
    m_net_payload_view->setStyleSheet("background: #070c13; color: #dbe5ee; border: 1px solid #1a2737; border-radius: 4px; padding: 4px;");
    p_layout->addWidget(m_net_payload_view, 1);
    m_net_detail_tabs->addTab(payload_page, "载荷 (Payload)");

    // Tab 3: 响应 (Response)
    auto* resp_page = new QWidget();
    auto* r_layout = new QVBoxLayout(resp_page);
    r_layout->setContentsMargins(6, 6, 6, 6);
    r_layout->setSpacing(4);

    auto* r_toolbar = new QHBoxLayout();
    r_toolbar->setContentsMargins(0, 0, 0, 0);
    m_net_resp_info_label = IconManager::label("响应体", "Muted");
    r_toolbar->addWidget(m_net_resp_info_label);
    r_toolbar->addStretch(1);

    auto* btn_format_json = IconManager::button("格式化 JSON", "", "code");
    btn_format_json->setFixedHeight(22);
    connect(btn_format_json, &QPushButton::clicked, this, &MainWindow::formatResponseJson);
    r_toolbar->addWidget(btn_format_json);

    auto* btn_copy_resp = IconManager::button("复制响应体", "", "copy");
    btn_copy_resp->setFixedHeight(22);
    connect(btn_copy_resp, &QPushButton::clicked, this, [this]{
        copyToClipboard(m_net_response_view->toPlainText(), "响应体");
    });
    r_toolbar->addWidget(btn_copy_resp);
    r_layout->addLayout(r_toolbar);

    m_net_response_view = new QPlainTextEdit();
    m_net_response_view->setReadOnly(true);
    m_net_response_view->setFont(QFont("Menlo", 11));
    m_net_response_view->setStyleSheet("background: #070c13; color: #dbe5ee; border: 1px solid #1a2737; border-radius: 4px; padding: 4px;");
    r_layout->addWidget(m_net_response_view, 1);
    m_net_detail_tabs->addTab(resp_page, "响应 (Response)");

    // Tab 4: cURL
    auto* curl_page = new QWidget();
    auto* c_layout = new QVBoxLayout(curl_page);
    c_layout->setContentsMargins(6, 6, 6, 6);
    c_layout->setSpacing(4);

    auto* c_toolbar = new QHBoxLayout();
    c_toolbar->setContentsMargins(0, 0, 0, 0);
    c_toolbar->addWidget(IconManager::label("终端 cURL 命令", "Muted"));
    c_toolbar->addStretch(1);
    auto* btn_copy_curl = IconManager::button("复制 cURL", "", "copy");
    btn_copy_curl->setFixedHeight(22);
    connect(btn_copy_curl, &QPushButton::clicked, this, [this]{
        copyToClipboard(m_net_curl_view->toPlainText(), "cURL 命令");
    });
    c_toolbar->addWidget(btn_copy_curl);
    c_layout->addLayout(c_toolbar);

    m_net_curl_view = new QPlainTextEdit();
    m_net_curl_view->setReadOnly(true);
    m_net_curl_view->setFont(QFont("Menlo", 11));
    m_net_curl_view->setStyleSheet("background: #070c13; color: #dbe5ee; border: 1px solid #1a2737; border-radius: 4px; padding: 4px;");
    c_layout->addWidget(m_net_curl_view, 1);
    m_net_detail_tabs->addTab(curl_page, "cURL 命令");

    layout->addWidget(m_net_detail_tabs, 1);
    return panel;
}

void MainWindow::onNetworkRowSelected() {
    auto selectedItems = m_wv_network_table->selectedItems();
    if (selectedItems.isEmpty()) return;

    int row = selectedItems[0]->row();
    auto* urlItem = m_wv_network_table->item(row, 2);
    if (!urlItem) return;

    std::string reqId = urlItem->data(Qt::UserRole).toString().toStdString();
    auto it = m_wv_requests.find(reqId);
    if (it == m_wv_requests.end()) return;

    m_current_selected_req_id = reqId;
    populateNetworkDetail(it->second);
}

void MainWindow::populateNetworkDetail(const WebNetworkRequest& req) {
    // 1. Method badge
    QString method = QString::fromStdString(req.method).toUpper();
    m_detail_method_badge->setText(method);
    if (method == "GET") {
        m_detail_method_badge->setStyleSheet("background: #112e23; color: #5ce0c1; font-weight: bold; font-size: 11px; padding: 2px 7px; border-radius: 4px;");
    } else if (method == "POST") {
        m_detail_method_badge->setStyleSheet("background: #172d4a; color: #79c0ff; font-weight: bold; font-size: 11px; padding: 2px 7px; border-radius: 4px;");
    } else if (method == "PUT") {
        m_detail_method_badge->setStyleSheet("background: #362f12; color: #f3c969; font-weight: bold; font-size: 11px; padding: 2px 7px; border-radius: 4px;");
    } else if (method == "DELETE") {
        m_detail_method_badge->setStyleSheet("background: #3d1a1f; color: #ff7b72; font-weight: bold; font-size: 11px; padding: 2px 7px; border-radius: 4px;");
    } else {
        m_detail_method_badge->setStyleSheet("background: #192433; color: #c2d1df; font-weight: bold; font-size: 11px; padding: 2px 7px; border-radius: 4px;");
    }

    // 2. URL label
    m_detail_url_label->setText(QString::fromStdString(req.url));
    m_detail_url_label->setToolTip(QString::fromStdString(req.url));

    // 3. Status badge
    if (req.status_code > 0) {
        QString sText = req.status_text.empty() ? "" : QString(" %1").arg(QString::fromStdString(req.status_text));
        m_detail_status_badge->setText(QString("%1%2").arg(req.status_code).arg(sText));
        if (req.status_code >= 500) {
            m_detail_status_badge->setStyleSheet("background: #35143d; color: #d2a8ff; font-weight: bold; font-size: 11px; padding: 2px 7px; border-radius: 4px;");
        } else if (req.status_code >= 400) {
            m_detail_status_badge->setStyleSheet("background: #3d1a1f; color: #ff7b72; font-weight: bold; font-size: 11px; padding: 2px 7px; border-radius: 4px;");
        } else if (req.status_code >= 300) {
            m_detail_status_badge->setStyleSheet("background: #362f12; color: #f3c969; font-weight: bold; font-size: 11px; padding: 2px 7px; border-radius: 4px;");
        } else {
            m_detail_status_badge->setStyleSheet("background: #112e23; color: #5ce0c1; font-weight: bold; font-size: 11px; padding: 2px 7px; border-radius: 4px;");
        }
    } else {
        m_detail_status_badge->setText("请求中…");
        m_detail_status_badge->setStyleSheet("background: #172433; color: #8ba2b9; font-weight: bold; font-size: 11px; padding: 2px 7px; border-radius: 4px;");
    }

    // 4. Headers Tab
    QString timingStr = (req.elapsed_ms >= 0) ? QString("%1 ms").arg(req.elapsed_ms) : "进行中…";
    QString resType = QString::fromStdString(req.resource_type.empty() ? normalizeResourceType("", req.mime_type, req.url).toStdString() : req.resource_type);
    QString html = "<div style=\"font-family:'Menlo',monospace; font-size:11px; color:#dbe5ee; line-height:1.6;\">";

    // General section
    html += "<div style=\"color:#5ce0c1; font-weight:bold; font-size:12px; margin-bottom:4px;\">常规 (General)</div>";
    html += "<table cellpadding=\"2\" cellspacing=\"0\" style=\"margin-bottom:12px; width:100%;\">";
    html += QString("<tr><td style=\"color:#798d9f; width:120px;\">请求 URL:</td><td style=\"color:#dbe5ee; word-break:break-all;\">%1</td></tr>").arg(QString::fromStdString(req.url).toHtmlEscaped());
    html += QString("<tr><td style=\"color:#798d9f;\">请求方法:</td><td style=\"color:#79c0ff; font-weight:bold;\">%1</td></tr>").arg(method);
    html += QString("<tr><td style=\"color:#798d9f;\">状态代码:</td><td style=\"color:%1;\">%2</td></tr>")
        .arg(req.status_code >= 400 ? "#ff7b72" : "#5ce0c1", QString::number(req.status_code));
    html += QString("<tr><td style=\"color:#798d9f;\">资源类型:</td><td style=\"color:#f3c969;\">%1</td></tr>").arg(resType);
    html += QString("<tr><td style=\"color:#798d9f;\">MIME 类型:</td><td style=\"color:#8ba2b9;\">%1</td></tr>").arg(QString::fromStdString(req.mime_type).toHtmlEscaped());
    html += QString("<tr><td style=\"color:#798d9f;\">发起时间:</td><td style=\"color:#8ba2b9;\">%1</td></tr>").arg(QString::fromStdString(req.timestamp));
    html += QString("<tr><td style=\"color:#798d9f;\">请求耗时:</td><td style=\"color:#8ba2b9;\">%1</td></tr>").arg(timingStr);
    html += "</table>";

    // Response Headers section
    if (!req.response_headers.empty()) {
        html += QString("<div style=\"color:#5ce0c1; font-weight:bold; font-size:12px; margin-bottom:4px;\">响应标头 (Response Headers, %1)</div>").arg(req.response_headers.size());
        html += "<table cellpadding=\"2\" cellspacing=\"0\" style=\"margin-bottom:12px; width:100%;\">";
        for (const auto& [k, v] : req.response_headers) {
            html += QString("<tr><td style=\"color:#798d9f; width:180px; vertical-align:top;\">%1:</td><td style=\"color:#dbe5ee; word-break:break-all;\">%2</td></tr>")
                .arg(QString::fromStdString(k).toHtmlEscaped(), QString::fromStdString(v).toHtmlEscaped());
        }
        html += "</table>";
    }

    // Request Headers section
    if (!req.headers.empty()) {
        html += QString("<div style=\"color:#5ce0c1; font-weight:bold; font-size:12px; margin-bottom:4px;\">请求标头 (Request Headers, %1)</div>").arg(req.headers.size());
        html += "<table cellpadding=\"2\" cellspacing=\"0\" style=\"width:100%;\">";
        for (const auto& [k, v] : req.headers) {
            html += QString("<tr><td style=\"color:#798d9f; width:180px; vertical-align:top;\">%1:</td><td style=\"color:#dbe5ee; word-break:break-all;\">%2</td></tr>")
                .arg(QString::fromStdString(k).toHtmlEscaped(), QString::fromStdString(v).toHtmlEscaped());
        }
        html += "</table>";
    }

    html += "</div>";
    m_net_headers_view->setHtml(html);

    // 5. Payload Tab (Query + Post Data)
    QString payloadText;
    QUrl qurl(QString::fromStdString(req.url));
    if (qurl.hasQuery()) {
        QUrlQuery query(qurl);
        payloadText += "=== 查询参数 (Query Parameters) ===\n";
        auto items = query.queryItems();
        for (const auto& [k, v] : items) {
            payloadText += QString("%1: %2\n").arg(k, v);
        }
        payloadText += "\n";
    }

    if (!req.post_data.empty()) {
        payloadText += "=== 请求体 (Request Body) ===\n";
        QJsonDocument jdoc = QJsonDocument::fromJson(QByteArray::fromStdString(req.post_data));
        if (!jdoc.isNull()) {
            payloadText += QString::fromUtf8(jdoc.toJson(QJsonDocument::Indented));
        } else {
            payloadText += QString::fromStdString(req.post_data);
        }
    }

    if (payloadText.isEmpty()) {
        m_net_payload_view->setPlainText("(此请求无查询参数或请求体)");
    } else {
        m_net_payload_view->setPlainText(payloadText.trimmed());
    }

    // 6. Response Tab
    if (req.response_body_loaded) {
        updateResponseBodyView(QString::fromStdString(req.response_body), false);
    } else {
        m_net_response_view->setPlainText("正在从 ArkWeb 调试端口获取响应体 (Network.getResponseBody)…");
        std::string capId = req.request_id;
        m_webview->getResponseBody(QString::fromStdString(req.request_id), [this, capId](const QString& body, bool base64) {
            auto it = m_wv_requests.find(capId);
            if (it != m_wv_requests.end()) {
                it->second.response_body = body.toStdString();
                it->second.response_body_loaded = true;
            }
            if (m_current_selected_req_id == capId) {
                updateResponseBodyView(body, base64);
            }
        });
    }

    // 7. cURL Tab
    m_net_curl_view->setPlainText(WebViewDebugger::generateCurl(req));

    // Show panel
    if (!m_wv_net_detail_panel->isVisible()) {
        m_wv_net_detail_panel->setVisible(true);
        m_wv_net_splitter->setSizes({350, 260});
    }
}

void MainWindow::updateResponseBodyView(const QString& body, bool base64) {
    m_raw_response_body = body;
    m_response_is_json = false;

    if (body.isEmpty()) {
        m_net_response_view->setPlainText("(响应体为空或尚未加载完成)");
        if (m_net_resp_info_label) m_net_resp_info_label->setText("响应体: 0 字节");
        return;
    }

    if (base64) {
        QByteArray decoded = QByteArray::fromBase64(body.toUtf8());
        QString asText = QString::fromUtf8(decoded);
        QJsonDocument jdoc = QJsonDocument::fromJson(decoded);
        if (!jdoc.isNull()) {
            m_response_is_json = true;
            m_net_response_view->setPlainText(QString::fromUtf8(jdoc.toJson(QJsonDocument::Indented)));
            if (m_net_resp_info_label) m_net_resp_info_label->setText(QString("响应体: JSON · %1 字节 (Base64 解码)").arg(decoded.size()));
        } else {
            m_net_response_view->setPlainText(asText);
            if (m_net_resp_info_label) m_net_resp_info_label->setText(QString("响应体: %1 字节 (Base64 解码)").arg(decoded.size()));
        }
        return;
    }

    QJsonDocument jdoc = QJsonDocument::fromJson(body.toUtf8());
    if (!jdoc.isNull()) {
        m_response_is_json = true;
        m_net_response_view->setPlainText(QString::fromUtf8(jdoc.toJson(QJsonDocument::Indented)));
        if (m_net_resp_info_label) m_net_resp_info_label->setText(QString("响应体: JSON · %1 字节").arg(body.toUtf8().size()));
    } else {
        m_net_response_view->setPlainText(body);
        if (m_net_resp_info_label) m_net_resp_info_label->setText(QString("响应体: 纯文本 · %1 字节").arg(body.toUtf8().size()));
    }
}

void MainWindow::formatResponseJson() {
    if (m_raw_response_body.isEmpty()) return;
    QJsonDocument jdoc = QJsonDocument::fromJson(m_raw_response_body.toUtf8());
    if (jdoc.isNull()) {
        showToast("响应体不是有效的 JSON 格式");
        return;
    }
    QString current = m_net_response_view->toPlainText();
    QString indented = QString::fromUtf8(jdoc.toJson(QJsonDocument::Indented));
    if (current == indented) {
        m_net_response_view->setPlainText(QString::fromUtf8(jdoc.toJson(QJsonDocument::Compact)));
        showToast("已切换为紧凑 JSON");
    } else {
        m_net_response_view->setPlainText(indented);
        showToast("已格式化 JSON");
    }
}

void MainWindow::filterNetworkTable() {
    if (!m_wv_network_table) return;

    QString typeFilter = m_current_net_type_filter; // ALL, XHR, JS, CSS, IMG, DOC, OTHER
    QString query = m_wv_filter_input ? m_wv_filter_input->text().trimmed() : "";

    int visibleCount = 0;
    for (int r = 0; r < m_wv_network_table->rowCount(); ++r) {
        auto* urlItem = m_wv_network_table->item(r, 2);
        if (!urlItem) continue;

        std::string reqId = urlItem->data(Qt::UserRole).toString().toStdString();
        auto it = m_wv_requests.find(reqId);
        if (it == m_wv_requests.end()) continue;
        const auto& req = it->second;

        QString resType = normalizeResourceType(req.resource_type, req.mime_type, req.url);

        bool typeMatch = true;
        if (typeFilter == "XHR") {
            typeMatch = (resType == "Fetch/XHR");
        } else if (typeFilter == "JS") {
            typeMatch = (resType == "JS");
        } else if (typeFilter == "CSS") {
            typeMatch = (resType == "CSS");
        } else if (typeFilter == "IMG") {
            typeMatch = (resType == "Img");
        } else if (typeFilter == "DOC") {
            typeMatch = (resType == "Doc");
        } else if (typeFilter == "OTHER") {
            typeMatch = (resType != "Fetch/XHR" && resType != "JS" && resType != "CSS" && resType != "Img" && resType != "Doc");
        }

        bool queryMatch = true;
        if (!query.isEmpty()) {
            queryMatch = QString::fromStdString(req.url).contains(query, Qt::CaseInsensitive) ||
                         QString::fromStdString(req.method).contains(query, Qt::CaseInsensitive) ||
                         QString::number(req.status_code).contains(query);
        }

        bool show = typeMatch && queryMatch;
        m_wv_network_table->setRowHidden(r, !show);
        if (show) visibleCount++;
    }

    if (m_wv_net_status_label) {
        m_wv_net_status_label->setText(QString("%1 条日志 · 显示 %2 / %3 个请求")
            .arg(m_wv_console_count).arg(visibleCount).arg(m_wv_network_count));
    }
}

void MainWindow::onWebNetworkRequest(const WebNetworkRequest& req) {
    m_wv_network_count++;

    // Cache request for cURL generation and timing
    m_wv_requests[req.request_id] = req;
    m_wv_request_times[req.request_id] = QDateTime::currentMSecsSinceEpoch();

    QString resType = normalizeResourceType(req.resource_type, req.mime_type, req.url);
    m_wv_requests[req.request_id].resource_type = resType.toStdString();

    if (m_wv_network_table->rowCount() >= 1000) {
        QTableWidgetItem* oldUrlItem = m_wv_network_table->item(0, 2);
        if (oldUrlItem) {
            std::string oldId = oldUrlItem->data(Qt::UserRole).toString().toStdString();
            m_wv_requests.erase(oldId);
            m_wv_request_times.erase(oldId);
        }
        m_wv_network_table->removeRow(0);
    }

    int row = m_wv_network_table->rowCount();
    m_wv_network_table->insertRow(row);

    // Col 0: status — pending
    auto* statusItem = new QTableWidgetItem("…");
    statusItem->setForeground(QColor("#627588"));
    m_wv_network_table->setItem(row, 0, statusItem);

    // Col 1: method — colored by verb
    auto* methodItem = new QTableWidgetItem(QString::fromStdString(req.method));
    QString method = QString::fromStdString(req.method).toUpper();
    if (method == "GET")         methodItem->setForeground(QColor("#5ce0c1"));
    else if (method == "POST")   methodItem->setForeground(QColor("#79c0ff"));
    else if (method == "PUT")    methodItem->setForeground(QColor("#f3c969"));
    else if (method == "DELETE") methodItem->setForeground(QColor("#ff7b72"));
    else                         methodItem->setForeground(QColor("#c2d1df"));
    m_wv_network_table->setItem(row, 1, methodItem);

    // Col 2: URL — store request_id in UserRole
    auto* urlItem = new QTableWidgetItem(QString::fromStdString(req.url));
    urlItem->setData(Qt::UserRole, QString::fromStdString(req.request_id));
    m_wv_network_table->setItem(row, 2, urlItem);

    // Col 3: resource type badge
    auto* typeItem = new QTableWidgetItem(resType);
    if (resType == "Fetch/XHR") {
        typeItem->setForeground(QColor("#f3c969"));
    } else if (resType == "Doc") {
        typeItem->setForeground(QColor("#79c0ff"));
    } else if (resType == "JS") {
        typeItem->setForeground(QColor("#a8d878"));
    } else if (resType == "CSS") {
        typeItem->setForeground(QColor("#d2a8ff"));
    } else if (resType == "Img") {
        typeItem->setForeground(QColor("#5ce0c1"));
    } else {
        typeItem->setForeground(QColor("#8ba2b9"));
    }
    m_wv_network_table->setItem(row, 3, typeItem);

    // Col 4: timing
    m_wv_network_table->setItem(row, 4, new QTableWidgetItem("进行中…"));

    filterNetworkTable();

    if (m_wv_autoscroll && m_wv_autoscroll->isChecked()) {
        m_wv_network_table->scrollToBottom();
    }
}

void MainWindow::onWebNetworkResponse(const QString& reqId, int status, const QString& mimeType) {
    std::string rid = reqId.toStdString();

    // Calculate elapsed time
    qint64 elapsed = -1;
    auto tit = m_wv_request_times.find(rid);
    if (tit != m_wv_request_times.end()) {
        elapsed = QDateTime::currentMSecsSinceEpoch() - tit->second;
        m_wv_request_times.erase(tit);
    }

    // Sync with debugger's internal record (contains statusText, response_headers, etc.)
    const WebNetworkRequest* dbgReq = m_webview->getRequest(rid);
    if (dbgReq) {
        m_wv_requests[rid].status_code = dbgReq->status_code;
        m_wv_requests[rid].status_text = dbgReq->status_text;
        m_wv_requests[rid].mime_type = dbgReq->mime_type;
        m_wv_requests[rid].response_headers = dbgReq->response_headers;
        if (!dbgReq->resource_type.empty()) {
            m_wv_requests[rid].resource_type = dbgReq->resource_type;
        }
    } else {
        m_wv_requests[rid].status_code = status;
        m_wv_requests[rid].mime_type = mimeType.toStdString();
    }
    m_wv_requests[rid].elapsed_ms = elapsed;

    QString resType = normalizeResourceType(m_wv_requests[rid].resource_type, mimeType.toStdString(), m_wv_requests[rid].url);
    m_wv_requests[rid].resource_type = resType.toStdString();

    for (int r = 0; r < m_wv_network_table->rowCount(); ++r) {
        auto* item = m_wv_network_table->item(r, 2);
        if (item && item->data(Qt::UserRole).toString() == reqId) {
            // Col 0: status code
            auto* statusItem = new QTableWidgetItem(QString::number(status));
            if (status >= 500)      statusItem->setForeground(QColor("#d2a8ff"));
            else if (status >= 400) statusItem->setForeground(QColor("#ff7b72"));
            else if (status >= 300) statusItem->setForeground(QColor("#f3c969"));
            else                    statusItem->setForeground(QColor("#5ce0c1"));
            m_wv_network_table->setItem(r, 0, statusItem);

            // Col 3: resource type
            auto* typeItem = new QTableWidgetItem(resType);
            if (resType == "Fetch/XHR") {
                typeItem->setForeground(QColor("#f3c969"));
            } else if (resType == "Doc") {
                typeItem->setForeground(QColor("#79c0ff"));
            } else if (resType == "JS") {
                typeItem->setForeground(QColor("#a8d878"));
            } else if (resType == "CSS") {
                typeItem->setForeground(QColor("#d2a8ff"));
            } else if (resType == "Img") {
                typeItem->setForeground(QColor("#5ce0c1"));
            } else {
                typeItem->setForeground(QColor("#8ba2b9"));
            }
            m_wv_network_table->setItem(r, 3, typeItem);

            // Col 4: elapsed
            QString timing = elapsed >= 0 ? QString("%1 ms").arg(elapsed) : "完成";
            auto* timeItem = new QTableWidgetItem(timing);
            timeItem->setForeground(elapsed > 2000 ? QColor("#f3c969") : QColor("#8ba2b9"));
            m_wv_network_table->setItem(r, 4, timeItem);
            break;
        }
    }

    // If currently displaying this request in detail panel, refresh it
    if (m_current_selected_req_id == rid && m_wv_net_detail_panel && m_wv_net_detail_panel->isVisible()) {
        populateNetworkDetail(m_wv_requests[rid]);
    }

    filterNetworkTable();
}

void MainWindow::clearWebViewConsole() {
    m_wv_console_count = 0;
    m_wv_network_count = 0;
    m_cached_wv_logs.clear();
    m_wv_requests.clear();
    m_wv_request_times.clear();
    m_current_selected_req_id.clear();
    if (m_wv_console_view) m_wv_console_view->clear();
    if (m_wv_network_table) m_wv_network_table->setRowCount(0);
    if (m_wv_net_detail_panel) m_wv_net_detail_panel->setVisible(false);
    if (m_wv_net_status_label) m_wv_net_status_label->setText("0 条日志 · 0 个请求");
}

void MainWindow::executeWebViewJs() {
    QString js = m_wv_js_input->text().trimmed();
    if (js.isEmpty()) return;

    if (!m_webview->isConnected()) {
        showToast("当前未连接 WebView 界面，无法执行 JS 指令");
        return;
    }

    m_wv_console_view->appendPlainText(QString("JS > %1").arg(js));
    m_wv_js_input->clear();
    m_webview->evaluateJs(js);
    showToast("已向页面发送 JS 指令");
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

    auto* urlItem = m_wv_network_table->item(row, 2);
    if (!urlItem) return;

    QString url = urlItem->text();
    std::string rid = urlItem->data(Qt::UserRole).toString().toStdString();

    QMenu menu(this);

    menu.addAction(QIcon(), "复制 URL", this, [this, url]{
        copyToClipboard(url, "请求 URL");
    });

    // cURL — use full request data if cached
    auto reqIt = m_wv_requests.find(rid);
    if (reqIt != m_wv_requests.end()) {
        const WebNetworkRequest& req = reqIt->second;
        menu.addAction(QIcon(), "复制为 cURL 命令", this, [this, req]{
            copyToClipboard(WebViewDebugger::generateCurl(req), "cURL 命令");
        });
        if (!req.headers.empty()) {
            menu.addAction(QIcon(), "复制请求 Headers", this, [this, req]{
                QStringList lines;
                for (const auto& [k, v] : req.headers) {
                    lines << QString("%1: %2").arg(QString::fromStdString(k), QString::fromStdString(v));
                }
                copyToClipboard(lines.join('\n'), "请求 Headers");
            });
        }
        if (!req.post_data.empty()) {
            menu.addAction(QIcon(), "复制请求 Body", this, [this, req]{
                copyToClipboard(QString::fromStdString(req.post_data), "请求 Body");
            });
        }
    } else {
        auto* methodItem = m_wv_network_table->item(row, 1);
        menu.addAction(QIcon(), "复制为 cURL 命令", this, [this, url, methodItem]{
            QString method = methodItem ? methodItem->text() : "GET";
            copyToClipboard(QString("curl -X %1 \"%2\"").arg(method, url), "cURL 命令");
        });
    }

    menu.addSeparator();
    menu.addAction(QIcon(), "在浏览器中打开", this, [url]{
        QDesktopServices::openUrl(QUrl(url));
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

void MainWindow::checkPreviousCrash() {
    if (!CrashHandler::hasCrashFromPreviousRun()) return;

    QString report = CrashHandler::readLatestCrashReport();
    QMessageBox box(this);
    box.setWindowTitle("异常退出检测 · HarmonyCast");
    box.setIcon(QMessageBox::Warning);
    box.setText("检测到 HarmonyCast 上次运行发生异常退出。");
    box.setInformativeText("系统已自动收集崩溃调用栈与关键排查日志，您可以一键导出诊断包或直接查看崩溃堆栈。");

    QPushButton* btnExport = box.addButton("导出排查诊断包", QMessageBox::AcceptRole);
    QPushButton* btnView = box.addButton("查看崩溃堆栈", QMessageBox::ActionRole);
    QPushButton* btnIgnore = box.addButton("忽略", QMessageBox::RejectRole);

    box.exec();
    CrashHandler::markCrashAsReviewed();

    if (box.clickedButton() == btnExport) {
        exportDiagnosticBundle();
    } else if (box.clickedButton() == btnView) {
        showCrashReportDialog(report);
    }
}

void MainWindow::openSettingsDialog() {
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle("系统偏好设置 · HarmonyCast");
    dlg->resize(560, 480);
    dlg->setStyleSheet(
        "QDialog { background: #0c1219; color: #dbe5ee; } "
        "QGroupBox { font-weight: bold; font-size: 13px; color: #5ce0c1; border: 1px solid #1c2b3d; border-radius: 6px; margin-top: 12px; padding-top: 12px; background: #090e15; } "
        "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; } "
        "QCheckBox { color: #dbe5ee; font-size: 13px; } "
        "QLabel { color: #8ba2b9; font-size: 12px; }"
    );

    auto* l = new QVBoxLayout(dlg);
    l->setContentsMargins(18, 16, 18, 16);
    l->setSpacing(14);

    // 1. 投屏通用设置
    auto* grpGeneral = new QGroupBox("投屏偏好设置", dlg);
    auto* gLayout = new QVBoxLayout(grpGeneral);
    gLayout->setContentsMargins(14, 14, 14, 14);
    gLayout->setSpacing(8);

    auto* cb_auto = new QCheckBox("连接设备后默认自动开启投屏", grpGeneral);
    cb_auto->setChecked(m_auto_start_mirror);
    gLayout->addWidget(cb_auto);

    auto* hint = IconManager::label("开启后，插拔 USB 或通过网络直连设备时，将自动建立连接并启动投屏。", "Muted");
    hint->setWordWrap(true);
    gLayout->addWidget(hint);

    bool autoUpdate = m_settings->value("auto_check_update", true).toBool();
    auto* cb_update = new QCheckBox("启动时自动检查最新版本更新", grpGeneral);
    cb_update->setChecked(autoUpdate);
    gLayout->addWidget(cb_update);

    l->addWidget(grpGeneral);

    // 2. 日志排查与崩溃日志 (重点区域)
    auto* grpDiag = new QGroupBox("排查诊断与崩溃日志", dlg);
    auto* dLayout = new QVBoxLayout(grpDiag);
    dLayout->setContentsMargins(14, 14, 14, 14);
    dLayout->setSpacing(10);

    // 日志路径行
    auto* pathRow = new QHBoxLayout();
    QString logDir = Logger::logDirectory();
    auto* lblPath = new QLabel(QString("日志目录: %1").arg(logDir));
    lblPath->setStyleSheet("color: #7d8f9f; font-family: 'Menlo', monospace; font-size: 11px;");
    pathRow->addWidget(lblPath, 1);

    auto* btnOpenDir = IconManager::button("打开目录", "", "hub");
    btnOpenDir->setFixedHeight(26);
    btnOpenDir->setToolTip("在 Finder 文件管理器中查看当前运行日志及历史归档");
    connect(btnOpenDir, &QPushButton::clicked, this, []{ Logger::openLogDirectory(); });
    pathRow->addWidget(btnOpenDir);
    dLayout->addLayout(pathRow);

    // 崩溃日志状态行
    auto* crashRow = new QHBoxLayout();
    QString latestCrash = CrashHandler::latestCrashReportPath();
    QLabel* lblCrashStatus = nullptr;
    QPushButton* btnViewCrash = nullptr;

    if (!latestCrash.isEmpty()) {
        lblCrashStatus = new QLabel(QString("⚠️ 检测到异常退出报告: %1").arg(QFileInfo(latestCrash).fileName()));
        lblCrashStatus->setStyleSheet("color: #ff9f9f; font-weight: 500; font-size: 12px;");
        btnViewCrash = IconManager::button("查看崩溃堆栈", "Danger", "clear");
        btnViewCrash->setFixedHeight(26);
        connect(btnViewCrash, &QPushButton::clicked, this, [this]{
            showCrashReportDialog(CrashHandler::readLatestCrashReport());
        });
    } else {
        lblCrashStatus = new QLabel("✓ 运行状态良好，暂无异常退出记录");
        lblCrashStatus->setStyleSheet("color: #5ce0c1; font-size: 12px;");
    }
    crashRow->addWidget(lblCrashStatus, 1);
    if (btnViewCrash) {
        crashRow->addWidget(btnViewCrash);
    }
    dLayout->addLayout(crashRow);

    // 导出诊断包行
    auto* exportRow = new QHBoxLayout();
    auto* lblExportHint = new QLabel("可将运行日志、崩溃报告及设备环境一键打包为 zip 反馈给开发人员排查。");
    lblExportHint->setStyleSheet("color: #63778a; font-size: 11px;");
    lblExportHint->setWordWrap(true);
    exportRow->addWidget(lblExportHint, 1);

    auto* btnExportZip = IconManager::button("导出诊断包 (.zip)", "Primary", "export");
    btnExportZip->setFixedHeight(26);
    connect(btnExportZip, &QPushButton::clicked, this, [this]{
        exportDiagnosticBundle();
    });
    exportRow->addWidget(btnExportZip);
    dLayout->addLayout(exportRow);

    l->addWidget(grpDiag);

    // 3. 版本检查与更新
    auto* grpUpdate = new QGroupBox("版本信息与更新", dlg);
    auto* uLayout = new QVBoxLayout(grpUpdate);
    uLayout->setContentsMargins(14, 14, 14, 14);
    uLayout->setSpacing(8);

    auto* verRow = new QHBoxLayout();
    QString curVer = m_updater ? m_updater->currentVersion() : "2.1.2";
    auto* lblCurVer = new QLabel(QString("当前版本: v%1 (正式版)").arg(curVer));
    lblCurVer->setStyleSheet("color: #dbe5ee; font-size: 12px; font-weight: 500;");
    verRow->addWidget(lblCurVer, 1);

    auto* btnCheckUpdate = IconManager::button("检查最新版本", "Primary", "refresh");
    btnCheckUpdate->setFixedHeight(26);
    verRow->addWidget(btnCheckUpdate);
    uLayout->addLayout(verRow);

    // 更新源配置
    auto* urlRow = new QHBoxLayout();
    auto* lblUrl = new QLabel("更新服务源:");
    lblUrl->setStyleSheet("color: #8ba2b9; font-size: 11px;");
    urlRow->addWidget(lblUrl);

    auto* editUrl = new QLineEdit();
    QString defaultUrl = m_updater ? m_updater->updateUrl() : "https://api.github.com/repos/kkzzyyt/HarmonyCast/releases/latest";
    QString configuredUrl = m_settings->value("update_check_url", defaultUrl).toString();
    editUrl->setText(configuredUrl);
    editUrl->setFixedHeight(26);
    editUrl->setPlaceholderText("https://.../releases/latest 或 version.json");
    editUrl->setStyleSheet("background: #070c13; color: #dbe5ee; border: 1px solid #1c2b3d; border-radius: 4px; padding: 2px 6px; font-size: 11px;");
    urlRow->addWidget(editUrl, 1);
    uLayout->addLayout(urlRow);

    connect(btnCheckUpdate, &QPushButton::clicked, this, [this, editUrl]{
        QString enteredUrl = editUrl->text().trimmed();
        if (!enteredUrl.isEmpty() && m_updater) {
            m_updater->setUpdateUrl(enteredUrl);
        }
        checkForUpdates(false);
    });

    auto* lblUpdateTip = new QLabel("自动连接指定版本源检查新特性、安全更新与性能改进补丁。");
    lblUpdateTip->setStyleSheet("color: #63778a; font-size: 11px;");
    uLayout->addWidget(lblUpdateTip);

    l->addWidget(grpUpdate);

    // 4. 底栏确定/取消按钮
    l->addStretch();
    auto* btn_box = new QHBoxLayout();
    btn_box->addStretch();
    auto* btn_close = IconManager::button("关闭", "");
    connect(btn_close, &QPushButton::clicked, dlg, &QDialog::reject);
    btn_box->addWidget(btn_close);

    auto* btn_save = IconManager::button("保存设置", "Primary");
    connect(btn_save, &QPushButton::clicked, dlg, [this, cb_auto, cb_update, editUrl, dlg]() {
        m_auto_start_mirror = cb_auto->isChecked();
        m_settings->setValue("auto_start_mirror", m_auto_start_mirror);
        m_settings->setValue("auto_check_update", cb_update->isChecked());
        QString newUrl = editUrl->text().trimmed();
        if (!newUrl.isEmpty()) {
            m_settings->setValue("update_check_url", newUrl);
            if (m_updater) m_updater->setUpdateUrl(newUrl);
        }
        dlg->accept();
        showToast("偏好设置已保存");
    });
    btn_box->addWidget(btn_save);

    l->addLayout(btn_box);
    dlg->exec();
    dlg->deleteLater();
}

void MainWindow::checkForUpdates(bool silent) {
    if (!silent) {
        showToast("正在连接版本服务检查更新…");
        statusBar()->showMessage("正在检查最新版本…", 3000);
    }
    if (m_updater) {
        QString configuredUrl = m_settings->value("update_check_url").toString();
        if (!configuredUrl.isEmpty()) {
            m_updater->setUpdateUrl(configuredUrl);
        }
        m_updater->checkForUpdates(silent);
    }
}

void MainWindow::onUpdateAvailable(const UpdateInfo& info, bool silent) {
    (void)silent;
    QWidget* parentDlg = QApplication::activeModalWidget() ? QApplication::activeModalWidget() : this;
    auto* dlg = new UpdateDialog(info, parentDlg);
    dlg->exec();
    if (dlg->wasIgnored() && m_updater) {
        m_updater->ignoreVersion(info.latestVersion);
        showToast(QString("已忽略 v%1 版本更新提示").arg(info.latestVersion));
    }
    dlg->deleteLater();
}

void MainWindow::onNoUpdateAvailable(bool silent) {
    if (!silent) {
        QString curVer = m_updater ? m_updater->currentVersion() : "2.1.2";
        showToast(QString("当前已是最新版本 (v%1)").arg(curVer));
        statusBar()->showMessage(QString("当前已是最新版本 (v%1)").arg(curVer), 4000);
        QWidget* parentDlg = QApplication::activeModalWidget() ? QApplication::activeModalWidget() : this;
        QMessageBox::information(parentDlg,
                                 "版本检查 · HarmonyCast",
                                 QString("您当前使用的 HarmonyCast v%1 已是最新版本，无需更新。").arg(curVer));
    }
}

void MainWindow::onUpdateCheckFailed(const QString& error, bool silent) {
    if (!silent) {
        showToast("检查更新失败: " + error);
        statusBar()->showMessage("检查更新失败: " + error, 4000);
        QWidget* parentDlg = QApplication::activeModalWidget() ? QApplication::activeModalWidget() : this;
        QMessageBox::warning(parentDlg,
                             "版本检查失败 · HarmonyCast",
                             QString("未能获取到最新版本信息，原因如下：\n\n%1\n\n请检查网络连接或自定义更新服务源配置。").arg(error));
    }
}

void MainWindow::exportDiagnosticBundle() {
    QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString defaultPath = QDir::homePath() + QString("/Downloads/HarmonyCast_Diagnostics_%1.zip").arg(ts);
    QString path = QFileDialog::getSaveFileName(this, "导出排查诊断包", defaultPath, "Zip Archive (*.zip);;All Files (*)");
    if (path.isEmpty()) return;

    QString resultPath = Logger::exportDiagnosticBundle(path);
    if (!resultPath.isEmpty()) {
        showToast("排查诊断包已导出: " + QFileInfo(resultPath).fileName());
        statusBar()->showMessage("诊断包已成功导出至: " + resultPath, 6000);
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(resultPath).absolutePath()));
    } else {
        showToast("导出诊断包失败");
    }
}

void MainWindow::showCrashReportDialog(const QString& report) {
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle("崩溃堆栈排查报告 · HarmonyCast");
    dlg->resize(720, 520);
    dlg->setStyleSheet(
        "QDialog { background: #0c1219; color: #dbe5ee; } "
        "QTextEdit { background: #070c13; color: #ff9f9f; border: 1px solid #233446; border-radius: 4px; font-family: 'Menlo', monospace; font-size: 12px; padding: 8px; }"
    );

    auto* layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto* hdr = new QLabel("崩溃堆栈快照 (请反馈给研发进行定位排查):");
    hdr->setStyleSheet("font-size: 13px; font-weight: bold; color: #dbe5ee;");
    layout->addWidget(hdr);

    auto* view = new QTextEdit();
    view->setReadOnly(true);
    view->setPlainText(report.isEmpty() ? "未检测到崩溃日志内容" : report);
    layout->addWidget(view, 1);

    auto* btnBox = new QHBoxLayout();
    btnBox->addStretch(1);

    auto* btnCopy = IconManager::button("复制内容", "", "copy");
    connect(btnCopy, &QPushButton::clicked, this, [this, report]{
        copyToClipboard(report, "崩溃日志内容");
    });
    btnBox->addWidget(btnCopy);

    auto* btnOpenFolder = IconManager::button("打开日志目录", "", "hub");
    connect(btnOpenFolder, &QPushButton::clicked, this, []{
        Logger::openLogDirectory();
    });
    btnBox->addWidget(btnOpenFolder);

    auto* btnClose = IconManager::button("关闭", "Primary", "");
    connect(btnClose, &QPushButton::clicked, dlg, &QDialog::accept);
    btnBox->addWidget(btnClose);

    layout->addLayout(btnBox);
    dlg->exec();
    dlg->deleteLater();
}

} // namespace harmonycast
