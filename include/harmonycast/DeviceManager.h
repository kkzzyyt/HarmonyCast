#pragma once

#include "Types.h"
#include "HdcClient.h"
#include <QObject>
#include <QTimer>
#include <vector>
#include <mutex>
#include <memory>

namespace harmonycast {

class DeviceManager : public QObject {
    Q_OBJECT
public:
    explicit DeviceManager(std::shared_ptr<HdcClient> hdc, QObject* parent = nullptr);
    ~DeviceManager() override;

    void startPolling(int interval_ms = 1500);
    void stopPolling();

    [[nodiscard]] std::vector<Device> devices() const;
    [[nodiscard]] std::optional<Device> findDevice(const std::string& serial) const;
    void queryDeviceInfoAsync(const std::string& serial, std::function<void(DeviceInfo)> callback);

public slots:
    void refresh();

signals:
    void devicesUpdated(const std::vector<Device>& devices);
    void deviceConnected(const Device& device);
    void deviceDisconnected(const Device& device);

private:
    std::shared_ptr<HdcClient> m_hdc;
    QTimer* m_timer{nullptr};
    mutable std::mutex m_mutex;
    std::vector<Device> m_devices;
};

} // namespace harmonycast
