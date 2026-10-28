#pragma once

#include "Types.h"
#include <mutex>
#include <optional>

namespace harmonycast {

// At most one displayed frame and one pending frame, regardless of UI load.
class FrameMailbox {
public:
    bool publish(const DecodedFrame& frame) {
        std::lock_guard lock(m_mutex);
        const bool notify = !m_latest.has_value();
        m_latest = frame;
        return notify;
    }

    std::optional<DecodedFrame> take() {
        std::lock_guard lock(m_mutex);
        auto latest = std::move(m_latest);
        m_latest.reset();
        return latest;
    }

    void clear() {
        std::lock_guard lock(m_mutex);
        m_latest.reset();
    }

private:
    std::mutex m_mutex;
    std::optional<DecodedFrame> m_latest;
};

} // namespace harmonycast
