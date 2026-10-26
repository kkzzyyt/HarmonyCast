#pragma once

#include "Types.h"
#include "HdcClient.h"
#include "DeviceManager.h"
#include "StreamSession.h"
#include "HiLogSession.h"
#include "ScreenView.h"
#include "WebViewDebugger.h"
#include "IconManager.h"

#include <QMainWindow>
#include <QPointer>
#include <QStackedWidget>
#include <QSplitter>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTableWidget>
#include <QTabWidget>
#include <QButtonGroup>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSettings>
#include <QTimer>
#include <memory>
#include <vector>
#include <map>

namespace harmonycast {

class MainWindow : public QMainWindow {
    Q_OBJECT
    Q_DISABLE_COPY(MainWindow)

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    // Navigation
    void switchModule(int index);

    // Device events
    void onDevicesUpdated(const std::vector<Device>& devices);
    void onDeviceSelected(int index);
    void onDeviceComboActivated(int index);
    void refreshDevices();
    void disconnectCurrentDevice();
    void openDeviceCenter();
    void switchToSingleDevice(const std::string& endpoint);

    // Mirroring controls
    void startMirror();
    void stopMirror(bool userInitiated = true);
    void onQualityChanged(int index);
    void rotateDisplay();
    void saveScreenshot();
    void toggleFullscreen();
    void sendKey(const QString& key);
    void launchPrimaryAppShortcut();
    void exportFaultReport();

    // Workspace layout
    void setWorkspaceLayout(const QString& mode);

    // HiLog controls
    void onHiLogEntries(const std::vector<HiLogEntry>& entries);
    void renderHiLog();
    void clearHiLog();
    void exportHiLog();
    void setLogFontSize(int size);

    // WebView controls
    void switchLogSubtab(int index);
    void setWebViewSubtab(int index);
    void autoConnectWebView();
    void onWebConsoleMessage(const WebConsoleMessage& msg);
    void onWebNetworkRequest(const WebNetworkRequest& req);
    void onWebNetworkResponse(const QString& reqId, int status, const QString& mimeType);
    void clearWebViewConsole();
    void renderWebViewConsole();
    void executeWebViewJs();
    void openChromeInspect();
    void onNetworkTableContextMenu(const QPoint& pos);
    void onNetworkRowSelected();
    void filterNetworkTable();
    void formatResponseJson();

    // Device Info Panel
    void refreshDeviceInfo();
    void copyAllDeviceInfo();
    void copyToClipboard(const QString& text, const QString& hint);
    void filterPackageTable(const QString& query);
    void startYarwardApp(const QString& bundle, const QString& ability);
    void stopYarwardApp(const QString& bundle);
    void fetchAppLogsAction(const QString& bundle);
    void setWatchdogStatusAction(bool stop);
    void feedWatchdogAction();

    // HDC Presets Panel
    void onHdcPresetSelected(int index);
    void executeCustomHdc();
    void interruptHdcCommand();
    void clearHdcTerminal();
    void copyHdcOutput();
    void exportHdcOutput();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupUi();
    void setupShortcuts();
    void applyTheme();

    QWidget* buildInfoPanel();
    QWidget* buildWorkspacePage();
    QWidget* buildHiLogWidget();
    QWidget* buildWebViewPanel();
    QWidget* buildNetworkDetailPanel();
    QWidget* buildHdcPanel();

    void populateNetworkDetail(const WebNetworkRequest& req);
    void updateResponseBodyView(const QString& body, bool base64);
    static QString normalizeResourceType(const std::string& type, const std::string& mime, const std::string& url);

    void renderEmptyInfo(const QString& hint = "未检测到已连接的设备");
    void renderDeviceInfo(const DeviceInfo& info);
    bool isLogMatch(const HiLogEntry& e, const QString& filterPkg, const QString& filterLvl, const QString& query) const;
    static QString formatLogLine(const HiLogEntry& e);
    void showToast(const QString& msg);
    [[nodiscard]] std::string currentSerial() const;

