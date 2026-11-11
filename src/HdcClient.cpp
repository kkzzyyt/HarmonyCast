#include "harmonycast/HdcClient.h"
#include <QProcess>
#include <QStandardPaths>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <QCoreApplication>
#include <charconv>
#include <sstream>
#include <iostream>
#include <QThreadPool>

namespace harmonycast {

HdcClient::HdcClient(std::string hdc_path, QObject* callbackContext)
    : m_hdc_path(std::move(hdc_path)),
      m_callback_context(callbackContext ? callbackContext : QCoreApplication::instance()) {
    if (m_hdc_path.empty()) {
        m_hdc_path = findHdc();
    }
}

std::string HdcClient::findHdc() {
    // 1. First priority: Check bundled hdc inside App Bundle Resources or adjacent resources directory
    QString appDir = QCoreApplication::applicationDirPath();
    std::vector<QString> bundled_candidates = {
        appDir + "/../Resources/hdc/hdc",
        appDir + "/../Resources/resources/hdc/hdc",
        appDir + "/../Resources/hdc",
        appDir + "/resources/hdc/hdc",
        appDir + "/hdc/hdc",
        appDir + "/hdc",
        QDir::currentPath() + "/resources/hdc/hdc",
        appDir + "/resources/hdc/hdc.exe",
        appDir + "/hdc/hdc.exe",
        appDir + "/hdc.exe",
        QDir::currentPath() + "/resources/hdc/hdc.exe",
        QDir::currentPath() + "/hdc.exe"
    };

    for (const auto& path : bundled_candidates) {
        if (QFileInfo::exists(path)) {
            QFileInfo fi(path);
            if (!fi.isExecutable()) {
                QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                            QFile::ReadGroup | QFile::ExeGroup |
                                            QFile::ReadOther | QFile::ExeOther);
            }
            if (QFileInfo(path).isExecutable()) {
                return QFileInfo(path).canonicalFilePath().toStdString();
            }
        }
    }

    // 2. Check system PATH
    QString in_path = QStandardPaths::findExecutable("hdc");
    if (!in_path.isEmpty()) {
        return in_path.toStdString();
    }

    // 3. Common DevEco Studio / OpenHarmony SDK paths
    std::vector<QString> candidates = {
        QDir::homePath() + "/Library/OpenHarmony/Sdk/toolchains/hdc",
        QDir::homePath() + "/Library/Huawei/Sdk/openharmony/toolchains/hdc",
        "/Applications/DevEco-Studio.app/Contents/tools/ohos-sdk/toolchains/hdc",
        "/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc",
        QDir::homePath() + "/AppData/Local/OpenHarmony/Sdk/toolchains/hdc.exe",
        QDir::homePath() + "/AppData/Local/Huawei/Sdk/openharmony/toolchains/hdc.exe",
        "C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe",
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

void HdcClient::configureProcess(QProcess& process, const std::string& hdc_path) {
    QFileInfo hdcFi(QString::fromStdString(hdc_path));
    if (hdcFi.exists() && hdcFi.isAbsolute()) {
        QString hdcDir = hdcFi.dir().absolutePath();
        process.setWorkingDirectory(hdcDir);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_WIN
        env.insert("PATH", hdcDir + ";" + env.value("PATH"));
#else
        env.insert("PATH", hdcDir + ":" + env.value("PATH"));
        env.insert("DYLD_LIBRARY_PATH", hdcDir + ":" + env.value("DYLD_LIBRARY_PATH"));
#endif
        process.setProcessEnvironment(env);
    }

#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(5, 7, 0)
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* pArgs) {
        pArgs->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#endif
}

CommandResult HdcClient::runProcess(const std::string& hdc_path,
                                    const std::vector<std::string>& args,
                                    const std::string& serial,
                                    int timeout_ms) {
    QProcess process;
    configureProcess(process, hdc_path);
    QStringList qargs;
    if (!serial.empty()) qargs << "-t" << QString::fromStdString(serial);
    for (const auto& arg : args) qargs << QString::fromStdString(arg);

    process.start(QString::fromStdString(hdc_path), qargs);
    if (!process.waitForStarted(2000)) {
        return {-1, "", "Failed to start HDC executable: " + hdc_path + ": " + process.errorString().toStdString()};
    }

    if (!process.waitForFinished(timeout_ms)) {
        process.kill();
        process.close();
        return {-2, "", "HDC command timed out after " + std::to_string(timeout_ms) + " ms"};
    }

    CommandResult res;
    res.exit_code = process.exitCode();
    res.out = QString::fromUtf8(process.readAllStandardOutput()).toStdString();
    res.err = QString::fromUtf8(process.readAllStandardError()).toStdString();
    if (process.exitStatus() != QProcess::NormalExit) {
        res.err += "\nHDC process terminated unexpectedly (exit=" + std::to_string(res.exit_code) + ")";
        res.exit_code = -3;
    }
    return res;
}

CommandResult HdcClient::run(const std::vector<std::string>& args,
                             const std::string& serial,
                             int timeout_ms) const {
    return runProcess(m_hdc_path, args, serial, timeout_ms);
}

std::vector<std::string> HdcClient::commandArguments(QString input, bool shellByDefault) {
    input = input.trimmed();
    if (input.isEmpty()) return {};
    const auto prefix = QRegularExpression(R"(^hdc(?:\.exe)?(?:\s+|$))", QRegularExpression::CaseInsensitiveOption).match(input);
    if (prefix.hasMatch()) {
        input = input.mid(prefix.capturedLength()).trimmed();
        shellByDefault = false;
    }
    const auto shell = QRegularExpression(R"(^shell(?:\s+|$))").match(input);
    if (shell.hasMatch()) {
        input = input.mid(shell.capturedLength()).trimmed();
        return input.isEmpty() ? std::vector<std::string>{} : std::vector<std::string>{"shell", input.toStdString()};
    }
    if (input.isEmpty()) return {};
    if (shellByDefault) return {"shell", input.toStdString()};
    std::vector<std::string> args;
    for (const auto& token : QProcess::splitCommand(input)) args.push_back(token.toStdString());
    return args;
}

std::vector<Device> HdcClient::parseDeviceList(const std::string& output) {
    std::vector<Device> devices;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line.empty() || line.front() == '[') continue;

        Device dev;
        std::istringstream fields(line);
        fields >> dev.serial;
        std::string state;
        bool has_transport = false;
        if (fields >> state) {
            if (state == "USB" || state == "TCP") {
                has_transport = true;
                dev.is_network = state == "TCP";
                if (!(fields >> state)) continue;
            }

            // Read the state column exactly: a device name may contain status words.
            if (state == "Connected") dev.state = DeviceState::Connected;
            else if (state == "Unauthorized") dev.state = DeviceState::Unauthorized;
            else if (state == "Offline" || state == "Ready" || state == "Unknown") dev.state = DeviceState::Offline;
            else continue;
        }

        // Verbose USB serials can themselves contain colons; honor the transport column.
        size_t colon = dev.serial.rfind(':');
        if ((!has_transport || dev.is_network) &&
            colon != std::string::npos && colon > 0 && colon < dev.serial.size() - 1) {
            std::string port_part = dev.serial.substr(colon + 1);
            int port = 0;
            auto [end, error] = std::from_chars(port_part.data(), port_part.data() + port_part.size(), port);
            if (error == std::errc{} && end == port_part.data() + port_part.size() && port > 0 && port <= 65535) {
                dev.port = port;
                dev.ip = dev.serial.substr(0, colon);
                dev.is_network = true;
            }
        }

        devices.push_back(std::move(dev));
    }
    return devices;
}

