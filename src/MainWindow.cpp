#include "harmonycast/MainWindow.h"
#include "harmonycast/FlowLayout.h"

#include <QApplication>
#include <QClipboard>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QScrollBar>
#include <QDateTime>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QHeaderView>
#include <QRegularExpression>
#include <iostream>

namespace harmonycast {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      m_hdc(std::make_shared<HdcClient>()),
      m_dev_mgr(std::make_shared<DeviceManager>(m_hdc, this)),
      m_webview(std::make_unique<WebViewDebugger>(m_hdc, this)) {

    setupUi();
    applyTheme();

    connect(m_dev_mgr.get(), &DeviceManager::devicesUpdated, this, &MainWindow::onDevicesUpdated);
    connect(m_webview.get(), &WebViewDebugger::pagesLoaded, this, [this](const std::vector<WebViewPage>& pages) {
        m_cached_pages = pages;
        m_wv_page_combo->clear();
        if (pages.empty()) {
            m_wv_page_combo->addItem("当前无活跃 WebView 页面");
            showToast("当前应用无活跃的 WebView 调试页面");
        } else {
            for (const auto& p : pages) {
                QString title = QString::fromStdString(p.title.empty() ? p.url : p.title);
                m_wv_page_combo->addItem(title, QString::fromStdString(p.id));
            }
            showToast(QString("成功发现 %1 个 WebView 调试目标").arg(pages.size()));
        }
    });

    connect(m_webview.get(), &WebViewDebugger::consoleMessageReceived, this, &MainWindow::onWebConsoleMessage);
    connect(m_webview.get(), &WebViewDebugger::networkRequestReceived, this, &MainWindow::onWebNetworkRequest);
    connect(m_webview.get(), &WebViewDebugger::networkResponseReceived, this, &MainWindow::onWebNetworkResponse);

    m_dev_mgr->startPolling(1500);
}

MainWindow::~MainWindow() {
    onStopMirror();
    if (m_hilog) m_hilog->stop();
    if (m_webview) m_webview->disconnectPage();
}

void MainWindow::setupUi() {
    setWindowTitle("HarmonyCast · 鸿蒙投屏工作台");
    setMinimumSize(1020, 720);
    resize(1200, 820);

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
    body_layout->addWidget(m_nav_info->parentWidget());

    m_pages = new QStackedWidget(body_widget);
    setupInfoPage();
    setupWorkspace();
    setupHdcPage();

    m_pages->addWidget(m_info_page);       // Index 0: 设备信息
    m_pages->addWidget(m_workspace_page);  // Index 1: 投屏工作台 (同屏含投屏 + HiLog + WebView)
    m_pages->addWidget(m_hdc_page);        // Index 2: 常用命令

    // Default to Workspace
    m_pages->setCurrentWidget(m_workspace_page);
    m_nav_workspace->setChecked(true);

    body_layout->addWidget(m_pages, 1);
    root_layout->addWidget(body_widget, 1);
}

void MainWindow::setupHeader() {
    auto* header = new QWidget(this);
    header->setFixedHeight(50);
    header->setObjectName("headerWidget");

    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(16, 8, 16, 8);
    layout->setSpacing(10);

    auto* title = new QLabel("HarmonyCast", header);
    title->setObjectName("appTitle");
    layout->addWidget(title);

    layout->addSpacing(8);

    // Layout Switcher: 左右 / 上下 / 仅投屏 / 仅日志
    auto* switcher_card = new QWidget(header);
    switcher_card->setObjectName("layoutSwitcherCard");
    auto* s_layout = new QHBoxLayout(switcher_card);
    s_layout->setContentsMargins(2, 2, 2, 2);
    s_layout->setSpacing(2);

    auto makeSwitchBtn = [this, switcher_card](const QString& text, const QString& mode) {
        auto* btn = new QPushButton(text, switcher_card);
        btn->setCheckable(true);
        btn->setAutoExclusive(true);
        btn->setFixedHeight(24);
        btn->setObjectName("layoutBtn");
        connect(btn, &QPushButton::clicked, this, [this, mode]() { setWorkspaceLayout(mode); });
        return btn;
    };

    m_btn_layout_h = makeSwitchBtn("左右", "horizontal");
    m_btn_layout_v = makeSwitchBtn("上下", "vertical");
    m_btn_layout_screen = makeSwitchBtn("投屏", "screen_only");
    m_btn_layout_logs = makeSwitchBtn("日志", "logs_only");

    m_btn_layout_h->setChecked(true);
    s_layout->addWidget(m_btn_layout_h);
    s_layout->addWidget(m_btn_layout_v);
    s_layout->addWidget(m_btn_layout_screen);
    s_layout->addWidget(m_btn_layout_logs);

    layout->addWidget(switcher_card);
    layout->addSpacing(8);

    m_device_combo = new QComboBox(header);
    m_device_combo->setMinimumWidth(200);
    m_device_combo->setMaximumWidth(280);
    m_device_combo->addItem("正在探测设备…");
    connect(m_device_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onDeviceSelected);
    layout->addWidget(m_device_combo);

    m_state_badge = new QLabel("未连接设备", header);
    m_state_badge->setObjectName("stateBadge");
    layout->addWidget(m_state_badge);

    m_btn_refresh = new QPushButton("刷新", header);
    m_btn_refresh->setFixedWidth(60);
    connect(m_btn_refresh, &QPushButton::clicked, this, &MainWindow::onRefreshDevices);
    layout->addWidget(m_btn_refresh);

    m_btn_disconnect = new QPushButton("断开", header);
    m_btn_disconnect->setFixedWidth(60);
    m_btn_disconnect->setObjectName("dangerButton");
    connect(m_btn_disconnect, &QPushButton::clicked, this, &MainWindow::onDisconnectDevice);
    layout->addWidget(m_btn_disconnect);

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
    sidebar->setFixedWidth(130);
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

    m_nav_workspace = makeNavBtn("投屏工作台");
    m_nav_info = makeNavBtn("设备信息");
    m_nav_hdc = makeNavBtn("常用命令");

    layout->addWidget(m_nav_workspace);
    layout->addWidget(m_nav_info);
    layout->addWidget(m_nav_hdc);
    layout->addStretch();

    auto* ver_label = new QLabel("HarmonyCast\nv1.5.0", sidebar);
    ver_label->setObjectName("mutedLabel");
    ver_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(ver_label);

    connect(m_nav_workspace, &QPushButton::clicked, this, [this]() { m_pages->setCurrentWidget(m_workspace_page); });
    connect(m_nav_info, &QPushButton::clicked, this, [this]() { m_pages->setCurrentWidget(m_info_page); onRefreshDeviceInfo(); });
    connect(m_nav_hdc, &QPushButton::clicked, this, [this]() { m_pages->setCurrentWidget(m_hdc_page); });
}

void MainWindow::setupWorkspace() {
    m_workspace_page = new QWidget(this);
    auto* ws_layout = new QVBoxLayout(m_workspace_page);
    ws_layout->setContentsMargins(0, 0, 0, 0);
    ws_layout->setSpacing(0);

    // QSplitter containing Left (Mirroring) and Right (HiLog + WebView Tabs)
    m_workspace_splitter = new QSplitter(Qt::Horizontal, m_workspace_page);
    m_workspace_splitter->setObjectName("workspaceSplitter");
    m_workspace_splitter->setHandleWidth(8);
    m_workspace_splitter->setChildrenCollapsible(false);

    setupMirrorPanel();
    setupRightTabPanel();

    m_workspace_splitter->addWidget(m_mirror_container);
    m_workspace_splitter->addWidget(m_log_container);
    m_workspace_splitter->setStretchFactor(0, 5);
    m_workspace_splitter->setStretchFactor(1, 5);

    ws_layout->addWidget(m_workspace_splitter);
}

void MainWindow::setupMirrorPanel() {
    m_mirror_container = new QWidget(m_workspace_splitter);
    auto* layout = new QVBoxLayout(m_mirror_container);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    // Top action bar with FlowLayout
    auto* tb = new QWidget(m_mirror_container);
    auto* flow = new FlowLayout(tb, 0, 6, 6);

    m_btn_toggle_mirror = new QPushButton("开始投屏", tb);
    m_btn_toggle_mirror->setObjectName("primaryButton");
    connect(m_btn_toggle_mirror, &QPushButton::clicked, this, &MainWindow::onToggleMirror);
    flow->addWidget(m_btn_toggle_mirror);

    auto* btn_back = new QPushButton("返回", tb);
    connect(btn_back, &QPushButton::clicked, this, &MainWindow::onActionBack);
    flow->addWidget(btn_back);

    auto* btn_home = new QPushButton("主页", tb);
    connect(btn_home, &QPushButton::clicked, this, &MainWindow::onActionHome);
    flow->addWidget(btn_home);

    auto* btn_wake = new QPushButton("亮屏", tb);
    connect(btn_wake, &QPushButton::clicked, this, &MainWindow::onActionWakeup);
    flow->addWidget(btn_wake);

    auto* btn_power = new QPushButton("电源", tb);
    connect(btn_power, &QPushButton::clicked, this, &MainWindow::onActionPower);
    flow->addWidget(btn_power);

    auto* btn_rotate = new QPushButton("旋转", tb);
    connect(btn_rotate, &QPushButton::clicked, this, &MainWindow::onActionRotate);
    flow->addWidget(btn_rotate);

    auto* btn_shot = new QPushButton("截图", tb);
    connect(btn_shot, &QPushButton::clicked, this, &MainWindow::onActionScreenshot);
    flow->addWidget(btn_shot);

    m_fps_label = new QLabel("就绪", tb);
    m_fps_label->setObjectName("fpsLabel");
    flow->addWidget(m_fps_label);

    layout->addWidget(tb);

    m_screen_view = new ScreenView(m_mirror_container);
    m_screen_view->setTouchCallback([this](TouchAction action, int x, int y, int dev_w, int dev_h) {
        if (m_stream && m_stream->controlChannel()) {
            m_stream->controlChannel()->sendTouch(action, x, y, dev_w, dev_h);
        }
    });

    layout->addWidget(m_screen_view, 1);
}

void MainWindow::setupRightTabPanel() {
    m_log_container = new QWidget(m_workspace_splitter);
    auto* layout = new QVBoxLayout(m_log_container);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_log_tabs = new QTabWidget(m_log_container);
    m_log_tabs->setObjectName("workspaceTabs");

    m_log_tabs->addTab(buildHiLogWidget(), "设备日志 (HiLog)");
    m_log_tabs->addTab(buildWebViewWidget(), "WebView 控制台");

    layout->addWidget(m_log_tabs);
}

QWidget* MainWindow::buildHiLogWidget() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(4, 8, 4, 4);
    layout->setSpacing(6);

