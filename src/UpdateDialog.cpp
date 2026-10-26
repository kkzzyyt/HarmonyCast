#include "harmonycast/UpdateDialog.h"
#include "harmonycast/IconManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>
#include <QDesktopServices>
#include <QUrl>

namespace harmonycast {

UpdateDialog::UpdateDialog(const UpdateInfo& info, QWidget* parent)
    : QDialog(parent), m_info(info) {
    setWindowTitle("发现新版本 · HarmonyCast");
    resize(520, 420);
    setStyleSheet(
        "QDialog { background: #0c1219; color: #dbe5ee; } "
        "QLabel { color: #dbe5ee; } "
        "QTextEdit { background: #070c13; color: #c9d1d9; border: 1px solid #1c2b3d; border-radius: 6px; font-size: 12px; padding: 10px; line-height: 1.4; }"
    );

    setupUi();
}

void UpdateDialog::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(14);

    // 1. Top Header: Rocket/Update badge + Title
    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(10);

    auto* badge = new QLabel("NEW");
    badge->setStyleSheet("background: #0d382d; color: #5ce0c1; font-weight: bold; font-size: 11px; padding: 3px 8px; border-radius: 4px; border: 1px solid #1e5948;");
    topRow->addWidget(badge);

    auto* title = new QLabel(m_info.releaseTitle.isEmpty() ? "HarmonyCast 发现新版本！" : m_info.releaseTitle);
    title->setStyleSheet("font-size: 15px; font-weight: bold; color: #ffffff;");
    topRow->addWidget(title, 1);
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

    auto* changelogView = new QTextEdit();
    changelogView->setReadOnly(true);
    changelogView->setPlainText(m_info.changelog.isEmpty() ? "• 体验优化与常规稳定性改进" : m_info.changelog);
    layout->addWidget(changelogView, 1);

    // 4. Buttons
    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);

    auto* btnIgnore = IconManager::button("忽略此版本", "");
    btnIgnore->setFixedHeight(30);
    btnIgnore->setToolTip("后续在静默检查中不再提示此版本");
    connect(btnIgnore, &QPushButton::clicked, this, &UpdateDialog::onIgnoreClicked);
    btnRow->addWidget(btnIgnore);

    btnRow->addStretch(1);

    auto* btnLater = IconManager::button("稍后提醒", "");
    btnLater->setFixedHeight(30);
    connect(btnLater, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(btnLater);

    auto* btnDownload = IconManager::button("前往下载更新", "Primary");
    btnDownload->setFixedHeight(30);
    connect(btnDownload, &QPushButton::clicked, this, &UpdateDialog::onDownloadClicked);
    btnRow->addWidget(btnDownload);

    layout->addLayout(btnRow);
}

void UpdateDialog::onDownloadClicked() {
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

} // namespace harmonycast
