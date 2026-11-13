#pragma once

#include "DeviceClient.h"
#include <QTemporaryDir>
#include <atomic>

namespace harmonycast {

struct SnapshotResult {
    QImage image;
    QSize sourceSize;
    QString error;
    QString detail;
    QString route;
    bool permanent{false};
};

// Per-session capture negotiation. Owns only its temporary device/host files.
class SnapshotCapture {
public:
    SnapshotCapture(std::shared_ptr<DeviceClient> client, std::string serial, std::atomic<bool>& cancelled,
                    std::function<void(const QString&)> progress);
    ~SnapshotCapture();
    SnapshotResult next(QSize target = {});
    static QImage decodeImage(const QByteArray& data);
private:
    enum class Capture { Snapshot, UiTest };
    enum class Transport { File, Text };
    std::shared_ptr<DeviceClient> m_client;
    std::string m_serial;
    std::atomic<bool>& m_cancelled;
    std::function<void(const QString&)> m_progress;
    QTemporaryDir m_temporary;
    std::string m_remote;
    Capture m_capture{Capture::Snapshot};
    Transport m_transport{Transport::File};
    std::string m_encoder{"base64"};
    bool m_resize_supported{true};
    QSize m_source;
    SnapshotResult capture(Capture kind, QSize target);
    SnapshotResult receive(const std::string& path);
    SnapshotResult receiveFile(const std::string& path);
    SnapshotResult receiveText(const std::string& path);
};

} // namespace harmonycast
