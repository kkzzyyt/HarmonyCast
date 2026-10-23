import struct

import pytest

from harmonycast.protocol import PacketParser, RawFrameParser, parse_screen_info


def test_fragmented_and_coalesced_packets_are_not_lost():
    parser = PacketParser()
    data = struct.pack(">II", 1, 3) + b"sps" + struct.pack(">II", 0, 0)
    assert parser.feed(data[:5]) == []
    assert parser.feed(data[5:10]) == []
    assert parser.feed(data[10:]) == [(1, b"sps"), (0, b"")]


@pytest.mark.parametrize("header", [struct.pack(">II", 9, 0), struct.pack(">II", 1, 30_000_000)])
def test_invalid_headers_are_rejected_without_unbounded_buffers(header):
    with pytest.raises(ValueError):
        PacketParser().feed(header)


def test_handshake_parses_h264_and_h265():
    assert parse_screen_info(b"SCREEN_INFO:1080:1920:30:1500000:h264\n").codec == "h264"
    assert parse_screen_info(b"SCREEN_INFO:1080:1920:30:1500000:h265\n").width == 1080


@pytest.mark.parametrize("line", [b"garbage", b"SCREEN_INFO:0:1920:30:1:h264", b"SCREEN_INFO:1:1:30:1:bad", b"SCREEN_INFO:99999:1:30:1:h264"])
def test_bad_handshake_fails_with_useful_error(line):
    with pytest.raises(ValueError, match="配置"):
        parse_screen_info(line)


def test_raw_frame_parser_handles_fragmentation_and_strided_rgba():
    parser = RawFrameParser()
    pixels = b"\x40\x80\xff\xff" * 8
    packet = struct.pack(">4sIIIIII", b"HCR1", 2, 2, 16, len(pixels), 4, 4) + pixels
    assert parser.feed(packet[:10]) == []
    assert parser.feed(packet[10:30]) == []
    assert parser.feed(packet[30:]) == [(2, 2, 16, 4, 4, pixels)]


@pytest.mark.parametrize("width,height,stride,length", [(0, 2, 8, 16), (8192, 2, 32768, 65536), (2, 2, 4, 8), (2, 2, 8, 12), (4096, 4096, 16384, 67108864)])
def test_raw_frame_parser_rejects_invalid_size_before_buffering(width, height, stride, length):
    with pytest.raises(ValueError):
        RawFrameParser().feed(struct.pack(">4sIIIIII", b"HCR1", width, height, stride, length, 4, 4))


def test_native_rgba_rotation_metadata_survives_fragmented_headers():
    parser = RawFrameParser()
    pixels = bytes(32)
    packet = struct.pack(">4sIIIIIII", b"HCR2", 2, 2, 16, 32, 4, 4, 1) + pixels
    assert parser.feed(packet[:30]) == []
    assert parser.feed(packet[30:]) == [(2, 2, 16, 4, 4, pixels, 1)]


def test_packet_parser_compacts_large_offset_with_partial_packet():
    parser = PacketParser()
    data = (struct.pack(">II", 1, 1024) + b"x" * 1024) * 70 + struct.pack(">II", 1, 10) + b"part"
    packets = parser.feed(data)
    assert len(packets) == 70
    assert parser.offset == 0
    assert len(parser.buffer) == 12


def test_raw_frame_parser_compacts_buffer_and_rejects_bad_rotation():
    parser = RawFrameParser()
    pixels = bytes(32)
    packet = struct.pack(">4sIIIIIII", b"HCR2", 2, 2, 16, 32, 4, 4, 5) + pixels
    with pytest.raises(ValueError, match="旋转"):
        parser.feed(packet)

    parser2 = RawFrameParser()
    width, height = 512, 513
    stride = width * 4
    length = stride * height
    big_pixels = bytes(length)
    big_packet = struct.pack(">4sIIIIII", b"HCR1", width, height, stride, length, width, height) + big_pixels + struct.pack(">4sIIIIII", b"HCR1", 2, 2, 8, 16, 2, 2)
    frames = parser2.feed(big_packet)
    assert len(frames) == 1
    assert parser2.offset == 0
    assert len(parser2.buffer) == 28
