#include "harmonycast/DeviceManager.h"
#include <algorithm>
#include <sstream>
#include <regex>
#include <thread>
#include <QPointer>

namespace harmonycast {

DeviceManager::DeviceManager(std::shared_ptr<HdcClient> hdc, QObject* parent)
    : QObject(parent), m_hdc(std::move(hdc)), m_timer(new QTimer(this)) {
    connect(m_timer, &QTimer::timeout, this, [this]{ refresh(false); });
}

DeviceManager::~DeviceManager() {
    stopPolling();
}

void DeviceManager::startPolling(int interval_ms) {
    refresh(true);
    m_timer->start(interval_ms);
}

void DeviceManager::stopPolling() {
    if (m_timer->isActive()) {
        m_timer->stop();
    }
}

void DeviceManager::refresh(bool forceEmit) {
    if (!m_hdc) return;
    if (forceEmit) m_needs_force_emit = true;

    if (m_is_refreshing.exchange(true)) {
        m_needs_refresh = true;
        return;
    }

    QPointer<DeviceManager> weak_this(this);
    auto hdc = m_hdc;
    std::thread([weak_this, hdc]() {
        if (!hdc) return;
        auto current = hdc->listDevices();

        if (weak_this) {
            QMetaObject::invokeMethod(weak_this.data(), [weak_this, current = std::move(current)]() {
                if (!weak_this) return;

                std::vector<Device> added;
                std::vector<Device> removed;

                {
                    std::lock_guard<std::mutex> lock(weak_this->m_mutex);

                    // Find newly added devices
                    for (const auto& dev : current) {
                        auto it = std::find(weak_this->m_devices.begin(), weak_this->m_devices.end(), dev);
                        if (it == weak_this->m_devices.end()) {
                            added.push_back(dev);
                        }
                    }

                    // Find removed devices
                    for (const auto& old_dev : weak_this->m_devices) {
                        auto it = std::find(current.begin(), current.end(), old_dev);
                        if (it == current.end()) {
                            removed.push_back(old_dev);
                        }
                    }

                    weak_this->m_devices = current;
                }

                for (const auto& d : added) {
                    emit weak_this->deviceConnected(d);
                }
                for (const auto& d : removed) {
                    emit weak_this->deviceDisconnected(d);
                }

                bool force = weak_this->m_needs_force_emit.exchange(false);
                if (force || !added.empty() || !removed.empty()) {
                    emit weak_this->devicesUpdated(current);
                }

                weak_this->m_is_refreshing = false;
                if (weak_this->m_needs_refresh.exchange(false)) {
                    weak_this->refresh(false);
                }
            });
        }
    }).detach();
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
        "echo \"<<<PARAM>>>\"; param dump 2>/dev/null; "
        "echo \"const.product.devUdid=$(param get const.product.devUdid 2>/dev/null)\"; "
        "echo \"ohos.boot.sn=$(param get ohos.boot.sn 2>/dev/null)\"; "
        "echo \"<<<UPTIME>>>\"; cat /proc/uptime 2>/dev/null; "
        "echo \"<<<MEMINFO>>>\"; cat /proc/meminfo 2>/dev/null; "
        "echo \"<<<DF>>>\"; df -h /data 2>/dev/null; "
        "echo \"<<<NET>>>\"; ifconfig 2>/dev/null; "
        "echo \"<<<IFACES>>>\"; for i in /sys/class/net/*; do [ -e \"$i\" ] && echo \"$(basename \"$i\"): $(cat \"$i/address\" 2>/dev/null)\"; done; "
        "echo \"<<<UNAME>>>\"; uname -a 2>/dev/null; "
        "echo \"<<<DISP>>>\"; hidumper -s DisplayManagerService -a \"-d -a\" 2>/dev/null; cat /sys/class/graphics/fb0/virtual_size 2>/dev/null; "
        "echo \"<<<ICU>>>\"; for f in model mcu_version hardware_version power_delay StopWatchDog FeedWatchdog; do [ -f /sys/class/ICU/version/$f ] && echo \"$f: $(cat /sys/class/ICU/version/$f 2>/dev/null)\"; done; "
        "echo \"<<<PKGS>>>\"; bm dump -a 2>/dev/null; "
        "echo \"<<<PS>>>\"; ps -A -o PID,NAME 2>/dev/null || ps 2>/dev/null";

    m_hdc->executeAsync({"shell", script}, serial, [callback, serial](const CommandResult& r) {
        DeviceInfo info;
        info.serial = serial;

        // Parse section by section based on standalone "<<<TAG>>>" line markers
        std::map<std::string, std::string> sections;
        std::string current_tag;
        std::string current_content;

        std::istringstream raw_stream(r.out);
        std::string raw_line;
        while (std::getline(raw_stream, raw_line)) {
            if (!raw_line.empty() && raw_line.back() == '\r') {
                raw_line.pop_back();
            }
            if (raw_line.rfind("<<<", 0) == 0 && raw_line.length() >= 6 &&
                raw_line.substr(raw_line.length() - 3) == ">>>") {
                if (!current_tag.empty()) {
                    sections[current_tag] = std::move(current_content);
                    current_content.clear();
                }
                current_tag = raw_line.substr(3, raw_line.length() - 6);
            } else {
                current_content += raw_line + "\n";
            }
        }
        if (!current_tag.empty()) {
            sections[current_tag] = std::move(current_content);
        }

        // Parse param dump
        std::istringstream pstream(sections["PARAM"]);
        std::string line;
        while (std::getline(pstream, line)) {
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
            if (line.rfind("param:", 0) == 0) {
                line = line.substr(6);
                line.erase(0, line.find_first_not_of(" \t\r\n"));
            }
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                std::string k = line.substr(0, eq);
                std::string v = line.substr(eq + 1);
                k.erase(0, k.find_first_not_of(" \t\r\n"));
                k.erase(k.find_last_not_of(" \t\r\n") + 1);
                v.erase(0, v.find_first_not_of(" \t\r\n"));
                v.erase(v.find_last_not_of(" \t\r\n") + 1);

                if (v == "fail" || v == "not found") continue;

                if (k == "const.product.brand" || k == "const.product.manufacturer") {
                    if (info.brand.empty()) info.brand = v;
                    info.manufacturer = v;
                } else if (k == "const.product.model") {
                    info.model = v;
                    info.model_code = v;
                } else if (k == "const.software.model" && info.model.empty()) {
                    info.model = v;
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
                } else if (k == "const.product.cpu.abilist" || k == "const.product.cpu.abi") {
                    if (info.cpu_abi.empty()) info.cpu_abi = v;
                } else if (k == "ohos.boot.hardware") {
                    info.chip = v;
                } else if (k == "const.product.devicetype" || k == "const.build.characteristics") {
                    if (info.device_type.empty()) info.device_type = v;
                } else if (k == "const.product.bootloader.version") {
                    info.bootloader = v;
                } else if (k == "const.product.devUdid" || k == "const.product.udid" || k == "const.product.uuid") {
                    if (info.udid.empty()) info.udid = v;
                } else if (k == "ohos.boot.sn" || k == "const.product.sn" || k == "const.product.serial") {
                    if (info.sn.empty()) info.sn = v;
                }
            }
        }

        // Format Uptime
        std::istringstream ustream(sections["UPTIME"]);
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
        std::istringstream mstream(sections["MEMINFO"]);
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
        std::istringstream dfstream(sections["DF"]);
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
        std::istringstream ifstream(sections["IFACES"]);
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
        std::istringstream nstream(sections["NET"]);
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
        std::istringstream pkstream(sections["PKGS"]);
        while (std::getline(pkstream, line)) {
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
            if (!line.empty() && line.rfind("ID:", 0) != 0 && line != "bundles:") {
                info.all_packages.push_back(line);
            }
        }

        // Running processes from PS
        std::map<std::string, std::string> running_process_pids;
        std::istringstream ps_stream(sections["PS"]);
        std::string ps_line;
        while (std::getline(ps_stream, ps_line)) {
            std::istringstream ss(ps_line);
            std::string pid_str;
            if (ss >> pid_str) {
                std::string rest;
                std::getline(ss, rest);
                for (const auto& pkg : info.all_packages) {
                    if (rest.find(pkg) != std::string::npos) {
                        if (running_process_pids[pkg].empty()) {
                            running_process_pids[pkg] = pid_str;
                        } else if (running_process_pids[pkg].find(pid_str) == std::string::npos) {
                            running_process_pids[pkg] += ", " + pid_str;
                        }
                    }
                }
            }
        }

        // Yarward apps resolution
        for (const auto& pkg : info.all_packages) {
            if (pkg.find("yarward") != std::string::npos) {
                YarwardAppInfo app;
                app.bundle = pkg;
                app.version_name = "已安装";
                app.version_code = 10000;
                app.target_api = 10;
                app.main_ability = "EntryAbility";
                if (running_process_pids.count(pkg)) {
                    app.running = true;
                    app.pids = running_process_pids[pkg];
                } else {
                    app.running = false;
                    app.pids = "";
                }
                info.yarward_apps.push_back(app);
            }
        }

        // Linux Kernel from uname
        std::string uname_clean = sections["UNAME"];
        uname_clean.erase(0, uname_clean.find_first_not_of(" \t\r\n"));
        uname_clean.erase(uname_clean.find_last_not_of(" \t\r\n") + 1);
        if (!uname_clean.empty()) {
            info.kernel = uname_clean;
        }

        // Display info from hidumper & fb0
        std::istringstream dstream(sections["DISP"]);
        std::string dline;
        int disp_w = 0, disp_h = 0;
        int refresh_rate = 0;
        while (std::getline(dstream, dline)) {
            std::smatch m;
            if (std::regex_search(dline, m, std::regex(R"(\[\s*\d+\s+\d+\s+(\d+)\s+(\d+)\s*\])"))) {
                disp_w = std::stoi(m[1]);
                disp_h = std::stoi(m[2]);
            }
            if (std::regex_search(dline, m, std::regex(R"(^\s*\d+\s+\d+\s+(\d+)\s+[\d.]+)"))) {
                refresh_rate = std::stoi(m[1]);
            }
            if (disp_w == 0 && std::regex_search(dline, m, std::regex(R"(^(\d+),(\d+))"))) {
                disp_w = std::stoi(m[1]);
                disp_h = std::stoi(m[2]);
            }
        }
        if (disp_w > 0 && disp_h > 0) {
            info.display_width = disp_w;
            info.display_height = disp_h;
            if (refresh_rate > 0) {
                info.display = std::to_string(disp_w) + " × " + std::to_string(disp_h) + " (" + std::to_string(refresh_rate) + " Hz)";
            } else {
                info.display = std::to_string(disp_w) + " × " + std::to_string(disp_h);
            }
        }

        // ICU info
        std::istringstream icustream(sections["ICU"]);
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
            info.display = "1280 × 800 (60 Hz)";
        }
        if (info.kernel.empty()) {
            info.kernel = "Linux 5.10 (OpenHarmony)";
        }
        if (info.model.empty()) {
            info.model = "OpenHarmony 智能终端";
        }
        if (info.device_type.empty()) {
            info.device_type = "标准终端";
        } else if (info.device_type == "tablet") {
            info.device_type = "平板终端 (tablet)";
        }

        callback(info);
    });
}

} // namespace harmonycast
