#include "harmonycast/UpdateDialog.h"
#include "harmonycast/IconManager.h"
#include "harmonycast/Logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QProcess>
#include <QCoreApplication>
#include <QApplication>
#include <QTextStream>

namespace harmonycast {

UpdateDialog::UpdateDialog(const UpdateInfo& info, QWidget* parent)
    : QDialog(parent),
      m_info(info),
      m_nam(new QNetworkAccessManager(this)) {
    setWindowTitle("发现新版本 · HarmonyCast");
    resize(540, 440);
    setStyleSheet(
        "QDialog { background: #0c1219; color: #dbe5ee; } "
        "QLabel { color: #dbe5ee; } "
        "QTextEdit { background: #070c13; color: #c9d1d9; border: 1px solid #1c2b3d; border-radius: 6px; font-size: 12px; padding: 10px; line-height: 1.4; }"
    );

    setupUi();
    setState(UpdateState::Ready);
}

UpdateDialog::~UpdateDialog() {
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    if (m_temp_file.isOpen()) {
        m_temp_file.close();
    }
}

void UpdateDialog::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);

    // 1. Top Header: NEW badge + Title
    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(10);

    auto* badge = new QLabel("NEW");
    badge->setStyleSheet("background: #0d382d; color: #5ce0c1; font-weight: bold; font-size: 11px; padding: 3px 8px; border-radius: 4px; border: 1px solid #1e5948;");
    topRow->addWidget(badge);

    m_title_label = new QLabel(m_info.releaseTitle.isEmpty() ? "HarmonyCast 发现新版本！" : m_info.releaseTitle);
    m_title_label->setStyleSheet("font-size: 15px; font-weight: bold; color: #ffffff;");
    topRow->addWidget(m_title_label, 1);
    layout->addLayout(topRow);

    // 2. Version comparison strip
    auto* verStrip = new QFrame();
    verStrip->setStyleSheet("background: #111a26; border: 1px solid #1e2c3d; border-radius: 6px; padding: 8px;");
    auto* verLayout = new QHBoxLayout(verStrip);
    verLayout->setContentsMargins(12, 6, 12, 6);

    auto* curVerLabel = new QLabel(QString("当前版本: v%1").arg(m_info.currentVersion));
    curVerLabel->setStyleSheet("color: #8ba2b9; font-size: 12px;");
    verLayout->addWidget(curVerLabel);

    auto* arrowLabel = new QLabel("➔");
    arrowLabel->setStyleSheet("color: #5ce0c1; font-weight: bold; font-size: 13px;");
    verLayout->addWidget(arrowLabel);

    auto* newVerLabel = new QLabel(QString("最新版本: v%1").arg(m_info.latestVersion));
    newVerLabel->setStyleSheet("color: #5ce0c1; font-weight: bold; font-size: 13px;");
    verLayout->addWidget(newVerLabel);

    verLayout->addStretch(1);

    if (!m_info.releaseDate.isEmpty()) {
        auto* dateLabel = new QLabel(QString("发布于 %1").arg(m_info.releaseDate));
        dateLabel->setStyleSheet("color: #63778a; font-size: 11px;");
        verLayout->addWidget(dateLabel);
    }
    layout->addWidget(verStrip);

    // 3. Changelog box
    auto* clHeader = new QLabel("更新内容：");
    clHeader->setStyleSheet("font-size: 12px; font-weight: 600; color: #8ba2b9;");
    layout->addWidget(clHeader);

    m_changelog_view = new QTextEdit();
    m_changelog_view->setReadOnly(true);
    m_changelog_view->setPlainText(m_info.changelog.isEmpty() ? "• 体验优化与常规稳定性改进" : m_info.changelog);
    layout->addWidget(m_changelog_view, 1);

    // 4. Download Progress Container (Shown during and after download)
    m_progress_container = new QWidget();
    auto* progLayout = new QVBoxLayout(m_progress_container);
    progLayout->setContentsMargins(0, 4, 0, 4);
    progLayout->setSpacing(6);

    auto* statusRow = new QHBoxLayout();
    m_status_label = new QLabel("准备下载…");
    m_status_label->setStyleSheet("font-size: 12px; color: #9bb0c4;");
    statusRow->addWidget(m_status_label, 1);

    m_speed_label = new QLabel("");
    m_speed_label->setStyleSheet("font-size: 11px; color: #6e8499;");
    statusRow->addWidget(m_speed_label);
    progLayout->addLayout(statusRow);

    m_progress_bar = new QProgressBar();
    m_progress_bar->setRange(0, 100);
    m_progress_bar->setValue(0);
    m_progress_bar->setTextVisible(true);
    m_progress_bar->setFixedHeight(16);
    m_progress_bar->setStyleSheet(
        "QProgressBar { background: #070c13; border: 1px solid #1c2b3d; border-radius: 4px; text-align: center; color: #ffffff; font-size: 10px; } "
        "QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1a6d59, stop:1 #5ce0c1); border-radius: 3px; }"
    );
    progLayout->addWidget(m_progress_bar);

    layout->addWidget(m_progress_container);

    // 5. Buttons Row
    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);

    m_btn_ignore = IconManager::button("忽略此版本", "");
    m_btn_ignore->setFixedHeight(32);
    m_btn_ignore->setToolTip("后续在静默检查中不再提示此版本");
    connect(m_btn_ignore, &QPushButton::clicked, this, &UpdateDialog::onIgnoreClicked);
    btnRow->addWidget(m_btn_ignore);

    m_btn_browser = IconManager::button("浏览器下载", "");
    m_btn_browser->setFixedHeight(32);
    m_btn_browser->setToolTip("直接通过浏览器下载 DMG 安装包");
    connect(m_btn_browser, &QPushButton::clicked, this, &UpdateDialog::onBrowserDownloadClicked);
    btnRow->addWidget(m_btn_browser);

    btnRow->addStretch(1);

    m_btn_secondary = IconManager::button("稍后提醒", "");
    m_btn_secondary->setFixedHeight(32);
    connect(m_btn_secondary, &QPushButton::clicked, this, [this]() {
        if (m_state == UpdateState::Downloading) {
            cancelDownload();
        } else {
            reject();
        }
    });
    btnRow->addWidget(m_btn_secondary);

    m_btn_primary = IconManager::button("立即在线升级", "Primary");
    m_btn_primary->setFixedHeight(32);
    connect(m_btn_primary, &QPushButton::clicked, this, &UpdateDialog::onPrimaryActionClicked);
    btnRow->addWidget(m_btn_primary);

    layout->addLayout(btnRow);
}

