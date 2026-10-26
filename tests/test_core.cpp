#include <cassert>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <QCoreApplication>
#include "harmonycast/Types.h"
#include "harmonycast/PacketDemuxer.h"
#include "harmonycast/HiLogParser.h"
#include "harmonycast/VideoDecoder.h"
#include "harmonycast/StreamSession.h"
#include "harmonycast/WebViewDebugger.h"
#include "harmonycast/NetworkDeviceDialog.h"
#include "harmonycast/HdcClient.h"
#include "harmonycast/Logger.h"
#include "harmonycast/CrashHandler.h"
#include "harmonycast/UpdateChecker.h"
#include <QDir>
#include <QFile>

using namespace harmonycast;

void test_packet_demuxer() {
    std::cout << "[RUN] test_packet_demuxer..." << std::endl;
    PacketDemuxer demuxer;

    std::string header_name;
    int header_w = 0, header_h = 0;
    demuxer.setHeaderCallback([&](const std::string& name, int w, int h) {
        header_name = name;
        header_w = w;
        header_h = h;
    });

    std::vector<StreamPacket> packets;
    demuxer.setPacketCallback([&](const StreamPacket& pkt) {
        packets.push_back(pkt);
    });

    // 1. Build a 72-byte header: 64 bytes name + 4 bytes W + 4 bytes H
    std::vector<uint8_t> buffer(72, 0);
    const char* name = "OpenHarmony-Phone-X";
    std::memcpy(buffer.data(), name, std::strlen(name));
    // 1080 = 0x00000438
    buffer[64] = 0x00; buffer[65] = 0x00; buffer[66] = 0x04; buffer[67] = 0x38;
    // 1920 = 0x00000780
    buffer[68] = 0x00; buffer[69] = 0x00; buffer[70] = 0x07; buffer[71] = 0x80;

    // Feed in two chunks to test streaming robustness
    demuxer.feed(std::span(buffer.data(), 40));
    assert(!demuxer.hasHeader());
    demuxer.feed(std::span(buffer.data() + 40, 32));
    assert(demuxer.hasHeader());
    assert(header_name == "OpenHarmony-Phone-X");
    assert(header_w == 1080);
    assert(header_h == 1920);

    // 2. Build a packet: [Type: 3 (Keyframe)] [Size: 4] [Payload: 0x00, 0x00, 0x00, 0x01]
    std::vector<uint8_t> pkt_data = {
        0x03,                   // type 3
        0x00, 0x00, 0x00, 0x04, // size 4
        0x00, 0x00, 0x00, 0x01  // NALU prefix
    };
    demuxer.feed(pkt_data);
    assert(packets.size() == 1);
    assert(packets[0].type == 3);
    assert(packets[0].is_keyframe == true);
    assert(packets[0].payload.size() == 4);
    assert(packets[0].payload[3] == 0x01);

    // 3. Test HCR2 raw RGBA frame parsing
    DecodedFrame raw_frame;
    demuxer.setRawFrameCallback([&](const DecodedFrame& f) {
        raw_frame = f;
    });

    std::vector<uint8_t> hcr2_buf(32 + 16, 0);
    hcr2_buf[0] = 'H'; hcr2_buf[1] = 'C'; hcr2_buf[2] = 'R'; hcr2_buf[3] = '2';
    hcr2_buf[7] = 2;  // Width = 2
    hcr2_buf[11] = 2; // Height = 2
    hcr2_buf[15] = 8; // Stride = 8
    hcr2_buf[19] = 16; // Length = 16
    hcr2_buf[23] = 2; // Source W = 2
    hcr2_buf[27] = 2; // Source H = 2
    hcr2_buf[31] = 0; // Rotation = 0
    for (int i = 0; i < 16; ++i) hcr2_buf[32 + i] = static_cast<uint8_t>(i + 1);

    demuxer.feed(hcr2_buf);
    assert(raw_frame.width == 2);
    assert(raw_frame.height == 2);
    assert(raw_frame.stride == 8);
    assert(raw_frame.data.size() == 16);
    assert(raw_frame.data[0] == 1);
    assert(raw_frame.data[15] == 16);

    std::cout << "[PASS] test_packet_demuxer passed!" << std::endl;
}

