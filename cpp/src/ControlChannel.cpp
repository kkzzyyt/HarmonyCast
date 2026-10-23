#include "harmonycast/ControlChannel.h"
#include <QDataStream>

namespace harmonycast {

enum ControlMsgType : uint8_t {
    TYPE_INJECT_KEY_EVENT = 0,
    TYPE_INJECT_TEXT = 1,
    TYPE_INJECT_TOUCH_EVENT = 2,
    TYPE_INJECT_SCROLL_EVENT = 3,
    TYPE_BACK_OR_SCREEN_ON = 4,
    TYPE_EXPAND_NOTIFICATION_PANEL = 5,
    TYPE_COLLAPSE_NOTIFICATION_PANEL = 6,
    TYPE_GET_CLIPBOARD = 7,
    TYPE_SET_CLIPBOARD = 8,
    TYPE_SET_SCREEN_POWER_MODE = 9,
    TYPE_ROTATE_DEVICE = 10,
};

ControlChannel::ControlChannel(QObject* parent)
    : QObject(parent), m_socket(new QTcpSocket(this)) {
    connect(m_socket, &QTcpSocket::connected, this, &ControlChannel::connected);
    connect(m_socket, &QTcpSocket::disconnected, this, &ControlChannel::disconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit errorOccurred(m_socket->errorString());
    });
}

ControlChannel::~ControlChannel() {
    disconnectFromHost();
}

bool ControlChannel::connectToHost(const QString& host, quint16 port) {
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->disconnectFromHost();
    }
    m_socket->connectToHost(host, port);
    return m_socket->waitForConnected(3000);
}

void ControlChannel::disconnectFromHost() {
    if (m_socket && m_socket->isOpen()) {
        m_socket->disconnectFromHost();
    }
}

bool ControlChannel::isConnected() const {
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

bool ControlChannel::sendTouch(TouchAction action, int x, int y, int screen_w, int screen_h, uint64_t pointer_id) {
    if (!isConnected()) return false;

    // Binary packet layout:
    // [1B Type: 2] [1B Action] [8B PointerId] [4B X] [4B Y] [2B Width] [2B Height] [2B Pressure] [4B ActionBtn] [4B Buttons]
    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);

    stream << static_cast<quint8>(TYPE_INJECT_TOUCH_EVENT);
    stream << static_cast<quint8>(action);
    stream << static_cast<quint64>(pointer_id);
    stream << static_cast<quint32>(x);
    stream << static_cast<quint32>(y);
    stream << static_cast<quint16>(screen_w);
    stream << static_cast<quint16>(screen_h);
    stream << static_cast<quint16>(0xFFFF); // Pressure (1.0f)
    stream << static_cast<quint32>(1);      // AMOTION_EVENT_BUTTON_PRIMARY
    stream << static_cast<quint32>(1);      // Buttons

    qint64 written = m_socket->write(packet);
    m_socket->flush();
    return written == packet.size();
}

bool ControlChannel::sendKey(KeyAction action, int keycode, uint32_t metastate) {
    if (!isConnected()) return false;

    // Binary packet layout:
    // [1B Type: 0] [1B Action] [4B KeyCode] [4B Repeat: 0] [4B MetaState]
    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);

    stream << static_cast<quint8>(TYPE_INJECT_KEY_EVENT);
    stream << static_cast<quint8>(action);
    stream << static_cast<quint32>(keycode);
    stream << static_cast<quint32>(0);
    stream << static_cast<quint32>(metastate);

    qint64 written = m_socket->write(packet);
    m_socket->flush();
    return written == packet.size();
}

bool ControlChannel::sendText(const std::string& text) {
    if (!isConnected() || text.empty()) return false;

    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);

    QByteArray utf8 = QByteArray::fromStdString(text);
    stream << static_cast<quint8>(TYPE_INJECT_TEXT);
    stream << static_cast<quint32>(utf8.size());
    packet.append(utf8);

    qint64 written = m_socket->write(packet);
    m_socket->flush();
    return written == packet.size();
}

} // namespace harmonycast
