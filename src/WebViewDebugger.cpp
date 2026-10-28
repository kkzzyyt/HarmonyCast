#include "harmonycast/WebViewDebugger.h"
#include <QRegularExpression>
#include <QDateTime>
#include <QEventLoop>
#include <QNetworkProxy>
#include <iostream>
#include <algorithm>
#include <QFile>
#include <QUuid>

namespace harmonycast {

WebViewDebugger::WebViewDebugger(std::shared_ptr<HdcClient> hdc, QObject* parent)
    : QObject(parent),
      m_hdc(std::move(hdc)),
      m_http(new QNetworkAccessManager(this)),
      m_ws(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this)) {

    // Ensure local 127.0.0.1 traffic bypasses any macOS system VPN or HTTP proxies
    m_http->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    m_ws->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));

    connect(m_ws, &QWebSocket::connected, this, &WebViewDebugger::onWsConnected);
    connect(m_ws, &QWebSocket::textMessageReceived, this, &WebViewDebugger::onWsTextMessage);
    connect(m_ws, &QWebSocket::disconnected, this, &WebViewDebugger::onWsDisconnected);
    m_callback_timer = new QTimer(this);
    m_callback_timer->setInterval(5000);
    connect(m_callback_timer, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        std::vector<std::function<void(const QJsonObject&)>> expired;
        for (auto it = m_callback_deadlines.begin(); it != m_callback_deadlines.end();) {
            if (it->second > now) { ++it; continue; }
            auto callback = m_callbacks.find(it->first);
            if (callback != m_callbacks.end()) {
                expired.push_back(std::move(callback->second));
                m_callbacks.erase(callback);
            }
            it = m_callback_deadlines.erase(it);
        }
        if (m_callbacks.empty()) m_callback_timer->stop();
        for (auto& callback : expired) callback({});
    });
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
    return forwardTarget(serial, instance.target, local_port);
}

bool WebViewDebugger::forwardTarget(const std::string& serial, const std::string& remote_target, int local_port) {
    if (!m_hdc) return false;
    m_active_serial = serial;
    m_forwarded_port = local_port;

    auto res = m_hdc->run({"fport", "tcp:" + std::to_string(local_port), remote_target}, serial, 4000);
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
                } else {
                    emit pagesLoaded({});
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

void WebViewDebugger::evaluateJs(const QString& js) {
    if (!isConnected()) return;
    QJsonObject params;
    params["expression"] = js;
    params["includeCommandLineAPI"] = true;
    params["returnByValue"] = true;
    sendCdpCommand("Runtime.evaluate", params);
}

void WebViewDebugger::sendCdpCommand(const QString& method, const QJsonObject& params, std::function<void(const QJsonObject&)> callback) {
    if (!isConnected()) {
        if (callback) callback({});
        return;
    }

    int id = m_msg_id++;
    if (callback) {
        m_callbacks[id] = callback;
        m_callback_deadlines[id] = QDateTime::currentMSecsSinceEpoch() + 30000;
        if (!m_callback_timer->isActive()) m_callback_timer->start();
    }

    QJsonObject cmd;
    cmd["id"] = id;
    cmd["method"] = method;
    if (!params.isEmpty()) {
        cmd["params"] = params;
    }

    QJsonDocument doc(cmd);
    m_ws->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

void WebViewDebugger::getResponseBody(const QString& requestId, std::function<void(const QString& body, bool base64)> callback) {
    std::string rid = requestId.toStdString();
    auto it = m_requests.find(rid);
    if (it != m_requests.end() && it->second.response_body_loaded) {
        if (!it->second.response_body_file.isEmpty()) {
            QFile file(it->second.response_body_file);
            if (file.open(QIODevice::ReadOnly)) {
                if (callback) callback(QString::fromUtf8(file.readAll()), it->second.response_body_base64);
                return;
            }
            it->second.response_body_loaded = false;
        } else {
            if (callback) callback(QString::fromStdString(it->second.response_body), it->second.response_body_base64);
            return;
        }
    }

    QJsonObject params;
    params["requestId"] = requestId;
    const uint64_t generation = m_cache_generation;
    sendCdpCommand("Network.getResponseBody", params, [this, rid, callback, generation](const QJsonObject& resp) {
        if (generation != m_cache_generation) return;
        QString body;
        bool base64 = false;
        if (resp.contains("result") && resp["result"].isObject()) {
            QJsonObject res = resp["result"].toObject();
            body = res["body"].toString();
            base64 = res["base64Encoded"].toBool();
            auto rit = m_requests.find(rid);
            if (rit != m_requests.end()) {
                // Preserve complete responses; spill large bodies to temporary files.
                std::string encoded = body.toStdString();
                m_body_cache_bytes -= rit->second.response_body.size();
                rit->second.response_body.clear();
                rit->second.response_body.shrink_to_fit();
                if (!rit->second.response_body_file.isEmpty()) QFile::remove(rit->second.response_body_file);
                rit->second.response_body_file.clear();
                rit->second.response_body_base64 = base64;
                if (encoded.size() <= 2 * 1024 * 1024) {
                    if (m_body_cache_bytes + encoded.size() > 16 * 1024 * 1024) {
                        for (auto& [id, request] : m_requests) {
                            if (!request.response_body_loaded || request.response_body.empty()) continue;
                            request.response_body_loaded = storeBodyOnDisk(request, request.response_body);
                            request.response_body.clear();
                            request.response_body.shrink_to_fit();
                        }
                        m_body_cache_bytes = 0;
                    }
                    rit->second.response_body = std::move(encoded);
                    m_body_cache_bytes += rit->second.response_body.size();
                    rit->second.response_body_loaded = true;
                } else {
                    rit->second.response_body_loaded = storeBodyOnDisk(rit->second, encoded);
                }
            }
        }
        if (callback) callback(body, base64);
    });
}

bool WebViewDebugger::storeBodyOnDisk(WebNetworkRequest& request, const std::string& body) {
    if (!m_response_files) m_response_files = std::make_unique<QTemporaryDir>();
    if (!m_response_files->isValid()) return false;
    if (!request.response_body_file.isEmpty()) QFile::remove(request.response_body_file);
    const QString path = m_response_files->filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".body");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(body.data(), qint64(body.size())) != qint64(body.size()) || !file.flush()) {
        file.remove();
        return false;
    }
    request.response_body_file = path;
    return true;
}

