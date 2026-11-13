#include "harmonycast/AdbClient.h"
#include "harmonycast/HdcClient.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThreadPool>
#include <QBuffer>
#include <QImageReader>
#include <algorithm>
#include <map>
#include <sstream>
#include <cmath>
#include <limits>

namespace harmonycast {

AdbClient::AdbClient(std::string adbPath, QObject* context)
    : m_path(adbPath.empty() ? findAdb() : std::move(adbPath)),
      m_delivery(new QObject, [](QObject* receiver) { receiver->deleteLater(); }),
      m_callback_context(context ? context : QCoreApplication::instance()) {}

std::string AdbClient::findAdb() {
    const auto adbOverride = qEnvironmentVariable("HARMONYCAST_ADB");
    if (!adbOverride.isEmpty()) return adbOverride.toStdString();
    const auto app = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    const QString tool = "adb.exe", host = "windows-x64";
#elif defined(Q_OS_MACOS)
    const QString tool = "adb", host = "macos";
#else
    const QString tool = "adb", host = "linux-x64";
#endif
    const QStringList roots{app + "/../Resources", app + "/resources", QDir::currentPath() + "/resources"};
    for (const auto& root : roots) {
        for (const auto& candidate : {root + "/adb/" + host + "/" + tool, root + "/adb/" + tool}) {
            if (QFileInfo(candidate).isExecutable()) return QDir::toNativeSeparators(candidate).toStdString();
        }
    }
    const auto onPath = QStandardPaths::findExecutable(tool);
    if (!onPath.isEmpty()) return onPath.toStdString();
    const QStringList sdkRoots{qEnvironmentVariable("ANDROID_SDK_ROOT"), qEnvironmentVariable("ANDROID_HOME"),
        QDir::homePath() + "/Library/Android/sdk", qEnvironmentVariable("LOCALAPPDATA") + "/Android/Sdk",
        QDir::homePath() + "/Android/Sdk"};
    for (const auto& root : sdkRoots) {
        const auto candidate = root + "/platform-tools/" + tool;
        if (!root.isEmpty() && QFileInfo(candidate).isExecutable()) return QDir::toNativeSeparators(candidate).toStdString();
    }
    return tool.toStdString();
}

bool AdbClient::isAvailable() const {
    return QFileInfo(QString::fromStdString(m_path)).isExecutable() ||
           !QStandardPaths::findExecutable(QString::fromStdString(m_path)).isEmpty();
}

CommandResult AdbClient::runProcess(const std::string& path, const std::vector<std::string>& args,
                                  const std::string& serial, int timeout_ms) {
    QProcess process;
    HdcClient::configureProcess(process, path);
    QStringList arguments;
    const auto target = deviceSerial(serial);
    if (!target.empty()) arguments << "-s" << QString::fromStdString(target);
    for (const auto& arg : args) arguments << QString::fromStdString(arg);
    QElapsedTimer clock; clock.start();
    QByteArray output, errors;
    bool oversized = false;
    const auto collect = [&] {
        const auto out = process.readAllStandardOutput();
        const auto err = process.readAllStandardError();
        constexpr qsizetype limit = 64 * 1024 * 1024;
        if (out.size() + err.size() + output.size() + errors.size() > limit) {
            oversized = true; process.kill(); return;
        }
        output += out; errors += err;
    };
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &process, collect);
    QObject::connect(&process, &QProcess::readyReadStandardError, &process, collect);
    process.start(QString::fromStdString(path), arguments);
    if (!process.waitForStarted(std::min(3000, timeout_ms))) {
        return {-1, {}, "ADB 启动失败：" + process.errorString().toStdString()};
    }
    if (!process.waitForFinished(std::max(1, timeout_ms - int(clock.elapsed())))) {
        process.kill(); process.waitForFinished(1000);
        return {-2, {}, "ADB 命令超时"};
    }
    // exec-out carries binary PNG data; UTF-8 conversion would corrupt it.
    collect();
    if (oversized) return {-4, {}, "ADB 返回内容超过 64 MiB"};
    CommandResult result{process.exitCode(), output.toStdString(), errors.toStdString()};
    if (process.exitStatus() != QProcess::NormalExit) result.exit_code = -3;
    return result;
}

