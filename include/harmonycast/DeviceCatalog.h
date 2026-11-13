#pragma once

#include "Types.h"
#include <QString>
#include <vector>

namespace harmonycast {

struct SavedNetworkDevice {
    std::string ip;
    std::string name;
    int port{8710};
    int64_t updated_at{0};
    DevicePlatform platform{DevicePlatform::OpenHarmony};
};

struct DeviceListEntry {
    Device device;
    QString name;
    bool saved{false};

    [[nodiscard]] QString stateText() const;
    [[nodiscard]] QString label() const;
    [[nodiscard]] QString searchText() const;
};

// Both selectors use the same endpoint-based merge. Distinct TCP ports and USB
// serials containing colons retain their identity, transport and live state.
class DeviceCatalog {
public:
    static std::vector<DeviceListEntry> merge(const std::vector<Device>& liveDevices,
                                             const std::vector<SavedNetworkDevice>& savedDevices);
    static bool matchesSearchText(const QString& text, const QString& query);
};

} // namespace harmonycast
