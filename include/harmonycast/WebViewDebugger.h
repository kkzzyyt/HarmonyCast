#pragma once

#include "HdcClient.h"
#include <QObject>
#include <QWebSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <vector>
#include <memory>
#include <string>

namespace harmonycast {

struct WebViewInstance {
    int pid{0};
    std::string socket_name;
    std::string target;
};

struct WebViewPage {
    std::string id;
    std::string title;
    std::string url;
    std::string ws_url;
    std::string devtools_frontend_url;
};

struct WebConsoleMessage {
    std::string timestamp;
    std::string level; // "log", "warn", "error", "info", "debug"
    std::string text;
    std::string source_url;
    int line_number{0};
};

struct WebNetworkRequest {
    std::string request_id;
    std::string url;
    std::string method;
    int status_code{0};
    std::string mime_type;
    std::string timestamp;
    std::string post_data;
    std::map<std::string, std::string> headers;
};

class WebViewDebugger : public QObject {
    Q_OBJECT
public:
    explicit WebViewDebugger(std::shared_ptr<HdcClient> hdc, QObject* parent = nullptr);
    ~WebViewDebugger() override;

    // Scan /proc/net/unix on device for @webview_devtools_remote_<pid>
    std::vector<WebViewInstance> scanInstances(const std::string& serial);

    // Forward device webview socket to local port
    bool forwardInstance(const std::string& serial, const WebViewInstance& instance, int local_port = 9222);

    // Fetch inspectable pages from http://127.0.0.1:<port>/json/list
    void fetchPages(int port = 9222);

    // Connect to WebSocket debugger for a page
    bool connectPage(const WebViewPage& page);
    void disconnectPage();
    [[nodiscard]] bool isConnected() const;

    // Generate cURL command from request
    static QString generateCurl(const WebNetworkRequest& req);

signals:
    void pagesLoaded(const std::vector<WebViewPage>& pages);
    void consoleMessageReceived(const WebConsoleMessage& msg);
    void networkRequestReceived(const WebNetworkRequest& req);
    void networkResponseReceived(const QString& request_id, int status, const QString& mime);
    void errorOccurred(const QString& err);

private slots:
    void onWsConnected();
    void onWsTextMessage(const QString& message);
    void onWsDisconnected();

private:
    std::shared_ptr<HdcClient> m_hdc;
    QNetworkAccessManager* m_http{nullptr};
    QWebSocket* m_ws{nullptr};
    std::string m_active_serial;
    int m_forwarded_port{0};
    int m_msg_id{1};

    std::map<std::string, WebNetworkRequest> m_requests;

    void sendCdpCommand(const QString& method, const QJsonObject& params = {});
};

} // namespace harmonycast