    // Services
    std::shared_ptr<HdcClient> m_hdc;
    std::shared_ptr<DeviceManager> m_dev_mgr;
    std::unique_ptr<StreamSession> m_stream;
    std::unique_ptr<HiLogSession> m_hilog;
    std::unique_ptr<WebViewDebugger> m_webview;
    std::unique_ptr<QSettings> m_settings;

    std::vector<Device> m_cached_devices;
    DeviceInfo m_cached_info;
    std::vector<HiLogEntry> m_cached_logs;
    std::vector<WebConsoleMessage> m_cached_wv_logs;

    // UI: Sidebar
    QFrame* m_sidebar{nullptr};
    QPushButton* m_info_nav{nullptr};
    QPushButton* m_mirror_nav{nullptr};
    QPushButton* m_hdc_nav{nullptr};
    QPushButton* m_settings_btn{nullptr};

    // UI: Header
    QFrame* m_header{nullptr};
    QLabel* m_page_title{nullptr};
    QWidget* m_layout_switcher{nullptr};
    QPushButton* m_btn_layout_h{nullptr};
    QPushButton* m_btn_layout_v{nullptr};
    QPushButton* m_btn_layout_screen{nullptr};
    QPushButton* m_btn_layout_logs{nullptr};
    QComboBox* m_device_combo{nullptr};
    QPushButton* m_refresh_btn{nullptr};
    QPushButton* m_disconnect_btn{nullptr};
    QPushButton* m_device_hub_btn{nullptr};

    // UI: Main Stack
    QStackedWidget* m_pages{nullptr};

    // Page 0: Info Panel
    QLabel* m_info_badge{nullptr};
    QLabel* m_info_device_label{nullptr};
    QPushButton* m_btn_refresh_info{nullptr};
    QPushButton* m_btn_copy_all_info{nullptr};
    QVBoxLayout* m_info_cards_layout{nullptr};
    QFrame* m_card_empty{nullptr};
    QVBoxLayout* m_empty_layout{nullptr};
    QFrame* m_card_yarward{nullptr};
    QVBoxLayout* m_yarward_layout{nullptr};
    QFrame* m_card_icu{nullptr};
    QVBoxLayout* m_icu_layout{nullptr};
    QFrame* m_card_network{nullptr};
    QVBoxLayout* m_network_layout{nullptr};
    QFrame* m_card_device{nullptr};
    QVBoxLayout* m_device_layout{nullptr};
    QFrame* m_card_packages{nullptr};
    QVBoxLayout* m_packages_layout{nullptr};
    QLineEdit* m_pkg_search{nullptr};
    QTableWidget* m_pkg_table{nullptr};

    // Page 1: Workspace Splitter
    QSplitter* m_workspace_splitter{nullptr};
    QWidget* m_mirror_page{nullptr};
    QFrame* m_mirror_toolbar{nullptr};
    QPushButton* m_start_btn{nullptr};
    QPushButton* m_stop_btn{nullptr};
    QComboBox* m_quality_combo{nullptr};
    QPushButton* m_rotate_btn{nullptr};
    QPushButton* m_screenshot_btn{nullptr};
    QPushButton* m_btn_key_back{nullptr};
    QPushButton* m_btn_key_home{nullptr};
    QPushButton* m_restore_primary_btn{nullptr};
    QPushButton* m_fault_report_btn{nullptr};
    QPushButton* m_fullscreen_btn{nullptr};
    QLabel* m_state_badge{nullptr};
    ScreenView* m_screen{nullptr};

    // Workspace Right: Logs & WebView
    QWidget* m_log_panel{nullptr};
    QPushButton* m_btn_log_tab_device{nullptr};
    QPushButton* m_btn_log_tab_webview{nullptr};
    QStackedWidget* m_log_subtab_stack{nullptr};

