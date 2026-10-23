import subprocess
from pathlib import Path

import pytest

from harmonycast.core import (
    DEFAULT_NETWORK_PORT, HDC, HDCError, LogStore, delete_saved_device,
    load_saved_devices, map_coordinates, parse_devices, save_device,
    server_variant, validate_endpoint, validated_server_logs, hilog_category,
)


def test_parse_devices_ignores_empty_diagnostics_and_duplicates():
    output = "[Empty]\n[W][2026-10-14] warning\n0123456789ABCDEF\n192.168.1.20:5555\n0123456789ABCDEF\n"
    assert parse_devices(output) == ["0123456789ABCDEF", "192.168.1.20:5555"]
    assert parse_devices("Unknown operation command...\n[Fail] device not found") == []


def test_hdc_preserves_arguments_on_macos_and_paths_with_spaces(tmp_path):
    executable = tmp_path / "fake hdc"
    executable.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\"\n")
    executable.chmod(0o755)
    hdc = HDC(executable)
    assert hdc.run(["file", "recv", "/data/local/tmp/test.log", "a b.log"], "ABC") == (
        "-t\nABC\nfile\nrecv\n/data/local/tmp/test.log\na b.log"
    )


@pytest.mark.parametrize("output", ["[Fail]Device not found", "Unknown operation command...", "error: permission denied"])
def test_hdc_rejects_error_text_even_if_exit_code_is_zero(tmp_path, output):
    executable = tmp_path / "hdc"
    executable.write_text(f"#!/bin/sh\nprintf '%s' '{output}'\n")
    executable.chmod(0o755)
    with pytest.raises(HDCError):
        HDC(executable).run(["list", "targets"])


def test_hdc_handles_missing_binary_and_timeouts(tmp_path):
    with pytest.raises(HDCError):
        HDC(tmp_path / "missing").run(["list", "targets"])
    executable = tmp_path / "hdc"
    executable.write_text("#!/bin/sh\nexec sleep 5\n")
    executable.chmod(0o755)
    with pytest.raises(HDCError, match="超时"):
        HDC(executable).run(["list", "targets"], timeout=0.05)


@pytest.mark.parametrize("value", ["192.168.1.20:5555", "device.local:8710", "10.0.0.2"])
def test_valid_network_addresses(value):
    assert validate_endpoint(value)


@pytest.mark.parametrize("value", ["x; touch /tmp/unsafe", "host:0", "host:65536", "--help", "256.1.1.1", "a b", "host:abc", ""])
def test_invalid_network_addresses(value):
    with pytest.raises(ValueError):
        validate_endpoint(value)


def test_validate_endpoint_default_port():
    assert validate_endpoint("192.168.1.100", default_port=DEFAULT_NETWORK_PORT) == "192.168.1.100:8710"
    assert validate_endpoint("192.168.1.100:5555", default_port=DEFAULT_NETWORK_PORT) == "192.168.1.100:5555"
    assert validate_endpoint("device.local", default_port=8710) == "device.local:8710"


def test_saved_devices_crud(tmp_path, monkeypatch):
    monkeypatch.setattr("harmonycast.core.data_dir", lambda: tmp_path)
    # 初始状态为空
    assert load_saved_devices() == []

    # 保存第一个设备
    devices = save_device("192.168.1.101", name="1床监控屏")
    assert len(devices) == 1
    assert devices[0]["ip"] == "192.168.1.101"
    assert devices[0]["name"] == "1床监控屏"
    assert devices[0]["port"] == 8710

    # 重复保存同一 IP 更新名称
    devices = save_device("192.168.1.101", name="1床主屏")
    assert len(devices) == 1
    assert devices[0]["name"] == "1床主屏"

    # 保存第二个设备，端口含在输入中
    devices = save_device("192.168.1.102:8710", name="2床分机")
    assert len(devices) == 2
    assert devices[0]["ip"] == "192.168.1.102"
    assert devices[0]["name"] == "2床分机"
    assert devices[0]["port"] == 8710

    # 验证从文件持久化读取
    loaded = load_saved_devices()
    assert len(loaded) == 2
    assert loaded[0]["ip"] == "192.168.1.102"

    # 删除设备
    remained = delete_saved_device("192.168.1.101")
    assert len(remained) == 1
    assert remained[0]["ip"] == "192.168.1.102"
    assert len(load_saved_devices()) == 1

    # 无效 IP 抛出异常且不保存
    with pytest.raises(ValueError):
        save_device("999.999.1.1")
    assert len(load_saved_devices()) == 1


def test_hdc_disconnect_and_reconnect(tmp_path):
    executable = tmp_path / "fake_hdc"
    executable.write_text("#!/bin/sh\ncase \"$*\" in\n*'-remove') echo \"Remove target success\";;\n*'tconn'*) echo \"Connect OK\";;\nesac\n")
    executable.chmod(0o755)
    hdc = HDC(executable)

    res_disconn = hdc.disconnect("192.168.1.100")
    assert "Remove target success" in res_disconn

    res_reconn = hdc.reconnect("192.168.1.100")
    assert "Connect OK" in res_reconn


def test_hdc_disconnect_ignores_not_found(tmp_path):
    executable = tmp_path / "fake_hdc_err"
    executable.write_text("#!/bin/sh\necho \"[Fail]No target available\"\n")
    executable.chmod(0o755)
    hdc = HDC(executable)
    assert hdc.disconnect("192.168.1.100") == "disconnected"


