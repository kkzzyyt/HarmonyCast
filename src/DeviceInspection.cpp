#include "harmonycast/DeviceInspection.h"
#include "harmonycast/DeviceManager.h"
#include <QFileInfo>
#include <QDir>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace harmonycast {
namespace {

QString commandDetail(const CommandResult& result) {
    // Transport errors must stay visible even after a large partial snapshot.
    QString detail = QString::fromStdString(result.err + "\n" + result.out).trimmed();
    if (detail.isEmpty()) detail = QString("HDC 退出码 %1").arg(result.exit_code);
    return detail.left(1024);
}

bool commandFailed(const CommandResult& result) {
    static const QRegularExpression failure(
        R"(\[Fail\]|\[E[0-9a-f]+\]|unauthori[sz]ed|\bfailed\b|\berror\b|permission denied)",
        QRegularExpression::CaseInsensitiveOption);
    return !result.success() || failure.match(QString::fromStdString(result.out + "\n" + result.err)).hasMatch();
}

void appendNote(InspectionResult& result, const QString& note) {
    if (!result.notes.isEmpty()) result.notes += "；";
    result.notes += note;
}

QString cleanValue(const QString& value) {
    const QString first = value.trimmed().section('\n', 0, 0).trimmed();
    static const QRegularExpression invalid(R"(^(\[|error\b|fail\b|invalid\b|unknown\b|not found\b)|\b(get parameter.*fail|parameter.*not (found|exist))\b)",
                                             QRegularExpression::CaseInsensitiveOption);
    return invalid.match(first).hasMatch() ? QString{} : first;
}

QString amount(const std::optional<quint64>& value, double divisor) {
    return value ? QString::number(static_cast<double>(*value) / divisor, 'f', 2) : QString{};
}

QString memoryPercent(const InspectionResult& row) {
    if (!row.memoryTotalKiB || !row.memoryAvailableKiB || *row.memoryTotalKiB == 0 ||
        *row.memoryAvailableKiB > *row.memoryTotalKiB) return {};
    return QString::number(100.0 * (1.0 - static_cast<double>(*row.memoryAvailableKiB) / *row.memoryTotalKiB), 'f', 1);
}

QByteArray csvLine(const QStringList& values) {
    QStringList quoted;
    for (QString value : values) {
        value.remove(QChar(0));
        const QString trimmed = value.trimmed();
        // Device strings are untrusted spreadsheet cells, even inside quotes.
        if ((!trimmed.isEmpty() && QString("=+-@").contains(trimmed.front())) ||
            (!value.isEmpty() && QString("\t\r\n").contains(value.front()))) value.prepend('\'');
        value.replace('"', "\"\"");
        quoted << '"' + value + '"';
    }
    return (quoted.join(',') + "\r\n").toUtf8();
}

} // namespace

InspectionImport InspectionReport::parseTargets(const QString& text) {
    InspectionImport result;
    QSet<QString> seen;
    static const QRegularExpression targetPattern(R"(^((?:\d{1,3}\.){3}\d{1,3})(?::(\d{1,5}))?$)");
    const auto lines = text.split('\n');
    bool limitReported = false;
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        if (i == 0 && line.startsWith(QChar(0xfeff))) line.remove(0, 1);
        line = line.section('#', 0, 0).trimmed();
        if (line.isEmpty()) continue;
        const auto tokens = line.split(QRegularExpression("[\\s,;，；]+"), Qt::SkipEmptyParts);
        for (const auto& token : tokens) {
            const auto match = targetPattern.match(token);
            bool valid = match.hasMatch();
            const QString ip = match.captured(1);
            for (const auto& octet : ip.split('.')) {
                if (octet.isEmpty() || octet.toInt() > 255 || (octet.size() > 1 && octet.startsWith('0'))) valid = false;
            }
            const int port = match.captured(2).isEmpty() ? 8710 : match.captured(2).toInt();
            if (port <= 0 || port > 65535) valid = false;
            if (!valid) {
                result.errors << QString("第 %1 行：%2").arg(i + 1).arg(token.left(80));
                continue;
            }
            InspectionTarget target{ip, port};
            if (seen.contains(target.endpoint())) {
                ++result.duplicates;
            } else if (result.targets.size() < MaxTargets) {
                seen.insert(target.endpoint());
                result.targets.push_back(std::move(target));
            } else if (!limitReported) {
                result.errors << QString("单次最多导入 %1 台设备").arg(MaxTargets);
                limitReported = true;
            }
        }
    }
    return result;
}

