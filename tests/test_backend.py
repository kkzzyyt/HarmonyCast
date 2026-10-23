from pathlib import Path

import pytest

from harmonycast.backend import DeviceTools, StreamSession
from harmonycast.core import Device, HDCError


class FakeHDC:
    def __init__(self):
        self.calls = []
        self.fail_transfer = False

    def run(self, args, device=None, timeout=8):
        self.calls.append((args, device))
        if args[:3] == ["shell", "ls", "-1"]:
            return "server_123_20261014_120000.log\nother.log\nserver_evil.log"
        if args[:2] == ["file", "recv"]:
            if self.fail_transfer:
                raise HDCError("transfer failed")
            Path(args[3]).write_bytes(b"test content")
        return ""


def test_capture_uses_selected_device_and_cleans_only_own_file():
    hdc = FakeHDC()
    assert DeviceTools(hdc).capture("SECOND") == b"test content"
    assert all(device == "SECOND" for _, device in hdc.calls)
    remote = hdc.calls[0][0][-1]
    assert remote.startswith("/data/local/tmp/harmonycast_")
    assert hdc.calls[-1][0] == ["shell", "rm", "-f", remote]


def test_failed_capture_still_cleans_up():
    hdc = FakeHDC()
    hdc.fail_transfer = True
    with pytest.raises(HDCError):
        DeviceTools(hdc).capture("ABC")
    assert hdc.calls[-1][0][:3] == ["shell", "rm", "-f"]


def test_fetch_logs_validates_filenames_and_never_deletes_remote_logs(tmp_path):
    hdc = FakeHDC()
    files = DeviceTools(hdc).fetch_server_logs("ABC", tmp_path)
    assert len(files) == 1
    assert files[0].read_bytes() == b"test content"
    assert not any("rm" in args for args, _ in hdc.calls)
    assert all(device == "ABC" for _, device in hdc.calls)


def test_failed_download_does_not_leave_a_fake_success_file(tmp_path):
    hdc = FakeHDC()
    hdc.fail_transfer = True
    with pytest.raises(HDCError):
        DeviceTools(hdc).fetch_server_logs("ABC", tmp_path)
    assert not list(tmp_path.rglob("*.log"))


def test_unsupported_device_is_rejected_before_installation():
    hdc = FakeHDC()
    session = StreamSession(hdc, Device("ABC", architecture="riscv64"))
    with pytest.raises(ValueError, match="riscv64"):
        session.deploy()
    assert hdc.calls == []


def test_key_actions_use_known_keycodes_and_reject_unknown():
    hdc = FakeHDC()
    tools = DeviceTools(hdc)
    tools.key("ABC", "back")
    assert hdc.calls[-1] == (["shell", "uinput", "-K", "-d", "2", "-u", "2"], "ABC")
    tools.key("ABC", "home")
    assert "com.ohos.launcher" in hdc.calls[-1][0][1]
    with pytest.raises(ValueError):
        tools.key("ABC", "evil")


def test_stream_session_and_native_session_support_max_size_scaling():
    from harmonycast.backend import NativeSession
    hdc = FakeHDC()
    dev = Device("ABC", architecture="riscv64")
    native_720 = NativeSession(hdc, dev, fps=30, bitrate=4_000_000, max_size=720)
    assert "-m 720" in native_720.server_command()

    native_1080 = NativeSession(hdc, dev, fps=30, bitrate=4_000_000, max_size=1080)
    assert "-m 1080" in native_1080.server_command()

    native_native = NativeSession(hdc, dev, fps=30, bitrate=4_000_000, max_size=0)
    assert "-m" not in native_native.server_command()

    stream_720 = StreamSession(hdc, dev, fps=30, bitrate=4_000_000, max_size=720)
    assert "-m 720" in stream_720.server_command()

    stream_unscaled = StreamSession(hdc, dev, fps=30, bitrate=4_000_000, max_size=0)
    assert "-m" not in stream_unscaled.server_command()


