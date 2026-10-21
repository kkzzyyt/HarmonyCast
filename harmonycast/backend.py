from __future__ import annotations

import base64
import gzip
import json
import logging
import re
import socket
import struct
import subprocess
import tempfile
import threading
import time
import uuid
import zipfile
from datetime import datetime
from pathlib import Path
from typing import Callable

from .core import Device, HDC, HDCError, hilog_pid, package_matches, resource_dir, server_variant, validate_package_filter, validated_server_logs
from .protocol import PacketParser, RawFrameParser, parse_screen_info
from .upstream.decoder import VideoDecoder

logger = logging.getLogger("HarmonyCast")
def format_bytes(num_bytes: int) -> str:
    if num_bytes < 1024:
        return f"{num_bytes} B"
    elif num_bytes < 1024 * 1024:
        return f"{num_bytes / 1024:.1f} KB"
    elif num_bytes < 1024 * 1024 * 1024:
        return f"{num_bytes / (1024 * 1024):.1f} MB"
    return f"{num_bytes / (1024 * 1024 * 1024):.2f} GB"


class DeviceTools:
    KEYS = {"home": 1, "back": 2, "volume_up": 16, "volume_down": 17, "power": 18, "recent": 2073}

    def __init__(self, hdc: HDC):
        self.hdc = hdc

    def capture(self, serial: str) -> bytes:
        remote = f"/data/local/tmp/harmonycast_{uuid.uuid4().hex}.jpeg"
        try:
            self.hdc.run(["shell", "snapshot_display", "-f", remote], serial, timeout=12)
            with tempfile.TemporaryDirectory(prefix="harmonycast-") as directory:
                local = Path(directory) / "screen.jpeg"
                self.hdc.run(["file", "recv", remote, str(local)], serial, timeout=12)
                if not local.is_file() or local.stat().st_size == 0:
                    raise HDCError("截图未生成，请检查设备的截图权限")
                return local.read_bytes()
        finally:
            try:
                self.hdc.run(["shell", "rm", "-f", remote], serial, timeout=2)
            except HDCError:
                logger.debug("临时截图清理失败：%s", remote)

    def home(self, serial: str) -> str:
        """返回桌面: 深度兼容通用应用、全屏沉浸应用及注册为系统 Home 的定制 Kiosk 业务分机 (如 ohsmartward)"""
        cmd = (
            "sh -c '"
            "aa start -b com.ohos.launcher -a com.ohos.launcher.settings.MainAbility >/dev/null 2>&1; "
            "sleep 0.15; "
            "uinput -K -d 2 -u 2 >/dev/null 2>&1; "
            "uinput -K -d 1 -u 1 >/dev/null 2>&1; "
            "which uitest >/dev/null 2>&1 && uitest uiInput keyEvent Home >/dev/null 2>&1; "
            "echo ok'"
        )
        return self.hdc.run(["shell", cmd], serial, timeout=4)

    def key(self, serial: str, action: str) -> None:
        if action not in self.KEYS:
            raise ValueError("未知设备按键")
        if action == "home":
            self.home(serial)
            return
        keycode = str(self.KEYS[action])
        self.hdc.run(["shell", "uinput", "-K", "-d", keycode, "-u", keycode], serial)

    def gesture(self, serial: str, start: tuple[int, int], end: tuple[int, int], duration: int = 200, click_interval: int | None = None) -> None:
        x1, y1 = map(str, start)
        x2, y2 = map(str, end)
        if start == end:
            args = ["shell", "uinput", "-T", "-c", x1, y1]
            if click_interval is not None:
                args.append(str(click_interval))
        else:
            args = ["shell", "uinput", "-T", "-m", x1, y1, x2, y2, str(duration)]
        logger.info("执行 uinput: %s (设备: %s)", " ".join(args), serial)
        output = self.hdc.run(args, serial)
        if output.strip():
            logger.debug("uinput 输出: %s", output.strip())

    def fetch_server_logs(self, serial: str, destination: Path) -> list[Path]:
        names = self.hdc.run(["shell", "ls", "-1", "/data/local/tmp"], serial)
        remote_files = validated_server_logs("\n".join("/data/local/tmp/" + name for name in names.splitlines()))
        destination.mkdir(parents=True, exist_ok=True)
        downloaded = []
        for remote in remote_files:
            local = destination / Path(remote).name
            temporary = destination / (local.name + ".part")
            try:
                self.hdc.run(["file", "recv", remote, str(temporary)], serial, timeout=30)
                if not temporary.is_file() or not temporary.stat().st_size:
                    raise HDCError(f"服务端日志下载失败：{local.name}")
                temporary.replace(local)
                downloaded.append(local)
            finally:
                temporary.unlink(missing_ok=True)
        return downloaded

    def get_device_details(self, serial: str) -> dict:
        params_raw = ""
        try:
            params_raw = self.hdc.run(["shell", "param dump"], serial, timeout=6)
        except HDCError:
            pass
        params = {}
        for line in params_raw.splitlines():
            line = line.strip()
            if line.startswith("param:"):
                key, _, val = line[6:].strip().partition("=")
                params[key.strip()] = val.strip()

        sys_cmd = (
            'uname -r -m; echo "---UPTIME---"; cat /proc/uptime; '
            'echo "---MEMINFO---"; cat /proc/meminfo; '
            'echo "---DF---"; df -h /data 2>/dev/null; '
            'echo "---NET---"; ifconfig 2>/dev/null; '
            'echo "---IFACES---"; for i in /sys/class/net/*; do [ -e "$i" ] && echo "$(basename "$i"): $(cat "$i/address" 2>/dev/null)"; done'
        )
        try:
            sys_raw = self.hdc.run(["shell", sys_cmd], serial, timeout=6)
        except HDCError:
            sys_raw = ""

        uname_str, uptime_str, mem_str, storage_str, net_str, ifaces_str = "", "", "", "", "", ""
        if "---UPTIME---" in sys_raw:
            parts = sys_raw.split("---UPTIME---")
            uname_str = parts[0].strip()
            if len(parts) > 1:
                p2 = parts[1].split("---MEMINFO---")
                uptime_str = p2[0].strip()
                if len(p2) > 1:
                    p3 = p2[1].split("---DF---")
                    mem_str = p3[0].strip()
                    if len(p3) > 1:
                        p4 = p3[1].split("---NET---")
                        storage_str = p4[0].strip()
                        if len(p4) > 1:
                            p5 = p4[1].split("---IFACES---")
                            net_str = p5[0].strip()
                            if len(p5) > 1:
                                ifaces_str = p5[1].strip()
        else:
            uname_str = sys_raw.strip()

        uptime_formatted = "未知"
        if uptime_str:
            try:
                seconds = float(uptime_str.split()[0])
                hours, rem = divmod(int(seconds), 3600)
                minutes, secs = divmod(rem, 60)
                uptime_formatted = f"{hours} 小时 {minutes} 分钟 {secs} 秒"
            except (ValueError, IndexError):
                pass

        mem_formatted = "未知"
        if mem_str:
            try:
                total_kb, avail_kb = 0, 0
                for line in mem_str.splitlines():
                    if line.startswith("MemTotal:"):
                        total_kb = int(re.search(r"\d+", line).group(0))
                    elif line.startswith("MemAvailable:"):
                        avail_kb = int(re.search(r"\d+", line).group(0))
                if total_kb:
                    used_kb = total_kb - avail_kb
                    mem_formatted = f"已用 {used_kb / (1024*1024):.2f} GB / 总计 {total_kb / (1024*1024):.2f} GB (可用 {avail_kb / 1024:.0f} MB)"
            except Exception:
                pass

        storage_formatted = "未知"
        if storage_str:
            lines = [l for l in storage_str.splitlines() if l.strip()]
            if len(lines) >= 2:
                cols = lines[-1].split()
                if len(cols) >= 5:
                    storage_formatted = f"已用 {cols[2]} / 总计 {cols[1]} (空闲 {cols[3]}，使用率 {cols[4]})"

        mac_map = {}
        for line in ifaces_str.splitlines():
            if ":" in line:
                iface, mac = line.split(":", 1)
                mac_map[iface.strip()] = mac.strip()

        ip_map = {}
        current_iface = None
        for line in net_str.splitlines():
            m_iface = re.match(r"^([a-zA-Z0-9_.-]+)\s+", line)
            if m_iface:
                current_iface = m_iface.group(1)
            if current_iface and "inet addr:" in line:
                m_ip = re.search(r"inet addr:([0-9.]+)", line)
                if m_ip:
                    ip_map[current_iface] = m_ip.group(1)

        networks = []
        for iface, mac in sorted(mac_map.items()):
            if iface == "lo":
                continue
            ip = ip_map.get(iface, "")
            is_eth = "eth" in iface
            is_wlan = "wlan" in iface
            name_label = "有线以太网 (eth)" if is_eth else ("无线局域网 (Wi-Fi)" if is_wlan else f"网络接口 ({iface})")
            networks.append({
                "interface": iface,
                "name": name_label,
                "mac": mac,
                "ip": ip or "未连接 / 未分配 IP",
                "connected": bool(ip),
            })

        all_pkgs_raw = ""
        try:
            all_pkgs_raw = self.hdc.run(["shell", "bm dump -a"], serial, timeout=5)
        except HDCError:
            pass
        packages = [line.strip() for line in all_pkgs_raw.splitlines() if line.strip() and not line.strip().startswith("ID:")]

        running_pids_raw = ""
        try:
            running_pids_raw = self.hdc.run(["shell", "ps -A -o PID,ARGS"], serial, timeout=4)
        except HDCError:
            pass

        yarward_apps = []
        for pkg in packages:
            if "yarward" in pkg.lower():
                app_info = self._get_bundle_detail(serial, pkg, running_pids_raw)
                yarward_apps.append(app_info)

        icu = self.get_icu_info(serial)
        effective_model = params.get("const.product.name") or params.get("const.product.model") or ""
        if icu.get("model"):
            icu_model = icu["model"]
            if not icu_model.startswith("YH-"):
                icu_model = f"YH-{icu_model}"
            if not effective_model or effective_model == "OpenHarmony 设备":
                effective_model = icu_model
            else:
                effective_model = f"{effective_model} ({icu_model})"
        else:
            effective_model = effective_model or "OpenHarmony 设备"

        return {
            "serial": serial,
            "brand": params.get("const.product.brand") or params.get("const.product.manufacturer") or "未知",
            "model": effective_model,
            "model_code": params.get("const.product.model") or "",
            "manufacturer": params.get("const.product.manufacturer") or "未知",
            "hardware_version": params.get("const.product.hardwareversion") or icu.get("hardware_version") or "未知",
            "software_version": params.get("const.product.software.version") or "未知",
            "incremental_version": params.get("const.product.incremental.version") or "",
            "os_fullname": params.get("const.ohos.fullname") or "OpenHarmony",
            "api_version": params.get("const.ohos.apiversion") or "未知",
            "security_patch": params.get("const.ohos.version.security_patch") or "未知",
            "cpu_abi": params.get("const.product.cpu.abilist") or (uname_str.split()[-1] if uname_str else "未知"),
            "udid": params.get("const.product.devUdid") or "未知",
            "kernel": uname_str or "未知",
            "uptime": uptime_formatted,
            "memory": mem_formatted,
            "storage": storage_formatted,
            "display": f"{params.get('const.product.display.width', '1080')} × {params.get('const.product.display.height', '1920')}",
            "networks": networks,
            "yarward_apps": yarward_apps,
            "all_packages": packages,
            "icu": icu,
            "icu_model": icu.get("model", ""),
            "icu_version": icu.get("mcu_version", ""),
            "watchdog_status": icu.get("watchdog_desc", "未知"),
            "watchdog_stopped": icu.get("watchdog_stopped"),
            "power_delay": icu.get("power_delay", ""),
        }

    def _get_bundle_detail(self, serial: str, bundle: str, ps_output: str = "") -> dict:
        v_name, v_code, target_api = "未知", 0, 0
        update_str, install_str = "未知", "未知"
        main_ability = ""
        try:
            dump_out = self.hdc.run(["shell", f"bm dump -n {bundle}"], serial, timeout=4)
            if "{" in dump_out:
                data = json.loads(dump_out[dump_out.index("{"):])
                v_name = data.get("versionName", "")
                v_code = data.get("versionCode", 0)
                target_api = data.get("targetVersion", 0)
                u_info = data.get("userInfo", [{}])[0]
                update_ms = u_info.get("updateTime", 0) or data.get("updateTime", 0)
                install_ms = u_info.get("installTime", 0) or data.get("installTime", 0)
                if update_ms:
                    update_str = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(update_ms / 1000))
                if install_ms:
                    install_str = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(install_ms / 1000))
                for module in data.get("hapModuleInfos", []):
                    if module.get("mainAbility"):
                        main_ability = module.get("mainAbility")
                        break
        except Exception as e:
            logger.debug("读取应用 %s 详情失败: %s", bundle, e)

        pids = []
        if ps_output:
            for line in ps_output.splitlines():
                if bundle in line:
                    parts = line.strip().split()
                    if parts and parts[0].isdigit():
                        pids.append(parts[0])

        return {
            "bundle": bundle,
            "version_name": v_name,
            "version_code": v_code,
            "target_api": target_api,
            "main_ability": main_ability,
            "update_time": update_str,
            "install_time": install_str,
            "running": bool(pids),
            "pids": pids,
        }

    def start_application(self, serial: str, bundle: str, ability: str = "") -> str:
        cmd = ["shell", "aa", "start", "-b", bundle]
        if ability:
            cmd.extend(["-a", ability])
        return self.hdc.run(cmd, serial, timeout=5)

    def stop_application(self, serial: str, bundle: str) -> str:
        return self.hdc.run(["shell", "aa", "force-stop", bundle], serial, timeout=5)

    def get_icu_info(self, serial: str) -> dict:
        """读取 Yarward 医疗终端 ICU / MCU 硬件及看门狗状态"""
        cmd = (
            "sh -c '"
            "echo model:$(cat /sys/class/ICU/version/model 2>/dev/null); "
            "echo version:$(cat /sys/class/ICU/version/Version 2>/dev/null); "
            "echo hw_version:$(cat /sys/class/ICU/version/HardwareVersion 2>/dev/null); "
            "echo stopwatchdog:$(cat /sys/class/ICU/version/StopWatchDog 2>/dev/null); "
            "echo stopdelay:$(cat /sys/class/ICU/version/StopDelay 2>/dev/null)'"
        )
        raw = self.hdc.run(["shell", cmd], serial, timeout=4)
        data = {}
        for line in raw.splitlines():
            if ":" in line:
                k, v = line.split(":", 1)
                data[k.strip()] = v.strip()

        model = data.get("model", "")
        version = data.get("version", "")
        hw_version = data.get("hw_version", "")
        stopwatchdog = data.get("stopwatchdog", "")
        stopdelay = data.get("stopdelay", "")

        has_icu = bool(model or version or stopwatchdog)
        watchdog_stopped = None
        watchdog_desc = "未启用/未知"
        if stopwatchdog == "1":
            watchdog_stopped = True
            watchdog_desc = "已暂停 (调试防复位)"
        elif stopwatchdog == "0":
            watchdog_stopped = False
            watchdog_desc = "运行中 (超时将复位)"

        return {
            "available": has_icu,
            "model": model,
            "mcu_version": version,
            "hardware_version": hw_version,
            "watchdog_stopped": watchdog_stopped,
            "watchdog_desc": watchdog_desc,
            "power_delay": stopdelay,
        }

    def set_watchdog_status(self, serial: str, stop: bool) -> str:
        """设置看门狗状态: stop=True 写入1暂停看门狗(防复位); stop=False 写入0恢复看门狗"""
        val = "1" if stop else "0"
        cmd = f"echo -n {val} > /sys/class/ICU/version/StopWatchDog"
        res = self.hdc.run(["shell", cmd], serial, timeout=3)
        logger.info("设置看门狗状态 serial=%s stop=%s -> %s", serial, stop, res)
        return res

    def feed_watchdog(self, serial: str) -> str:
        """手动喂狗一次"""
        cmd = "echo -n 1 > /sys/class/ICU/version/FeedWatchdog"
        res = self.hdc.run(["shell", cmd], serial, timeout=3)
        logger.info("手动喂狗 serial=%s -> %s", serial, res)
        return res

    def read_remote_file_bytes(self, serial: str, remote_path: str, max_bytes: int = 25 * 1024 * 1024) -> bytes:
        """通过 gzip+base64 高效无损读取远端文件，规避 OpenHarmony hdc file recv 崩溃或0字节问题"""
        try:
            out = self.hdc.run(["shell", f"gzip -c {remote_path} 2>/dev/null | base64"], serial, timeout=15)
            clean = out.strip().replace("\r\n", "").replace("\n", "")
            if clean:
                decoded = base64.b64decode(clean)
                return gzip.decompress(decoded)
        except Exception as e:
            logger.debug("gzip+base64 读取失败 (%s)，回退至纯 base64: %s", remote_path, e)
            try:
                out = self.hdc.run(["shell", f"base64 {remote_path} 2>/dev/null"], serial, timeout=15)
                clean = out.strip().replace("\r\n", "").replace("\n", "")
                if clean:
                    return base64.b64decode(clean)
            except Exception as e2:
                logger.warning("读取远端文件失败: %s (%s)", remote_path, e2)
        return b""

    def list_app_logs(self, serial: str, bundle: str = "com.yarward.*") -> list[dict]:
        """扫描应用持久化日志文件列表 (包含 files/log/*.log 和 files/hiappevent/*.log)"""
        safe_bundle = re.sub(r"[^a-zA-Z0-9_.*-]", "", bundle or "com.yarward.*")
        cmd = (
            f"ls -ld /data/app/el2/100/base/{safe_bundle}/haps/*/files/log/* "
            f"/data/app/el2/100/base/{safe_bundle}/files/log/* "
            f"/data/app/el2/100/base/{safe_bundle}/files/hiappevent/*.log 2>/dev/null"
        )
        raw = self.hdc.run(["shell", cmd], serial, timeout=5)
        logs = []
        for line in raw.splitlines():
            line = line.strip()
            if not line or line.startswith("ls:") or line.startswith("total"):
                continue
            parts = line.split()
            if len(parts) >= 8 and parts[0].startswith("-"):
                try:
                    size = int(parts[4])
                except ValueError:
                    size = 0
                file_path = parts[-1]
                file_name = Path(file_path).name
                m = re.search(r"/data/app/el2/100/base/([^/]+)/", file_path)
                bundle_name = m.group(1) if m else ""
                cat = "hiappevent" if "/hiappevent/" in file_path else "log"
                logs.append({
                    "path": file_path,
                    "name": file_name,
                    "size": size,
                    "size_formatted": format_bytes(size),
                    "mtime": f"{parts[5]} {parts[6]}" if len(parts) >= 7 else "",
                    "bundle": bundle_name,
                    "category": cat,
                })
        return logs

    def fetch_app_logs(self, serial: str, destination_dir: Path | str, bundle: str = "com.yarward.*") -> list[Path]:
        """批量导出业务应用持久化日志到本地目录"""
        dest = Path(destination_dir)
        dest.mkdir(parents=True, exist_ok=True)
        logs = self.list_app_logs(serial, bundle)
        exported = []
        for log_item in logs:
            rpath = log_item["path"]
            parts = rpath.strip("/").split("/")
            prefix = ""
            if "haps" in parts:
                idx = parts.index("haps")
                module = parts[idx + 1] if idx + 1 < len(parts) else ""
                prefix = f"{module}_"
            elif "hiappevent" in parts:
                prefix = "hiappevent_"
            local_name = f"{prefix}{log_item['name']}"
            local_file = dest / local_name
            content = self.read_remote_file_bytes(serial, rpath)
            if content:
                local_file.write_bytes(content)
                exported.append(local_file)
                logger.info("已拉取应用日志: %s -> %s (%d bytes)", rpath, local_file, len(content))
        return exported

    def export_fault_bundle(
        self,
        serial: str,
        destination: Path | str,
        screenshot_bytes: bytes | None = None,
        log_entries: list[str] | None = None,
    ) -> Path:
        dest_path = Path(destination)
        dest_path.parent.mkdir(parents=True, exist_ok=True)
        now_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

        # 1. 现场屏幕截图 (PNG/JPEG)
        if not screenshot_bytes:
            try:
                screenshot_bytes = self.capture(serial)
            except Exception as e:
                logger.warning("导出故障现场时捕获屏幕失败: %s", e)
                screenshot_bytes = b""

        # 2. 设备全量硬件与系统规格
        try:
            device_info = self.get_device_details(serial)
        except Exception as e:
            logger.warning("导出故障现场时读取设备详情失败: %s", e)
            device_info = {"serial": serial, "error": str(e)}

        # 3. OpenHarmony 系统参数 dump
        try:
            params_raw = self.hdc.run(["shell", "param dump"], serial, timeout=6)
        except Exception as e:
            params_raw = f"获取系统参数失败: {e}"

        # 4. 运行中进程快照
        try:
            ps_raw = self.hdc.run(["shell", "ps -A -o PID,PPID,UID,USER,ARGS"], serial, timeout=6)
        except Exception as e:
            try:
                ps_raw = self.hdc.run(["shell", "ps -ef"], serial, timeout=6)
            except Exception as e2:
                ps_raw = f"获取进程列表失败: {e2}"

        # 5. 网络接口详情 (ifconfig)
        try:
            ifconfig_raw = self.hdc.run(["shell", "ifconfig"], serial, timeout=4)
        except Exception as e:
            ifconfig_raw = f"获取网络信息失败: {e}"

        # 6. 内核日志 (dmesg / hilog -K)
        try:
            dmesg_raw = self.hdc.run(["shell", "dmesg"], serial, timeout=4)
        except Exception:
            try:
                dmesg_raw = self.hdc.run(["shell", "hilog", "-K"], serial, timeout=4)
            except Exception as e:
                dmesg_raw = f"获取内核日志失败: {e}"

        # 7. 设备端原始 HiLog 环形缓冲输出
        try:
            hilog_device_raw = self.hdc.run(["shell", "hilog", "-x"], serial, timeout=8)
        except Exception as e:
            hilog_device_raw = f"获取设备端 HiLog 失败: {e}"

        # 8. 投屏服务端临时日志 (/data/local/tmp/harmonycast_*)
        server_logs: dict[str, bytes] = {}
        try:
            with tempfile.TemporaryDirectory(prefix="hc-srv-logs-") as td:
                paths = self.fetch_server_logs(serial, Path(td))
                for p in paths:
                    server_logs[p.name] = p.read_bytes()
        except Exception as e:
            logger.debug("获取投屏服务日志失败: %s", e)

        # 9. 业务应用持久化落盘日志 (/data/app/el2/100/base/com.yarward.*/...)
        app_logs: dict[str, bytes] = {}
        try:
            with tempfile.TemporaryDirectory(prefix="hc-app-logs-") as td:
                paths = self.fetch_app_logs(serial, Path(td), bundle="com.yarward.*")
                for p in paths:
                    app_logs[p.name] = p.read_bytes()
        except Exception as e:
            logger.debug("获取应用磁盘日志失败: %s", e)

        # 10. 构建 Markdown 现场诊断报告
        brand = device_info.get("brand", "未知")
        model = device_info.get("model", "未知")
        os_ver = device_info.get("os_fullname", "未知")
        kernel = device_info.get("kernel", "未知")
        uptime = device_info.get("uptime", "未知")
        memory = device_info.get("memory", "未知")
        storage = device_info.get("storage", "未知")
        display = device_info.get("display", "未知")

        icu_info = device_info.get("icu", {})
        icu_model = icu_info.get("model") or device_info.get("icu_model") or "未检测到 / 无 ICU"
        icu_ver = icu_info.get("mcu_version") or device_info.get("icu_version") or "未知"
        watchdog_desc = icu_info.get("watchdog_desc") or device_info.get("watchdog_status") or "未知"
        power_delay = icu_info.get("power_delay") or device_info.get("power_delay") or "0"

        networks = device_info.get("networks", [])
        if networks:
            net_lines = ["| 接口 | 名称 | IP 地址 | MAC 地址 | 状态 |", "| :--- | :--- | :--- | :--- | :--- |"]
            for net in networks:
                status = "已连接" if net.get("connected") else "未连接"
                net_lines.append(f"| {net.get('interface')} | {net.get('name')} | {net.get('ip')} | {net.get('mac')} | {status} |")
            net_table = "\n".join(net_lines)
        else:
            net_table = "无网络接口信息"

        apps = device_info.get("yarward_apps", [])
        if apps:
            app_lines = ["| 应用包名 | 版本名称 | 版本代码 | 状态 | 进程 PID |", "| :--- | :--- | :--- | :--- | :--- |"]
            for a in apps:
                status = "正在运行" if a.get("running") else "未运行"
                pids_str = ", ".join(map(str, a.get("pids", []))) or "-"
                app_lines.append(f"| {a.get('bundle')} | {a.get('version_name')} | {a.get('version_code')} | {status} | {pids_str} |")
            app_table = "\n".join(app_lines)
        else:
            app_table = "未检测到业务应用或无需分析"

        summary_md = f"""# HarmonyCast 故障现场诊断报告

- **导出时间**：{now_str}
- **设备序列号**：`{serial}`
- **设备型号**：{brand} · {model}
- **操作系统**：{os_ver}
- **内核版本**：`{kernel}`
- **屏幕分辨率**：{display}
- **运行时间**：{uptime}

---

## 亚华医疗 ICU 硬件与看门狗状态
- **ICU 硬件模块**：{icu_model}
- **MCU 固件版本**：{icu_ver}
- **看门狗状态**：{watchdog_desc}
- **关机延时配置**：{power_delay}

---

## 硬件与资源状态
- **内存使用**：{memory}
- **存储空间**：{storage}

## 网络接口与连接
{net_table}

## 业务应用运行状态 (Yarward)
{app_table}

---

## 诊断包内容说明
- `screenshot.png`：故障现场屏幕截图
- `REPORT_SUMMARY.md`：本诊断分析摘要报告
- `device_info.json`：设备全量硬件规格与状态详情 (JSON)
- `hilog_harmonycast.log`：投屏客户端抓取日志
- `hilog_device_recent.log`：设备端原始 HiLog 环形缓冲输出
- `running_processes.txt`：设备当前全部运行进程快照 (PID/UID/ARGS)
- `system_parameters.txt`：全量 OpenHarmony 系统参数 dump
- `network_ifconfig.txt`：网络接口配置 (ifconfig)
- `kernel_dmesg.log`：内核 ring buffer 日志
- `server_logs/`：投屏服务端执行日志
- `app_disk_logs/`：业务应用持久化落盘日志 (含 files/log/*.log 及 HiAppEvent)
"""

        # 11. 写入 ZIP 压缩包
        with zipfile.ZipFile(dest_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("REPORT_SUMMARY.md", summary_md.encode("utf-8"))
            if screenshot_bytes:
                zf.writestr("screenshot.png", screenshot_bytes)
            if log_entries is not None:
                zf.writestr("hilog_harmonycast.log", "\n".join(log_entries).encode("utf-8") + b"\n")
            if hilog_device_raw is not None:
                zf.writestr("hilog_device_recent.log", (hilog_device_raw or "(无 HiLog 数据)").encode("utf-8"))
            if device_info is not None:
                zf.writestr("device_info.json", json.dumps(device_info, ensure_ascii=False, indent=2).encode("utf-8"))
            if params_raw is not None:
                zf.writestr("system_parameters.txt", (params_raw or "(无系统参数输出)").encode("utf-8"))
            if ps_raw is not None:
                zf.writestr("running_processes.txt", (ps_raw or "(无进程列表输出)").encode("utf-8"))
            if ifconfig_raw is not None:
                zf.writestr("network_ifconfig.txt", (ifconfig_raw or "(无网络配置输出)").encode("utf-8"))
            if dmesg_raw is not None:
                zf.writestr("kernel_dmesg.log", (dmesg_raw or "(无内核日志输出)").encode("utf-8"))
            for sname, sbytes in server_logs.items():
                zf.writestr(f"server_logs/{sname}", sbytes)
            for aname, abytes in app_logs.items():
                zf.writestr(f"app_disk_logs/{aname}", abytes)

        logger.info("故障现场诊断包打包完毕: %s (包含 %d 个应用日志)", dest_path, len(app_logs))
        return dest_path


class HiLogSession:
    LOG_TYPES = {"": "", "app": "app", "system": "core,init,only_prerelease"}

    def __init__(self, hdc: HDC, serial: str, on_line: Callable[[str], None], on_exit: Callable[[int], None], category: str = "", bundle: str = "", on_status: Callable[[str], None] | None = None, on_package_line: Callable[[str, str], None] | None = None):
        if category not in self.LOG_TYPES:
            raise ValueError("无效的 HiLog 日志分类")
        self.hdc, self.serial = hdc, serial
        self.category = category
        if bundle:
            validate_package_filter(bundle)
            if category != "app":
                raise ValueError("包名筛选仅适用于应用日志")
        self.bundle = bundle
        self.on_status = on_status or (lambda status: None)
        self.on_line, self.on_exit = on_line, on_exit
        self.on_package_line = on_package_line
        self.process: subprocess.Popen | None = None
        self.reader_thread: threading.Thread | None = None
        self.poll_interval = 1.0
        self.package_refresh_interval = 10.0
        self.stopped = threading.Event()
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._run, name="harmonycast-hilog", daemon=True)

    def start(self) -> None:
        self.thread.start()

    def _run(self) -> None:
        try:
            if self.stopped.is_set():
                return
            if self.bundle:
                self._monitor_application()
            else:
                self._start_process()
                while not self.stopped.is_set() and self.process.poll() is None:
                    self.stopped.wait(0.1)
                if not self.stopped.is_set():
                    if self.reader_thread:
                        self.reader_thread.join(timeout=1)
                    self.on_exit(self.process.returncode)
        except (OSError, ValueError, HDCError) as error:
            if not self.stopped.is_set():
                self.on_status(str(error))
                self.on_line(f"[Fail] {error}")
                self.on_exit(-1)
        finally:
            self.stopped.set()
            self._stop_process()

    def _start_process(self, pids: tuple[int, ...] | None = None, packages: dict[int, str] | None = None) -> None:
        args = ["shell", "hilog", "-v", "time"]
        if self.category:
            args.extend(["-t", self.LOG_TYPES[self.category]])
        if pids and len(pids) <= 5:
            args.extend(["-P", ",".join(map(str, pids))])
        with self.lock:
            if self.stopped.is_set():
                return
            process = subprocess.Popen(
                self.hdc.command(args, self.serial), shell=False,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, encoding="utf-8", errors="replace", bufsize=1,
            )
            self.process = process
            self.reader_thread = threading.Thread(target=self._read_lines, args=(process, pids, packages),
                                                  name="harmonycast-hilog-reader", daemon=True)
            self.reader_thread.start()

    def _read_lines(self, process, pids, packages) -> None:
        allowed = frozenset(pids) if pids is not None else None
        try:
            for line in process.stdout:
                if self.stopped.is_set():
                    break
                pid = hilog_pid(line)
                if allowed is None or pid in allowed:
                    if self.on_package_line and packages and pid in packages:
                        self.on_package_line(line.rstrip(), packages[pid])
                    else:
                        self.on_line(line.rstrip())
                elif line.startswith("[Fail]"):
                    self.on_status(line.rstrip())
        finally:
            process.stdout.close()

    def _monitor_application(self) -> None:
        prefix = self.bundle.endswith(".*")
        applications = {} if prefix else {self.bundle: self.hdc.application(self.serial, self.bundle)}
        next_refresh = 0.0
        previous = None
        while not self.stopped.is_set():
            if prefix:
                if time.monotonic() >= next_refresh:
                    installed = [name for name in self.hdc.applications(self.serial) if package_matches(name, self.bundle)]
                    applications = {}
                    for name in installed:
                        if self.stopped.is_set():
                            return
                        try:
                            applications[name] = self.hdc.application(self.serial, name)
                        except HDCError:
                            # An installation may disappear between the list and metadata queries.
                            continue
                    next_refresh = time.monotonic() + self.package_refresh_interval
                packages = self.hdc.application_processes(self.serial, list(applications.values())) if applications else {}
            else:
                packages = {pid: self.bundle for pid in self.hdc.application_pids(self.serial, applications[self.bundle])}
            pids = tuple(sorted(packages))
            if self.stopped.is_set():
                return
            if packages != previous:
                self._stop_process()
                if pids:
                    self._start_process(pids, packages)
                    self.on_status(f"采集中 · {len(set(packages.values()))} 个应用 / {len(pids)} 个进程" if prefix
                                   else f"采集中 · {len(pids)} 个进程")
                else:
                    self.on_status("等待匹配应用安装" if prefix and not applications else "等待应用启动")
                previous = packages
            if pids and self.process is not None and self.process.poll() is not None:
                if self.reader_thread:
                    self.reader_thread.join(timeout=1)
                if not self.stopped.is_set():
                    self.on_exit(self.process.returncode)
                return
            self.stopped.wait(self.poll_interval)

    def _stop_process(self) -> None:
        with self.lock:
            process, reader = self.process, self.reader_thread
            self.reader_thread = None
        if process is not None and process.poll() is None:
            try:
                process.terminate()
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            except ProcessLookupError:
                pass
        if reader and reader is not threading.current_thread():
            reader.join(timeout=2)

    def stop(self) -> None:
        self.stopped.set()
        self._stop_process()
        if self.thread.is_alive() and threading.current_thread() is not self.thread:
            self.thread.join(timeout=5)


