import json
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
import pytest
from PyQt5.QtWidgets import QApplication

from harmonycast.webview import (
    parse_webview_instances,
    fetch_webview_pages,
    format_cdp_console_message,
    format_cdp_eval_result,
    generate_curl_command,
    WebViewSession,
    DEFAULT_DEVTOOLS_PORT,
)


@pytest.fixture
def app():
    return QApplication.instance() or QApplication([])


def test_parse_webview_instances():
    raw = """
Num       RefCount Protocol Flags    Type St Inode Path
0000000000000000: 00000002 00000000 00010000 0001 01 2318187 @webview_devtools_remote_43406
0000000000000000: 00000002 00000000 00010000 0001 01 2318188 @webview_devtools_remote_12345
0000000000000000: 00000002 00000000 00010000 0001 01 2318187 @webview_devtools_remote_43406
0000000000000000: 00000002 00000000 00010000 0001 01 9999999 /dev/socket/other_socket
"""
    instances = parse_webview_instances(raw)
    assert len(instances) == 2
    pids = [i["pid"] for i in instances]
    assert 43406 in pids
    assert 12345 in pids
    assert instances[0]["target"] == "localabstract:webview_devtools_remote_43406"

    # 空输入测试
    assert parse_webview_instances("") == []
    assert parse_webview_instances("no devtools here") == []


def test_fetch_webview_pages_success():
    class MockCDPHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path in ("/json", "/json/list"):
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                data = [
                    {
                        "id": "P1",
                        "title": "智慧病房",
                        "url": "https://yarward.com/bedside",
                        "webSocketDebuggerUrl": "ws://127.0.0.1:9876/devtools/page/P1",
                    }
                ]
                self.wfile.write(json.dumps(data).encode("utf-8"))
            else:
                self.send_response(404)
                self.end_headers()

    server = HTTPServer(("127.0.0.1", 9876), MockCDPHandler)
    t = threading.Thread(target=server.handle_request, daemon=True)
    t.start()

    pages = fetch_webview_pages(port=9876)
    assert len(pages) == 1
    assert pages[0]["id"] == "P1"
    assert pages[0]["title"] == "智慧病房"


def test_fetch_webview_pages_unreachable():
    pages = fetch_webview_pages(port=19876)
    assert pages == []


def test_format_cdp_console_message():
    # 字符串与数字
    msg_type, text, color = format_cdp_console_message({
        "type": "log",
        "args": [{"type": "string", "value": "用户点击了按钮"}, {"type": "number", "value": 42}],
    })
    assert msg_type == "log"
    assert text == "用户点击了按钮 42"
    assert color == "#c2d1df"

    # Warning
    msg_type, text, color = format_cdp_console_message({
        "type": "warn",
        "args": [{"type": "string", "value": "资源加载超时"}],
    })
    assert msg_type == "warn"
    assert color == "#f3c969"

    # Error
    msg_type, text, color = format_cdp_console_message({
        "type": "error",
        "args": [{"type": "string", "value": "Uncaught SyntaxError"}],
    })
    assert msg_type == "error"
    assert color == "#ff8080"

    # Object preview
    _, text, _ = format_cdp_console_message({
        "type": "info",
        "args": [{
            "type": "object",
            "preview": {
                "properties": [
                    {"name": "code", "type": "number", "value": 200},
                    {"name": "msg", "type": "string", "value": "ok"},
                ]
            }
        }],
    })
    assert text == '{code: 200, msg: "ok"}'


def test_format_cdp_eval_result():
    # 基础类型
    res, is_err = format_cdp_eval_result({"result": {"type": "string", "value": "hello"}})
    assert res == '"hello"'
    assert is_err is False

    res, is_err = format_cdp_eval_result({"result": {"type": "number", "value": 100}})
    assert res == "100"
    assert is_err is False

    res, is_err = format_cdp_eval_result({"result": {"type": "undefined"}})
    assert res == "undefined"
    assert is_err is False

    # 异常
    res, is_err = format_cdp_eval_result({
        "exceptionDetails": {
            "text": "Uncaught",
            "exception": {"description": "ReferenceError: a is not defined"},
        }
    })
    assert is_err is True
    assert "ReferenceError: a is not defined" in res


