#include "harmonycast/WebViewDebugger.h"
#include <QRegularExpression>
#include <QDateTime>
#include <QEventLoop>
#include <iostream>

namespace harmonycast {

WebViewDebugger::WebViewDebugger(std::shared_ptr<HdcClient> hdc, QObject* parent)
    : QObject(parent),
      m_hdc(std::move(hdc)),
      m_http(new QNetworkAccessManager(this)),
      m_ws(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this)) {

    connect(m_ws, &QWebSocket::connected, this, &WebViewDebugger::onWsConnected);
    connect(m_ws, &QWebSocket::textMessageReceived, this, &WebViewDebugger::onWsTextMessage);
    connect(m_ws, &QWebSocket::disconnected, this, &WebViewDebugger::onWsDisconnected);
}

WebViewDebugger::~WebViewDebugger() {
    disconnectPage();
}

std::vector<WebViewInstance> WebViewDebugger::scanInstances(const std::string& serial) {
    std::vector<WebViewInstance> list;
    if (!m_hdc) return list;

    auto res = m_hdc->run({"shell", "cat /proc/net/unix 2>/dev/null"}, serial, 4000);
    if (!res.success()) return list;

    QRegularExpression re(R"(@?(webview_devtools_remote_(\d+)))");
    QString output = QString::fromStdString(res.out);
    auto it = re.globalMatch(output);

    std::map<int, WebViewInstance> unique_map;
    while (it.hasNext()) {
        auto m = it.next();
        std::string socket_name = m.captured(1).toStdString();
        int pid = m.captured(2).toInt();
        if (unique_map.find(pid) == unique_map.end()) {
            WebViewInstance inst;
            inst.pid = pid;
            inst.socket_name = socket_name;
            inst.target = "localabstract:" + socket_name;
            unique_map[pid] = inst;
            list.push_back(inst);
        }
    }
    return list;
}

bool WebViewDebugger::forwardInstance(const std::string& serial, const WebViewInstance& instance, int local_port) {
    if (!m_hdc) return false;
    m_active_serial = serial;
    m_forwarded_port = local_port;

    auto res = m_hdc->run({"fport", "tcp:" + std::to_string(local_port), instance.target}, serial, 4000);
    return res.success();
}

void WebViewDebugger::fetchPages(int port) {
    QUrl url(QString("http://127.0.0.1:%1/json/list").arg(port));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "HarmonyCast");
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    QNetworkReply* reply = m_http->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, port]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            // Try fallback endpoint /json
            QUrl fallback_url(QString("http://127.0.0.1:%1/json").arg(port));
            QNetworkReply* fb = m_http->get(QNetworkRequest(fallback_url));
            connect(fb, &QNetworkReply::finished, this, [this, fb]() {
                fb->deleteLater();
                if (fb->error() == QNetworkReply::NoError) {
                    QByteArray data = fb->readAll();
                    QJsonDocument doc = QJsonDocument::fromJson(data);
                    std::vector<WebViewPage> pages;
                    if (doc.isArray()) {
                        for (const auto& item : doc.array()) {
                            QJsonObject obj = item.toObject();
                            WebViewPage p;
                            p.id = obj["id"].toString().toStdString();
                            p.title = obj["title"].toString().toStdString();
                            p.url = obj["url"].toString().toStdString();
                            p.ws_url = obj["webSocketDebuggerUrl"].toString().toStdString();
                            p.devtools_frontend_url = obj["devtoolsFrontendUrl"].toString().toStdString();
                            pages.push_back(p);
                        }
                    }
                    emit pagesLoaded(pages);
                }
            });
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        std::vector<WebViewPage> pages;
        if (doc.isArray()) {
            for (const auto& item : doc.array()) {
                QJsonObject obj = item.toObject();
                WebViewPage p;
                p.id = obj["id"].toString().toStdString();
                p.title = obj["title"].toString().toStdString();
                p.url = obj["url"].toString().toStdString();
                p.ws_url = obj["webSocketDebuggerUrl"].toString().toStdString();
                p.devtools_frontend_url = obj["devtoolsFrontendUrl"].toString().toStdString();
                pages.push_back(p);
            }
        }
        emit pagesLoaded(pages);
    });
}

