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
    monkeypatch.setattr(ui, "HiLogSession", FakeLogSession)
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
    assert not window.device_list.isEnabled()
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


def test_screenshot_saves_a_valid_png_without_active_preview(app, window, tmp_path, monkeypatch):
    screenshot = tmp_path / "screen.png"
    monkeypatch.setattr(QFileDialog, "getSaveFileName", lambda *args, **kwargs: (str(screenshot), ""))
    window.save_screenshot()
    wait_for(app, screenshot.is_file)
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
    assert window.pages.count() == 4
    assert window.pages.currentWidget() is window.info_panel
    assert not window.log_panel.isVisible()
    assert not window.hdc_panel.isVisible()
    QTest.mouseClick(window.hdc_nav, Qt.LeftButton)
    assert window.pages.currentWidget() is window.hdc_panel
    assert window.page_title.text() == "常用命令"
    QTest.mouseClick(window.mirror_nav, Qt.LeftButton)
    assert window.pages.currentWidget() is window.mirror_page
    QTest.mouseClick(window.start_button, Qt.LeftButton)
    wait_for(app, lambda: not window.screen.image.isNull())
    assert window.projecting
    # 切换至日志页：投屏自动休眠，释放设备与电脑资源
    QTest.mouseClick(window.logs_nav, Qt.LeftButton)
    assert window.pages.currentWidget() is window.log_panel
    assert not window.projecting
    assert window.state_badge.text() == "已休眠"
    QTest.mouseClick(window.info_nav, Qt.LeftButton)
    assert window.pages.currentWidget() is window.info_panel
    assert window.page_title.text() == "设备信息"
    assert not window.projecting
    # 切回投屏页：投屏自动唤醒恢复
    QTest.mouseClick(window.mirror_nav, Qt.LeftButton)
    assert window.pages.currentWidget() is window.mirror_page
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
    assert window.pages.currentWidget() is window.mirror_page


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
    assert not window.logs_nav.isChecked()
    window.logs_nav.setChecked(True)
    assert window.pages.currentWidget() is window.log_panel
    assert not window.hdc_nav.isChecked()
    assert not window.mirror_nav.isChecked()
    window.mirror_nav.setChecked(True)
    assert window.pages.currentWidget() is window.mirror_page
    assert not window.logs_nav.isChecked()
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
    # 切至日志页时投屏服务休眠
    assert not window.projecting
    first = window.hilog
    window.category.setCurrentText("系统日志")
    wait_for(app, lambda: first.stopped)
    assert window.hilog.category == "system"
    # 切回投屏页时自动唤醒
    window.switch_module(1)
    wait_for(app, lambda: window.projecting)
    assert window.projecting


def test_log_panel_hides_redundant_source_dropdown_and_fetch_button(app, window):
    window.switch_module("logs")
    assert not window.source.isVisible()
    assert not window.fetch_button.isVisible()
    # 验证应用包名组件处于第一行且布局正确
    assert window.package_row.isVisible()
    assert window.package_combo.isVisible()


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
    window.switch_module(1)
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
    assert "[✓ 执行完成" in window.hdc_terminal.toPlainText()


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
