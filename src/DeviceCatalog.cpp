#include "harmonycast/DeviceCatalog.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace harmonycast {
namespace {

QString savedEndpoint(const SavedNetworkDevice& saved) {
    const QString host = QString::fromStdString(saved.ip).trimmed();
    const int port = saved.port > 0 && saved.port <= 65535 ? saved.port : 8710;
    return host.isEmpty() ? QString{} : host + ":" + QString::number(port);
}

Device normalizeLive(Device device) {
    if (!device.is_network) return device;
    QString serial = QString::fromStdString(device.serial).trimmed();
    QString host = QString::fromStdString(device.ip).trimmed();
    int port = device.port;
    const auto colon = serial.lastIndexOf(':');
    bool validPort = false;
    const int serialPort = serial.mid(colon + 1).toInt(&validPort);
    if (colon > 0 && validPort && serialPort > 0 && serialPort <= 65535) {
        host = serial.left(colon);
        port = serialPort;
    } else if (host.isEmpty()) {
        host = serial;
    }
    if (port <= 0 || port > 65535) port = 8710;
    device.ip = host.toStdString();
    device.port = port;
    device.serial = (host + ":" + QString::number(port)).toStdString();
    return device;
}

} // namespace

QString DeviceListEntry::stateText() const {
    switch (device.state) {
    case DeviceState::Connected: return "已连接";
    case DeviceState::Unauthorized: return "未授权";
    case DeviceState::Offline: return "离线";
    case DeviceState::Disconnected: return "未连接";
    }
    return "未连接";
}

QString DeviceListEntry::label() const {
    const QString serial = QString::fromStdString(device.serial);
    QString value = device.is_network ? serial : "USB: " + serial;
    if (!name.isEmpty() && name != serial) value += " · " + name;
    return value + " · " + stateText();
}

QString DeviceListEntry::searchText() const {
    return QString("%1 %2 %3 %4 %5 %6")
        .arg(QString::fromStdString(device.serial), QString::fromStdString(device.ip), name,
             QString::fromStdString(device.model), stateText(),
             device.is_network ? QStringLiteral("TCP 网络 无线") : QStringLiteral("USB 有线"));
}

std::vector<DeviceListEntry> DeviceCatalog::merge(const std::vector<Device>& liveDevices,
                                                const std::vector<SavedNetworkDevice>& savedDevices) {
    QHash<QString, QString> savedNames;
    for (const auto& saved : savedDevices) {
        const QString endpoint = savedEndpoint(saved);
        if (!endpoint.isEmpty() && !savedNames.contains(endpoint)) savedNames.insert(endpoint, QString::fromStdString(saved.name));
    }
    std::vector<DeviceListEntry> entries;
    entries.reserve(liveDevices.size() + savedDevices.size());
    QSet<QString> seen;
    for (const auto& live : liveDevices) {
        auto device = normalizeLive(live);
        const QString endpoint = QString::fromStdString(device.serial);
        if (endpoint.isEmpty() || seen.contains(endpoint)) continue;
        seen.insert(endpoint);
        DeviceListEntry entry;
        entry.device = std::move(device);
        entry.saved = entry.device.is_network && savedNames.contains(endpoint);
        entry.name = entry.saved ? savedNames.value(endpoint) : QString{};
        if (entry.name.isEmpty()) entry.name = QString::fromStdString(entry.device.model);
        entries.push_back(std::move(entry));
    }
    for (const auto& saved : savedDevices) {
        const QString endpoint = savedEndpoint(saved);
        if (endpoint.isEmpty() || seen.contains(endpoint)) continue;
        seen.insert(endpoint);
        DeviceListEntry entry;
        entry.device.serial = endpoint.toStdString();
        entry.device.ip = QString::fromStdString(saved.ip).trimmed().toStdString();
        entry.device.port = saved.port > 0 && saved.port <= 65535 ? saved.port : 8710;
        entry.device.is_network = true;
        entry.device.state = DeviceState::Disconnected;
        entry.name = QString::fromStdString(saved.name);
        entry.saved = true;
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool DeviceCatalog::matchesSearchText(const QString& text, const QString& query) {
    const auto tokens = query.trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    for (const auto& token : tokens) if (!text.contains(token, Qt::CaseInsensitive)) return false;
    return true;
}

} // namespace harmonycast
