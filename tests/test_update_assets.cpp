#include "harmonycast/UpdateChecker.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <QThread>
#include <QPointer>
#include <QTemporaryDir>
#include <QSettings>
#include <functional>
#include <iostream>
#include <stdexcept>

using harmonycast::UpdateChecker;
using harmonycast::UpdateInfo;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate> static void waitUntil(Predicate predicate, int timeout = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    check(predicate(), "update source test timed out");
}

struct HttpResponse {
    int status{200};
    QByteArray body;
    bool hold{false};
};

class HttpFixture : public QTcpServer {
public:
    QStringList paths;
    QList<QByteArray> requests;
    std::function<HttpResponse(const QString&)> respond;

    explicit HttpFixture(QObject* parent = nullptr) : QTcpServer(parent) {
        check(listen(QHostAddress(QStringLiteral("127.0.0.1")), 0), "HTTP source fixture could not listen");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                socket->setParent(this);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, request = QByteArray{}, replied = false]() mutable {
                    request += socket->readAll();
                    if (replied || !request.contains("\r\n\r\n")) return;
                    replied = true;
                    const QString path = QString::fromUtf8(request.split(' ').value(1));
                    paths << path;
                    requests << request;
                    const auto response = respond(path);
                    if (response.hold) return;
                    const QByteArray status = response.status == 200 ? "200 OK" : "503 Service Unavailable";
                    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
                                  QByteArray::number(response.body.size()) + "\r\nConnection: close\r\n\r\n" + response.body);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    QString url(const QString& path) const { return QString("http://127.0.0.1:%1%2").arg(serverPort()).arg(path); }
};

struct CheckSignals {
    int updates{0};
    int noUpdates{0};
    int failures{0};
    bool silent{true};
    UpdateInfo info;
    QString error;

    void observe(UpdateChecker& checker) {
        QObject::connect(&checker, &UpdateChecker::updateAvailable, &checker, [this](const UpdateInfo& value, bool isSilent) {
            ++updates; info = value; silent = isSilent;
        });
        QObject::connect(&checker, &UpdateChecker::noUpdateAvailable, &checker, [this](bool isSilent) {
            ++noUpdates; silent = isSilent;
        });
        QObject::connect(&checker, &UpdateChecker::checkFailed, &checker, [this](const QString& value, bool isSilent) {
            ++failures; error = value; silent = isSilent;
        });
    }
    int finished() const { return updates + noUpdates + failures; }
};

static QByteArray manifest(const QString& version = "99.0.0") {
    return QJsonDocument(QJsonObject{
        {"version", version}, {"title", "测试更新"}, {"download_url", "https://github.com/example/project/releases/download/v99/file.zip"}
    }).toJson(QJsonDocument::Compact);
}

static void testSourceConfiguration() {
    UpdateChecker checker;
    check(checker.updateUrl() == "https://gh-proxy.com/https://api.github.com/repos/kkzzyyt/HarmonyCast/releases/latest",
          "default update check does not use the tested API mirror");
    check(checker.updateSources() == QStringList{UpdateChecker::defaultUpdateUrl(), UpdateChecker::officialUpdateUrl()},
          "default mirror must fall back only to the official API");
    checker.setUpdateUrl(" https://updates.example.test/version.json ");
    check(checker.updateSources() == QStringList{"https://updates.example.test/version.json"},
          "custom update source acquired an unexpected public fallback");
    checker.setFallbackUrls({"https://updates.example.test/backup.json", "https://updates.example.test/version.json"});
    check(checker.updateSources().size() == 2, "explicit fallback sources are not deduplicated");
    checker.setUpdateUrl("https://new-updates.example.test/version.json");
    check(checker.updateSources().size() == 1, "changing source retains the old fallback configuration");
}

static void testFallback(int primaryStatus, const QByteArray& primaryBody) {
    HttpFixture server;
    server.respond = [&](const QString& path) {
        return path == "/primary" ? HttpResponse{primaryStatus, primaryBody} : HttpResponse{200, manifest()};
    };
    UpdateChecker checker;
    checker.setUpdateUrl(server.url("/primary"));
    checker.setFallbackUrls({server.url("/backup")});
    CheckSignals observed;
    observed.observe(checker);
    checker.checkForUpdates(false);
    waitUntil([&] { return observed.finished() == 1; });
    check(server.paths == QStringList{"/primary", "/backup"}, "failed primary did not retry the backup exactly once");
    check(observed.updates == 1 && observed.failures == 0 && !observed.silent, "fallback emits an intermediate failure or loses manual-check mode");
    check(observed.info.downloadUrl == "https://github.com/example/project/releases/download/v99/file.zip",
          "API proxy setting unexpectedly rewrites download URLs");
    check(server.requests.front().contains("Accept: application/vnd.github.v3+json, application/json"),
          "source request does not accept GitHub release JSON");
}

static void testNoImplicitCustomFallback() {
    HttpFixture server;
    server.respond = [](const QString&) { return HttpResponse{503, "{}"}; };
    UpdateChecker checker;
    checker.setUpdateUrl(server.url("/custom"));
    CheckSignals observed;
    observed.observe(checker);
    checker.checkForUpdates(false);
    waitUntil([&] { return observed.finished() == 1; });
    check(observed.failures == 1 && server.paths == QStringList{"/custom"}, "custom source failure silently contacts another service");
}