void UpdateDialog::setState(UpdateState state) {
    m_state = state;

    switch (state) {
    case UpdateState::Ready:
        m_progress_container->hide();
        m_changelog_view->show();
        m_btn_primary->setText("立即在线升级");
        m_btn_primary->setEnabled(true);
        m_btn_secondary->setText("稍后提醒");
        m_btn_browser->show();
        m_btn_ignore->show();
        break;

    case UpdateState::Downloading:
        m_progress_container->show();
        m_progress_bar->setValue(0);
        m_status_label->setText("正在连接更新服务器…");
        m_status_label->setStyleSheet("font-size: 12px; color: #9bb0c4;");
        m_speed_label->setText("");
        m_btn_primary->setText("正在下载…");
        m_btn_primary->setEnabled(false);
        m_btn_secondary->setText("取消下载");
        m_btn_browser->show();
        m_btn_ignore->hide();
        break;

    case UpdateState::Downloaded:
        m_progress_container->show();
        m_progress_bar->setValue(100);
        m_status_label->setText("✓ 升级包下载就绪！点击「立即重启升级」将自动替换并启动新版本。");
        m_status_label->setStyleSheet("color: #5ce0c1; font-weight: bold; font-size: 12px;");
        m_speed_label->setText("");
        m_btn_primary->setText("立即重启升级");
        m_btn_primary->setEnabled(true);
        m_btn_primary->setStyleSheet("background: #00a688; color: #ffffff; font-weight: bold; font-size: 13px; border-radius: 4px; padding: 6px 16px;");
        m_btn_secondary->setText("稍后安装");
        m_btn_browser->hide();
        m_btn_ignore->hide();
        break;

    case UpdateState::Installing:
        m_progress_container->show();
        m_status_label->setText("正在执行自动替换安装并准备重启应用…");
        m_btn_primary->setEnabled(false);
        m_btn_secondary->setEnabled(false);
        break;

    case UpdateState::Error:
        m_progress_container->show();
        m_status_label->setText("下载失败，请检查网络或点击「浏览器下载」");
        m_status_label->setStyleSheet("color: #ff6b6b; font-size: 12px;");
        m_btn_primary->setText("重试下载");
        m_btn_primary->setEnabled(true);
        m_btn_secondary->setText("关闭");
        m_btn_browser->show();
        m_btn_ignore->show();
        break;
    }
}

void UpdateDialog::onPrimaryActionClicked() {
    if (m_state == UpdateState::Ready || m_state == UpdateState::Error) {
        startDownload();
    } else if (m_state == UpdateState::Downloaded) {
        executeInstallAndRelaunch();
    }
}

