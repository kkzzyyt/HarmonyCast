import os
import time
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest
from PyQt5.QtCore import QBuffer, QIODevice, QPoint, QSettings, Qt
from PyQt5.QtGui import QColor, QImage
from PyQt5.QtTest import QTest
from PyQt5.QtWidgets import QApplication, QFileDialog, QPushButton

from harmonycast import ui
from harmonycast.core import Device, HDCError


@pytest.fixture(scope="module")
def app():
    application = QApplication.instance() or QApplication([])
    application.setStyle("Fusion")
    application.setStyleSheet(ui.STYLE)
    yield application


def wait_for(app, condition, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        app.processEvents()
        if condition():
            return
        QTest.qWait(20)
    assert condition(), "UI action did not finish"


class FakeLogSession:
    def __init__(self, hdc, serial, on_line, on_exit, category="", bundle="", on_status=None, on_package_line=None):
        self.serial, self.category, self.bundle = serial, category, bundle
        self.on_line, self.on_exit = on_line, on_exit
        self.on_status, self.on_package_line = on_status, on_package_line
        self.stopped = False

    def start(self):
        if self.on_status:
            self.on_status("等待应用启动")

    def stop(self):
        self.stopped = True


@pytest.fixture
def window(app, tmp_path, monkeypatch):
    class FakeHDC:
        executable = tmp_path / "hdc"
        calls = []
        def devices(self):
            return ["ABC"]
        def info(self, serial):
            return Device(serial, "测试设备", "OpenHarmony", "6.0", "riscv64")
        def run(self, args, device=None, timeout=8):
            self.calls.append((args, device))
            return ""
        def applications(self, serial):
            return ["com.example.app", "com.other.app"]
    fake = FakeHDC()
    monkeypatch.setattr(ui, "find_hdc", lambda custom="": fake.executable)
    monkeypatch.setattr(ui, "HDC", lambda selected: fake)
    monkeypatch.setattr(ui, "data_dir", lambda: tmp_path)
    monkeypatch.setattr("harmonycast.core.data_dir", lambda: tmp_path)
    monkeypatch.setattr(ui, "HiLogSession", FakeLogSession)
    monkeypatch.setattr("harmonycast.webview.fetch_webview_pages", lambda *args, **kwargs: [])
    image = QImage(64, 128, QImage.Format_RGB32)
    image.fill(QColor("#53d6b5"))
    buffer = QBuffer()
    buffer.open(QIODevice.WriteOnly)
    image.save(buffer, "PNG")
    raw = bytes(buffer.data())
    monkeypatch.setattr(ui.DeviceTools, "capture", lambda self, serial: raw)
    settings = QSettings(str(tmp_path / "settings.ini"), QSettings.IniFormat)
    settings.setValue("mode", 2)
    settings.setValue("auto_start_mirror", False)
    current = ui.MainWindow(settings)
    current.show()
    wait_for(app, lambda: current.selected_device() is not None and not current.scan_busy)
    yield current
    current.close()
    app.processEvents()


def test_device_scan_preview_touch_and_stop(app, window):
    assert window.device_list.count() == 1
    assert window.start_button.isEnabled()
    assert window.screen.cursor().shape() == Qt.ArrowCursor
    QTest.mouseClick(window.start_button, Qt.LeftButton)
    wait_for(app, lambda: not window.screen.image.isNull())
    assert window.projecting
    assert window.state_badge.text() == "投屏中"
    assert window.device_list.isEnabled()
    center = QPoint(window.screen.width() // 2, window.screen.height() // 2)
    window.screen._update_cursor(center)
    assert window.screen.cursor().shape() == Qt.ArrowCursor
    QTest.mousePress(window.screen, Qt.LeftButton, pos=center)
    assert window.screen.cursor().shape() == Qt.ArrowCursor
    QTest.mouseRelease(window.screen, Qt.LeftButton, pos=center)
    assert window.screen.cursor().shape() == Qt.ArrowCursor
    wait_for(app, lambda: any(args[:2] == ["shell", "uinput"] for args, device in window.hdc.calls))
    QTest.mouseClick(window.stop_button, Qt.LeftButton)
    assert not window.projecting
    assert window.device_list.isEnabled()
    assert window.state_badge.text() == "已停止"
    assert window.screen.cursor().shape() == Qt.ArrowCursor


def test_search_level_pause_clear_and_plain_log_export(app, window, tmp_path, monkeypatch):
    window.category.setCurrentText("全部日志")
    window.messages.put(("ERROR", "HiLog", "<error> hello needle"))
    window.messages.put(("INFO", "工具", "normal message"))
    window.pump()
    window.search.setText("needle")
    assert "hello needle" in window.log_view.toPlainText()
    assert "normal message" not in window.log_view.toPlainText()
    window.level.setCurrentText("INFO")
    assert window.log_view.toPlainText() == ""
    window.level.setCurrentText("ERROR")
    export = tmp_path / "filtered.log"
    monkeypatch.setattr(QFileDialog, "getSaveFileName", lambda *args, **kwargs: (str(export), ""))
    window.export_logs()
    assert "<error> hello needle" in export.read_text()
    assert "normal message" not in export.read_text()
    window.pause.setChecked(True)
    window.messages.put(("ERROR", "HiLog", "needle while paused"))
    window.pump()
    assert "while paused" not in window.log_view.toPlainText()
    window.pause.setChecked(False)
    assert "while paused" in window.log_view.toPlainText()
    window.clear_log_view()
    assert not window.store.entries
    assert window.log_view.toPlainText() == ""


def test_log_view_right_click_clear_and_smart_scroll(app, window):
    window.switch_module("logs")
    window.category.setCurrentText("全部日志")
    for i in range(30):
        window.messages.put(("INFO", "HiLog", f"log line {i}"))
    window.pump()
    app.processEvents()
    assert "log line 29" in window.log_view.toPlainText()

    # 测试右键清空 (clear_requested 信号触发清空)
    window.log_view.clear_requested.emit()
    assert window.log_view.toPlainText() == ""
    assert len(window.store.entries) == 0

    # 填充足够多的日志使出现滚动条
    for i in range(120):
        window.messages.put(("INFO", "HiLog", f"line {i:03d}"))
    window.pump()
    app.processEvents()

    scroll = window.log_view.verticalScrollBar()
    # 正常在底部时，新日志追加会继续滚动到底部
    assert scroll.value() == scroll.maximum()

    # 用户向上滑动滚动条
    scroll.setValue(10)
    app.processEvents()
    saved_pos = scroll.value()
    assert saved_pos < scroll.maximum()

    # 新日志输入时，不应该强制定位到底部，而应保持用户滚动位置
    window.messages.put(("INFO", "HiLog", "new incoming line while reading"))
    window.pump()
    app.processEvents()
    assert scroll.value() == saved_pos

    # 当用户切回自动滚动（或勾选）时，能够重新定位到底部
    window.autoscroll.toggled.emit(True)
    assert scroll.value() == scroll.maximum()


def test_screenshot_saves_a_valid_png_without_active_preview(app, window, tmp_path, monkeypatch):
    screenshot = tmp_path / "screen.png"
    monkeypatch.setattr(QFileDialog, "getSaveFileName", lambda *args, **kwargs: (str(screenshot), ""))
    window.save_screenshot()
    wait_for(app, lambda: screenshot.is_file() and screenshot.stat().st_size > 0)
    result = QImage(str(screenshot))
    assert not result.isNull()
    assert result.width() == 64 and result.height() == 128


def test_logs_automatically_collect_on_the_active_tab_without_manual_controls(app, window):
    assert window.hilog is None
    window.switch_module("logs")
    assert window.category.currentText() == "应用日志"
    assert window.selected_package() == "com.yarward.*"
    session = window.hilog
    assert session.bundle == "com.yarward.*"
    assert not {"开始采集", "停止采集", "刷新应用"} & {item.text() for item in window.findChildren(QPushButton)}
    session.on_package_line("10-14 12:00:00.123 123 456 E A05668/TestTag: a device error", "com.yarward.ohsmartward")
    window.pump()
    window.source.setCurrentText("HiLog")
    window.level.setCurrentText("ERROR")
    assert "a device error" in window.log_view.toPlainText()
    window.switch_module(0)
    wait_for(app, lambda: session.stopped)
    assert window.hilog is None
    window.switch_module("logs")
    assert window.hilog is not None and window.hilog is not session


def test_log_levels_have_distinct_colors_and_keep_plain_text_during_filtering(app, window):
    window.switch_module("logs")
    for level, initial in [("DEBUG", "D"), ("INFO", "I"), ("WARN", "W"), ("ERROR", "E"), ("FATAL", "F")]:
        window.messages.put((level, "HiLog", f"10-14 12:00:00.123 123 456 {initial} A05668/Tag: <literal> {level}", "com.yarward.ohsmartward"))
    window.pump()
    app.processEvents()
    document = window.log_view.document()
    colors = []
    block = document.firstBlock()
    while block.isValid():
        formats = block.layout().formats()
        assert formats, block.text()
        colors.append(formats[0].format.foreground().color().name())
        block = block.next()
    assert len(set(colors)) == 5
    assert colors[3] == "#ff8080"
    window.level.setCurrentText("ERROR")
    app.processEvents()
    assert "<literal> ERROR" in window.log_view.toPlainText()
    assert document.firstBlock().layout().formats()[0].format.foreground().color().name() == "#ff8080"


def test_log_font_size_control_and_shortcuts_preserve_lines_and_only_apply_on_logs_tab(app, window):
    window.switch_module("logs")
    window.messages.put(("ERROR", "HiLog", "10-14 12:00:00.123 123 456 E A05668/Tag: font sample", "com.yarward.ohsmartward"))
    window.pump()
    original = window.log_view.toPlainText()
    window.log_font_size.setValue(16)
    app.processEvents()
    assert window.log_view.font().pointSize() == 16
    assert window.log_view.document().defaultFont().pointSize() == 16
    assert int(window.settings.value("logs/font_size")) == 16
    window.log_view.setFocus()
    QTest.keyClick(window.log_view, Qt.Key_Equal, Qt.ControlModifier)
    assert window.log_font_size.value() == 17
    QTest.keyClick(window.log_view, Qt.Key_Minus, Qt.ControlModifier)
    assert window.log_font_size.value() == 16
    QTest.keyClick(window.log_view, Qt.Key_0, Qt.ControlModifier)
    assert window.log_font_size.value() == 11
    assert window.log_view.toPlainText() == original
    window.log_font_size.setValue(28)
    QTest.keyClick(window.log_view, Qt.Key_Equal, Qt.ControlModifier)
    assert window.log_font_size.value() == 28
    window.log_font_size.setValue(8)
    QTest.keyClick(window.log_view, Qt.Key_Minus, Qt.ControlModifier)
    assert window.log_font_size.value() == 8
    window.switch_module(1)
    window.screen.setFocus()
    QTest.keyClick(window.screen, Qt.Key_Equal, Qt.ControlModifier)
    assert window.log_font_size.value() == 8
    window.switch_module("logs")
    window.showFullScreen()
    app.processEvents()
    QTest.keyClick(window.screen, Qt.Key_Equal, Qt.ControlModifier)
    assert window.log_font_size.value() == 8
    window.showNormal()
    app.processEvents()


def test_saved_log_font_size_is_restored_when_reopening(app, window):
    window.log_font_size.setValue(18)
    window.close()
    restored = ui.MainWindow(window.settings)
    try:
        assert restored.log_font_size.value() == 18
        assert restored.log_view.font().pointSize() == 18
    finally:
        restored.close()


def test_logs_resume_automatically_after_device_reconnect_and_unexpected_exit(app, window):
    window.switch_module("logs")
    first = window.hilog
    device = window.selected_device()
    window.populate_devices([])
    wait_for(app, lambda: first.stopped)
    assert window.hilog is None
    window.populate_devices([device])
    assert window.hilog is not None
    failed = window.hilog
    failed.on_exit(1)
    wait_for(app, lambda: failed.stopped)
    wait_for(app, lambda: window.hilog is not None and window.hilog is not failed, timeout=5)


def test_logs_retry_device_discovery_after_a_temporary_connection_failure(app, window, monkeypatch):
    window.populate_devices([])
    original = window.hdc.info
    attempts = []
    def info(serial):
        attempts.append(serial)
        if len(attempts) == 1:
            raise HDCError("The communication channel is being established")
        return original(serial)
    monkeypatch.setattr(window.hdc, "info", info)
    window.switch_module("logs")
    window.maintain_logs()
    wait_for(app, lambda: not window.scan_busy)
    assert window.hilog is None
    window.maintain_logs()
    wait_for(app, lambda: window.hilog is not None and not window.scan_busy)
    assert window.selected_package() == "com.yarward.*"
    assert len(attempts) == 2


def test_modules_are_separate_and_mirroring_stays_active_when_viewing_logs(app, window):
    # 精简为 3 个主模块：设备信息、投屏工作台、常用命令
    assert window.pages.count() == 3
    assert window.pages.currentWidget() is window.info_panel
    assert not window.log_panel.isVisible()
    assert not window.hdc_panel.isVisible()
    assert not window.webview_panel.isVisible()
    QTest.mouseClick(window.hdc_nav, Qt.LeftButton)
    assert window.pages.currentWidget() is window.hdc_panel
    assert window.page_title.text() == "常用命令"
    QTest.mouseClick(window.mirror_nav, Qt.LeftButton)
    assert window.pages.currentWidget() is window.workspace_page
    assert window.page_title.text() == "投屏工作台"
    QTest.mouseClick(window.start_button, Qt.LeftButton)
    wait_for(app, lambda: not window.screen.image.isNull())
    assert window.projecting
    # 切换至仅日志布局：投屏自动休眠，释放设备与电脑资源
    window.set_workspace_layout("logs_only")
    assert not window.mirror_page.isVisible()
    assert window.log_panel.isVisible()
    assert not window.projecting
    assert window.state_badge.text() == "已休眠"
    QTest.mouseClick(window.info_nav, Qt.LeftButton)
    assert window.pages.currentWidget() is window.info_panel
    assert window.page_title.text() == "设备信息"
    assert not window.projecting
    # 切回投屏工作台：投屏自动唤醒恢复
    QTest.mouseClick(window.mirror_nav, Qt.LeftButton)
    assert window.pages.currentWidget() is window.workspace_page
    wait_for(app, lambda: window.projecting)
    assert window.projecting


def test_fullscreen_hides_navigation_and_escape_restores_selected_module(app, window):
    window.toggle_fullscreen()
    app.processEvents()
    assert window.isFullScreen()
    assert not window.sidebar.isVisible()
    assert not window.header.isVisible()
    assert not window.mirror_toolbar.isVisible()
    assert window.screen.isVisible()
    QTest.keyClick(window, Qt.Key_Escape)
    app.processEvents()
    assert not window.isFullScreen()
    assert window.sidebar.isVisible()
    assert window.header.isVisible()
    assert window.pages.currentWidget() is window.workspace_page


def test_double_click_does_not_fullscreen(app, window):
    gestures = []
    window.screen.gesture.connect(lambda s, e, d: gestures.append((s, e, d)))
    window.start_mirror()
    wait_for(app, lambda: not window.screen.image.isNull())
    assert not window.isFullScreen()
    center = QPoint(window.screen.width() // 2, window.screen.height() // 2)
    QTest.mouseDClick(window.screen, Qt.LeftButton, pos=center)
    QTest.mouseRelease(window.screen, Qt.LeftButton, pos=center)
    app.processEvents()
    assert not window.isFullScreen()
    QTest.mouseClick(window.stop_button, Qt.LeftButton)


def test_screen_jitter_click_detection(app, window):
    gestures = []
    window.screen.gesture.connect(lambda s, e, d: gestures.append((s, e, d)))
    window.start_mirror()
    wait_for(app, lambda: not window.screen.image.isNull())
    center = QPoint(window.screen.width() // 2, window.screen.height() // 2)
    # Press and release with slight micro-movement jitter (3, 4 px)
    QTest.mousePress(window.screen, Qt.LeftButton, pos=center)
    QTest.mouseRelease(window.screen, Qt.LeftButton, pos=center + QPoint(3, 4))
    app.processEvents()
    assert gestures
    start, end, duration = gestures[-1]
    assert start == end  # micro movement must be recognized as a tap
    QTest.mouseClick(window.stop_button, Qt.LeftButton)


def test_click_does_not_trigger_capture(app, window, monkeypatch):
    window.start_mirror()
    wait_for(app, lambda: not window.screen.image.isNull())
    calls_before = len(window.hdc.calls)
    # Send a click gesture
    window.send_gesture((100, 200), (100, 200), 100)
    wait_for(app, lambda: any(args[:2] == ["shell", "uinput"] for args, device in window.hdc.calls[calls_before:]))

    # Verify uinput event was sent
    uinput_calls = [args for args, device in window.hdc.calls[calls_before:] if args[:2] == ["shell", "uinput"]]
    assert len(uinput_calls) > 0

    QTest.mouseClick(window.stop_button, Qt.LeftButton)


def test_rotate_display_button_and_touch_coordinate_mapping(app, window):
    window.user_rotation = 0
    assert window.rotate_button.text() == "旋转"
    QTest.mouseClick(window.rotate_button, Qt.LeftButton)
    assert window.user_rotation == 90
    assert window.screen.user_rotation == 90
    QTest.mouseClick(window.rotate_button, Qt.LeftButton)
    assert window.user_rotation == 180
    QTest.mouseClick(window.rotate_button, Qt.LeftButton)
    assert window.user_rotation == 270
    QTest.mouseClick(window.rotate_button, Qt.LeftButton)
    assert window.user_rotation == 0
    assert window.screen.user_rotation == 0

    # Test coordinate mapping with rotation
    window.screen.device_size = (1000, 2000)
    test_img = QImage(1000, 2000, QImage.Format_RGB32)
    window.screen.set_image(test_img, (1000, 2000))
    window.screen.resize(1000, 2000)
    pos = QPoint(100, 200)

    # At 0 rotation: (100, 200)
    window.screen.user_rotation = 0
    assert window.screen.coordinates(pos) == (100, 200)

    # At 180 rotation: (1000 - 1 - 100, 2000 - 1 - 200) = (899, 1799)
    window.screen.user_rotation = 180
    assert window.screen.coordinates(pos) == (899, 1799)


def test_screen_coordinates_scaling_with_720p_adaptive_resolution(app, window):
    window.screen.device_size = (1080, 1920)
    img_720p = QImage(720, 1280, QImage.Format_RGB32)
    window.screen.set_image(img_720p, (1080, 1920))
    window.screen.resize(720, 1280)

    center_pos = QPoint(360, 640)
    window.screen.user_rotation = 0
    assert window.screen.coordinates(center_pos) == (540, 960)
    assert window.screen.coordinates(QPoint(0, 0)) == (0, 0)
    assert window.screen.coordinates(QPoint(719, 1279)) == (1079, 1919)

    window.screen.user_rotation = 180
    assert window.screen.coordinates(QPoint(0, 0)) == (1079, 1919)


def test_quality_selection_toolbar_and_settings(app, window):
    assert window.quality_combo.count() == 3
    assert window.quality_combo.currentIndex() == 0
    assert window.quality.currentIndex() == 0

    # Switch to 1.5M
    window.quality_combo.setCurrentIndex(1)
    assert window.quality.currentIndex() == 1
    assert window.settings.value("quality") == 1
    assert window.bitrate.currentIndex() == 0

    # Switch back to 4M
    window.quality_combo.setCurrentIndex(0)
    assert window.quality.currentIndex() == 0
    assert window.settings.value("quality") == 0
    assert window.bitrate.currentIndex() == 1



def test_closing_waits_for_capture_worker_cleanup(app, window, monkeypatch):
    import threading
    entered, release, finished = threading.Event(), threading.Event(), threading.Event()
    original_capture = ui.DeviceTools.capture
    def capture(tools, serial):
        entered.set()
        release.wait(timeout=2)
        finished.set()
        return original_capture(tools, serial)
    monkeypatch.setattr(ui.DeviceTools, "capture", capture)
    window.start_mirror()
    assert entered.wait(timeout=2)
    threading.Timer(0.1, release.set).start()
    window.close()
    assert finished.is_set()
    assert not window.workers


def test_native_window_fullscreen_also_hides_chrome(app, window):
    window.showFullScreen()
    app.processEvents()
    assert not window.sidebar.isVisible()
    assert not window.header.isVisible()
    window.showNormal()
    app.processEvents()
    assert window.sidebar.isVisible()
    assert window.header.isVisible()


def test_native_display_rotation_and_alignment_padding_match_logical_pixels(app):
    # The physical display is landscape; the logical touch coordinates are portrait.
    physical = QImage(1920, 1088, QImage.Format_RGB32)
    physical.fill(QColor("red"))
    result = ui.orient_native_image(physical, (1080, 1920), 90)
    assert (result.width(), result.height()) == (1080, 1920)
    assert result.pixelColor(0, 0) == QColor("red")


def test_accessibility_navigation_selection_switches_the_actual_page(app, window):
    window.hdc_nav.setChecked(True)
    assert window.pages.currentWidget() is window.hdc_panel
    assert not window.mirror_nav.isChecked()
    assert not window.info_nav.isChecked()

    window.info_nav.setChecked(True)
    assert window.pages.currentWidget() is window.info_panel
    assert not window.hdc_nav.isChecked()
    assert not window.mirror_nav.isChecked()

    window.mirror_nav.setChecked(True)
    assert window.pages.currentWidget() is window.workspace_page
    assert not window.info_nav.isChecked()
    assert not window.hdc_nav.isChecked()


def test_application_and_system_filters_apply_to_live_rows_and_export(app, window, tmp_path, monkeypatch):
    window.switch_module("logs")
    window.package_combo.setCurrentText("全部应用")
    window.commit_package()
    window.messages.put(("ERROR", "HiLog", "10-14 12:00:00.123 123 456 E A00001/MyApp: needle app error"))
    window.messages.put(("ERROR", "HiLog", "10-14 12:00:00.123 321 654 E C00001/CoreService: needle system error"))
    window.messages.put(("INFO", "HiLog", "10-14 12:00:00.123 123 456 I A00001/MyApp: needle app info"))
    window.pump()
    window.category.setCurrentText("应用日志")
    window.search.setText("needle")
    window.level.setCurrentText("ERROR")
    assert "app error" in window.log_view.toPlainText()
    assert "system error" not in window.log_view.toPlainText()
    assert "app info" not in window.log_view.toPlainText()
    window.messages.put(("ERROR", "HiLog", "10-14 12:00:01.123 321 654 E C00001/CoreService: needle new system"))
    window.pump()
    assert "new system" not in window.log_view.toPlainText()
    output = tmp_path / "filtered-app.log"
    monkeypatch.setattr(QFileDialog, "getSaveFileName", lambda *args, **kwargs: (str(output), ""))
    window.export_logs()
    assert "app error" in output.read_text() and "system" not in output.read_text()
    window.category.setCurrentText("系统日志")
    assert "system error" in window.log_view.toPlainText()
    assert "new system" in window.log_view.toPlainText()
    assert "app error" not in window.log_view.toPlainText()


def test_switching_category_updates_collection_without_stopping_mirroring(app, window, monkeypatch):
    window.switch_module(1)
    window.start_mirror()
    wait_for(app, lambda: not window.screen.image.isNull())
    assert window.projecting
    window.category.setCurrentText("应用日志")
    window.switch_module("logs")
    assert window.hilog.category == "app"
    # 切至日志页时投屏服务休眠，但保留最后一帧画面绝不黑屏
    assert not window.projecting
    assert not window.screen.image.isNull()
    first = window.hilog
    window.category.setCurrentText("系统日志")
    wait_for(app, lambda: first.stopped)
    assert window.hilog.category == "system"
    # 切回投屏页时自动唤醒且画面保持连续
    window.switch_module(1)
    wait_for(app, lambda: window.projecting)
    assert window.projecting
    assert not window.screen.image.isNull()


def test_log_panel_hides_redundant_source_dropdown_and_fetch_button(app, window):
    window.switch_module("logs")
    assert not window.source.isVisible()
    assert not window.fetch_button.isVisible()
    # 验证应用包名组件处于第一行且布局正确，提示标签隐藏
    assert window.package_row.isVisible()
    assert window.package_combo.isVisible()
    assert not window.package_hint.isVisible()

    # 验证第一行操作栏已精简，去除了重复的“拉取应用日志”和“导出故障现场”
    buttons = [b.text() for b in window.log_panel.findChildren(QPushButton) if b.isVisible()]
    assert "拉取应用日志" not in buttons
    assert "导出故障现场" not in buttons
    assert any("导出日志" in b for b in buttons)


def test_package_selector_is_searchable_and_only_visible_for_application_logs(app, window):
    window.switch_module("logs")
    assert window.package_row.isVisible()
    window.category.setCurrentText("应用日志")
    wait_for(app, lambda: window.package_combo.count() == 4)
    assert window.package_row.isVisible()
    assert window.package_combo.isEditable()
    assert window.package_combo.completer().filterMode() == Qt.MatchContains
    window.package_combo.setCurrentText("com.example.app")
    assert window.commit_package()
    assert window.selected_package() == "com.example.app"
    window.category.setCurrentText("系统日志")
    assert not window.package_row.isVisible()
    assert window.selected_package() == ""


def test_package_dropdown_arrow_opens_the_device_application_list(app, window):
    window.switch_module("logs")
    window.category.setCurrentText("应用日志")
    wait_for(app, lambda: window.package_combo.count() == 4)
    combo = window.package_combo
    QTest.mouseClick(combo, Qt.LeftButton, pos=QPoint(combo.width() - 12, combo.height() // 2))
    app.processEvents()
    assert combo.view().isVisible()
    index = combo.model().index(combo.findText("com.example.app"), 0)
    QTest.mouseMove(combo.view().viewport(), combo.view().visualRect(index).center())
    QTest.mouseClick(combo.view().viewport(), Qt.LeftButton, pos=combo.view().visualRect(index).center())
    app.processEvents()
    assert window.selected_package() == "com.example.app"


def test_package_filter_and_export_exclude_other_apps_even_when_their_message_mentions_the_package(app, window, tmp_path, monkeypatch):
    window.switch_module("logs")
    window.category.setCurrentText("应用日志")
    wait_for(app, lambda: window.package_combo.count() == 4)
    window.package_combo.setCurrentText("com.example.app")
    window.commit_package()
    window.messages.put(("INFO", "HiLog", "10-14 12:00:00.123 101 101 I A00001/Tag: target", "com.example.app"))
    window.messages.put(("INFO", "HiLog", "10-14 12:00:00.123 202 202 I A00001/Tag: com.example.app mentioned", "com.other.app"))
    window.pump()
    assert "target" in window.log_view.toPlainText()
    assert "mentioned" not in window.log_view.toPlainText()
    export = tmp_path / "selected-app.log"
    monkeypatch.setattr(QFileDialog, "getSaveFileName", lambda *args, **kwargs: (str(export), ""))
    window.export_logs()
    assert "com.example.app" in export.read_text()
    assert "mentioned" not in export.read_text()


def test_selecting_a_package_restarts_collection_and_keeps_its_lines_scoped_to_that_package(app, window, monkeypatch):
    window.switch_module("logs")
    window.category.setCurrentText("应用日志")
    wait_for(app, lambda: window.package_combo.count() == 4)
    previous = window.hilog
    window.package_combo.setCurrentText("com.example.app")
    window.commit_package()
    wait_for(app, lambda: previous.stopped)
    assert window.hilog.bundle == "com.example.app"
    app.processEvents()
    assert "等待应用启动" in window.package_hint.text()
    window.hilog.on_line("10-14 12:00:00.123 101 101 I A00001/Tag: target")
    window.pump()
    assert window.store.entries[-1].bundle == "com.example.app"
    window.switch_module(0)
    assert window.hilog is None


def test_auto_reconnect_on_stream_disconnect(app, window):
    window.start_mirror()
    wait_for(app, lambda: window.projecting)
    token = window.active_token
    # Trigger unexpected disconnect
    window.on_stream_finished(token, "网络连接重置")
    assert window._reconnect_attempts == 1
    assert "重连中" in window.state_badge.text()
    # User clicks stop to cancel
    window.stop_mirror()
    assert window._user_stopped_mirror
    assert window._reconnect_attempts == 0
    assert window.state_badge.text() == "已停止"


def test_no_auto_reconnect_when_no_hdc_device(app, window):
    window.start_mirror()
    wait_for(app, lambda: window.projecting)
    token = window.active_token

    # 模拟设备离线或无 HDC 设备连接
    window.devices = []
    window.on_stream_finished(token, "设备已断开连接")

    # 验证不触发自动重连，直接停止投屏
    assert window._reconnect_attempts == 0
    assert not window.projecting
    assert window.state_badge.text() == "已停止"


def test_device_info_rendering_and_actions(app, window):
    sample_data = {
        "serial": "ABC123456",
        "brand": "YAWARD",
        "model": "YH002",
        "model_code": "ohos",
        "manufacturer": "YARWARD",
        "hardware_version": "1.0.0.0",
        "software_version": "1.0.0.1 (V0.1)",
        "os_fullname": "OpenHarmony-6.1.0.31",
        "api_version": "23",
        "security_patch": "2024-05",
        "cpu_abi": "riscv64",
        "udid": "6BB5EA6EFC19821C",
        "kernel": "Linux 5.10.0 riscv64",
        "uptime": "10 小时 20 分钟 30 秒",
        "memory": "已用 1.20 GB / 总计 3.84 GB",
        "storage": "已用 2.1G / 总计 16G (空闲 13G)",
        "display": "1080 × 1920",
        "networks": [
            {
                "interface": "eth0",
                "name": "有线以太网 (eth)",
                "mac": "fe:fe:fe:c3:93:68",
                "ip": "192.168.1.100",
                "connected": True,
            },
            {
                "interface": "wlan0",
                "name": "无线局域网 (Wi-Fi)",
                "mac": "b0:8c:b3:89:9e:b8",
                "ip": "未连接 / 未分配 IP",
                "connected": False,
            },
        ],
        "yarward_apps": [
            {
                "bundle": "com.yarward.ohsmartward",
                "version_name": "3.5.0.2026101001",
                "version_code": 2026101001,
                "target_api": 23,
                "main_ability": "EtnroomAbility",
                "update_time": "2026-10-10 15:30:00",
                "install_time": "2026-10-10 15:30:00",
                "running": True,
                "pids": ["3308"],
            }
        ],
        "all_packages": ["com.yarward.ohsmartward", "com.huawei.hmos.settings", "com.ohos.launcher"],
    }
    window.switch_module(0)
    assert window.pages.currentWidget() is window.info_panel
    window.render_device_info(sample_data)

    assert "YH002" in window.info_badge.text()
    assert window.pkg_table.rowCount() == 3
    # Test package filter
    window._filter_package_table("settings")
    assert not window.pkg_table.isRowHidden(1)
    assert window.pkg_table.isRowHidden(0)
    window._filter_package_table("")
    assert not window.pkg_table.isRowHidden(0)

    # Test copy actions
    window.copy_all_device_info()
    clipboard_text = QApplication.clipboard().text()
    assert "fe:fe:fe:c3:93:68" in clipboard_text
    assert "3.5.0.2026101001" in clipboard_text
    assert "YH002" in clipboard_text


def test_auto_start_mirror_when_device_connected(app, tmp_path, monkeypatch):
    class FakeHDC:
        executable = tmp_path / "hdc"
        calls = []
        def devices(self):
            return ["AUTO_DEV"]
        def info(self, serial):
            return Device(serial, "自动投屏测试设备", "OpenHarmony", "6.0", "riscv64")
        def run(self, args, device=None, timeout=8):
            return ""
        def applications(self, serial):
            return []
    fake = FakeHDC()
    monkeypatch.setattr(ui, "find_hdc", lambda custom="": fake.executable)
    monkeypatch.setattr(ui, "HDC", lambda selected: fake)
    image = QImage(64, 128, QImage.Format_RGB32)
    image.fill(QColor("#53d6b5"))
    buffer = QBuffer()
    buffer.open(QIODevice.WriteOnly)
    image.save(buffer, "PNG")
    monkeypatch.setattr(ui.DeviceTools, "capture", lambda self, serial: bytes(buffer.data()))
    settings = QSettings(str(tmp_path / "settings_auto.ini"), QSettings.IniFormat)
    settings.setValue("mode", 2)
    settings.setValue("auto_start_mirror", True)
    window = ui.MainWindow(settings)
    try:
        window.show()
        wait_for(app, lambda: not window.screen.image.isNull(), timeout=4)
        assert window.projecting
        assert window.state_badge.text() == "投屏中"
    finally:
        window.close()


def test_portrait_device_orientation_and_full_display(app):
    # Physical landscape capture buffer 1920x1080 (or padded 1920x1088)
    physical_frame = QImage(1920, 1088, QImage.Format_RGB32)
    physical_frame.fill(QColor("blue"))
    # Device logical size is 1080x1920 (portrait), native rotation is 90
    oriented = ui.orient_native_image(physical_frame, (1080, 1920), 90, 0)
    assert (oriented.width(), oriented.height()) == (1080, 1920)
    assert oriented.pixelColor(0, 0) == QColor("blue")

    # When user applies manual 90° rotation
    user_rotated = ui.orient_native_image(physical_frame, (1080, 1920), 90, 90)
    assert (user_rotated.width(), user_rotated.height()) == (1920, 1080)


def test_hdc_panel_navigation_and_preset_templates(app, window):
    window.switch_module("hdc")
    assert window.pages.currentWidget() is window.hdc_panel
    assert window.page_title.text() == "常用命令"
    assert "ABC" in window.hdc_target_badge.text()
    assert "-t ABC" in window.hdc_target_prefix.text()

    # Preset selection
    idx = window.hdc_preset_combo.findText("shell param get")
    assert idx > 0
    window.hdc_preset_combo.setCurrentIndex(idx)
    assert window.hdc_input.text() == "shell param get"

    idx2 = window.hdc_preset_combo.findText("shell bm dump -a")
    assert idx2 > 0
    window.hdc_preset_combo.setCurrentIndex(idx2)
    assert window.hdc_input.text() == "shell bm dump -a"

    idx_mount = window.hdc_preset_combo.findText("target mount")
    assert idx_mount > 0
    window.hdc_preset_combo.setCurrentIndex(idx_mount)
    assert window.hdc_input.text() == "target mount"


def test_hdc_panel_quick_keys(app, window, monkeypatch):
    window.switch_module("hdc")
    executed_cmds = []

    class FakeProc:
        returncode = 0
        def communicate(self, timeout=None):
            return "you raised the key 1\n", ""

    def mock_popen(cmd, *args, **kwargs):
        executed_cmds.append(cmd)
        return FakeProc()

    monkeypatch.setattr(ui.subprocess, "Popen", mock_popen)
    window.quick_hdc_key("home", "返回桌面")
    wait_for(app, lambda: any("uinput" in str(c) for c in executed_cmds))
    assert any("-d 1 -u 1" in " ".join(c) for c in executed_cmds)
    wait_for(app, lambda: "[✓ 执行完成" in window.hdc_terminal.toPlainText())


def test_hdc_panel_custom_command_execution(app, window, monkeypatch):
    window.switch_module("hdc")

    class FakeProc:
        returncode = 0
        def communicate(self, timeout=None):
            return "Linux localhost 5.10.0 riscv64\n", ""
        def kill(self):
            pass
        def terminate(self):
            pass

    monkeypatch.setattr(ui.subprocess, "Popen", lambda *args, **kwargs: FakeProc())
    window.hdc_input.setText("shell uname -a")
    window.btn_run_hdc.click()

    wait_for(app, lambda: "Linux localhost" in window.hdc_terminal.toPlainText(), timeout=4)
    assert window.btn_run_hdc.isEnabled()
    assert not window.btn_kill_hdc.isEnabled()
    assert "[✓ 执行完成" in window.hdc_terminal.toPlainText()
    assert "shell uname -a" in window.hdc_terminal.toPlainText()


def test_hdc_panel_clear_copy_export(app, window, tmp_path, monkeypatch):
    window.switch_module("hdc")
    window._append_hdc_output("test console output line\n")
    assert "test console output line" in window.hdc_terminal.toPlainText()

    # Copy
    window.copy_hdc_output()
    assert "test console output line" in QApplication.clipboard().text()

    # Export
    out_file = tmp_path / "hdc_export.txt"
    monkeypatch.setattr(QFileDialog, "getSaveFileName", lambda *args, **kwargs: (str(out_file), ""))
    window.export_hdc_output()
    assert out_file.exists()
    assert "test console output line" in out_file.read_text(encoding="utf-8")

    # Clear
    window.clear_hdc_terminal()
    assert window.hdc_terminal.toPlainText() == ""


def test_hdc_panel_interrupt(app, window):
    window.switch_module("hdc")

    class FakeLongProc:
        terminated = False
        def terminate(self):
            self.terminated = True

    fake = FakeLongProc()
    with window._hdc_proc_lock:
        window._current_hdc_proc = fake

    window.interrupt_hdc_command()
    assert fake.terminated
    assert "[!] 已向进程发送终止信号" in window.hdc_terminal.toPlainText()


def test_screen_view_touch_indicator_and_trajectory(app):
    from PyQt5.QtGui import QMouseEvent, QPainter, QPixmap
    screen = ui.ScreenView()
    screen.resize(400, 600)
    screen.active = True
    img = QImage(360, 640, QImage.Format_RGB32)
    img.fill(QColor("#111923"))
    screen.set_image(img)

    assert screen.show_touches is True
    assert screen._touch_active is False
    assert screen._touch_pos is None

    # 1. Mouse press
    press_pos = QPoint(150, 200)
    event_press = QMouseEvent(QMouseEvent.MouseButtonPress, press_pos, Qt.LeftButton, Qt.LeftButton, Qt.NoModifier)
    screen.mousePressEvent(event_press)

    assert screen._touch_active is True
    assert screen._touch_pos == press_pos
    assert len(screen._touch_trail) == 1
    assert screen._ripple_opacity > 0

    # 2. Mouse move (drag)
    move_pos = QPoint(160, 240)
    event_move = QMouseEvent(QMouseEvent.MouseMove, move_pos, Qt.LeftButton, Qt.LeftButton, Qt.NoModifier)
    screen.mouseMoveEvent(event_move)

    assert screen._touch_pos == move_pos
    assert len(screen._touch_trail) == 2

    # 3. Paint with touch indicator
    pixmap = QPixmap(400, 600)
    painter = QPainter(pixmap)
    screen.render(painter)
    painter.end()

    # 4. Mouse release (starts fade)
    event_release = QMouseEvent(QMouseEvent.MouseButtonRelease, move_pos, Qt.LeftButton, Qt.NoButton, Qt.NoModifier)
    screen.mouseReleaseEvent(event_release)

    assert screen._touch_active is False
    assert screen._fade_timer.isActive()

    # 5. Fade timer ticks
    initial_radius = screen._ripple_radius
    initial_opacity = screen._ripple_opacity
    screen._on_fade_tick()
    assert screen._ripple_radius > initial_radius
    assert screen._ripple_opacity < initial_opacity

    # Finish fading
    screen._ripple_opacity = 0.05
    screen._on_fade_tick()
    assert screen._ripple_opacity == 0.0
    assert screen._touch_pos is None
    assert len(screen._touch_trail) == 0
    assert not screen._fade_timer.isActive()

    # 6. Disabled touches
    screen.show_touches = False
    screen.mousePressEvent(event_press)
    assert screen._touch_active is False
    assert screen._touch_pos is None


def test_mainwindow_fault_report_export(app, window, tmp_path, monkeypatch):
    import zipfile
    out_zip = tmp_path / "fault_test_output.zip"
    monkeypatch.setattr(QFileDialog, "getSaveFileName", lambda *args, **kwargs: (str(out_zip), ""))
    window._suppress_dialogs = True

    # Click or invoke export_fault_report
    window.export_fault_report()
    wait_for(app, lambda: "已导出" in window.statusBar().currentMessage())

    assert out_zip.stat().st_size > 0
    with zipfile.ZipFile(out_zip, "r") as zf:
        names = zf.namelist()
        assert "REPORT_SUMMARY.md" in names
        assert "screenshot.png" in names
        assert "device_info.json" in names
        assert "system_parameters.txt" in names
        assert "running_processes.txt" in names
        assert "network_ifconfig.txt" in names

        summary = zf.read("REPORT_SUMMARY.md").decode("utf-8")
        assert "# HarmonyCast 故障现场诊断报告" in summary


def test_mainwindow_toggle_touch_indicator(app, window):
    window.toggle_touch_indicator(False)
    assert window.screen.show_touches is False
    assert int(window.settings.value("show_touches")) == 0

    window.toggle_touch_indicator(True)
    assert window.screen.show_touches is True
    assert int(window.settings.value("show_touches")) == 1


def test_mainwindow_more_button_removed(app, window):
    # 验证投屏模块中的“更多”选项已被彻底移除
    assert not hasattr(window, "more_button")
    assert not hasattr(window, "deploy_button")


def test_mainwindow_icu_card_rendering(app, window):
    data = {
        "serial": "TEST1234",
        "model": "YH-H36M",
        "icu": {
            "available": True,
            "model": "H36M",
            "mcu_version": "v1.9",
            "hardware_version": "v1.0",
            "watchdog_stopped": True,
            "watchdog_desc": "已暂停 (调试防复位)",
            "power_delay": "0",
        },
        "yarward_apps": [
            {
                "bundle": "com.yarward.ohsmartward",
                "version_name": "1.0.0",
                "version_code": 100,
                "target_api": 10,
                "running": True,
                "pids": ["1234"],
            }
        ],
        "networks": [],
        "all_packages": ["com.yarward.ohsmartward"],
    }
    window.render_device_info(data)
    assert window.card_icu.layout().count() > 0


def test_mainwindow_watchdog_actions(app, window, monkeypatch):
    tools_calls = []

    class FakeTools:
        def __init__(self, hdc):
            pass
        def set_watchdog_status(self, serial, stop):
            tools_calls.append(("set_watchdog", serial, stop))
            return "ok"
        def get_icu_info(self, serial):
            return {
                "available": True,
                "model": "H36M",
                "mcu_version": "v1.9",
                "watchdog_stopped": True,
                "watchdog_desc": "已暂停 (调试防复位)",
                "power_delay": "0",
            }
        def feed_watchdog(self, serial):
            tools_calls.append(("feed_watchdog", serial))
            return "ok"

    monkeypatch.setattr(ui, "DeviceTools", FakeTools)

    window.set_watchdog_status_action(True)
    wait_for(app, lambda: any(c[0] == "set_watchdog" for c in tools_calls))
    assert ("set_watchdog", "ABC", True) in tools_calls

    window.feed_watchdog_action()
    wait_for(app, lambda: any(c[0] == "feed_watchdog" for c in tools_calls))
    assert ("feed_watchdog", "ABC") in tools_calls


def test_mainwindow_fetch_app_logs_action(app, window, tmp_path, monkeypatch):
    fake_log_file = tmp_path / "etnroom_2026-10-15.log"
    fake_log_file.write_text("dummy log content")

    class FakeTools:
        def __init__(self, hdc):
            pass
        def fetch_app_logs(self, serial, dest_dir, bundle="com.yarward.*"):
            return [fake_log_file]

    monkeypatch.setattr(ui, "DeviceTools", FakeTools)
    monkeypatch.setattr(QFileDialog, "getExistingDirectory", lambda *args, **kwargs: str(tmp_path))
    monkeypatch.setattr(ui.QMessageBox, "information", lambda *args, **kwargs: ui.QMessageBox.Ok)

    window.fetch_app_logs_action("com.yarward.ohsmartward")
    wait_for(app, lambda: "已拉取 1 个应用日志" in window.statusBar().currentMessage())


def test_network_device_dialog_crud_and_connect(app, tmp_path, monkeypatch):
    monkeypatch.setattr("harmonycast.core.data_dir", lambda: tmp_path)
    connected_endpoints = []

    dialog = ui.NetworkDeviceDialog(on_connect=lambda ep: connected_endpoints.append(ep))

    # 初始状态没有设备
    assert dialog.table.rowCount() == 1
    assert "暂无设备" in dialog.table.item(0, 0).text()

    # 未输入 IP 点击保存提示错误
    dialog._on_save_only()
    assert "请输入设备 IP 地址" in dialog.error_label.text()

    # 输入无效 IP 提示格式错误
    dialog.ip_input.setText("999.1.1.1")
    dialog._on_save_only()
    assert "IP 地址格式不正确" in dialog.error_label.text()

    # 成功保存设备 1
    dialog.ip_input.setText("192.168.1.101")
    dialog.name_input.setText("1床监控")
    dialog._on_save_only()
    assert dialog.error_label.text() == ""
    assert dialog.table.rowCount() == 1
    assert dialog.table.item(0, 0).text() == "192.168.1.101"
    assert dialog.table.item(0, 1).text() == "8710"
    assert dialog.table.item(0, 2).text() == "○ 未连接"

    # 双击该行直接连接
    dialog._on_table_double_clicked(0, 0)
    assert connected_endpoints == ["192.168.1.101:8710"]

    # 连接并保存新设备
    dialog.ip_input.setText("192.168.1.102")
    dialog.name_input.setText("2床分机")
    dialog._on_connect_and_save()
    assert connected_endpoints[-1] == "192.168.1.102:8710"
    assert dialog.table.rowCount() == 2

    # 删除取消测试 (弹窗点击取消，不删除)
    monkeypatch.setattr(ui.QMessageBox, "question", lambda *args, **kwargs: ui.QMessageBox.No)
    dialog._delete_target("192.168.1.101")
    assert dialog.table.rowCount() == 2

    # 删除确认测试 (弹窗点击确认，删除成功)
    monkeypatch.setattr(ui.QMessageBox, "question", lambda *args, **kwargs: ui.QMessageBox.Yes)
    dialog._delete_target("192.168.1.101")
    assert dialog.table.rowCount() == 1
    assert dialog.table.item(0, 0).text() == "192.168.1.102"

    # 自定义端口测试 (新保存的设备置顶)
    dialog.ip_input.setText("192.168.1.103")
    dialog.port_input.setText("5555")
    dialog._on_save_only()
    assert dialog.table.rowCount() == 2
    assert dialog.table.item(0, 0).text() == "192.168.1.103"
    assert dialog.table.item(0, 1).text() == "5555"
    assert dialog.table.item(1, 0).text() == "192.168.1.102"


def test_network_device_dialog_disconnect_and_reconnect(app, tmp_path, monkeypatch):
    monkeypatch.setattr("harmonycast.core.data_dir", lambda: tmp_path)
    ui.save_device("192.168.1.105", name="已连设备")

    events = []
    dialog = ui.NetworkDeviceDialog(
        on_connect=lambda ep: events.append(("connect", ep)),
        on_disconnect=lambda ep: events.append(("disconnect", ep)),
        on_reconnect=lambda ep: events.append(("reconnect", ep)),
        connected_targets=["192.168.1.105:8710"],
    )

    assert dialog.table.rowCount() == 1
    assert dialog.table.item(0, 2).text() == "● 已连接"
    assert dialog.table.item(0, 0).textAlignment() == Qt.AlignCenter
    assert dialog.table.horizontalHeader().stretchLastSection() is True
    assert not dialog.btn_connect.icon().isNull()
    assert dialog.table.rowHeight(0) >= 40
    assert dialog.table.horizontalHeader().sectionResizeMode(0) == ui.QHeaderView.Interactive

    # 在线状态：操作列应包含 3 个图标按钮 (投屏、断开、删除)
    cell_act = dialog.table.cellWidget(0, 3)
    btns = cell_act.findChildren(QPushButton)
    assert len(btns) == 3
    assert all(not b.icon().isNull() for b in btns)
    assert any("断开" in b.toolTip() for b in btns)
    assert any("删除" in b.toolTip() for b in btns)

    # 点击断开
    dialog._disconnect_target("192.168.1.105:8710")
    assert ("disconnect", "192.168.1.105:8710") in events
    assert dialog.table.item(0, 2).text() == "○ 未连接"

    # 离线状态：操作列应变为 2 个图标按钮 (连接、删除)
    cell_act_off = dialog.table.cellWidget(0, 3)
    btns_off = cell_act_off.findChildren(QPushButton)
    assert len(btns_off) == 2
    assert all(not b.icon().isNull() for b in btns_off)
    assert any("连接" in b.toolTip() for b in btns_off)
    assert any("删除" in b.toolTip() for b in btns_off)

    # 再次重连
    dialog._reconnect_target("192.168.1.105:8710")
    assert ("reconnect", "192.168.1.105:8710") in events


def test_mainwindow_quick_save_device(app, window, tmp_path, monkeypatch):
    monkeypatch.setattr("harmonycast.core.data_dir", lambda: tmp_path)
    window.quick_save_device("192.168.1.120", "eth0")
    assert "存入常用设备列表" in window.statusBar().currentMessage()
    saved = ui.load_saved_devices()
    assert len(saved) == 1
    assert saved[0]["ip"] == "192.168.1.120"
    assert saved[0]["port"] == 8710


def test_mainwindow_reconnect_current_device(app, window, monkeypatch):
    reconnect_calls = []

    class FakeHDC:
        def reconnect(self, serial):
            reconnect_calls.append(serial)
            return "reconnected"
        def devices(self):
            return ["192.168.1.50:8710"]
        def info(self, serial):
            return Device(serial, "Network Dev")

    monkeypatch.setattr(window, "hdc", FakeHDC())
    net_dev = Device("192.168.1.50:8710", "Network Dev")
    monkeypatch.setattr(window, "selected_device", lambda: net_dev)

    window.reconnect_current_device()
    wait_for(app, lambda: len(reconnect_calls) == 1)
    assert reconnect_calls == ["192.168.1.50:8710"]


def test_mainwindow_disconnect_current_device(app, window, monkeypatch):
    disconnect_calls = []

    class FakeHDC:
        def disconnect(self, serial):
            disconnect_calls.append(serial)
            return "disconnected"
        def devices(self):
            return []
        def info(self, serial):
            return Device(serial, "Network Dev")

    monkeypatch.setattr(window, "hdc", FakeHDC())
    net_dev = Device("192.168.1.50:8710", "Network Dev")
    monkeypatch.setattr(window, "selected_device", lambda: net_dev)

    window.projecting = True
    window.disconnect_current_device()
    assert not window.projecting
    wait_for(app, lambda: len(disconnect_calls) == 1)
    assert disconnect_calls == ["192.168.1.50:8710"]


def test_network_device_dialog_select_and_connect_only(app, tmp_path, monkeypatch):
    monkeypatch.setattr("harmonycast.core.data_dir", lambda: tmp_path)
    select_events = []
    connect_events = []

    dialog = ui.NetworkDeviceDialog(
        on_connect=lambda ep: connect_events.append(ep),
        on_select=lambda ep: select_events.append(ep),
        connected_targets=["10.10.214.43:8710"],
    )

    assert dialog.table.rowCount() == 1
    dialog._on_table_double_clicked(0, 0)
    assert select_events == ["10.10.214.43:8710"]

    dialog.ip_input.setText("10.10.220.99")
    dialog._on_connect_only()
    assert connect_events == ["10.10.220.99:8710"]


def test_device_dropdown_switch_while_projecting(app, window, monkeypatch):
    dev1 = Device("DEV_1", "Model 1")
    dev2 = Device("DEV_2", "Model 2")
    window.populate_devices([dev1, dev2])

    assert window.device_list.count() == 2
    assert window.device_list.isEnabled()

    started = []
    stopped = []
    monkeypatch.setattr(window, "start_mirror", lambda: started.append(window.selected_device().serial))
    monkeypatch.setattr(window, "stop_mirror", lambda user_initiated=False: stopped.append(True))

    window.projecting = True
    window.update_actions()
    # 投屏状态下下拉框与刷新依然保持可用
    assert window.device_list.isEnabled()
    assert window.refresh_button.isEnabled()
    assert window.network_button.isEnabled()

    # 在下拉框中切换设备
    window.device_list.setCurrentIndex(1)
    assert stopped == [True]
    wait_for(app, lambda: len(started) == 1)
    assert started == ["DEV_2"]


def test_device_dropdown_disconnects_other_network_device(app, window, monkeypatch):
    disconnect_calls = []

    class FakeHDC:
        def devices(self):
            return ["10.10.214.43:8710", "10.10.220.41:8710"]
        def disconnect(self, serial):
            disconnect_calls.append(serial)
            return "disconnected"
        def info(self, serial):
            return Device(serial, "Test Dev")

    fake = FakeHDC()
    monkeypatch.setattr(window, "hdc", fake)

    dev1 = Device("10.10.214.43:8710", "Dev 1")
    dev2 = Device("10.10.220.41:8710", "Dev 2")
    window.populate_devices([dev1, dev2])

    assert window.device_list.count() == 2

    disconnect_calls.clear()

    # 切换至第二个设备，单设备独占策略触发主动断开 DEV 1
    window.device_list.setCurrentIndex(1)
    wait_for(app, lambda: len(disconnect_calls) == 1)
    assert disconnect_calls == ["10.10.214.43:8710"]


def test_switch_to_single_device_disconnects_others(app, window, monkeypatch):
    disconnect_calls = []
    connect_calls = []

    class FakeHDC:
        def devices(self):
            return ["10.10.214.43:8710"]
        def disconnect(self, serial):
            disconnect_calls.append(serial)
            return "disconnected"
        def connect(self, serial):
            connect_calls.append(serial)
            return "connected"
        def info(self, serial):
            return Device(serial, "Test Dev")

    fake = FakeHDC()
    monkeypatch.setattr(window, "hdc", fake)

    # 切换至 10.10.220.41:8710，进入加载等待状态，同步重置各模块
    window.switch_to_single_device("10.10.220.41:8710")
    assert "10.10.220.41:8710" in window.screen.switching_message
    assert "正在连接 10.10.220.41:8710 日志" in window.log_state.text()

    wait_for(app, lambda: len(disconnect_calls) == 1 and len(connect_calls) == 1)
    assert disconnect_calls == ["10.10.214.43:8710"]
    assert connect_calls == ["10.10.220.41:8710"]


def test_dropdown_reads_saved_devices_list(app, window, tmp_path, monkeypatch):
    monkeypatch.setattr("harmonycast.core.data_dir", lambda: tmp_path)
    monkeypatch.setattr(ui, "data_dir", lambda: tmp_path)

    ui.save_device("10.10.220.41")
    ui.save_device("10.10.214.61")

    online_dev = Device("10.10.214.43:8710", "YH-H36")
    window.populate_devices([online_dev])

    # 下拉框应读取设备中心的所有设备：1 个在线 + 2 个保存的设备
    assert window.device_list.count() == 3
    items = [window.device_list.itemText(i) for i in range(3)]
    assert any(s.startswith("🟢 10.10.214.43:8710") for s in items)
    assert any(s.startswith("⚪ 10.10.220.41:8710") for s in items)
    assert any(s.startswith("⚪ 10.10.214.61:8710") for s in items)

    switched = []
    monkeypatch.setattr(window, "switch_to_single_device", lambda ep: switched.append(ep))

    # 在下拉框中选择未在线的保存设备
    window.device_list.setCurrentIndex(1)
    assert len(switched) == 1


def test_settings_button_moved_to_sidebar_and_checkbox_icons(app, window):
    # 验证右上角 header 不再包含设置按钮
    assert window.settings_button not in window.header.findChildren(QPushButton)

    # 验证左下角 sidebar 包含设置图标按钮
    assert window.settings_button in window.sidebar.findChildren(QPushButton)
    assert not window.settings_button.icon().isNull()
    assert "设置" in window.settings_button.toolTip() or "设置" in window.settings_button.accessibleName()

    # 验证复选框勾选图标文件存在
    check_svg = ui.resource_dir() / "check.svg"
    assert check_svg.is_file()
    assert "<svg" in check_svg.read_text()
    assert window.autoscroll.isCheckable()
    assert window.pause.isCheckable()


def test_mainwindow_webview_panel_and_controls(app, window, monkeypatch):
    window.switch_module("webview")
    assert window.pages.currentWidget() is window.workspace_page
    assert window.page_title.text() == "投屏工作台"
    assert window.log_subtab_stack.currentWidget() is window.webview_panel

    # 验证关键控件存在
    assert hasattr(window, "webview_instance_combo")
    assert hasattr(window, "btn_webview_scan")
    assert hasattr(window, "btn_webview_forward")
    assert hasattr(window, "webview_console_view")
    assert hasattr(window, "webview_js_input")
    assert hasattr(window, "btn_webview_eval")

    # 验证初始具有默认直连模式
    assert window.webview_instance_combo.count() >= 1
    assert "tcp:9222" in window.webview_instance_combo.itemText(0)

    # 模拟扫描返回套接字实例
    monkeypatch.setattr(
        window.webview_session,
        "scan_instances",
        lambda *args, **kwargs: [{"pid": 1234, "socket": "webview_devtools_remote_1234", "bundle": "com.test.app"}],
    )
    window.scan_webview_instances()
    wait_for(app, lambda: window.webview_instance_combo.count() >= 2)
    assert window.webview_instance_combo.count() >= 2
    assert "PID 1234" in window.webview_instance_combo.itemText(1)

    # 模拟建立端口映射
    forward_calls = []
    def fake_start_forward(*args, **kwargs):
        forward_calls.append((args, kwargs))
        return (True, "已建立映射: 127.0.0.1:9222 ➔ tcp:9222")
    monkeypatch.setattr(window.webview_session, "start_forward", fake_start_forward)

    window.toggle_webview_forward()
    wait_for(app, lambda: "断开" in window.btn_webview_forward.text())
    assert "断开" in window.btn_webview_forward.text()
    assert len(forward_calls) == 1

    # 控制台接收日志输出
    window._on_webview_console_entry({
        "type": "info",
        "text": "Hello from webview console",
        "color": "#73d6c1",
        "source": "index.js:10",
        "time": "12:00:00",
    })
    assert "Hello from webview console" in window.webview_console_view.toPlainText()
    assert window.webview_log_counts["log"] >= 1

    # 接收警告日志与过滤
    window._on_webview_console_entry({
        "type": "warn",
        "text": "Warning: slow network",
        "color": "#ffb74d",
        "source": "net.js:25",
        "time": "12:00:01",
    })
    assert "Warning: slow network" in window.webview_console_view.toPlainText()
    assert window.webview_log_counts["warn"] == 1

    # 切换等级过滤: 仅看 warn
    window._set_webview_log_level("warn")
    text_warn_only = window.webview_console_view.toPlainText()
    assert "Warning: slow network" in text_warn_only
    assert "Hello from webview console" not in text_warn_only

    # 恢复全部
    window._set_webview_log_level("all")
    assert "Hello from webview console" in window.webview_console_view.toPlainText()

    # 关键词过滤
    window.webview_filter_input.setText("slow")
    assert "Warning: slow network" in window.webview_console_view.toPlainText()
    assert "Hello from webview console" not in window.webview_console_view.toPlainText()
    window.webview_filter_input.clear()

    # 控制台执行结果
    window._on_webview_eval_result(1001, "42", False)
    assert "<- 42" in window.webview_console_view.toPlainText()

    # 快捷片段
    window.run_webview_snippet("document.title")
    assert "> document.title" in window.webview_console_view.toPlainText()

    # 清空控制台 (支持右键菜单信号与方法)
    window.webview_console_view.clear_requested.emit()
    assert window.webview_console_view.toPlainText() == ""
    assert window.webview_log_counts["all"] == 0

    # 接口网络 Network 功能测试
    window._set_webview_subtab(1)
    assert window.webview_right_stack.currentIndex() == 1
    assert hasattr(window, "webview_network_table")

    # 模拟网络请求到达
    window._on_webview_network_entry({
        "id": "req-1",
        "url": "http://10.10.248.21/api/patient/info",
        "method": "GET",
        "type": "fetch",
        "status": "pending",
        "time": "12:00:00",
        "duration": "-",
    })
    assert window.webview_network_table.rowCount() == 1
    assert window.webview_network_table.item(0, 1).text() == "GET"

    # 响应返回
    window._on_webview_network_entry({
        "id": "req-1",
        "status": "200",
        "duration": "24ms",
    })
    assert window.webview_network_table.item(0, 0).text() == "200"
    assert window.webview_network_table.item(0, 4).text() == "24ms"

    # 清空网络请求
    window.clear_webview_network()
    assert window.webview_network_table.rowCount() == 0
    assert len(window.webview_network_items) == 0

    # 切回控制台
    window._set_webview_subtab(0)
    assert window.webview_right_stack.currentIndex() == 0


def test_mainwindow_webview_direct_tcp_forward_and_chrome_inspect(app, window, monkeypatch):
    window.switch_module("webview")
    assert window.webview_instance_combo.count() >= 1

    # 直接使用默认直连模式建立映射，不需要任何扫描
    forwarded_target = []
    def fake_start_forward(hdc, serial, target_or_pid=None, **kwargs):
        forwarded_target.append(target_or_pid or kwargs.get("target"))
        return (True, "已建立映射: 127.0.0.1:9222 ➔ tcp:9222")
    monkeypatch.setattr(window.webview_session, "start_forward", fake_start_forward)

    # 1. 直接点击建立映射 (应该默认映射 tcp:9222)
    window.toggle_webview_forward()
    wait_for(app, lambda: "断开" in window.btn_webview_forward.text())
    assert "断开" in window.btn_webview_forward.text()
    assert forwarded_target[-1] == "tcp:9222"

    # 2. Chrome 调试调用测试
    chrome_opened = []
    monkeypatch.setattr("subprocess.Popen", lambda args, **kw: chrome_opened.append(args))
    window.open_chrome_inspect()
    assert len(chrome_opened) >= 1
    assert "chrome://inspect/#devices" in chrome_opened[0]

    # 3. 复制日志测试
    window._on_webview_console_entry({
        "type": "error",
        "text": "Uncaught TypeError in bedside.ets",
        "source": "bedside.ets:42",
        "time": "12:05:00",
    })
    copied = []
    monkeypatch.setattr(window, "copy_to_clipboard", lambda text, title="": copied.append((text, title)))
    window.copy_all_webview_logs()
    assert len(copied) == 1
    assert "Uncaught TypeError in bedside.ets" in copied[0][0]


def test_mainwindow_webview_network_detail(app, window, monkeypatch):
    window.switch_module("webview")
    window._set_webview_subtab(1)

    assert hasattr(window, "webview_network_detail_widget")
    assert window.webview_network_detail_widget.isHidden()

    # 模拟 POST 请求带标头和载荷
    req_item = {
        "id": "req-999",
        "url": "http://10.10.248.21/api/bed/status?dept=cardio&page=1",
        "method": "POST",
        "type": "xhr",
        "status": "pending",
        "time": "12:10:00",
        "duration": "-",
        "request_headers": {
            "Content-Type": "application/json",
            "Authorization": "Bearer tokenXYZ",
        },
        "post_data": '{"patientId": "P100", "nurse": "张三"}',
        "response_headers": {},
        "response_body": "",
    }
    window._on_webview_network_entry(req_item)
    assert window.webview_network_table.rowCount() == 1

    # 模拟用户点击行
    window.webview_network_table.selectRow(0)
    assert not window.webview_network_detail_widget.isHidden()
    assert window.net_detail_badge_method.text() == "POST"

    # 标头检查
    headers_text = window.webview_net_headers_view.toPlainText()
    assert "http://10.10.248.21/api/bed/status" in headers_text
    assert "POST" in headers_text
    assert "Content-Type: application/json" in headers_text
    assert "Authorization: Bearer tokenXYZ" in headers_text

    # 载荷检查
    payload_text = window.webview_net_payload_view.toPlainText()
    assert "P100" in payload_text
    assert "dept: cardio" in payload_text

    # 标签页切换测试
    window._switch_net_detail_tab(1)
    assert window.net_detail_stack.currentIndex() == 1
    window._switch_net_detail_tab(2)
    assert window.net_detail_stack.currentIndex() == 2

    # 响应返回并拉取
    window._on_response_body_received("req-999", '{"code": 0, "msg": "success"}', False, "")
    response_text = window.webview_net_response_view.toPlainText()
    assert '"code": 0' in response_text
    assert '"success"' in response_text

    # 复制响应测试
    copied = []
    monkeypatch.setattr(QApplication.clipboard(), "setText", lambda text: copied.append(text))
    window._on_copy_network_response()
    assert len(copied) == 1
    assert '"code": 0' in copied[0]

    # 关闭详情面板
    window._on_close_network_detail()
    assert window.webview_network_detail_widget.isHidden()

    # 清空请求
    window.clear_webview_network()
    assert window.webview_network_table.rowCount() == 0
    assert window.webview_network_detail_widget.isHidden()


def test_workspace_splitter_drag_and_compact_toolbar(app, window):
    window.switch_module("workspace")
    assert window.pages.currentWidget() is window.workspace_page

    # 1. 验证精简后的投屏底部操作栏
    assert hasattr(window, "btn_device_ops")
    assert window.btn_device_ops.text() == "操作 ▾"
    assert window.rotate_button.text() == "旋转"
    assert window.start_button.text() == "开始投屏"
    assert window.quality_combo.count() == 3

    # 验证操作菜单项健全 (仅保留返回、主屏、业务主程序、打包诊断)
    menu = window.btn_device_ops.menu()
    assert menu is not None
    action_texts = [a.text() for a in menu.actions() if not a.isSeparator()]
    assert any("返回" in t for t in action_texts)
    assert any("主屏幕" in t for t in action_texts)
    assert any("业务主程序" in t for t in action_texts)
    assert any("打包诊断" in t for t in action_texts)
    assert not any("拉起应用" in t for t in action_texts)
    assert not any("唤醒" in t for t in action_texts)
    assert not any("断开设备" in t for t in action_texts)

    # 2. 验证左右分屏拖拽宽度自由度
    window.set_workspace_layout("horizontal")
    assert window.workspace_splitter.orientation() == Qt.Horizontal
    handle = window.workspace_splitter.handle(1)
    assert handle.cursor().shape() == Qt.SplitHCursor
    window.resize(1200, 700)
    app.processEvents()

    # 模拟用户自由拖拽分屏宽度 (如左窄右宽)
    window.workspace_splitter.setSizes([250, 750])
    app.processEvents()
    sizes = window.workspace_splitter.sizes()
    assert sizes[0] < 400
    assert sizes[1] > 600

    # 模拟用户自由拖拽分屏宽度 (如左宽右窄)
    window.workspace_splitter.setSizes([750, 250])
    app.processEvents()
    sizes = window.workspace_splitter.sizes()
    assert sizes[0] > 600
    assert sizes[1] < 400

    # 3. 验证上下分屏拖拽高度自由度
    window.set_workspace_layout("vertical")
    assert window.workspace_splitter.orientation() == Qt.Vertical
    handle = window.workspace_splitter.handle(1)
    assert handle.cursor().shape() == Qt.SplitVCursor

    # 模拟用户拖拽分屏高度 (如上窄下高)
    window.workspace_splitter.setSizes([150, 450])
    app.processEvents()
    sizes = window.workspace_splitter.sizes()
    assert sizes[0] < 250
    assert sizes[1] > 350

    # 模拟用户拖拽分屏高度 (如上高下窄)
    window.workspace_splitter.setSizes([450, 150])
    app.processEvents()
    sizes = window.workspace_splitter.sizes()
    assert sizes[0] > 350
    assert sizes[1] < 250


def test_workspace_streamlined_workflow_and_shortcuts(app, window, monkeypatch):
    window.switch_module("workspace")
    assert window.pages.currentWidget() is window.workspace_page

    # 1. 未投屏时操作菜单仍然可用 (解耦投屏状态限制)
    assert window.btn_device_ops.isEnabled()
    menu = window.btn_device_ops.menu()
    action_texts = [a.text() for a in menu.actions() if not a.isSeparator()]
    assert any("业务主程序" in t for t in action_texts)
    assert len(action_texts) == 4

    # 2. 点击空屏区域直接启动投屏 (极简直觉操作)
    assert not window.projecting
    assert window.screen.image.isNull()
    # 模拟在空屏幕中央点击
    center = QPoint(window.screen.width() // 2, window.screen.height() // 2)
    QTest.mouseClick(window.screen, Qt.LeftButton, pos=center)
    wait_for(app, lambda: not window.screen.image.isNull())
    assert window.projecting
    assert window.state_badge.text() == "投屏中"

    # 3. 投屏中右键快捷触发返回 (Back)
    keys_sent = []
    monkeypatch.setattr(window, "send_key", lambda act: keys_sent.append(act))
    QTest.mouseClick(window.screen, Qt.RightButton, pos=center)
    assert "back" in keys_sent

    # 4. 键盘快捷键: Escape 在非全屏投屏中触发返回 (Back)
    keys_sent.clear()
    assert not window.isFullScreen()
    window.on_escape_pressed()
    assert "back" in keys_sent

    # 5. 分屏拖拽记忆与持久化恢复
    window.set_workspace_layout("horizontal")
    window.workspace_splitter.setSizes([320, 680])
    app.processEvents()
    cur_sizes = window.workspace_splitter.sizes()
    window._on_workspace_splitter_moved(0, 0)
    assert f"{cur_sizes[0]},{cur_sizes[1]}" == window.settings.value("workspace/splitter_h")

    # 切换到上下再切回左右，验证恢复记录的尺寸
    window.set_workspace_layout("vertical")
    assert window.workspace_splitter.orientation() == Qt.Vertical
    window.set_workspace_layout("horizontal")
    assert window.workspace_splitter.orientation() == Qt.Horizontal
    restored_sizes = window.workspace_splitter.sizes()
    assert abs(restored_sizes[0] - cur_sizes[0]) <= 10
    assert abs(restored_sizes[1] - cur_sizes[1]) <= 10

    # 6. WebView 面板精简：内层多余的二级按钮被隐藏
    window.switch_log_subtab(1)
    assert window.mode_header_widget.isHidden()
    assert window.webview_right_stack.currentIndex() == 0
    window.switch_log_subtab(2)
    assert window.mode_header_widget.isHidden()
    assert window.webview_right_stack.currentIndex() == 1

    # 7. 停止投屏
    QTest.mouseClick(window.stop_button, Qt.LeftButton)
    assert not window.projecting


def test_workspace_responsive_adaptation_and_uniform_button_sizes(app, window):
    window.switch_module("workspace")
    assert window.pages.currentWidget() is window.workspace_page

    # 1. 验证控件高度统一定制为 28px (或布局切换小钮 24px)
    assert window.start_button.height() == 28
    assert window.stop_button.height() == 28
    assert window.quality_combo.height() == 28
    assert window.rotate_button.height() == 28
    assert window.screenshot_button.height() == 28
    assert window.btn_device_ops.height() == 28
    assert window.fullscreen_button.height() == 28
    assert window.state_badge.height() == 28

    assert window.refresh_button.height() == 28
    assert window.disconnect_button.height() == 28
    assert window.network_button.height() == 28
    assert window.device_list.height() == 28

    assert window.search.height() == 28
    assert window.package_combo.height() == 28
    assert window.level.height() == 28
    assert window.clear_button.height() == 28
    assert window.export_button.height() == 28

    # 2. 验证导航栏、布局切换、搜索框及快捷菜单均去除了表情图标，使用纯文本与矢量 icon
    emoji_chars = ["ℹ", "▣", "⚡", "≡", "🌐", "◧", "⬒", "🔍", "⮌", "⌂", "🔆", "🚀", "🏥", "📦", "⏏"]
    for nav_btn in (window.info_nav, window.mirror_nav, window.hdc_nav, window.logs_nav, window.webview_nav):
        for em in emoji_chars:
            assert em not in nav_btn.text(), f"Nav {nav_btn.text()} contains emoji {em}"

    for layout_btn in (window.btn_layout_horizontal, window.btn_layout_vertical, window.btn_layout_screen_only, window.btn_layout_logs_only):
        for em in emoji_chars:
            assert em not in layout_btn.text(), f"Layout btn {layout_btn.text()} contains emoji {em}"

    assert "🔍" not in window.search.placeholderText()
    assert "🔍" not in window.webview_filter_input.placeholderText()

    menu = window.btn_device_ops.menu()
    for action in menu.actions():
        if not action.isSeparator():
            for em in emoji_chars:
                assert em not in action.text(), f"Action {action.text()} contains emoji {em}"

    # 3. 验证不同窗口尺寸与窄分屏下的自适应平滑折叠 (小窗口折叠辅助按钮文字，避免截断)
    # 模拟左侧窄面板 (< 460px)
    window.mirror_page.resize(380, 500)
    window._update_responsive_toolbars()
    assert window.fullscreen_button.text() == ""
    assert window.screenshot_button.text() == ""
    # 核心按钮文字完整保留
    assert window.start_button.text() == "开始投屏"
    assert window.rotate_button.text() == "旋转"
    assert window.btn_device_ops.text() == "操作 ▾"

    # 模拟宽面板 (>= 460px)
    window.mirror_page.resize(600, 500)
    window._update_responsive_toolbars()
    assert window.fullscreen_button.text() == "全屏"
    assert window.screenshot_button.text() == "截图"

    # 模拟 WebView 面板自适应
    window.webview_panel.resize(500, 500)
    window._update_responsive_toolbars()
    assert window.btn_webview_chrome.text() == ""
    assert window.btn_webview_reload.text() == ""
    assert window.btn_webview_clear.text() == ""

    window.webview_panel.resize(800, 500)
    window._update_responsive_toolbars()
    assert window.btn_webview_chrome.text() == "Chrome"
    assert window.btn_webview_reload.text() == "刷新"
    assert window.btn_webview_clear.text() == "清空"


def test_direct_mirror_shortcut_buttons_and_webview_improvements(app, window, monkeypatch, tmp_path):
    window.switch_module("workspace")

    # 1. 验证投屏操作栏独立按钮直接外露，文案简化
    assert hasattr(window, "btn_key_back")
    assert window.btn_key_back.text() == "返回"
    assert not window.btn_key_back.isHidden()
    assert window.btn_key_back.height() == 28

    assert hasattr(window, "btn_key_home")
    assert window.btn_key_home.text() == "主页"
    assert not window.btn_key_home.isHidden()
    assert window.btn_key_home.height() == 28

    assert hasattr(window, "restore_primary_button")
    assert window.restore_primary_button.text() == "主程序"
    assert not window.restore_primary_button.isHidden()
    assert window.restore_primary_button.height() == 28

    assert hasattr(window, "fault_report_button")
    assert window.fault_report_button.text() == "打包诊断"
    assert not window.fault_report_button.isHidden()
    assert window.fault_report_button.height() == 28

    # 验证操作菜单被隐藏以精简界面
    assert window.btn_device_ops.isHidden()

    # 验证点击快捷按钮触发预期行为
    sent_keys = []
    monkeypatch.setattr(window, "send_key", lambda k: sent_keys.append(k))
    QTest.mouseClick(window.btn_key_back, Qt.LeftButton)
    assert "back" in sent_keys
    QTest.mouseClick(window.btn_key_home, Qt.LeftButton)
    assert "home" in sent_keys

    # 2. 验证 WebView 控制台精简与 HiLog 对齐
    window.webview_panel.resize(800, 500)
    window._update_responsive_toolbars()
    assert window.btn_webview_forward.isHidden()
    assert window.webview_page_combo.isHidden()
    assert hasattr(window, "cb_webview_autoscroll")
    assert hasattr(window, "cb_webview_pause")
    assert hasattr(window, "btn_webview_export")
    assert window.btn_webview_export.text() == "导出日志"
    assert window.cb_webview_pause.text() == "暂停"

    # 验证 WebView 控制台日志等级下拉框位于左侧（在搜索输入框之前）
    toolbar = window.webview_panel.layout().itemAt(0).layout()
    combo_idx = toolbar.indexOf(window.webview_filter_combo)
    input_idx = toolbar.indexOf(window.webview_filter_input)
    assert combo_idx != -1
    assert input_idx != -1
    assert combo_idx < input_idx

    # 3. 验证设备日志与 WebView 控制台字号双向同步
    assert hasattr(window, "webview_font_size")
    window.log_font_size.setValue(16)
    assert window.webview_font_size.value() == 16
    assert "16pt" in window.log_view.styleSheet()
    assert "16pt" in window.webview_console_view.styleSheet()

    window.webview_font_size.setValue(13)
    assert window.log_font_size.value() == 13
    assert "13pt" in window.log_view.styleSheet()
    assert "13pt" in window.webview_console_view.styleSheet()

    # 4. 验证 WebView 导出功能
    export_file = tmp_path / "webview_export.log"
    monkeypatch.setattr(QFileDialog, "getSaveFileName", lambda *args, **kwargs: (str(export_file), ""))
    window.webview_logs = [
        {"type": "info", "text": "Init Web App", "source": "app.js:1", "time": "10:00:00"},
        {"type": "error", "text": "Network Failed", "source": "api.js:42", "time": "10:00:05"},
    ]
    window._on_export_webview_logs()
    assert export_file.exists()
    content = export_file.read_text(encoding="utf-8")
    assert "Init Web App" in content
    assert "Network Failed" in content

    # 5. 验证自动匹配连接：无可用 Web 页面时自动切回设备日志 Tab (0)
    monkeypatch.setattr(window.webview_session, "fetch_pages", lambda: [])
    monkeypatch.setattr(window.webview_session, "scan_instances", lambda *args: [])
    window.switch_log_subtab(1, auto_connect=True)
    wait_for(app, lambda: window.current_log_subtab == 0)
    assert window.current_log_subtab == 0
    assert "无可用 Web 页面" in window.statusBar().currentMessage()

    # 6. 验证设备日志 HiLog 的应用包名与日志级别位于左侧，且宽度充裕不截断
    hilog_toolbar = window.hilog_widget.layout().itemAt(0).layout()
    pkg_idx = hilog_toolbar.indexOf(window.package_row)
    lvl_idx = hilog_toolbar.indexOf(window.level)
    srch_idx = hilog_toolbar.indexOf(window.search)
    assert pkg_idx != -1
    assert lvl_idx != -1
    assert srch_idx != -1
    assert pkg_idx < srch_idx
    assert lvl_idx < srch_idx
    assert 120 <= window.package_combo.width() <= 150
    assert window.level.width() >= 90
    assert window.search.width() >= 160


def test_homepage_empty_state_when_no_device_connected(app, window):
    window.switch_module(0)
    window.populate_devices([])
    app.processEvents()

    assert "未连接" in window.info_badge.text()
    assert window.card_empty.isVisible() is True
    assert window.card_yarward.isVisible() is False
    assert window.btn_refresh_info.isEnabled() is False
    assert window.start_button.isEnabled() is False

    dev = Device("DEV_USB_TEST", "Test Device", "HUAWEI", "6.1", "aarch64")
    window.populate_devices([dev])
    app.processEvents()

    assert window.is_device_connected(dev) is True
    assert window.start_button.isEnabled() is True


def test_log_combobox_popups_have_no_white_borders(app, window):
    """验证日志模块及其他核心下拉菜单的弹出层没有浅色/白边框瑕疵"""
    for cb_name in ("level", "package_combo", "webview_filter_combo", "quality_combo"):
        cb = getattr(window, cb_name)
        assert cb is not None
        view = cb.view()
        assert view is not None
        container = view.window()
        assert container is not None
        palette_win = container.palette().color(container.palette().Window).name()
        assert palette_win in ("#141f2c", "#111923", "#0c131c")
        layout = container.layout()
        if layout:
            assert layout.contentsMargins().top() == 0
            assert layout.contentsMargins().bottom() == 0
            if layout.count() > 0:
                assert layout.itemAt(0).spacerItem() is None
                assert layout.itemAt(layout.count() - 1).spacerItem() is None


def test_screenview_device_switch_retains_valid_target_rect_and_no_black_screen():
    """验证连续切换相同分辨率设备时，ScreenView 不会因为缓存机制残留空 Rect 导致永久黑屏"""
    from harmonycast.ui import ScreenView

    sv = ScreenView()
    sv.resize(800, 600)

    # 1. 设备 A 投屏首帧 (1280x800)
    img_a = QImage(1280, 800, QImage.Format_RGB888)
    sv.set_image(img_a)
    assert sv._target_rect.isValid()
    assert sv._target_rect.width() > 0 and sv._target_rect.height() > 0

    # 2. 切换设备：中间置空
    sv.set_image(QImage())
    assert not sv._target_rect.isValid()

    # 3. 设备 B (同样是 1280x800) 连接并输出首帧
    img_b = QImage(1280, 800, QImage.Format_RGB888)
    sv.set_image(img_b)
    # 核心断言：必须重新计算有效的 target_rect，杜绝永久黑屏
    assert sv._target_rect.isValid()
    assert sv._target_rect.width() > 0 and sv._target_rect.height() > 0
    assert sv.coordinates(sv.rect().center()) is not None


def test_rapid_device_switching_serializes_and_recovers_mirroring(app, window, monkeypatch):
    """验证快速切换多次网络设备时，代数保护与锁机制使投屏能平滑恢复且各模块正常工作"""
    window.switch_to_single_device("10.10.214.60:8710")
    window.switch_to_single_device("10.10.214.61:8710")
    window.switch_to_single_device("10.10.224.6:8710")

    wait_for(app, lambda: window._switch_generation == 3 and window.device_list.isEnabled())
    assert window._active_device_serial == "10.10.224.6:8710"


def test_webview_no_interface_shows_toast(app, window, monkeypatch):
    """验证当前设备无可用的 WebView 界面时，自动弹出浮动 Toast 气泡提醒"""
    monkeypatch.setattr(window.webview_session, "fetch_pages", lambda: [])
    monkeypatch.setattr(window.webview_session, "scan_instances", lambda *args: [])

    # 1. 验证 show_toast 基本机制与样式
    window.show_toast("当前设备未检测到运行中的 WebView 界面", level="warning", duration_ms=2000)
    assert hasattr(window, "_toast_widget")
    toast = window._toast_widget
    assert toast.isVisible()
    assert "当前设备未检测到运行中的 WebView 界面" in toast.text_label.text()
    assert toast.icon_label.text() == "⚠️"

    # 2. 模拟切换至 WebView 入口，无可用网页时自动弹出 Toast
    window.switch_log_subtab(1, auto_connect=True)
    wait_for(app, lambda: window.current_log_subtab == 0)
    assert toast.isVisible()
    assert "WebView" in toast.text_label.text()


def test_mirror_toolbar_flow_layout_wrapping(app, window):
    layout = window.mirror_toolbar.layout()
    assert isinstance(layout, ui.FlowLayout)
    h_wide = layout.heightForWidth(1200)
    h_narrow = layout.heightForWidth(320)
    assert h_narrow > h_wide, f"Narrow height {h_narrow} should exceed wide height {h_wide} due to wrapping"


def test_log_scroll_pinning_when_scrolled_up(app, window):
    window.resize(600, 400)
    window.show()
    window.switch_module("logs")
    # Preload 50 logs matching the active filter
    for i in range(50):
        window.store.append("INFO", "HiLog", f"10-23 10:00:00.{i:03d} 100 100 I A00001/Tag: Log line {i}", bundle="com.yarward.demo")
    window.render_logs()
    app.processEvents()
    scroll = window.log_view.verticalScrollBar()
    assert scroll.maximum() > 100
    # Scroll up to line 10 area
    scroll.setValue(100)
    app.processEvents()
    saved_val = scroll.value()

    # Append 5 new lines
    for i in range(50, 55):
        window.messages.put(("INFO", "HiLog", f"10-23 10:00:00.{i:03d} 100 100 I A00001/Tag: Log line {i}", "com.yarward.demo"))
    window.pump()
    app.processEvents()

    # Should remain at the pinned scrolled position, not snapped to bottom
    assert scroll.value() < scroll.maximum()
    assert abs(scroll.value() - saved_val) <= 2


def test_hilog_toolbar_flow_wrapping_and_compact_package_selector(app, window):
    window.switch_module("logs")
    hilog_toolbar = window.hilog_widget.layout().itemAt(0).layout()
    assert isinstance(hilog_toolbar, ui.FlowLayout)

    # 验证包名区域紧凑，搜索框优先自适应
    assert window.package_combo.width() <= 150
    assert window.search.minimumWidth() >= 180

    # 验证宽度不足时自动折行
    h_wide = hilog_toolbar.heightForWidth(1200)
    h_narrow = hilog_toolbar.heightForWidth(450)
    assert h_narrow > h_wide, f"Narrow height {h_narrow} should exceed wide height {h_wide} due to flow wrapping"


def test_zero_copy_video_frame_publishing_and_reference_retention(app, window):
    import numpy as np
    import gc

    window.start_mirror()
    token = window.active_token

    # Create dummy frame numpy array
    rgb = np.zeros((100, 100, 3), dtype=np.uint8)
    rgb[10, 10] = [123, 200, 255]

    # Zero-copy QImage
    image = QImage(rgb.data, 100, 100, 100 * 3, QImage.Format_RGB888)
    image._ref = rgb
    window.publish_frame(token, image, (100, 100), from_video=True)

    del rgb
    gc.collect()

    # Paint frame
    window.paint_frame()
    assert not window.screen.image.isNull()
    assert window.screen.image.pixelColor(10, 10) == QColor(123, 200, 255)

    QTest.mouseClick(window.stop_button, Qt.LeftButton)
















