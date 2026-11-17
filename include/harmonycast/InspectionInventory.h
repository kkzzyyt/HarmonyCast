#pragma once
#include "DeviceInspection.h"

namespace harmonycast {
class InspectionInventory {
public:
    static constexpr qint64 MaxFileBytes = 16 * 1024 * 1024;
    static InspectionImport read(const QString& path, DevicePlatform platform = DevicePlatform::OpenHarmony,
                                 QString* error = nullptr);
    static InspectionImport parseCsv(const QString& text, DevicePlatform platform = DevicePlatform::OpenHarmony);
};
} // namespace harmonycast