void UpdateDialog::startDownload() {
    QString urlStr = m_info.downloadUrl.isEmpty() ? m_info.pageUrl : m_info.downloadUrl;
    QUrl url(urlStr);
    if (!url.isValid()) {
        setState(UpdateState::Error);
        m_status_label->setText("下载地址无效: " + urlStr);
        return;
    }

    QString filename = QFileInfo(url.path()).fileName();
    if (filename.isEmpty()) {
        filename = QString("HarmonyCast-%1.dmg").arg(m_info.latestVersion);
    }

    m_downloaded_dmg_path = QDir::tempPath() + "/" + filename;
    if (m_temp_file.isOpen()) {
        m_temp_file.close();
    }
    m_temp_file.setFileName(m_downloaded_dmg_path);
    if (!m_temp_file.open(QIODevice::WriteOnly)) {
        setState(UpdateState::Error);
        m_status_label->setText("无法创建临时下载文件: " + m_downloaded_dmg_path);
        return;
    }

    setState(UpdateState::Downloading);
    m_status_label->setText(QString("正在下载: %1").arg(filename));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "HarmonyCast-Updater/" + m_info.currentVersion);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_last_bytes = 0;
    m_last_time = 0;
    m_download_timer.restart();

    HC_LOG_INFO(QString("Starting in-app download: %1 -> %2").arg(urlStr, m_downloaded_dmg_path));
    m_reply = m_nam->get(request);

    connect(m_reply, &QNetworkReply::downloadProgress, this, &UpdateDialog::onDownloadProgress);
    connect(m_reply, &QNetworkReply::readyRead, this, &UpdateDialog::onDownloadReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &UpdateDialog::onDownloadFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &UpdateDialog::onDownloadError);
}

void UpdateDialog::cancelDownload() {
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    if (m_temp_file.isOpen()) {
        m_temp_file.close();
        m_temp_file.remove();
    }
    HC_LOG_INFO("User cancelled in-app download");
    setState(UpdateState::Ready);
}

void UpdateDialog::onDownloadReadyRead() {
    if (m_reply && m_temp_file.isOpen()) {
        m_temp_file.write(m_reply->readAll());
    }
}

void UpdateDialog::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    if (bytesTotal > 0) {
        int percent = static_cast<int>((bytesReceived * 100) / bytesTotal);
        m_progress_bar->setValue(percent);

        qint64 elapsed = m_download_timer.elapsed();
        if (elapsed - m_last_time >= 500 && elapsed > 0) {
            double speed = static_cast<double>(bytesReceived - m_last_bytes) / (static_cast<double>(elapsed - m_last_time) / 1000.0);
            m_last_bytes = bytesReceived;
            m_last_time = elapsed;

            QString speedStr;
            if (speed >= 1024 * 1024) {
                speedStr = QString("%1 MB/s").arg(speed / (1024.0 * 1024.0), 0, 'f', 1);
            } else {
                speedStr = QString("%1 KB/s").arg(speed / 1024.0, 0, 'f', 0);
            }

            double curMB = static_cast<double>(bytesReceived) / (1024.0 * 1024.0);
            double totalMB = static_cast<double>(bytesTotal) / (1024.0 * 1024.0);

            m_status_label->setText(QString("正在下载: %1 MB / %2 MB (%3%)")
                                        .arg(curMB, 0, 'f', 1)
                                        .arg(totalMB, 0, 'f', 1)
                                        .arg(percent));
            m_speed_label->setText(speedStr);
        }
    }
}

void UpdateDialog::onDownloadFinished() {
    if (!m_reply) return;

    if (m_temp_file.isOpen()) {
        m_temp_file.write(m_reply->readAll());
        m_temp_file.flush();
        m_temp_file.close();
    }

    if (m_reply->error() == QNetworkReply::NoError) {
        HC_LOG_INFO(QString("Download completed successfully: %1 (%2 bytes)")
                        .arg(m_downloaded_dmg_path).arg(QFileInfo(m_downloaded_dmg_path).size()));
        setState(UpdateState::Downloaded);
    } else if (m_reply->error() != QNetworkReply::OperationCanceledError) {
        HC_LOG_WARN(QString("Download finished with error: %1").arg(m_reply->errorString()));
        setState(UpdateState::Error);
    }

    m_reply->deleteLater();
    m_reply = nullptr;
}

void UpdateDialog::onDownloadError(QNetworkReply::NetworkError code) {
    if (code == QNetworkReply::OperationCanceledError) return;
    HC_LOG_WARN(QString("Download network error occurred: %1").arg(m_reply ? m_reply->errorString() : "Unknown"));
    setState(UpdateState::Error);
}