    auto* tb = new QWidget(page);
    auto* flow = new FlowLayout(tb, 0, 6, 6);

    m_search_input = new QLineEdit(tb);
    m_search_input->setPlaceholderText("搜索日志内容、Tag 或 PID…");
    m_search_input->setMinimumWidth(180);
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

    m_log_view = new QPlainTextEdit(page);
    m_log_view->setReadOnly(true);
    m_log_view->setMaximumBlockCount(10000);
    m_log_view->setFont(QFont("Menlo", 12));
    layout->addWidget(m_log_view, 1);

    m_log_count_label = new QLabel("缓存 0 条 · 设备日志自动流式采集", page);
    m_log_count_label->setObjectName("mutedLabel");
    layout->addWidget(m_log_count_label);

    return page;
}

QWidget* MainWindow::buildWebViewWidget() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(4, 8, 4, 4);
    layout->setSpacing(6);

    // Toolbar
    auto* tb = new QWidget(page);
    auto* flow = new FlowLayout(tb, 0, 6, 6);

    auto* btn_scan = new QPushButton("扫描 WebView", tb);
    connect(btn_scan, &QPushButton::clicked, this, &MainWindow::onScanWebViewInstances);
    flow->addWidget(btn_scan);

    m_wv_instance_combo = new QComboBox(tb);
    m_wv_instance_combo->addItem("请选择 WebView 实例…");
    m_wv_instance_combo->setMinimumWidth(180);
    connect(m_wv_instance_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onWebViewInstanceSelected);
    flow->addWidget(m_wv_instance_combo);

    m_wv_page_combo = new QComboBox(tb);
    m_wv_page_combo->addItem("等待获取页面列表…");
    m_wv_page_combo->setMinimumWidth(200);
    connect(m_wv_page_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onWebViewPageSelected);
    flow->addWidget(m_wv_page_combo);

    m_btn_open_browser = new QPushButton("浏览器调试 ↗", tb);
    m_btn_open_browser->setObjectName("primaryButton");
    connect(m_btn_open_browser, &QPushButton::clicked, this, &MainWindow::onOpenInBrowser);
    flow->addWidget(m_btn_open_browser);

    m_btn_export_curl = new QPushButton("导出 cURL", tb);
    connect(m_btn_export_curl, &QPushButton::clicked, this, &MainWindow::onExportCurl);
    flow->addWidget(m_btn_export_curl);

    layout->addWidget(tb);

    // Splitter for Console (Top) and Network Requests (Bottom)
    auto* wv_splitter = new QSplitter(Qt::Vertical, page);

    auto* console_box = new QWidget(wv_splitter);
    auto* c_layout = new QVBoxLayout(console_box);
    c_layout->setContentsMargins(0, 0, 0, 0);
    c_layout->addWidget(new QLabel("Console 日志", console_box));
    m_wv_console_view = new QPlainTextEdit(console_box);
    m_wv_console_view->setReadOnly(true);
    m_wv_console_view->setFont(QFont("Menlo", 12));
    c_layout->addWidget(m_wv_console_view);

    auto* net_box = new QWidget(wv_splitter);
    auto* n_layout = new QVBoxLayout(net_box);
    n_layout->setContentsMargins(0, 0, 0, 0);
    n_layout->addWidget(new QLabel("Network 接口请求 (点击可导出 cURL)", net_box));
    m_wv_network_table = new QTableWidget(net_box);
    m_wv_network_table->setColumnCount(4);
    m_wv_network_table->setHorizontalHeaderLabels({"Method", "Status", "URL", "Time"});
    m_wv_network_table->horizontalHeader()->setStretchLastSection(true);
    m_wv_network_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_wv_network_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    n_layout->addWidget(m_wv_network_table);

    wv_splitter->addWidget(console_box);
    wv_splitter->addWidget(net_box);
    wv_splitter->setStretchFactor(0, 5);
    wv_splitter->setStretchFactor(1, 5);

    layout->addWidget(wv_splitter, 1);
    return page;
}