QString InspectionReport::collectionScript() {
    // Fixed, read-only script: imported addresses are passed only as HDC arguments.
    return QStringLiteral(
        "echo '<<<HC_INSPECTION:BEGIN>>>'; "
        "echo '<<<HC_INSPECTION:ICU>>>'; "
        "for dir in /sys/class/ICU/version /sys/class/icu/version /sys/class/ICU /sys/class/icu; do "
        "for f in model Model MODEL; do if [ -r \"$dir/$f\" ]; then "
        "echo \"model: $(cat \"$dir/$f\" 2>/dev/null)\"; fi; done; done; "
        "echo '<<<HC_INSPECTION:SERIAL>>>'; param get ohos.boot.sn 2>/dev/null; "
        "echo '<<<HC_INSPECTION:SYSTEM>>>'; param get const.ohos.fullname 2>/dev/null; "
        "echo '<<<HC_INSPECTION:API>>>'; param get const.ohos.apiversion 2>/dev/null; "
        "echo '<<<HC_INSPECTION:UPTIME>>>'; cat /proc/uptime 2>/dev/null; "
        "echo '<<<HC_INSPECTION:MEMORY>>>'; cat /proc/meminfo 2>/dev/null; "
        "echo '<<<HC_INSPECTION:STORAGE>>>'; df -k /data 2>/dev/null; "
        "echo '<<<HC_INSPECTION:IP>>>'; ip -o -4 addr show 2>/dev/null; "
        "echo '<<<HC_INSPECTION:IFCONFIG>>>'; ifconfig 2>/dev/null; "
        "echo '<<<HC_INSPECTION:MAC>>>'; "
        "for i in /sys/class/net/*; do [ -r \"$i/address\" ] && "
        "echo \"$(basename \"$i\") $(cat \"$i/address\" 2>/dev/null)\"; done; "
        "echo '<<<HC_INSPECTION:ARCH>>>'; uname -m 2>/dev/null; "
        "echo '<<<HC_INSPECTION:KERNEL>>>'; uname -r 2>/dev/null; "
        "echo '<<<HC_INSPECTION:LOAD>>>'; cat /proc/loadavg 2>/dev/null; "
        "echo '<<<HC_INSPECTION:TIME>>>'; date '+%Y-%m-%d %H:%M:%S %z' 2>/dev/null; "
        "echo '<<<HC_INSPECTION:END>>>'");
}