void UpdateDialog::onBrowserDownloadClicked() {
    QString targetUrl = m_info.downloadUrl.isEmpty() ? m_info.pageUrl : m_info.downloadUrl;
    if (!targetUrl.isEmpty()) {
        QDesktopServices::openUrl(QUrl(targetUrl));
    }
    accept();
}

void UpdateDialog::onIgnoreClicked() {
    m_ignored = true;
    accept();
}

void UpdateDialog::executeInstallAndRelaunch() {
    if (m_downloaded_dmg_path.isEmpty() || !QFile::exists(m_downloaded_dmg_path)) {
        setState(UpdateState::Error);
        m_status_label->setText("未找到已下载的升级包，请重新下载");
        return;
    }

    setState(UpdateState::Installing);

    QString appDirPath = QCoreApplication::applicationDirPath();
    QDir dir(appDirPath);
    dir.cdUp(); // Contents
    dir.cdUp(); // HarmonyCast.app
    QString targetAppPath = dir.canonicalPath();

    HC_LOG_INFO(QString("Executing auto-update install. Target App: %1, DMG: %2")
                    .arg(targetAppPath, m_downloaded_dmg_path));

    // If running outside a standard .app bundle (e.g. CLI debug), open the DMG directly
    if (!targetAppPath.endsWith(".app")) {
#ifdef Q_OS_MAC
        QProcess::startDetached("open", {m_downloaded_dmg_path});
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_downloaded_dmg_path));
#endif
        accept();
        return;
    }

    qint64 pid = QCoreApplication::applicationPid();
    QString scriptPath = QDir::tempPath() + QString("/harmonycast_update_%1.sh").arg(pid);

    QFile script(scriptPath);
    if (script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&script);
        out << "#!/bin/bash\n"
            << "TARGET_APP=\"" << targetAppPath << "\"\n"
            << "DMG_PATH=\"" << m_downloaded_dmg_path << "\"\n"
            << "APP_PID=" << pid << "\n"
            << "LOG_FILE=\"/tmp/harmonycast_update.log\"\n\n"
            << "echo \"[$(date)] Starting auto update for PID $APP_PID to $TARGET_APP\" >> \"$LOG_FILE\"\n\n"
            << "# 1. Wait for current application process to exit\n"
            << "while kill -0 \"$APP_PID\" 2>/dev/null; do\n"
            << "    sleep 0.1\n"
            << "done\n\n"
            << "# 2. Mount DMG silently\n"
            << "MOUNT_DIR=$(mktemp -d /tmp/hc_mount_XXXXXX)\n"
            << "hdiutil attach \"$DMG_PATH\" -nobrowse -mountpoint \"$MOUNT_DIR\" -quiet || \\\n"
            << "hdiutil attach \"$DMG_PATH\" -nobrowse -mountpoint \"$MOUNT_DIR\"\n\n"
            << "if [ -d \"$MOUNT_DIR/HarmonyCast.app\" ]; then\n"
            << "    echo \"Replacing $TARGET_APP...\" >> \"$LOG_FILE\"\n"
            << "    rm -rf \"$TARGET_APP\"\n"
            << "    cp -R \"$MOUNT_DIR/HarmonyCast.app\" \"$TARGET_APP\"\n"
            << "    xattr -r -d com.apple.quarantine \"$TARGET_APP\" 2>/dev/null || true\n"
            << "    echo \"Replacement succeeded!\" >> \"$LOG_FILE\"\n"
            << "else\n"
            << "    echo \"Error: HarmonyCast.app not found in mounted DMG!\" >> \"$LOG_FILE\"\n"
            << "fi\n\n"
            << "# 3. Cleanup\n"
            << "hdiutil detach \"$MOUNT_DIR\" -quiet 2>/dev/null || hdiutil detach \"$MOUNT_DIR\" -force 2>/dev/null || true\n"
            << "rm -rf \"$MOUNT_DIR\"\n"
            << "rm -f \"$DMG_PATH\"\n\n"
            << "# 4. Relaunch new application\n"
            << "open \"$TARGET_APP\"\n"
            << "echo \"[$(date)] Relaunched $TARGET_APP\" >> \"$LOG_FILE\"\n"
            << "rm -f \"$0\"\n";
        script.close();
        script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                              QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                              QFileDevice::ReadOther | QFileDevice::ExeOther);
    }

    // Launch detached updater script and terminate current app
    QProcess::startDetached("/bin/bash", {scriptPath});
    QApplication::quit();
}

} // namespace harmonycast