void MainWindow::setupInfoPage() {
    m_info_page = new QWidget(this);
    auto* layout = new QVBoxLayout(m_info_page);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(14);

    auto makeCard = [](const QString& label_text) {
        auto* card = new QWidget();
        card->setObjectName("infoCard");
        auto* l = new QVBoxLayout(card);
        l->setContentsMargins(14, 10, 14, 10);
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

    auto [c4, v4] = makeCard("开机运行时长 (超过 24 小时自动转换天数)");
    m_info_uptime = v4;
    layout->addWidget(c4);

    auto [c5, v5] = makeCard("屏幕规格与旋转");
    m_info_display = v5;
    layout->addWidget(c5);

    // Wireless Connect Box
    auto* wifi_box = new QWidget(m_info_page);
    wifi_box->setObjectName("infoCard");
    auto* w_layout = new QHBoxLayout(wifi_box);
    w_layout->setContentsMargins(14, 10, 14, 10);
    w_layout->addWidget(new QLabel("无线网络连接:", wifi_box));
    m_wireless_ip = new QLineEdit(wifi_box);
    m_wireless_ip->setPlaceholderText("设备 IP 地址 (如: 192.168.1.100)");
    w_layout->addWidget(m_wireless_ip, 1);
    m_wireless_port = new QLineEdit("8710", wifi_box);
    m_wireless_port->setFixedWidth(60);
    w_layout->addWidget(m_wireless_port);
    auto* btn_tconn = new QPushButton("无线连接", wifi_box);
    btn_tconn->setObjectName("primaryButton");
    connect(btn_tconn, &QPushButton::clicked, this, &MainWindow::onWirelessConnect);
    w_layout->addWidget(btn_tconn);
    layout->addWidget(wifi_box);

    // Application List
    auto* app_box = new QWidget(m_info_page);
    app_box->setObjectName("infoCard");
    auto* a_layout = new QVBoxLayout(app_box);
    a_layout->setContentsMargins(14, 10, 14, 10);
    auto* a_header = new QHBoxLayout();
    a_header->addWidget(new QLabel("已安装应用列表 (bm dump -a)", app_box));
    auto* btn_refresh_app = new QPushButton("刷新应用列表", app_box);
    connect(btn_refresh_app, &QPushButton::clicked, this, &MainWindow::onRefreshAppList);
    a_header->addWidget(btn_refresh_app);
    auto* btn_launch_app = new QPushButton("启动选中应用", app_box);
    connect(btn_launch_app, &QPushButton::clicked, this, &MainWindow::onLaunchSelectedApp);
    a_header->addWidget(btn_launch_app);
    a_layout->addLayout(a_header);

    m_app_list = new QListWidget(app_box);
    m_app_list->setMaximumHeight(160);
    a_layout->addWidget(m_app_list);
    layout->addWidget(app_box);

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
            background-color: #0c1219;
            color: #dbe8f5;
        }
        QWidget#headerWidget {
            border-bottom: 1px solid #1c2b3d;
        }
        QWidget#sidebarWidget {
            border-right: 1px solid #1c2b3d;
        }
        QLabel#appTitle {
            font-size: 16px;
            font-weight: bold;
            color: #53d6b5;
        }
        QWidget#layoutSwitcherCard {
            background: #091017;
            border: 1px solid #1c2b3d;
            border-radius: 6px;
            padding: 2px;
        }
        QPushButton#layoutBtn {
            background: transparent;
            border: none;
            border-radius: 4px;
            padding: 2px 8px;
            color: #7d8f9f;
            font-size: 12px;
        }
        QPushButton#layoutBtn:hover {
            background: #162436;
            color: #c9d1d9;
        }
        QPushButton#layoutBtn:checked {
            background: #1f3b60;
            color: #53d6b5;
            font-weight: bold;
        }
        QLabel#stateBadge {
            background-color: #102a24;
            color: #53d6b5;
            padding: 4px 10px;
            border-radius: 4px;
            font-size: 12px;
            font-weight: 500;
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
        QPushButton#dangerButton {
            background-color: #3d1c1c;
            color: #ff7b7b;
            border-color: #542d2d;
        }
        QPushButton#dangerButton:hover {
            background-color: #4d2424;
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
        QPlainTextEdit, QTableWidget, QListWidget {
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
        QTabWidget#workspaceTabs::pane {
            border: 1px solid #1e2c3d;
            background: #09111a;
            border-radius: 6px;
        }
        QTabBar::tab {
            background: #141f2c;
            color: #8b9bb4;
            padding: 8px 16px;
            border-top-left-radius: 6px;
            border-top-right-radius: 6px;
            margin-right: 4px;
        }
        QTabBar::tab:selected {
            background: #1e2c3d;
            color: #53d6b5;
            font-weight: bold;
        }
    )");
}