QImage AdbClient::screenshotImage(const CommandResult& result) {
    static constexpr char signature[] = "\x89PNG\r\n\x1a\n";
    static constexpr char end[] = "\0\0\0\0IEND\xae\x42\x60\x82";
    if (!result.success() || result.out.size() < 45 || result.out.size() > 32 * 1024 * 1024 ||
        result.out.compare(0, 8, signature, 8) != 0 ||
        result.out.compare(result.out.size() - 12, 12, end, 12) != 0) return {};
    QBuffer buffer;
    buffer.setData(QByteArray::fromRawData(result.out.data(), int(result.out.size())));
    if (!buffer.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&buffer, "PNG");
    const auto size = reader.size();
    if (size.width() <= 0 || size.height() <= 0 || size.width() > 16384 || size.height() > 16384 ||
        int64_t(size.width()) * size.height() > 64 * 1024 * 1024) return {};
    return reader.read();
}

CommandResult AdbClient::run(const std::vector<std::string>& args, const std::string& serial, int timeout_ms) const {
    return runProcess(m_path, args, serial, timeout_ms);
}

void AdbClient::executeAsync(const std::vector<std::string>& args, const std::string& serial,
                             std::function<void(CommandResult)> callback) const {
    static QThreadPool commands, input;
    static const bool configured = [] { commands.setMaxThreadCount(4); input.setMaxThreadCount(1); return true; }();
    (void)configured;
    const bool isInput = args.size() > 1 && args[0] == "shell" && args[1] == "input";
    auto task = [path = m_path, args, serial, callback = std::move(callback),
                 delivery = m_delivery, context = m_callback_context]() {
        auto result = runProcess(path, args, serial);
        if (callback) QMetaObject::invokeMethod(delivery.get(), [context, callback, result = std::move(result)] {
            if (context) callback(result);
        }, Qt::QueuedConnection);
    };
    (isInput ? input : commands).start(std::move(task));
}

std::vector<Device> AdbClient::parseDeviceList(const std::string& text) {
    std::vector<Device> result;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        Device device; std::string state;
        if (!(fields >> device.serial >> state)) continue;
        if (state == "device") device.state = DeviceState::Connected;
        else if (state == "offline") device.state = DeviceState::Offline;
        else if (state == "unauthorized") device.state = DeviceState::Unauthorized;
        else if (state == "no" && line.find("permissions") != std::string::npos) device.state = DeviceState::Unauthorized;
        else continue;
        device.platform = DevicePlatform::Android;
        std::string field;
        while (fields >> field) {
            if (field.rfind("model:", 0) == 0) {
                device.model = field.substr(6);
                std::replace(device.model.begin(), device.model.end(), '_', ' ');
            }
        }
        const auto serial = QString::fromStdString(device.serial);
        const auto colon = serial.lastIndexOf(':');
        bool valid = false;
        const int port = serial.mid(colon + 1).toInt(&valid);
        auto host = serial.left(colon);
        if (host.startsWith('[') && host.endsWith(']')) host = host.mid(1, host.size() - 2);
        if (colon > 0 && valid && port > 0 && port <= 65535 && !QHostAddress(host).isNull()) {
            device.is_network = true; device.ip = host.toStdString(); device.port = port;
        } else if (serial.contains("._adb-tls-connect._tcp")) device.is_network = true;
        result.push_back(std::move(device));
    }
    return result;
}

std::optional<std::vector<Device>> AdbClient::listDevicesChecked() const {
    if (!isAvailable()) return std::vector<Device>{};
    const auto result = run({"devices", "-l"}, "", 4000);
    if (!result.success() || result.out.find("List of devices attached") == std::string::npos) return std::nullopt;
    return parseDeviceList(result.out);
}

std::vector<std::string> AdbClient::commandArguments(QString input, bool shellByDefault) {
    input = input.trimmed();
    const auto prefix = QRegularExpression(R"(^adb(?:\.exe)?(?:\s+|$))", QRegularExpression::CaseInsensitiveOption).match(input);
    if (prefix.hasMatch()) { input = input.mid(prefix.capturedLength()).trimmed(); shellByDefault = false; }
    return HdcClient::commandArguments(input, shellByDefault);
}

