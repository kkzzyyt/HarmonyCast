"""Verify device discovery, compatible preview, HiLog, and local export on a real HDC target."""
import json
import os
import sys
import tempfile
import time
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root))

from PyQt5.QtCore import QSettings, Qt
from PyQt5.QtGui import QImage
from PyQt5.QtWidgets import QApplication, QMessageBox

from harmonycast.ui import MainWindow, STYLE
from harmonycast.backend import DeviceTools

import numpy as np


def wait_for(app, condition, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        app.processEvents()
        if condition():
            return
        time.sleep(0.025)
    raise RuntimeError("设备验证超时")


app = QApplication([])
app.setStyleSheet(STYLE)
QMessageBox.warning = lambda parent, title, message: print(f"{title}: {message}", file=sys.stderr)
artifacts = root / "artifacts"
artifacts.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="harmonycast-smoke-") as temporary:
    settings = QSettings(str(Path(temporary) / "settings.ini"), QSettings.IniFormat)
    settings.setValue("logs/category", "")
    window = MainWindow(settings)
    window.show()
    try:
        wait_for(app, lambda: not window.scan_busy and window.selected_device() is not None, 20)
        device = window.selected_device()
        window.mode.setCurrentIndex(0)
        window.start_mirror()
        window.switch_module(1)
        wait_for(app, lambda: not window.screen.image.isNull(), 25)
        wait_for(app, lambda: any(entry.source == "HiLog" for entry in window.store.entries), 15)
        window.screen.image.save(str(artifacts / "device-screen.png"))
        reference = QImage.fromData(DeviceTools(window.hdc).capture(device.serial))
        actual = window.screen.image.convertToFormat(QImage.Format_RGB888)
        reference = reference.scaled(actual.size(), Qt.IgnoreAspectRatio, Qt.SmoothTransformation).convertToFormat(QImage.Format_RGB888)
        def pixels(image):
            data = image.bits()
            data.setsize(image.byteCount())
            return np.frombuffer(data, dtype=np.uint8).reshape(image.height(), image.bytesPerLine())[:, :image.width() * 3].reshape(image.height(), image.width(), 3).copy()
        difference = np.abs(pixels(reference).astype(np.int16) - pixels(actual).astype(np.int16)).mean()
        if difference > 30:
            raise RuntimeError("视频画面与设备截图差异过大；请保持设备画面静止后复测")
        window.grab().save(str(artifacts / "HarmonyCast-preview.png"))
        window.switch_module(1)
        hilog = window.hilog
        window.grab().save(str(artifacts / "HarmonyCast-logs.png"))
        window.switch_module(0)
        window.toggle_fullscreen()
        app.processEvents()
        window.grab().save(str(artifacts / "HarmonyCast-fullscreen.png"))
        window.exit_fullscreen()
        window.store.export(artifacts / "smoke-device.log")
        report = {
            "device": device.serial,
            "model": device.model,
            "architecture": device.architecture,
            "preview": [window.screen.image.width(), window.screen.image.height()],
            "capture_mode": window.statusBar().currentMessage(),
            "snapshot_mean_absolute_pixel_difference": round(float(difference), 2),
            "hilog_entries": sum(entry.source == "HiLog" for entry in window.store.entries),
            "export_bytes": (artifacts / "smoke-device.log").stat().st_size,
        }
        window.stop_mirror()
        wait_for(app, lambda: hilog is not None and hilog.process is not None and hilog.process.poll() is not None, 5)
        report["hilog_stopped"] = True
        (artifacts / "device-verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps(report, ensure_ascii=False, indent=2))
    finally:
        window.close()