void MainWindow::setWorkspaceLayout(const QString& mode) {
    if (mode == "horizontal") {
        m_workspace_splitter->setOrientation(Qt::Horizontal);
        m_mirror_container->show();
        m_log_container->show();
    } else if (mode == "vertical") {
        m_workspace_splitter->setOrientation(Qt::Vertical);
        m_mirror_container->show();
        m_log_container->show();
    } else if (mode == "screen_only") {
        m_mirror_container->show();
        m_log_container->hide();
    } else if (mode == "logs_only") {
        m_mirror_container->hide();
        m_log_container->show();
    }
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
        // Auto start HiLog session for the selected device
        if (m_hilog) m_hilog->stop();
        m_hilog = std::make_unique<HiLogSession>(m_hdc->path(), dev->serial, "", "all", this);
        connect(m_hilog.get(), &HiLogSession::entriesReady, this, &MainWindow::onHiLogEntriesReady);
        m_hilog->start();
    }
}

void MainWindow::onRefreshDevices() {
    m_dev_mgr->refresh();
}

void MainWindow::onDisconnectDevice() {
    auto dev = currentDevice();
    if (!dev) return;
    onStopMirror();
    if (dev->is_network) {
        m_hdc->run({"tconn", dev->serial, "-remove"});
    }
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
        showToast("截图已成功保存至:\n" + path);
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
    m_log_count_label->setText(QString("缓存 %1 条 · 设备日志自动流式采集").arg(m_cached_entries.size()));
}

