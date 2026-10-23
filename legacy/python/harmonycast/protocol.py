"""Bounded parsing for the OpenHarmony_Scrcpy v2.3 TCP protocol."""

import struct

from .upstream.config import VideoStreamConfig


class PacketParser:
    def __init__(self):
        self.buffer = bytearray()
        self.offset = 0

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        self.buffer.extend(data)
        packets = []
        buf = self.buffer
        offset = self.offset
        buf_len = len(buf)
        while buf_len - offset >= 8:
            packet_type, size = struct.unpack_from(">II", buf, offset)
            if packet_type > 8 or size > 10 * 1024 * 1024:
                buf.clear()
                self.offset = 0
                raise ValueError("投屏数据包类型或长度无效")
            if buf_len - offset < 8 + size:
                break
            payload_start = offset + 8
            payload_end = payload_start + size
            packets.append((packet_type, bytes(buf[payload_start:payload_end])))
            offset = payload_end
        if offset > 0:
            if offset == buf_len:
                buf.clear()
                self.offset = 0
            elif offset > 65536:
                del buf[:offset]
                self.offset = 0
            else:
                self.offset = offset
        return packets


class RawFrameParser:
    def __init__(self):
        self.buffer = bytearray()
        self.offset = 0

    def feed(self, data: bytes) -> list[tuple]:
        self.buffer.extend(data)
        frames = []
        buf = self.buffer
        offset = self.offset
        buf_len = len(buf)
        while buf_len - offset >= 28:
            magic, width, height, stride, length, source_width, source_height = struct.unpack_from(">4sIIIIII", buf, offset)
            header_size = 32 if magic == b"HCR2" else 28
            if (magic not in {b"HCR1", b"HCR2"} or not 1 <= width <= 4096 or not 1 <= height <= 4096
                    or not width * 4 <= stride <= width * 4 + 4096
                    or length != stride * height or length > 16 * 1024 * 1024
                    or not 1 <= source_width <= 8192 or not 1 <= source_height <= 8192):
                buf.clear()
                self.offset = 0
                raise ValueError("原生投屏帧格式或长度无效")
            if buf_len - offset < header_size:
                break
            rotation = struct.unpack_from(">I", buf, offset + 28)[0] if header_size == 32 else 0
            if rotation > 3:
                buf.clear()
                self.offset = 0
                raise ValueError("原生投屏旋转方向无效")
            if buf_len - offset < header_size + length:
                break
            pixel_start = offset + header_size
            pixel_end = pixel_start + length
            frame = (width, height, stride, source_width, source_height, bytes(buf[pixel_start:pixel_end]))
            frames.append((*frame, rotation) if header_size == 32 else frame)
            offset = pixel_end
        if offset > 0:
            if offset == buf_len:
                buf.clear()
                self.offset = 0
            elif offset > 1048576:
                del buf[:offset]
                self.offset = 0
            else:
                self.offset = offset
        return frames


def parse_screen_info(line: bytes) -> VideoStreamConfig:
    try:
        prefix, width, height, fps, bitrate, codec = line.decode("ascii").strip().split(":")
        width, height, fps, bitrate = map(int, (width, height, fps, bitrate))
        if prefix != "SCREEN_INFO" or codec not in {"h264", "h265"}:
            raise ValueError
        if not (1 <= width <= 8192 and 1 <= height <= 8192 and 1 <= fps <= 240 and 1 <= bitrate <= 100_000_000):
            raise ValueError
        return VideoStreamConfig(width, height, fps, bitrate, codec)
    except (UnicodeError, ValueError) as error:
        raise ValueError("无法解析设备投屏配置") from error