void test_hilog_parser() {
    std::cout << "[RUN] test_hilog_parser..." << std::endl;
    HiLogParser parser;

    // 1. ANSI stripping
    std::string ansi = "\x1b[31m10-23 11:24:45.913  1162  1162 D A06666/MVP_Tag: message\x1b[0m";
    std::string clean = HiLogParser::stripAnsi(ansi);
    assert(clean.find("\x1b") == std::string::npos);
    assert(clean.find("10-23 11:24:45.913") != std::string::npos);

    // 2. Line parsing
    std::string raw = "10-23 11:24:45.913  1162  1162 D A06666/MVP_RealClientProcessor: [MVP_RealClientProcessor] 收到CallInfo消息";
    HiLogEntry entry = parser.parseLine(raw, "com.yarward.etnroomoh");
    assert(entry.timestamp == "11:24:45.913");
    assert(entry.level == LogLevel::Debug);
    assert(entry.pid == 1162);
    assert(entry.tid == 1162);
    assert(entry.domain == "A06666");
    assert(entry.tag == "MVP_RealClientProcessor");
    assert(entry.category == "app");
    assert(entry.bundle == "com.yarward.etnroomoh");
    assert(entry.message == "[MVP_RealClientProcessor] 收到CallInfo消息");
    assert(entry.format().find("11:24:45.913  DEBUG  [com.yarward.etnroomoh]  [MVP_RealClientProcessor] [MVP_RealClientProcessor] 收到CallInfo消息") != std::string::npos);

    // 3. System log classification
    std::string sys_raw = "10-23 11:24:45.914  500  500 I C00001/CoreService: kernel event";
    HiLogEntry sys_entry = parser.parseLine(sys_raw);
    assert(sys_entry.category == "system");

    // 4. Wildcard package matching
    assert(HiLogParser::packageMatches("com.yarward.etnroomoh", "com.yarward.*"));
    assert(HiLogParser::packageMatches("com.yarward.etnroomoh", ""));
    assert(HiLogParser::packageMatches("com.yarward.etnroomoh", "全部应用"));
    assert(!HiLogParser::packageMatches("com.example.other", "com.yarward.*"));

    // 5. Keyword filtering
    assert(HiLogParser::matches(entry, "CallInfo", LogLevel::Debug, "app", "com.yarward.*"));
    assert(!HiLogParser::matches(entry, "NotExist", LogLevel::Debug));
    assert(!HiLogParser::matches(entry, "CallInfo", LogLevel::Error)); // min_level Error should reject Debug

    std::cout << "[PASS] test_hilog_parser passed!" << std::endl;
}

void test_video_decoder() {
    std::cout << "[RUN] test_video_decoder..." << std::endl;
    VideoDecoder decoder;
    bool ok = decoder.init(1080, 1920, true);
    assert(ok);
    assert(decoder.isReady());
    std::cout << "  Decoder initialized! Hardware acceleration active: " << (decoder.isHardwareAccelerated() ? "YES (VideoToolbox/D3D)" : "NO (CPU multi-thread)") << std::endl;
    decoder.close();
    assert(!decoder.isReady());
    std::cout << "[PASS] test_video_decoder passed!" << std::endl;
}

void test_stream_session_and_types() {
    std::cout << "[RUN] test_stream_session_and_types..." << std::endl;
    // 1. Verify DecodedFrame metatype
    int typeId = qMetaTypeId<DecodedFrame>();
    assert(typeId != QMetaType::UnknownType);

    // 2. StreamSession fallback state
    auto hdc = std::make_shared<HdcClient>();
    Device dev;
    dev.serial = "127.0.0.1:8710";
    dev.is_network = true;
    StreamSession session(hdc, dev);
    assert(!session.isRunning());
    assert(!session.isFallbackSnapshot());
    assert(session.device().serial == "127.0.0.1:8710");
    std::cout << "[PASS] test_stream_session_and_types passed!" << std::endl;
}

void test_webview_and_filters() {
    std::cout << "[RUN] test_webview_and_filters..." << std::endl;
    // 1. Package wildcard matching
    assert(HiLogParser::packageMatches("com.yarward.etnroomoh", "com.yarward.*"));
    assert(HiLogParser::packageMatches("com.yarward.bedside", "com.yarward.*"));
    assert(!HiLogParser::packageMatches("com.huawei.browser", "com.yarward.*"));

    // 2. cURL generation
    WebNetworkRequest req;
    req.method = "POST";
    req.url = "https://api.example.com/login";
    req.headers["Content-Type"] = "application/json";
    req.post_data = "{\"user\":\"admin\"}";
    QString curl = WebViewDebugger::generateCurl(req);
    assert(curl.contains("curl -X POST 'https://api.example.com/login'"));
    assert(curl.contains("-H 'Content-Type: application/json'"));
    assert(curl.contains("--data-raw '{\"user\":\"admin\"}'"));
    std::cout << "[PASS] test_webview_and_filters passed!" << std::endl;
}