InspectionResult InspectionReport::parseSnapshot(const InspectionTarget& target, const QString& output) {
    InspectionResult row;
    row.target = target;
    row.inspectedAt = QDateTime::currentDateTime();
    QMap<QString, QString> sections;
    QString current;
    bool began = false, ended = false;
    static const QRegularExpression marker(R"(^<<<HC_INSPECTION:([A-Z]+)>>>$)");
    for (QString line : output.split('\n')) {
        if (line.endsWith('\r')) line.chop(1);
        const auto match = marker.match(line);
        if (match.hasMatch()) {
            current = match.captured(1);
            if (current == "BEGIN") began = true;
            if (current == "END") ended = true;
        } else if (began && !ended) {
            sections[current] += line + '\n';
        }
    }
    if (!began) {
        row.status = InspectionStatus::Failed;
        row.notes = "读取失败：" + (output.trimmed().isEmpty() ? QStringLiteral("设备未返回巡检信息") : output.trimmed().left(1024));
        return row;
    }
    // The script orders version nodes before root nodes. Keep the first usable
    // hardware model when a firmware exposes several aliases for the same ICU.
    for (const auto& line : sections["ICU"].split('\n')) {
        DeviceInfo icu;
        DeviceManager::parseIcuInfo(icu, line.toStdString());
        row.model = cleanValue(QString::fromStdString(icu.icu_model));
        if (!row.model.isEmpty()) break;
    }
    row.serial = cleanValue(sections["SERIAL"]);
    row.systemVersion = cleanValue(sections["SYSTEM"]);
    row.apiVersion = cleanValue(sections["API"]);
    row.architecture = cleanValue(sections["ARCH"]);
    row.kernel = cleanValue(sections["KERNEL"]);
    row.deviceTime = cleanValue(sections["TIME"]);
    bool ok = false;
    double seconds = sections["UPTIME"].simplified().section(' ', 0, 0).toDouble(&ok);
    if (ok && std::isfinite(seconds) && seconds >= 0 && seconds < static_cast<double>(std::numeric_limits<qint64>::max()))
        row.uptimeSeconds = static_cast<qint64>(seconds);

    auto memory = [&](const QString& key) -> std::optional<quint64> {
        const auto match = QRegularExpression("(?m)^" + key + R"(:\s+(\d+)\s+kB\s*$)").match(sections["MEMORY"]);
        if (!match.hasMatch()) return {};
        const auto value = match.captured(1).toULongLong(&ok);
        return ok ? std::optional<quint64>(value) : std::nullopt;
    };
    row.memoryTotalKiB = memory("MemTotal");
    if (row.memoryTotalKiB && *row.memoryTotalKiB == 0) row.memoryTotalKiB.reset();
    row.memoryAvailableKiB = memory("MemAvailable");
    if (!row.memoryTotalKiB || (row.memoryAvailableKiB && *row.memoryAvailableKiB > *row.memoryTotalKiB))
        row.memoryAvailableKiB.reset();

    const auto storage = QRegularExpression(R"((\d+)\s+\d+\s+(\d+)\s+\S+\s+/data(?:\s|$))")
        .match(sections["STORAGE"]);
    if (storage.hasMatch()) {
        bool totalOk, availableOk;
        const auto total = storage.captured(1).toULongLong(&totalOk);
        const auto available = storage.captured(2).toULongLong(&availableOk);
        if (totalOk && availableOk && total > 0 && available <= total) {
            row.storageTotalKiB = total;
            row.storageAvailableKiB = available;
        }
    }
    const auto loads = sections["LOAD"].simplified().split(' ', Qt::SkipEmptyParts);
    if (loads.size() >= 3) {
        bool valid = true;
        for (int i = 0; i < 3; ++i) {
            const double load = loads[i].toDouble(&ok);
            valid = valid && ok && std::isfinite(load) && load >= 0;
        }
        if (valid) row.cpuLoad = loads.mid(0, 3).join(" / ");
    }

    QMap<QString, InspectionNetwork> networks;
    static const QRegularExpression macPattern(R"(^([^\s]+)\s+((?:[0-9a-f]{2}:){5}[0-9a-f]{2})$)", QRegularExpression::CaseInsensitiveOption);
    for (const auto& line : sections["MAC"].split('\n')) {
        auto match = macPattern.match(line.trimmed());
        if (match.hasMatch()) networks[match.captured(1)].mac = match.captured(2).toLower();
    }
    auto addIp = [&](const QString& name, const QString& ip) {
        if (name == "lo" || ip.startsWith("127.") || ip == "0.0.0.0") return;
        auto& net = networks[name];
        if (!net.ips.contains(ip)) net.ips << ip;
    };
    static const QRegularExpression ipPattern(R"(^\d+:\s+([^\s:@]+)(?:@[^\s:]+)?\s+.*?\binet\s+([\d.]+)(?:/|\s))");
    for (const auto& line : sections["IP"].split('\n')) {
        auto match = ipPattern.match(line);
        if (match.hasMatch()) addIp(match.captured(1), match.captured(2));
    }
    QString iface;
    static const QRegularExpression ifacePattern(R"(^([a-zA-Z0-9_.-]+)(?::\s|\s))");
    static const QRegularExpression inetPattern(R"(\binet\s+(?:addr:)?([\d.]+))");
    static const QRegularExpression etherPattern(R"((?:HWaddr|ether)\s+((?:[0-9a-f]{2}:){5}[0-9a-f]{2}))", QRegularExpression::CaseInsensitiveOption);
    for (const auto& line : sections["IFCONFIG"].split('\n')) {
        auto match = ifacePattern.match(line);
        if (match.hasMatch()) iface = match.captured(1);
        if (iface.isEmpty()) continue;
        match = inetPattern.match(line);
        if (match.hasMatch()) addIp(iface, match.captured(1));
        match = etherPattern.match(line);
        if (match.hasMatch()) networks[iface].mac = match.captured(1).toLower();
    }
    for (auto it = networks.begin(); it != networks.end(); ++it) {
        auto net = it.value();
        net.name = it.key();
        if (net.mac == "00:00:00:00:00:00") net.mac.clear();
        if (net.name != "lo" && (!net.ips.isEmpty() || !net.mac.isEmpty())) row.networks.push_back(std::move(net));
    }

    QStringList missing;
    if (!row.uptimeSeconds) missing << "运行时间";
    if (!row.memoryTotalKiB) missing << "总内存";
    if (!row.memoryAvailableKiB) missing << "可用内存";
    if (macs(row).isEmpty()) missing << "MAC 地址";
    if (!row.storageTotalKiB || !row.storageAvailableKiB) missing << "/data 容量";
    if (row.model.isEmpty()) missing << "看门狗硬件型号";
    if (row.systemVersion.isEmpty()) missing << "系统版本";
    if (row.architecture.isEmpty()) missing << "CPU 架构";
    if (row.cpuLoad.isEmpty()) missing << "CPU 负载";
    row.status = missing.isEmpty() && ended ? InspectionStatus::Succeeded : InspectionStatus::Partial;
    if (!missing.isEmpty()) appendNote(row, "未读取到：" + missing.join("、"));
    if (!ended) appendNote(row, "采集未完整返回");
    if (!row.uptimeSeconds && !row.memoryTotalKiB && row.networks.empty() && row.model.isEmpty()) {
        row.status = InspectionStatus::Failed;
        appendNote(row, "设备未返回有效信息");
    }
    return row;
}

