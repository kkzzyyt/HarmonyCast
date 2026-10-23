from __future__ import annotations

import base64
import json
import logging
import re
import time
import urllib.error
import urllib.request
from datetime import datetime
from typing import Any, Callable

from PyQt5.QtCore import QMetaObject, QObject, QThread, QTimer, Qt, QUrl, pyqtSignal
from PyQt5.QtGui import QImage
from PyQt5.QtWebSockets import QWebSocket

logger = logging.getLogger("HarmonyCast.WebView")

DEFAULT_DEVTOOLS_PORT = 9222


def parse_webview_instances(proc_net_unix: str) -> list[dict[str, Any]]:
    """
    解析 /proc/net/unix 输出中的 webview devtools 抽象套接字。
    输出格式示例：
    ... @webview_devtools_remote_43406
    返回: [{"pid": 43406, "socket": "webview_devtools_remote_43406", "target": "localabstract:webview_devtools_remote_43406"}]
    """
    pattern = re.compile(r"@?(webview_devtools_remote_(\d+))")
    instances: dict[int, dict[str, Any]] = {}
    for line in proc_net_unix.splitlines():
        match = pattern.search(line)
        if match:
            socket_name = match.group(1)
            pid = int(match.group(2))
            if pid not in instances:
                instances[pid] = {
                    "pid": pid,
                    "socket": socket_name,
                    "target": f"localabstract:{socket_name}",
                    "bundle": "",
                }
    return list(instances.values())


def fetch_webview_pages(port: int = DEFAULT_DEVTOOLS_PORT, host: str = "127.0.0.1", timeout: float = 1.5) -> list[dict[str, Any]]:
    """
    通过 HTTP GET 请求 http://{host}:{port}/json 或 /json/list 获取当前已映射的 WebView 页面列表。
    强制绕过系统代理，避免 macOS 本地回环请求超时或挂死。
    """
    proxy_handler = urllib.request.ProxyHandler({})  # 确保 localhost 不走系统代理 (Clash/Surge/VPN等)
    opener = urllib.request.build_opener(proxy_handler)

    for endpoint in ("/json", "/json/list"):
        url = f"http://{host}:{port}{endpoint}"
        req = urllib.request.Request(url, headers={"User-Agent": "HarmonyCast"})
        try:
            with opener.open(req, timeout=timeout) as resp:
                data = resp.read().decode("utf-8")
                pages = json.loads(data)
                if isinstance(pages, list):
                    return pages
        except Exception as exc:
            logger.debug("获取 WebView 页面列表失败 (%s): %s", url, exc)
    return []


def format_cdp_arg(arg: dict[str, Any]) -> str:
    """格式化 CDP 协议中的单一参数对象。"""
    arg_type = arg.get("type", "")
    subtype = arg.get("subtype", "")

    if subtype == "null":
        return "null"
    if arg_type == "undefined":
        return "undefined"
    if arg_type in ("number", "boolean"):
        return str(arg.get("value"))
    if arg_type == "string":
        return str(arg.get("value", ""))

    if "preview" in arg and isinstance(arg["preview"], dict):
        preview = arg["preview"]
        props = preview.get("properties", [])
        if props:
            items = []
            for p in props:
                val = p.get("value", "")
                if p.get("type") == "string":
                    val = f'"{val}"'
                items.append(f"{p.get('name')}: {val}")
            return "{" + ", ".join(items) + "}"

    desc = arg.get("description")
    if desc:
        return str(desc)
    val = arg.get("value")
    if val is not None:
        return str(val)
    return str(arg)