def test_webview_session_lifecycle(app):
    class FakeHDC:
        def __init__(self):
            self.commands = []

        def run(self, args, device=None, timeout=5):
            self.commands.append((args, device))
            if args == ["shell", "cat /proc/net/unix"]:
                return "0000: 0002 0000 00010000 0001 01 100 @webview_devtools_remote_9988\n"
            if args == ["shell", "cat /proc/9988/cmdline"]:
                return "com.yarward.bedside\x00"
            if args[0] == "fport":
                return "Forwardport result:OK"
            return ""

    hdc = FakeHDC()
    session = WebViewSession()

    # 1. 扫描实例
    instances = session.scan_instances(hdc, "TEST_DEVICE")
    assert len(instances) == 1
    assert instances[0]["pid"] == 9988
    assert instances[0]["bundle"] == "com.yarward.bedside"

    # 2. 建立端口转发
    ok, info = session.start_forward(hdc, "TEST_DEVICE", 9988, "webview_devtools_remote_9988", port=9222)
    assert ok is True
    assert session.forwarded is True
    assert ("fport", "tcp:9222", "localabstract:webview_devtools_remote_9988") in [tuple(c[0]) for c in hdc.commands]

    # 3. 停止转发
    session.stop_forward()
    assert session.forwarded is False
    assert ("fport", "rm", "tcp:9222", "localabstract:webview_devtools_remote_9988") in [tuple(c[0]) for c in hdc.commands]


def test_webview_session_direct_tcp_forward(app):
    class FakeHDC:
        def __init__(self):
            self.commands = []

        def run(self, args, device=None, timeout=5):
            self.commands.append((args, device))
            if args[0] == "fport":
                return "Forwardport result:OK"
            return ""

    hdc = FakeHDC()
    session = WebViewSession()
    ok, info = session.start_forward(hdc, "TEST_DEVICE", target="tcp:9222", port=9222)
    assert ok is True
    assert session.forwarded is True
    assert ("fport", "tcp:9222", "tcp:9222") in [tuple(c[0]) for c in hdc.commands]

    session.stop_forward()
    assert session.forwarded is False
    assert ("fport", "rm", "tcp:9222", "tcp:9222") in [tuple(c[0]) for c in hdc.commands]


def test_webview_session_network_events_and_click(app):
    session = WebViewSession()
    received_net = []
    session.network_entry.connect(lambda e: received_net.append(e))

    # 模拟 requestWillBeSent
    session._on_ws_message(json.dumps({
        "method": "Network.requestWillBeSent",
        "params": {
            "requestId": "req_100",
            "request": {"url": "http://10.10.248.21/api/bed", "method": "POST"},
            "type": "XHR",
            "wallTime": 1700000000,
        }
    }))
    assert len(received_net) == 1
    assert received_net[0]["method"] == "POST"
    assert received_net[0]["status"] == "pending"

    # 模拟 responseReceived
    session._on_ws_message(json.dumps({
        "method": "Network.responseReceived",
        "params": {
            "requestId": "req_100",
            "response": {"status": 200, "mimeType": "application/json"},
        }
    }))
    assert len(received_net) == 2
    assert received_net[1]["status"] == "200"

    # 模拟 loadingFinished
    session._on_ws_message(json.dumps({
        "method": "Network.loadingFinished",
        "params": {"requestId": "req_100"},
    }))
    assert len(received_net) == 3

    # 元数据检查
    entry = session.get_network_request("req_100")
    assert entry is not None
    assert entry["method"] == "POST"
    assert entry["url"] == "http://10.10.248.21/api/bed"

    # 模拟响应体拉取与回调
    body_res = []
    # 模拟注册回调
    cmd_id = session._req_id + 1
    session._body_callbacks[cmd_id] = ("req_100", lambda b, is_b64, err: body_res.append((b, is_b64, err)))
    session._on_ws_message(json.dumps({
        "id": cmd_id,
        "result": {"body": '{"code":0,"data":{"bedId":"01"}}', "base64Encoded": False}
    }))
    assert len(body_res) == 1
    assert '{"bedId":"01"}' in body_res[0][0]
    assert body_res[0][1] is False

    # 检查已缓存至 entry
    assert '{"bedId":"01"}' in entry["response_body"]

    # 再次请求走缓存
    cached_res = []
    session.get_response_body("req_100", lambda b, is_b64, err: cached_res.append(b))
    assert len(cached_res) == 1
    assert cached_res[0] == entry["response_body"]

    # 清空网络请求
    session.clear_network()
    assert session.get_network_request("req_100") is None


def test_generate_curl_command():
    entry = {
        "url": "http://10.10.248.21/api/bed/update",
        "method": "POST",
        "request_headers": {
            ":method": "POST",
            "Content-Type": "application/json",
            "Authorization": "Bearer token123",
        },
        "post_data": '{"bed": "101", "name": "张三"}',
    }
    cmd = generate_curl_command(entry)
    assert cmd.startswith("curl -X POST 'http://10.10.248.21/api/bed/update'")
    assert "-H 'Content-Type: application/json'" in cmd
    assert "-H 'Authorization: Bearer token123'" in cmd
    assert ":method" not in cmd  # 排除伪标头
    assert """--data-raw '{"bed": "101", "name": "张三"}'""" in cmd




