"""Verify automatic, colored package logs and font shortcuts on the connected device."""
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
from PyQt5.QtTest import QTest
from PyQt5.QtWidgets import QApplication, QMessageBox, QPushButton

from harmonycast import ui


def wait_for(app, condition, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        app.processEvents()
        if condition():
            return
        QTest.qWait(25)
    raise RuntimeError("日志设备验证超时")


app = QApplication([])
app.setStyle("Fusion")
app.setStyleSheet(ui.STYLE)
QMessageBox.warning = lambda parent, title, message: print(f"{title}: {message}", file=sys.stderr)
artifacts = root / "artifacts/log-defaults"
artifacts.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="harmonycast-logs-") as temporary:
    ui.data_dir = lambda: Path(temporary) / "data"
    window = ui.MainWindow(QSettings(str(Path(temporary) / "settings.ini"), QSettings.IniFormat))
    window.show()
    try:
        window.switch_module(1)
        wait_for(app, lambda: window.selected_device() is not None and not window.scan_busy)
        wait_for(app, lambda: any(entry.source == "HiLog" for entry in window.store.entries))
        wait_for(app, lambda: not window.packages_loading)
        assert window.selected_package() == "com.yarward.*"
        entries = window.store.filtered(*window.filters())
        assert entries and all(entry.bundle.startswith("com.yarward.") and not entry.bundle.endswith(".*") for entry in entries)
        assert not {"开始采集", "停止采集", "刷新应用"} & {item.text() for item in window.findChildren(QPushButton)}
        window.log_view.setFocus()
        QTest.keyClick(window.log_view, Qt.Key_Equal, Qt.ControlModifier)
        assert window.log_font_size.value() == 12
        QTest.keyClick(window.log_view, Qt.Key_Minus, Qt.ControlModifier)
        assert window.log_font_size.value() == 11
        QTest.keyClick(window.log_view, Qt.Key_Equal, Qt.ControlModifier)
        QTest.keyClick(window.log_view, Qt.Key_0, Qt.ControlModifier)
        assert window.log_font_size.value() == 11
        app.processEvents()
        assert window.log_view.font().pointSize() == 11
        block = window.log_view.document().firstBlock()
        colors = set()
        while block.isValid():
            formats = block.layout().formats()
            if formats:
                colors.add(formats[0].format.foreground().color().name())
            block = block.next()
        assert colors
        window.store.export(artifacts / "yarward.log", *window.filters())
        window.grab().save(str(artifacts / "interface.png"))
        first = window.hilog
        window.switch_module(0)
        wait_for(app, lambda: not first.thread.is_alive())
        assert window.hilog is None
        window.switch_module(1)
        assert window.hilog is not None and window.hilog is not first
        window.category.setCurrentText("系统日志")
        wait_for(app, lambda: any(entry.category == "system" for entry in window.store.entries))
        system = window.store.filtered(*window.filters())
        assert system and all(entry.category == "system" for entry in system)
        window.category.setCurrentText("应用日志")
        report = {
            "device": window.selected_device().serial,
            "default_filter": window.selected_package(),
            "packages": sorted({entry.bundle for entry in entries}),
            "application_lines": len(entries),
            "application_list_count": window.package_combo.count() - 2,
            "colors": sorted(colors),
            "system_lines": len(system),
            "automatic_tab_collection": True,
            "font_shortcuts": True,
        }
    finally:
        final = window.hilog
        window.close()
        if final:
            assert not final.thread.is_alive()
        assert not window.workers
    report["clean_shutdown"] = True
    (artifacts / "verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))