def format_cdp_console_message(params: dict[str, Any]) -> tuple[str, str, str]:
    """
    解析并格式化 Runtime.consoleAPICalled 事件。
    返回: (type, text, hex_color)
    """
    msg_type = params.get("type", "log").lower()
    args = params.get("args", [])
    parts = [format_cdp_arg(a) for a in args]
    text = " ".join(parts) if parts else ""

    colors = {
        "log": "#c2d1df",
        "info": "#73d6c1",
        "warn": "#f3c969",
        "warning": "#f3c969",
        "error": "#ff8080",
        "debug": "#8295a9",
        "dir": "#79e6c9",
        "table": "#5ce0c1",
    }
    color = colors.get(msg_type, "#c2d1df")
    return msg_type, text, color


def format_cdp_console_entry(params: dict[str, Any]) -> dict[str, Any]:
    """
    解析并提取包含时间戳、调用位置、级别的完整日志字典。
    """
    msg_type, text, color = format_cdp_console_message(params)

    stack = params.get("stackTrace", {})
    call_frames = stack.get("callFrames", []) if isinstance(stack, dict) else []
    source = ""
    if call_frames:
        frame = call_frames[0]
        url = frame.get("url", "")
        if url:
            url = url.split("?")[0].split("/")[-1] or url
        line = frame.get("lineNumber", 0) + 1
        source = f"{url}:{line}" if url else f"line {line}"

    timestamp_ms = params.get("timestamp")
    if timestamp_ms:
        time_str = time.strftime("%H:%M:%S", time.localtime(timestamp_ms / 1000.0)) + f".{int(timestamp_ms % 1000):03d}"
    else:
        now = time.time()
        time_str = time.strftime("%H:%M:%S", time.localtime(now)) + f".{int((now % 1) * 1000):03d}"

    return {
        "type": msg_type,
        "text": text,
        "color": color,
        "source": source,
        "time": time_str,
    }


def format_cdp_eval_result(result_obj: dict[str, Any]) -> tuple[str, bool]:
    """
    解析 Runtime.evaluate 的返回结果。
    返回: (display_text, is_error)
    """
    if not isinstance(result_obj, dict):
        return str(result_obj), False

    if "exceptionDetails" in result_obj:
        exc = result_obj["exceptionDetails"]
        text = exc.get("text", "Uncaught Error")
        details = exc.get("exception", {}).get("description") or exc.get("exception", {}).get("value") or text
        return f"{text}: {details}", True

    res = result_obj.get("result", {})
    val = res.get("value")
    res_type = res.get("type")

    if res_type == "undefined":
        return "undefined", False
    if res.get("subtype") == "null":
        return "null", False
    if res_type == "string":
        return f'"{val}"', False
    if val is not None:
        return str(val), False
    if "description" in res:
        return str(res["description"]), False
    return json.dumps(res, ensure_ascii=False), False


def generate_curl_command(entry: dict[str, Any]) -> str:
    """
    根据网络请求条目生成标准可执行的 curl 命令。
    """
    url = entry.get("url", "")
    method = str(entry.get("method", "GET")).upper()
    parts = ["curl", "-X", method, f"'{url}'"]
    for k, v in entry.get("request_headers", {}).items():
        if not str(k).startswith(":"):
            clean_v = str(v).replace("'", "'\\''")
            parts.append(f"-H '{k}: {clean_v}'")
    post_data = entry.get("post_data")
    if post_data:
        data_str = str(post_data).replace("'", "'\\''")
        parts.append(f"--data-raw '{data_str}'")
    return " ".join(parts)


