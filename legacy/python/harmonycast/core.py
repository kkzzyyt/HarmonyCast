from __future__ import annotations

import ipaddress
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path


class HDCError(RuntimeError):
    """An HDC command failed, including failures returned with exit status zero."""


DEFAULT_NETWORK_PORT = 8710


def saved_devices_file() -> Path:
    return data_dir() / "saved_devices.json"


def load_saved_devices() -> list[dict]:
    path = saved_devices_file()
    if not path.is_file():
        return []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(data, list):
            valid = []
            for item in data:
                if isinstance(item, dict) and "ip" in item:
                    ip = str(item["ip"]).strip()
                    if ip:
                        valid.append({
                            "ip": ip,
                            "name": str(item.get("name", "")).strip(),
                            "port": int(item.get("port", DEFAULT_NETWORK_PORT)),
                            "updated_at": item.get("updated_at", 0),
                        })
            return valid
    except Exception:
        pass
    return []


def save_device(ip: str, name: str = "", port: int = DEFAULT_NETWORK_PORT) -> list[dict]:
    ip = ip.strip()
    if ":" in ip:
        host_part, port_part = ip.rsplit(":", 1)
        if port_part.isdigit():
            port = int(port_part)
            ip = host_part
    validate_endpoint(f"{ip}:{port}")
    devices = load_saved_devices()
    devices = [d for d in devices if d["ip"] != ip]
    devices.insert(0, {
        "ip": ip,
        "name": name.strip(),
        "port": port,
        "updated_at": int(time.time()),
    })
    path = saved_devices_file()
    path.write_text(json.dumps(devices, ensure_ascii=False, indent=2), encoding="utf-8")
    return devices


def delete_saved_device(ip: str) -> list[dict]:
    ip = ip.strip()
    if ":" in ip:
        ip = ip.rsplit(":", 1)[0]
    devices = [d for d in load_saved_devices() if d["ip"] != ip]
    path = saved_devices_file()
    path.write_text(json.dumps(devices, ensure_ascii=False, indent=2), encoding="utf-8")
    return devices


def resource_dir() -> Path:
    return Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent.parent)) / "resources"


def data_dir() -> Path:
    directory = Path.home() / "Library/Application Support/HarmonyCast"
    directory.mkdir(parents=True, exist_ok=True)
    return directory


def find_hdc(custom: str = "") -> Path:
    if custom:
        selected = Path(custom).expanduser()
        if selected.is_file() and os.access(selected, os.X_OK):
            return selected
        raise HDCError("指定的 HDC 文件不存在或没有执行权限")
    candidates = [
        Path("/Applications/DevEco-Studio6.app/Contents/sdk/default/openharmony/toolchains/hdc"),
        Path("/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"),
    ]
    candidates.extend(sorted((Path.home() / "Library/OpenHarmony/Sdk").glob("*/toolchains/hdc"), reverse=True))
    candidates.append(resource_dir() / "hdc/hdc")
    if shutil.which("hdc"):
        candidates.append(Path(shutil.which("hdc")))
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise HDCError("未找到 HDC。请在设置中选择 DevEco Studio 的 hdc 工具")


_FAILURE = re.compile(
    r"(?im)(?:^\[Fail\]|unknown operation command|^error:|permission denied|"
    r"no such file or directory|failed to (?:connect|send|recv)|device not found)"
)