static void testAllSourcesFail() {
    HttpFixture server;
    server.respond = [](const QString&) { return HttpResponse{200, "{\"message\":\"rate limited\"}"}; };
    UpdateChecker checker;
    checker.setUpdateUrl(server.url("/one"));
    checker.setFallbackUrls({server.url("/two")});
    CheckSignals observed;
    observed.observe(checker);
    checker.checkForUpdates(true);
    waitUntil([&] { return observed.finished() == 1; });
    check(observed.failures == 1 && observed.silent && server.paths.size() == 2 && !observed.error.isEmpty(),
          "exhausted update sources do not emit one final failure");
}

static void testReplacementCancelsOldReply() {
    HttpFixture server;
    server.respond = [](const QString& path) { return path == "/old" ? HttpResponse{200, {}, true} : HttpResponse{200, manifest()}; };
    UpdateChecker checker;
    checker.setUpdateUrl(server.url("/old"));
    checker.setFallbackUrls({server.url("/unwanted-backup")});
    CheckSignals observed;
    observed.observe(checker);
    checker.checkForUpdates(true);
    waitUntil([&] { return server.paths.contains("/old"); });
    checker.setUpdateUrl(server.url("/new"));
    checker.checkForUpdates(false);
    waitUntil([&] { return observed.finished() == 1; });
    check(observed.updates == 1 && observed.failures == 0 && !observed.silent &&
          server.paths == QStringList{"/old", "/new"}, "cancelled reply affected the replacement check");
}

static void testTimeoutFallback() {
    HttpFixture server;
    server.respond = [](const QString& path) { return path == "/slow" ? HttpResponse{200, {}, true} : HttpResponse{200, manifest()}; };
    UpdateChecker checker;
    checker.setUpdateUrl(server.url("/slow"));
    checker.setFallbackUrls({server.url("/backup")});
    CheckSignals observed;
    observed.observe(checker);
    checker.checkForUpdates(false);
    waitUntil([&] { return observed.finished() == 1; }, 9000);
    check(observed.updates == 1 && server.paths == QStringList{"/slow", "/backup"}, "timed-out source did not fall back");
}

static QString checkRelease(const QStringList& names) {
    QJsonArray assets;
    for (const QString& name : names) {
        assets.append(QJsonObject{{"name", name}, {"browser_download_url", "https://example.test/" + name}});
    }
    const QByteArray body = QJsonDocument(QJsonObject{
        {"tag_name", "v99.0.0"}, {"html_url", "https://example.test/release"}, {"assets", assets}
    }).toJson(QJsonDocument::Compact);

    QTcpServer server;
    if (!server.listen(QHostAddress(QStringLiteral("127.0.0.1")), 0)) throw std::runtime_error("HTTP fixture failed to listen");
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto* socket = server.nextPendingConnection();
        socket->setParent(&server);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, body, request = QByteArray{}]() mutable {
            request += socket->readAll();
            if (!request.contains("\r\n\r\n")) return;
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                          QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });

    harmonycast::UpdateChecker checker;
    checker.setUpdateUrl(QString("http://127.0.0.1:%1/release").arg(server.serverPort()));
    QEventLoop loop;
    QString result;
    QObject::connect(&checker, &harmonycast::UpdateChecker::updateAvailable, &loop,
                     [&](const harmonycast::UpdateInfo& info, bool) { result = info.downloadUrl; loop.quit(); });
    QObject::connect(&checker, &harmonycast::UpdateChecker::checkFailed, &loop,
                     [&](const QString&, bool) { loop.quit(); });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    checker.checkForUpdates(false);
    loop.exec();
    if (result.isEmpty()) throw std::runtime_error("update check did not return a release");
    return result;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    try {
        testSourceConfiguration();
        testFallback(503, "{}");
        testFallback(200, "<html>proxy error</html>");
        testNoImplicitCustomFallback();
        testAllSourcesFail();
        testReplacementCancelsOldReply();
        testTimeoutFallback();
        const QString compat = "HarmonyCast-99.0.0-Windows-Server2012-x64.zip";
        const QString windows = "HarmonyCast-99.0.0-Windows-x64.zip";
        const QString mac = "HarmonyCast-99.0.0-macOS-arm64.dmg";
#if defined(HARMONYCAST_SERVER2012_COMPAT)
        const QString expected = compat;
#elif defined(Q_OS_WIN)
        const QString expected = windows;
#elif defined(Q_OS_MACOS)
        const QString expected = mac;
#else
        const QString expected;
#endif
        const QString expectedUrl = expected.isEmpty() ? "https://example.test/release" : "https://example.test/" + expected;
        for (const QStringList& order : {QStringList{compat, windows, mac}, QStringList{mac, windows, compat}}) {
            if (checkRelease(order) != expectedUrl) throw std::runtime_error("selected an incompatible update package");
        }
        QStringList wrongPlatforms{compat, windows, mac};
        wrongPlatforms.removeAll(expected);
        if (checkRelease(wrongPlatforms) != "https://example.test/release") {
            throw std::runtime_error("must use release page when no matching package exists");
        }
        if (checkRelease({}) != "https://example.test/release") throw std::runtime_error("empty release must use its page");
        std::cout << "Update asset selection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