QString InspectionReport::statusText(InspectionStatus status) {
    switch (status) {
    case InspectionStatus::Pending: return "待巡检";
    case InspectionStatus::Connecting: return "连接中";
    case InspectionStatus::Collecting: return "采集中";
    case InspectionStatus::Succeeded: return "完成";
    case InspectionStatus::Partial: return "部分完成";
    case InspectionStatus::Failed: return "失败";
    case InspectionStatus::Cancelled: return "已停止";
    }
    return {};
}

QString InspectionReport::uptime(const InspectionResult& row) {
    if (!row.uptimeSeconds) return {};
    const qint64 seconds = *row.uptimeSeconds;
    return QString("%1 天 %2 小时 %3 分 %4 秒").arg(seconds / 86400).arg(seconds / 3600 % 24)
        .arg(seconds / 60 % 60).arg(seconds % 60);
}

QString InspectionReport::ips(const InspectionResult& row) {
    QStringList values;
    for (const auto& net : row.networks) if (!net.ips.isEmpty()) values << net.name + ": " + net.ips.join(", ");
    return values.join("; ");
}

QString InspectionReport::macs(const InspectionResult& row) {
    QStringList values;
    for (const auto& net : row.networks) if (!net.mac.isEmpty()) values << net.name + ": " + net.mac;
    return values.join("; ");
}

QStringList InspectionReport::headers() {
    return {"目标 IP", "HDC 端口", "巡检结果", "运行时间", "运行秒数", "MAC 地址", "设备型号",
            "序列号", "系统版本", "API 版本", "CPU 架构", "CPU 负载（1/5/15 分钟）", "总内存（MiB）",
            "已用内存（MiB）", "可用内存（MiB）", "内存使用率（%）", "/data 总容量（GiB）",
            "/data 可用容量（GiB）", "内核版本", "设备时间", "巡检时间", "备注"};
}