def test_export_fault_bundle_generates_complete_zip(tmp_path):
    import zipfile
    hdc = FakeHDC()
    tools = DeviceTools(hdc)
    zip_dest = tmp_path / "fault_report.zip"

    result = tools.export_fault_bundle(
        serial="ABC12345",
        destination=zip_dest,
        screenshot_bytes=b"fake-png-bytes",
        log_entries=["12:00:00.000 I HiLog Test line 1", "12:00:01.000 E HiLog Error line 2"],
    )
    assert result == zip_dest
    assert zip_dest.exists()

    with zipfile.ZipFile(zip_dest, "r") as zf:
        names = zf.namelist()
        assert "REPORT_SUMMARY.md" in names
        assert "screenshot.png" in names
        assert "device_info.json" in names
        assert "system_parameters.txt" in names
        assert "running_processes.txt" in names
        assert "network_ifconfig.txt" in names
        assert "kernel_dmesg.log" in names
        assert "hilog_harmonycast.log" in names

        # Verify screenshot bytes
        assert zf.read("screenshot.png") == b"fake-png-bytes"

        # Verify log content
        log_content = zf.read("hilog_harmonycast.log").decode("utf-8")
        assert "HiLog Test line 1" in log_content
        assert "HiLog Error line 2" in log_content

        # Verify summary report markdown
        summary_text = zf.read("REPORT_SUMMARY.md").decode("utf-8")
        assert "# HarmonyCast 故障现场诊断报告" in summary_text
        assert "ABC12345" in summary_text
        assert "诊断包内容说明" in summary_text


def test_format_bytes():
    from harmonycast.backend import format_bytes
    assert format_bytes(500) == "500 B"
    assert format_bytes(1024) == "1.0 KB"
    assert format_bytes(1024 * 1024) == "1.0 MB"
    assert format_bytes(1024 * 1024 * 1024 * 2) == "2.00 GB"


def test_get_icu_info():
    class ICUFakeHDC:
        def __init__(self, output):
            self.output = output
            self.calls = []

        def run(self, args, device=None, timeout=8):
            self.calls.append((args, device))
            return self.output

    # 1. Available ICU with stopped watchdog
    out_stopped = "model:H36M\nversion:v1.9\nhw_version:v1.0\nstopwatchdog:1\nstopdelay:0\n"
    tools = DeviceTools(ICUFakeHDC(out_stopped))
    icu = tools.get_icu_info("TEST1")
    assert icu["available"] is True
    assert icu["model"] == "H36M"
    assert icu["mcu_version"] == "v1.9"
    assert icu["watchdog_stopped"] is True
    assert icu["watchdog_desc"] == "已暂停 (调试防复位)"
    assert icu["power_delay"] == "0"

    # 2. Available ICU with active watchdog
    out_active = "model:H36M\nversion:v1.9\nhw_version:v1.0\nstopwatchdog:0\nstopdelay:5\n"
    tools = DeviceTools(ICUFakeHDC(out_active))
    icu = tools.get_icu_info("TEST1")
    assert icu["available"] is True
    assert icu["watchdog_stopped"] is False
    assert icu["watchdog_desc"] == "运行中 (超时将复位)"
    assert icu["power_delay"] == "5"

    # 3. Non-ICU device
    out_none = "model:\nversion:\nhw_version:\nstopwatchdog:\nstopdelay:\n"
    tools = DeviceTools(ICUFakeHDC(out_none))
    icu = tools.get_icu_info("TEST1")
    assert icu["available"] is False
    assert icu["watchdog_stopped"] is None