class StreamSession:
    """Owns one device service process, one port forward, and one decoder."""

    def __init__(self, hdc: HDC, device: Device, fps: int = 30, bitrate: int = 4_000_000, custom_server: str = "", max_size: int = 0):
        self.hdc, self.device = hdc, device
        self.fps, self.bitrate, self.custom_server = fps, bitrate, custom_server
        self.max_size = max_size
        self.port = 0
        self.remote_port = 0
        self.process: subprocess.Popen | None = None
        self.socket: socket.socket | None = None
        self.stop_event = threading.Event()
        self.decoder: VideoDecoder | None = None
        self.forwarded = False
        self.server_pid: int | None = None
        self.pid_ready = threading.Event()
        self.output_thread: threading.Thread | None = None
        self.configured = False
        self.device_size: tuple[int, int] | None = None
        self.rotation = 0

    def selected_server(self) -> Path:
        if self.custom_server:
            path = Path(self.custom_server).expanduser()
            if not path.is_file():
                raise ValueError("自定义服务端文件不存在")
            return path
        return resource_dir() / "servers" / server_variant(self.device.architecture, self.device.vendor, self.device.version)

    def deploy(self) -> None:
        server = self.selected_server()
        config = resource_dir() / "servers/ohscrcpy_server.cfg"
        if not server.is_file() or not config.is_file():
            raise HDCError("投屏服务资源缺失，请重新打包应用")
        self.hdc.run(["target", "mount"], self.device.serial, timeout=15)
        self.hdc.run(["file", "send", str(server), "/system/bin/ohscrcpy_server"], self.device.serial, timeout=30)
        self.hdc.run(["shell", "chmod", "755", "/system/bin/ohscrcpy_server"], self.device.serial)
        self.hdc.run(["file", "send", str(config), "/system/etc/init/ohscrcpy_server.cfg"], self.device.serial, timeout=30)
        logger.info("投屏服务部署完成")

    def installed(self) -> bool:
        return self.hdc.run(["shell", "if test -x /system/bin/ohscrcpy_server; then echo installed; fi"], self.device.serial) == "installed"

    def prepare(self) -> None:
        pass

    def server_command(self) -> str:
        command = f"/system/bin/ohscrcpy_server -p {self.remote_port} -f {self.fps} -b {self.bitrate}"
        if self.max_size > 0:
            command += f" -m {self.max_size}"
        return command + " --log"

    def run(self, on_frame: Callable, on_status: Callable[[str], None]) -> None:
        try:
            on_status("正在准备投屏服务…")
            self.prepare()
            if self.stop_event.is_set():
                return
            self.port = self._free_port()
            self.remote_port = self.port
            self.hdc.run(["fport", f"tcp:{self.port}", f"tcp:{self.remote_port}"], self.device.serial)
            self.forwarded = True
            if self.stop_event.is_set():
                return
            # The remote shell reports our PID and stops only this child on exit.
            command = (
                self.server_command() + " & "
                "server_pid=$!; echo HARMONYCAST_PID=$server_pid; "
                "trap 'kill \"$server_pid\" 2>/dev/null' EXIT HUP INT TERM; wait \"$server_pid\""
            )
            self.process = subprocess.Popen(
                self.hdc.command(["shell", command], self.device.serial),
                shell=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, encoding="utf-8", errors="replace",
            )
            self.output_thread = threading.Thread(target=self._server_output, name="harmonycast-server-log", daemon=True)
            self.output_thread.start()
            on_status("正在连接视频流…")
            for attempt in range(30):
                if self.stop_event.is_set():
                    return
                try:
                    self.socket = socket.create_connection(("127.0.0.1", self.port), timeout=1)
                    self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024)
                    self._receive(on_frame, on_status)
                    return
                except (OSError, HDCError):
                    if self.stop_event.is_set():
                        return
                    if self.configured:
                        raise
                    if self.socket:
                        self.socket.close()
                    if self.process.poll() is not None:
                        raise HDCError("投屏服务启动失败，请查看工具日志并拉取服务端日志")
                    self.stop_event.wait(0.2)
            else:
                raise HDCError("投屏服务连接超时，请检查系统版本、root 权限和服务端版本")
        finally:
            self.cleanup()

    def _server_output(self) -> None:
        try:
            for line in self.process.stdout:
                if line.startswith("HARMONYCAST_PID="):
                    candidate = line.partition("=")[2].strip()
                    if candidate.isdecimal() and int(candidate) > 1:
                        self.server_pid = int(candidate)
                        self.pid_ready.set()
                else:
                    logger.info("[服务端] %s", line.rstrip())
        finally:
            self.pid_ready.set()

    @staticmethod
    def _free_port() -> int:
        with socket.socket() as temporary:
            temporary.bind(("127.0.0.1", 0))
            return temporary.getsockname()[1]

    def _receive(self, on_frame: Callable, on_status: Callable[[str], None]) -> None:
        self.socket.settimeout(1)
        buffer = bytearray()
        deadline = time.monotonic() + 8
        while b"\n" not in buffer:
            if self.stop_event.is_set():
                return
            if time.monotonic() > deadline:
                raise HDCError("等待视频流配置超时")
            try:
                chunk = self.socket.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise HDCError("视频连接已关闭")
            buffer.extend(chunk)
            if len(buffer) > 1024 * 1024:
                raise HDCError("视频流配置长度无效")
        line, remaining = bytes(buffer).split(b"\n", 1)
        config = parse_screen_info(line)
        self.configured = True
        self.decoder = VideoDecoder(config)
        self.socket.sendall(b"CONFIG_ACK\n")
        parser = PacketParser()
        res_label = "720P" if (config.width == 1280 and config.height == 720) or (config.width == 720 and config.height == 1280) else (
            "1080P" if (config.width == 1920 and config.height == 1080) or (config.width == 1080 and config.height == 1920) else f"{config.width} × {config.height}"
        )
        on_status(f"流畅投屏 · {res_label} · {config.codec.upper()}")
        last_receive = time.monotonic()
        last_heartbeat = 0.0
        pending = remaining
        while not self.stop_event.is_set():
            now = time.monotonic()
            if now - last_heartbeat >= 2:
                try:
                    self.socket.sendall(struct.pack(">II", 0, 0))
                except (OSError, socket.error):
                    if self.stop_event.is_set():
                        return
                    raise HDCError("设备已断开视频连接")
                last_heartbeat = now
            if now - last_receive > 30:
                raise HDCError("视频流已超时，请重新连接")
            if pending:
                for packet_type, payload in parser.feed(pending):
                    self._packet(packet_type, payload, on_frame)
                pending = b""
            if self.stop_event.is_set():
                return
            try:
                pending = self.socket.recv(1024 * 1024)
            except socket.timeout:
                continue
            if not pending:
                if self.stop_event.is_set():
                    return
                raise HDCError("设备已断开视频连接")
            last_receive = time.monotonic()

    def _packet(self, packet_type: int, payload: bytes, on_frame: Callable) -> None:
        if packet_type in {1, 2, 6}:
            {1: self.decoder.set_sps, 2: self.decoder.set_pps, 6: self.decoder.set_vps}[packet_type](payload)
        elif packet_type == 5:
            if len(payload) < 16:
                raise HDCError("视频分辨率配置长度无效")
            width, height, fps, bitrate = struct.unpack(">IIII", payload[:16])
            parse_screen_info(f"SCREEN_INFO:{width}:{height}:{fps}:{bitrate}:{self.decoder.config.codec}".encode())
            self.decoder.update_resolution(width, height)
            if len(payload) >= 24:
                source_width, source_height = struct.unpack(">II", payload[16:24])
                if not 1 <= source_width <= 8192 or not 1 <= source_height <= 8192:
                    raise HDCError("设备原始分辨率无效")
                self.device_size = (source_width, source_height)
            if len(payload) >= 28:
                rotation = struct.unpack(">I", payload[24:28])[0]
                if rotation > 3:
                    raise HDCError("设备旋转方向无效")
                self.rotation = rotation * 90
        elif packet_type in {3, 4} and self.decoder.is_ready():
            frame = self.decoder.decode_frame(payload, is_keyframe=packet_type == 3)
            if frame is not None:
                on_frame(frame)
        elif packet_type == 8:
            logger.info("[服务端] %s", payload.decode("utf-8", errors="replace"))

    def stop(self) -> None:
        self.stop_event.set()
        connection = self.socket
        if connection is not None:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def cleanup(self) -> None:
        if self.socket:
            self.socket.close()
        if self.process:
            self.pid_ready.wait(timeout=1)
            if self.server_pid is not None and self.process.poll() is None:
                try:
                    self.hdc.run(["shell", "kill", str(self.server_pid)], self.device.serial, timeout=3)
                except HDCError:
                    logger.warning("投屏服务停止失败：PID %s", self.server_pid)
                self.server_pid = None
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
            if self.output_thread:
                self.output_thread.join(timeout=2)
            if self.process.stdout:
                self.process.stdout.close()
        # Close only our forwarded port; other HDC sessions keep their mappings.
        if self.forwarded:
            try:
                self.hdc.run(["fport", "rm", f"tcp:{self.port}", f"tcp:{self.remote_port}"], self.device.serial, timeout=3)
            except HDCError:
                logger.warning("端口转发清理失败：%s", self.port)
            self.forwarded = False
        if self.decoder:
            self.decoder.cleanup()


