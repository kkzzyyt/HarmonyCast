import json
import sys
import threading
from pathlib import Path

import pytest

from harmonycast.backend import HiLogSession
from harmonycast.core import (
    Application, HDC, HDCError, LogStore, hilog_pid, parse_applications,
    parse_application, parse_application_pids, validate_bundle_name,
    package_matches, validate_package_filter,
)


def test_installed_packages_exclude_headers_and_duplicate_users():
    output = "ID: 100:\n  com.example.alpha\n com.ohos.UserFile.ExternalFileManager\nID: 101:\n com.example.alpha\n[Fail]failed\n"
    assert parse_applications(output) == ["com.example.alpha", "com.ohos.UserFile.ExternalFileManager"]


@pytest.mark.parametrize("bundle", ["com.example.app", "com.ohos.UserFile.ExternalFileManager", "org.example.app_2"])
def test_bundle_names_accept_real_identifiers(bundle):
    assert validate_bundle_name(bundle) == bundle


@pytest.mark.parametrize("bundle", ["", "com.example.app;echo bad", "--help", "com.example app", "com..example", "com/example/app"])
def test_bundle_names_reject_command_fragments(bundle):
    with pytest.raises(ValueError):
        validate_bundle_name(bundle)


def test_package_prefix_filter_has_a_namespace_boundary_and_preserves_exact_selection(tmp_path):
    assert validate_package_filter("com.yarward.*") == "com.yarward.*"
    assert package_matches("com.yarward.ohsmartward", "com.yarward.*")
    assert package_matches("com.yarward.other", "com.yarward.*")
    assert not package_matches("com.yarwardevil.app", "com.yarward.*")
    assert not package_matches("com.other.app", "com.yarward.*")
    assert not package_matches("com.yarward.ohsmartward.extra", "com.yarward.ohsmartward")
    store = LogStore()
    for bundle in ("com.yarward.ohsmartward", "com.yarward.other", "com.yarwardevil.app"):
        store.append("INFO", "HiLog", "10-14 12:00:00.123 123 456 I A00001/Tag: event", bundle)
    store.export(tmp_path / "prefix.log", category="app", bundle="com.yarward.*")
    exported = (tmp_path / "prefix.log").read_text()
    assert "com.yarward.ohsmartward" in exported and "com.yarward.other" in exported
    assert "com.yarwardevil" not in exported
    assert len(store.filtered(bundle="com.yarward.ohsmartward")) == 1


@pytest.mark.parametrize("value", ["*", "com.*.app", "com.yarward*", "com.yarward.*;echo bad"])
def test_package_prefix_rejects_ambiguous_or_unsafe_filters(value):
    with pytest.raises(ValueError):
        validate_package_filter(value)


def application_metadata():
    return {"name": "com.example.app", "uid": 20010001,
            "applicationInfo": {"bundleName": "com.example.app", "process": "main.custom", "uid": 20010001},
            "hapModuleInfos": [{"process": "module.custom", "abilityInfos": [{"process": "ability.custom"}],
                                "extensionInfos": [{"process": "extension.custom"}]}],
            "userInfo": [{"uid": 20210001}]}


def test_package_metadata_and_processes_match_names_uid_and_subprocesses():
    application = parse_application("com.example.app:\n" + json.dumps(application_metadata()), "com.example.app")
    processes = """PID UID ARGS
11 20010001 com.example.app
12 20010001 com.example.app:worker --child
13 20010001 main.custom
14 20010001 module.custom
15 20010001 ability.custom
16 20010001 extension.custom
17 20210001 com.example.app
18 20010002 com.example.app
19 20010001 com.example.app.other
20 0 sh -c echo com.example.app
21 20010001 unrelated
"""
    assert parse_application_pids(processes, application) == (11, 12, 13, 14, 15, 16, 17)


def test_multiple_packages_resolve_their_real_names_in_a_single_process_query():
    class ProcessHDC(HDC):
        def __init__(self):
            self.calls = []
        def run(self, args, serial, timeout):
            self.calls.append((args, serial))
            return "PID UID ARGS\n101 20010001 com.yarward.alpha\n102 20010001 com.yarward.alpha:worker\n202 20010002 custom.process\n999 20010003 com.yarward.alpha\n"
    hdc = ProcessHDC()
    applications = [Application("com.yarward.alpha", ("com.yarward.alpha",), (20010001,)),
                    Application("com.yarward.beta", ("custom.process",), (20010002,))]
    assert hdc.application_processes("DEVICE", applications) == {101: "com.yarward.alpha", 102: "com.yarward.alpha", 202: "com.yarward.beta"}
    assert hdc.calls == [(["shell", "ps", "-A", "-o", "PID,UID,ARGS"], "DEVICE")]


def test_invalid_or_mismatched_package_info_fails_closed():
    for text in ["bundle not found", "{}", json.dumps({"name": "com.other.app"})]:
        with pytest.raises(HDCError):
            parse_application(text, "com.example.app")


def test_bundle_filter_does_not_match_other_apps_or_message_mentions(tmp_path):
    store = LogStore()
    first = "10-14 12:00:00.123 123 456 I A00001/Tag: message"
    other = "10-14 12:00:00.123 321 654 I A00001/Tag: com.example.app mentioned"
    assert hilog_pid(first) == 123
    assert hilog_pid("fake pid 123 I A00001/Tag: body") is None
    store.append("INFO", "HiLog", first, bundle="com.example.app")
    store.append("INFO", "HiLog", other, bundle="com.other.app")
    store.append("INFO", "HiLog", first)
    assert len(store.filtered(category="app", bundle="com.example.app")) == 1
    store.export(tmp_path / "app.log", category="app", bundle="com.example.app")
    assert "mentioned" not in (tmp_path / "app.log").read_text()


