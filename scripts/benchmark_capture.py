"""Measure continuous capture on the selected device. Moves only between launcher pages."""
import json
import logging
import sys
import threading
import time
from pathlib import Path

root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root))

from harmonycast.backend import DeviceTools, NativeSession
from harmonycast.core import HDC, find_hdc

logging.basicConfig(level=logging.INFO)
hdc = HDC(find_hdc())
devices = hdc.devices()
if not devices:
    raise SystemExit("没有连接设备")
device = hdc.info(devices[0])
if not NativeSession.available(device):
    raise SystemExit("当前设备不支持此原生采屏测试")
session = NativeSession(hdc, device)
startup_time = time.monotonic()
first_frame = threading.Event()
frame_times = []
source_size = [0, 0]
errors = []
def receive(*frame):
    frame_times.append(time.monotonic())
    if len(frame) == 1:
        source_size[:] = list(session.device_size or (frame[0].shape[1], frame[0].shape[0]))
    else:
        source_size[:] = list(frame[3:5])
    first_frame.set()
def capture():
    try:
        session.run(receive, print)
    except Exception as error:
        errors.append(str(error))
        first_frame.set()
thread = threading.Thread(target=capture)
thread.start()
try:
    if not first_frame.wait(timeout=20) or errors:
        raise RuntimeError(errors or "没有收到画面")
    tools = DeviceTools(hdc)
    tools.key(device.serial, "home")
    time.sleep(1)
    start = time.monotonic()
    count_before = len(frame_times)
    width, height = source_size
    for index in range(6):
        x1, x2 = (int(width * 0.75), int(width * 0.25)) if index % 2 == 0 else (int(width * 0.25), int(width * 0.75))
        tools.gesture(device.serial, (x1, height // 2), (x2, height // 2), 400)
        time.sleep(0.6)
    elapsed = time.monotonic() - start
    frames = len(frame_times) - count_before
    result = {"device": device.serial, "mode": "native_h264" if session.decoder else "native_rgba", "seconds": round(elapsed, 2),
              "first_frame_seconds": round(frame_times[0] - startup_time, 2),
              "frames": frames, "average_fps": round(frames / elapsed, 2), "errors": errors}
    artifact = root / "artifacts/capture-benchmark.json"
    artifact.parent.mkdir(exist_ok=True)
    artifact.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
finally:
    session.stop()
    thread.join(timeout=15)
    if thread.is_alive():
        raise RuntimeError("采屏进程未及时结束")
