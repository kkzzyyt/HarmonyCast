#include "harmonycast/HdcClient.h"
#include <QProcess>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <QCoreApplication>
#include <sstream>
#include <iostream>
#include <thread>

namespace harmonycast {

HdcClient::HdcClient(std::string hdc_path)
    : m_hdc_path(std::move(hdc_path)) {
    if (m_hdc_path.empty()) {
        m_hdc_path = findHdc();
    }
}

std::string HdcClient::findHdc() {
    // 1. Check PATH
    QString in_path = QStandardPaths::findExecutable("hdc");
    if (!in_path.isEmpty()) {
        return in_path.toStdString();
    }

    // 2. Common DevEco Studio / OpenHarmony SDK paths
    std::vector<QString> candidates = {
        QDir::homePath() + "/Library/OpenHarmony/Sdk/toolchains/hdc",
        QDir::homePath() + "/Library/Huawei/Sdk/openharmony/toolchains/hdc",
        "/Applications/DevEco-Studio.app/Contents/tools/ohos-sdk/toolchains/hdc",
        "/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc",
        "C:/Users/" + QDir::home().dirName() + "/AppData/Local/OpenHarmony/Sdk/toolchains/hdc.exe",
        "D:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe"
    };

    for (const auto& path : candidates) {
        if (QFileInfo::exists(path) && QFileInfo(path).isExecutable()) {
            return path.toStdString();
        }
    }

    return "hdc";
}

bool HdcClient::isAvailable() const {
    if (m_hdc_path.empty()) return false;
    QFileInfo fi(QString::fromStdString(m_hdc_path));
    if (fi.isAbsolute()) {
        return fi.exists() && fi.isExecutable();
    }
    return !QStandardPaths::findExecutable(QString::fromStdString(m_hdc_path)).isEmpty();
}

CommandResult HdcClient::run(const std::vector<std::string>& args,
                             const std::string& serial,
                             int timeout_ms) const {
    QProcess process;
    QStringList qargs;

    if (!serial.empty()) {
        qargs << "-t" << QString::fromStdString(serial);
    }
    for (const auto& a : args) {
        qargs << QString::fromStdString(a);
    }

    process.start(QString::fromStdString(m_hdc_path), qargs);
    if (!process.waitForStarted(2000)) {
        return {-1, "", "Failed to start HDC executable: " + m_hdc_path};
    }

    if (!process.waitForFinished(timeout_ms)) {
        process.kill();
        process.waitForFinished(500);
        return {-2, "", "HDC command timed out after " + std::to_string(timeout_ms) + " ms"};
    }

    CommandResult res;
    res.exit_code = process.exitCode();
    res.out = QString::fromUtf8(process.readAllStandardOutput()).toStdString();
    res.err = QString::fromUtf8(process.readAllStandardError()).toStdString();
    return res;
}

std::vector<Device> HdcClient::listDevices() const {
    std::vector<Device> devices;
    auto res = run({"list", "targets"});
    if (!res.success()) {
        return devices;
    }

    std::istringstream stream(res.out);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line.empty() || line.find("[Empty]") != std::string::npos) continue;

        Device dev;
        size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            dev.serial = line.substr(0, tab);
            std::string state_str = line.substr(tab + 1);
            if (state_str.find("Connected") != std::string::npos) dev.state = DeviceState::Connected;
            else if (state_str.find("Offline") != std::string::npos) dev.state = DeviceState::Offline;
            else if (state_str.find("Unauthorized") != std::string::npos) dev.state = DeviceState::Unauthorized;
        } else {
            size_t space = line.find(' ');
            dev.serial = (space != std::string::npos) ? line.substr(0, space) : line;
            dev.state = DeviceState::Connected;
        }

        // Check if network IP:port device
        size_t colon = dev.serial.rfind(':');
        if (colon != std::string::npos && colon > 0 && colon < dev.serial.size() - 1) {
            std::string port_part = dev.serial.substr(colon + 1);
            try {
                dev.port = std::stoi(port_part);
                dev.ip = dev.serial.substr(0, colon);
                dev.is_network = true;
            } catch (...) {}
        }

        devices.push_back(std::move(dev));
    }
    return devices;
}

bool HdcClient::forwardPort(const std::string& serial, int local_port, int remote_port) const {
    std::string fport_spec = "tcp:" + std::to_string(local_port) + " tcp:" + std::to_string(remote_port);
    auto res = run({"fport", "tcp:" + std::to_string(local_port), "tcp:" + std::to_string(remote_port)}, serial);
    return res.success() && res.out.find("Forwardport result:OK") != std::string::npos;
}

