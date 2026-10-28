#include "harmonycast/UpdateChecker.h"
#include "harmonycast/Logger.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <QRegularExpression>
#include <QTimer>

namespace harmonycast {

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent),
      m_nam(new QNetworkAccessManager(this)) {
}

int UpdateChecker::compareVersions(const QString& v1, const QString& v2) {
    auto cleanVersion = [](QString v) -> QStringList {
        v = v.trimmed();
        if (v.startsWith('v') || v.startsWith('V')) {
            v.remove(0, 1);
        }
        int dashIdx = v.indexOf('-');
        if (dashIdx >= 0) {
            v = v.left(dashIdx);
        }
        return v.split('.', Qt::SkipEmptyParts);
    };

    QStringList parts1 = cleanVersion(v1);
    QStringList parts2 = cleanVersion(v2);

    int maxLen = std::max(parts1.size(), parts2.size());
    for (int i = 0; i < maxLen; ++i) {
        int num1 = (i < parts1.size()) ? parts1[i].toInt() : 0;
        int num2 = (i < parts2.size()) ? parts2[i].toInt() : 0;
        if (num1 > num2) return 1;
        if (num1 < num2) return -1;
    }
    return 0;
}

void UpdateChecker::checkForUpdates(bool silent) {
    m_silent_check = silent;
    emit checkStarted();

    if (m_current_reply) {
        m_current_reply->abort();
        m_current_reply->deleteLater();
        m_current_reply = nullptr;
    }

    QUrl url(m_update_url);
    if (!url.isValid()) {
        HC_LOG_WARN("Update check failed: invalid URL: " + m_update_url);
        emit checkFailed("无效的更新检查地址", m_silent_check);
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "HarmonyCast-Updater/" + m_current_version);
    request.setRawHeader("Accept", "application/vnd.github.v3+json, application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    HC_LOG_INFO(QString("Checking for updates from %1 (silent=%2)").arg(m_update_url).arg(silent ? "true" : "false"));
    m_current_reply = m_nam->get(request);

    // 6-second timeout watchdog
    QTimer* timer = new QTimer(m_current_reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this]() {
        if (m_current_reply && m_current_reply->isRunning()) {
            HC_LOG_WARN("Update check timed out after 6000ms");
            m_current_reply->abort();
        }
    });
    timer->start(6000);

    connect(m_current_reply, &QNetworkReply::finished, this, &UpdateChecker::onReplyFinished);
}

void UpdateChecker::onReplyFinished() {
    if (!m_current_reply) return;

    QNetworkReply* reply = m_current_reply;
    m_current_reply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QString err = reply->errorString();
        HC_LOG_WARN("Update check network error: " + err);
        emit checkFailed(err, m_silent_check);
        return;
    }

    QByteArray data = reply->readAll();
    UpdateInfo info = parseManifest(data);

    if (info.latestVersion.isEmpty()) {
        HC_LOG_WARN("Failed to parse update manifest JSON");
        emit checkFailed("无法解析远程版本数据", m_silent_check);
        return;
    }

    info.currentVersion = m_current_version;
    bool hasNewer = (compareVersions(info.latestVersion, m_current_version) > 0);
    info.hasUpdate = hasNewer;

    HC_LOG_INFO(QString("Update check result: current=%1, latest=%2, hasUpdate=%3")
                    .arg(m_current_version, info.latestVersion, hasNewer ? "true" : "false"));

    if (hasNewer) {
        if (m_silent_check && isVersionIgnored(info.latestVersion)) {
            HC_LOG_INFO(QString("Version %1 is ignored by user in silent check").arg(info.latestVersion));
            emit noUpdateAvailable(m_silent_check);
            return;
        }
        emit updateAvailable(info, m_silent_check);
    } else {
        emit noUpdateAvailable(m_silent_check);
    }
}

UpdateInfo UpdateChecker::parseManifest(const QByteArray& data) const {
    UpdateInfo info;
    info.currentVersion = m_current_version;

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) return info;

    QJsonObject obj = doc.object();

    // 1. GitHub Releases API format: tag_name, name, body, html_url, assets
    if (obj.contains("tag_name")) {
        QString tag = obj["tag_name"].toString();
        if (tag.startsWith('v') || tag.startsWith('V')) {
            info.latestVersion = tag.mid(1);
        } else {
            info.latestVersion = tag;
        }
        info.releaseTitle = obj["name"].toString();
        info.changelog = obj["body"].toString();
        info.pageUrl = obj["html_url"].toString();
        info.releaseDate = obj["published_at"].toString().left(10);

        // Keep each edition on its own platform/runtime when updating.
        QJsonArray assets = obj["assets"].toArray();
        for (const auto& a : assets) {
            QJsonObject assetObj = a.toObject();
            QString name = assetObj["name"].toString();
            bool matches = false;
#if defined(HARMONYCAST_SERVER2012_COMPAT)
            matches = name.endsWith("-Windows-Server2012-x64.zip", Qt::CaseInsensitive);
#elif defined(Q_OS_WIN)
            matches = name.endsWith("-Windows-x64.zip", Qt::CaseInsensitive);
#elif defined(Q_OS_MACOS)
            matches = name.endsWith(".dmg", Qt::CaseInsensitive) ||
                      (name.contains("macOS", Qt::CaseInsensitive) && name.endsWith(".zip", Qt::CaseInsensitive));
#endif
            if (matches) {
                info.downloadUrl = assetObj["browser_download_url"].toString();
                break;
            }
        }
        if (info.downloadUrl.isEmpty()) {
            info.downloadUrl = info.pageUrl;
        }
    }
    // 2. Generic custom manifest format: version, title, changelog, download_url, page_url
    else if (obj.contains("version")) {
        info.latestVersion = obj["version"].toString();
        info.releaseTitle = obj["title"].toString();
        info.changelog = obj["changelog"].toString();
        info.downloadUrl = obj["download_url"].toString();
        info.pageUrl = obj["page_url"].toString();
        info.releaseDate = obj["release_date"].toString();
        info.mandatory = obj["mandatory"].toBool(false);

        if (info.pageUrl.isEmpty()) {
            info.pageUrl = info.downloadUrl;
        }
    }

    return info;
}

void UpdateChecker::ignoreVersion(const QString& version) {
    QSettings settings("HarmonyCast", "HarmonyCast");
    settings.setValue("ignored_update_version", version);
}

bool UpdateChecker::isVersionIgnored(const QString& version) const {
    QSettings settings("HarmonyCast", "HarmonyCast");
    return settings.value("ignored_update_version").toString() == version;
}

void UpdateChecker::clearIgnoredVersion() {
    QSettings settings("HarmonyCast", "HarmonyCast");
    settings.remove("ignored_update_version");
}

} // namespace harmonycast