bool WebViewDebugger::connectPage(const WebViewPage& page) {
    disconnectPage();
    if (page.ws_url.empty()) return false;

    m_ws->open(QUrl(QString::fromStdString(page.ws_url)));
    return true;
}

void WebViewDebugger::disconnectPage() {
    if (m_ws && m_ws->isValid()) {
        m_ws->close();
    }
}

bool WebViewDebugger::isConnected() const {
    return m_ws && m_ws->isValid();
}

void WebViewDebugger::sendCdpCommand(const QString& method, const QJsonObject& params) {
    if (!isConnected()) return;

    QJsonObject cmd;
    cmd["id"] = m_msg_id++;
    cmd["method"] = method;
    if (!params.isEmpty()) {
        cmd["params"] = params;
    }

    QJsonDocument doc(cmd);
    m_ws->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

void WebViewDebugger::onWsConnected() {
    sendCdpCommand("Runtime.enable");
    sendCdpCommand("Network.enable");
    sendCdpCommand("Page.enable");
}

void WebViewDebugger::onWsDisconnected() {
    m_requests.clear();
}

void WebViewDebugger::onWsTextMessage(const QString& message) {
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) return;
    QJsonObject obj = doc.object();

    QString method = obj["method"].toString();
    QJsonObject params = obj["params"].toObject();

    // 1. Console API Call
    if (method == "Runtime.consoleAPICalled") {
        WebConsoleMessage msg;
        msg.timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz").toStdString();
        msg.level = params["type"].toString("log").toStdString();

        QJsonArray args = params["args"].toArray();
        QStringList str_args;
        for (const auto& a : args) {
            QJsonObject arg_obj = a.toObject();
            if (arg_obj.contains("value")) {
                str_args << arg_obj["value"].toVariant().toString();
            } else if (arg_obj.contains("description")) {
                str_args << arg_obj["description"].toString();
            }
        }
        msg.text = str_args.join(" ").toStdString();

        QJsonObject stack = params["stackTrace"].toObject();
        QJsonArray callFrames = stack["callFrames"].toArray();
        if (!callFrames.isEmpty()) {
            QJsonObject top = callFrames[0].toObject();
            msg.source_url = top["url"].toString().toStdString();
            msg.line_number = top["lineNumber"].toInt();
        }

        emit consoleMessageReceived(msg);
    }
    // 2. Network Request Sent
    else if (method == "Network.requestWillBeSent") {
        std::string req_id = params["requestId"].toString().toStdString();
        QJsonObject req = params["request"].toObject();

        WebNetworkRequest nr;
        nr.request_id = req_id;
        nr.url = req["url"].toString().toStdString();
        nr.method = req["method"].toString("GET").toStdString();
        nr.timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz").toStdString();
        nr.post_data = req["postData"].toString().toStdString();

        QJsonObject headers = req["headers"].toObject();
        for (auto k : headers.keys()) {
            nr.headers[k.toStdString()] = headers[k].toString().toStdString();
        }

        m_requests[req_id] = nr;
        emit networkRequestReceived(nr);
    }
    // 3. Network Response Received
    else if (method == "Network.responseReceived") {
        QString req_id = params["requestId"].toString();
        QJsonObject resp = params["response"].toObject();
        int status = resp["status"].toInt();
        QString mime = resp["mimeType"].toString();

        emit networkResponseReceived(req_id, status, mime);
    }
}

QString WebViewDebugger::generateCurl(const WebNetworkRequest& req) {
    QStringList parts;
    parts << "curl";
    if (req.method != "GET") {
        parts << "-X" << QString::fromStdString(req.method);
    }
    parts << QString("'%1'").arg(QString::fromStdString(req.url));

    for (const auto& [k, v] : req.headers) {
        parts << QString("-H '%1: %2'").arg(QString::fromStdString(k), QString::fromStdString(v));
    }

    if (!req.post_data.empty()) {
        parts << QString("--data-raw '%1'").arg(QString::fromStdString(req.post_data));
    }

    return parts.join(" \\\n  ");
}

} // namespace harmonycast