void MainWindow::onHiLogClear() {
    m_cached_entries.clear();
    m_log_view->clear();
    m_log_count_label->setText("缓存 0 条 · 设备日志自动流式采集");
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
            showToast("日志已导出至: " + path);
        }
    }
}

// WebView Debugger Actions
void MainWindow::onScanWebViewInstances() {
    auto dev = currentDevice();
    if (!dev) {
        showToast("请先选择目标设备");
        return;
    }

    m_cached_instances = m_webview->scanInstances(dev->serial);
    m_wv_instance_combo->clear();
    if (m_cached_instances.empty()) {
        m_wv_instance_combo->addItem("未检测到运行中的 WebView 实例");
        showToast("当前设备没有开启 Webview 调试的应用");
    } else {
        for (const auto& inst : m_cached_instances) {
            m_wv_instance_combo->addItem(QString("PID %1 (%2)").arg(inst.pid).arg(QString::fromStdString(inst.socket_name)));
        }
        showToast(QString("检测到 %1 个 WebView 调试套接字").arg(m_cached_instances.size()));
        onWebViewInstanceSelected(0);
    }
}

void MainWindow::onWebViewInstanceSelected(int index) {
    auto dev = currentDevice();
    if (!dev || index < 0 || index >= static_cast<int>(m_cached_instances.size())) return;

    const auto& inst = m_cached_instances[index];
    if (m_webview->forwardInstance(dev->serial, inst, 9222)) {
        onFetchWebViewPages();
    }
}

