#include "harmonycast/SnapshotCapture.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>

namespace harmonycast {
namespace {
constexpr int maxImageBytes = 32 * 1024 * 1024;
QString detail(const CommandResult& result) {
    return QString::fromStdString(result.err + "\n" + result.out).trimmed().left(2000);
}
bool failed(const CommandResult& result) {
    static const QRegularExpression errors(R"(\[Fail\]|\[E[0-9a-f]+\]|\bfailed\b|\berror\b|permission denied)",
        QRegularExpression::CaseInsensitiveOption);
    return !result.success() || errors.match(detail(result)).hasMatch();
}
bool unavailable(const QString& text) {
    static const QRegularExpression errors(R"(not found|inaccessible|no such file or directory|permission denied|access denied|not permitted|permission denial)",
        QRegularExpression::CaseInsensitiveOption);
    return errors.match(text).hasMatch();
}
}

SnapshotCapture::SnapshotCapture(std::shared_ptr<DeviceClient> client, std::string serial, std::atomic<bool>& cancelled,
                                 std::function<void(const QString&)> progress)
    : m_client(std::move(client)), m_serial(std::move(serial)), m_cancelled(cancelled), m_progress(std::move(progress)),
      m_temporary(QDir::tempPath() + "/harmonycast_snapshot_XXXXXX"),
      m_remote("/data/local/tmp/hc_snapshot_" + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').toStdString()) {
#ifdef Q_OS_WIN
    // Avoid local code-page, Unicode path and file-lock problems in Windows HDC.
    m_transport = Transport::Text;
#endif
    if (!QImageReader::supportedImageFormats().contains("jpeg")) m_capture = Capture::UiTest;
}

SnapshotCapture::~SnapshotCapture() {
    if (m_client) m_client->run({"shell", "rm", "-f", m_remote + ".jpeg", m_remote + ".png"}, m_serial, 1000);
}

QImage SnapshotCapture::decodeImage(const QByteArray& data) {
    if (data.size() < 4 || data.size() > maxImageBytes) return {};
    const bool jpeg = data.startsWith(QByteArray::fromHex("ffd8")) && data.endsWith(QByteArray::fromHex("ffd9"));
    const bool png = data.startsWith(QByteArray::fromHex("89504e470d0a1a0a")) &&
        data.endsWith(QByteArray::fromHex("0000000049454e44ae426082"));
    if (!jpeg && !png) return {};
    QBuffer buffer; buffer.setData(data); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, jpeg ? "JPEG" : "PNG");
    const auto size = reader.size();
    if (size.width() <= 0 || size.height() <= 0 || size.width() > 16384 || size.height() > 16384 ||
        int64_t(size.width()) * size.height() > 64 * 1024 * 1024) return {};
    return reader.read();
}

SnapshotResult SnapshotCapture::receiveFile(const std::string& path) {
    SnapshotResult result;
    result.error = "兼容截图传输失败，未收到新的截图文件";
    if (!m_temporary.isValid()) { result.detail = m_temporary.errorString(); return result; }
    const auto local = QDir::toNativeSeparators(m_temporary.filePath("frame.image"));
    QFile file(local);
    if (file.exists() && !file.remove()) { result.detail = file.errorString(); return result; }
    m_progress("兼容截图传输中（文件通道）");
    const auto received = m_client->run({"file", "recv", path, local.toStdString()}, m_serial, 8000);
    if (m_cancelled.load()) return result;
    result.detail = detail(received);
    if (failed(received) || !file.exists() || !file.open(QIODevice::ReadOnly)) return result;
    const auto bytes = file.read(maxImageBytes + 1);
    result.image = decodeImage(bytes);
    if (result.image.isNull()) {
        result.error = "兼容截图图片不完整或无法解码";
        result.detail = QString("Received %1 bytes via file transfer").arg(bytes.size());
    } else { result.error.clear(); result.route = "HDC file recv"; }
    return result;
}

