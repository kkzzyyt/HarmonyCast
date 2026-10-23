#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <memory>
#include <string_view>
#include <QMetaType>

namespace harmonycast {

enum class DeviceState {
    Disconnected,
    Connected,
    Offline,
    Unauthorized
};

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

enum class StreamMode {
    Native = 0,
    OHScrcpy = 1,
    Compatibility = 2
};

enum class TouchAction : uint8_t {
    Down = 0,
    Move = 1,
    Up = 2
};

enum class KeyAction : uint8_t {
    Down = 0,
    Up = 1
};

struct Device {
    std::string serial;
    std::string model;
    std::string ip;
    int port{0};
    bool is_network{false};
    DeviceState state{DeviceState::Connected};

    bool operator==(const Device& other) const {
        return serial == other.serial;
    }
};

struct NetworkInterfaceInfo {
    std::string interface_name;
    std::string name;
    std::string mac;
    std::string ip;
    bool connected{false};
};

struct YarwardAppInfo {
    std::string bundle;
    std::string version_name;
    int version_code{0};
    int target_api{0};
    std::string main_ability;
    std::string update_time;
    std::string install_time;
    bool running{false};
    std::string pids;
};

struct DeviceInfo {
    std::string serial;
    std::string brand;
    std::string model;
    std::string model_code;
    std::string manufacturer;
    std::string hardware_version;
    std::string software_version;
    std::string os_fullname;
    std::string os_version;
    std::string api_version;
    std::string security_patch;
    std::string cpu_abi;
    std::string udid;
    std::string kernel;
    std::string uptime;
    std::string memory;
    std::string storage;
    std::string display;

    std::vector<NetworkInterfaceInfo> networks;
    std::vector<YarwardAppInfo> yarward_apps;
    std::vector<std::string> all_packages;

    bool icu_available{false};
    std::string icu_model;
    std::string icu_version;
    std::string watchdog_desc;
    bool watchdog_stopped{false};
    std::string power_delay;

    int64_t uptime_seconds{0};
    uint64_t mem_total_kb{0};
    uint64_t mem_free_kb{0};
    int display_width{0};
    int display_height{0};
    int display_rotation{0};
};

struct HiLogEntry {
    std::string timestamp;   // e.g. "11:24:45.913"
    LogLevel level{LogLevel::Info};
    int pid{0};
    int tid{0};
    std::string domain;      // e.g. "A06666"
    std::string tag;         // e.g. "MVP_RealClientProcessor"
    std::string message;     // message content
    std::string category;    // "app" or "system"
    std::string bundle;      // package name if known
    std::string raw;         // original line text

    [[nodiscard]] std::string levelString() const {
        switch (level) {
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO";
            case LogLevel::Warn:  return "WARN";
            case LogLevel::Error: return "ERROR";
            case LogLevel::Fatal: return "FATAL";
        }
        return "INFO";
    }

    [[nodiscard]] std::string format() const {
        std::string res;
        res.reserve(32 + tag.size() + message.size() + bundle.size());
        res += timestamp;
        res += "  ";
        std::string lvl = levelString();
        res += lvl;
        res.append(std::max(0, 5 - static_cast<int>(lvl.size())), ' ');
        if (!bundle.empty()) {
            res += "  [";
            res += bundle;
            res += "]";
        }
        if (!tag.empty()) {
            res += "  [";
            res += tag;
            res += "] ";
        } else {
            res += "  ";
        }
        res += message;
        return res;
    }
};

struct DecodedFrame {
    int width{0};
    int height{0};
    int stride{0};
    int format{0}; // e.g. AV_PIX_FMT_RGB24 or AV_PIX_FMT_VIDEOTOOLBOX
    int64_t pts{0};
    bool is_keyframe{false};
    void* hw_surface{nullptr}; // CVPixelBufferRef on macOS or ID3D11Texture2D on Win
    std::vector<uint8_t> data; // CPU RGB24 buffer fallback
};

} // namespace harmonycast

Q_DECLARE_METATYPE(harmonycast::DecodedFrame)
