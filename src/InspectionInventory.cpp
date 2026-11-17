#include "harmonycast/InspectionInventory.h"
#include <QDir>
#include <QFile>
#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include "miniz.h"

namespace harmonycast {
namespace {
constexpr int maxColumns = 128, maxCell = 4096, maxXml = 32 * 1024 * 1024;
constexpr qint64 maxExpanded = 64 * 1024 * 1024;
using Rows = QList<QStringList>;

QString deviceType(QString value) {
    static const QMap<QString, QString> names{
        {"wnBedHeadExtension", "床头分机"}, {"wnDoorWayExtension", "门口分机"},
        {"wnCorridorScreen", "走廊显示屏"}, {"wnMedicalMainframe", "医疗主机"},
        {"wnToiletExtension", "卫生间分机"}, {"wnDutyMainframe", "值班主机"},
        {"wnMedicalAudioAssistant", "医疗音频助手"}, {"wnDoorLampExtension", "门灯分机"},
        {"bnNursingTV", "护理电视"}, {"iDoorWayExtension", "智能门口机"}};
    return names.value(value, value);
}

InspectionImport targets(const Rows& rows, DevicePlatform platform) {
    InspectionImport result;
    if (rows.isEmpty()) { result.errors << "设备清单为空"; return result; }
    for (const auto& row : rows) {
        if (row.size() > maxColumns || std::any_of(row.begin(), row.end(), [](const QString& cell) { return cell.size() > maxCell; })) {
            result.errors << "清单超过列数或单元格长度限制"; return result;
        }
    }
    const QList<QStringList> aliases{
        {"ip", "设备ip", "设备 ip", "ip地址", "ip 地址", "目标ip", "目标 ip"},
        {"devicetype", "设备类型"}, {"devicemodel", "设备型号", "型号"},
        {"deptname", "科室", "科室名称"}, {"bedname", "床位", "床号", "床位名称"},
        {"roomname", "房间", "房间号", "房间名称"}};
    QList<int> columns;
    for (const auto& accepted : aliases) {
        int found = -1;
        for (int i = 0; i < rows.front().size(); ++i) if (accepted.contains(rows.front()[i].trimmed().toLower())) {
            if (found >= 0) { result.errors << "设备清单存在重复字段：" + rows.front()[i]; return result; }
            found = i;
        }
        columns << found;
    }
    if (columns[0] < 0) { result.errors << "设备清单缺少 IP 列（ip / 设备IP）"; return result; }
    QMap<QString, QList<InspectionTarget>> seen;
    for (int i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (std::all_of(row.begin(), row.end(), [](const QString& cell) { return cell.trimmed().isEmpty(); })) continue;
        auto ip = row.value(columns[0]).trimmed();
        // Hospital exports use a suffix for logical secondary devices.
        ip.remove(QRegularExpression("-[0-9]+$"));
        const auto parsed = InspectionReport::parseTargets(ip, platform);
        if (ip.isEmpty() || !parsed.errors.isEmpty() || parsed.targets.size() != 1 ||
            (ip != parsed.targets[0].ip && ip != parsed.targets[0].endpoint())) {
            if (result.errors.size() < 100) result.errors << QString("第 %1 行：无效设备 IP %2").arg(i + 1).arg(ip.left(80));
            continue;
        }
        auto target = parsed.targets.front();
        target.deviceType = deviceType(row.value(columns[1]).trimmed());
        target.inventoryModel = row.value(columns[2]).trimmed();
        target.department = row.value(columns[3]).trimmed();
        target.bed = row.value(columns[4]).trimmed();
        target.room = row.value(columns[5]).trimmed();
        auto& previous = seen[target.endpoint()];
        const auto duplicate = std::any_of(previous.begin(), previous.end(), [&](const InspectionTarget& old) {
            return old.deviceType == target.deviceType && old.inventoryModel == target.inventoryModel &&
                old.department == target.department && old.bed == target.bed && old.room == target.room;
        });
        if (duplicate) { ++result.duplicates; continue; }
        if (result.targets.size() >= InspectionReport::MaxTargets) { result.errors << "设备清单最多 10000 行"; break; }
        previous << target;
        result.targets.push_back(std::move(target));
    }
    if (result.targets.empty() && result.errors.isEmpty()) result.errors << "设备清单中没有有效目标";
    return result;
}

Rows csvRows(QString text, QString& error) {
    if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
    const auto header = text.section('\n', 0, 0);
    const QChar delimiter = header.count('\t') > header.count(',') ? '\t' : ',';
    Rows rows;
    QStringList row;
    QString cell;
    bool quoted = false, closed = false;
    auto finishCell = [&] { row << cell; cell.clear(); closed = false; };
    for (int i = 0; i < text.size(); ++i) {
        const auto ch = text[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { cell += '"'; ++i; }
                else { quoted = false; closed = true; }
            } else cell += ch;
        } else if (ch == delimiter) finishCell();
        else if (ch == '\n' || ch == '\r') {
            if (ch == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            finishCell(); rows << row; row.clear();
        } else if (ch == '"' && cell.isEmpty() && !closed) quoted = true;
        else if (ch == '"' || (closed && !ch.isSpace())) { error = "CSV 引号格式不完整或不正确"; return {}; }
        else if (!closed) cell += ch;
        if (cell.size() > maxCell || row.size() > maxColumns || rows.size() > InspectionReport::MaxTargets + 1) {
            error = "清单超过行数、列数或单元格长度限制"; return {};
        }
    }
    if (quoted) { error = "CSV 引号未闭合"; return {}; }
    if (!cell.isEmpty() || !row.isEmpty() || closed) { finishCell(); rows << row; }
    if (rows.size() > InspectionReport::MaxTargets + 1) { error = "设备清单最多 10000 行"; return {}; }
    return rows;
}

class XlsxReader {
public:
    explicit XlsxReader(const QByteArray& bytes) { valid = mz_zip_reader_init_mem(&zip, bytes.constData(), size_t(bytes.size()), 0); }
    ~XlsxReader() { if (valid) mz_zip_reader_end(&zip); }
    bool valid{false};
    QString error;
    QByteArray xml(const QByteArray& name, bool optional = false) {
        if (!error.isEmpty()) return {};
        const int index = mz_zip_reader_locate_file(&zip, name.constData(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
        if (index < 0) { if (!optional) error = "XLSX 缺少文件：" + QString::fromUtf8(name); return {}; }
        mz_zip_archive_file_stat info{};
        if (!mz_zip_reader_file_stat(&zip, index, &info) || info.m_is_encrypted || info.m_uncomp_size > maxXml ||
            expanded + qint64(info.m_uncomp_size) > maxExpanded) { error = "XLSX 文件加密、损坏或解压内容超过限制"; return {}; }
        expanded += qint64(info.m_uncomp_size);
        QByteArray data(int(info.m_uncomp_size), '\0');
        if (!mz_zip_reader_extract_to_mem(&zip, index, data.data(), size_t(data.size()), 0)) error = "XLSX XML 解压或完整性校验失败";
        return error.isEmpty() ? data : QByteArray{};
    }
    bool next(QXmlStreamReader& reader) {
        if (!error.isEmpty()) return false;
        // readElementText() consumes its matching EndElement itself. Account
        // for the current token before advancing so leaf cells do not inflate
        // the nesting count, and track each document independently.
        auto& depth = depths[&reader];
        if (reader.isEndElement()) --depth;
        reader.readNext();
        if (reader.isStartElement() && ++depth > 128) {
            error = "XLSX XML 嵌套深度超过 128 层限制";
            return false;
        }
        if (reader.tokenType() == QXmlStreamReader::DTD) { error = "XLSX 不允许包含 DOCTYPE"; return false; }
        if (reader.hasError()) { error = "XLSX XML 格式错误：" + reader.errorString(); return false; }
        return !reader.atEnd();
    }
    Rows read() {
        if (!valid) { error = "XLSX ZIP 文件损坏"; return {}; }
        if (mz_zip_reader_get_num_files(&zip) > 4096) { error = "XLSX 包含过多内部文件"; return {}; }
        QString relationship;
        QXmlStreamReader workbook(xml("xl/workbook.xml"));
        while (next(workbook)) if (workbook.isStartElement() && workbook.name() == QStringLiteral("sheet") && relationship.isEmpty())
            for (const auto& attribute : workbook.attributes()) if (attribute.name() == QStringLiteral("id")) relationship = attribute.value().toString();
        if (relationship.isEmpty()) { if (error.isEmpty()) error = "XLSX 没有可读取的工作表"; return {}; }
        QString sheet;
        QXmlStreamReader relations(xml("xl/_rels/workbook.xml.rels"));
        while (next(relations)) if (relations.isStartElement() && relations.name() == QStringLiteral("Relationship") &&
            relations.attributes().value("Id") == relationship) {
            const auto attributes = relations.attributes();
            if (attributes.value("TargetMode") == QStringLiteral("External")) { error = "XLSX 工作表不能引用外部文件"; return {}; }
            auto target = attributes.value("Target").toString();
            if (target.contains(':') || target.contains('\\')) { error = "XLSX 工作表路径不合法"; return {}; }
            sheet = QDir::cleanPath(target.startsWith('/') ? target.mid(1) : "xl/" + target);
            if (!sheet.startsWith("xl/") || !sheet.endsWith(".xml")) { error = "XLSX 工作表路径越界"; return {}; }
        }
        if (sheet.isEmpty()) { if (error.isEmpty()) error = "XLSX 工作表关系缺失"; return {}; }
        QStringList strings;
        const auto sharedBytes = xml("xl/sharedStrings.xml", true);
        QXmlStreamReader shared(sharedBytes);
        QString string;
        while (!sharedBytes.isEmpty() && next(shared)) {
            if (shared.isStartElement() && shared.name() == QStringLiteral("si")) string.clear();
            else if (shared.isStartElement() && shared.name() == QStringLiteral("t")) string += shared.readElementText();
            else if (shared.isEndElement() && shared.name() == QStringLiteral("si")) strings << string;
            if (string.size() > maxCell || strings.size() > InspectionReport::MaxTargets * maxColumns) { error = "XLSX 文本内容超过限制"; return {}; }
        }
        if (!error.isEmpty()) return {};
        QXmlStreamReader worksheet(xml(sheet.toUtf8()));
        Rows rows; QStringList row; QString value, inlineValue, type; int column = 0, nextColumn = 0;
        while (next(worksheet)) {
            const auto name = worksheet.name();
            if (worksheet.isStartElement() && name == QStringLiteral("row")) { row.clear(); nextColumn = 0; }
            else if (worksheet.isStartElement() && name == QStringLiteral("c")) {
                column = 0;
                const auto reference = worksheet.attributes().value("r").toString();
                for (const auto ch : reference) {
                    if (ch >= 'A' && ch <= 'Z') column = column * 26 + ch.unicode() - 'A' + 1;
                    else break;
                    if (column > maxColumns) break;
                }
                column = column ? column - 1 : nextColumn; nextColumn = column + 1;
                if (column >= maxColumns || column < 0) { error = "XLSX 超过 128 列限制"; return {}; }
                type = worksheet.attributes().value("t").toString(); value.clear(); inlineValue.clear();
            } else if (worksheet.isStartElement() && name == QStringLiteral("v")) value = worksheet.readElementText();
            else if (worksheet.isStartElement() && name == QStringLiteral("t")) inlineValue += worksheet.readElementText();
            else if (worksheet.isEndElement() && name == QStringLiteral("c")) {
                if (type == "s") {
                    bool ok = false; const int index = value.toInt(&ok);
                    if (!ok || index < 0 || index >= strings.size()) { error = "XLSX 共享字符串索引无效"; return {}; }
                    value = strings[index];
                } else if (type == "inlineStr") value = inlineValue;
                if (value.size() > maxCell) { error = "XLSX 单元格内容超过限制"; return {}; }
                while (row.size() <= column) row << QString{};
                row[column] = value;
            } else if (worksheet.isEndElement() && name == QStringLiteral("row")) {
                if (rows.isEmpty() && std::all_of(row.begin(), row.end(), [](const QString& v) { return v.trimmed().isEmpty(); })) continue;
                rows << row;
                if (rows.size() > InspectionReport::MaxTargets + 1) { error = "设备清单最多 10000 行"; return {}; }
            }
        }
        return error.isEmpty() ? rows : Rows{};
    }
private:
    mz_zip_archive zip{};
    qint64 expanded{0};
    QHash<QXmlStreamReader*, int> depths;
};
} // namespace

InspectionImport InspectionInventory::parseCsv(const QString& text, DevicePlatform platform) {
    if (text.size() > MaxFileBytes || text.toUtf8().size() > MaxFileBytes) {
        InspectionImport result; result.errors << "设备清单不能超过 16 MiB"; return result;
    }
    QString error;
    const auto rows = csvRows(text, error);
    if (!error.isEmpty()) { InspectionImport result; result.errors << error; return result; }
    return targets(rows, platform);
}

InspectionImport InspectionInventory::read(const QString& path, DevicePlatform platform, QString* error) {
    if (error) error->clear();
    auto fail = [&](const QString& message) { if (error) *error = message; InspectionImport result; result.errors << message; return result; };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(file.errorString());
    if (file.size() > MaxFileBytes) return fail("设备清单不能超过 16 MiB");
    const auto bytes = file.read(MaxFileBytes + 1);
    if (file.error() != QFile::NoError || bytes.size() > MaxFileBytes) return fail("设备清单读取失败或超过 16 MiB");
    if (bytes.startsWith("PK\003\004")) {
        XlsxReader reader(bytes); const auto rows = reader.read();
        if (!reader.error.isEmpty()) return fail(reader.error);
        return targets(rows, platform);
    }
    if (bytes.startsWith(QByteArray::fromHex("d0cf11e0a1b11ae1"))) return fail("该文件为旧版二进制 .xls，请另存为 .xlsx 或 CSV 后导入");
    QString text;
    if (bytes.startsWith(QByteArray::fromHex("fffe")) || bytes.startsWith(QByteArray::fromHex("feff"))) {
        if (bytes.size() % 2) return fail("UTF-16 设备清单内容不完整");
        const bool little = static_cast<unsigned char>(bytes[0]) == 0xff;
        for (int i = 2; i < bytes.size(); i += 2) {
            const auto a = static_cast<unsigned char>(bytes[i]), b = static_cast<unsigned char>(bytes[i + 1]);
            text += QChar(ushort(little ? a | b << 8 : a << 8 | b));
        }
    } else text = QString::fromUtf8(bytes);
    return parseCsv(text, platform);
}
} // namespace harmonycast
