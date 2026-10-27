#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <memory>

namespace harmonycast {

struct UpdateInfo {
    QString currentVersion{"2.2.0"};
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
    void setUpdateUrl(const QString& url) { m_update_url = url; }

    // Ignore version management
    void ignoreVersion(const QString& version);
    [[nodiscard]] bool isVersionIgnored(const QString& version) const;
    void clearIgnoredVersion();

signals:
    void checkStarted();
    void updateAvailable(const UpdateInfo& info, bool silent);
    void noUpdateAvailable(bool silent);
    void checkFailed(const QString& error, bool silent);

private slots:
    void onReplyFinished();

private:
    QNetworkAccessManager* m_nam{nullptr};
    QNetworkReply* m_current_reply{nullptr};
    QString m_current_version{"2.2.0"};
    QString m_update_url{"https://api.github.com/repos/kkzzyyt/HarmonyCast/releases/latest"};
    bool m_silent_check{true};

    UpdateInfo parseManifest(const QByteArray& data) const;
};

} // namespace harmonycast
