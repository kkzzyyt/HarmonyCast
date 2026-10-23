import socket
import struct
import sys
import threading
import time
from fractions import Fraction
from pathlib import Path

import av
import numpy as np
import pytest

from harmonycast.backend import DeviceTools, HiLogSession, NativeSession, StreamSession
from harmonycast.core import Device, HDC, HDCError, find_hdc, resource_dir, data_dir, validate_endpoint


def encoded_h264():
    encoder = av.CodecContext.create("libx264", "w")
    encoder.width, encoder.height = 64, 64
    encoder.pix_fmt = "yuv420p"
    encoder.time_base = Fraction(1, 30)
    encoder.framerate = Fraction(30, 1)
    encoder.options = {"preset": "ultrafast", "tune": "zerolatency"}
    rgb = np.zeros((64, 64, 3), dtype=np.uint8)
    rgb[:, :, 1] = 180
    packets = encoder.encode(av.VideoFrame.from_ndarray(rgb, format="rgb24"))
    raw = b"".join(bytes(packet) for packet in packets)
    # The encoder emits Annex-B parameter sets and a real IDR frame.
    import re
    nals = [part for part in re.split(b"\x00\x00\x00?\x01", raw) if part]
    sps = next(nal for nal in nals if nal[0] & 31 == 7)
    pps = next(nal for nal in nals if nal[0] & 31 == 8)
    return sps, pps, raw


class SessionHDC:
    def __init__(self):
        self.calls = []

    def run(self, args, device=None, timeout=8):
        self.calls.append((args, device))
        return "installed" if args[0] == "shell" and "test -x" in args[1] else ""

    def command(self, args, device=None):
        return [sys.executable, "-u", "-c", "import time; print('HARMONYCAST_PID=12345'); time.sleep(30)"]


@pytest.mark.parametrize("native", [False, True])
def test_actual_h264_stream_handshake_decode_and_cleanup(monkeypatch, native):
    sps, pps, keyframe = encoded_h264()
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    listener.settimeout(5)
    port = listener.getsockname()[1]
    server_errors = []
    acknowledgments = []
    def serve():
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(5)
                connection.sendall(b"SCREEN_INFO:64:64:30:1500000:h264\n")
                acknowledgments.append(connection.recv(4096))
                if native:
                    payload = struct.pack(">IIIIII", 64, 64, 30, 1500000, 1080, 1920)
                    connection.sendall(struct.pack(">II", 5, len(payload)) + payload)
                packets = b"".join(struct.pack(">II", kind, len(data)) + data
                                   for kind, data in [(1, sps), (2, pps), (0, b""), (3, keyframe)])
                connection.sendall(packets[:11])
                connection.sendall(packets[11:])
                while connection.recv(4096):
                    pass
        except Exception as error:
            server_errors.append(error)
        finally:
            listener.close()
    server_thread = threading.Thread(target=serve, daemon=True)
    server_thread.start()
    hdc = SessionHDC()
    session_type = NativeSession if native else StreamSession
    session = session_type(hdc, Device("SECOND", architecture="armv7l"))
    monkeypatch.setattr(session, "_free_port", lambda: port)
    frames = []
    statuses = []
    def frame_received(frame):
        frames.append(frame)
        session.stop()
    session.run(frame_received, statuses.append)
    server_thread.join(timeout=5)
    assert not server_thread.is_alive()
    assert server_errors == []
    assert len(acknowledgments) == 1 and acknowledgments[0].startswith(b"CONFIG_ACK\n")
    assert len(frames) == 1 and frames[0].shape == (64, 64, 3)
    assert frames[0][:, :, 1].mean() > 150
    assert session.process.poll() is not None
    assert (["shell", "kill", "12345"], "SECOND") in hdc.calls
    assert (["fport", "rm", f"tcp:{port}", f"tcp:{port}"], "SECOND") in hdc.calls
    assert session.decoder.codec_ctx is None
    if native:
        assert session.device_size == (1080, 1920)


