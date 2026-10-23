#pragma once

#include "Types.h"
#include "HdcClient.h"
#include "DeviceManager.h"
#include "StreamSession.h"
#include "ScreenView.h"
#include "HiLogSession.h"
#include "WebViewDebugger.h"

#include <QMainWindow>
#include <QStackedWidget>
#include <QSplitter>
#include <QTabWidget>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QCheckBox>
#include <QTableWidget>
#include <QListWidget>
#include <memory>

namespace harmonycast {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    // Device Management
    void onDevicesUpdated(const std::vector<Device>& devices);
    void onDeviceSelected(int index);
    void onRefreshDevices();
    void onDisconnectDevice();

    // Layout Switcher
    void setWorkspaceLayout(const QString& mode);

    // Mirroring Actions
    void onToggleMirror();
    void onStopMirror();
    void onFrameReady(const DecodedFrame& frame);
    void onStreamStatusChanged(const QString& status);
    void onStreamFinished(const QString& error);
    void onActionBack();
    void onActionHome();
    void onActionWakeup();
    void onActionPower();
    void onActionRotate();
    void onActionScreenshot();

    // HiLog Actions
    void onHiLogEntriesReady(const std::vector<HiLogEntry>& entries);
    void onHiLogClear();
    void onHiLogFilterChanged();
    void onHiLogExport();

    // WebView Actions
    void onScanWebViewInstances();
    void onWebViewInstanceSelected(int index);
    void onFetchWebViewPages();
    void onWebViewPageSelected(int index);
    void onOpenInBrowser();
    void onExportCurl();
    void onWebConsoleMessage(const WebConsoleMessage& msg);
    void onWebNetworkRequest(const WebNetworkRequest& req);
    void onWebNetworkResponse(const QString& req_id, int status, const QString& mime);

    // Device Center Actions
    void onRefreshDeviceInfo();
    void onWirelessConnect();
    void onRefreshAppList();
    void onLaunchSelectedApp();
    void onUninstallSelectedApp();

    // HDC Terminal Actions
    void onHdcRunCommand();
    void onHdcPresetSelected(int index);

private:
    std::shared_ptr<HdcClient> m_hdc;
    std::shared_ptr<DeviceManager> m_dev_mgr;
    std::unique_ptr<StreamSession> m_stream;
    std::unique_ptr<HiLogSession> m_hilog;
    std::unique_ptr<WebViewDebugger> m_webview;

    // Header Widgets
    QComboBox* m_device_combo{nullptr};
    QLabel* m_state_badge{nullptr};
    QPushButton* m_btn_refresh{nullptr};
    QPushButton* m_btn_disconnect{nullptr};
    QComboBox* m_mode_combo{nullptr};
    QComboBox* m_quality_combo{nullptr};

    // Header Layout Switcher buttons
    QPushButton* m_btn_layout_h{nullptr};
    QPushButton* m_btn_layout_v{nullptr};
    QPushButton* m_btn_layout_screen{nullptr};
    QPushButton* m_btn_layout_logs{nullptr};

    // Sidebar
    QPushButton* m_nav_info{nullptr};
    QPushButton* m_nav_workspace{nullptr};
    QPushButton* m_nav_hdc{nullptr};
    QStackedWidget* m_pages{nullptr};

    // Workspace (投屏 + 日志 + WebView 同屏工作台)
    QWidget* m_workspace_page{nullptr};
    QSplitter* m_workspace_splitter{nullptr};
    QWidget* m_mirror_container{nullptr};
    QWidget* m_log_container{nullptr};
    QTabWidget* m_log_tabs{nullptr};

    // Mirroring components
    ScreenView* m_screen_view{nullptr};
    QPushButton* m_btn_toggle_mirror{nullptr};
    QLabel* m_fps_label{nullptr};
    int m_fps_counter{0};
    int64_t m_fps_last_time{0};

    // HiLog Tab
    QLineEdit* m_search_input{nullptr};
    QComboBox* m_pkg_combo{nullptr};
    QComboBox* m_level_combo{nullptr};
    QComboBox* m_cat_combo{nullptr};
    QPlainTextEdit* m_log_view{nullptr};
    QCheckBox* m_chk_autoscroll{nullptr};
    QCheckBox* m_chk_pause{nullptr};
    QLabel* m_log_count_label{nullptr};
    std::vector<HiLogEntry> m_cached_entries;

    // WebView Tab
    QComboBox* m_wv_instance_combo{nullptr};
    QComboBox* m_wv_page_combo{nullptr};
    QPushButton* m_btn_open_browser{nullptr};
    QPushButton* m_btn_export_curl{nullptr};
    QPlainTextEdit* m_wv_console_view{nullptr};
    QTableWidget* m_wv_network_table{nullptr};
    std::vector<WebViewInstance> m_cached_instances;
    std::vector<WebViewPage> m_cached_pages;
    std::map<std::string, WebNetworkRequest> m_web_requests;

    // Device Center Page
    QWidget* m_info_page{nullptr};
    QLabel* m_info_model{nullptr};
    QLabel* m_info_serial{nullptr};
    QLabel* m_info_version{nullptr};
    QLabel* m_info_uptime{nullptr};
    QLabel* m_info_display{nullptr};
    QProgressBar* m_mem_bar{nullptr};
    QLabel* m_mem_label{nullptr};
    QLineEdit* m_wireless_ip{nullptr};
    QLineEdit* m_wireless_port{nullptr};
    QListWidget* m_app_list{nullptr};

    // HDC Page
    QWidget* m_hdc_page{nullptr};
    QComboBox* m_hdc_preset_combo{nullptr};
    QLineEdit* m_hdc_input{nullptr};
    QPlainTextEdit* m_hdc_output{nullptr};

    void setupUi();
    void setupHeader();
    void setupSidebar();
    void setupWorkspace();
    void setupMirrorPanel();
    void setupRightTabPanel();
    QWidget* buildHiLogWidget();
    QWidget* buildWebViewWidget();
    void setupInfoPage();
    void setupHdcPage();
    void applyTheme();

    std::optional<Device> currentDevice() const;
    void renderHiLog();
    void showToast(const QString& message);
};

} // namespace harmonycast