bool AdbClient::forwardPort(const std::string& serial, int local, int remote) const {
    return run({"forward", "tcp:" + std::to_string(local), "tcp:" + std::to_string(remote)}, serial).success();
}
bool AdbClient::removeForward(const std::string& serial, int local, int) const {
    return run({"forward", "--remove", "tcp:" + std::to_string(local)}, serial, 2000).success();
}
bool AdbClient::enableTcpMode(const std::string& serial, int port) const { return run({"tcpip", std::to_string(port)}, serial).success(); }
bool AdbClient::restartServer() const { return run({"start-server"}).success(); }
bool AdbClient::fileSend(const std::string& serial, const std::string& local, const std::string& remote) const {
    return run({"push", local, remote}, serial, 30000).success();
}
bool AdbClient::fileRecv(const std::string& serial, const std::string& remote, const std::string& local) const {
    return run({"pull", remote, local}, serial, 30000).success();
}
bool AdbClient::wakeup(const std::string& serial) const { return run({"shell", "input", "keyevent", "224"}, serial, 3000).success(); }
void AdbClient::wakeupAsync(const std::string& serial) const { executeAsync({"shell", "input", "keyevent", "224"}, serial, nullptr); }
bool AdbClient::home(const std::string& serial) const { return run({"shell", "input", "keyevent", "3"}, serial).success(); }
bool AdbClient::back(const std::string& serial) const { return run({"shell", "input", "keyevent", "4"}, serial).success(); }

