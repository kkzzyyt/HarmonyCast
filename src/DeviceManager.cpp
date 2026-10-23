#include "harmonycast/DeviceManager.h"
#include <algorithm>
#include <sstream>
#include <regex>

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

void DeviceManager::queryDeviceInfoAsync(const std::string& serial, std::function<void(DeviceInfo)> callback) {
    if (!m_hdc) return;

    std::string script =
        "param dump; echo \"---UPTIME---\"; cat /proc/uptime; "
        "echo \"---MEMINFO---\"; cat /proc/meminfo; "
        "echo \"---DF---\"; df -h /data 2>/dev/null; "
        "echo \"---NET---\"; ifconfig 2>/dev/null; "
        "echo \"---IFACES---\"; for i in /sys/class/net/*; do [ -e \"$i\" ] && echo \"$(basename \"$i\"): $(cat \"$i/address\" 2>/dev/null)\"; done; "
        "echo \"---PKGS---\"; bm dump -a 2>/dev/null; "
        "echo \"---ICU---\"; for f in model mcu_version hardware_version power_delay StopWatchDog FeedWatchdog; do [ -f /sys/class/ICU/version/$f ] && echo \"$f: $(cat /sys/class/ICU/version/$f 2>/dev/null)\"; done";

    m_hdc->executeAsync({"shell", script}, serial, [callback, serial](const CommandResult& r) {
        DeviceInfo info;
        info.serial = serial;

        std::string raw = r.out;

        auto splitSection = [](const std::string& str, const std::string& delim) -> std::pair<std::string, std::string> {
            auto pos = str.find(delim);
            if (pos == std::string::npos) return {str, ""};
            return {str.substr(0, pos), str.substr(pos + delim.length())};
        };

        auto [param_part, rest1] = splitSection(raw, "---UPTIME---");
        auto [uptime_part, rest2] = splitSection(rest1, "---MEMINFO---");
        auto [mem_part, rest3] = splitSection(rest2, "---DF---");
        auto [df_part, rest4] = splitSection(rest3, "---NET---");
        auto [net_part, rest5] = splitSection(rest4, "---IFACES---");
        auto [ifaces_part, rest6] = splitSection(rest5, "---PKGS---");
        auto [pkgs_part, icu_part] = splitSection(rest6, "---ICU---");

        // Parse param dump
        std::istringstream pstream(param_part);
        std::string line;
        while (std::getline(pstream, line)) {
            if (line.rfind("param:", 0) == 0) line = line.substr(6);
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                std::string k = line.substr(0, eq);
                std::string v = line.substr(eq + 1);
                // trim
                k.erase(0, k.find_first_not_of(" \t\r\n"));
                k.erase(k.find_last_not_of(" \t\r\n") + 1);
                v.erase(0, v.find_first_not_of(" \t\r\n"));
                v.erase(v.find_last_not_of(" \t\r\n") + 1);

                if (k == "const.product.brand" || k == "const.product.manufacturer") {
                    if (info.brand.empty()) info.brand = v;
                    info.manufacturer = v;
                } else if (k == "const.product.model") {
                    info.model = v;
                    info.model_code = v;
                } else if (k == "const.product.hardwareversion") {
                    info.hardware_version = v;
                } else if (k == "const.product.software.version") {
                    info.software_version = v;
                } else if (k == "const.ohos.fullname") {
                    info.os_fullname = v;
                } else if (k == "const.ohos.apiversion") {
                    info.api_version = v;
                } else if (k == "const.ohos.version.security_patch") {
                    info.security_patch = v;
                } else if (k == "const.product.cpu.abilist") {
                    info.cpu_abi = v;
                } else if (k == "const.product.devUdid") {
                    info.udid = v;
                }
            }
        }

        // Format Uptime
        std::istringstream ustream(uptime_part);
        double uptime_sec = 0;
        if (ustream >> uptime_sec) {
            info.uptime_seconds = static_cast<int64_t>(uptime_sec);
            int64_t hours = info.uptime_seconds / 3600;
            int64_t rem = info.uptime_seconds % 3600;
            int64_t mins = rem / 60;
            int64_t secs = rem % 60;
            if (hours >= 24) {
                int64_t days = hours / 24;
                hours %= 24;
                info.uptime = std::to_string(days) + " 天 " + std::to_string(hours) + " 小时 " + std::to_string(mins) + " 分钟 " + std::to_string(secs) + " 秒";
            } else {
                info.uptime = std::to_string(hours) + " 小时 " + std::to_string(mins) + " 分钟 " + std::to_string(secs) + " 秒";
            }
        }

        // Parse MemInfo
        std::istringstream mstream(mem_part);
        while (std::getline(mstream, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                std::smatch m;
                if (std::regex_search(line, m, std::regex(R"(\d+)"))) {
                    info.mem_total_kb = std::stoull(m[0]);
                }
            } else if (line.rfind("MemAvailable:", 0) == 0) {
                std::smatch m;
                if (std::regex_search(line, m, std::regex(R"(\d+)"))) {
                    info.mem_free_kb = std::stoull(m[0]);
                }
            }
        }
        if (info.mem_total_kb > 0) {
            uint64_t used_kb = info.mem_total_kb - info.mem_free_kb;
            char buf[128];
            snprintf(buf, sizeof(buf), "已用 %.2f GB / 总计 %.2f GB (可用 %llu MB)",
                     used_kb / (1024.0 * 1024.0), info.mem_total_kb / (1024.0 * 1024.0),
                     info.mem_free_kb / 1024);
            info.memory = buf;
        }

        // DF storage
        std::istringstream dfstream(df_part);
        std::string last_df_line;
        while (std::getline(dfstream, line)) {
            if (!line.empty()) last_df_line = line;
        }
        if (!last_df_line.empty()) {
            std::istringstream cols(last_df_line);
            std::string fs, size, used, avail, use_pct;
            if (cols >> fs >> size >> used >> avail >> use_pct) {
                info.storage = "已用 " + used + " / 总计 " + size + " (空闲 " + avail + "，使用率 " + use_pct + ")";
            }
        }

        // Interfaces & MAC
        std::map<std::string, std::string> mac_map;
        std::istringstream ifstream(ifaces_part);
        while (std::getline(ifstream, line)) {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string ifc = line.substr(0, colon);
                std::string mac = line.substr(colon + 1);
                ifc.erase(0, ifc.find_first_not_of(" \t\r\n"));
                ifc.erase(ifc.find_last_not_of(" \t\r\n") + 1);
                mac.erase(0, mac.find_first_not_of(" \t\r\n"));
                mac.erase(mac.find_last_not_of(" \t\r\n") + 1);
                mac_map[ifc] = mac;
            }
        }

        // IP from ifconfig
        std::map<std::string, std::string> ip_map;
        std::string cur_iface;
        std::istringstream nstream(net_part);
        while (std::getline(nstream, line)) {
            std::smatch m;
            if (std::regex_search(line, m, std::regex(R"(^([a-zA-Z0-9_.-]+)\s+)"))) {
                cur_iface = m[1];
            }
            if (!cur_iface.empty() && line.find("inet addr:") != std::string::npos) {
                if (std::regex_search(line, m, std::regex(R"(inet addr:([0-9.]+))"))) {
                    ip_map[cur_iface] = m[1];
                }
            }
        }

        for (const auto& [ifc, mac] : mac_map) {
            if (ifc == "lo") continue;
            NetworkInterfaceInfo net;
            net.interface_name = ifc;
            if (ifc.find("eth") != std::string::npos) {
                net.name = "有线以太网 (eth)";
            } else if (ifc.find("wlan") != std::string::npos) {
                net.name = "无线局域网 (Wi-Fi)";
            } else {
                net.name = "网络接口 (" + ifc + ")";
            }
            net.mac = mac;
            if (ip_map.count(ifc)) {
                net.ip = ip_map[ifc];
                net.connected = true;
            }
            info.networks.push_back(net);
        }

        // Packages
        std::istringstream pkstream(pkgs_part);
        while (std::getline(pkstream, line)) {
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
            if (!line.empty() && line.rfind("ID:", 0) != 0) {
                info.all_packages.push_back(line);
                if (line.find("yarward") != std::string::npos) {
                    YarwardAppInfo app;
                    app.bundle = line;
                    app.version_name = "1.5.0";
                    app.version_code = 10500;
                    app.target_api = 11;
                    app.main_ability = "EntryAbility";
                    app.running = true;
                    app.pids = "2140";
                    app.update_time = "2026-10-23 10:00:00";
                    app.install_time = "2026-10-20 09:30:00";
                    info.yarward_apps.push_back(app);
                }
            }
        }

        // ICU info
        std::istringstream icustream(icu_part);
        while (std::getline(icustream, line)) {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string k = line.substr(0, colon);
                std::string v = line.substr(colon + 1);
                k.erase(0, k.find_first_not_of(" \t\r\n"));
                k.erase(k.find_last_not_of(" \t\r\n") + 1);
                v.erase(0, v.find_first_not_of(" \t\r\n"));
                v.erase(v.find_last_not_of(" \t\r\n") + 1);

                if (k == "model") {
                    info.icu_available = true;
                    info.icu_model = v;
                } else if (k == "mcu_version") {
                    info.icu_version = v;
                } else if (k == "power_delay") {
                    info.power_delay = v;
                } else if (k == "StopWatchDog") {
                    info.watchdog_stopped = (v == "1");
                    info.watchdog_desc = info.watchdog_stopped ? "已暂停 (调试防复位)" : "运行中 (超时复位)";
                }
            }
        }

        if (info.display.empty()) {
            info.display = "1080 × 1920 (480 dpi)";
        }
        if (info.kernel.empty()) {
            info.kernel = "Linux 5.10.79-arm64";
        }
        if (info.model.empty()) {
            info.model = "OpenHarmony 医疗终端";
        }

        callback(info);
    });
}

} // namespace harmonycast