void MainWindow::onFetchWebViewPages() {
    m_webview->fetchPages(9222);
}

void MainWindow::onWebViewPageSelected(int index) {
    if (index < 0 || index >= static_cast<int>(m_cached_pages.size())) return;
    const auto& page = m_cached_pages[index];
    m_webview->connectPage(page);
    showToast("已连接页面 CDP 调试: " + QString::fromStdString(page.title));
}

void MainWindow::onOpenInBrowser() {
    int idx = m_wv_page_combo->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(m_cached_pages.size())) {
        showToast("当前无可用 WebView 页面");
        return;
    }
    const auto& page = m_cached_pages[idx];
    if (!page.devtools_frontend_url.empty()) {
        QDesktopServices::openUrl(QUrl(QString::fromStdString(page.devtools_frontend_url)));
    } else {
        // Fallback to inspect edge/chrome frontend
        QString fallback = QString("devtools://devtools/bundled/inspector.html?ws=127.0.0.1:9222/devtools/page/%1")
                               .arg(QString::fromStdString(page.id));
        QDesktopServices::openUrl(QUrl(fallback));
    }
}

void MainWindow::onExportCurl() {
    int row = m_wv_network_table->currentRow();
    if (row < 0) {
        showToast("请在表格中选择一条请求以导出 cURL");
        return;
    }
    QString req_id = m_wv_network_table->item(row, 0)->data(Qt::UserRole).toString();
    auto it = m_web_requests.find(req_id.toStdString());
    if (it != m_web_requests.end()) {
        QString curl = WebViewDebugger::generateCurl(it->second);
        QApplication::clipboard()->setText(curl);
        showToast("cURL 命令已复制到系统剪贴板！");
    }
}

void MainWindow::onWebConsoleMessage(const WebConsoleMessage& msg) {
    m_wv_console_view->appendPlainText(QString("[%1] %2: %3")
                                           .arg(QString::fromStdString(msg.timestamp))
                                           .arg(QString::fromStdString(msg.level).toUpper())
                                           .arg(QString::fromStdString(msg.text)));
}

