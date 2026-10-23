#pragma once

#include "Types.h"
#include "HdcClient.h"
#include "DeviceManager.h"
#include "StreamSession.h"
#include "ScreenView.h"
#include "HiLogSession.h"

#include <QMainWindow>
#include <QStackedWidget>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QCheckBox>
#include <memory>

namespace harmonycast {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onDevicesUpdated(const std::vector<Device>& devices);
    void onDeviceSelected(int index);
    void onRefreshClicked();
    void onToggleMirror();
    void onStopMirror();
    void onFrameReady(const DecodedFrame& frame);
    void onStreamStatusChanged(const QString& status);
    void onStreamFinished(const QString& error);

    // Mirroring Actions
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

    // HDC Actions
    void onHdcRunCommand();
    void onHdcPresetSelected(int index);

    // Device Info Actions
    void onRefreshDeviceInfo();

private:
    std::shared_ptr<HdcClient> m_hdc;
    std::shared_ptr<DeviceManager> m_dev_mgr;
    std::unique_ptr<StreamSession> m_stream;
    std::unique_ptr<HiLogSession> m_hilog;

    // UI Widgets
    QComboBox* m_device_combo{nullptr};
    QLabel* m_state_badge{nullptr};
    QPushButton* m_refresh_btn{nullptr};
    QComboBox* m_mode_combo{nullptr};
    QComboBox* m_quality_combo{nullptr};

    // Sidebar
    QPushButton* m_nav_mirror{nullptr};
    QPushButton* m_nav_hilog{nullptr};
    QPushButton* m_nav_info{nullptr};
    QPushButton* m_nav_hdc{nullptr};
    QStackedWidget* m_pages{nullptr};

    // Page 0: Mirror
    QWidget* m_mirror_page{nullptr};
    ScreenView* m_screen_view{nullptr};
    QPushButton* m_btn_toggle_mirror{nullptr};
    QLabel* m_fps_label{nullptr};
    int m_fps_counter{0};
    int64_t m_fps_last_time{0};

    // Page 1: HiLog
    QWidget* m_hilog_page{nullptr};
    QLineEdit* m_search_input{nullptr};
    QComboBox* m_pkg_combo{nullptr};
    QComboBox* m_level_combo{nullptr};
    QComboBox* m_cat_combo{nullptr};
    QPlainTextEdit* m_log_view{nullptr};
    QCheckBox* m_chk_autoscroll{nullptr};
    QCheckBox* m_chk_pause{nullptr};
    QLabel* m_log_count_label{nullptr};
    std::vector<HiLogEntry> m_cached_entries;

    // Page 2: Info
    QWidget* m_info_page{nullptr};
    QLabel* m_info_model{nullptr};
    QLabel* m_info_serial{nullptr};
    QLabel* m_info_version{nullptr};
    QLabel* m_info_uptime{nullptr};
    QLabel* m_info_display{nullptr};
    QProgressBar* m_mem_bar{nullptr};
    QLabel* m_mem_label{nullptr};

    // Page 3: HDC
    QWidget* m_hdc_page{nullptr};
    QComboBox* m_hdc_preset_combo{nullptr};
    QLineEdit* m_hdc_input{nullptr};
    QPlainTextEdit* m_hdc_output{nullptr};

    void setupUi();
    void setupHeader();
    void setupSidebar();
    void setupMirrorPage();
    void setupHiLogPage();
    void setupInfoPage();
    void setupHdcPage();
    void applyTheme();

    std::optional<Device> currentDevice() const;
    void renderHiLog();
};

} // namespace harmonycast
