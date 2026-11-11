#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QStringList>
#include <memory>

namespace harmonycast {

struct UpdateInfo {
    QString currentVersion{"2.3.3"};
    QString latestVersion;
    QString releaseTitle;
    QString changelog;
    QString downloadUrl;
    QString pageUrl;
    QString releaseDate;
    bool hasUpdate{false};
    bool mandatory{false};
};

class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject* parent = nullptr);
    ~UpdateChecker() override = default;

    // Trigger update check. If silent=true, no prompt is displayed when up-to-date or on network error.
    void checkForUpdates(bool silent = true);

    // SemVer comparison: returns > 0 if v1 > v2, < 0 if v1 < v2, 0 if equal
    static int compareVersions(const QString& v1, const QString& v2);

    [[nodiscard]] QString currentVersion() const { return m_current_version; }
    void setCurrentVersion(const QString& ver) { m_current_version = ver; }

    [[nodiscard]] QString updateUrl() const { return m_update_url; }
    void setUpdateUrl(const QString& url);
    static QString defaultUpdateUrl();
    static QString officialUpdateUrl();
    [[nodiscard]] QStringList updateSources() const;
    void setFallbackUrls(const QStringList& urls) { m_fallback_urls = urls; }

    // Ignore version management
    void ignoreVersion(const QString& version);
    [[nodiscard]] bool isVersionIgnored(const QString& version) const;
    void clearIgnoredVersion();

signals:
    void checkStarted();
    void updateAvailable(const UpdateInfo& info, bool silent);
    void noUpdateAvailable(bool silent);
    void checkFailed(const QString& error, bool silent);

private:
    QNetworkAccessManager* m_nam{nullptr};
    QPointer<QNetworkReply> m_current_reply;
    QString m_current_version{"2.3.3"};
    QString m_update_url{defaultUpdateUrl()};
    QStringList m_fallback_urls;
    QStringList m_pending_urls;
    QStringList m_failures;
    quint64 m_check_generation{0};
    bool m_silent_check{true};

    void requestNextSource(quint64 generation);
    void onReplyFinished(QNetworkReply* reply, quint64 generation);
    void sourceFailed(const QString& reason, quint64 generation);
    UpdateInfo parseManifest(const QByteArray& data) const;
};

} // namespace harmonycast