QStringList InspectionReport::cells(const InspectionResult& row) {
    std::optional<quint64> used;
    if (row.memoryTotalKiB && row.memoryAvailableKiB && *row.memoryAvailableKiB <= *row.memoryTotalKiB)
        used = *row.memoryTotalKiB - *row.memoryAvailableKiB;
    return {row.target.ip, QString::number(row.target.port), statusText(row.status), uptime(row),
            row.uptimeSeconds ? QString::number(*row.uptimeSeconds) : QString{}, macs(row), row.model,
            row.serial, row.systemVersion, row.apiVersion, row.architecture, row.cpuLoad,
            amount(row.memoryTotalKiB, 1024), amount(used, 1024), amount(row.memoryAvailableKiB, 1024), memoryPercent(row),
            amount(row.storageTotalKiB, 1048576), amount(row.storageAvailableKiB, 1048576),
            row.kernel,
            row.deviceTime, row.inspectedAt.toString("yyyy-MM-dd HH:mm:ss"), row.notes};
}

bool InspectionReport::writeCsv(const QString& path, const std::vector<InspectionResult>& results, QString* error) {
    if (error) error->clear();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    auto write = [&](const QByteArray& bytes) { return file.write(bytes) == bytes.size(); };
    bool ok = write(QByteArray::fromHex("efbbbf")) && write(csvLine(headers()));
    for (const auto& row : results) if (ok) ok = write(csvLine(cells(row)));
    if (!ok || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

InspectionRunner::InspectionRunner(std::shared_ptr<HdcClient> hdc, QObject* parent, int commandTimeoutMs)
    : QObject(parent), m_hdc(std::move(hdc)), m_command_timeout_ms(std::max(100, commandTimeoutMs)) {}

InspectionRunner::~InspectionRunner() {
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
    }
    if (m_running && m_connection_attempted && m_index < static_cast<int>(m_results.size())) {
        HdcClient::runProcess(m_hdc_path, {"tconn", m_results[m_index].target.endpoint().toStdString(), "-remove"}, "", 2000);
    }
}

bool InspectionRunner::start(const std::vector<InspectionTarget>& targets) {
    if (m_running || !m_hdc || targets.empty() || targets.size() > InspectionReport::MaxTargets) return false;
    for (const auto& target : targets) {
        const auto parsed = InspectionReport::parseTargets(target.endpoint());
        if (!parsed.errors.isEmpty() || parsed.targets.size() != 1 || parsed.targets.front().endpoint() != target.endpoint()) return false;
    }
    m_results.clear();
    for (const auto& target : targets) {
        InspectionResult row;
        row.target = target;
        m_results.push_back(std::move(row));
    }
    m_hdc_path = m_hdc->path();
    m_index = 0;
    m_stopping = false;
    m_connection_attempted = false;
    m_disconnecting = false;
    m_running = true;
    emit runningChanged(true);
    emit progressChanged(0, static_cast<int>(m_results.size()));
    QTimer::singleShot(0, this, &InspectionRunner::nextTarget);
    return true;
}

void InspectionRunner::stop() {
    if (!m_running) return;
    m_stopping = true;
    emit activityChanged("正在停止，断开当前设备后结束");
    if (m_process && !m_disconnecting) m_process->kill();
}

void InspectionRunner::nextTarget() {
    if (m_stopping || m_index >= static_cast<int>(m_results.size())) {
        finishBatch();
        return;
    }
    auto& row = m_results[m_index];
    row.status = InspectionStatus::Connecting;
    row.inspectedAt = QDateTime::currentDateTime();
    m_connection_attempted = true;
    emit targetChanged(m_index);
    emit activityChanged("正在连接 " + row.target.endpoint());
    runCommand({"tconn", row.target.endpoint().toStdString()}, "", std::min(10000, m_command_timeout_ms), [this](CommandResult result) {
        auto& row = m_results[m_index];
        if (m_stopping || commandFailed(result)) {
            row.status = m_stopping ? InspectionStatus::Cancelled : InspectionStatus::Failed;
            row.notes = m_stopping ? "用户停止" : "连接失败：" + commandDetail(result);
            disconnectCurrent();
        } else {
            collectCurrent();
        }
    });
}

void InspectionRunner::collectCurrent() {
    auto& row = m_results[m_index];
    row.status = InspectionStatus::Collecting;
    emit targetChanged(m_index);
    emit activityChanged("正在读取 " + row.target.endpoint());
    runCommand({"shell", InspectionReport::collectionScript().toStdString()}, row.target.endpoint(), m_command_timeout_ms,
               [this](CommandResult result) {
        auto& row = m_results[m_index];
        if (m_stopping) {
            row.status = InspectionStatus::Cancelled;
            row.notes = "用户停止";
        } else {
            const auto inspectedAt = row.inspectedAt;
            row = InspectionReport::parseSnapshot(row.target, QString::fromStdString(result.out));
            row.inspectedAt = inspectedAt;
            const CommandResult transport{result.exit_code, {}, result.err};
            if (commandFailed(transport)) {
                // Keep any readable values, but expose transport errors and truncation.
                if (row.status != InspectionStatus::Failed) row.status = InspectionStatus::Partial;
                appendNote(row, "读取失败：" + commandDetail(result));
            }
        }
        disconnectCurrent();
    });
}

void InspectionRunner::disconnectCurrent() {
    m_disconnecting = true;
    emit activityChanged("正在断开 " + m_results[m_index].target.endpoint());
    runCommand({"tconn", m_results[m_index].target.endpoint().toStdString(), "-remove"}, "", std::min(5000, m_command_timeout_ms),
               [this](CommandResult result) {
        auto& row = m_results[m_index];
        if (commandFailed(result)) {
            appendNote(row, "断开失败：" + commandDetail(result));
            if (row.status == InspectionStatus::Succeeded) row.status = InspectionStatus::Partial;
        }
        m_connection_attempted = false;
        m_disconnecting = false;
        emit targetChanged(m_index);
        ++m_index;
        emit progressChanged(m_index, static_cast<int>(m_results.size()));
        QTimer::singleShot(0, this, &InspectionRunner::nextTarget);
    });
}

void InspectionRunner::finishBatch() {
    for (int i = m_index; i < static_cast<int>(m_results.size()); ++i) {
        m_results[i].status = InspectionStatus::Cancelled;
        m_results[i].notes = "用户停止，未执行";
        emit targetChanged(i);
    }
    m_running = false;
    emit runningChanged(false);
    emit finished(m_stopping);
}

void InspectionRunner::runCommand(const std::vector<std::string>& args, const QString& serial, int timeoutMs,
                                  std::function<void(CommandResult)> callback) {
    auto* process = new QProcess(this);
    m_process = process;
    m_callback = std::move(callback);
    m_output.clear();
    m_errors.clear();
    m_command_error.clear();
    QStringList arguments;
    if (!serial.isEmpty()) arguments << "-t" << serial;
    for (const auto& arg : args) arguments << QString::fromStdString(arg);
    HdcClient::configureProcess(*process, m_hdc_path);
    auto* timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, this, [this, process, timeoutMs] {
        if (m_process != process) return;
        m_command_error = QString("命令超时（%1 秒）").arg(timeoutMs / 1000.0, 0, 'g', 3);
        process->kill();
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] { readOutput(process); });
    connect(process, &QProcess::readyReadStandardError, this, [this, process] { readOutput(process); });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
        completeCommand(process, status == QProcess::NormalExit ? code : -1);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) completeCommand(process, -1, "无法启动 HDC：" + process->errorString());
    });
    timeout->start(timeoutMs);
    process->start(QString::fromStdString(m_hdc_path), arguments);
}

void InspectionRunner::readOutput(QProcess* process) {
    if (m_process != process) return;
    constexpr int limit = 2 * 1024 * 1024;
    const auto out = process->readAllStandardOutput();
    const auto err = process->readAllStandardError();
    if (m_output.size() + m_errors.size() + out.size() + err.size() > limit) {
        m_command_error = "HDC 返回内容超过 2 MiB";
        process->kill();
        return;
    }
    m_output += out;
    m_errors += err;
}

void InspectionRunner::completeCommand(QProcess* process, int exitCode, const QString& error) {
    if (m_process != process) return;
    readOutput(process);
    if (auto* timeout = process->findChild<QTimer*>()) timeout->stop();
    if (!error.isEmpty()) m_command_error = error;
    CommandResult result{m_command_error.isEmpty() ? exitCode : -1, QString::fromUtf8(m_output).toStdString(),
                         (QString::fromUtf8(m_errors) + "\n" + m_command_error).trimmed().toStdString()};
    m_process = nullptr;
    process->deleteLater();
    auto callback = std::exchange(m_callback, {});
    if (callback) callback(std::move(result));
}

} // namespace harmonycast
