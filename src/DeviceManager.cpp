#include "harmonycast/DeviceManager.h"
#include <algorithm>
#include <sstream>
#include <regex>
#include <thread>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>

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
        auto currentOpt = hdc->listDevicesChecked();

        if (weak_this) {
            QMetaObject::invokeMethod(weak_this.data(), [weak_this, currentOpt = std::move(currentOpt)]() {
                if (!weak_this) return;

                if (!currentOpt.has_value()) {
                    weak_this->m_consecutive_poll_failures++;
                    // Ignore transient HDC command latency or query timeouts (< 3 cycles) to prevent false disconnects
                    if (weak_this->m_consecutive_poll_failures < 3) {
                        weak_this->m_is_refreshing = false;
                        if (weak_this->m_needs_refresh.exchange(false)) {
                            weak_this->refresh(false);
                        }
                        return;
                    }
                } else {
                    weak_this->m_consecutive_poll_failures = 0;
                }

                auto current = currentOpt.value_or(std::vector<Device>{});

                std::vector<Device> connected_events;
                std::vector<Device> disconnected_events;
                bool has_changes = false;

                {
                    std::lock_guard<std::mutex> lock(weak_this->m_mutex);

                    // Check current devices against previous devices
                    for (auto& dev : current) {
                        auto it = std::find_if(weak_this->m_devices.begin(), weak_this->m_devices.end(),
                                               [&dev](const Device& old) { return isSameDevice(old.serial, dev.serial); });
                        // Verbose HDC lists Offline explicitly instead of omitting the target.
                        // Apply the same debounce as missing targets, but report lost authorization immediately.
                        if (it != weak_this->m_devices.end() && it->state == DeviceState::Connected &&
                            dev.state == DeviceState::Offline) {
                            int& misses = weak_this->m_miss_counts[it->serial];
                            const int threshold = it->is_network ? 3 : 2;
                            if (++misses < threshold) {
                                dev = *it;
                                continue;
                            }
                        } else {
                            weak_this->m_miss_counts[dev.serial] = 0;
                        }
                        if (it == weak_this->m_devices.end()) {
                            // Brand new device
                            if (dev.state == DeviceState::Connected) {
                                connected_events.push_back(dev);
                            }
                        } else {
                            // Existing device whose state transitioned to Connected
                            if (it->state != DeviceState::Connected && dev.state == DeviceState::Connected) {
                                connected_events.push_back(dev);
                            } else if (it->state == DeviceState::Connected && dev.state != DeviceState::Connected) {
                                // Transitioned from Connected to Offline / Unauthorized
                                disconnected_events.push_back(dev);
                            }
                        }
                    }

                    // Check for devices missing from current target list with debounce
                    std::vector<Device> preserved_missing;
                    for (const auto& old_dev : weak_this->m_devices) {
                        auto it = std::find_if(current.begin(), current.end(),
                                               [&old_dev](const Device& dev) { return isSameDevice(dev.serial, old_dev.serial); });
                        if (it == current.end()) {
                            int& misses = weak_this->m_miss_counts[old_dev.serial];
                            misses++;
                            // Network devices tolerate up to 3 missed poll cycles (~7.5s) to survive Wi-Fi jitter; USB devices tolerate 2 (~5s)
                            int threshold = old_dev.is_network ? 3 : 2;
                            if (misses < threshold) {
                                preserved_missing.push_back(old_dev);
                            } else {
                                if (old_dev.state == DeviceState::Connected) {
                                    disconnected_events.push_back(old_dev);
                                }
                                weak_this->m_miss_counts.erase(old_dev.serial);
                            }
                        }
                    }

                    for (const auto& preserved : preserved_missing) {
                        current.push_back(preserved);
                    }

                    has_changes = (weak_this->m_devices != current);
                    weak_this->m_devices = current;
                }

                // Direct Qt slots may read devices() or findDevice(). Never call
                // them while holding the non-recursive device-list mutex.
                const bool force = weak_this->m_needs_force_emit.exchange(false);
                for (const auto& d : connected_events) {
                    if (!weak_this) return;
                    emit weak_this->deviceConnected(d);
                }
                for (const auto& d : disconnected_events) {
                    if (!weak_this) return;
                    emit weak_this->deviceDisconnected(d);
                }
                if (!weak_this) return;
                if (force || has_changes) emit weak_this->devicesUpdated(current);

                if (!weak_this) return;
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
        "echo \"<<<ICU>>>\"; "
        "for dir in /sys/class/ICU/version /sys/class/icu/version /sys/class/ICU /sys/class/icu; do "
        "  if [ -e \"$dir\" ]; then "
        "    for f in \"$dir\"/*; do [ -f \"$f\" ] && echo \"$(basename \"$f\"): $(cat \"$f\" 2>/dev/null)\"; done; "
        "    break; "
        "  fi; "
        "done; "
        "for f in model Model Version version mcu_version MCU_Version HardwareVersion hardware_version StopWatchDog stopwatchdog StopDelay stopdelay power_delay FeedWatchdog feedwatchdog; do "
        "  [ -f /sys/class/ICU/version/$f ] && echo \"$f: $(cat /sys/class/ICU/version/$f 2>/dev/null)\"; "
        "done; "
        "echo \"<<<PKGS>>>\"; bm dump -a 2>/dev/null; "
        "echo \"<<<PS>>>\"; ps -A -o PID,NAME 2>/dev/null || ps 2>/dev/null; "
        "echo \"<<<BM_DETAILS>>>\"; for p in $(bm dump -a 2>/dev/null | grep -i yarward); do echo \"===BUNDLE:$p===\"; bm dump -n \"$p\" 2>/dev/null; done";

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
                } else {
                    std::string k_lower = k;
                    std::transform(k_lower.begin(), k_lower.end(), k_lower.begin(), ::tolower);
                    if (k_lower.find("mcu") != std::string::npos && (k_lower.find("version") != std::string::npos || k_lower.find("ver") != std::string::npos)) {
                        if (info.icu_version.empty()) {
                            info.icu_version = v;
                            info.icu_available = true;
                        }
                    }
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

        // Parse BM_DETAILS section: map bundle name -> raw dump output
        std::map<std::string, std::string> bundle_dumps;
        if (sections.count("BM_DETAILS")) {
            std::istringstream bd_stream(sections["BM_DETAILS"]);
            std::string bd_line;
            std::string current_bundle;
            std::string current_dump;
            while (std::getline(bd_stream, bd_line)) {
                if (!bd_line.empty() && bd_line.back() == '\r') {
                    bd_line.pop_back();
                }
                if (bd_line.rfind("===BUNDLE:", 0) == 0 && bd_line.size() > 13 && bd_line.substr(bd_line.size() - 3) == "===") {
                    if (!current_bundle.empty()) {
                        bundle_dumps[current_bundle] = std::move(current_dump);
                        current_dump.clear();
                    }
                    current_bundle = bd_line.substr(10, bd_line.size() - 13);
                } else {
                    current_dump += bd_line + "\n";
                }
            }
            if (!current_bundle.empty()) {
                bundle_dumps[current_bundle] = std::move(current_dump);
            }
        }

        // Yarward apps resolution
        for (const auto& pkg : info.all_packages) {
            std::string pkg_lower = pkg;
            for (auto& c : pkg_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (pkg_lower.find("yarward") != std::string::npos) {
                std::string raw_dump;
                if (bundle_dumps.count(pkg)) {
                    raw_dump = bundle_dumps[pkg];
                } else {
                    for (const auto& [b_name, b_dump] : bundle_dumps) {
                        if (isSameDevice(b_name, pkg) || b_name == pkg || b_name.find(pkg) != std::string::npos || pkg.find(b_name) != std::string::npos) {
                            raw_dump = b_dump;
                            break;
                        }
                    }
                }

                YarwardAppInfo app = parseBundleDetail(pkg, raw_dump);
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
        parseIcuInfo(info, sections["ICU"]);

        if (info.display.empty()) {
            info.display = "1280 × 800 (60 Hz)";
        }
        if (info.kernel.empty()) {
            info.kernel = "Linux 5.10 (OpenHarmony)";
        }
        if (!info.icu_model.empty() && (info.model.empty() || info.model == "OpenHarmony 智能终端")) {
            info.model = (info.icu_model.rfind("YH-", 0) == 0) ? info.icu_model : ("YH-" + info.icu_model);
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

YarwardAppInfo DeviceManager::parseBundleDetail(const std::string& bundle, const std::string& raw_dump) {
    YarwardAppInfo app;
    app.bundle = bundle;
    app.version_name = "未知";
    app.version_code = 0;
    app.target_api = 0;
    app.main_ability = "EntryAbility";
    app.install_time = "未知";
    app.update_time = "未知";

    if (raw_dump.empty()) return app;

    size_t json_start = raw_dump.find('{');
    size_t json_end = raw_dump.rfind('}');
    if (json_start != std::string::npos && json_end != std::string::npos && json_end > json_start) {
        std::string json_str = raw_dump.substr(json_start, json_end - json_start + 1);
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromRawData(json_str.data(), static_cast<int>(json_str.size())), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject obj = doc.object();
            QJsonObject appObj = obj.contains("appInfo") && obj["appInfo"].isObject() ? obj["appInfo"].toObject() : obj;

            // 1. Version Name
            QString vName;
            if (obj.contains("versionName")) vName = obj["versionName"].toString();
            else if (appObj.contains("versionName")) vName = appObj["versionName"].toString();
            else if (obj.contains("version_name")) vName = obj["version_name"].toString();

            if (!vName.isEmpty()) {
                app.version_name = vName.trimmed().toStdString();
            }

            // 2. Version Code
            int vCode = 0;
            auto parseCode = [](const QJsonValue& val) -> int {
                if (val.isDouble()) return val.toInt();
                if (val.isString()) return val.toString().toInt();
                return 0;
            };
            if (obj.contains("versionCode")) vCode = parseCode(obj["versionCode"]);
            else if (appObj.contains("versionCode")) vCode = parseCode(appObj["versionCode"]);
            else if (obj.contains("version_code")) vCode = parseCode(obj["version_code"]);
            app.version_code = vCode;

            // 3. Target API / Version
            int targetApi = 0;
            if (obj.contains("targetVersion")) targetApi = parseCode(obj["targetVersion"]);
            else if (appObj.contains("targetVersion")) targetApi = parseCode(appObj["targetVersion"]);
            else if (obj.contains("targetApi")) targetApi = parseCode(obj["targetApi"]);
            else if (obj.contains("compatibleVersion")) targetApi = parseCode(obj["compatibleVersion"]);
            app.target_api = targetApi;

            // 4. Main Ability
            std::string mainAbility;
            if (obj.contains("hapModuleInfos") && obj["hapModuleInfos"].isArray()) {
                for (const auto& item : obj["hapModuleInfos"].toArray()) {
                    if (item.isObject()) {
                        QJsonObject mObj = item.toObject();
                        if (mObj.contains("mainAbility") && !mObj["mainAbility"].toString().isEmpty()) {
                            mainAbility = mObj["mainAbility"].toString().toStdString();
                            break;
                        } else if (mObj.contains("mainElement") && !mObj["mainElement"].toString().isEmpty()) {
                            mainAbility = mObj["mainElement"].toString().toStdString();
                            break;
                        }
                    }
                }
            }
            if (mainAbility.empty() && obj.contains("mainAbility") && !obj["mainAbility"].toString().isEmpty()) {
                mainAbility = obj["mainAbility"].toString().toStdString();
            }
            if (!mainAbility.empty()) {
                app.main_ability = mainAbility;
            }

            // 5. Install & Update Time (millisecond timestamp)
            int64_t installMs = 0;
            int64_t updateMs = 0;
            if (obj.contains("userInfo") && obj["userInfo"].isArray()) {
                auto uArr = obj["userInfo"].toArray();
                if (!uArr.isEmpty() && uArr[0].isObject()) {
                    auto uObj = uArr[0].toObject();
                    installMs = uObj.value("installTime").toVariant().toLongLong();
                    updateMs = uObj.value("updateTime").toVariant().toLongLong();
                }
            }
            if (installMs == 0 && obj.contains("installTime")) {
                installMs = obj.value("installTime").toVariant().toLongLong();
            }
            if (updateMs == 0 && obj.contains("updateTime")) {
                updateMs = obj.value("updateTime").toVariant().toLongLong();
            }

            if (installMs > 0) {
                app.install_time = QDateTime::fromMSecsSinceEpoch(installMs).toString("yyyy-MM-dd HH:mm:ss").toStdString();
            }
            if (updateMs > 0) {
                app.update_time = QDateTime::fromMSecsSinceEpoch(updateMs).toString("yyyy-MM-dd HH:mm:ss").toStdString();
            }
        }
    }

    // Fallback: regex for text-based dump output or missed fields
    std::smatch m;
    if (app.version_name == "未知" && std::regex_search(raw_dump, m, std::regex(R"((?:versionName|version_name)["']?\s*[:=]\s*["']?([^"',\s\r\n}]+))", std::regex_constants::icase))) {
        app.version_name = m[1].str();
    }
    if (app.version_code == 0 && std::regex_search(raw_dump, m, std::regex(R"((?:versionCode|version_code)["']?\s*[:=]\s*["']?(\d+))", std::regex_constants::icase))) {
        try { app.version_code = std::stoi(m[1].str()); } catch (...) {}
    }
    if (app.target_api == 0 && std::regex_search(raw_dump, m, std::regex(R"((?:targetVersion|targetApi|compatibleVersion)["']?\s*[:=]\s*["']?(\d+))", std::regex_constants::icase))) {
        try { app.target_api = std::stoi(m[1].str()); } catch (...) {}
    }
    if (app.main_ability == "EntryAbility" && std::regex_search(raw_dump, m, std::regex(R"((?:mainAbility|mainElement)["']?\s*[:=]\s*["']?([^"',\s\r\n}]+))", std::regex_constants::icase))) {
        app.main_ability = m[1].str();
    }

    return app;
}

void DeviceManager::parseIcuInfo(DeviceInfo& info, const std::string& raw_icu) {
    if (raw_icu.empty()) return;
    std::istringstream icustream(raw_icu);
    std::string line;
    while (std::getline(icustream, line)) {
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon);
            std::string v = line.substr(colon + 1);
            k.erase(0, k.find_first_not_of(" \t\r\n"));
            k.erase(k.find_last_not_of(" \t\r\n") + 1);
            v.erase(0, v.find_first_not_of(" \t\r\n"));
            v.erase(v.find_last_not_of(" \t\r\n") + 1);

            if (v.empty() || v == "fail" || v == "not found") continue;
            if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) {
                v = v.substr(1, v.size() - 2);
                v.erase(0, v.find_first_not_of(" \t\r\n"));
                v.erase(v.find_last_not_of(" \t\r\n") + 1);
            }

            std::string k_lower = k;
            std::transform(k_lower.begin(), k_lower.end(), k_lower.begin(), ::tolower);

            if (k_lower == "model") {
                info.icu_available = true;
                info.icu_model = v;
            } else if (k_lower == "version" || k_lower == "mcu_version" || k_lower == "mcuversion" || k_lower == "mcu" || k_lower == "icu_version") {
                info.icu_available = true;
                info.icu_version = v;
            } else if (k_lower == "hardwareversion" || k_lower == "hardware_version" || k_lower == "hw_version") {
                if (info.hardware_version.empty() || info.hardware_version == "未知") {
                    info.hardware_version = v;
                }
            } else if (k_lower == "stopdelay" || k_lower == "power_delay" || k_lower == "stop_delay" || k_lower == "powerdelay") {
                info.power_delay = v;
            } else if (k_lower == "stopwatchdog" || k_lower == "stop_watchdog") {
                info.icu_available = true;
                info.watchdog_stopped = (v == "1");
                info.watchdog_desc = info.watchdog_stopped ? "已暂停 (调试防复位)" : "运行中 (超时复位)";
            }
        }
    }

    if (!info.icu_model.empty() || !info.icu_version.empty() || !info.power_delay.empty() || info.watchdog_stopped) {
        info.icu_available = true;
    }
}

} // namespace harmonycast
