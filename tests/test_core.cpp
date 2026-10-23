#include <cassert>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include "harmonycast/Types.h"
#include "harmonycast/PacketDemuxer.h"
#include "harmonycast/HiLogParser.h"
#include "harmonycast/VideoDecoder.h"
#include "harmonycast/HdcClient.h"

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

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "   HarmonyCast C++ Core Verification    " << std::endl;
    std::cout << "========================================" << std::endl;

    test_packet_demuxer();
    test_hilog_parser();
    test_video_decoder();

    std::cout << "\n>>> ALL C++ CORE TESTS PASSED! <<<" << std::endl;
    return 0;
}