SnapshotResult SnapshotCapture::receiveText(const std::string& path) {
    SnapshotResult result;
    result.error = "兼容截图传输失败（文本通道）";
    const std::string alternate = m_encoder == "base64" ? "toybox base64" : "base64";
    for (const auto& encoder : {m_encoder, alternate}) {
        if (m_cancelled.load()) return result;
        m_progress("兼容截图传输中（文本通道）");
        const std::string command = "printf '__HC_IMAGE_BEGIN__\\n'; " + encoder + " " + path +
            " && printf '\\n__HC_IMAGE_END__\\n'";
        const auto response = m_client->runWithOutputLimit({"shell", command}, m_serial, 8000, 48 * 1024 * 1024);
        if (m_cancelled.load()) return result;
        const auto bytes = QByteArray::fromStdString(response.out);
        const QByteArray begin("__HC_IMAGE_BEGIN__"), end("__HC_IMAGE_END__");
        const int first = bytes.indexOf(begin), last = bytes.indexOf(end, std::max(0, first));
        // Never put encoded screenshots in diagnostic logs.
        result.detail = QString("%1 exit=%2, stdout=%3 bytes, stderr=%4")
            .arg(QString::fromStdString(encoder)).arg(response.exit_code).arg(bytes.size()).arg(QString::fromStdString(response.err).left(800));
        if (!response.success() || first < 0 || last <= first || bytes.size() > 48 * 1024 * 1024 ||
            failed(CommandResult{0, (bytes.left(first) + bytes.mid(last + end.size())).toStdString(), response.err})) continue;
        auto encoded = bytes.mid(first + begin.size(), last - first - begin.size());
        encoded.replace("\r", ""); encoded.replace("\n", ""); encoded.replace(" ", ""); encoded.replace("\t", "");
        const auto decoded = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
        result.image = decodeImage(decoded);
        if (!result.image.isNull()) {
            m_encoder = encoder;
            result.error.clear(); result.route = QString("HDC shell %1").arg(QString::fromStdString(encoder)); return result;
        }
        result.error = "兼容截图图片不完整或无法解码（文本通道）";
    }
    return result;
}

SnapshotResult SnapshotCapture::receive(const std::string& path) {
    auto first = m_transport == Transport::File ? receiveFile(path) : receiveText(path);
    if (!first.image.isNull() || m_cancelled.load()) return first;
    auto second = m_transport == Transport::File ? receiveText(path) : receiveFile(path);
    if (!second.image.isNull()) {
        m_transport = m_transport == Transport::File ? Transport::Text : Transport::File;
        return second;
    }
    // Prefer a real image error over an unavailable alternate transport.
    if (first.error.contains("图片")) { first.detail += "\n" + second.detail; return first; }
    second.detail = first.detail + "\n" + second.detail;
    return second;
}

SnapshotResult SnapshotCapture::capture(Capture kind, QSize target) {
    SnapshotResult result;
    const auto path = m_remote + (kind == Capture::Snapshot ? ".jpeg" : ".png");
    static const QRegularExpression resizeError(R"(invalid option|unknown option|unrecognized option|unsupported option)",
        QRegularExpression::CaseInsensitiveOption);
    for (int attempt = 0; attempt < 2 && !m_cancelled.load(); ++attempt) {
        const bool scaled = kind == Capture::Snapshot && m_resize_supported && !m_source.isEmpty() &&
            target.width() > 0 && target.height() > 0;
        std::string command = "rm -f " + path + " && " + (kind == Capture::Snapshot ? "snapshot_display -f " : "uitest screenCap -p ") + path;
        if (scaled) command += " -w " + std::to_string(target.width()) + " -h " + std::to_string(target.height());
        m_progress(kind == Capture::Snapshot ? "正在执行设备截图命令（snapshot_display）" : "正在执行设备截图命令（uitest screenCap）");
        const auto response = m_client->run({"shell", command}, m_serial, 8000);
        if (m_cancelled.load()) return result;
        const auto text = detail(response);
        if (scaled && resizeError.match(text).hasMatch()) { m_resize_supported = false; continue; }
        if (failed(response) || unavailable(text) || resizeError.match(text).hasMatch()) {
            result.detail = text;
            result.permanent = unavailable(text);
            if (text.contains("denied", Qt::CaseInsensitive) || text.contains("not permitted", Qt::CaseInsensitive))
                result.error = "设备未授予截图权限，兼容截图无法继续";
            else if (result.permanent) result.error = "设备截图命令不可用，无法启用兼容截图";
            else result.error = "兼容截图命令执行失败";
            return result;
        }
        result = receive(path);
        if (!result.image.isNull()) {
            if (!scaled) m_source = result.image.size();
            result.sourceSize = m_source;
            if (!scaled && target.width() > 0 && target.height() > 0)
                result.image = result.image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            result.route.prepend(kind == Capture::Snapshot ? "snapshot_display / " : "uitest screenCap / ");
        }
        return result;
    }
    return result;
}

SnapshotResult SnapshotCapture::next(QSize target) {
    auto first = capture(m_capture, target);
    if (!first.image.isNull() || m_cancelled.load()) return first;
    const auto alternate = m_capture == Capture::Snapshot ? Capture::UiTest : Capture::Snapshot;
    auto second = capture(alternate, target);
    if (!second.image.isNull()) { m_capture = alternate; return second; }
    if (first.permanent && !second.permanent) {
        second.detail.prepend(first.error + "\n" + first.detail + "\n");
        return second;
    }
    first.permanent = first.permanent && second.permanent;
    if (second.error.contains("截图权限")) first.error = second.error;
    first.detail += "\nAlternate capture: " + second.error + "\n" + second.detail;
    return first;
}

} // namespace harmonycast