class FakePackageHDC:
    def __init__(self, executable, pid_groups):
        self.executable = executable
        self.pid_groups = iter(pid_groups)
        self.last = ()
        self.commands = []
    def application(self, serial, bundle):
        return Application(bundle, (bundle,), (20010001,))
    def application_pids(self, serial, application):
        self.last = next(self.pid_groups, self.last)
        return self.last
    def command(self, args, device=None):
        self.commands.append(args)
        return [sys.executable, str(self.executable), *args]


def make_log_producer(tmp_path):
    executable = tmp_path / "produce.py"
    executable.write_text("""import sys, time
args = sys.argv
pids = args[args.index('-P') + 1].split(',') if '-P' in args else ['101', '102', '103', '104', '105', '106', '999']
for pid in pids:
    print(f'10-14 12:00:00.123 {pid} {pid} I A00001/Tag: event {pid}', flush=True)
time.sleep(30)
""")
    return executable


def test_package_capture_waits_for_start_and_tracks_restarted_process(tmp_path):
    hdc = FakePackageHDC(make_log_producer(tmp_path), [(), (101,), (202,)])
    restarted = threading.Event()
    lines, statuses = [], []
    def receive(line):
        lines.append(line)
        if hilog_pid(line) == 202:
            restarted.set()
    session = HiLogSession(hdc, "DEVICE", receive, lambda code: None, category="app",
                          bundle="com.example.app", on_status=statuses.append)
    session.poll_interval = 0.05
    session.start()
    try:
        assert restarted.wait(timeout=4)
    finally:
        session.stop()
    assert {hilog_pid(line) for line in lines} == {101, 202}
    assert any("等待应用启动" in status for status in statuses)
    assert [args[-2:] for args in hdc.commands] == [["-P", "101"], ["-P", "202"]]
    assert not session.thread.is_alive()
    assert session.process.poll() is not None


def test_more_than_five_app_processes_are_filtered_without_dropping_any(tmp_path):
    hdc = FakePackageHDC(make_log_producer(tmp_path), [(101, 102, 103, 104, 105, 106)])
    complete = threading.Event()
    lines = []
    def receive(line):
        lines.append(line)
        if len(lines) == 6:
            complete.set()
    session = HiLogSession(hdc, "DEVICE", receive, lambda code: None, category="app", bundle="com.example.app")
    session.start()
    try:
        assert complete.wait(timeout=4)
    finally:
        session.stop()
    assert {hilog_pid(line) for line in lines} == {101, 102, 103, 104, 105, 106}
    assert "-P" not in hdc.commands[0]


def test_package_monitoring_can_stop_before_an_app_starts(tmp_path):
    hdc = FakePackageHDC(make_log_producer(tmp_path), [()])
    waiting = threading.Event()
    session = HiLogSession(hdc, "DEVICE", lambda line: None, lambda code: None,
                          category="app", bundle="com.example.app", on_status=lambda status: waiting.set())
    session.start()
    assert waiting.wait(timeout=2)
    session.stop()
    assert hdc.commands == [] and not session.thread.is_alive()


class FakePrefixHDC(FakePackageHDC):
    def __init__(self, executable, groups):
        super().__init__(executable, groups)
        self.installed = ["com.yarward.alpha", "com.yarward.beta", "com.other.app", "com.yarwardevil.app"]
        self.queried = []

    def applications(self, serial):
        return self.installed

    def application(self, serial, bundle):
        self.queried.append(bundle)
        return Application(bundle, (bundle,), (20010001,))

    def application_processes(self, serial, applications):
        self.last = next(self.pid_groups, self.last)
        allowed = {application.bundle for application in applications}
        return {pid: bundle for pid, bundle in self.last.items() if bundle in allowed}


def test_prefix_capture_labels_each_app_and_tracks_newly_installed_packages(tmp_path):
    hdc = FakePrefixHDC(make_log_producer(tmp_path), [{101: "com.yarward.alpha"}, {101: "com.yarward.alpha", 202: "com.yarward.beta"}])
    hdc.installed = []
    collected, waiting = threading.Event(), threading.Event()
    entries, statuses = [], []
    def receive(line, bundle):
        entries.append((hilog_pid(line), bundle))
        if (202, "com.yarward.beta") in entries:
            collected.set()
    def status(message):
        statuses.append(message)
        if "匹配应用" in message:
            waiting.set()
    session = HiLogSession(hdc, "DEVICE", lambda line: None, lambda code: None, category="app",
                          bundle="com.yarward.*", on_status=status, on_package_line=receive)
    session.poll_interval = 0.05
    session.package_refresh_interval = 0.05
    session.start()
    try:
        assert waiting.wait(timeout=2)
        hdc.installed = ["com.yarward.alpha", "com.yarward.beta", "com.other.app", "com.yarwardevil.app"]
        assert collected.wait(timeout=4)
    finally:
        session.stop()
    assert set(entries) == {(101, "com.yarward.alpha"), (202, "com.yarward.beta")}
    assert set(hdc.queried) == {"com.yarward.alpha", "com.yarward.beta"}
    assert not session.thread.is_alive()