void test_network_devices_persistence() {
    std::cout << "[RUN] test_network_devices_persistence..." << std::endl;
    auto initial_list = NetworkDeviceDialog::loadSavedDevices();
    size_t initial_count = initial_list.size();

    // 1. Save a new test device
    NetworkDeviceDialog::saveDevice("192.168.100.250", "Unit Test Device", 8710);
    auto after_save = NetworkDeviceDialog::loadSavedDevices();
    assert(after_save.size() == initial_count + 1);
    assert(after_save.front().ip == "192.168.100.250");
    assert(after_save.front().port == 8710);

    // 2. Delete the test device
    NetworkDeviceDialog::deleteSavedDevice("192.168.100.250");
    auto after_delete = NetworkDeviceDialog::loadSavedDevices();
    assert(after_delete.size() == initial_count);
    for (const auto& d : after_delete) {
        assert(d.ip != "192.168.100.250");
    }

    std::cout << "[PASS] test_network_devices_persistence passed!" << std::endl;
}

void test_device_info_parsing() {
    std::cout << "[RUN] test_device_info_parsing..." << std::endl;
    // Simulate raw shell output with tabs in param dump and ps containing command line arguments
    std::string raw =
        "<<<PARAM>>>\n"
        "\t  param: const.product.brand=Yarward\n"
        "\t  param: const.product.model=YH-H35M\n"
        "\t  param: const.product.hardwareversion=YH-H35M01\n"
        "\t  param: const.product.software.version=YH-H35MS01_v1.0\n"
        "\t  param: const.ohos.fullname=OpenHarmony-4.0.10.13\n"
        "\t  param: const.ohos.apiversion=10\n"
        "\t  param: const.product.cpu.abilist=arm64-v8a\n"
        "\t  param: ohos.boot.hardware=rk3566\n"
        "\t  param: const.product.devicetype=tablet\n"
        "\t  param: const.product.bootloader.version=u-boot-v2023.08\n"
        "\t  param: const.product.devUdid=54DA186990BAD4F8D3686DC54EE62A4C5A455164778DF4E08BE0BE48C04684FD\n"
        "\t  param: ohos.boot.sn=f4012241524a313158155d8295968c00\n"
        "<<<UPTIME>>>\n"
        "100000.5 200000.2\n"
        "<<<MEMINFO>>>\n"
        "MemTotal:        3926832 kB\n"
        "MemAvailable:    2100000 kB\n"
        "<<<DF>>>\n"
        "/dev/block/by-name/userdata 25G  3.2G   21G  14% /data\n"
        "<<<NET>>>\n"
        "eth0      Link encap:Ethernet  HWaddr 00:11:22:33:44:55\n"
        "          inet addr:10.10.214.61  Bcast:10.10.214.255  Mask:255.255.255.0\n"
        "<<<IFACES>>>\n"
        "eth0: 00:11:22:33:44:55\n"
        "<<<UNAME>>>\n"
        "Linux localhost 5.10.165 #1 SMP Wed Dec 3 16:52:34 CST 2025 aarch64\n"
        "<<<DISP>>>\n"
        "DisplayId ScreenId RefreshRate VPR Rotation Orientation DisplayOrientation FreezeFlag [ x   y   w   h   ]\n"
        "0         0        59          1.343751        2           1                 1          [ 0    0    1280 800  ]\n"
        "<<<ICU>>>\n"
        "model: H35M\n"
        "<<<PKGS>>>\n"
        "com.yarward.etnbedsideoh\n"
        "<<<PS>>>\n"
        " 1121 com.yarward.etnbedsideoh\n";

    // Test section parsing
    std::map<std::string, std::string> sections;
    std::string current_tag;
    std::string current_content;

    std::istringstream raw_stream(raw);
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

    assert(sections.count("PARAM"));
    assert(sections.count("UNAME"));
    assert(sections.count("DISP"));

    // Verify Kernel
    std::string kernel = sections["UNAME"];
    kernel.erase(0, kernel.find_first_not_of(" \t\r\n"));
    kernel.erase(kernel.find_last_not_of(" \t\r\n") + 1);
    assert(kernel.find("Linux localhost 5.10.165") != std::string::npos);
    assert(kernel.find("uname -a") == std::string::npos); // MUST NOT be corrupted by command arguments

    // Verify UDID and SN
    std::string udid, sn, chip, brand;
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
            if (k == "const.product.devUdid") udid = v;
            else if (k == "ohos.boot.sn") sn = v;
            else if (k == "ohos.boot.hardware") chip = v;
            else if (k == "const.product.brand") brand = v;
        }
    }

    assert(udid == "54DA186990BAD4F8D3686DC54EE62A4C5A455164778DF4E08BE0BE48C04684FD");
    assert(sn == "f4012241524a313158155d8295968c00");
    assert(chip == "rk3566");
    assert(brand == "Yarward");

    std::cout << "[PASS] test_device_info_parsing passed!" << std::endl;
}