std::string AdbClient::deviceInfoScript() {
    return R"HC(echo '<<<PROP>>>'; getprop; echo '<<<UPTIME>>>'; cat /proc/uptime;
echo '<<<MEM>>>'; cat /proc/meminfo; echo '<<<DF>>>'; df -h /data;
echo '<<<NET>>>'; ip -o -4 addr show; echo '<<<MAC>>>';
for i in /sys/class/net/*; do echo "$(basename "$i") $(cat "$i/address" 2>/dev/null)"; done;
echo '<<<KERNEL>>>'; uname -a; echo '<<<DISPLAY>>>'; wm size; dumpsys input | grep SurfaceOrientation;
echo '<<<PKGS>>>'; pm list packages; echo '<<<PS>>>'; ps -A -o PID,NAME;
echo '<<<DETAILS>>>'; for p in $(pm list packages com.yarward | cut -d: -f2); do
echo "===PACKAGE:$p==="; dumpsys package "$p"; done)HC";
}

DeviceInfo AdbClient::parseDeviceInfo(const std::string& serial, const std::string& text) {
    DeviceInfo info; info.serial = serial;
    std::map<QString, QString> sections, props;
    QString tag;
    for (const auto& line : QString::fromStdString(text).split('\n')) {
        const auto trimmed = line.trimmed();
        if (trimmed.startsWith("<<<") && trimmed.endsWith(">>>")) tag = trimmed.mid(3, trimmed.size() - 6);
        else sections[tag] += line + '\n';
    }
    const QRegularExpression property(R"(^\[([^\]]+)\]: \[(.*)\]$)");
    for (const auto& line : sections["PROP"].split('\n')) {
        const auto match = property.match(line.trimmed());
        if (match.hasMatch()) props[match.captured(1)] = match.captured(2);
    }
    const auto prop = [&props](const char* name) { return props[QString::fromLatin1(name)].toStdString(); };
    info.model = prop("ro.product.model"); info.model_code = prop("ro.product.device");
    info.brand = prop("ro.product.brand"); info.manufacturer = prop("ro.product.manufacturer");
    info.os_version = prop("ro.build.version.release"); info.api_version = prop("ro.build.version.sdk");
    info.os_fullname = "Android " + info.os_version; info.software_version = prop("ro.build.display.id");
    info.hardware_version = prop("ro.hardware"); info.chip = prop("ro.board.platform");
    info.cpu_abi = prop("ro.product.cpu.abilist"); if (info.cpu_abi.empty()) info.cpu_abi = prop("ro.product.cpu.abi");
    info.security_patch = prop("ro.build.version.security_patch"); info.sn = prop("ro.serialno");
    if (info.sn.empty()) info.sn = prop("ro.boot.serialno");
    info.device_type = prop("ro.build.characteristics"); info.bootloader = prop("ro.bootloader");
    info.kernel = sections["KERNEL"].trimmed().toStdString();
    bool validUptime = false;
    const double uptime = sections["UPTIME"].trimmed().section(' ', 0, 0).toDouble(&validUptime);
    if (validUptime && std::isfinite(uptime) && uptime >= 0 && uptime < double(std::numeric_limits<int64_t>::max())) {
        info.uptime_seconds = int64_t(uptime);
        info.uptime = QString("%1 天 %2 小时 %3 分钟").arg(info.uptime_seconds / 86400)
            .arg(info.uptime_seconds / 3600 % 24).arg(info.uptime_seconds / 60 % 60).toStdString();
    }
    const auto number = [](const QString& source, const QString& pattern) {
        return QRegularExpression(pattern).match(source).captured(1).toULongLong();
    };
    info.mem_total_kb = number(sections["MEM"], R"(MemTotal:\s+(\d+))");
    info.mem_free_kb = number(sections["MEM"], R"(MemAvailable:\s+(\d+))");
    if (!info.mem_free_kb) info.mem_free_kb = number(sections["MEM"], R"(MemFree:\s+(\d+))");
    if (info.mem_total_kb) info.memory = QString("可用 %1 MB / 总计 %2 MB")
        .arg(info.mem_free_kb / 1024).arg(info.mem_total_kb / 1024).toStdString();
    for (const auto& line : sections["DF"].split('\n')) {
        const auto cols = line.simplified().split(' ');
        if (cols.size() >= 6 && cols.last() == "/data") info.storage =
            QString("已用 %1 / 总计 %2（空闲 %3）").arg(cols[2], cols[1], cols[3]).toStdString();
    }
    std::map<QString, QString> macs;
    for (const auto& line : sections["MAC"].split('\n')) {
        const auto cols = line.simplified().split(' ');
        if (cols.size() == 2) macs[cols[0]] = cols[1];
    }
    const QRegularExpression address(R"(^\d+:\s+(\S+)\s+inet\s+([\d.]+)/)");
    for (const auto& line : sections["NET"].split('\n')) {
        const auto match = address.match(line.trimmed());
        if (!match.hasMatch()) continue;
        NetworkInterfaceInfo net; const auto name = match.captured(1).section('@', 0, 0);
        net.interface_name = name.toStdString(); net.name = net.interface_name;
        net.ip = match.captured(2).toStdString(); net.mac = macs[name].toStdString(); net.connected = true;
        info.networks.push_back(std::move(net));
    }
    const auto dimensions = QRegularExpression(R"((?:Physical|Override) size:\s*(\d+)x(\d+))").globalMatch(sections["DISPLAY"]);
    auto sizes = dimensions;
    while (sizes.hasNext()) { const auto size = sizes.next(); info.display_width = size.captured(1).toInt(); info.display_height = size.captured(2).toInt(); }
    info.display_rotation = int(number(sections["DISPLAY"], R"(SurfaceOrientation:\s*(\d+))")) * 90;
    if (info.display_width && info.display_height) info.display = std::to_string(info.display_width) + "x" + std::to_string(info.display_height);
    for (const auto& line : sections["PKGS"].split('\n')) {
        const auto pkg = line.trimmed();
        if (!pkg.startsWith("package:")) continue;
        const auto name = pkg.mid(8).toStdString(); info.all_packages.push_back(name);
        if (name.rfind("com.yarward", 0) != 0) continue;
        YarwardAppInfo app; app.bundle = name;
        const auto details = sections["DETAILS"].section("===PACKAGE:" + pkg.mid(8) + "===", 1, 1).section("===PACKAGE:", 0, 0);
        app.version_name = QRegularExpression(R"(versionName=([^\s]+))").match(details).captured(1).toStdString();
        app.version_code = int(number(details, R"(versionCode=(\d+))"));
        app.target_api = int(number(details, R"(targetSdk=(\d+))"));
        for (const auto& process : sections["PS"].split('\n')) {
            const auto cols = process.simplified().split(' ');
            if (cols.size() == 2 && (cols[1] == pkg.mid(8) || cols[1].startsWith(pkg.mid(8) + ':'))) {
                app.running = true; if (!app.pids.empty()) app.pids += ","; app.pids += cols[0].toStdString();
            }
        }
        info.yarward_apps.push_back(std::move(app));
    }
    return info;
}

std::optional<DeviceInfo> AdbClient::getDeviceInfo(const std::string& serial) const {
    const auto result = run({"shell", deviceInfoScript()}, serial, 15000);
    if (!result.success()) return std::nullopt;
    return parseDeviceInfo(serial, result.out);
}

} // namespace harmonycast
