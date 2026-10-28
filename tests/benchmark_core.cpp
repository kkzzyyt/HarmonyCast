#include "harmonycast/PacketDemuxer.h"
#include "harmonycast/ScreenView.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <cstring>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    constexpr int width = 1080, height = 1920, iterations = 120;
    QByteArray bytes(32 + width * height * 4, '\0');
    std::memcpy(bytes.data(), "HCR2", 4);
    auto write32 = [&](int offset, quint32 value) {
        for (int i = 0; i < 4; ++i) bytes[offset + i] = char(value >> (24 - i * 8));
    };
    write32(4, width); write32(8, height); write32(12, width * 4);
    write32(16, width * height * 4); write32(20, width); write32(24, height);
    for (int i = 32; i < bytes.size(); ++i) bytes[i] = char(i % 251);
    harmonycast::PacketDemuxer parser;
    harmonycast::ScreenView screen;
    screen.resize(540, 960);
    screen.show();
    int frames = 0;
    parser.setRawFrameCallback([&](const harmonycast::DecodedFrame& frame) {
        screen.updateFrame(frame);
        ++frames;
    });
    auto feed = [&] {
        for (qsizetype offset = 0; offset < bytes.size(); offset += 64 * 1024) {
            size_t size = size_t(std::min(qsizetype(64 * 1024), bytes.size() - offset));
            parser.feed(std::span(reinterpret_cast<const uint8_t*>(bytes.constData() + offset), size));
        }
        app.processEvents();
    };
    for (int i = 0; i < 5; ++i) feed();
    QElapsedTimer clock;
    clock.start();
    for (int i = 0; i < iterations; ++i) feed();
    const double milliseconds = double(clock.nsecsElapsed()) / 1e6;
    QJsonObject result{{"scenario", "1080x1920 RGBA receive + 540x960 presentation"},
                       {"frames", iterations}, {"elapsed_ms", milliseconds},
                       {"ms_per_frame", milliseconds / iterations},
                       {"device_connected", false}};
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
    return frames == iterations + 5 ? 0 : 1;
}