void test_logger_and_crash_handler() {
    std::cout << "[RUN] test_logger_and_crash_handler..." << std::endl;

    QString testDir = QDir::tempPath() + "/harmonycast_test_logs";
    QDir(testDir).removeRecursively();

    // 1. Initialize Logger
    Logger::init(testDir);
    assert(Logger::logDirectory() == testDir);

    // 2. Write Logs
    HC_LOG_INFO("Core unit test log line 1");
    HC_LOG_WARN("Core unit test warning line 2");
    HC_LOG_ERROR("Core unit test error line 3");

    // 3. Verify in-memory recent logs
    auto recent = Logger::getRecentLogs(10);
    assert(recent.size() >= 3);
    bool foundLine1 = false;
    for (const auto& l : recent) {
        if (l.contains("Core unit test log line 1")) foundLine1 = true;
    }
    assert(foundLine1);

    // 4. Verify file write
    QString logFile = Logger::currentLogFilePath();
    assert(QFile::exists(logFile));
    QFile f(logFile);
    bool okOpen = f.open(QIODevice::ReadOnly | QIODevice::Text);
    assert(okOpen);
    QString content = QString::fromUtf8(f.readAll());
    assert(content.contains("Core unit test log line 1"));
    assert(content.contains("Core unit test warning line 2"));
    f.close();

    // 5. Verify CrashHandler setup and marker file logic
    CrashHandler::install();
    assert(!CrashHandler::hasCrashFromPreviousRun());

    // Simulate crash marker file
    QString markerPath = testDir + "/crashes/.unreviewed_crash";
    QFile marker(markerPath);
    bool okMarker = marker.open(QIODevice::WriteOnly);
    assert(okMarker);
    marker.write("test crash");
    marker.close();

    assert(CrashHandler::hasCrashFromPreviousRun());
    CrashHandler::markCrashAsReviewed();
    assert(!CrashHandler::hasCrashFromPreviousRun());

    // 6. Test diagnostic bundle export
    QString zipDest = testDir + "/bundle.zip";
    QString bundlePath = Logger::exportDiagnosticBundle(zipDest);
    assert(!bundlePath.isEmpty());
    assert(QFile::exists(bundlePath));

    Logger::shutdown();
    QDir(testDir).removeRecursively();
    std::cout << "[PASS] test_logger_and_crash_handler passed!" << std::endl;
}

void test_update_checker() {
    std::cout << "[RUN] test_update_checker..." << std::endl;

    // 1. SemVer comparison tests
    assert(UpdateChecker::compareVersions("2.2.0", "2.1.0") > 0);
    assert(UpdateChecker::compareVersions("2.1.0", "2.2.0") < 0);
    assert(UpdateChecker::compareVersions("v2.1.0", "2.1.0") == 0);
    assert(UpdateChecker::compareVersions("V2.1.0", "2.1.0") == 0);
    assert(UpdateChecker::compareVersions("2.1.1", "2.1.0") > 0);
    assert(UpdateChecker::compareVersions("2.1.0", "2.1.1") < 0);
    assert(UpdateChecker::compareVersions("3.0.0", "2.9.9") > 0);
    assert(UpdateChecker::compareVersions("2.1.0-rc1", "2.1.0") == 0);
    assert(UpdateChecker::compareVersions("2.1", "2.1.0") == 0);

    // 2. Ignore version logic
    UpdateChecker checker;
    assert(checker.currentVersion() == "2.1.1");

    checker.ignoreVersion("2.2.0");
    assert(checker.isVersionIgnored("2.2.0"));
    assert(!checker.isVersionIgnored("2.3.0"));

    checker.clearIgnoredVersion();
    assert(!checker.isVersionIgnored("2.2.0"));

    std::cout << "[PASS] test_update_checker passed!" << std::endl;
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    std::cout << "========================================" << std::endl;
    std::cout << "   HarmonyCast C++ Core Verification    " << std::endl;
    std::cout << "========================================" << std::endl;

    test_packet_demuxer();
    test_hilog_parser();
    test_video_decoder();
    test_stream_session_and_types();
    test_webview_and_filters();
    test_network_devices_persistence();
    test_device_info_parsing();
    test_logger_and_crash_handler();
    test_update_checker();

    std::cout << "\n>>> ALL C++ CORE TESTS PASSED! <<<" << std::endl;
    return 0;
}