    // HiLog
    QComboBox* m_package_combo{nullptr};
    QComboBox* m_level_combo{nullptr};
    QLineEdit* m_search_input{nullptr};
    QCheckBox* m_autoscroll_cb{nullptr};
    QCheckBox* m_pause_cb{nullptr};
    QCheckBox* m_only_errors_cb{nullptr};
    QPushButton* m_clear_log_btn{nullptr};
    QPushButton* m_export_log_btn{nullptr};
    QPlainTextEdit* m_log_view{nullptr};
    QLabel* m_log_count{nullptr};
    QSpinBox* m_log_font_size{nullptr};
    std::unique_ptr<class LogHighlighter> m_log_highlighter;
    std::unique_ptr<class LogHighlighter> m_wv_log_highlighter;

    // WebView
    QPushButton* m_btn_subtab_console{nullptr};
    QPushButton* m_btn_subtab_network{nullptr};
    QComboBox* m_wv_filter_combo{nullptr};
    QLineEdit* m_wv_filter_input{nullptr};
    QCheckBox* m_wv_autoscroll{nullptr};
    QCheckBox* m_wv_pause{nullptr};
    QPushButton* m_wv_clear_btn{nullptr};
    QPushButton* m_wv_export_btn{nullptr};
    QPushButton* m_wv_chrome_btn{nullptr};
    QStackedWidget* m_wv_right_stack{nullptr};
    QPlainTextEdit* m_wv_console_view{nullptr};
    QLineEdit* m_wv_js_input{nullptr};
    QPushButton* m_btn_wv_eval{nullptr};
    QTableWidget* m_wv_network_table{nullptr};
    QLabel* m_wv_net_status_label{nullptr};
    QSpinBox* m_wv_font_size{nullptr};
    QButtonGroup* m_net_type_btn_group{nullptr};
    QString m_current_net_type_filter{"ALL"};
    QSplitter* m_wv_net_splitter{nullptr};
    QFrame* m_wv_net_detail_panel{nullptr};
    QLabel* m_detail_method_badge{nullptr};
    QLabel* m_detail_url_label{nullptr};
    QLabel* m_detail_status_badge{nullptr};
    QTabWidget* m_net_detail_tabs{nullptr};
    QTextEdit* m_net_headers_view{nullptr};
    QPlainTextEdit* m_net_payload_view{nullptr};
    QPlainTextEdit* m_net_response_view{nullptr};
    QPlainTextEdit* m_net_curl_view{nullptr};
    QLabel* m_net_resp_info_label{nullptr};
    std::string m_current_selected_req_id;
    bool m_response_is_json{false};
    QString m_raw_response_body;

    // Page 2: HDC Command Presets
    QLabel* m_hdc_target_badge{nullptr};
    QLabel* m_hdc_desc_label{nullptr};
    QComboBox* m_hdc_preset_combo{nullptr};
    QLabel* m_hdc_target_prefix{nullptr};
    QLineEdit* m_hdc_input{nullptr};
    QPushButton* m_btn_run_hdc{nullptr};
    QPushButton* m_btn_kill_hdc{nullptr};
    QLabel* m_hdc_status_label{nullptr};
    QTextEdit* m_hdc_terminal{nullptr};

    // State
    int m_user_rotation{0};
    int m_bitrate_idx{1};
    int m_log_font_size_val{11};
    QString m_current_layout{"horizontal"};
    bool m_projecting{false};
    bool m_info_loading{false};
    bool m_hdc_running{false};
    bool m_auto_start_mirror{true};
    bool m_user_stopped_mirror{false};
    std::string m_pending_auto_start_serial;
    std::string m_active_device_serial;
    uint64_t m_switch_generation{0};
    int m_wv_console_count{0};
    int m_wv_network_count{0};
    int m_visible_log_count{0};
    std::map<std::string, WebNetworkRequest> m_wv_requests; // for cURL generation
    std::map<std::string, qint64> m_wv_request_times;       // requestId -> ms since epoch
};

} // namespace harmonycast