void WebViewDebugger::forgetRequest(const std::string& id) {
    const std::string key = id; // id may refer to m_request_order.front().
    auto request = m_requests.find(key);
    if (request == m_requests.end()) return;
    if (!request->second.response_body_file.isEmpty()) QFile::remove(request->second.response_body_file);
    m_body_cache_bytes -= request->second.response_body.size();
    m_requests.erase(request);
    if (!m_request_order.empty() && m_request_order.front() == key) {
        m_request_order.pop_front();
        return;
    }
    auto order = std::find(m_request_order.begin(), m_request_order.end(), key);
    if (order != m_request_order.end()) m_request_order.erase(order);
}

void WebViewDebugger::clearRequests() {
    ++m_cache_generation;
    m_requests.clear();
    m_request_order.clear();
    m_body_cache_bytes = 0;
    m_response_files.reset();
    m_callbacks.clear();
    m_callback_deadlines.clear();
    m_callback_timer->stop();
}

const WebNetworkRequest* WebViewDebugger::getRequest(const std::string& requestId) const {
    auto it = m_requests.find(requestId);
    if (it != m_requests.end()) return &it->second;
    return nullptr;
}

void WebViewDebugger::onWsConnected() {
    sendCdpCommand("Runtime.enable");
    sendCdpCommand("Console.enable");
    sendCdpCommand("Log.enable");
    sendCdpCommand("Network.enable");
    sendCdpCommand("Page.enable");
}

void WebViewDebugger::onWsDisconnected() {
    // Keep already captured responses available for offline inspection.
    // Explicit clear, request eviction and destruction release their storage.
    ++m_cache_generation;
    m_callbacks.clear();
    m_callback_deadlines.clear();
    m_callback_timer->stop();
}