def test_set_watchdog_status_and_feed():
    class CmdHDC:
        def __init__(self):
            self.calls = []

        def run(self, args, device=None, timeout=8):
            self.calls.append((args, device))
            return "ok"

    hdc = CmdHDC()
    tools = DeviceTools(hdc)

    tools.set_watchdog_status("DEV1", True)
    assert hdc.calls[-1] == (["shell", "echo -n 1 > /sys/class/ICU/version/StopWatchDog"], "DEV1")

    tools.set_watchdog_status("DEV1", False)
    assert hdc.calls[-1] == (["shell", "echo -n 0 > /sys/class/ICU/version/StopWatchDog"], "DEV1")

    tools.feed_watchdog("DEV1")
    assert hdc.calls[-1] == (["shell", "echo -n 1 > /sys/class/ICU/version/FeedWatchdog"], "DEV1")


def test_read_remote_file_bytes_gzip():
    import base64
    import gzip

    original_data = b"Hello OpenHarmony Medical Terminal Logs" * 50
    compressed_b64 = base64.b64encode(gzip.compress(original_data)).decode("ascii")

    class ReadHDC:
        def run(self, args, device=None, timeout=8):
            return compressed_b64

    tools = DeviceTools(ReadHDC())
    read_data = tools.read_remote_file_bytes("DEV1", "/data/app/test.log")
    assert read_data == original_data


def test_list_app_logs_and_fetch(tmp_path):
    import base64
    import gzip

    log_file_content = b"2026-10-15 12:00:00 [INFO] SmartWard app running"
    compressed_b64 = base64.b64encode(gzip.compress(log_file_content)).decode("ascii")

    ls_output = (
        "-rw-rw-r-- 1 20010046 20010046 4084262 2026-10-15 13:15 /data/app/el2/100/base/com.yarward.ohsmartward/haps/etnroom/files/log/2026-10-15.log\n"
        "-rw------- 1 20010046 20010046 17670 2026-10-15 13:15 /data/app/el2/100/base/com.yarward.ohsmartward/files/hiappevent/app_event_20261015.log\n"
    )

    class LogHDC:
        def run(self, args, device=None, timeout=8):
            cmd = args[1] if len(args) > 1 else ""
            if "ls -ld" in cmd:
                return ls_output
            if "gzip" in cmd:
                return compressed_b64
            return ""

    tools = DeviceTools(LogHDC())
    logs = tools.list_app_logs("DEV1", "com.yarward.*")
    assert len(logs) == 2
    assert logs[0]["name"] == "2026-10-15.log"
    assert logs[0]["bundle"] == "com.yarward.ohsmartward"
    assert logs[0]["size"] == 4084262
    assert logs[0]["category"] == "log"

    assert logs[1]["name"] == "app_event_20261015.log"
    assert logs[1]["category"] == "hiappevent"

    # Fetch app logs to tmp_path
    dest = tmp_path / "app_logs"
    fetched = tools.fetch_app_logs("DEV1", dest, "com.yarward.*")
    assert len(fetched) == 2
    assert any("etnroom_2026-10-15.log" == p.name for p in fetched)
    assert any("hiappevent_app_event_20261015.log" == p.name for p in fetched)
    for p in fetched:
        assert p.read_bytes() == log_file_content


def test_device_uptime_formatting():
    class MockHDC:
        def __init__(self, uptime_val):
            self.uptime_val = uptime_val
        def run(self, args, serial, timeout=6):
            if "param dump" in args[1]:
                return ""
            return f"5.10.0 aarch64\n---UPTIME---\n{self.uptime_val}\n---MEMINFO---\n---DF---\n---NET---\n---IFACES---\n"

    # > 24 hours (120,000 seconds = 1 day, 9 hours, 20 mins, 0 secs)
    tools = DeviceTools(MockHDC("120000.00 240000.00"))
    details = tools.get_device_details("SERIAL_1")
    assert details["uptime"] == "1 天 9 小时 20 分钟 0 秒"

    # < 24 hours (3661 seconds = 1 hour, 1 min, 1 sec)
    tools = DeviceTools(MockHDC("3661.00 7000.00"))
    details = tools.get_device_details("SERIAL_2")
    assert details["uptime"] == "1 小时 1 分钟 1 秒"
