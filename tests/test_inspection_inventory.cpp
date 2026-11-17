#include "harmonycast/InspectionInventory.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <iostream>
#include <stdexcept>
using namespace harmonycast;
static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void save(const QString& path, const QByteArray& bytes) {
    QFile file(path); check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "fixture write failed");
}
static quint32 crc32(const QByteArray& data) {
    quint32 crc = 0xffffffffU;
    for (unsigned char byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
static void u16(QByteArray& out, quint16 value) { const auto le = qToLittleEndian(value); out.append(reinterpret_cast<const char*>(&le), 2); }
static void u32(QByteArray& out, quint32 value) { const auto le = qToLittleEndian(value); out.append(reinterpret_cast<const char*>(&le), 4); }
static QByteArray zip(const QList<QPair<QByteArray, QByteArray>>& files, bool deflate = false) {
    QByteArray out, directory;
    for (const auto& entry : files) {
        const auto offset = out.size(); const auto checksum = crc32(entry.second);
        auto packed = deflate ? qCompress(entry.second) : entry.second;
        if (deflate) packed = packed.mid(6, packed.size() - 10); // qCompress length + zlib framing -> raw DEFLATE.
        u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, deflate ? 8 : 0); u16(out, 0); u16(out, 0);
        u32(out, checksum); u32(out, packed.size()); u32(out, entry.second.size()); u16(out, entry.first.size()); u16(out, 0);
        out += entry.first; out += packed;
        u32(directory, 0x02014b50); u16(directory, 20); u16(directory, 20); u16(directory, 0); u16(directory, deflate ? 8 : 0); u16(directory, 0); u16(directory, 0);
        u32(directory, checksum); u32(directory, packed.size()); u32(directory, entry.second.size());
        u16(directory, entry.first.size()); u16(directory, 0); u16(directory, 0); u16(directory, 0); u16(directory, 0); u32(directory, 0); u32(directory, offset);
        directory += entry.first;
    }
    const auto offset = out.size(); out += directory;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, files.size()); u16(out, files.size());
    u32(out, directory.size()); u32(out, offset); u16(out, 0); return out;
}
static QList<QPair<QByteArray, QByteArray>> workbook() {
    return {
        {"xl/workbook.xml", R"(<workbook xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="清单" r:id="rSheet"/></sheets></workbook>)"},
        {"xl/_rels/workbook.xml.rels", R"(<Relationships><Relationship Id="rSheet" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/custom.xml"/></Relationships>)"},
        {"xl/sharedStrings.xml", R"(<sst><si><t>ip</t></si><si><r><t>dept</t></r><r><t>name</t></r></si><si><t>bedname</t></si><si><t>192.0.2.7-1</t></si><si><t>内科</t></si></sst>)"},
        {"xl/worksheets/custom.xml", R"(<worksheet><sheetData><row r="1"><c r="A1" t="s"><v>0</v></c><c r="C1" t="s"><v>1</v></c><c r="D1" t="s"><v>2</v></c><c r="F1" t="inlineStr"><is><t>roomname</t></is></c></row><row r="2"><c r="A2" t="s"><v>3</v></c><c r="C2" t="s"><v>4</v></c><c r="D2"><v>12</v></c><c r="F2" t="inlineStr"><is><t>病房3</t></is></c></row></sheetData></worksheet>)"}
    };
}
static void testCsv() {
    auto list = InspectionInventory::parseCsv("ip,devicetype,devicemodel,deptname,bedname,roomname\r\n192.0.2.1,wnBedHeadExtension,H35,\"内科,一病区\",01,301\r\n192.0.2.1,wnBedHeadExtension,H35,\"内科,一病区\",01,301\r\n192.0.2.1,wnBedHeadExtension,H35,\"内科,一病区\",02,301\r\n");
    check(list.errors.isEmpty() && list.targets.size() == 2 && list.duplicates == 1, "inventory duplicates lose distinct beds");
    const auto& first = list.targets[0];
    check(first.deviceType == "床头分机" && first.inventoryModel == "H35" && first.department == "内科,一病区" && first.bed == "01" && first.room == "301", "inventory metadata lost");
    auto chinese = InspectionInventory::parseCsv("设备IP\t设备类型\t型号\t科室\t床号\t房间\n192.0.2.2:9000\t门口机\tM2\t\"内科\n二病区\"\t003\t302\n");
    check(chinese.errors.isEmpty() && chinese.targets[0].port == 9000 && chinese.targets[0].department == "内科\n二病区", "Chinese TSV or multiline fields lost");
    auto suffix = InspectionInventory::parseCsv("ip,bedname\n192.0.2.3-1,1\n", DevicePlatform::Android);
    check(suffix.errors.isEmpty() && suffix.targets[0].endpoint() == "192.0.2.3:5555", "script IP suffix or platform port lost");
    check(!InspectionInventory::parseCsv("ip\n999.0.2.1\n").errors.isEmpty(), "invalid IP silently accepted");
    check(!InspectionInventory::parseCsv("deptname\n内科\n").errors.isEmpty(), "missing IP header accepted");
    check(!InspectionInventory::parseCsv("ip,bedname\n192.0.2.3,\"unterminated\n").errors.isEmpty(), "broken CSV quote accepted");
    check(!InspectionInventory::parseCsv("ip,IP\n192.0.2.1,192.0.2.2\n").errors.isEmpty(), "ambiguous IP column accepted");
    check(!InspectionInventory::parseCsv("ip,bedname\n192.0.2.1," + QString(4097, 'a')).errors.isEmpty(), "oversized metadata accepted");
    QStringList columns; columns << "ip"; for (int i = 0; i < 128; ++i) columns << "field" + QString::number(i);
    check(!InspectionInventory::parseCsv(columns.join(',') + "\n192.0.2.1\n").errors.isEmpty(), "oversized column count accepted");
}
static void testFiles(const QString& folder) {
    QString error;
    auto utf16 = QByteArray::fromHex("fffe"); const QString text = "IP,科室,床位\r\n192.0.2.4,外科,9\r\n";
    for (const auto ch : text) u16(utf16, ch.unicode());
    save(folder + "/unicode.csv", utf16);
    auto csv = InspectionInventory::read(folder + "/unicode.csv", DevicePlatform::OpenHarmony, &error);
    check(error.isEmpty() && csv.targets.size() == 1 && csv.targets[0].department == "外科", "UTF16 CSV unavailable");
    save(folder + "/book.xls", zip(workbook()));
    auto xlsx = InspectionInventory::read(folder + "/book.xls", DevicePlatform::OpenHarmony, &error);
    check(error.isEmpty() && xlsx.errors.isEmpty() && xlsx.targets.size() == 1, "ZIP-formatted xls inventory unavailable");
    check(xlsx.targets[0].ip == "192.0.2.7" && xlsx.targets[0].department == "内科" && xlsx.targets[0].bed == "12" && xlsx.targets[0].room == "病房3", "xlsx shared/inline/sparse/number cells lost");
    save(folder + "/compressed.xlsx", zip(workbook(), true));
    check(InspectionInventory::read(folder + "/compressed.xlsx", DevicePlatform::OpenHarmony, &error).targets.size() == 1 && error.isEmpty(), "real DEFLATE workbook unavailable");
    auto inlineWorkbook = workbook(); inlineWorkbook.removeAt(2);
    inlineWorkbook.last().second = R"(<worksheet><sheetData><row><c r="A1" t="inlineStr"><is><t>IP</t></is></c></row><row><c r="A2" t="inlineStr"><is><t>192.0.2.8</t></is></c></row></sheetData></worksheet>)";
    save(folder + "/inline.xlsx", zip(inlineWorkbook));
    check(InspectionInventory::read(folder + "/inline.xlsx", DevicePlatform::OpenHarmony, &error).targets.size() == 1 && error.isEmpty(), "inline-only workbook requires shared strings");
    auto deepWorkbook = workbook();
    deepWorkbook[3].second = QByteArray("") + QByteArray("<worksheet>") + QByteArray("<x>").repeated(300) +
        QByteArray("</x>").repeated(300) + "</worksheet>";
    save(folder + "/deep.xlsx", zip(deepWorkbook, true));
    check(InspectionInventory::read(folder + "/deep.xlsx", DevicePlatform::OpenHarmony, &error).targets.empty() && error.contains("嵌套深度"),
          "deeply nested worksheet XML accepted");
    deepWorkbook = workbook();
    deepWorkbook[0].second = QByteArray("<workbook>") + QByteArray("<x>").repeated(300) +
        QByteArray("</x>").repeated(300) + "</workbook>";
    save(folder + "/deep-workbook.xlsx", zip(deepWorkbook, true));
    check(InspectionInventory::read(folder + "/deep-workbook.xlsx", DevicePlatform::OpenHarmony, &error).targets.empty() && error.contains("嵌套深度"),
          "deeply nested workbook XML accepted");
    auto manyLeaves = inlineWorkbook;
    QByteArray sheet = "<worksheet><sheetData><row><c t=\"inlineStr\"><is><t>IP</t></is></c></row>";
    for (int i = 0; i < 400; ++i) {
        const auto ip = QString("192.0.%1.%2").arg(i / 250 + 1).arg(i % 250 + 1).toUtf8();
        sheet += "<row><c t=\"inlineStr\"><is><t>" + ip + "</t></is></c></row>";
    }
    sheet += "</sheetData></worksheet>"; manyLeaves.last().second = sheet;
    save(folder + "/many-leaves.xlsx", zip(manyLeaves, true));
    check(InspectionInventory::read(folder + "/many-leaves.xlsx", DevicePlatform::OpenHarmony, &error).targets.size() == 400 && error.isEmpty(),
          "many shallow leaf cells incorrectly count as XML nesting");
    auto malicious = workbook(); malicious[1].second.replace("worksheets/custom.xml", "../../outside.xml");
    save(folder + "/traversal.xlsx", zip(malicious));
    check(InspectionInventory::read(folder + "/traversal.xlsx", DevicePlatform::OpenHarmony, &error).targets.empty() && !error.isEmpty(), "xlsx traversal accepted");
    malicious = workbook(); malicious[3].second.prepend("<!DOCTYPE worksheet [<!ENTITY x 'bad'>]>");
    save(folder + "/doctype.xlsx", zip(malicious));
    check(InspectionInventory::read(folder + "/doctype.xlsx", DevicePlatform::OpenHarmony, &error).targets.empty() && !error.isEmpty(), "xlsx DOCTYPE accepted");
    save(folder + "/legacy.xls", QByteArray::fromHex("d0cf11e0a1b11ae1"));
    check(InspectionInventory::read(folder + "/legacy.xls", DevicePlatform::OpenHarmony, &error).targets.empty() && error.contains("xlsx"), "legacy XLS conversion message missing");
    check(InspectionInventory::read(folder + "/missing.xlsx", DevicePlatform::OpenHarmony, &error).targets.empty() && !error.isEmpty(), "missing file accepted");
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); QTemporaryDir temporary;
    try { testCsv(); testFiles(temporary.path()); std::cout << "Inventory import tests passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