class NativeSession(StreamSession):
    """Continuous H.264 capture with RGBA fallback for RISC-V OpenHarmony."""

    def __init__(self, hdc: HDC, device: Device, fps: int = 30, bitrate: int = 4_000_000, max_size: int = 720):
        super().__init__(hdc, device, fps, bitrate, max_size=max_size)
        self.bitrate = bitrate
        self.max_size = max_size
        self.remote_executable = f"/data/local/tmp/harmonycast_capture_{uuid.uuid4().hex}"
        self.uploaded = False

    @staticmethod
    def available(device: Device) -> bool:
        return device.architecture == "riscv64" and (resource_dir() / "capture/harmonycast_capture_riscv64").is_file()

    def prepare(self) -> None:
        binary = resource_dir() / "capture/harmonycast_capture_riscv64"
        self.hdc.run(["file", "send", str(binary), self.remote_executable], self.device.serial, timeout=15)
        self.uploaded = True
        self.hdc.run(["shell", "chmod", "700", self.remote_executable], self.device.serial)

    def server_command(self) -> str:
        command = f"{self.remote_executable} -p {self.remote_port} -f {self.fps} -b {self.bitrate}"
        if self.max_size > 0:
            command += f" -m {self.max_size}"
        return command

    def _receive(self, on_frame: Callable, on_status: Callable[[str], None]) -> None:
        self.socket.settimeout(1)
        deadline = time.monotonic() + 12
        while not self.stop_event.is_set():
            try:
                greeting = self.socket.recv(4, socket.MSG_PEEK)
                if not greeting:
                    raise HDCError("原生投屏服务已关闭")
                if len(greeting) >= 4:
                    break
                self.stop_event.wait(0.005)
            except socket.timeout:
                pass
            if time.monotonic() > deadline:
                raise HDCError("等待原生投屏超时")
        else:
            return
        if greeting == b"SCRE":
            return super()._receive(on_frame, on_status)
        parser = RawFrameParser()
        first_frame = True
        started = time.monotonic()
        while not self.stop_event.is_set():
            try:
                chunk = self.socket.recv(1024 * 1024)
            except socket.timeout:
                if first_frame and time.monotonic() - started > 12:
                    raise HDCError("等待原生采屏超时，请在设备上允许屏幕录制")
                continue
            if not chunk:
                if self.stop_event.is_set():
                    return
                raise HDCError("原生采屏服务已关闭，请查看工具日志")
            for frame in parser.feed(chunk):
                if len(frame) == 7:
                    self.rotation = frame[-1] * 90
                    frame = frame[:-1]
                width, height, stride, source_width, source_height, pixels = frame
                self.device_size = (source_width, source_height)
                if first_frame:
                    on_status(f"原生投屏 · {width} × {height}")
                    self.configured = True
                    first_frame = False
                on_frame(width, height, stride, source_width, source_height, pixels)

    def cleanup(self) -> None:
        super().cleanup()
        if self.uploaded:
            try:
                self.hdc.run(["shell", "rm", "-f", self.remote_executable], self.device.serial, timeout=3)
            except HDCError:
                logger.debug("原生采屏临时程序清理失败")
            self.uploaded = False