class HDC:
    def __init__(self, executable: Path | str):
        self.executable = Path(executable)

    def command(self, args: list[str], device: str | None = None) -> list[str]:
        return [str(self.executable), *(["-t", device] if device else []), *map(str, args)]

    def run(self, args: list[str], device: str | None = None, timeout: float = 8.0, _retry: int = 0) -> str:
        try:
            result = subprocess.run(
                self.command(args, device), shell=False, capture_output=True,
                text=True, encoding="utf-8", errors="replace", timeout=timeout,
            )
        except subprocess.TimeoutExpired as error:
            raise HDCError("HDC 命令超时，请检查设备连接") from error
        except OSError as error:
            raise HDCError(f"无法启动 HDC：{error}") from error
        combined = result.stdout + "\n" + result.stderr
        if result.returncode != 0 or _FAILURE.search(combined):
            if _retry < 2 and ("E000004" in combined or "communication channel is being established" in combined):
                time.sleep(0.5 * (_retry + 1))
                return self.run(args, device, timeout=timeout, _retry=_retry + 1)
            raise HDCError(combined.strip() or f"HDC 退出码：{result.returncode}")
        return result.stdout.strip()

    def devices(self) -> list[str]:
        try:
            return parse_devices(self.run(["list", "targets"]))
        except HDCError:
            return []

    def info(self, serial: str) -> Device:
        cmd = (
            "param get const.product.model 2>/dev/null; echo __HCAST__; "
            "param get const.product.manufacturer 2>/dev/null; echo __HCAST__; "
            "param get const.product.software.version 2>/dev/null; echo __HCAST__; "
            "uname -m 2>/dev/null"
        )
        try:
            output = self.run(["shell", cmd], serial, timeout=4)
            if "__HCAST__" in output:
                parts = output.split("__HCAST__")
                if len(parts) >= 4:
                    model = parts[0].strip() or "OpenHarmony 设备"
                    vendor = parts[1].strip()
                    version = parts[2].strip()
                    architecture = parts[3].strip()
                    return Device(serial, model, vendor, version, architecture)
        except HDCError:
            pass

        def property_value(name: str) -> str:
            try:
                return self.run(["shell", "param", "get", name], serial, timeout=3)
            except HDCError:
                return ""

        model = property_value("const.product.model") or "OpenHarmony 设备"
        vendor = property_value("const.product.manufacturer")
        version = property_value("const.product.software.version")
        architecture = self.run(["shell", "uname", "-m"], serial, timeout=3)
        return Device(serial, model, vendor, version, architecture)

    def connect(self, address: str) -> str:
        return self.run(["tconn", validate_endpoint(address, default_port=DEFAULT_NETWORK_PORT)], timeout=12)

    def disconnect(self, address: str) -> str:
        try:
            return self.run(["tconn", validate_endpoint(address, default_port=DEFAULT_NETWORK_PORT), "-remove"], timeout=8)
        except HDCError as error:
            msg = str(error).lower()
            if "no target available" in msg or "not found" in msg or "unknown" in msg:
                return "disconnected"
            raise

    def reconnect(self, address: str) -> str:
        try:
            self.disconnect(address)
        except Exception:
            pass
        time.sleep(0.4)
        return self.connect(address)

    def applications(self, serial: str) -> list[str]:
        return parse_applications(self.run(["shell", "bm", "dump", "-a"], serial))

    def application(self, serial: str, bundle: str) -> Application:
        validate_bundle_name(bundle)
        output = self.run(["shell", "bm", "dump", "-n", bundle], serial, timeout=4)
        return parse_application(output, bundle)

    def application_pids(self, serial: str, application: Application) -> tuple[int, ...]:
        output = self.run(["shell", "ps", "-A", "-o", "PID,UID,ARGS"], serial, timeout=3)
        return parse_application_pids(output, application)

    def application_processes(self, serial: str, applications: list[Application]) -> dict[int, str]:
        output = self.run(["shell", "ps", "-A", "-o", "PID,UID,ARGS"], serial, timeout=3)
        return {pid: application.bundle for application in applications
                for pid in parse_application_pids(output, application)}


@dataclass(frozen=True)
class Device:
    serial: str
    model: str = "OpenHarmony 设备"
    vendor: str = ""
    version: str = ""
    architecture: str = ""


_BUNDLE_NAME = re.compile(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)+")


def validate_bundle_name(bundle: str) -> str:
    if len(bundle) > 128 or _BUNDLE_NAME.fullmatch(bundle) is None:
        raise ValueError("请输入完整应用包名，例如 com.example.app")
    return bundle


def validate_package_filter(value: str) -> str:
    validate_bundle_name(value[:-2] if value.endswith(".*") else value)
    return value


def package_matches(bundle: str, package_filter: str) -> bool:
    if package_filter.endswith(".*"):
        return bundle.startswith(package_filter[:-1])
    return not package_filter or bundle == package_filter


def parse_applications(output: str) -> list[str]:
    return sorted({line.strip() for line in output.splitlines() if _BUNDLE_NAME.fullmatch(line.strip())})


@dataclass(frozen=True)
class Application:
    bundle: str
    process_names: tuple[str, ...]
    uids: tuple[int, ...] = ()


def parse_application(output: str, bundle: str) -> Application:
    try:
        data = json.loads(output[output.index("{"):])
        info = data.get("applicationInfo", {})
        if data.get("name") != bundle and info.get("bundleName") != bundle:
            raise ValueError("应用包名不匹配")
        process_names = {bundle}
        uids = set()
        records = [data, info, *data.get("userInfo", [])]
        for module in data.get("hapModuleInfos", []):
            records.extend([module, *module.get("abilityInfos", []), *module.get("extensionInfos", [])])
        for record in records:
            process = record.get("process", "")
            if isinstance(process, str) and re.fullmatch(r"[A-Za-z0-9_.:-]{1,256}", process):
                process_names.add(process)
            uid = record.get("uid")
            if type(uid) is int and uid >= 0:
                uids.add(uid)
        return Application(bundle, tuple(sorted(process_names)), tuple(sorted(uids)))
    except (ValueError, TypeError, AttributeError) as error:
        raise HDCError(f"无法读取应用 {bundle}，请确认它已安装且设备允许查询应用信息") from error