def test_stream_eof_and_corrupt_configuration_fail_cleanly():
    for payload in [b"", b"not a config\n"]:
        client, server = socket.socketpair()
        session = StreamSession(SessionHDC(), Device("ABC"))
        session.socket = client
        server.sendall(payload)
        server.close()
        with pytest.raises((HDCError, ValueError)):
            session._receive(lambda frame: None, lambda text: None)
        session.cleanup()


def test_stream_start_canceled_does_not_create_a_forward(monkeypatch):
    hdc = SessionHDC()
    session = StreamSession(hdc, Device("ABC"))
    session.stop()
    session.run(lambda frame: None, lambda text: None)
    assert session.forwarded is False
    assert hdc.calls == []


def test_stream_handles_server_process_exiting_without_video(monkeypatch):
    hdc = SessionHDC()
    hdc.command = lambda args, device=None: [sys.executable, "-c", "raise SystemExit(1)"]
    session = StreamSession(hdc, Device("ABC"))
    monkeypatch.setattr(socket, "create_connection", lambda *args, **kwargs: (_ for _ in ()).throw(ConnectionRefusedError()))
    with pytest.raises(HDCError, match="启动失败"):
        session.run(lambda frame: None, lambda text: None)
    assert session.forwarded is False


def test_deployment_and_preinstalled_server_checks(tmp_path):
    hdc = SessionHDC()
    session = StreamSession(hdc, Device("ABC", architecture="armv7l"))
    assert session.installed()
    session.deploy()
    assert hdc.calls[1][0] == ["target", "mount"]
    assert all(device == "ABC" for args, device in hdc.calls)
    custom = tmp_path / "custom server"
    custom.write_bytes(b"server")
    session.custom_server = str(custom)
    assert session.selected_server() == custom
    custom.unlink()
    with pytest.raises(ValueError, match="不存在"):
        session.selected_server()


def test_hilog_collects_output_and_stops_owned_process(tmp_path):
    fake = tmp_path / "hdc"
    fake.write_text("#!/bin/sh\nprintf '10-14 12:00:00 I hello\\n'\nexec sleep 30\n")
    fake.chmod(0o755)
    received = threading.Event()
    lines, exits = [], []
    def on_line(line):
        lines.append(line)
        received.set()
    session = HiLogSession(HDC(fake), "ABC", on_line, exits.append)
    session.start()
    assert received.wait(timeout=3)
    session.stop()
    assert lines == ["10-14 12:00:00 I hello"]
    assert session.process.poll() is not None
    assert not session.thread.is_alive()
    assert exits == []


def test_hilog_reports_unexpected_exit_and_missing_hdc(tmp_path):
    for missing in [False, True]:
        fake = tmp_path / ("missing" if missing else "hdc")
        if not missing:
            fake.write_text("#!/bin/sh\nprintf '[Fail] bad device\\n'\nexit 2\n")
            fake.chmod(0o755)
        done = threading.Event()
        lines, exits = [], []
        def on_exit(code):
            exits.append(code)
            done.set()
        session = HiLogSession(HDC(fake), "ABC", lines.append, on_exit)
        session.start()
        assert done.wait(timeout=3)
        session.stop()
        assert len(lines) == 1 and lines[0].startswith("[Fail]")
        assert exits == ([-1] if missing else [2])


def test_hilog_canceled_before_start_never_launches_process(tmp_path):
    session = HiLogSession(HDC(tmp_path / "missing"), "ABC", lambda line: None, lambda code: None)
    session.stop()
    session.start()
    session.thread.join(timeout=1)
    assert session.process is None


@pytest.mark.parametrize("category,expected", [("app", "app"), ("system", "core,init,only_prerelease"), ("", "")])
def test_hilog_category_filters_are_passed_to_the_device(tmp_path, category, expected):
    fake = tmp_path / "hdc"
    fake.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\"\n")
    fake.chmod(0o755)
    done = threading.Event()
    lines = []
    session = HiLogSession(HDC(fake), "SELECTED", lines.append, lambda code: done.set(), category=category)
    session.start()
    assert done.wait(timeout=3)
    session.stop()
    assert lines[:5] == ["-t", "SELECTED", "shell", "hilog", "-v"]
    assert lines[5:] == (["time", "-t", expected] if expected else ["time"])