class WebViewSession(QObject):
    """
    管理单个设备的 WebView 调试会话：
    - 实例扫描
    - HDC 端口转发管理 (hdc fport)
    - 页面获取 (/json)
    - WebSocket CDP 控制台交互
    - 页面实时画面捕获 (Page.startScreencast / Page.captureScreenshot)
    - 鼠标触摸事件转发 (Input.dispatchMouseEvent)
    """

    instances_discovered = pyqtSignal(list)
    forward_status_changed = pyqtSignal(bool, str)  # (active, info_or_err)
    pages_discovered = pyqtSignal(list)
    console_message = pyqtSignal(str, str, str)  # (type, text, color)
    console_entry = pyqtSignal(dict)  # (entry_dict)
    network_entry = pyqtSignal(dict)  # (network_dict: id, url, method, status, type, duration, time)
    eval_result = pyqtSignal(int, str, bool)  # (req_id, text, is_error)
    ws_state_changed = pyqtSignal(bool, str)  # (connected, url_or_msg)
    screencast_frame = pyqtSignal(QImage, dict)  # (frame_image, metadata)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.hdc: Any = None
        self.device_serial: str = ""
        self.current_pid: int | None = None
        self.current_target: str = ""
        self.current_socket: str = ""
        self.local_port: int = DEFAULT_DEVTOOLS_PORT
        self.forwarded: bool = False
        self.current_page_id: str = ""
        self.current_ws_url: str = ""
        self._screenshot_req_id: int | None = None
        self._last_screencast_at: float = 0.0
        self._network_requests: dict[str, dict[str, Any]] = {}
        self._body_callbacks: dict[int, tuple[str, Any]] = {}

        self._req_id = 1000
        self.ws = QWebSocket(parent=self)
        self.ws.connected.connect(self._on_ws_connected)
        self.ws.disconnected.connect(self._on_ws_disconnected)
        self.ws.textMessageReceived.connect(self._on_ws_message)
        self.ws.error.connect(self._on_ws_error)

    def scan_instances(self, hdc, serial: str) -> list[dict[str, Any]]:
        """在设备上运行 shell 命令探测包含 webview_devtools_remote 的套接字及进程。"""
        self.hdc = hdc
        self.device_serial = serial
        if not hdc or not serial:
            self.instances_discovered.emit([])
            return []

        try:
            raw_unix = hdc.run(["shell", "cat /proc/net/unix"], device=serial, timeout=3.0)
            instances = parse_webview_instances(raw_unix)

            # 尝试补充获取各 PID 的应用包名
            for inst in instances:
                pid = inst["pid"]
                try:
                    cmdline = hdc.run(["shell", f"cat /proc/{pid}/cmdline"], device=serial, timeout=1.5)
                    bundle = cmdline.replace("\x00", " ").strip()
                    if bundle:
                        inst["bundle"] = bundle
                except Exception:
                    pass

            self.instances_discovered.emit(instances)
            return instances
        except Exception as exc:
            logger.warning("扫描 WebView 实例失败: %s", exc)
            self.instances_discovered.emit([])
            return []

    def start_forward(
        self,
        hdc,
        serial: str,
        target_or_pid: Any = "tcp:9222",
        socket_name: str | None = None,
        port: int = DEFAULT_DEVTOOLS_PORT,
        pid: int | None = None,
        target: str | None = None,
    ) -> tuple[bool, str]:
        """开启 HDC 端口转发: hdc fport tcp:{port} {remote_target}"""
        self.stop_forward()

        # 解析远端目标
        remote_target = ""
        if target:
            remote_target = str(target).strip()
        elif socket_name:
            s_name = str(socket_name).strip()
            if s_name.startswith(("tcp:", "localabstract:", "localreserved:", "localfilesystem:")):
                remote_target = s_name
            else:
                remote_target = f"localabstract:{s_name}"
            if isinstance(target_or_pid, int):
                pid = target_or_pid
        elif isinstance(target_or_pid, int):
            remote_target = f"localabstract:webview_devtools_remote_{target_or_pid}"
            pid = target_or_pid
        elif isinstance(target_or_pid, str):
            val = target_or_pid.strip()
            if val.startswith(("tcp:", "localabstract:", "localreserved:", "localfilesystem:")):
                remote_target = val
            elif val.isdigit():
                remote_target = f"tcp:{val}"
            elif val.startswith("webview_devtools_remote_"):
                remote_target = f"localabstract:{val}"
            elif not val:
                remote_target = f"tcp:{port}"
            else:
                remote_target = val
        else:
            remote_target = f"tcp:{port}"

        self.hdc = hdc
        self.device_serial = serial
        self.current_pid = pid
        self.current_target = remote_target
        self.current_socket = remote_target.replace("localabstract:", "")
        self.local_port = port

        try:
            # 1. 执行建立端口转发
            res = hdc.run(["fport", f"tcp:{port}", remote_target], device=serial, timeout=3.5)
            self.forwarded = True
            info = f"已建立映射: 127.0.0.1:{port} ➔ {remote_target}"
            self.forward_status_changed.emit(True, info)
            return True, info
        except Exception as exc:
            err_str = str(exc)
            # 如果报 listen failed、occupied 或已有旧规则，检测端口是否实际可用
            if "listen failed" in err_str.lower() or "occupied" in err_str.lower() or "already" in err_str.lower() or "exist" in err_str.lower():
                existing_pages = fetch_webview_pages(port=port, timeout=1.0)
                if existing_pages:
                    self.forwarded = True
                    info = f"已连通 127.0.0.1:{port} (发现 {len(existing_pages)} 个目标页面)"
                    self.forward_status_changed.emit(True, info)
                    return True, info
                # 尝试先 rm 后重试建立
                try:
                    hdc.run(["fport", "rm", f"tcp:{port}", remote_target], device=serial, timeout=2.0)
                    hdc.run(["fport", f"tcp:{port}", remote_target], device=serial, timeout=3.0)
                    self.forwarded = True
                    info = f"已建立映射: 127.0.0.1:{port} ➔ {remote_target}"
                    self.forward_status_changed.emit(True, info)
                    return True, info
                except Exception as exc2:
                    exc = exc2
            err = f"端口转发失败: {exc}"
            self.forward_status_changed.emit(False, err)
            return False, err

    def stop_forward(self):
        """停止当前转发并关闭 WebSocket"""
        self.disconnect_page()
        if self.forwarded and self.hdc and self.device_serial:
            target_to_rm = self.current_target or (f"localabstract:{self.current_socket}" if self.current_socket else f"tcp:{self.local_port}")
            try:
                self.hdc.run(
                    ["fport", "rm", f"tcp:{self.local_port}", target_to_rm],
                    device=self.device_serial,
                    timeout=2.0,
                )
            except Exception as exc:
                logger.debug("移除端口转发失败: %s", exc)
        self.forwarded = False
        self.current_pid = None
        self.current_target = ""
        self.current_socket = ""
        self.forward_status_changed.emit(False, "未建立映射")

    def fetch_pages(self) -> list[dict[str, Any]]:
        """拉取当前映射端口下的可用 Web 页面列表"""
        pages = fetch_webview_pages(port=self.local_port)
        if pages:
            self.forwarded = True
            self.forward_status_changed.emit(True, f"已连接 127.0.0.1:{self.local_port}")
        self.pages_discovered.emit(pages)
        return pages

    def connect_page(self, ws_url: str, page_id: str = ""):
        """连接到指定页面的 WebSocket 调试端点"""
        self.disconnect_page()
        if not ws_url:
            return
        self.current_ws_url = ws_url
        self.current_page_id = page_id
        logger.info("正在连接 WebView 调试 WebSocket: %s", ws_url)
        self.ws.open(QUrl(ws_url))

    def disconnect_page(self):
        """断开当前页面的 WebSocket 连接，强制中止以防 QtWebSockets 忙死锁循环"""
        if hasattr(self, "ws") and self.ws:
            try:
                if self.ws.thread() != QThread.currentThread():
                    QTimer.singleShot(0, self.ws.abort)
                else:
                    self.ws.abort()
            except Exception:
                pass
        self.current_ws_url = ""
        self.current_page_id = ""

    def evaluate(self, expression: str) -> int:
        """在当前页面执行一段 JavaScript 表达式并返回请求 ID"""
        if not self.ws.isValid():
            raise RuntimeError("控制台未连接到任何 Web 页面")
        self._req_id += 1
        req_id = self._req_id
        msg = {
            "id": req_id,
            "method": "Runtime.evaluate",
            "params": {
                "expression": expression,
                "returnByValue": True,
                "generatePreview": True,
                "awaitPromise": True,
            },
        }
        self.ws.sendTextMessage(json.dumps(msg))
        return req_id

    def reload_page(self, ignore_cache: bool = True):
        """刷新当前页面"""
        if not self.ws.isValid():
            return
        self._req_id += 1
        msg = {
            "id": self._req_id,
            "method": "Page.reload",
            "params": {"ignoreCache": ignore_cache},
        }
        self.ws.sendTextMessage(json.dumps(msg))
        # 刷新后稍作延时主动抓取一帧截图
        self.capture_screenshot()

    def start_screencast(self, format: str = "jpeg", quality: int = 60, max_width: int = 1280, max_height: int = 1280):
        """启动页面实时画面推流 (CDP Page.startScreencast)"""
        if not self.ws.isValid():
            return
        self._req_id += 1
        msg = {
            "id": self._req_id,
            "method": "Page.startScreencast",
            "params": {
                "format": format,
                "quality": quality,
                "maxWidth": max_width,
                "maxHeight": max_height,
                "everyNthFrame": 2,
            },
        }
        self.ws.sendTextMessage(json.dumps(msg))

    def capture_screenshot(self):
        """主动截取一帧当前网页图像"""
        if not self.ws.isValid():
            return
        self._req_id += 1
        self._screenshot_req_id = self._req_id
        msg = {
            "id": self._req_id,
            "method": "Page.captureScreenshot",
            "params": {"format": "jpeg", "quality": 80},
        }
        self.ws.sendTextMessage(json.dumps(msg))

    def dispatch_mouse_click(self, x: int, y: int):
        """向目标网页发送全套触摸/鼠标/JS/物理输入事件，确保在移动端/平板/Iframe内都能精准响应点击"""
        if not self.ws.isValid():
            return
        ix, iy = int(x), int(y)

        # 1. CDP 移动端触摸事件 (移动端 H5 / Vue / React 触摸组件)
        self._req_id += 1
        self.ws.sendTextMessage(json.dumps({
            "id": self._req_id,
            "method": "Input.dispatchTouchEvent",
            "params": {"type": "touchStart", "touchPoints": [{"x": ix, "y": iy}]},
        }))
        self._req_id += 1
        self.ws.sendTextMessage(json.dumps({
            "id": self._req_id,
            "method": "Input.dispatchTouchEvent",
            "params": {"type": "touchEnd", "touchPoints": []},
        }))

        # 2. CDP 鼠标事件 (标准桌面鼠标点击，含 buttons: 1 标志位)
        self._req_id += 1
        self.ws.sendTextMessage(json.dumps({
            "id": self._req_id,
            "method": "Input.dispatchMouseEvent",
            "params": {"type": "mouseMoved", "x": ix, "y": iy},
        }))
        self._req_id += 1
        self.ws.sendTextMessage(json.dumps({
            "id": self._req_id,
            "method": "Input.dispatchMouseEvent",
            "params": {"type": "mousePressed", "x": ix, "y": iy, "button": "left", "buttons": 1, "clickCount": 1},
        }))
        self._req_id += 1
        self.ws.sendTextMessage(json.dumps({
            "id": self._req_id,
            "method": "Input.dispatchMouseEvent",
            "params": {"type": "mouseReleased", "x": ix, "y": iy, "button": "left", "buttons": 0},
        }))

        # 3. JS 穿透式点击 (穿透 iframe 嵌套层级，直接触发元素 click 与 pointer/mouse 事件)
        js_click = f"""
        (() => {{
            const x = {ix}, y = {iy};
            let el = document.elementFromPoint(x, y);
            if (el && el.tagName === "IFRAME") {{
                try {{
                    const rect = el.getBoundingClientRect();
                    const subDoc = el.contentDocument || el.contentWindow.document;
                    el = subDoc.elementFromPoint(x - rect.left, y - rect.top) || el;
                }} catch(e) {{}}
            }}
            if (el) {{
                ['pointerdown', 'mousedown', 'touchstart', 'touchend', 'pointerup', 'mouseup', 'click'].forEach(evt => {{
                    try {{
                        el.dispatchEvent(new MouseEvent(evt, {{bubbles: true, cancelable: true, clientX: x, clientY: y, view: window}}));
                    }} catch(e) {{}}
                }});
                if (typeof el.click === 'function') el.click();
            }}
        }})();
        """
        try:
            self.evaluate(js_click)
        except Exception:
            pass

        # 4. HDC 物理层真实触摸 (在设备端直接模拟真实硬件屏幕点击)
        if self.hdc and self.device_serial:
            try:
                self.hdc.run(["shell", "uinput", "-T", "-c", str(ix), str(iy)], device=self.device_serial, timeout=1.0)
            except Exception:
                pass

        # 点击后抓取更新截图
        self.capture_screenshot()

    def get_network_request(self, req_id: str) -> dict[str, Any] | None:
        """获取指定网络请求的完整元数据"""
        return self._network_requests.get(str(req_id))

    def get_response_body(self, req_id: str, callback: Any):
        """
        异步拉取网络请求的返回体数据 (CDP Network.getResponseBody)。
        优先使用本地已缓存的数据；如未缓存则向目标设备发送命令拉取。
        callback 签名: callback(body: str, is_base64: bool, error: str)
        """
        req_id_str = str(req_id)
        entry = self._network_requests.get(req_id_str)
        if entry and entry.get("response_body"):
            callback(entry["response_body"], bool(entry.get("response_base64", False)), "")
            return

        if not self.ws.isValid():
            callback("", False, "CDP 调试通道未连接")
            return

        self._req_id += 1
        cmd_id = self._req_id
        self._body_callbacks[cmd_id] = (req_id_str, callback)
        msg = {
            "id": cmd_id,
            "method": "Network.getResponseBody",
            "params": {"requestId": req_id_str},
        }
        self.ws.sendTextMessage(json.dumps(msg))

    def clear_network(self):
        """清空网络请求缓存与待处理回调"""
        self._network_requests.clear()
        self._body_callbacks.clear()

    def _on_ws_connected(self):
        self.ws_state_changed.emit(True, self.current_ws_url)
        # 激活基础调试域及网络监听
        for method in ("Runtime.enable", "Console.enable", "Page.enable", "Log.enable", "Network.enable"):
            self._req_id += 1
            self.ws.sendTextMessage(json.dumps({"id": self._req_id, "method": method}))

    def _on_ws_disconnected(self):
        self._body_callbacks.clear()
        self.ws_state_changed.emit(False, "连接已断开")

    def _on_ws_error(self, error_code):
        err_msg = self.ws.errorString()
        logger.warning("WebView WebSocket 错误: %s (%s)", err_msg, error_code)
        try:
            self.ws.abort()
        except Exception:
            pass
        self.ws_state_changed.emit(False, f"连接错误: {err_msg}")

    def _on_ws_message(self, message: str):
        try:
            data = json.loads(message)
        except Exception:
            return

        method = data.get("method")
        params = data.get("params", {})

        # 1. 拦截页面画面推流帧 (Page.screencastFrame)
        if method == "Page.screencastFrame":
            session_id = params.get("sessionId")
            data_b64 = params.get("data", "")
            metadata = params.get("metadata", {})
            if session_id is not None:
                self._req_id += 1
                ack_msg = {
                    "id": self._req_id,
                    "method": "Page.screencastFrameAck",
                    "params": {"sessionId": session_id},
                }
                self.ws.sendTextMessage(json.dumps(ack_msg))

            now = time.monotonic()
            if data_b64 and (now - self._last_screencast_at >= 0.04):
                self._last_screencast_at = now
                try:
                    img_bytes = base64.b64decode(data_b64)
                    img = QImage()
                    if img.loadFromData(img_bytes, "JPEG"):
                        self.screencast_frame.emit(img, metadata)
                except Exception as exc:
                    logger.debug("画面帧解码异常: %s", exc)

        # 2. 拦截主动截图返回
        req_id = data.get("id")
        if req_id is not None and req_id == self._screenshot_req_id:
            res = data.get("result", {})
            data_b64 = res.get("data", "")
            if data_b64:
                try:
                    img_bytes = base64.b64decode(data_b64)
                    img = QImage()
                    if img.loadFromData(img_bytes, "JPEG"):
                        self.screencast_frame.emit(img, {})
                except Exception as exc:
                    logger.debug("截图返回解码异常: %s", exc)

        # 拦截 Network.getResponseBody 返回结果
        if req_id is not None and req_id in self._body_callbacks:
            target_req_id, cb = self._body_callbacks.pop(req_id)
            if "result" in data:
                res_obj = data.get("result", {})
                body_str = res_obj.get("body", "")
                is_b64 = bool(res_obj.get("base64Encoded", False))
                target_entry = self._network_requests.get(target_req_id)
                if target_entry:
                    target_entry["response_body"] = body_str
                    target_entry["response_base64"] = is_b64
                try:
                    cb(body_str, is_b64, "")
                except Exception as exc:
                    logger.debug("response_body 回调执行异常: %s", exc)
            elif "error" in data:
                err_obj = data.get("error", {})
                err_msg = err_obj.get("message", "获取失败")
                try:
                    cb("", False, err_msg)
                except Exception as exc:
                    logger.debug("response_body 失败回调执行异常: %s", exc)
            return

        # 3. 拦截 Console 日志输出
        if method == "Runtime.consoleAPICalled":
            entry = format_cdp_console_entry(params)
            self.console_message.emit(entry["type"], entry["text"], entry["color"])
            self.console_entry.emit(entry)
        elif method == "Runtime.exceptionThrown":
            details = params.get("exceptionDetails", {})
            text = details.get("text", "Uncaught Exception")
            exc = details.get("exception", {})
            desc = exc.get("description") or exc.get("value") or text
            stack = details.get("stackTrace", {})
            call_frames = stack.get("callFrames", []) if isinstance(stack, dict) else []
            source = ""
            if call_frames:
                frame = call_frames[0]
                url = frame.get("url", "")
                if url:
                    url = url.split("?")[0].split("/")[-1] or url
                line = frame.get("lineNumber", 0) + 1
                source = f"{url}:{line}" if url else f"line {line}"

            now = time.time()
            time_str = time.strftime("%H:%M:%S", time.localtime(now)) + f".{int((now % 1) * 1000):03d}"
            entry = {
                "type": "error",
                "text": str(desc),
                "color": "#ff8080",
                "source": source,
                "time": time_str,
            }
            self.console_message.emit("error", str(desc), "#ff8080")
            self.console_entry.emit(entry)
        elif method == "Console.messageAdded":
            item = params.get("message", {})
            level = item.get("level", "log").lower()
            text = item.get("text", "")
            colors = {"log": "#c2d1df", "warning": "#f3c969", "error": "#ff8080", "info": "#73d6c1"}
            color = colors.get(level, "#c2d1df")
            now = time.time()
            time_str = time.strftime("%H:%M:%S", time.localtime(now)) + f".{int((now % 1) * 1000):03d}"
            entry = {
                "type": level,
                "text": text,
                "color": color,
                "source": f"{item.get('url', '').split('/')[-1]}:{item.get('line', '')}" if item.get("url") else "",
                "time": time_str,
            }
            self.console_message.emit(level, text, color)
            self.console_entry.emit(entry)
        elif method == "Log.entryAdded":
            item = params.get("entry", {})
            level = item.get("level", "info").lower()
            text = item.get("text", "")
            colors = {"info": "#73d6c1", "warning": "#f3c969", "error": "#ff8080"}
            color = colors.get(level, "#c2d1df")
            now = time.time()
            time_str = time.strftime("%H:%M:%S", time.localtime(now)) + f".{int((now % 1) * 1000):03d}"
            entry = {
                "type": level,
                "text": text,
                "color": color,
                "source": item.get("source", ""),
                "time": time_str,
            }
            self.console_message.emit(level, text, color)
            self.console_entry.emit(entry)

        # 4. 拦截 Network 网络请求事件 (Fetch/XHR/API/Document)
        elif method == "Network.requestWillBeSent":
            req_id_str = str(params.get("requestId", ""))
            req_data = params.get("request", {})
            wall_time = params.get("wallTime") or time.time()
            url = req_data.get("url", "")
            method_str = req_data.get("method", "GET").upper()
            req_type = str(params.get("type", "Other")).lower()
            now_dt = datetime.fromtimestamp(wall_time) if wall_time else datetime.now()
            headers = req_data.get("headers", {})
            post_data = req_data.get("postData", "")
            entry = {
                "id": req_id_str,
                "url": url,
                "method": method_str,
                "type": req_type,
                "status": "pending",
                "status_text": "",
                "time": now_dt.strftime("%H:%M:%S"),
                "duration": "-",
                "start_ts": time.monotonic(),
                "request_headers": headers if isinstance(headers, dict) else {},
                "post_data": post_data or "",
                "response_headers": {},
                "response_body": "",
                "response_base64": False,
            }
            if len(self._network_requests) > 600:
                self._network_requests.clear()
            self._network_requests[req_id_str] = entry
            self.network_entry.emit(entry)

        elif method == "Network.responseReceived":
            req_id_str = str(params.get("requestId", ""))
            res_data = params.get("response", {})
            entry = self._network_requests.get(req_id_str)
            if entry:
                entry["status"] = str(res_data.get("status", 200))
                entry["status_text"] = str(res_data.get("statusText", "OK"))
                entry["mimeType"] = res_data.get("mimeType", "")
                entry["protocol"] = res_data.get("protocol", "")
                res_headers = res_data.get("headers", {})
                entry["response_headers"] = res_headers if isinstance(res_headers, dict) else {}
                if entry.get("start_ts"):
                    dur = int((time.monotonic() - entry["start_ts"]) * 1000)
                    entry["duration"] = f"{dur}ms"
                self.network_entry.emit(entry)

        elif method == "Network.loadingFinished":
            req_id_str = str(params.get("requestId", ""))
            entry = self._network_requests.get(req_id_str)
            if entry:
                if entry.get("start_ts") and entry["duration"] == "-":
                    dur = int((time.monotonic() - entry["start_ts"]) * 1000)
                    entry["duration"] = f"{dur}ms"
                self.network_entry.emit(entry)

        elif method == "Network.loadingFailed":
            req_id_str = str(params.get("requestId", ""))
            entry = self._network_requests.get(req_id_str)
            if entry:
                entry["status"] = "failed"
                entry["error"] = params.get("errorText", "Failed")
                if entry.get("start_ts"):
                    dur = int((time.monotonic() - entry["start_ts"]) * 1000)
                    entry["duration"] = f"{dur}ms"
                self.network_entry.emit(entry)

        # 5. 拦截 JS 表达式执行结果
        if req_id is not None and req_id >= 1000 and req_id != self._screenshot_req_id:
            if "result" in data:
                text, is_err = format_cdp_eval_result(data.get("result", {}))
                self.eval_result.emit(req_id, text, is_err)
            elif "error" in data:
                err_info = data.get("error", {})
                err_text = f"Error {err_info.get('code')}: {err_info.get('message')}"
                self.eval_result.emit(req_id, err_text, True)