def parse_application_pids(output: str, application: Application) -> tuple[int, ...]:
    result = set()
    for line in output.splitlines():
        match = re.match(r"^\s*(\d+)\s+(\d+)\s+(\S+)(?:\s|$)", line)
        if not match:
            continue
        pid, uid, process = int(match[1]), int(match[2]), match[3]
        name_matches = any(process == name or process.startswith(name + ":") for name in application.process_names)
        if pid > 0 and name_matches and (not application.uids or uid in application.uids):
            result.add(pid)
    return tuple(sorted(result))


def parse_devices(output: str) -> list[str]:
    result = []
    for line in output.splitlines():
        serial = line.strip()
        if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.:-]{2,127}", serial) and serial not in result:
            if serial.lower() not in {"empty", "unknown", "none"}:
                result.append(serial)
    return result


def validate_endpoint(value: str, default_port: int | None = None) -> str:
    value = value.strip()
    match = re.fullmatch(r"([A-Za-z0-9][A-Za-z0-9.-]*)(?::([0-9]{1,5}))?", value)
    if not match:
        raise ValueError("请输入设备 IP 或主机名，例如 192.168.1.20:8710")
    host, port = match.groups()
    if port is not None and not 1 <= int(port) <= 65535:
        raise ValueError("端口必须在 1–65535 之间")
    if re.fullmatch(r"[0-9.]+", host):
        try:
            ipaddress.IPv4Address(host)
        except ipaddress.AddressValueError as error:
            raise ValueError("IP 地址格式不正确") from error
    elif any(not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label) for label in host.split(".")):
        raise ValueError("主机名格式不正确")
    if port is None and default_port is not None:
        return f"{host}:{default_port}"
    return value


def server_variant(architecture: str, vendor: str, version: str) -> str:
    if architecture.startswith("armv") or architecture == "arm":
        return "rk3568/ohscrcpy_server"
    if architecture in {"aarch64", "arm64"}:
        if vendor.upper() not in {"HUAWEI", "HONOR"}:
            raise ValueError("该 ARM64 开发设备需要匹配系统版本的服务端；请在设置中选择自定义服务端，或使用兼容预览")
        for release in ("6.1", "6.0", "5.0.1"):
            if release in version:
                return f"harmonyos/ohscrcpy_server_{release}"
        return "harmonyos/ohscrcpy_server"
    raise ValueError(f"内置流畅投屏服务暂不支持 {architecture}，请使用兼容预览，或选择对应架构的自定义服务端")


def validated_server_logs(output: str) -> list[str]:
    return list(dict.fromkeys(
        line.strip() for line in output.splitlines()
        if re.fullmatch(r"/data/local/tmp/server_[0-9]+_[0-9]{8}_[0-9]{6}\.log", line.strip())
    ))


def map_coordinates(x: float, y: float, view_width: int, view_height: int, width: int, height: int) -> tuple[int, int] | None:
    if min(view_width, view_height, width, height) <= 0:
        return None
    scale = min(view_width / width, view_height / height)
    left, top = (view_width - width * scale) / 2, (view_height - height * scale) / 2
    if not (left - 2 <= x <= left + width * scale + 2 and top - 2 <= y <= top + height * scale + 2):
        return None
    clamped_x = max(left, min(left + width * scale, x))
    clamped_y = max(top, min(top + height * scale, y))
    return min(width - 1, int((clamped_x - left) / scale)), min(height - 1, int((clamped_y - top) / scale))


_HILOG_HEADER = re.compile(
    r"^\s*(?:\d{4}-)?\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}(?:\.\d{1,9})?"
    r"\s+(?P<pid>\d+)\s+\d+\s+[DIWEF]\s+(?P<type>[ACIKP])[0-9a-fA-F]{5,8}/"
)

_HILOG_LINE_RE = re.compile(
    r"^\s*(?:(?:\d{4}-)?(?P<date>\d{2}-\d{2})\s+)?(?P<time>\d{2}:\d{2}:\d{2}(?:\.\d{1,9})?)\s+"
    r"(?P<pid>\d+)\s+(?P<tid>\d+)\s+"
    r"(?P<level>[DIWEF])\s+"
    r"(?:(?P<domain>[ACIKP0-9a-fA-F]{3,10})/)?\s*"
    r"(?P<tag>[^:\s][^:]*?):\s*"
    r"(?P<msg>.*)$"
)

_HILOG_LEVELS = {"D": "DEBUG", "I": "INFO", "W": "WARN", "E": "ERROR", "F": "FATAL"}