def test_hilog_category_rejects_invalid_values_before_launch(tmp_path):
    with pytest.raises(ValueError):
        HiLogSession(HDC(tmp_path / "missing"), "ABC", lambda line: None, lambda code: None, category="app; echo bad")


def test_gestures_and_failed_snapshot_validation(tmp_path):
    hdc = SessionHDC()
    tools = DeviceTools(hdc)
    tools.gesture("ABC", (10, 20), (10, 20))
    assert hdc.calls[-1][0] == ["shell", "uinput", "-T", "-c", "10", "20"]
    tools.gesture("ABC", (10, 20), (10, 20), click_interval=10)
    assert hdc.calls[-1][0] == ["shell", "uinput", "-T", "-c", "10", "20", "10"]
    tools.gesture("ABC", (10, 20), (50, 80), 250)
    assert hdc.calls[-1][0] == ["shell", "uinput", "-T", "-m", "10", "20", "50", "80", "250"]
    with pytest.raises(HDCError, match="未生成"):
        tools.capture("ABC")


def test_hdc_info_properties_network_connection_and_discovery(tmp_path, monkeypatch):
    fake = tmp_path / "hdc"
    fake.write_text("#!/bin/sh\ncase \"$*\" in\n*'list targets') echo ABC;;\n*'const.product.model') echo Test;;\n*'const.product.manufacturer') echo HUAWEI;;\n*'const.product.software.version') echo 6.1;;\n*'uname -m') echo aarch64;;\n*'tconn'*) echo connected;;\nesac\n")
    fake.chmod(0o755)
    hdc = HDC(fake)
    assert hdc.devices() == ["ABC"]
    assert hdc.info("ABC") == Device("ABC", "Test", "HUAWEI", "6.1", "aarch64")
    assert hdc.connect("192.168.1.20:5555") == "connected"
    assert find_hdc(str(fake)) == fake
    with pytest.raises(HDCError):
        find_hdc(str(tmp_path / "missing"))
    monkeypatch.setattr("harmonycast.core.shutil.which", lambda command: str(fake))
    assert find_hdc().is_file()
    assert resource_dir().is_dir()
    monkeypatch.setattr(Path, "home", lambda: tmp_path)
    assert data_dir().is_dir()
    with pytest.raises(ValueError):
        validate_endpoint("host..local")


def test_native_stream_preserves_device_coordinates_and_cleans_temporary_service(monkeypatch):
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    listener.settimeout(3)
    port = listener.getsockname()[1]
    pixels = b"\x11\x22\x33\xff" * 4
    packet = struct.pack(">4sIIIIII", b"HCR1", 2, 2, 8, len(pixels), 1080, 1920) + pixels
    errors = []
    def serve():
        try:
            connection, _ = listener.accept()
            with connection:
                connection.sendall(packet[:7])
                connection.sendall(packet[7:])
                connection.recv(1)
        except Exception as error:
            errors.append(error)
        finally:
            listener.close()
    server = threading.Thread(target=serve, daemon=True)
    server.start()
    hdc = SessionHDC()
    session = NativeSession(hdc, Device("ABC", architecture="riscv64"))
    monkeypatch.setattr(session, "_free_port", lambda: port)
    frames = []
    def receive(*frame):
        frames.append(frame)
        session.stop()
    session.run(receive, lambda state: None)
    server.join(timeout=3)
    assert errors == []
    assert frames == [(2, 2, 8, 1080, 1920, pixels)]
    assert not session.uploaded and not session.forwarded
    assert (["shell", "rm", "-f", session.remote_executable], "ABC") in hdc.calls
    assert session.configured


def test_native_capture_availability_requires_matching_architecture():
    assert NativeSession.available(Device("ABC", architecture="riscv64"))
    assert not NativeSession.available(Device("ABC", architecture="aarch64"))
