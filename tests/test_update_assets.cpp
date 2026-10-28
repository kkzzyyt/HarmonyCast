#include "harmonycast/UpdateChecker.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <iostream>
#include <stdexcept>

static QString checkRelease(const QStringList& names) {
    QJsonArray assets;
    for (const QString& name : names) {
        assets.append(QJsonObject{{"name", name}, {"browser_download_url", "https://example.test/" + name}});
    }
    const QByteArray body = QJsonDocument(QJsonObject{
        {"tag_name", "v99.0.0"}, {"html_url", "https://example.test/release"}, {"assets", assets}
    }).toJson(QJsonDocument::Compact);

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) throw std::runtime_error("HTTP fixture failed to listen");
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
    try {
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