def hilog_category(message: str) -> str:
    """Classify the HiLog type in its header, independent of tag/message text."""
    if "\x1b" in message:
        message = re.sub(r"\x1b\[[0-9;]*m", "", message)
    match = _HILOG_HEADER.match(message)
    if match is None:
        return ""
    return "app" if match.group("type") == "A" else "system"


def hilog_pid(message: str) -> int | None:
    if "\x1b" in message:
        message = re.sub(r"\x1b\[[0-9;]*m", "", message)
    match = _HILOG_HEADER.match(message)
    return int(match.group("pid")) if match else None


@dataclass(frozen=True)
class LogEntry:
    timestamp: str
    level: str
    source: str
    message: str
    category: str = ""
    bundle: str = ""
    _text: str = ""
    _text_folded: str = ""

    def __post_init__(self):
        if not self._text:
            clean = re.sub(r"\x1b\[[0-9;]*m", "", self.message) if "\x1b" in self.message else self.message
            m = _HILOG_LINE_RE.match(clean) if self.source == "HiLog" else None
            if m:
                bundle_label = f"  [{self.bundle}]" if self.bundle else ""
                text = f"{self.timestamp}  {self.level:<5}{bundle_label}  [{m.group('tag')}] {m.group('msg')}"
            else:
                bundle_label = f" [{self.bundle}]" if self.bundle else ""
                text = f"{self.timestamp}  {self.level:<5}  [{self.source}]{bundle_label} {self.message}"
            object.__setattr__(self, "_text", text)
        if not self._text_folded:
            object.__setattr__(self, "_text_folded", f"{self._text} {self.bundle} {self.message}".casefold())

    def text(self) -> str:
        return self._text

    def matches(self, keyword: str = "", level: str = "", source: str = "", category: str = "", bundle: str = "") -> bool:
        return ((not level or self.level == level)
                and (not source or self.source == source)
                and (not category or self.category == category)
                and package_matches(self.bundle, bundle)
                and (not keyword or keyword.casefold() in self._text_folded))


def make_hilog_entry(message: str, bundle: str = "", default_level: str = "INFO") -> LogEntry:
    clean = re.sub(r"\x1b\[[0-9;]*m", "", message) if "\x1b" in message else message
    m = _HILOG_LINE_RE.match(clean)
    if m:
        timestamp = m.group("time")[:12]
        level = _HILOG_LEVELS.get(m.group("level"), default_level)
        domain = m.group("domain") or ""
        category = "app" if (domain and domain.startswith("A")) else "system"
        bundle_label = f"  [{bundle}]" if bundle else ""
        text = f"{timestamp}  {level:<5}{bundle_label}  [{m.group('tag')}] {m.group('msg')}"
    else:
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        match = re.search(r"\s([DIWEF])\s", clean)
        level = {"D": "DEBUG", "I": "INFO", "W": "WARN", "E": "ERROR", "F": "FATAL"}.get(match.group(1) if match else "", default_level)
        if clean.startswith("[Fail]"):
            level = "ERROR"
        category = hilog_category(clean)
        bundle_label = f" [{bundle}]" if bundle else ""
        text = f"{timestamp}  {level:<5}  [HiLog]{bundle_label} {message}"
    text_folded = f"{text} {bundle} {message}".casefold()
    return LogEntry(timestamp, level, "HiLog", message, category=category, bundle=bundle, _text=text, _text_folded=text_folded)


class LogStore:
    def __init__(self, limit: int = 10000):
        self.entries: deque[LogEntry] = deque(maxlen=limit)

    def append_entry(self, entry: LogEntry) -> LogEntry:
        self.entries.append(entry)
        return entry

    def append(self, level: str, source: str, message: str, bundle: str = "") -> LogEntry:
        if source == "HiLog":
            entry = make_hilog_entry(message, bundle=bundle, default_level=level)
        else:
            timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            clean = re.sub(r"\x1b\[[0-9;]*m", "", message) if "\x1b" in message else message
            bundle_label = f" [{bundle}]" if bundle else ""
            text = f"{timestamp}  {level:<5}  [{source}]{bundle_label} {clean}"
            text_folded = f"{text} {bundle} {clean}".casefold()
            entry = LogEntry(timestamp, level, source, clean, "", bundle, _text=text, _text_folded=text_folded)
        self.entries.append(entry)
        return entry

    def filtered(self, keyword: str = "", level: str = "", source: str = "", category: str = "", bundle: str = "") -> list[LogEntry]:
        return [entry for entry in self.entries if entry.matches(keyword, level, source, category, bundle)]

    def export(self, destination: Path, keyword: str = "", level: str = "", source: str = "", category: str = "", bundle: str = "") -> None:
        destination.write_text("\n".join(entry.text() for entry in self.filtered(keyword, level, source, category, bundle)) + "\n", encoding="utf-8")