def test_coordinate_mapping_ignores_letterboxing_and_clamps_edges():
    # A portrait device occupies the middle 300 pixels of an 800x600 view.
    assert map_coordinates(250, 0, 800, 600, 1080, 2160) == (0, 0)
    assert map_coordinates(400, 300, 800, 600, 1080, 2160) == (540, 1080)
    assert map_coordinates(550, 600, 800, 600, 1080, 2160) == (1079, 2159)
    assert map_coordinates(10, 300, 800, 600, 1080, 2160) is None
    assert map_coordinates(0, 0, 0, 0, 1080, 2160) is None


def test_server_selection_handles_architecture_and_vendor():
    assert server_variant("aarch64", "HUAWEI", "6.1") == "harmonyos/ohscrcpy_server_6.1"
    assert server_variant("aarch64", "HUAWEI", "5.0.1") == "harmonyos/ohscrcpy_server_5.0.1"
    assert server_variant("armv7l", "OpenHarmony", "5.0") == "rk3568/ohscrcpy_server"
    with pytest.raises(ValueError, match="riscv64"):
        server_variant("riscv64", "OpenHarmony", "6.0")
    with pytest.raises(ValueError, match="ARM64"):
        server_variant("aarch64", "unknown", "6.0")


def test_server_log_paths_are_validated_before_download():
    text = "/data/local/tmp/server_123_20261014_120000.log\n/data/local/tmp/../../passwd\n/data/local/tmp/server_2_bad;evil.log\n[Fail] failed\n"
    assert validated_server_logs(text) == ["/data/local/tmp/server_123_20261014_120000.log"]


def test_log_filtering_and_exports_are_plain_text_and_bounded(tmp_path):
    store = LogStore(limit=3)
    store.append("INFO", "设备", "connected")
    store.append("ERROR", "HiLog", "error message")
    store.append("WARN", "投屏", "warning")
    store.append("DEBUG", "设备", "debug")
    assert len(store.entries) == 3
    assert [item.message for item in store.filtered("error", "ERROR")] == ["error message"]
    assert [item.message for item in store.filtered(source="投屏")] == ["warning"]
    destination = tmp_path / "export.log"
    store.export(destination, level="WARN")
    assert "warning" in destination.read_text()
    assert "debug" not in destination.read_text()


@pytest.mark.parametrize("prefix,expected", [("A", "app"), ("C", "system"), ("I", "system"), ("P", "system"), ("K", "system")])
def test_hilog_categories_use_the_typed_header(prefix, expected):
    line = f"10-14 12:34:56.123 123 456 E {prefix}0001a/TestTag: user message"
    assert hilog_category(line) == expected
    assert hilog_category("\x1b[31m" + line + "\x1b[0m") == expected


def test_hilog_category_does_not_guess_from_message_text_or_tag():
    assert hilog_category("user text I A00000/FakeTag: not a log header") == ""
    assert hilog_category("10-14 12:34:56.123 123 456 I 00000/OldTag: no type") == ""
    assert hilog_category("01-01 08:03:33.586 1724 1724 I A0001a/SystemUI_Default: started") == "app"


def test_log_category_combines_with_search_level_source_and_export(tmp_path):
    store = LogStore()
    app = "10-14 12:34:56.123 123 456 E A00001/MyApp: needle app error"
    system = "10-14 12:34:56.124 321 654 E C00001/SystemService: needle system error"
    store.append("ERROR", "HiLog", app)
    store.append("ERROR", "HiLog", system)
    store.append("INFO", "HiLog", "10-14 12:34:56.125 123 456 I A00001/MyApp: needle info")
    store.append("ERROR", "工具", app)
    assert [entry.message for entry in store.filtered("needle", "ERROR", "HiLog", "app")] == [app]
    assert [entry.message for entry in store.filtered(category="system")] == [system]
    assert len(store.filtered()) == 4
    destination = tmp_path / "application.log"
    store.export(destination, "needle", "ERROR", "HiLog", "app")
    assert "app error" in destination.read_text()
    assert "system error" not in destination.read_text()
    assert "needle info" not in destination.read_text()


def test_hilog_streamlined_format_and_device_timestamp():
    store = LogStore()
    raw = "10-23 11:24:45.913  1162  1162 D A06666/MVP_RealClientProcessor: MVP_PROTOCOL#[MVP_RealClientProcessor] 收到CallInfo消息 [connectAfterLost: false]，回调业务..."
    entry_no_bundle = store.append("DEBUG", "HiLog", raw)
    assert entry_no_bundle.timestamp == "11:24:45.913"
    assert entry_no_bundle.level == "DEBUG"
    assert entry_no_bundle.text() == "11:24:45.913  DEBUG  [MVP_RealClientProcessor] MVP_PROTOCOL#[MVP_RealClientProcessor] 收到CallInfo消息 [connectAfterLost: false]，回调业务..."

    entry_bundle = store.append("DEBUG", "HiLog", raw, bundle="com.yarward.etnroomoh")
    assert entry_bundle.text() == "11:24:45.913  DEBUG  [com.yarward.etnroomoh]  [MVP_RealClientProcessor] MVP_PROTOCOL#[MVP_RealClientProcessor] 收到CallInfo消息 [connectAfterLost: false]，回调业务..."


def test_make_hilog_entry_and_log_store_append_entry():
    from harmonycast.core import make_hilog_entry
    raw = "10-23 11:24:45.913  1162  1162 D A06666/MVP_RealClientProcessor: test msg"
    entry = make_hilog_entry(raw, bundle="com.yarward.demo")
    assert entry.timestamp == "11:24:45.913"
    assert entry.level == "DEBUG"
    assert entry.category == "app"
    assert entry.bundle == "com.yarward.demo"
    assert "test msg" in entry.text()

    store = LogStore()
    store.append_entry(entry)
    assert len(store.entries) == 1
    assert store.entries[0] is entry
    assert len(store.filtered(keyword="test", category="app")) == 1

