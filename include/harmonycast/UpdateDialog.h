#pragma once

#include <QDialog>
#include <QFile>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include "harmonycast/UpdateChecker.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QTextEdit;

namespace harmonycast {

enum class UpdateState {
    Ready,       // Shows new version changelog, "立即在线升级" button
    Downloading, // In-app downloading with live progress bar and speed
    Downloaded,  // Download completed, "立即重启升级" button
    Installing,  // Executing installer script and quitting
    Error        // Download failed, retry or browser fallback
};

class UpdateDialog : public QDialog {
    Q_OBJECT
public:
    explicit UpdateDialog(const UpdateInfo& info, QWidget* parent = nullptr);
    ~UpdateDialog() override;

    [[nodiscard]] bool wasIgnored() const { return m_ignored; }

private slots:
    void onPrimaryActionClicked();
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onDownloadReadyRead();
    void onDownloadFinished();
    void onDownloadError(QNetworkReply::NetworkError code);
    void onBrowserDownloadClicked();
    void onIgnoreClicked();
    void cancelDownload();

private:
    UpdateInfo m_info;
    bool m_ignored{false};
    UpdateState m_state{UpdateState::Ready};

    // UI elements
    QLabel* m_title_label{nullptr};
    QTextEdit* m_changelog_view{nullptr};
    QWidget* m_progress_container{nullptr};
    QProgressBar* m_progress_bar{nullptr};
    QLabel* m_status_label{nullptr};
    QLabel* m_speed_label{nullptr};

    QPushButton* m_btn_primary{nullptr};
    QPushButton* m_btn_secondary{nullptr};
    QPushButton* m_btn_browser{nullptr};
    QPushButton* m_btn_ignore{nullptr};

    // Download & Install internals
    QNetworkAccessManager* m_nam{nullptr};
    QNetworkReply* m_reply{nullptr};
    QFile m_temp_file;
    QString m_downloaded_dmg_path;
    QElapsedTimer m_download_timer;
    qint64 m_last_bytes{0};
    qint64 m_last_time{0};

    void setupUi();
    void setState(UpdateState state);
    void startDownload();
    void executeInstallAndRelaunch();
};

} // namespace harmonycast