void WebViewDebugger::onWsTextMessage(const QString& message) {
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) return;
    QJsonObject obj = doc.object();

    // Check callbacks for command IDs
    if (obj.contains("id")) {
        int id = obj["id"].toInt();
        auto it = m_callbacks.find(id);
        if (it != m_callbacks.end()) {
            auto cb = std::move(it->second);
            m_callbacks.erase(it);
            m_callback_deadlines.erase(id);
            if (m_callbacks.empty()) m_callback_timer->stop();
            if (cb) cb(obj);
        }
    }

    QString method = obj["method"].toString();
    QJsonObject params = obj["params"].toObject();

    // 0. Command Result (e.g. Runtime.evaluate)
    if (obj.contains("result") && obj["result"].isObject()) {
        QJsonObject res = obj["result"].toObject();
        if (res.contains("result")) {
            QJsonObject evalResult = res["result"].toObject();
            QString valStr = evalResult["value"].toVariant().toString();
            if (valStr.isEmpty() && evalResult.contains("description")) {
                valStr = evalResult["description"].toString();
            }
            if (!valStr.isEmpty()) {
                WebConsoleMessage msg;
                msg.timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz").toStdString();
                msg.level = "info";
                msg.text = "← " + valStr.toStdString();
                emit consoleMessageReceived(msg);
            }
        }
    }

    // 1. Console API Call (Runtime.consoleAPICalled)
    if (method == "Runtime.consoleAPICalled") {
        WebConsoleMessage msg;
        msg.timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz").toStdString();
        msg.level = params["type"].toString("log").toStdString();

        QJsonArray args = params["args"].toArray();
        QStringList str_args;
        for (const auto& a : args) {
            QJsonObject arg_obj = a.toObject();
            if (arg_obj.contains("value")) {
                QJsonValue v = arg_obj["value"];
                if (v.isObject()) {
                    str_args << QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
                } else if (v.isArray()) {
                    str_args << QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
                } else {
                    str_args << v.toVariant().toString();
                }
            } else if (arg_obj.contains("description")) {
                str_args << arg_obj["description"].toString();
            } else if (arg_obj["type"].toString() == "undefined") {
                str_args << "undefined";
            }
        }
        msg.text = str_args.join(" ").toStdString();

        QJsonObject stack = params["stackTrace"].toObject();
        QJsonArray callFrames = stack["callFrames"].toArray();
        if (!callFrames.isEmpty()) {
            QJsonObject top = callFrames[0].toObject();
            msg.source_url = top["url"].toString().toStdString();
            msg.line_number = top["lineNumber"].toInt() + 1;
        }

        emit consoleMessageReceived(msg);
    }
    // 2. Console Message Added (Console.messageAdded)
    else if (method == "Console.messageAdded") {
        QJsonObject mObj = params["message"].toObject();
        WebConsoleMessage msg;
        msg.timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz").toStdString();
        msg.level = mObj["level"].toString("log").toStdString();
        msg.text = mObj["text"].toString().toStdString();
        msg.source_url = mObj["url"].toString().toStdString();
        msg.line_number = mObj["line"].toInt();
        emit consoleMessageReceived(msg);
    }
    // 3. Log Entry Added (Log.entryAdded)
    else if (method == "Log.entryAdded") {
        QJsonObject entryObj = params["entry"].toObject();
        WebConsoleMessage msg;
        msg.timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz").toStdString();
        msg.level = entryObj["level"].toString("log").toStdString();
        msg.text = entryObj["text"].toString().toStdString();
        msg.source_url = entryObj["url"].toString().toStdString();
        msg.line_number = entryObj["lineNumber"].toInt();
        emit consoleMessageReceived(msg);
    }
    // 4. Uncaught JavaScript Exceptions (Runtime.exceptionThrown)
    else if (method == "Runtime.exceptionThrown") {
        QJsonObject details = params["exceptionDetails"].toObject();
        QString desc = details["text"].toString("Uncaught Error");
        if (details.contains("exception") && details["exception"].isObject()) {
            QJsonObject exc = details["exception"].toObject();
            if (exc.contains("description")) {
                desc = exc["description"].toString();
            } else if (exc.contains("value")) {
                desc = exc["value"].toVariant().toString();
            }
        }
        WebConsoleMessage msg;
        msg.timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz").toStdString();
        msg.level = "error";
        msg.text = desc.toStdString();

        QJsonObject stack = details["stackTrace"].toObject();
        QJsonArray callFrames = stack["callFrames"].toArray();
        if (!callFrames.isEmpty()) {
            QJsonObject top = callFrames[0].toObject();
            msg.source_url = top["url"].toString().toStdString();
            msg.line_number = top["lineNumber"].toInt() + 1;
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
        nr.start_time_ms = QDateTime::currentMSecsSinceEpoch();

        if (params.contains("type")) {
            nr.resource_type = params["type"].toString().toStdString();
        }

        QJsonObject headers = req["headers"].toObject();
        for (auto k : headers.keys()) {
            nr.headers[k.toStdString()] = headers[k].toString().toStdString();
        }

        forgetRequest(req_id);
        while (m_requests.size() >= 1000 && !m_request_order.empty()) forgetRequest(m_request_order.front());
        m_request_order.push_back(req_id);
        m_requests[req_id] = nr;
        emit networkRequestReceived(nr);
    }
    // 3. Network Response Received
    else if (method == "Network.responseReceived") {
        std::string req_id = params["requestId"].toString().toStdString();
        QJsonObject resp = params["response"].toObject();
        int status = resp["status"].toInt();
        std::string mime = resp["mimeType"].toString().toStdString();
        std::string statusText = resp["statusText"].toString().toStdString();
        std::string resType = params["type"].toString().toStdString();

        std::map<std::string, std::string> respHeaders;
        QJsonObject headersObj = resp["headers"].toObject();
        for (const auto& k : headersObj.keys()) {
            respHeaders[k.toStdString()] = headersObj[k].toString().toStdString();
        }

        auto it = m_requests.find(req_id);
        if (it != m_requests.end()) {
            it->second.status_code = status;
            it->second.status_text = statusText;
            it->second.mime_type = mime;
            if (!resType.empty()) it->second.resource_type = resType;
            it->second.response_headers = respHeaders;
            if (it->second.start_time_ms > 0) {
                it->second.elapsed_ms = QDateTime::currentMSecsSinceEpoch() - it->second.start_time_ms;
            }
        } else {
            // Do not resurrect evicted/cleared requests when a late response arrives.
            return;
        }

        emit networkResponseReceived(QString::fromStdString(req_id), status, QString::fromStdString(mime));
    }
    // 4. Network Loading Finished
    else if (method == "Network.loadingFinished") {
        std::string req_id = params["requestId"].toString().toStdString();
        auto it = m_requests.find(req_id);
        if (it != m_requests.end()) {
            if (it->second.elapsed_ms <= 0 && it->second.start_time_ms > 0) {
                it->second.elapsed_ms = QDateTime::currentMSecsSinceEpoch() - it->second.start_time_ms;
            }
        }
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
