#include "harmonycast/DeviceManager.h"
#include <algorithm>

namespace harmonycast {

DeviceManager::DeviceManager(std::shared_ptr<HdcClient> hdc, QObject* parent)
    : QObject(parent), m_hdc(std::move(hdc)), m_timer(new QTimer(this)) {
    connect(m_timer, &QTimer::timeout, this, &DeviceManager::refresh);
}

DeviceManager::~DeviceManager() {
    stopPolling();
}

void DeviceManager::startPolling(int interval_ms) {
    refresh();
    m_timer->start(interval_ms);
}

void DeviceManager::stopPolling() {
    if (m_timer->isActive()) {
        m_timer->stop();
    }
}

void DeviceManager::refresh() {
    if (!m_hdc) return;

    auto current = m_hdc->listDevices();

    std::vector<Device> added;
    std::vector<Device> removed;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Find newly added devices
        for (const auto& dev : current) {
            auto it = std::find(m_devices.begin(), m_devices.end(), dev);
            if (it == m_devices.end()) {
                added.push_back(dev);
            }
        }

        // Find removed devices
        for (const auto& old_dev : m_devices) {
            auto it = std::find(current.begin(), current.end(), old_dev);
            if (it == current.end()) {
                removed.push_back(old_dev);
            }
        }

        m_devices = current;
    }

    for (const auto& d : added) {
        emit deviceConnected(d);
    }
    for (const auto& d : removed) {
        emit deviceDisconnected(d);
    }
    if (!added.empty() || !removed.empty()) {
        emit devicesUpdated(current);
    }
}

std::vector<Device> DeviceManager::devices() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_devices;
}

std::optional<Device> DeviceManager::findDevice(const std::string& serial) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& dev : m_devices) {
        if (dev.serial == serial) return dev;
    }
    return std::nullopt;
}

} // namespace harmonycast