bool HdcClient::removeForward(const std::string& serial, int local_port, int remote_port) const {
    auto res = run({"fport", "rm", "tcp:" + std::to_string(local_port), "tcp:" + std::to_string(remote_port)}, serial);
    return res.success();
}

bool HdcClient::fileSend(const std::string& serial, const std::string& local_path, const std::string& remote_path) const {
    auto res = run({"file", "send", local_path, remote_path}, serial, 30000);
    return res.success();
}

bool HdcClient::fileRecv(const std::string& serial, const std::string& remote_path, const std::string& local_path) const {
    auto res = run({"file", "recv", remote_path, local_path}, serial, 30000);
    return res.success();
}

bool HdcClient::wakeup(const std::string& serial) const {
    auto res = run({"shell", "power-shell wakeup || uinput -K -d 18 -u 18"}, serial, 3000);
    return res.success();
}

bool HdcClient::home(const std::string& serial) const {
    std::string cmd = "aa start -b com.ohos.launcher -a com.ohos.launcher.MainAbility 2>/dev/null || uinput -K -d 1 -u 1";
    auto res = run({"shell", cmd}, serial, 3000);
    return res.success();
}

bool HdcClient::back(const std::string& serial) const {
    auto res = run({"shell", "uinput -K -d 2 -u 2"}, serial, 2000);
    return res.success();
}

std::optional<DeviceInfo> HdcClient::getDeviceInfo(const std::string& serial) const {
    DeviceInfo info;
    info.serial = serial;

    // Single compound shell script to query all device metrics in one round trip!
    std::string script =
        "param get const.product.model; echo '---SEP---'; "
        "param get const.product.software.version; echo '---SEP---'; "
        "cat /proc/uptime; echo '---SEP---'; "
        "cat /proc/meminfo; echo '---SEP---'; "
        "hidumper -s DisplayManagerService -a -a 2>/dev/null | grep -E 'displayId|width|height|rotation'";

    auto res = run({"shell", script}, serial, 5000);
    if (!res.success()) return std::nullopt;

    QString full = QString::fromStdString(res.out);
    QStringList parts = full.split("---SEP---");

    if (parts.size() >= 1) info.model = parts[0].trimmed().toStdString();
    if (parts.size() >= 2) info.os_version = parts[1].trimmed().toStdString();
    if (parts.size() >= 3) {
        QString uptime_str = parts[2].trimmed().section(' ', 0, 0);
        bool ok = false;
        double up = uptime_str.toDouble(&ok);
        if (ok) info.uptime_seconds = static_cast<int64_t>(up);
    }
    if (parts.size() >= 4) {
        QRegularExpression reMemTotal(R"(MemTotal:\s+(\d+)\s+kB)");
        QRegularExpression reMemFree(R"(MemAvailable:\s+(\d+)\s+kB)");
        auto mTotal = reMemTotal.match(parts[3]);
        if (mTotal.hasMatch()) info.mem_total_kb = mTotal.captured(1).toULongLong();
        auto mFree = reMemFree.match(parts[3]);
        if (mFree.hasMatch()) info.mem_free_kb = mFree.captured(1).toULongLong();
    }
    if (parts.size() >= 5) {
        QRegularExpression reW(R"(width[:\s]+(\d+))", QRegularExpression::CaseInsensitiveOption);
        QRegularExpression reH(R"(height[:\s]+(\d+))", QRegularExpression::CaseInsensitiveOption);
        QRegularExpression reR(R"(rotation[:\s]+(\d+))", QRegularExpression::CaseInsensitiveOption);
        auto mW = reW.match(parts[4]);
        if (mW.hasMatch()) info.display_width = mW.captured(1).toInt();
        auto mH = reH.match(parts[4]);
        if (mH.hasMatch()) info.display_height = mH.captured(1).toInt();
        auto mR = reR.match(parts[4]);
        if (mR.hasMatch()) info.display_rotation = mR.captured(1).toInt() * 90;
    }

    return info;
}

void HdcClient::executeAsync(const std::vector<std::string>& args,
                             const std::string& serial,
                             std::function<void(CommandResult)> callback) const {
    std::thread([this, args, serial, callback]() {
        CommandResult res = run(args, serial);
        if (callback) {
            QMetaObject::invokeMethod(QCoreApplication::instance(), [callback, res]() {
                callback(res);
            });
        }
    }).detach();
}

} // namespace harmonycast
