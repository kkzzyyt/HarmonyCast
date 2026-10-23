#pragma once

#include "Types.h"
#include <QObject>
#include <QTcpSocket>
#include <cstdint>
#include <string>

namespace harmonycast {

class ControlChannel : public QObject {
    Q_OBJECT
public:
    explicit ControlChannel(QObject* parent = nullptr);
    ~ControlChannel() override;

    bool connectToHost(const QString& host, quint16 port);
    void disconnectFromHost();
    [[nodiscard]] bool isConnected() const;

    // Direct binary touch injection (<2ms latency, bypasses shell uinput)
    bool sendTouch(TouchAction action, int x, int y, int screen_w, int screen_h, uint64_t pointer_id = 0);

    // Direct key injection (HOME, BACK, POWER, etc.)
    bool sendKey(KeyAction action, int keycode, uint32_t metastate = 0);

    // Text injection
    bool sendText(const std::string& text);

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString& error);

private:
    QTcpSocket* m_socket{nullptr};
};

} // namespace harmonycast