void MainWindow::onWebNetworkRequest(const WebNetworkRequest& req) {
    m_web_requests[req.request_id] = req;

    int row = m_wv_network_table->rowCount();
    m_wv_network_table->insertRow(row);

    auto* m_item = new QTableWidgetItem(QString::fromStdString(req.method));
    m_item->setData(Qt::UserRole, QString::fromStdString(req.request_id));
    m_wv_network_table->setItem(row, 0, m_item);
    m_wv_network_table->setItem(row, 1, new QTableWidgetItem("Pending"));
    m_wv_network_table->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(req.url)));
    m_wv_network_table->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(req.timestamp)));
}

void MainWindow::onWebNetworkResponse(const QString& req_id, int status, const QString& mime) {
    for (int r = 0; r < m_wv_network_table->rowCount(); ++r) {
        if (m_wv_network_table->item(r, 0)->data(Qt::UserRole).toString() == req_id) {
            m_wv_network_table->item(r, 1)->setText(QString::number(status));
            m_wv_network_table->item(r, 2)->setToolTip(mime);
            break;
        }
    }
}

// Device Center Actions
void MainWindow::onRefreshDeviceInfo() {
    auto dev = currentDevice();
    if (!dev) return;

    auto info = m_hdc->getDeviceInfo(dev->serial);
    if (!info) return;

    m_info_model->setText(QString::fromStdString(info->model.empty() ? "OpenHarmony Device" : info->model));
    m_info_serial->setText(QString::fromStdString(info->serial));
    m_info_version->setText(QString::fromStdString(info->os_version.empty() ? "OpenHarmony 5.0" : info->os_version));

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

    onRefreshAppList();
}

void MainWindow::onWirelessConnect() {
    QString ip = m_wireless_ip->text().trimmed();
    QString port = m_wireless_port->text().trimmed();
    if (ip.isEmpty()) {
        showToast("请输入设备 IP 地址");
        return;
    }
    QString target = ip + ":" + port;
    auto res = m_hdc->run({"tconn", target.toStdString()});
    if (res.out.find("Connect OK") != std::string::npos || res.success()) {
        showToast("无线连接成功: " + target);
        m_dev_mgr->refresh();
    } else {
        showToast("连接失败: " + QString::fromStdString(res.err.empty() ? res.out : res.err));
    }
}

void MainWindow::onRefreshAppList() {
    auto dev = currentDevice();
    if (!dev) return;

    auto res = m_hdc->run({"shell", "bm dump -a 2>/dev/null"}, dev->serial);
    m_app_list->clear();
    if (res.success()) {
        QStringList lines = QString::fromStdString(res.out).split("\n", Qt::SkipEmptyParts);
        for (const auto& line : lines) {
            QString trimmed = line.trimmed();
            if (!trimmed.isEmpty() && !trimmed.contains("bm dump") && !trimmed.contains("error")) {
                m_app_list->addItem(trimmed);
            }
        }
    }
}

void MainWindow::onLaunchSelectedApp() {
    auto dev = currentDevice();
    auto* item = m_app_list->currentItem();
    if (!dev || !item) {
        showToast("请在列表中选择一个应用");
        return;
    }
    QString bundle = item->text();
    m_hdc->run({"shell", QString("aa start -b %1 -m default").arg(bundle).toStdString()}, dev->serial);
    showToast("已发送启动请求: " + bundle);
}

void MainWindow::onUninstallSelectedApp() {
    auto dev = currentDevice();
    auto* item = m_app_list->currentItem();
    if (!dev || !item) return;
    QString bundle = item->text();
    m_hdc->run({"shell", QString("bm uninstall -n %1").arg(bundle).toStdString()}, dev->serial);
    showToast("已卸载应用: " + bundle);
    onRefreshAppList();
}

void MainWindow::onHdcPresetSelected(int index) {
    if (index > 0) {
        m_hdc_input->setText(m_hdc_preset_combo->currentText());
    }
}

void MainWindow::onHdcRunCommand() {
    auto dev = currentDevice();
    if (!dev) {
        showToast("请选择目标设备");
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

void MainWindow::showToast(const QString& message) {
    statusBar()->showMessage(message, 3500);
}

} // namespace harmonycast