std::optional<std::vector<Device>> HdcClient::listDevicesChecked() const {
    // The terse list omits offline targets and cannot describe connection readiness.
    auto res = run({"list", "targets", "-v"});
    if (!res.success()) {
        return std::nullopt;
    }
    auto devices = parseDeviceList(res.out);
    if (devices.empty() && res.out.find("[Empty]") == std::string::npos &&
        res.out.find_first_not_of(" \t\r\n") != std::string::npos) {
        // Some HDC versions report command failures in stdout with exit code zero.
        return std::nullopt;
    }
    return devices;
}

std::vector<Device> HdcClient::listDevices() const {
    return listDevicesChecked().value_or(std::vector<Device>{});
}

bool HdcClient::forwardPort(const std::string& serial, int local_port, int remote_port) const {
    auto res = run({"fport", "tcp:" + std::to_string(local_port), "tcp:" + std::to_string(remote_port)}, serial);
    if (!res.success()) return false;

    // OpenHarmony hdc output variations: "Forwardport result:OK", "Forwardport result: OK", "result:OK", "success"
    std::string out = res.out;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out.find("ok") != std::string::npos || out.find("success") != std::string::npos;
}

bool HdcClient::removeForward(const std::string& serial, int local_port, int remote_port) const {
    auto res = run({"fport", "rm", "tcp:" + std::to_string(local_port) + " tcp:" + std::to_string(remote_port)}, serial, 1500);
    if (!res.success()) {
        res = run({"fport", "rm", "tcp:" + std::to_string(local_port), "tcp:" + std::to_string(remote_port)}, serial, 1500);
    }
    return res.success();
}

bool HdcClient::enableTcpMode(const std::string& serial, int port) const {
    auto res = run({"tmode", "port", std::to_string(port)}, serial, 8000);
    if (!res.success()) return false;
    std::string out = res.out;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out.find("success") != std::string::npos ||
           out.find("ok") != std::string::npos ||
           out.find("set device run mode") != std::string::npos;
}

bool HdcClient::restartServer() const {
    run({"kill"}, "", 3000);
    auto res = run({"start", "-r"}, "", 6000);
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

void HdcClient::wakeupAsync(const std::string& serial) const {
    executeAsync({"shell", "power-shell wakeup || uinput -K -d 18 -u 18"}, serial, nullptr);
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
    std::string hdc_path = m_hdc_path;
    static QThreadPool commands;
    static QThreadPool input;
    static const bool configured = [] {
        commands.setMaxThreadCount(4);
        input.setMaxThreadCount(1);
        commands.setExpiryTimeout(10000);
        input.setExpiryTimeout(10000);
        return true;
    }();
    (void)configured;
    const bool isInput = args.size() > 1 && args[0] == "shell" && args[1] == "uinput";
    auto context = m_callback_context;
    auto task = [hdc_path, args, serial, callback = std::move(callback), context, isInput]() {
        if (isInput && !context) return;
        CommandResult res = runProcess(hdc_path, args, serial, 10000);
        if (callback && context) {
            QMetaObject::invokeMethod(context.data(), [callback, res = std::move(res)]() {
                callback(res);
            });
        }
    };
    (isInput ? input : commands).start(std::move(task));
}

} // namespace harmonycast
