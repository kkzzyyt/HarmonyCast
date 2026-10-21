from __future__ import annotations

import html
import logging
import logging.handlers
import queue
import re
import shlex
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

from PyQt5.QtCore import QBuffer, QByteArray, QEvent, QIODevice, QObject, QPoint, QPointF, QRect, QSettings, Qt, QTimer, QUrl, pyqtSignal
from PyQt5.QtGui import QColor, QDesktopServices, QFont, QIcon, QImage, QKeySequence, QPainter, QPen, QSyntaxHighlighter, QTextBlockFormat, QTextCharFormat, QTextCursor, QTransform
from PyQt5.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QCompleter, QDialog, QFileDialog, QFormLayout,
    QFrame, QGridLayout, QHBoxLayout, QHeaderView, QInputDialog, QLabel, QLineEdit, QMenu, QShortcut, QSpinBox, QSplitter, QStackedWidget,
    QMainWindow, QMessageBox, QPushButton, QScrollArea, QTableWidget, QTableWidgetItem, QTextEdit,
    QVBoxLayout, QWidget,
)

from . import __version__
from .backend import DeviceTools, HiLogSession, NativeSession, StreamSession, format_bytes
from .core import Device, HDC, HDCError, LogStore, data_dir, find_hdc, map_coordinates, resource_dir, validate_package_filter


DEFAULT_PACKAGE_FILTER = "com.yarward.*"


def open_containing_folder(path: Path) -> None:
    try:
        resolved = str(path.resolve())
        if sys.platform == "darwin":
            subprocess.Popen(["open", "-R", resolved])
        elif sys.platform == "win32":
            subprocess.Popen(f'explorer /select,"{resolved}"')
        else:
            subprocess.Popen(["xdg-open", str(path.parent)])
    except Exception as exc:
        logging.getLogger("HarmonyCast").warning("无法在文件管理器中定位文件: %s", exc)


STYLE = """
QWidget { background: #111923; color: #e7edf4; font-family: 'PingFang SC'; font-size: 13px; }
QMainWindow { background: #111923; }
QFrame#Sidebar { background: #0c131c; border-right: 1px solid #253242; }
QFrame#Header { border-bottom: 1px solid #253242; }
QLabel { background: transparent; }
QLabel#Title { font-size: 25px; font-weight: 600; }
QLabel#Brand { font-size: 17px; font-weight: 600; }
QLabel#Muted { color: #8295a9; font-size: 12px; }
QLabel#Section { color: #98acbe; font-size: 11px; font-weight: 600; }
QLabel#Badge { color: #5ce0c1; background: #12352f; padding: 5px 12px; border-radius: 11px; }
QPushButton { background: #1d2938; border: 1px solid #304257; border-radius: 7px; padding: 8px 14px; }
QPushButton:hover { background: #293b4e; border-color: #5f829a; }
QPushButton:pressed { background: #33495e; }
QPushButton:disabled { color: #526375; border-color: #233141; background: #16212d; }
QPushButton#Primary { background: #53d6b5; border: 1px solid #53d6b5; color: #09231e; font-weight: 600; }
QPushButton#Primary:hover { background: #78ebcb; }
QPushButton#Primary:disabled { background: #24463f; border-color: #24463f; color: #66847c; }
QPushButton#Danger { color: #ff9f9f; }
QPushButton#Danger:disabled { color: #526375; }
QPushButton#Navigation { text-align: left; background: transparent; border: none; color: #9caec0; font-size: 15px; }
QPushButton#Navigation:checked { background: #1b3e35; color: #79e6c9; }
QPushButton#Navigation:hover { background: #1d2938; }
QMenu { background: #1d2938; border: 1px solid #304257; padding: 5px; }
QMenu::item { padding: 8px 20px; }
QMenu::item:selected { background: #245546; }
QListWidget { background: transparent; border: none; outline: none; }
QListWidget::item { background: #141f2c; margin-bottom: 8px; padding: 14px 10px; border: 1px solid #25354a; border-radius: 8px; }
QListWidget::item:selected { background: #15372f; border-color: #53d6b5; color: #e9fff8; }
QLineEdit, QComboBox, QSpinBox { background: #0c131c; border: 1px solid #304257; border-radius: 6px; padding: 7px 9px; min-height: 18px; }
QLineEdit:focus, QComboBox:focus { border-color: #53d6b5; }
QComboBox { padding-right: 32px; }
QComboBox QLineEdit { background: transparent; border: none; border-radius: 0; padding: 0; min-height: 0; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width: 28px; border-left: 1px solid #304257; background: #1d2938; border-top-right-radius: 6px; border-bottom-right-radius: 6px; }
QComboBox::drop-down:hover { background: #293b4e; }
QComboBox::down-arrow { image: url("__CHEVRON_DOWN__"); width: 12px; height: 12px; }
QComboBox QAbstractItemView { background: #1d2938; color: #e7edf4; selection-background-color: #245546; }
QTextEdit { background: #0a1119; color: #c2d1df; border: 1px solid #25354a; border-radius: 7px; padding: 8px; selection-background-color: #245546; }
QCheckBox { background: transparent; spacing: 6px; }
QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #4b6176; border-radius: 3px; background: #0c131c; }
QCheckBox::indicator:checked { background: #53d6b5; border-color: #53d6b5; }
QSplitter::handle { background: #253242; }
QStatusBar { background: #0c131c; border-top: 1px solid #253242; color: #8295a9; }
QToolTip { background: #243549; color: white; border: 1px solid #42566b; }
QFrame#InfoCard { background: #141f2c; border: 1px solid #25354a; border-radius: 8px; }
QFrame#AppBox { background: #0c131c; border: 1px solid #25354a; border-radius: 6px; }
QLabel#CardTitle { font-size: 15px; font-weight: 600; color: #5ce0c1; }
QLabel#CardTitleWarn { font-size: 15px; font-weight: 600; color: #f3c969; }
QLabel#InfoKey { color: #8295a9; font-size: 12px; }
QLabel#InfoVal { color: #e7edf4; font-size: 13px; }
QLabel#InfoValMono { color: #79e6c9; font-size: 13px; font-family: 'Menlo', 'Courier New', monospace; font-weight: 600; }
QLabel#TagSuccess { color: #5ce0c1; background: #12352f; border: 1px solid #1f564b; padding: 2px 8px; border-radius: 4px; font-size: 12px; }
QLabel#TagMuted { color: #8295a9; background: #1d2938; border: 1px solid #304257; padding: 2px 8px; border-radius: 4px; font-size: 12px; }
QLabel#AppVersion { color: #5ce0c1; font-size: 16px; font-weight: 600; font-family: 'Menlo', 'Courier New', monospace; }
QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
QTableWidget { background: #0a1119; border: 1px solid #25354a; border-radius: 6px; gridline-color: #1a2736; selection-background-color: #1b3e35; color: #e7edf4; }
QTableWidget::item { padding: 4px 8px; }
QTableWidget::item:selected { background: #1b3e35; color: #79e6c9; }
QHeaderView::section { background: #141f2c; color: #8295a9; font-weight: 600; border: none; border-bottom: 1px solid #25354a; padding: 5px 8px; }
""".replace("__CHEVRON_DOWN__", str(resource_dir() / "chevron-down.svg"))


def label(text: str, name: str = "", wrap: bool = False) -> QLabel:
    item = QLabel(text)
    item.setObjectName(name)
    item.setWordWrap(wrap)
    return item


def button(text: str, callback, name: str = "") -> QPushButton:
    item = QPushButton(text)
    item.setObjectName(name)
    item.setAccessibleName(text)
    item.clicked.connect(callback)
    return item


def _clear_layout(layout) -> None:
    if layout is None:
        return
    while layout.count():
        item = layout.takeAt(0)
        widget = item.widget()
        if widget is not None:
            widget.deleteLater()
        elif item.layout() is not None:
            _clear_layout(item.layout())


def orient_native_image(image: QImage, device_size, device_rotation: int = 0, user_rotation: int = 0) -> QImage:
    if device_size:
        logical_width, logical_height = device_size
        phy_w, phy_h = (logical_height, logical_width) if device_rotation % 180 else (logical_width, logical_height)
        if image.width() > phy_w or image.height() > phy_h:
            image = image.copy(0, 0, min(phy_w, image.width()), min(phy_h, image.height()))
    total_rotation = (device_rotation + user_rotation) % 360
    return image.transformed(QTransform().rotate(total_rotation)) if total_rotation else image


class Signals(QObject):
    completed = pyqtSignal(object, object)
    stream_status = pyqtSignal(str, str)
    stream_finished = pyqtSignal(str, str)
    hilog_finished = pyqtSignal(str, int)
    hilog_status = pyqtSignal(str, str)


class QueueHandler(logging.Handler):
    def __init__(self, messages: queue.Queue):
        super().__init__()
        self.messages = messages

    def emit(self, record: logging.LogRecord) -> None:
        level = {"WARNING": "WARN", "CRITICAL": "FATAL"}.get(record.levelname, record.levelname)
        try:
            self.messages.put_nowait((level, "工具", record.getMessage()))
        except queue.Full:
            pass


class LogView(QTextEdit):
    def __init__(self):
        super().__init__()
        self.setAcceptRichText(False)
        self.setUndoRedoEnabled(False)
        self.document().setMaximumBlockCount(5000)
        self.line_format = QTextBlockFormat()
        self.line_format.setLineHeight(140, QTextBlockFormat.ProportionalHeight)

    def setPlainText(self, text):
        super().setPlainText(text)
        cursor = QTextCursor(self.document())
        cursor.select(QTextCursor.Document)
        cursor.mergeBlockFormat(self.line_format)

    def appendPlainText(self, text):
        cursor = QTextCursor(self.document())
        cursor.movePosition(QTextCursor.End)
        cursor.beginEditBlock()
        cursor.mergeBlockFormat(self.line_format)
        if not self.document().isEmpty():
            cursor.insertBlock(self.line_format)
        cursor.insertText(text)
        cursor.endEditBlock()


class LogHighlighter(QSyntaxHighlighter):
    COLORS = {"DEBUG": "#8295a9", "INFO": "#73d6c1", "WARN": "#f3c969", "ERROR": "#ff8080", "FATAL": "#e8a0ff"}
    HEADER = re.compile(r"^\d{2}:\d{2}:\d{2}\.\d{3}  (DEBUG|INFO|WARN|ERROR|FATAL)\b")

    def __init__(self, document):
        super().__init__(document)
        self.levels = list(self.COLORS)
        self.level_map = {level: index for index, level in enumerate(self.levels)}
        self.formats = {}
        for level, color in self.COLORS.items():
            style = QTextCharFormat()
            style.setForeground(QColor(color))
            if level in {"ERROR", "FATAL"}:
                style.setFontWeight(QFont.DemiBold)
            self.formats[level] = style

    def highlightBlock(self, text):
        match = self.HEADER.match(text)
        state = self.level_map.get(match[1], -1) if match else self.previousBlockState()
        self.setCurrentBlockState(state)
        if 0 <= state < len(self.levels):
            self.setFormat(0, len(text), self.formats[self.levels[state]])


class ScreenView(QWidget):
    gesture = pyqtSignal(object, object, int)
    fullscreen_requested = pyqtSignal()

    def __init__(self):
        super().__init__()
        self.image = QImage()
        self.started_at = 0.0
        self.drag_start = None
        self._active = False
        self.device_size = None
        self.user_rotation = 0
        self._target_rect = QRect()
        self._last_image_size = (0, 0)
        self._last_size = (0, 0)

        # 触摸轨迹小白点状态
        self.show_touches = True
        self._touch_active = False
        self._touch_pos: QPoint | None = None
        self._touch_trail: list[QPoint] = []
        self._ripple_radius = 16.0
        self._ripple_opacity = 0.0
        self._fade_timer = QTimer(self)
        self._fade_timer.setInterval(16)
        self._fade_timer.timeout.connect(self._on_fade_tick)

        self.setMinimumSize(280, 220)
        self.setAccessibleName("设备投屏画面")
        self.setMouseTracking(True)
        self.setCursor(Qt.ArrowCursor)

    def _on_fade_tick(self):
        self._ripple_radius += 1.8
        self._ripple_opacity -= 0.12
        if self._ripple_opacity <= 0.0:
            self._ripple_opacity = 0.0
            self._touch_pos = None
            self._touch_trail.clear()
            self._fade_timer.stop()
        self.update()

    @property
    def active(self) -> bool:
        return self._active

    @active.setter
    def active(self, value: bool) -> None:
        self._active = bool(value)
        if not self._active:
            self.drag_start = None
            self._touch_active = False
            self._touch_pos = None
            self._touch_trail.clear()
            self._fade_timer.stop()
        self.setCursor(Qt.ArrowCursor)
        self.update()

    def set_active(self, active: bool) -> None:
        self.active = active

    def _update_cursor(self, pos=None):
        self.setCursor(Qt.ArrowCursor)

    def set_image(self, image: QImage, device_size=None) -> None:
        self.image = image
        self.device_size = device_size
        self._update_target_rect()
        self.update()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._update_target_rect()

    def _update_target_rect(self):
        if self.image.isNull() or self.width() <= 0 or self.height() <= 0:
            self._target_rect = QRect()
            return
        img_size = (self.image.width(), self.image.height())
        widget_size = (self.width(), self.height())
        if img_size == self._last_image_size and widget_size == self._last_size:
            return
        self._last_image_size = img_size
        self._last_size = widget_size
        scaled = self.image.size().scaled(self.size(), Qt.KeepAspectRatio)
        left = (self.width() - scaled.width()) // 2
        top = (self.height() - scaled.height()) // 2
        self._target_rect = QRect(left, top, scaled.width(), scaled.height())

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#09111a"))
        if self.image.isNull():
            painter.setPen(QColor("#53d6b5"))
            painter.setRenderHint(QPainter.Antialiasing)
            cx, cy = self.width() // 2, self.height() // 2 - 45
            painter.setPen(QPen(QColor("#355a68"), 2))
            painter.drawRoundedRect(cx - 27, cy - 42, 54, 84, 10, 10)
            painter.drawLine(cx - 10, cy + 29, cx + 10, cy + 29)
            painter.setPen(QColor("#cfdfeb"))
            painter.setFont(QFont("PingFang SC", 16))
            painter.drawText(0, cy + 68, self.width(), 30, Qt.AlignCenter, "让设备画面来到桌面")
            painter.setFont(QFont("PingFang SC", 12))
            painter.setPen(QColor("#748ca2"))
            painter.drawText(0, cy + 103, self.width(), 25, Qt.AlignCenter, "连接 USB 设备，选择设备后点击「开始投屏」")
            return
        if not self._target_rect.isValid():
            self._update_target_rect()
        painter.setRenderHint(QPainter.SmoothPixmapTransform)
        painter.drawImage(self._target_rect, self.image)

        # 绘制触摸轨迹流光与小白点光晕
        if self.show_touches and (self._touch_active or self._ripple_opacity > 0.0) and self._touch_pos is not None:
            painter.save()
            if self._target_rect.isValid():
                painter.setClipRect(self._target_rect)
            painter.setRenderHint(QPainter.Antialiasing, True)

            # 1. 拖动轨迹线 (Tapering gesture trail)
            if len(self._touch_trail) >= 2:
                n_trail = len(self._touch_trail)
                for i in range(n_trail - 1):
                    p1 = self._touch_trail[i]
                    p2 = self._touch_trail[i + 1]
                    ratio = (i + 1) / n_trail
                    alpha = int(140 * ratio * (1.0 if self._touch_active else self._ripple_opacity))
                    width = 2.5 + ratio * 3.5
                    pen = QPen(QColor(83, 214, 181, alpha), width, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin)
                    painter.setPen(pen)
                    painter.drawLine(p1, p2)

            # 2. 外圈水波纹/光晕 (Outer ripple ring)
            eff_opacity = 1.0 if self._touch_active else self._ripple_opacity
            alpha_outer = max(0, min(255, int(160 * eff_opacity)))
            alpha_bg = max(0, min(255, int(50 * eff_opacity)))
            painter.setPen(QPen(QColor(83, 214, 181, alpha_outer), 2.0))
            painter.setBrush(QColor(83, 214, 181, alpha_bg))
            painter.drawEllipse(QPointF(self._touch_pos.x(), self._touch_pos.y()), float(self._ripple_radius), float(self._ripple_radius))

            # 3. 中心白色触控小白点 (Inner white touch dot)
            alpha_inner = max(0, min(255, int(245 * eff_opacity)))
            painter.setPen(QPen(QColor(83, 214, 181, alpha_inner), 1.5))
            painter.setBrush(QColor(255, 255, 255, alpha_inner))
            painter.drawEllipse(QPointF(self._touch_pos.x(), self._touch_pos.y()), 5.5, 5.5)

            painter.restore()

    def mouseDoubleClickEvent(self, event):
        if event.button() == Qt.LeftButton:
            self.mousePressEvent(event)

    def coordinates(self, position):
        if self.image.isNull():
            return None
        width, height = self.device_size or (self.image.width(), self.image.height())
        mapped = map_coordinates(position.x(), position.y(), self.width(), self.height(), self.image.width(), self.image.height())
        if mapped is None:
            return None
        mx, my = mapped
        if self.image.width() > 1 and self.image.height() > 1:
            if (self.image.width() <= self.image.height()) == (width <= height):
                scale_x = (width - 1) / (self.image.width() - 1)
                scale_y = (height - 1) / (self.image.height() - 1)
                mx = max(0, min(width - 1, int(round(mx * scale_x))))
                my = max(0, min(height - 1, int(round(my * scale_y))))
            else:
                scale_x = (height - 1) / (self.image.width() - 1)
                scale_y = (width - 1) / (self.image.height() - 1)
                mx = max(0, min(height - 1, int(round(mx * scale_x))))
                my = max(0, min(width - 1, int(round(my * scale_y))))
        if self.user_rotation == 90:
            return (my, height - 1 - mx)
        elif self.user_rotation == 180:
            return (width - 1 - mx, height - 1 - my)
        elif self.user_rotation == 270:
            return (width - 1 - my, mx)
        return (mx, my)

    def mousePressEvent(self, event):
        if event.button() == Qt.LeftButton and self._active and not self.image.isNull():
            self.drag_start = self.coordinates(event.pos())
            self.started_at = time.monotonic()
            if self.show_touches:
                self._fade_timer.stop()
                self._touch_active = True
                self._touch_pos = event.pos()
                self._touch_trail = [event.pos()]
                self._ripple_radius = 14.0
                self._ripple_opacity = 0.9
                self.update()

    def mouseMoveEvent(self, event):
        super().mouseMoveEvent(event)
        if self.show_touches and self._touch_active:
            self._touch_pos = event.pos()
            self._touch_trail.append(event.pos())
            if len(self._touch_trail) > 18:
                self._touch_trail.pop(0)
            self.update()

    def mouseReleaseEvent(self, event):
        if self.show_touches and self._touch_active:
            self._touch_active = False
            self._ripple_radius = 15.0
            self._ripple_opacity = 0.85
            self._fade_timer.start()
            self.update()

        if self.drag_start is not None and self._active:
            end = self.coordinates(event.pos())
            if end is None and self._target_rect.isValid():
                clamped_pos = QPoint(
                    max(self._target_rect.left(), min(self._target_rect.right(), event.pos().x())),
                    max(self._target_rect.top(), min(self._target_rect.bottom(), event.pos().y())),
                )
                end = self.coordinates(clamped_pos)
            if end is None:
                end = self.drag_start

            distance = ((self.drag_start[0] - end[0]) ** 2 + (self.drag_start[1] - end[1]) ** 2) ** 0.5
            duration_ms = int((time.monotonic() - self.started_at) * 1000)

            # Jitter threshold: micro movements (< 36 device px) or quick taps (< 300ms with < 75 device px)
            # are always treated as click (start == end).
            if distance < 36 or (duration_ms < 300 and distance < 75):
                end = self.drag_start

            duration = min(2000, max(100, duration_ms))
            self.gesture.emit(self.drag_start, end, duration)
        self.drag_start = None

    def leaveEvent(self, event):
        super().leaveEvent(event)
        self.setCursor(Qt.ArrowCursor)
        if not self._touch_active:
            self._touch_pos = None
            self._touch_trail.clear()
            self.update()


class MainWindow(QMainWindow):
    def __init__(self, settings: QSettings | None = None):
        super().__init__()
        self.settings = settings or QSettings("HarmonyCast", "HarmonyCast")
        self.setWindowTitle("鸿蒙投屏 · HarmonyCast")
        self.setWindowIcon(QIcon(str(resource_dir() / "app-icon.svg")))
        self.resize(1120, 800)
        self.setMinimumSize(880, 600)
        self.signals = Signals()
        self.signals.completed.connect(lambda callback, result: callback(result))
        self.signals.stream_status.connect(self.on_stream_status)
        self.signals.stream_finished.connect(self.on_stream_finished)
        self.signals.hilog_finished.connect(self.on_hilog_finished)
        self.signals.hilog_status.connect(self.on_hilog_status)
        self.messages: queue.Queue = queue.Queue(maxsize=10000)
        self.frames: queue.Queue = queue.Queue(maxsize=1)
        self.store = LogStore()
        self.devices: list[Device] = []
        self.hdc: HDC | None = None
        self.scan_busy = False
        self.stream: StreamSession | None = None
        self.preview_stop: threading.Event | None = None
        self.hilog: HiLogSession | None = None
        self.active_token = ""
        self.hilog_token = ""
        self.package_token = ""
        self.packages_device = ""
        self.packages_loading = False
        self._selected_bundle = DEFAULT_PACKAGE_FILTER
        self.packages_last_scan = 0.0
        self.hilog_retry_at = 0.0
        self.device_info_data: dict | None = None
        self.info_loading = False
        self.projecting = False
        self.deploying = False
        self.closing = False
        self._current_hdc_proc: subprocess.Popen | None = None
        self._hdc_proc_lock = threading.Lock()
        self.workers: set[threading.Thread] = set()
        self.worker_lock = threading.Lock()
        self.fps_frames = 0
        self.fps_start = time.monotonic()
        self.first_frame_displayed = False
        self.presentation_fullscreen = False
        self.user_rotation = int(self.settings.value("rotation", 0))
        self.auto_start_mirror = str(self.settings.value("auto_start_mirror", "true")).lower() in ("true", "1", "yes")
        self._user_stopped_mirror = not self.auto_start_mirror
        self._mirror_suspended = False
        self._reconnect_attempts = 0
        self._gesture_queue: queue.Queue = queue.Queue(maxsize=10)
        self._gesture_thread = threading.Thread(target=self._run_gesture_worker, name="harmonycast-gesture-worker", daemon=True)
        self._gesture_thread.start()
        self._configure_logging()
        self._build_ui()
        self.load_hdc()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.pump)
        self.timer.start(100)
        self.frame_timer = QTimer(self)
        self.frame_timer.timeout.connect(self.paint_frame)
        self.log_timer = QTimer(self)
        self.log_timer.timeout.connect(self.maintain_logs)
        self.log_timer.start(2000)
        QTimer.singleShot(100, self.refresh_devices)

    def _configure_logging(self):
        self.logger = logging.getLogger("HarmonyCast")
        self.logger.setLevel(logging.DEBUG)
        self.logger.propagate = False
        self.log_directory = data_dir() / "logs"
        self.log_directory.mkdir(parents=True, exist_ok=True)
        self.file_handler = logging.handlers.RotatingFileHandler(
            self.log_directory / "harmonycast.log", maxBytes=5_000_000, backupCount=3, encoding="utf-8",
        )
        self.file_handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
        self.queue_handler = QueueHandler(self.messages)
        self.logger.addHandler(self.file_handler)
        self.logger.addHandler(self.queue_handler)

    def _build_ui(self):
        root = QWidget()
        self.setCentralWidget(root)
        horizontal = QHBoxLayout(root)
        horizontal.setContentsMargins(0, 0, 0, 0)
        horizontal.setSpacing(0)
        self.sidebar = QFrame()
        self.sidebar.setObjectName("Sidebar")
        self.sidebar.setFixedWidth(156)
        side = QVBoxLayout(self.sidebar)
        side.setContentsMargins(12, 24, 12, 16)
        side.setSpacing(10)
        side.addWidget(label("◉  鸿蒙投屏", "Brand"))
        side.addSpacing(22)
        self.info_nav = button("ℹ    设备信息", lambda: None, "Navigation")
        self.mirror_nav = button("▣    投屏", lambda: None, "Navigation")
        self.hdc_nav = button("⚡    常用命令", lambda: None, "Navigation")
        self.logs_nav = button("≡    日志", lambda: None, "Navigation")
        for item in (self.info_nav, self.mirror_nav, self.hdc_nav, self.logs_nav):
            item.setCheckable(True)
            item.setAutoExclusive(True)
            item.setMinimumHeight(42)
            side.addWidget(item)
        self.info_nav.toggled.connect(lambda selected: self.switch_module(0) if selected else None)
        self.mirror_nav.toggled.connect(lambda selected: self.switch_module(1) if selected else None)
        self.hdc_nav.toggled.connect(lambda selected: self.switch_module(2) if selected else None)
        self.logs_nav.toggled.connect(lambda selected: self.switch_module(3) if selected else None)
        side.addStretch()
        side.addWidget(label(f"HarmonyCast\nv{__version__}", "Muted"))
        horizontal.addWidget(self.sidebar)
        self.main_area = QWidget()
        content = QVBoxLayout(self.main_area)
        content.setContentsMargins(0, 0, 0, 0)
        content.setSpacing(0)
        self.header = QFrame()
        self.header.setObjectName("Header")
        heading = QHBoxLayout(self.header)
        heading.setContentsMargins(18, 14, 18, 14)
        self.page_title = label("设备信息", "Title")
        heading.addWidget(self.page_title)
        heading.addStretch()
        self.device_list = QComboBox()
        self.device_list.setMinimumWidth(230)
        self.device_list.setAccessibleName("选择设备")
        self.device_list.currentIndexChanged.connect(self.device_changed)
        heading.addWidget(self.device_list)
        self.refresh_button = button("刷新", self.refresh_devices)
        self.refresh_button.setShortcut("F5")
        heading.addWidget(self.refresh_button)
        self.network_button = button("网络连接", self.connect_network)
        heading.addWidget(self.network_button)
        self.settings_button = button("设置", self.open_settings)
        heading.addWidget(self.settings_button)
        content.addWidget(self.header)
        self.pages = QStackedWidget()
        self.info_panel = self._build_info_panel()
        self.pages.addWidget(self.info_panel)
        self.mirror_page = QWidget()
        mirror = QVBoxLayout(self.mirror_page)
        mirror.setContentsMargins(0, 0, 0, 0)
        mirror.setSpacing(0)
        self.screen = ScreenView()
        self.screen.user_rotation = self.user_rotation
        self.screen.gesture.connect(self.send_gesture)
        self.screen.fullscreen_requested.connect(self.toggle_fullscreen)
        mirror.addWidget(self.screen, 1)
        self.mirror_toolbar = QFrame()
        controls = QHBoxLayout(self.mirror_toolbar)
        controls.setContentsMargins(14, 11, 14, 11)
        self.start_button = button("开始投屏", self.start_mirror, "Primary")
        self.start_button.setShortcut("Ctrl+Return")
        self.stop_button = button("停止投屏", lambda: self.stop_mirror(user_initiated=True), "Danger")
        self.stop_button.hide()
        controls.addWidget(self.start_button)
        controls.addWidget(self.stop_button)
        self.key_buttons = []
        for text, action in [("返回", "back"), ("主页", "home")]:
            key = button(text, lambda checked=False, name=action: self.send_key(name))
            self.key_buttons.append(key)
            controls.addWidget(key)
        self.rotate_button = button("旋转", self.rotate_display)
        self.rotate_button.setShortcut("Ctrl+R")
        self.rotate_button.setToolTip("旋转画面 0° / 90° / 180° / 270° (快捷键: ⌘R)")
        self.key_buttons.append(self.rotate_button)
        controls.addWidget(self.rotate_button)
        self.quality_combo = QComboBox()
        self.quality_combo.addItems(["1080P · 标准 (4M)", "1080P · 流畅 (1.5M)", "1080P · 超清 (8M)"])
        self.quality_combo.setToolTip("投屏画质：1080P 完整全屏，支持动态调节视频流码率")
        bitrate_idx = int(self.settings.value("bitrate", 1))
        combo_idx = {1: 0, 0: 1, 2: 2}.get(bitrate_idx, 0)
        self.quality_combo.setCurrentIndex(combo_idx)
        self.quality_combo.currentIndexChanged.connect(self.on_quality_changed)
        controls.addWidget(self.quality_combo)
        self.state_badge = label("待连接", "Muted")
        controls.addWidget(self.state_badge)
        controls.addStretch()
        self.frame_info = label("按 F11 全屏", "Muted")
        controls.addWidget(self.frame_info)
        self.screenshot_button = button("截图", self.save_screenshot)
        self.screenshot_button.setShortcut("Ctrl+Shift+S")
        controls.addWidget(self.screenshot_button)
        self.fault_report_button = button("故障现场打包", self.export_fault_report)
        self.fault_report_button.setToolTip("一键打包现场高清截图、HiLog日志、进程快照与系统参数为 ZIP 诊断包")
        controls.addWidget(self.fault_report_button)
        self.fullscreen_button = button("全屏", self.toggle_fullscreen)
        controls.addWidget(self.fullscreen_button)
        self.more_button = QPushButton("更多")
        menu = QMenu(self.more_button)
        self.deploy_button = menu.addAction("部署视频投屏服务", self.deploy_server)
        for title, action in [("近期任务", "recent"), ("电源", "power"), ("音量 +", "volume_up"), ("音量 −", "volume_down")]:
            menu.addAction(title, lambda checked=False, name=action: self.send_key(name))
        menu.addSeparator()
        self.touch_action = menu.addAction("显示触摸小白点", self.toggle_touch_indicator)
        self.touch_action.setCheckable(True)
        self.touch_action.setChecked(bool(int(self.settings.value("show_touches", 1))))
        self.screen.show_touches = self.touch_action.isChecked()
        menu.addAction("一键导出故障现场 (ZIP)", self.export_fault_report)
        self.more_button.setMenu(menu)
        controls.addWidget(self.more_button)
        mirror.addWidget(self.mirror_toolbar)
        self.pages.addWidget(self.mirror_page)
        self.hdc_panel = self._build_hdc_panel()
        self.pages.addWidget(self.hdc_panel)
        self.log_panel = QWidget()
        log_layout = QVBoxLayout(self.log_panel)
        log_layout.setContentsMargins(18, 18, 18, 14)
        log_layout.setSpacing(12)
        # 日志分类选择提前实例化，供包名联动使用
        self.category = QComboBox()
        self.category.setAccessibleName("日志分类")
        self.category.addItem("全部日志", "")
        self.category.addItem("应用日志", "app")
        self.category.addItem("系统日志", "system")
        saved_category = self.category.findData(str(self.settings.value("logs/category", "app")))
        self.category.setCurrentIndex(max(0, saved_category))
        self.category.currentIndexChanged.connect(self.log_category_changed)

        # 第一行操作栏：状态Badge + 应用包名选择 + 清空视图 + 导出日志 (隐藏不必要的拉取服务日志按钮)
        log_actions = QHBoxLayout()
        self.log_state = label("连接设备后自动采集", "Badge")
        log_actions.addWidget(self.log_state)

        # 应用包名选择移动至导出日志这一行
        self.package_row = QWidget()
        package_layout = QHBoxLayout(self.package_row)
        package_layout.setContentsMargins(8, 0, 0, 0)
        package_layout.setSpacing(6)
        package_layout.addWidget(label("应用包名:"))
        self.package_combo = QComboBox()
        self.package_combo.setAccessibleName("应用包名")
        self.package_combo.setEditable(True)
        self.package_combo.setMaxVisibleItems(12)
        self.package_combo.setInsertPolicy(QComboBox.NoInsert)
        self.package_combo.addItems([DEFAULT_PACKAGE_FILTER, "全部应用"])
        self.package_combo.lineEdit().setPlaceholderText("包名或前缀，例如 com.yarward.*")
        self.package_combo.completer().setCompletionMode(QCompleter.PopupCompletion)
        self.package_combo.completer().setFilterMode(Qt.MatchContains)
        self.package_combo.completer().setCaseSensitivity(Qt.CaseInsensitive)
        self.package_combo.activated.connect(self.commit_package)
        self.package_combo.lineEdit().editingFinished.connect(self.commit_package)
        self.package_combo.setMinimumWidth(210)
        package_layout.addWidget(self.package_combo, 1)
        self.package_hint = label("应用列表自动更新", "Muted")
        self.package_hint.setMaximumWidth(220)
        package_layout.addWidget(self.package_hint)
        self.package_row.setVisible(self.category.currentData() == "app")
        log_actions.addWidget(self.package_row, 1)

        self.fetch_button = button("拉取投屏服务日志", self.fetch_logs)
        self.fetch_button.hide()

        log_actions.addStretch()
        log_actions.addWidget(button("清空视图", self.clear_log_view))
        log_actions.addWidget(button("导出日志", self.export_logs))
        log_actions.addWidget(button("拉取应用日志", lambda: self.fetch_app_logs_action()))
        log_actions.addWidget(button("导出故障现场", self.export_fault_report))
        log_layout.addLayout(log_actions)

        # 第二行过滤栏：搜索框 + 日志分类 + 级别 + 自动滚动 + 暂停显示 (来源下拉隐藏)
        filters = QHBoxLayout()
        self.search = QLineEdit()
        self.search.setPlaceholderText("搜索内容、Tag 或 PID…")
        self.search.setAccessibleName("搜索日志")
        self.search.textChanged.connect(self.render_logs)
        self.level = QComboBox()
        self.level.addItems(["全部级别", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"])
        self.level.currentIndexChanged.connect(self.render_logs)
        self.source = QComboBox(self)
        self.source.addItems(["全部来源", "HiLog", "工具"])
        self.source.currentIndexChanged.connect(self.render_logs)
        self.source.hide()
        self.autoscroll = QCheckBox("自动滚动")
        self.autoscroll.setChecked(True)
        self.pause = QCheckBox("暂停显示")
        self.pause.toggled.connect(self.render_logs)
        filters.addWidget(self.search, 1)
        filters.addWidget(self.category)
        filters.addWidget(self.level)
        filters.addWidget(self.autoscroll)
        filters.addWidget(self.pause)
        log_layout.addLayout(filters)
        self.log_view = LogView()
        self.log_view.setReadOnly(True)
        self.log_view.setAccessibleName("实时日志内容")
        self.log_view.setFont(QFont("Menlo", 11))
        self.log_view.setLineWrapMode(QTextEdit.NoWrap)
        self.log_view.document().setMaximumBlockCount(5000)
        self.log_highlighter = LogHighlighter(self.log_view.document())
        log_layout.addWidget(self.log_view, 1)
        self.log_count = label("0 条日志", "Muted")
        log_footer = QHBoxLayout()
        log_footer.addWidget(self.log_count)
        log_footer.addStretch()
        font_hint = label("字号  ⌘+/⌘−", "Muted")
        font_hint.setToolTip("⌘ + 放大 · ⌘ − 缩小 · ⌘ 0 恢复默认字号")
        log_footer.addWidget(font_hint)
        self.log_font_size = QSpinBox()
        self.log_font_size.setAccessibleName("日志字号")
        self.log_font_size.setRange(8, 28)
        self.log_font_size.setSuffix(" pt")
        self.log_font_size.setFixedWidth(82)
        self.log_font_size.setKeyboardTracking(False)
        self.log_font_size.setToolTip(font_hint.toolTip())
        try:
            saved_font_size = int(self.settings.value("logs/font_size", 11))
        except (TypeError, ValueError):
            saved_font_size = 11
        self.log_font_size.setValue(saved_font_size)
        self.log_font_size.valueChanged.connect(self.set_log_font_size)
        log_footer.addWidget(self.log_font_size)
        log_layout.addLayout(log_footer)
        self.set_log_font_size(self.log_font_size.value())
        self.log_font_shortcuts = []
        for key, callback in [("Ctrl++", lambda: self.log_font_size.stepUp()),
                              ("Ctrl+=", lambda: self.log_font_size.stepUp()),
                              ("Ctrl+-", lambda: self.log_font_size.stepDown()),
                              ("Ctrl+0", lambda: self.log_font_size.setValue(11))]:
            shortcut = QShortcut(QKeySequence(key), self)
            shortcut.activated.connect(callback)
            self.log_font_shortcuts.append(shortcut)
        self.pages.addWidget(self.log_panel)
        content.addWidget(self.pages, 1)
        horizontal.addWidget(self.main_area, 1)
        # Advanced video options live in Settings, outside the main workflow.
        self.mode = QComboBox(self)
        self.mode.addItems(["自动", "视频投屏", "兼容截图"])
        self.mode.setCurrentIndex(int(self.settings.value("mode", 0)))
        self.fps = QComboBox(self)
        self.fps.addItems(["15 FPS", "30 FPS", "60 FPS"])
        self.fps.setCurrentIndex(int(self.settings.value("fps", 1)))
        self.bitrate = QComboBox(self)
        self.bitrate.addItems(["1.5 Mbps", "4 Mbps", "8 Mbps"])
        self.bitrate.setCurrentIndex(int(self.settings.value("bitrate", 1)))
        self.quality = QComboBox(self)
        self.quality.addItems(["720P · 流畅", "1080P · 原画"])
        self.quality.setCurrentIndex(int(self.settings.value("quality", 0)))
        for option in (self.mode, self.fps, self.bitrate, self.quality):
            option.hide()
        self.mode.currentIndexChanged.connect(self.update_mode)
        self.device_note = QLabel(self)
        self.device_note.hide()
        self.subtitle = QLabel(self)
        self.subtitle.hide()
        self.fullscreen_shortcut = QShortcut(QKeySequence("Ctrl+Meta+F"), self)
        self.fullscreen_shortcut.activated.connect(self.toggle_fullscreen)
        self.fullscreen_f11 = QShortcut(QKeySequence("F11"), self)
        self.fullscreen_f11.activated.connect(self.toggle_fullscreen)
        self.escape_shortcut = QShortcut(QKeySequence("Escape"), self)
        self.escape_shortcut.activated.connect(self.exit_fullscreen)
        self.statusBar().showMessage("连接设备后查看基本信息；点击「投屏」开始投屏")
        self.switch_module(0)
        self.update_actions()

    def switch_module(self, index):
        if isinstance(index, str):
            mapping = {
                "info": 0, "device": 0,
                "mirror": 1, "screen": 1,
                "hdc": 2, "cmd": 2, "commands": 2, "command": 2,
                "logs": 3, "log": 3,
            }
            index = mapping.get(index.lower(), 0)
        if self.isFullScreen():
            self.exit_fullscreen()
        self.pages.setCurrentIndex(index)
        self.info_nav.setChecked(index == 0)
        self.mirror_nav.setChecked(index == 1)
        self.hdc_nav.setChecked(index == 2)
        self.logs_nav.setChecked(index == 3)
        titles = {0: "设备信息", 1: "投屏", 2: "常用命令", 3: "日志"}
        self.page_title.setText(titles.get(index, "设备信息"))
        for shortcut in self.log_font_shortcuts:
            shortcut.setEnabled(index == 3)
        # 标签页休眠：切出投屏页面时自动暂停投屏服务，切回时自动唤醒恢复
        if index != 1 and self.projecting:
            self._mirror_suspended = True
            self.stop_mirror(user_initiated=False)

        if index == 0:
            self.refresh_device_info()
        elif index == 1:
            should_resume = getattr(self, "_mirror_suspended", False)
            self._mirror_suspended = False
            if (should_resume or self.auto_start_mirror) and not self.projecting and self.selected_device() and not getattr(self, "_user_stopped_mirror", False):
                self.start_mirror()
        elif index == 2:
            self.refresh_hdc_target_label()
        elif index == 3:
            self.render_logs()
            self.refresh_packages()
        self.sync_hilog()

    def _build_info_panel(self) -> QWidget:
        panel = QWidget()
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(18, 18, 18, 14)
        layout.setSpacing(12)

        top_bar = QHBoxLayout()
        self.info_badge = label("未连接", "Badge")
        top_bar.addWidget(self.info_badge)
        self.info_device_label = label("请选择设备以查看详情", "Muted")
        top_bar.addWidget(self.info_device_label)
        top_bar.addStretch()

        self.btn_refresh_info = button("刷新设备信息", self.refresh_device_info)
        top_bar.addWidget(self.btn_refresh_info)
        self.btn_copy_all_info = button("复制全部信息", self.copy_all_device_info)
        top_bar.addWidget(self.btn_copy_all_info)
        layout.addLayout(top_bar)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)

        self.info_content = QWidget()
        self.info_cards_layout = QVBoxLayout(self.info_content)
        self.info_cards_layout.setContentsMargins(0, 0, 8, 0)
        self.info_cards_layout.setSpacing(14)

        # 1. Yarward 卡片
        self.card_yarward = QFrame()
        self.card_yarward.setObjectName("InfoCard")
        self.yarward_layout = QVBoxLayout(self.card_yarward)
        self.yarward_layout.setContentsMargins(16, 16, 16, 16)
        self.yarward_layout.setSpacing(10)
        self.info_cards_layout.addWidget(self.card_yarward)

        # 2. ICU 硬件与看门狗卡片
        self.card_icu = QFrame()
        self.card_icu.setObjectName("InfoCard")
        self.icu_layout = QVBoxLayout(self.card_icu)
        self.icu_layout.setContentsMargins(16, 16, 16, 16)
        self.icu_layout.setSpacing(10)
        self.info_cards_layout.addWidget(self.card_icu)

        # 3. 网络与 MAC 地址卡片
        self.card_network = QFrame()
        self.card_network.setObjectName("InfoCard")
        self.network_layout = QVBoxLayout(self.card_network)
        self.network_layout.setContentsMargins(16, 16, 16, 16)
        self.network_layout.setSpacing(10)
        self.info_cards_layout.addWidget(self.card_network)

        # 4. 硬件与系统信息卡片
        self.card_device = QFrame()
        self.card_device.setObjectName("InfoCard")
        self.device_layout = QVBoxLayout(self.card_device)
        self.device_layout.setContentsMargins(16, 16, 16, 16)
        self.device_layout.setSpacing(10)
        self.info_cards_layout.addWidget(self.card_device)

        # 5. 全部应用包清单
        self.card_packages = QFrame()
        self.card_packages.setObjectName("InfoCard")
        self.packages_layout = QVBoxLayout(self.card_packages)
        self.packages_layout.setContentsMargins(16, 16, 16, 16)
        self.packages_layout.setSpacing(10)
        self.info_cards_layout.addWidget(self.card_packages)

        self.info_cards_layout.addStretch()
        scroll.setWidget(self.info_content)
        layout.addWidget(scroll, 1)

        self._render_empty_info()
        return panel

    def _render_empty_info(self, hint: str = "请连接并选择设备以查看详情"):
        self.info_badge.setText("未连接")
        self.info_device_label.setText(hint)
        _clear_layout(self.yarward_layout)
        _clear_layout(self.icu_layout)
        _clear_layout(self.network_layout)
        _clear_layout(self.device_layout)
        _clear_layout(self.packages_layout)

        empty_box = label("请在顶部选择已连接的 OpenHarmony 设备，然后点击「刷新设备信息」。", "Muted")
        self.device_layout.addWidget(empty_box)

    def refresh_device_info(self):
        device = self.selected_device()
        if device is None or self.hdc is None:
            self._render_empty_info("请先连接并选择设备")
            return
        if self.info_loading:
            return
        self.info_loading = True
        self.info_badge.setText("正在查询…")
        self.btn_refresh_info.setEnabled(False)
        self.statusBar().showMessage("正在获取设备基本信息及应用版本…")
        serial = device.serial
        tools = DeviceTools(self.hdc)

        def query():
            return tools.get_device_details(serial)

        def on_done(result):
            self.info_loading = False
            self.btn_refresh_info.setEnabled(True)
            self.render_device_info(result)
            self.statusBar().showMessage(f"设备信息已更新 ({serial[-6:]})", 3000)

        self.submit(query, on_done, "获取设备信息", alert=False)

    def render_device_info(self, data: dict):
        self.device_info_data = data
        model_name = data.get("model") or "未知设备"
        serial = data.get("serial") or ""
        self.info_badge.setText(f"{model_name} · 已更新")
        self.info_device_label.setText(f"序列号: {serial}  |  更新于 {datetime.now().strftime('%H:%M:%S')}")

        # 1. Yarward 专属应用卡片
        _clear_layout(self.yarward_layout)
        hdr_box = QHBoxLayout()
        hdr_box.addWidget(label("★  Yarward 专属应用", "CardTitle"))
        yarward_apps = data.get("yarward_apps", [])
        if yarward_apps:
            hdr_box.addWidget(label(f"发现 {len(yarward_apps)} 个应用", "TagSuccess"))
        else:
            hdr_box.addWidget(label("未发现相关应用", "TagMuted"))
        hdr_box.addStretch()
        self.yarward_layout.addLayout(hdr_box)

        if not yarward_apps:
            hint = label("未检测到包名包含 'yarward' 的应用包 (例如 com.yarward.*)", "Muted")
            self.yarward_layout.addWidget(hint)
        else:
            for app in yarward_apps:
                app_box = QFrame()
                app_box.setObjectName("AppBox")
                box_layout = QVBoxLayout(app_box)
                box_layout.setContentsMargins(14, 12, 14, 12)
                box_layout.setSpacing(8)

                # Row 1: Package Name + Running state + Start/Stop Buttons
                r1 = QHBoxLayout()
                pkg_label = label(app.get("bundle", ""), "Brand")
                pkg_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
                r1.addWidget(pkg_label)
                if app.get("running"):
                    pids_str = ", ".join(app.get("pids", []))
                    r1.addWidget(label(f"● 运行中 (PID: {pids_str})", "TagSuccess"))
                else:
                    r1.addWidget(label("○ 未运行", "TagMuted"))
                r1.addStretch()

                btn_start = button("启动应用", lambda _, s=serial, b=app.get("bundle"), a=app.get("main_ability", ""): self.start_yarward_app(s, b, a))
                btn_stop = button("停止应用", lambda _, s=serial, b=app.get("bundle"): self.stop_yarward_app(s, b), "Danger")
                btn_pull = button("拉取日志", lambda _, b=app.get("bundle"): self.fetch_app_logs_action(b))
                btn_start.setFixedHeight(28)
                btn_stop.setFixedHeight(28)
                btn_pull.setFixedHeight(28)
                btn_pull.setToolTip("从设备拉取该应用的持久化运行日志及 HiAppEvent 异常事件")
                r1.addWidget(btn_start)
                r1.addWidget(btn_stop)
                r1.addWidget(btn_pull)
                box_layout.addLayout(r1)

                # Row 2: Version details
                r2 = QHBoxLayout()
                r2.addWidget(label("版本名称 (versionName):", "InfoKey"))
                v_name = str(app.get("version_name") or "未知")
                v_lbl = label(v_name, "AppVersion")
                v_lbl.setTextInteractionFlags(Qt.TextSelectableByMouse)
                r2.addWidget(v_lbl)
                btn_copy_v = button("复制版本号", lambda _, v=v_name: self.copy_to_clipboard(v, "版本号"))
                btn_copy_v.setFixedHeight(26)
                btn_copy_v.setStyleSheet("padding: 2px 8px; font-size: 11px;")
                r2.addWidget(btn_copy_v)
                r2.addSpacing(16)

                r2.addWidget(label("内部构建号 (versionCode):", "InfoKey"))
                v_code_lbl = label(str(app.get("version_code") or "未知"), "InfoValMono")
                v_code_lbl.setTextInteractionFlags(Qt.TextSelectableByMouse)
                r2.addWidget(v_code_lbl)
                r2.addSpacing(16)

                r2.addWidget(label("目标 API:", "InfoKey"))
                api_lbl = label(str(app.get("target_api") or "未知"), "InfoValMono")
                api_lbl.setTextInteractionFlags(Qt.TextSelectableByMouse)
                r2.addWidget(api_lbl)
                r2.addStretch()
                box_layout.addLayout(r2)

                # Row 3: Meta details & copy action
                r3 = QHBoxLayout()
                r3.addWidget(label(f"入口 Ability: {app.get('main_ability') or '默认'}", "Muted"))
                r3.addSpacing(16)
                r3.addWidget(label(f"更新时间: {app.get('update_time', '未知')}", "Muted"))
                r3.addSpacing(16)
                r3.addWidget(label(f"安装时间: {app.get('install_time', '未知')}", "Muted"))
                r3.addStretch()
                btn_copy_detail = button("复制应用详情", lambda _, a=app: self.copy_app_info(a))
                btn_copy_detail.setFixedHeight(26)
                btn_copy_detail.setStyleSheet("padding: 2px 8px; font-size: 11px;")
                r3.addWidget(btn_copy_detail)
                box_layout.addLayout(r3)

                self.yarward_layout.addWidget(app_box)

        # 2. ICU 硬件与看门狗卡片
        _clear_layout(self.icu_layout)
        icu = data.get("icu", {})
        icu_hdr = QHBoxLayout()
        icu_hdr.addWidget(label("🛡️  亚华医疗 ICU 硬件与看门狗管控", "CardTitle"))

        has_icu = icu.get("available", False)
        watchdog_stopped = icu.get("watchdog_stopped")
        if not has_icu:
            icu_hdr.addWidget(label("通用设备 (未检测到 ICU 节点)", "TagMuted"))
        elif watchdog_stopped is True:
            icu_hdr.addWidget(label("看门狗已暂停 (调试防复位)", "TagWarning"))
        elif watchdog_stopped is False:
            icu_hdr.addWidget(label("看门狗运行中 (超时将自动复位)", "TagSuccess"))
        else:
            icu_hdr.addWidget(label("状态未知", "TagMuted"))
        icu_hdr.addStretch()
        self.icu_layout.addLayout(icu_hdr)

        if not has_icu:
            self.icu_layout.addWidget(label("未在 /sys/class/ICU/version 发现硬件控制节点 (非医疗终端硬件或节点未挂载)", "Muted"))
        else:
            icu_grid = QGridLayout()
            icu_grid.setContentsMargins(0, 4, 0, 4)
            icu_grid.setHorizontalSpacing(24)
            icu_grid.setVerticalSpacing(8)

            model_val = icu.get("model") or "未知"
            ver_val = icu.get("mcu_version") or "未知"
            hw_val = icu.get("hardware_version") or "未知"
            delay_val = icu.get("power_delay") or "0"
            wd_desc = icu.get("watchdog_desc") or "未知"

            disp_model = f"{model_val} (YH-{model_val})" if model_val and not model_val.startswith("YH-") else model_val

            items = [
                ("ICU 硬件型号", disp_model),
                ("MCU 固件版本", ver_val),
                ("硬件版本", hw_val),
                ("关机延时", f"{delay_val} 秒"),
                ("看门狗状态", wd_desc),
            ]
            for i, (k, v) in enumerate(items):
                col = (i % 2) * 2
                row = i // 2
                k_lbl = label(k, "InfoKey")
                k_lbl.setFixedWidth(85)
                icu_grid.addWidget(k_lbl, row, col)
                v_lbl = label(v, "InfoVal")
                v_lbl.setTextInteractionFlags(Qt.TextSelectableByMouse)
                icu_grid.addWidget(v_lbl, row, col + 1)
            self.icu_layout.addLayout(icu_grid)

            icu_actions = QHBoxLayout()
            btn_stop_wd = button("⏸ 暂停看门狗 (调试防复位)", lambda: self.set_watchdog_status_action(True))
            btn_stop_wd.setToolTip("向 /sys/class/ICU/version/StopWatchDog 写入 1，暂停看门狗计时，防止调试或升级时死机重启")

            btn_start_wd = button("▶ 开启看门狗 (启用防护)", lambda: self.set_watchdog_status_action(False))
            btn_start_wd.setToolTip("向 /sys/class/ICU/version/StopWatchDog 写入 0，恢复看门狗监控，超时未喂狗将自动复位")

            btn_feed_wd = button("🦴 手动喂狗一次", self.feed_watchdog_action)
            btn_feed_wd.setToolTip("向 /sys/class/ICU/version/FeedWatchdog 写入 1，执行单次手动喂狗重置计时器")

            btn_refresh_icu = button("🔄 刷新 ICU 状态", self.refresh_device_info)
            btn_refresh_icu.setToolTip("重新查询 ICU 硬件及看门狗状态")

            icu_actions.addWidget(btn_stop_wd)
            icu_actions.addWidget(btn_start_wd)
            icu_actions.addWidget(btn_feed_wd)
            icu_actions.addWidget(btn_refresh_icu)
            icu_actions.addStretch()
            self.icu_layout.addLayout(icu_actions)

        # 3. 网络与 MAC 地址卡片
        _clear_layout(self.network_layout)
        net_hdr = QHBoxLayout()
        net_hdr.addWidget(label("🌐  网络接口与 MAC 地址", "CardTitle"))
        networks = data.get("networks", [])
        net_hdr.addWidget(label(f"{len(networks)} 个接口", "Badge"))
        net_hdr.addStretch()
        self.network_layout.addLayout(net_hdr)

        if not networks:
            self.network_layout.addWidget(label("未检测到有效网络接口", "Muted"))
        else:
            net_grid = QGridLayout()
            net_grid.setContentsMargins(0, 4, 0, 4)
            net_grid.setHorizontalSpacing(14)
            net_grid.setVerticalSpacing(8)
            for row_idx, net in enumerate(networks):
                iface_lbl = label(f"● {net.get('name')} ({net.get('interface')})", "Brand" if net.get("connected") else "Muted")
                net_grid.addWidget(iface_lbl, row_idx, 0)

                mac_val = net.get("mac", "")
                mac_box = QHBoxLayout()
                mac_lbl = label(f"MAC:  {mac_val}", "InfoValMono")
                mac_lbl.setTextInteractionFlags(Qt.TextSelectableByMouse)
                mac_box.addWidget(mac_lbl)
                btn_copy_mac = button("复制 MAC", lambda _, m=mac_val, ifc=net.get('interface'): self.copy_to_clipboard(m, f"{ifc} MAC"))
                btn_copy_mac.setFixedHeight(24)
                btn_copy_mac.setStyleSheet("padding: 1px 6px; font-size: 11px;")
                mac_box.addWidget(btn_copy_mac)
                net_grid.addLayout(mac_box, row_idx, 1)

                ip_val = net.get("ip", "")
                ip_box = QHBoxLayout()
                ip_lbl = label(f"IP:  {ip_val}", "InfoVal" if net.get("connected") else "Muted")
                ip_lbl.setTextInteractionFlags(Qt.TextSelectableByMouse)
                ip_box.addWidget(ip_lbl)
                if net.get("connected") and ip_val:
                    btn_copy_ip = button("复制 IP", lambda _, ip=ip_val, ifc=net.get('interface'): self.copy_to_clipboard(ip, f"{ifc} IP"))
                    btn_copy_ip.setFixedHeight(24)
                    btn_copy_ip.setStyleSheet("padding: 1px 6px; font-size: 11px;")
                    ip_box.addWidget(btn_copy_ip)
                net_grid.addLayout(ip_box, row_idx, 2)

                tag_lbl = label("已联网" if net.get("connected") else "未连接", "TagSuccess" if net.get("connected") else "TagMuted")
                net_grid.addWidget(tag_lbl, row_idx, 3)

            self.network_layout.addLayout(net_grid)

        # 3. 设备硬件与系统信息卡片
        _clear_layout(self.device_layout)
        dev_hdr = QHBoxLayout()
        dev_hdr.addWidget(label("📱  设备基本硬件与系统信息", "CardTitle"))
        dev_hdr.addStretch()
        self.device_layout.addLayout(dev_hdr)

        grid = QGridLayout()
        grid.setContentsMargins(0, 4, 0, 4)
        grid.setHorizontalSpacing(24)
        grid.setVerticalSpacing(8)

        items = [
            ("品牌厂商", f"{data.get('manufacturer', '')} / {data.get('brand', '')}"),
            ("产品型号", f"{data.get('model', '')} ({data.get('model_code', '')})"),
            ("硬件版本", data.get("hardware_version", "未知")),
            ("软件版本", data.get("software_version", "未知")),
            ("操作系统", data.get("os_fullname", "OpenHarmony")),
            ("API 版本", f"API {data.get('api_version', '未知')}"),
            ("CPU 架构", data.get("cpu_abi", "未知")),
            ("安全补丁", data.get("security_patch", "未知")),
            ("Linux 内核", data.get("kernel", "未知")),
            ("屏幕分辨率", data.get("display", "未知")),
            ("开机时长", data.get("uptime", "未知")),
            ("内存使用", data.get("memory", "未知")),
            ("内部存储", data.get("storage", "未知")),
        ]

        row = 0
        for i, (k, v) in enumerate(items):
            col = (i % 2) * 2
            row = i // 2
            k_lbl = label(k, "InfoKey")
            k_lbl.setFixedWidth(80)
            grid.addWidget(k_lbl, row, col)
            v_lbl = label(v, "InfoVal")
            v_lbl.setTextInteractionFlags(Qt.TextSelectableByMouse)
            grid.addWidget(v_lbl, row, col + 1)

        udid_val = data.get("udid", "未知")
        row += 1
        k_udid = label("设备 UDID", "InfoKey")
        k_udid.setFixedWidth(80)
        grid.addWidget(k_udid, row, 0)
        udid_box = QHBoxLayout()
        v_udid = label(udid_val, "InfoValMono")
        v_udid.setTextInteractionFlags(Qt.TextSelectableByMouse)
        udid_box.addWidget(v_udid)
        if udid_val and udid_val != "未知":
            btn_copy_udid = button("复制 UDID", lambda _, u=udid_val: self.copy_to_clipboard(u, "设备 UDID"))
            btn_copy_udid.setFixedHeight(24)
            btn_copy_udid.setStyleSheet("padding: 1px 6px; font-size: 11px;")
            udid_box.addWidget(btn_copy_udid)
        udid_box.addStretch()
        grid.addLayout(udid_box, row, 1, 1, 3)

        self.device_layout.addLayout(grid)

        # 4. 全部已安装应用包清单
        _clear_layout(self.packages_layout)
        all_pkgs = data.get("all_packages", [])
        pkg_hdr = QHBoxLayout()
        pkg_hdr.addWidget(label(f"📦  已安装应用清单 ({len(all_pkgs)} 个)", "CardTitle"))
        pkg_hdr.addStretch()
        self.pkg_search = QLineEdit()
        self.pkg_search.setPlaceholderText("过滤应用包名…")
        self.pkg_search.setFixedWidth(240)
        self.pkg_search.textChanged.connect(self._filter_package_table)
        pkg_hdr.addWidget(self.pkg_search)
        self.packages_layout.addLayout(pkg_hdr)

        self.pkg_table = QTableWidget()
        self.pkg_table.setColumnCount(3)
        self.pkg_table.setHorizontalHeaderLabels(["序号", "应用包名", "操作"])
        self.pkg_table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeToContents)
        self.pkg_table.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        self.pkg_table.horizontalHeader().setSectionResizeMode(2, QHeaderView.ResizeToContents)
        self.pkg_table.verticalHeader().setVisible(False)
        self.pkg_table.setFixedHeight(280)
        self.pkg_table.setRowCount(len(all_pkgs))
        for idx, pkg in enumerate(all_pkgs):
            item_num = QTableWidgetItem(str(idx + 1))
            item_num.setTextAlignment(Qt.AlignCenter)
            self.pkg_table.setItem(idx, 0, item_num)

            item_name = QTableWidgetItem(pkg)
            if "yarward" in pkg.lower():
                item_name.setForeground(QColor("#5ce0c1"))
                font = item_name.font()
                font.setBold(True)
                item_name.setFont(font)
            self.pkg_table.setItem(idx, 1, item_name)

            btn_copy = button("复制", lambda _, p=pkg: self.copy_to_clipboard(p, "包名"))
            btn_copy.setFixedHeight(22)
            btn_copy.setStyleSheet("padding: 0 6px; font-size: 11px;")
            self.pkg_table.setCellWidget(idx, 2, btn_copy)

        self.packages_layout.addWidget(self.pkg_table)

    def _filter_package_table(self, text: str):
        if not hasattr(self, "pkg_table") or self.pkg_table is None:
            return
        query = text.strip().lower()
        for row in range(self.pkg_table.rowCount()):
            item = self.pkg_table.item(row, 1)
            if item is None:
                continue
            visible = (not query) or (query in item.text().lower())
            self.pkg_table.setRowHidden(row, not visible)

    def start_yarward_app(self, serial: str, bundle: str, ability: str = ""):
        if not self.hdc:
            return
        self.statusBar().showMessage(f"正在启动应用 {bundle}…")
        tools = DeviceTools(self.hdc)
        def run_start():
            return tools.start_application(serial, bundle, ability)
        def on_started(out):
            self.statusBar().showMessage(f"应用已启动: {bundle}", 3000)
            QTimer.singleShot(1000, self.refresh_device_info)
        self.submit(run_start, on_started, "启动应用")

    def stop_yarward_app(self, serial: str, bundle: str):
        if not self.hdc:
            return
        self.statusBar().showMessage(f"正在停止应用 {bundle}…")
        tools = DeviceTools(self.hdc)
        def run_stop():
            return tools.stop_application(serial, bundle)
        def on_stopped(out):
            self.statusBar().showMessage(f"应用已停止: {bundle}", 3000)
            QTimer.singleShot(500, self.refresh_device_info)
        self.submit(run_stop, on_stopped, "停止应用")

    def set_watchdog_status_action(self, stop: bool):
        device = self.selected_device()
        if not device or not self.hdc:
            self.alert("未选择设备", "请先选择已连接的设备")
            return
        action_name = "暂停看门狗 (防复位)" if stop else "恢复看门狗"
        serial = device.serial
        tools = DeviceTools(self.hdc)

        def worker():
            tools.set_watchdog_status(serial, stop)
            return tools.get_icu_info(serial)

        def on_done(icu_info):
            self.statusBar().showMessage(f"已设置看门狗：{action_name}", 3000)
            if hasattr(self, "device_info_data") and self.device_info_data:
                self.device_info_data["icu"] = icu_info
                self.device_info_data["watchdog_status"] = icu_info.get("watchdog_desc")
                self.device_info_data["watchdog_stopped"] = icu_info.get("watchdog_stopped")
                self.render_device_info(self.device_info_data)
            else:
                self.refresh_device_info()

        self.submit(worker, on_done, f"设置看门狗 ({action_name})")

    def feed_watchdog_action(self):
        device = self.selected_device()
        if not device or not self.hdc:
            self.alert("未选择设备", "请先选择已连接的设备")
            return
        serial = device.serial
        tools = DeviceTools(self.hdc)

        def worker():
            return tools.feed_watchdog(serial)

        def on_done(_):
            self.statusBar().showMessage("看门狗手动喂狗成功 (FeedWatchdog=1)", 3000)

        self.submit(worker, on_done, "手动喂狗")

    def fetch_app_logs_action(self, bundle: str | None = None):
        device = self.selected_device()
        if not device or not self.hdc:
            self.alert("未选择设备", "请先选择已连接的设备")
            return
        serial = device.serial
        tools = DeviceTools(self.hdc)

        target_bundle = bundle or (self.package_combo.currentData() if hasattr(self, "package_combo") else "") or "com.yarward.*"
        default_dir = str(Path.home() / "Downloads")
        dest_dir = QFileDialog.getExistingDirectory(self, "选择保存应用日志的本地目录", default_dir)
        if not dest_dir:
            return

        def worker():
            return tools.fetch_app_logs(serial, dest_dir, bundle=target_bundle)

        def on_done(files: list[Path]):
            if not files:
                self.alert("未拉取到日志", f"未在设备上扫描到应用持久化日志文件 (目标包: {target_bundle})")
                return
            count = len(files)
            total_size = sum(f.stat().st_size for f in files if f.is_file())
            msg = f"成功拉取 {count} 个应用持久化日志文件到：\n{dest_dir}\n\n总大小: {format_bytes(total_size)}\n包含应用运行日志与 HiAppEvent 异常事件落盘。"
            self.statusBar().showMessage(f"已拉取 {count} 个应用日志", 4000)
            reply = QMessageBox.information(self, "应用日志拉取成功", msg, QMessageBox.Open | QMessageBox.Ok, QMessageBox.Open)
            if reply == QMessageBox.Open:
                QDesktopServices.openUrl(QUrl.fromLocalFile(dest_dir))

        self.submit(worker, on_done, "拉取应用日志")

    def copy_to_clipboard(self, text: str, hint: str = "内容"):
        QApplication.clipboard().setText(text)
        self.statusBar().showMessage(f"已复制到剪贴板：{hint} ({text})", 3000)

    def copy_app_info(self, app: dict):
        text = (
            f"应用包名: {app.get('bundle', '')}\n"
            f"版本名称 (versionName): {app.get('version_name', '')}\n"
            f"内部构建号 (versionCode): {app.get('version_code', '')}\n"
            f"目标 API: {app.get('target_api', '')}\n"
            f"入口 Ability: {app.get('main_ability', '')}\n"
            f"运行状态: {'运行中 (PID: ' + ', '.join(app.get('pids', [])) + ')' if app.get('running') else '未运行'}\n"
            f"更新时间: {app.get('update_time', '')}\n"
            f"安装时间: {app.get('install_time', '')}"
        )
        self.copy_to_clipboard(text, f"应用详情 [{app.get('bundle')}]")

    def copy_all_device_info(self):
        if not hasattr(self, "device_info_data") or not self.device_info_data:
            self.statusBar().showMessage("无可用设备信息可复制", 3000)
            return
        d = self.device_info_data
        lines = [
            f"# 设备基本信息 ({d.get('serial', '')})",
            f"- 品牌厂商: {d.get('manufacturer', '')} / {d.get('brand', '')}",
            f"- 产品型号: {d.get('model', '')} ({d.get('model_code', '')})",
            f"- 硬件版本: {d.get('hardware_version', '')}",
            f"- 软件版本: {d.get('software_version', '')}",
            f"- 操作系统: {d.get('os_fullname', '')} (API {d.get('api_version', '')})",
            f"- CPU 架构: {d.get('cpu_abi', '')}",
            f"- 设备 UDID: {d.get('udid', '')}",
            f"- Linux 内核: {d.get('kernel', '')}",
            f"- 屏幕分辨率: {d.get('display', '')}",
            f"- 运行时间: {d.get('uptime', '')}",
            f"- 内存使用: {d.get('memory', '')}",
            f"- 存储空间: {d.get('storage', '')}",
            "",
        ]
        icu = d.get("icu", {})
        if icu.get("available"):
            lines.extend([
                "## ICU 硬件与看门狗",
                f"- ICU 模块型号: {icu.get('model', '')}",
                f"- MCU 固件版本: {icu.get('mcu_version', '')}",
                f"- 看门狗状态: {icu.get('watchdog_desc', '')}",
                f"- 关机延时: {icu.get('power_delay', '')}",
                "",
            ])
        lines.append("## 网络接口与 MAC 地址")
        for net in d.get("networks", []):
            lines.append(f"- {net.get('name')} ({net.get('interface')}): MAC={net.get('mac')}, IP={net.get('ip')}")
        lines.append("")
        lines.append("## Yarward 相关应用")
        for app in d.get("yarward_apps", []):
            lines.append(f"- {app.get('bundle')}: 版本={app.get('version_name')} (构建号={app.get('version_code')}), 运行状态={'运行中' if app.get('running') else '未运行'}")
        lines.append("")
        lines.append(f"已安装全部应用数: {len(d.get('all_packages', []))}")
        content = "\n".join(lines)
        self.copy_to_clipboard(content, "全部设备信息")

    def _build_hdc_panel(self) -> QWidget:
        panel = QWidget()
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(18, 16, 18, 14)
        layout.setSpacing(10)

        # 顶部目标设备状态栏
        top_bar = QHBoxLayout()
        self.hdc_target_badge = label("目标: 未选择设备", "Badge")
        top_bar.addWidget(self.hdc_target_badge)
        self.hdc_desc_label = label("设备命令自动指定目标设备；支持一键常用操作与自定义交互命令。", "Muted")
        top_bar.addWidget(self.hdc_desc_label)
        top_bar.addStretch()
        layout.addLayout(top_bar)

        # 垂直分割：上半部为卡片与命令输入，下半部为终端输出
        splitter = QSplitter(Qt.Vertical)
        splitter.setChildrenCollapsible(False)

        top_widget = QWidget()
        top_layout = QVBoxLayout(top_widget)
        top_layout.setContentsMargins(0, 0, 0, 0)
        top_layout.setSpacing(10)

        # 2 个精炼常用卡片：设备控制与快捷键、应用与文件传输
        cards_grid = QGridLayout()
        cards_grid.setSpacing(10)

        # 1. 设备与系统控制
        card_sys = QFrame()
        card_sys.setObjectName("InfoCard")
        sys_layout = QVBoxLayout(card_sys)
        sys_layout.setContentsMargins(14, 12, 14, 12)
        sys_layout.setSpacing(8)
        sys_layout.addWidget(label("⚡  设备与系统控制", "CardTitle"))

        btn_grid_sys = QGridLayout()
        btn_grid_sys.setSpacing(6)
        btn_home = button("⌂ 返回桌面", lambda: self.quick_hdc_key("home", "返回桌面"))
        btn_home.setToolTip("返回系统主桌面（深度适配，兼容通用应用及以 Home 属性注册的沉浸式/Kiosk医疗终端分机）")
        btn_back = button("↩ 返回上一级", lambda: self.quick_hdc_key("back", "返回上一级"))
        btn_back.setToolTip("执行 uinput -K -d 2 -u 2，返回上一级页面")
        btn_power = button("⏻ 电源/锁屏", lambda: self.quick_hdc_key("power", "电源/锁屏"))
        btn_power.setToolTip("执行 uinput -K -d 18 -u 18，唤醒或关闭屏幕")

        btn_mount = button("⚡ 挂载读写 (mount)", self.quick_hdc_mount)
        btn_mount.setToolTip("执行 hdc target mount，将系统/根分区重新挂载为读写状态")
        btn_screenshot = button("📸 屏幕截图", self.quick_hdc_screenshot)
        btn_screenshot.setToolTip("调用硬件 snapshot_display 截屏并自动拉取到本地保存")
        btn_reboot = button("🔄 重启设备", self.quick_hdc_reboot, "Danger")
        btn_reboot.setToolTip("向设备发送 shell reboot 指令重启系统")

        btn_stop_wd = button("⏸ 禁用看门狗 (防复位)", lambda: self.set_watchdog_status_action(True))
        btn_stop_wd.setToolTip("执行 echo -n 1 > /sys/class/ICU/version/StopWatchDog，停止看门狗计时，防止调试重启")
        btn_start_wd = button("▶ 开启看门狗", lambda: self.set_watchdog_status_action(False))
        btn_start_wd.setToolTip("执行 echo -n 0 > /sys/class/ICU/version/StopWatchDog，恢复看门狗监控")
        btn_feed_wd = button("🦴 手动喂狗一次", self.feed_watchdog_action)
        btn_feed_wd.setToolTip("执行 echo -n 1 > /sys/class/ICU/version/FeedWatchdog，执行单次喂狗")

        btn_grid_sys.addWidget(btn_home, 0, 0)
        btn_grid_sys.addWidget(btn_back, 0, 1)
        btn_grid_sys.addWidget(btn_power, 0, 2)
        btn_grid_sys.addWidget(btn_mount, 1, 0)
        btn_grid_sys.addWidget(btn_screenshot, 1, 1)
        btn_grid_sys.addWidget(btn_reboot, 1, 2)
        btn_grid_sys.addWidget(btn_stop_wd, 2, 0)
        btn_grid_sys.addWidget(btn_start_wd, 2, 1)
        btn_grid_sys.addWidget(btn_feed_wd, 2, 2)
        sys_layout.addLayout(btn_grid_sys)
        cards_grid.addWidget(card_sys, 0, 0)

        # 2. 应用与文件传输
        card_app = QFrame()
        card_app.setObjectName("InfoCard")
        app_layout = QVBoxLayout(card_app)
        app_layout.setContentsMargins(14, 12, 14, 12)
        app_layout.setSpacing(8)
        app_layout.addWidget(label("📦  应用与文件", "CardTitle"))

        btn_grid_app = QGridLayout()
        btn_grid_app.setSpacing(6)
        btn_install = button("安装 HAP 应用...", self.quick_hdc_install_hap)
        btn_install.setToolTip("选择本地 .hap 文件通过 hdc app install -r 安装到设备")
        btn_uninstall = button("卸载指定应用...", self.quick_hdc_uninstall_app)
        btn_uninstall.setToolTip("输入 Bundle 名称通过 hdc app uninstall 卸载应用")
        btn_clean = button("清除应用数据...", self.quick_hdc_clean_app)
        btn_clean.setToolTip("执行 bm clean -c -b 清除应用缓存与用户数据")

        btn_send = button("推送文件 (send)...", self.quick_hdc_send_file)
        btn_send.setToolTip("选择本地文件并推送到设备指定目录 (例如 /data/local/tmp/)")
        btn_recv = button("拉取文件 (recv)...", self.quick_hdc_recv_file)
        btn_recv.setToolTip("从设备拉取指定文件保存到本地 Downloads 目录")
        btn_net = button("查网络 IP (ifconfig)", lambda: self.run_hdc_cmd_async(["shell", "ifconfig"], title="查看网络配置"))
        btn_net.setToolTip("执行 ifconfig 查看设备各网卡 IP 地址与连接状态")

        btn_pull_disk_log = button("📂 拉取业务应用日志...", lambda: self.fetch_app_logs_action("com.yarward.*"))
        btn_pull_disk_log.setToolTip("批量拉取设备端业务应用落盘日志及 HiAppEvent 异常事件")

        btn_grid_app.addWidget(btn_install, 0, 0)
        btn_grid_app.addWidget(btn_uninstall, 0, 1)
        btn_grid_app.addWidget(btn_clean, 0, 2)
        btn_grid_app.addWidget(btn_send, 1, 0)
        btn_grid_app.addWidget(btn_recv, 1, 1)
        btn_grid_app.addWidget(btn_net, 1, 2)
        btn_grid_app.addWidget(btn_pull_disk_log, 2, 0, 1, 3)
        app_layout.addLayout(btn_grid_app)
        cards_grid.addWidget(card_app, 0, 1)

        top_layout.addLayout(cards_grid)

        # 命令交互输入区
        cmd_box = QFrame()
        cmd_box.setObjectName("AppBox")
        cmd_box_layout = QVBoxLayout(cmd_box)
        cmd_box_layout.setContentsMargins(12, 10, 12, 10)
        cmd_box_layout.setSpacing(8)

        # Row 1: 常用命令模板选择
        template_row = QHBoxLayout()
        template_row.setSpacing(8)
        template_row.addWidget(label("常用模板:"))
        self.hdc_preset_combo = QComboBox()
        self.hdc_preset_combo.addItems([
            "—— 快速选择常用命令模板 (点击填入) ——",
            "target mount",
            "shell ifconfig",
            "shell cat /sys/class/ICU/version/StopWatchDog",
            "shell echo -n 1 > /sys/class/ICU/version/StopWatchDog",
            "shell echo -n 0 > /sys/class/ICU/version/StopWatchDog",
            "shell echo -n 1 > /sys/class/ICU/version/FeedWatchdog",
            "shell ls -l /data/app/el2/100/base/com.yarward.*/haps/*/files/log/",
            "shell param get",
            "shell bm dump -a",
            "shell ps -A",
            "shell reboot",
            "list targets",
        ])
        self.hdc_preset_combo.currentIndexChanged.connect(self.on_hdc_preset_selected)
        template_row.addWidget(self.hdc_preset_combo, 1)
        cmd_box_layout.addLayout(template_row)

        # Row 2: 交互输入框 + 执行 + 中断
        input_row = QHBoxLayout()
        input_row.setSpacing(8)
        input_row.addWidget(label("hdc", "Brand"))
        self.hdc_target_prefix = label("-t <未选>", "TagMuted")
        input_row.addWidget(self.hdc_target_prefix)
        self.hdc_input = QLineEdit()
        self.hdc_input.setPlaceholderText("输入命令参数，例如 shell ls -la /data 或 target mount (回车直接执行)...")
        self.hdc_input.returnPressed.connect(self.execute_custom_hdc)
        input_row.addWidget(self.hdc_input, 1)
        self.btn_run_hdc = button("执行", self.execute_custom_hdc, "Primary")
        self.btn_kill_hdc = button("中断", self.interrupt_hdc_command, "Danger")
        self.btn_kill_hdc.setEnabled(False)
        input_row.addWidget(self.btn_run_hdc)
        input_row.addWidget(self.btn_kill_hdc)
        cmd_box_layout.addLayout(input_row)

        top_layout.addWidget(cmd_box)
        splitter.addWidget(top_widget)

        # 下半部分：终端控制台输出
        term_widget = QWidget()
        term_layout = QVBoxLayout(term_widget)
        term_layout.setContentsMargins(0, 0, 0, 0)
        term_layout.setSpacing(8)

        term_bar = QHBoxLayout()
        term_bar.addWidget(label("终端控制台输出", "CardTitle"))
        self.hdc_status_label = label("就绪", "Muted")
        term_bar.addWidget(self.hdc_status_label)
        term_bar.addStretch()
        term_bar.addWidget(button("复制输出", self.copy_hdc_output))
        term_bar.addWidget(button("清空终端", self.clear_hdc_terminal))
        term_bar.addWidget(button("导出为文件...", self.export_hdc_output))
        term_layout.addLayout(term_bar)

        self.hdc_terminal = QTextEdit()
        self.hdc_terminal.setReadOnly(True)
        self.hdc_terminal.setFont(QFont("Menlo", 11))
        self.hdc_terminal.setLineWrapMode(QTextEdit.NoWrap)
        self.hdc_terminal.setStyleSheet("background: #080e15; color: #e7edf4; border: 1px solid #1f2d3d; border-radius: 6px; padding: 10px; font-family: 'Menlo', 'Courier New', monospace;")
        term_layout.addWidget(self.hdc_terminal, 1)

        splitter.addWidget(term_widget)
        splitter.setSizes([330, 320])
        layout.addWidget(splitter, 1)

        return panel

    def refresh_hdc_target_label(self):
        if not hasattr(self, "hdc_target_badge"):
            return
        device = self.selected_device()
        if device:
            self.hdc_target_badge.setText(f"目标设备: {device.serial} ({device.model})")
            if hasattr(self, "hdc_target_prefix"):
                self.hdc_target_prefix.setText(f"-t {device.serial}")
        else:
            self.hdc_target_badge.setText("目标: 未选择设备")
            if hasattr(self, "hdc_target_prefix"):
                self.hdc_target_prefix.setText("-t <未选>")

    def on_hdc_preset_selected(self, index: int):
        if index <= 0:
            return
        text = self.hdc_preset_combo.itemText(index)
        self.hdc_input.setText(text)
        self.hdc_input.setFocus()
        self.hdc_preset_combo.blockSignals(True)
        self.hdc_preset_combo.setCurrentIndex(0)
        self.hdc_preset_combo.blockSignals(False)

    def execute_custom_hdc(self):
        cmd_text = self.hdc_input.text().strip()
        if not cmd_text:
            return
        try:
            args = shlex.split(cmd_text)
        except ValueError:
            args = cmd_text.split()
        if not args:
            return
        if args[0].lower() in ("hdc", "hdc.exe"):
            args = args[1:]
        if not args:
            return
        target_device = True
        if len(args) >= 2 and args[0] == "-t":
            target_device = False
        elif args[0] in ("list", "kill", "tconn", "help", "version", "checkserver"):
            target_device = False
        self.run_hdc_cmd_async(args, target_device=target_device, title=f"自定义: {' '.join(args[:2])}")

    def run_hdc_cmd_async(self, args: list[str], target_device: bool = True, title: str = ""):
        if self.hdc is None:
            self._append_hdc_output("[错误] 未配置或未找到 HDC 工具，请在「设置」中配置 HDC 路径。\n", color="#ff7b72")
            return

        serial = None
        if target_device:
            device = self.selected_device()
            if not device:
                self._append_hdc_output("[错误] 未检测到或未选择目标设备，请先在顶部下拉列表选择设备。\n", color="#ff7b72")
                return
            serial = device.serial

        if hasattr(self.hdc, "command"):
            full_cmd = self.hdc.command(args, serial)
        else:
            full_cmd = [str(getattr(self.hdc, "executable", "hdc")), *(["-t", serial] if serial else []), *map(str, args)]

        cmd_display = " ".join(args)
        target_str = f" -t {serial}" if serial else ""

        now_str = datetime.now().strftime("%H:%M:%S")
        self._append_hdc_output(f"[{now_str}] $ hdc{target_str} {cmd_display}\n", color="#5ce0c1", is_command=True)

        self.btn_run_hdc.setEnabled(False)
        self.btn_kill_hdc.setEnabled(True)
        self.hdc_status_label.setText(f"正在执行: {title or cmd_display}…")

        def worker():
            start_t = time.monotonic()
            stdout_data, stderr_data = "", ""
            return_code = 0
            is_timeout = False
            try:
                with self._hdc_proc_lock:
                    self._current_hdc_proc = subprocess.Popen(
                        full_cmd,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                        encoding="utf-8",
                        errors="replace"
                    )
                    proc = self._current_hdc_proc

                stdout_data, stderr_data = proc.communicate(timeout=45)
                return_code = proc.returncode
            except subprocess.TimeoutExpired:
                is_timeout = True
                with self._hdc_proc_lock:
                    if self._current_hdc_proc:
                        try:
                            self._current_hdc_proc.kill()
                        except Exception:
                            pass
            except Exception as exc:
                stderr_data = str(exc)
                return_code = -1
            finally:
                elapsed = time.monotonic() - start_t
                with self._hdc_proc_lock:
                    self._current_hdc_proc = None

            def on_finished(_):
                self.btn_run_hdc.setEnabled(True)
                self.btn_kill_hdc.setEnabled(False)
                if is_timeout:
                    self._append_hdc_output(f"[超时] 命令执行超过 45 秒已终止 (耗时 {elapsed:.2f}s)\n\n", color="#ff7b72")
                    self.hdc_status_label.setText(f"执行超时 ({elapsed:.2f}s)")
                else:
                    if stdout_data:
                        self._append_hdc_output(stdout_data.rstrip() + "\n", color="#e7edf4")
                    if stderr_data:
                        self._append_hdc_output(stderr_data.rstrip() + "\n", color="#ff7b72")
                    if return_code == 0:
                        self._append_hdc_output(f"[✓ 执行完成 · 耗时 {elapsed:.2f}s · 退出码 0]\n\n", color="#5ce0c1")
                        self.hdc_status_label.setText(f"就绪 · 上次耗时 {elapsed:.2f}s")
                    else:
                        self._append_hdc_output(f"[✗ 执行失败 · 耗时 {elapsed:.2f}s · 退出码 {return_code}]\n\n", color="#ff7b72")
                        self.hdc_status_label.setText(f"执行失败 (退出码 {return_code})")

            if not self.closing:
                self.signals.completed.emit(on_finished, None)

        self.start_worker(worker, "harmonycast-hdc-runner")

    def _append_hdc_output(self, text: str, color: str = "#e7edf4", is_command: bool = False):
        if not hasattr(self, "hdc_terminal"):
            return
        cursor = self.hdc_terminal.textCursor()
        cursor.movePosition(QTextCursor.End)
        escaped = html.escape(text).replace("\n", "<br/>").replace(" ", "&nbsp;")
        if is_command:
            html_snippet = f"<div style='font-family: Menlo, monospace; margin-top: 8px; margin-bottom: 2px; line-height: 1.4;'><b style='color: {color};'>{escaped}</b></div>"
        else:
            html_snippet = f"<span style='color: {color}; font-family: Menlo, monospace; line-height: 1.3;'>{escaped}</span>"
        cursor.insertHtml(html_snippet)
        cursor.movePosition(QTextCursor.End)
        self.hdc_terminal.setTextCursor(cursor)

    def interrupt_hdc_command(self):
        with self._hdc_proc_lock:
            proc = self._current_hdc_proc
        if proc:
            try:
                proc.terminate()
            except Exception:
                pass
            self._append_hdc_output("[!] 已向进程发送终止信号\n", color="#f3c969")
            self.hdc_status_label.setText("已发送终止信号")

    def quick_hdc_key(self, key_name: str, title: str = ""):
        if key_name == "home":
            home_cmd = (
                "sh -c '"
                "aa start -b com.ohos.launcher -a com.ohos.launcher.settings.MainAbility >/dev/null 2>&1; "
                "sleep 0.15; "
                "uinput -K -d 2 -u 2 >/dev/null 2>&1; "
                "uinput -K -d 1 -u 1 >/dev/null 2>&1; "
                "which uitest >/dev/null 2>&1 && uitest uiInput keyEvent Home >/dev/null 2>&1; "
                "echo \"[+] 已执行返回桌面指令 (兼容 Kiosk/沉浸式应用)\"'"
            )
            self.run_hdc_cmd_async(["shell", home_cmd], title="返回主桌面")
            return

        keycodes = {"home": 1, "back": 2, "volume_up": 16, "volume_down": 17, "power": 18, "recent": 2073}
        code = keycodes.get(key_name)
        if code is None:
            return
        self.run_hdc_cmd_async(
            ["shell", "uinput", "-K", "-d", str(code), "-u", str(code)],
            title=title or f"按键: {key_name}"
        )

    def quick_hdc_mount(self):
        self.run_hdc_cmd_async(["target", "mount"], title="挂载系统读写")

    def quick_hdc_reboot(self):
        reply = QMessageBox.question(
            self, "确认重启设备",
            "确定要向设备发送 reboot 重启命令吗？\n重启过程中设备与投屏连接将会暂时断开。",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No
        )
        if reply == QMessageBox.Yes:
            self.run_hdc_cmd_async(["shell", "reboot"], title="重启设备")

    def quick_hdc_restart_server(self):
        self.run_hdc_cmd_async(["kill", "-r"], target_device=False, title="重启 HDC 服务")

    def quick_hdc_screenshot(self):
        device = self.selected_device()
        if not device or not self.hdc:
            self._append_hdc_output("[错误] 未选择设备，无法截屏\n", color="#ff7b72")
            return

        serial = device.serial
        ts = int(time.time())
        remote_file = f"/data/local/tmp/screen_{ts}.jpeg"
        local_dir = Path.home() / "Pictures"
        if not local_dir.is_dir():
            local_dir = Path.home() / "Downloads"
        local_file = str(local_dir / f"screen_{serial[-6:]}_{ts}.jpeg")

        now_str = datetime.now().strftime("%H:%M:%S")
        self._append_hdc_output(f"[{now_str}] $ 设备硬件截图: snapshot_display -> {remote_file} -> {local_file}\n", color="#5ce0c1", is_command=True)
        self.hdc_status_label.setText("正在执行设备屏幕截图…")
        self.btn_run_hdc.setEnabled(False)
        self.btn_kill_hdc.setEnabled(True)

        def worker():
            start_t = time.monotonic()
            try:
                cmd1 = [str(getattr(self.hdc, "executable", "hdc")), "-t", serial, "shell", "snapshot_display", "-f", remote_file]
                res1 = subprocess.run(cmd1, capture_output=True, text=True, timeout=12)
                if res1.returncode != 0:
                    raise HDCError(res1.stderr or res1.stdout or "snapshot_display 失败")

                cmd2 = [str(getattr(self.hdc, "executable", "hdc")), "-t", serial, "file", "recv", remote_file, local_file]
                res2 = subprocess.run(cmd2, capture_output=True, text=True, timeout=15)
                if res2.returncode != 0:
                    raise HDCError(res2.stderr or res2.stdout or "file recv 失败")

                try:
                    subprocess.run([str(getattr(self.hdc, "executable", "hdc")), "-t", serial, "shell", "rm", "-f", remote_file], timeout=5)
                except Exception:
                    pass

                elapsed = time.monotonic() - start_t
                def on_done(_):
                    self.btn_run_hdc.setEnabled(True)
                    self.btn_kill_hdc.setEnabled(False)
                    self._append_hdc_output(f"[✓ 截图成功] 文件已保存至: {local_file} (耗时 {elapsed:.2f}s)\n\n", color="#5ce0c1")
                    self.hdc_status_label.setText(f"截图完成 ({elapsed:.2f}s)")
                    self.statusBar().showMessage(f"屏幕截图已保存到: {local_file}", 4000)
                    QDesktopServices.openUrl(QUrl.fromLocalFile(local_file))

                if not self.closing:
                    self.signals.completed.emit(on_done, None)
            except Exception as exc:
                elapsed = time.monotonic() - start_t
                def on_fail(_):
                    self.btn_run_hdc.setEnabled(True)
                    self.btn_kill_hdc.setEnabled(False)
                    self._append_hdc_output(f"[✗ 截图失败] {exc} (耗时 {elapsed:.2f}s)\n\n", color="#ff7b72")
                    self.hdc_status_label.setText("截图失败")
                if not self.closing:
                    self.signals.completed.emit(on_fail, None)

        self.start_worker(worker, "harmonycast-screenshot-task")

    def quick_hdc_install_hap(self):
        path, _ = QFileDialog.getOpenFileName(self, "选择 HAP 安装包", "", "HarmonyOS HAP (*.hap);;所有文件 (*)")
        if path:
            self.run_hdc_cmd_async(["app", "install", "-r", path], title=f"安装应用 {Path(path).name}")

    def quick_hdc_uninstall_app(self):
        bundle, ok = QInputDialog.getText(self, "卸载应用", "请输入要卸载的应用包名 (Bundle Name):", text="")
        if ok and bundle.strip():
            self.run_hdc_cmd_async(["app", "uninstall", bundle.strip()], title=f"卸载应用 {bundle.strip()}")

    def quick_hdc_force_stop_app(self):
        bundle, ok = QInputDialog.getText(self, "强制停止应用", "请输入要停止的应用包名 (Bundle Name):", text="")
        if ok and bundle.strip():
            self.run_hdc_cmd_async(["shell", "aa", "force-stop", bundle.strip()], title=f"停止应用 {bundle.strip()}")

    def quick_hdc_clean_app(self):
        bundle, ok = QInputDialog.getText(self, "清除应用数据", "请输入要清除数据的应用包名 (Bundle Name):", text="")
        if ok and bundle.strip():
            self.run_hdc_cmd_async(["shell", "bm", "clean", "-c", "-b", bundle.strip()], title=f"清理数据 {bundle.strip()}")

    def quick_hdc_send_file(self):
        local_path, _ = QFileDialog.getOpenFileName(self, "选择要推送的本地文件", "", "所有文件 (*)")
        if local_path:
            filename = Path(local_path).name
            remote_path, ok = QInputDialog.getText(self, "推送到设备", "请输入设备端存储路径:", text=f"/data/local/tmp/{filename}")
            if ok and remote_path.strip():
                self.run_hdc_cmd_async(["file", "send", local_path, remote_path.strip()], title=f"推送文件 {filename}")

    def quick_hdc_recv_file(self):
        remote_path, ok = QInputDialog.getText(self, "从设备拉取文件", "请输入设备端文件绝对路径:", text="/data/local/tmp/")
        if ok and remote_path.strip():
            filename = Path(remote_path.strip()).name or "downloaded_file"
            dest_dir = QFileDialog.getExistingDirectory(self, "选择本地保存目录", str(Path.home() / "Downloads"))
            if dest_dir:
                local_dest = str(Path(dest_dir) / filename)
                self.run_hdc_cmd_async(["file", "recv", remote_path.strip(), local_dest], title=f"拉取文件 {filename}")

    def copy_hdc_output(self):
        text = self.hdc_terminal.toPlainText()
        if text:
            QApplication.clipboard().setText(text)
            self.statusBar().showMessage("已复制终端输出内容到剪贴板", 3000)
        else:
            self.statusBar().showMessage("终端无内容可复制", 2000)

    def clear_hdc_terminal(self):
        self.hdc_terminal.clear()
        self.hdc_status_label.setText("就绪")

    def export_hdc_output(self):
        text = self.hdc_terminal.toPlainText()
        if not text.strip():
            self.statusBar().showMessage("终端无内容可导出", 2000)
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "导出终端输出",
            str(Path.home() / f"hdc_output_{datetime.now().strftime('%Y%m%d_%H%M%S')}.txt"),
            "文本文件 (*.txt);;所有文件 (*)"
        )
        if path:
            Path(path).write_text(text, encoding="utf-8")
            self.statusBar().showMessage(f"终端输出已保存至 {path}", 4000)

    def toggle_fullscreen(self):
        if self.isFullScreen():
            self.exit_fullscreen()
            return
        self.was_maximized = self.isMaximized()
        self.presentation_fullscreen = True
        self.pages.setCurrentWidget(self.mirror_page)
        self.stop_hilog()
        for shortcut in self.log_font_shortcuts:
            shortcut.setEnabled(False)
        self.sidebar.hide()
        self.header.hide()
        self.mirror_toolbar.hide()
        self.statusBar().hide()
        self.showFullScreen()

    def exit_fullscreen(self):
        if not self.isFullScreen():
            return
        self.showMaximized() if self.was_maximized else self.showNormal()
        self.sidebar.show()
        self.header.show()
        self.mirror_toolbar.show()
        self.statusBar().show()
        self.presentation_fullscreen = False
        self.switch_module(1)

    def changeEvent(self, event):
        super().changeEvent(event)
        if event.type() != QEvent.WindowStateChange or not hasattr(self, "pages"):
            return
        if self.isFullScreen():
            if not self.presentation_fullscreen:
                self.was_maximized = bool(event.oldState() & Qt.WindowMaximized)
            self.presentation_fullscreen = True
            self.pages.setCurrentWidget(self.mirror_page)
            self.stop_hilog()
            for shortcut in self.log_font_shortcuts:
                shortcut.setEnabled(False)
            self.sidebar.hide()
            self.header.hide()
            self.mirror_toolbar.hide()
            self.statusBar().hide()
        elif self.presentation_fullscreen:
            self.presentation_fullscreen = False
            self.sidebar.show()
            self.header.show()
            self.mirror_toolbar.show()
            self.statusBar().show()
            self.switch_module(1)


    def update_mode(self):
        smooth = self.mode.currentIndex() == 1
        self.fps.setEnabled(smooth and not self.projecting and not self.deploying)
        self.bitrate.setEnabled(smooth and not self.projecting and not self.deploying)
        self.settings.setValue("mode", self.mode.currentIndex())

    def on_quality_changed(self, index: int):
        bitrate_idx = {0: 1, 1: 0, 2: 2}.get(index, 1)
        self.bitrate.setCurrentIndex(bitrate_idx)
        self.settings.setValue("bitrate", bitrate_idx)
        self.quality.setCurrentIndex(index)
        self.settings.setValue("quality", index)
        if self.projecting:
            self.logger.info("切换投屏画质为 %s，正在重新连接投屏", self.quality_combo.currentText())
            self.stop_mirror()
            QTimer.singleShot(250, self.start_mirror)

    def load_hdc(self):
        try:
            self.hdc = HDC(find_hdc(str(self.settings.value("hdc", ""))))
            self.logger.info("HDC：%s", self.hdc.executable)
        except HDCError as error:
            self.hdc = None
            self.logger.error("%s", error)
            self.statusBar().showMessage(str(error))
        self.update_actions()

    def selected_device(self) -> Device | None:
        return self.device_list.currentData()

    def submit(self, work, complete, title: str, alert: bool = True):
        def task():
            try:
                result = work()
            except Exception as error:
                self.logger.error("%s：%s", title, error)
                if not self.closing:
                    self.signals.completed.emit(lambda value: self.task_failed(title, value, alert), str(error))
                return
            if not self.closing:
                self.signals.completed.emit(complete, result)
        self.start_worker(task, "harmonycast-task")

    def start_worker(self, work, name):
        def task():
            try:
                work()
            finally:
                with self.worker_lock:
                    self.workers.discard(threading.current_thread())
        worker = threading.Thread(target=task, name=name, daemon=False)
        with self.worker_lock:
            self.workers.add(worker)
        worker.start()
        return worker

    def task_failed(self, title, message, alert):
        self.statusBar().showMessage(f"{title}：{message}")
        if title == "扫描设备":
            self.scan_busy = False
            self.refresh_button.setEnabled(True)
        if title == "部署服务":
            self.deploying = False
            self.update_actions()
        if title == "获取设备信息":
            self.info_loading = False
            if hasattr(self, "btn_refresh_info"):
                self.btn_refresh_info.setEnabled(True)
            if hasattr(self, "info_badge"):
                self.info_badge.setText("获取失败")
        if alert:
            QMessageBox.warning(self, title, message)

    def refresh_devices(self):
        if self.hdc is None or self.scan_busy or self.projecting or self.deploying:
            return
        self.scan_busy = True
        self.update_actions()
        self.statusBar().showMessage("正在扫描设备…")
        hdc = self.hdc
        def scan():
            result = []
            for serial in hdc.devices():
                try:
                    result.append(hdc.info(serial))
                except HDCError as error:
                    self.logger.warning("设备 %s 暂不可用：%s", serial, error)
            return result
        self.submit(scan, self.populate_devices, "扫描设备", alert=False)

    def populate_devices(self, devices):
        old = self.selected_device()
        self.devices = devices
        self.device_list.blockSignals(True)
        self.device_list.clear()
        for device in devices:
            connection = "网络" if ":" in device.serial else "USB"
            self.device_list.addItem(f"{device.model} · {connection} · {device.serial[-6:]}", device)
            self.device_list.setItemData(self.device_list.count() - 1,
                                         f"{device.serial}\n{device.architecture}\n{device.version}", Qt.ToolTipRole)
            if old and old.serial == device.serial:
                self.device_list.setCurrentIndex(self.device_list.count() - 1)
        self.device_list.blockSignals(False)
        selected = self.selected_device()
        if old is None or selected is None or old.serial != selected.serial:
            self.device_changed(self.device_list.currentIndex())
        if self.hilog and self.hilog.serial not in {device.serial for device in devices}:
            self.stop_hilog()
        self.scan_busy = False
        self.refresh_button.setEnabled(True)
        self.device_note.setText(f"发现 {len(devices)} 台设备" if devices else "未发现设备。请打开 USB 调试，\n并在设备上授权此电脑。")
        self.statusBar().showMessage(f"已连接 {len(devices)} 台设备")
        self.logger.info("扫描完成，发现 %d 台设备", len(devices))
        self.update_actions()
        self.sync_hilog()
        if devices and self.auto_start_mirror and not self.projecting and not getattr(self, "_user_stopped_mirror", False):
            QTimer.singleShot(250, self._auto_start_mirror_if_needed)

    def _auto_start_mirror_if_needed(self):
        if self.closing or self.projecting or getattr(self, "_user_stopped_mirror", False):
            return
        device = self.selected_device()
        if device and self.hdc:
            self.logger.info("默认自动开启投屏：%s", device.serial)
            self.start_mirror()

    def device_changed(self, current, previous=None):
        device = self.selected_device()
        if self.hilog and (device is None or self.hilog.serial != device.serial):
            self.stop_hilog()
        self.screen.set_image(QImage())
        self.package_token = ""
        self.packages_device = ""
        self.packages_loading = False
        self.packages_last_scan = 0.0
        self.hilog_retry_at = 0.0
        self._selected_bundle = str(self.settings.value(f"logs/package/{device.serial}", DEFAULT_PACKAGE_FILTER)) if device else DEFAULT_PACKAGE_FILTER
        try:
            if self._selected_bundle:
                validate_package_filter(self._selected_bundle)
        except ValueError:
            self._selected_bundle = DEFAULT_PACKAGE_FILTER
        self.package_combo.blockSignals(True)
        self.package_combo.clear()
        self.package_combo.addItems([DEFAULT_PACKAGE_FILTER, "全部应用"])
        if self._selected_bundle:
            if self.package_combo.findText(self._selected_bundle) < 0:
                self.package_combo.addItem(self._selected_bundle)
            self.package_combo.setCurrentText(self._selected_bundle)
        else:
            self.package_combo.setCurrentText("全部应用")
        self.package_combo.blockSignals(False)
        self.package_hint.setText("应用列表自动更新")
        self.subtitle.setText(f"{device.model}  ·  {device.serial}" if device else "选择设备，开始你的调试工作")
        self.update_actions()
        self.refresh_hdc_target_label()
        if device and self.pages.currentWidget() is self.log_panel and self.category.currentData() == "app":
            self.refresh_packages()
        elif device and self.pages.currentWidget() is getattr(self, "info_panel", None):
            self.refresh_device_info()
        elif not device and hasattr(self, "info_panel") and self.pages.currentWidget() is self.info_panel:
            self._render_empty_info("请连接并选择设备以查看详情")
        self.sync_hilog()

    def update_actions(self):
        connected = self.hdc is not None and self.selected_device() is not None
        busy = self.projecting or self.deploying
        self.start_button.setEnabled(connected and not busy and not self.scan_busy)
        self.stop_button.setEnabled(self.projecting)
        self.start_button.setVisible(not self.projecting)
        self.stop_button.setVisible(self.projecting)
        self.screenshot_button.setEnabled(connected)
        if hasattr(self, "fault_report_button"):
            self.fault_report_button.setEnabled(connected)
        self.deploy_button.setEnabled(connected and not busy and not self.scan_busy)
        self.package_combo.setEnabled(connected)
        self.fetch_button.setEnabled(connected)
        for item in self.key_buttons:
            item.setEnabled(connected and self.projecting)
        self.rotate_button.setEnabled(True)
        self.device_list.setEnabled(not busy and not self.scan_busy)
        self.mode.setEnabled(not busy)
        self.refresh_button.setEnabled(not busy and not self.scan_busy)
        self.network_button.setEnabled(self.hdc is not None and not busy)
        self.update_mode()

    def set_projecting(self, active):
        self.projecting = active
        self.screen.active = active
        self.update_actions()

    def publish_frame(self, token: str, image: QImage, device_size=None, from_video: bool = False):
        if token != self.active_token or self.closing:
            return
        if from_video:
            self._last_video_frame_at = time.monotonic()
        try:
            self.frames.put_nowait((token, image, device_size))
        except queue.Full:
            try:
                self.frames.get_nowait()
            except queue.Empty:
                pass
            try:
                self.frames.put_nowait((token, image, device_size))
            except queue.Full:
                pass

    def start_mirror(self):
        device = self.selected_device()
        if not device or not self.hdc or self.projecting or self.deploying:
            return
        self._user_stopped_mirror = False
        self._mirror_suspended = False
        self.active_token = str(time.monotonic_ns())
        token = self.active_token
        self.set_projecting(True)
        self.state_badge.setText("正在连接…")
        self.fps_start, self.fps_frames = time.monotonic(), 0
        self.first_frame_displayed = False
        self.frame_timer.start(16)
        self.screen.user_rotation = self.user_rotation
        max_size = 0
        if self.mode.currentIndex() == 0 and NativeSession.available(device):
            self.stream = NativeSession(self.hdc, device, [15, 30, 60][self.fps.currentIndex()],
                                        [1_500_000, 4_000_000, 8_000_000][self.bitrate.currentIndex()],
                                        max_size=max_size)
            session = self.stream
            def native_video():
                error_message = ""
                try:
                    def on_frame(*frame):
                        hw_rot = getattr(session, "rotation", 0)
                        if len(frame) == 1:
                            rgb = frame[0]
                            image = QImage(rgb.data, rgb.shape[1], rgb.shape[0], rgb.strides[0], QImage.Format_RGB888)
                            self.publish_frame(token, orient_native_image(image, session.device_size, hw_rot, self.user_rotation), session.device_size, from_video=True)
                        else:
                            width, height, stride, source_width, source_height, pixels = frame
                            image = QImage(pixels, width, height, stride, QImage.Format_RGBA8888)
                            dev_size = session.device_size or (source_width, source_height)
                            self.publish_frame(token, orient_native_image(image, dev_size, hw_rot, self.user_rotation), dev_size, from_video=True)
                    session.run(on_frame, lambda state: self.signals.stream_status.emit(token, state))
                except Exception as error:
                    if not session.stop_event.is_set():
                        error_message = str(error)
                if not self.closing:
                    self.signals.stream_finished.emit(token, error_message)
            self.start_worker(native_video, "harmonycast-native")
        elif self.mode.currentIndex() in {0, 2}:
            self.preview_stop = threading.Event()
            stopped = self.preview_stop
            tools = DeviceTools(self.hdc)
            def preview():
                error_message = ""
                try:
                    while not stopped.is_set():
                        raw = tools.capture(device.serial)
                        image = QImage.fromData(raw)
                        if image.isNull():
                            raise HDCError("设备返回的截图无法显示")
                        if stopped.is_set():
                            break
                        if self.user_rotation:
                            image = image.transformed(QTransform().rotate(self.user_rotation))
                        self.publish_frame(token, image)
                        self.signals.stream_status.emit(token, "兼容预览 · 截图刷新")
                        stopped.wait(0.05)
                except Exception as error:
                    if not stopped.is_set():
                        error_message = str(error)
                if not self.closing:
                    self.signals.stream_finished.emit(token, error_message)
            self.start_worker(preview, "harmonycast-preview")
        else:
            self.stream = self.new_stream(device)
            session = self.stream
            def video():
                error_message = ""
                try:
                    if not session.installed():
                        raise HDCError("设备尚未安装投屏服务，请先点击「部署投屏服务」，或切换到兼容预览")
                    def on_video_frame(rgb):
                        hw_rot = getattr(session, "rotation", 0)
                        image = QImage(rgb.data, rgb.shape[1], rgb.shape[0], rgb.strides[0], QImage.Format_RGB888).copy()
                        self.publish_frame(token, orient_native_image(image, session.device_size, hw_rot, self.user_rotation), session.device_size, from_video=True)
                    session.run(on_video_frame, lambda state: self.signals.stream_status.emit(token, state))
                except Exception as error:
                    if not session.stop_event.is_set():
                        error_message = str(error)
                if not self.closing:
                    self.signals.stream_finished.emit(token, error_message)
            self.start_worker(video, "harmonycast-video")
        self.logger.info("开始投屏：%s，%s", device.serial, self.mode.currentText())

    def new_stream(self, device):
        return StreamSession(self.hdc, device, [15, 30, 60][self.fps.currentIndex()],
                             [1_500_000, 4_000_000, 8_000_000][self.bitrate.currentIndex()],
                             str(self.settings.value("server", "")),
                             max_size=0)

    def stop_mirror(self, *args, user_initiated: bool = True):
        if user_initiated:
            self._user_stopped_mirror = True
            self._mirror_suspended = False
        self._reconnect_attempts = 0
        self.active_token = ""
        while not self._gesture_queue.empty():
            try:
                self._gesture_queue.get_nowait()
                self._gesture_queue.task_done()
            except queue.Empty:
                break
        if self.preview_stop:
            self.preview_stop.set()
            self.preview_stop = None
        if self.stream:
            self.stream.stop()
            self.stream = None
        self.set_projecting(False)
        self.frame_timer.stop()
        self.state_badge.setText("已休眠" if not user_initiated else "已停止")
        self.logger.info("投屏已暂停休眠 (切换标签页)" if not user_initiated else "投屏已停止")

    def on_stream_status(self, token, state):
        if token == self.active_token:
            self.state_badge.setText("投屏中" if "投屏" in state or "兼容预览" in state else "正在连接…")
            self.statusBar().showMessage(state)

    def on_stream_finished(self, token, error_message):
        if token != self.active_token:
            return
        if error_message and not self._user_stopped_mirror and not self.closing:
            if self._reconnect_attempts < 5:
                self._reconnect_attempts += 1
                self.logger.warning("投屏连接中断：%s，正在进行第 %d/5 次自动重连...", error_message, self._reconnect_attempts)
                self.state_badge.setText(f"重连中 ({self._reconnect_attempts}/5)…")
                self.statusBar().showMessage(f"投屏中断，正在自动重连 ({self._reconnect_attempts}/5)…")
                if self.stream:
                    self.stream.stop()
                    self.stream = None
                if self.preview_stop:
                    self.preview_stop.set()
                    self.preview_stop = None
                self.set_projecting(False)
                QTimer.singleShot(800, self._auto_reconnect)
                return

        self.stop_mirror()
        if error_message:
            self.state_badge.setText("连接失败")
            self.logger.error("投屏：%s", error_message)
            self.statusBar().showMessage(error_message)
            QMessageBox.warning(self, "投屏未能完成", f"投屏已中断，多次自动重连失败：\n{error_message}" if self._reconnect_attempts else error_message)

    def _auto_reconnect(self):
        if self._user_stopped_mirror or self.closing:
            return
        device = self.selected_device()
        if not device or not self.hdc:
            if self._reconnect_attempts < 5:
                self.logger.info("设备暂未就绪，刷新设备列表并等待重连...")
                self.refresh_devices()
                QTimer.singleShot(1500, self._auto_reconnect)
                return
            self.stop_mirror()
            self.state_badge.setText("未连接设备")
            return
        self.logger.info("执行自动重连投屏服务 (尝试 %d/5)...", self._reconnect_attempts)
        saved_attempts = self._reconnect_attempts
        self.start_mirror()
        self._reconnect_attempts = saved_attempts

    def paint_frame(self):
        try:
            token, image, device_size = self.frames.get_nowait()
        except queue.Empty:
            return
        if token != self.active_token:
            return
        self.screen.set_image(image, device_size)
        if not self.first_frame_displayed:
            self.first_frame_displayed = True
            self._reconnect_attempts = 0
            self.fps_start = time.monotonic()
            self.frame_info.setText(f"{image.width()} × {image.height()}")
        self.fps_frames += 1
        elapsed = time.monotonic() - self.fps_start
        if elapsed >= 1:
            measured_fps = self.fps_frames / elapsed
            suffix = f"{measured_fps:.1f} FPS" if measured_fps >= 1 else "画面已同步"
            self.frame_info.setText(f"{image.width()} × {image.height()}  ·  {suffix}")
            self.fps_start, self.fps_frames = time.monotonic(), 0

    def deploy_server(self):
        device = self.selected_device()
        if not device or not self.hdc:
            return
        session = self.new_stream(device)
        try:
            selected = session.selected_server()
        except ValueError as error:
            QMessageBox.information(self, "设备兼容性", str(error))
            return
        reply = QMessageBox.question(self, "部署投屏服务", (
            f"将在 {device.model} 的系统目录安装投屏服务。\n\n"
            "需要设备具备 root 权限和可写系统分区。\n"
            f"服务版本：{selected.parent.name} / {selected.name}\n\n"
            "完成后即可使用流畅投屏。继续部署？"
        ), QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
        if reply != QMessageBox.Yes:
            return
        self.deploying = True
        self.update_actions()
        def done(result):
            self.deploying = False
            self.update_actions()
            self.mode.setCurrentIndex(1)
            self.statusBar().showMessage("投屏服务已部署，可点击开始投屏")
        self.submit(session.deploy, done, "部署服务")

    def rotate_display(self):
        self.user_rotation = (self.user_rotation + 90) % 360
        self.settings.setValue("rotation", self.user_rotation)
        self.screen.user_rotation = self.user_rotation
        self.statusBar().showMessage(f"画面旋转：{self.user_rotation}°", 1500)
        self.screen.update()

    def _run_gesture_worker(self):
        while not self.closing:
            try:
                item = self._gesture_queue.get(timeout=0.2)
            except queue.Empty:
                continue
            if item is None or self.closing:
                break
            serial, start, end, duration, is_click, desc, token = item
            if not self.projecting or self.active_token != token:
                self._gesture_queue.task_done()
                continue
            try:
                tools = DeviceTools(self.hdc)
                tools.gesture(serial, start, end, duration, click_interval=10 if is_click else None)
                if not self.closing:
                    self.signals.completed.emit(lambda _: self.statusBar().showMessage(f"已完成{desc}", 1200), None)
            except Exception as error:
                self.logger.warning("触控操作失败: %s", error)
            finally:
                self._gesture_queue.task_done()

    def send_key(self, action):
        device = self.stream.device if (self.stream and hasattr(self.stream, "device")) else self.selected_device()
        if device and self.hdc:
            tools = DeviceTools(self.hdc)
            def execute():
                tools.key(device.serial, action)
            self.submit(execute, lambda result: None, "设备按键", alert=False)

    def send_gesture(self, start, end, duration):
        device = self.stream.device if (self.stream and hasattr(self.stream, "device")) else self.selected_device()
        if not device or not self.hdc:
            self.logger.warning("触控操作未执行：未连接设备或 HDC 未就绪")
            return
        is_click = (start == end)
        if is_click:
            desc = f"点击 ({start[0]}, {start[1]})"
        else:
            desc = f"滑动 ({start[0]}, {start[1]}) -> ({end[0]}, {end[1]}) [{duration}ms]"
        self.logger.info("触控事件：%s (设备: %s)", desc, device.serial)
        self.statusBar().showMessage(f"正在{desc}…", 1000)

        if is_click and self._gesture_queue.qsize() >= 3:
            try:
                self._gesture_queue.get_nowait()
                self._gesture_queue.task_done()
            except queue.Empty:
                pass

        try:
            self._gesture_queue.put_nowait((device.serial, start, end, duration, is_click, desc, self.active_token))
        except queue.Full:
            self.logger.warning("触控队列已满，丢弃操作: %s", desc)

    def save_screenshot(self):
        device = self.selected_device()
        if not device or not self.hdc:
            return
        filename, _ = QFileDialog.getSaveFileName(self, "保存设备截图", str(Path.home() / "Downloads" / f"screen_{datetime.now():%Y%m%d_%H%M%S}.png"), "PNG 图像 (*.png)")
        if not filename:
            return
        destination = Path(filename)
        image = self.screen.image.copy()
        def save():
            captured = image if not image.isNull() else QImage.fromData(DeviceTools(self.hdc).capture(device.serial))
            if captured.isNull() or not captured.save(str(destination), "PNG"):
                raise HDCError("截图保存失败，请检查保存目录")
            return destination
        self.submit(save, lambda result: self.logger.info("截图已保存：%s", result), "保存截图")

    def export_fault_report(self):
        device = self.selected_device()
        if not device or not self.hdc:
            QMessageBox.warning(self, "未连接设备", "请先连接并选择 OpenHarmony 设备后再导出故障现场。")
            return

        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        serial_short = device.serial[-6:] if len(device.serial) >= 6 else device.serial
        default_filename = f"HarmonyCast_Fault_{serial_short}_{ts}.zip"
        default_path = str(Path.home() / "Downloads" / default_filename)

        filename, _ = QFileDialog.getSaveFileName(
            self,
            "导出故障现场诊断包",
            default_path,
            "ZIP 压缩包 (*.zip);;所有文件 (*.*)",
        )
        if not filename:
            return

        destination = Path(filename)
        self.statusBar().showMessage("正在打包故障现场（截图、HiLog、进程快照、硬件参数）…", 10000)

        image_bytes = None
        if not self.screen.image.isNull():
            ba = QByteArray()
            buf = QBuffer(ba)
            buf.open(QIODevice.WriteOnly)
            self.screen.image.save(buf, "PNG")
            image_bytes = bytes(ba)

        log_lines = [entry.text() for entry in self.store.entries]

        def do_export():
            tools = DeviceTools(self.hdc)
            return tools.export_fault_bundle(
                serial=device.serial,
                destination=destination,
                screenshot_bytes=image_bytes,
                log_entries=log_lines,
            )

        def on_complete(result_path: Path):
            self.statusBar().showMessage(f"故障现场已导出：{result_path.name}", 5000)
            self.logger.info("故障现场诊断包导出成功：%s", result_path)
            self._show_fault_export_dialog(result_path)

        self.submit(do_export, on_complete, "导出故障现场")

    def _show_fault_export_dialog(self, result_path: Path):
        if getattr(self, "_suppress_dialogs", False):
            return
        size_bytes = result_path.stat().st_size if result_path.exists() else 0
        size_mb = size_bytes / (1024 * 1024)
        size_str = f"{size_mb:.2f} MB" if size_mb >= 0.1 else f"{size_bytes / 1024:.1f} KB"

        msg_box = QMessageBox(self)
        msg_box.setWindowTitle("故障现场打包成功")
        msg_box.setText(f"故障现场诊断包已生成：\n{result_path.name}")
        msg_box.setInformativeText(
            f"保存路径：{result_path}\n"
            f"文件大小：{size_str}\n\n"
            "诊断包已包含：\n"
            "• 现场高清截图 (screenshot.png)\n"
            "• 实时 HiLog 日志及设备缓冲 (hilog_*.log)\n"
            "• 详细设备硬件与网络规格 (device_info.json)\n"
            "• 运行中的全部进程快照 (running_processes.txt)\n"
            "• 系统参数 dump (system_parameters.txt)\n"
            "• 亚华医疗 ICU 与看门狗诊断报告 (REPORT_SUMMARY.md)\n"
            "• 业务应用持久化落盘日志 (app_disk_logs/)"
        )
        msg_box.setIcon(QMessageBox.Information)
        open_btn = msg_box.addButton("打开所在文件夹", QMessageBox.ActionRole)
        ok_btn = msg_box.addButton("确定", QMessageBox.AcceptRole)
        msg_box.setDefaultButton(ok_btn)
        msg_box.exec()
        if msg_box.clickedButton() == open_btn:
            open_containing_folder(result_path)

    def toggle_touch_indicator(self, checked: bool):
        self.screen.show_touches = checked
        self.settings.setValue("show_touches", 1 if checked else 0)
        self.logger.info("已%s触摸小白点显示", "开启" if checked else "关闭")

    def maintain_logs(self):
        self.sync_hilog()
        if self.pages.currentWidget() is self.log_panel:
            if self.hdc and self.selected_device() is None:
                self.refresh_devices()
            self.refresh_packages()

    def set_log_font_size(self, size):
        self.log_view.setStyleSheet(f"QTextEdit {{ font-family: 'Menlo'; font-size: {size}pt; }}")
        self.settings.setValue("logs/font_size", size)

    def sync_hilog(self):
        if self.closing:
            return
        device = self.selected_device()
        if self.pages.currentWidget() is not self.log_panel or not device or not self.hdc:
            self.stop_hilog()
            self.log_state.setText("连接设备后自动采集" if not device or not self.hdc else "日志页自动采集")
            return
        bundle = self.selected_package()
        if self.hilog and (self.hilog.serial, self.hilog.category, self.hilog.bundle) == (device.serial, self.category.currentData(), bundle):
            return
        if time.monotonic() < self.hilog_retry_at:
            return
        self.stop_hilog()
        self.hilog_token = str(time.monotonic_ns())
        token = self.hilog_token
        def receive(line, package=None):
            if token != self.hilog_token:
                return
            match = re.search(r"\s([DIWEF])\s", line)
            level = {"D": "DEBUG", "I": "INFO", "W": "WARN", "E": "ERROR", "F": "FATAL"}.get(match.group(1) if match else "", "INFO")
            if line.startswith("[Fail]"):
                level = "ERROR"
            self.file_handler.handle(logging.LogRecord("HarmonyCast", getattr(logging, {"WARN": "WARNING", "FATAL": "CRITICAL"}.get(level, level)), "", 0, "[HiLog %s] %s", (device.serial, line), None))
            try:
                self.messages.put_nowait((level, "HiLog", line, package if package is not None else bundle))
            except queue.Full:
                pass
        options = {"category": self.category.currentData(),
                   "on_status": lambda status: self.signals.hilog_status.emit(token, status)}
        if bundle:
            options.update(bundle=bundle, on_package_line=receive)
        self.hilog = HiLogSession(self.hdc, device.serial, receive,
                                 lambda code: self.signals.hilog_finished.emit(token, code), **options)
        self.log_state.setText("正在连接日志…" if bundle else "自动采集中")
        self.hilog.start()
        self.logger.info("HiLog 采集已开始：%s，%s%s", device.serial, self.category.currentText(), f"，{bundle}" if bundle else "")

    def log_category_changed(self):
        self.settings.setValue("logs/category", self.category.currentData())
        application_logs = self.category.currentData() == "app"
        self.package_row.setVisible(application_logs)
        device = self.selected_device()
        if application_logs and device and self.pages.currentWidget() is self.log_panel:
            self.refresh_packages()
        self.hilog_retry_at = 0.0
        self.sync_hilog()
        self.render_logs()

    def selected_package(self):
        return self._selected_bundle if self.category.currentData() == "app" else ""

    def commit_package(self, index=None):
        if self.category.currentData() != "app":
            return True
        text = self.package_combo.currentText().strip()
        bundle = "" if text in {"", "全部应用"} else text
        try:
            if bundle:
                validate_package_filter(bundle)
        except ValueError as error:
            self.package_hint.setText(str(error))
            return False
        if bundle == self._selected_bundle:
            return True
        self._selected_bundle = bundle
        device = self.selected_device()
        if device:
            self.settings.setValue(f"logs/package/{device.serial}", bundle)
        self.hilog_retry_at = 0.0
        self.sync_hilog()
        self.render_logs()
        return True

    def refresh_packages(self):
        device = self.selected_device()
        if (not device or not self.hdc or self.packages_loading or self.category.currentData() != "app"
                or self.pages.currentWidget() is not self.log_panel
                or time.monotonic() - self.packages_last_scan < 10
                or self.package_combo.hasFocus() or self.package_combo.lineEdit().hasFocus()
                or self.package_combo.view().isVisible()):
            return
        token = self.package_token = str(time.monotonic_ns())
        hdc = self.hdc
        self.packages_loading = True
        self.packages_last_scan = time.monotonic()
        self.update_actions()
        def load():
            try:
                return hdc.applications(device.serial), ""
            except HDCError as error:
                return [], str(error)
        def complete(result):
            if token != self.package_token:
                return
            packages, error = result
            self.packages_loading = False
            self.packages_device = device.serial if not error else ""
            selected = self._selected_bundle
            editing = self.package_combo.hasFocus() or self.package_combo.lineEdit().hasFocus()
            typed = self.package_combo.currentText()
            self.package_combo.blockSignals(True)
            self.package_combo.clear()
            self.package_combo.addItems([DEFAULT_PACKAGE_FILTER, "全部应用"])
            self.package_combo.addItems(packages)
            if selected:
                if self.package_combo.findText(selected) < 0:
                    self.package_combo.addItem(selected)
                self.package_combo.setCurrentText(selected)
            else:
                self.package_combo.setCurrentText("全部应用")
            if editing:
                self.package_combo.setEditText(typed)
            self.package_combo.blockSignals(False)
            if not self.hilog or not selected:
                self.package_hint.setText("读取失败，将自动重试" if error else f"{len(packages)} 个应用 · 自动更新")
            self.package_hint.setToolTip(error)
            self.update_actions()
        self.submit(load, complete, "读取应用列表", alert=False)

    def on_hilog_status(self, token, status):
        if token == self.hilog_token:
            self.log_state.setText(status)
            self.log_state.setToolTip(status)
            self.package_hint.setText(status)
            self.package_hint.setToolTip(status)
            self.statusBar().showMessage(status)

    def stop_hilog(self):
        self.hilog_token = ""
        if self.hilog:
            session, self.hilog = self.hilog, None
            self.start_worker(session.stop, "harmonycast-hilog-stop")
            self.logger.info("HiLog 采集已结束")

    def on_hilog_finished(self, token, code):
        if token == self.hilog_token:
            self.stop_hilog()
            self.hilog_retry_at = time.monotonic() + 2
            self.log_state.setText("日志连接中断，自动重试…")
            self.logger.warning("HiLog 进程退出，退出码：%s", code)

    def fetch_logs(self):
        device = self.selected_device()
        if not device or not self.hdc:
            return
        folder = QFileDialog.getExistingDirectory(self, "选择日志保存目录", str(Path.home() / "Downloads"))
        if not folder:
            return
        tools = DeviceTools(self.hdc)
        destination = Path(folder) / f"HarmonyCast_{datetime.now():%Y%m%d_%H%M%S}_{re.sub('[^A-Za-z0-9_-]', '_', device.serial)}"
        def done(files):
            self.logger.info("服务端日志：已保存 %d 个文件到 %s", len(files), destination)
            self.statusBar().showMessage(f"已拉取 {len(files)} 个服务端日志" if files else "设备上没有 OHScrcpy 服务端日志")
        self.submit(lambda: tools.fetch_server_logs(device.serial, destination), done, "拉取日志")

    def filters(self):
        return (self.search.text(), self.level.currentText() if self.level.currentIndex() else "",
                self.source.currentText() if self.source.currentIndex() else "", self.category.currentData(), self.selected_package())

    def pump(self):
        new_entries = []
        for _ in range(1000):
            try:
                record = self.messages.get_nowait()
            except queue.Empty:
                break
            level, source, message = record[:3]
            bundle = record[3] if len(record) > 3 else ""
            if "\x1b" in message:
                message = re.sub(r"\x1b\[[0-9;]*m", "", message)
            new_entries.append(self.store.append(level, source, message, bundle=bundle))
        if new_entries and not self.pause.isChecked() and self.pages.currentWidget() is self.log_panel:
            filters = self.filters()
            selected = [entry.text() for entry in new_entries if entry.matches(*filters)]
            scroll = self.log_view.verticalScrollBar()
            position = scroll.value()
            if selected:
                self.log_view.appendPlainText("\n".join(selected))
            if self.autoscroll.isChecked():
                scroll.setValue(scroll.maximum())
            else:
                scroll.setValue(position)
            self.log_count.setText(f"缓存 {len(self.store.entries)} 条 · 文件自动保存")

    def render_logs(self):
        if self.pause.isChecked():
            return
        entries = self.store.filtered(*self.filters())
        scroll = self.log_view.verticalScrollBar()
        position = scroll.value()
        self.log_view.setPlainText("\n".join(entry.text() for entry in entries))
        scroll.setValue(scroll.maximum() if self.autoscroll.isChecked() else position)
        self.log_count.setText(f"显示 {len(entries)} 条 / 缓存 {len(self.store.entries)} 条")

    def clear_log_view(self):
        self.store.entries.clear()
        self.log_view.clear()
        self.log_count.setText("0 条日志 · 本地日志文件继续保存")

    def export_logs(self):
        filename, _ = QFileDialog.getSaveFileName(self, "导出当前筛选日志", str(Path.home() / "Downloads" / f"HarmonyCast_{datetime.now():%Y%m%d_%H%M%S}.log"), "日志文件 (*.log)")
        if filename:
            try:
                self.store.export(Path(filename), *self.filters())
                self.logger.info("筛选日志已导出：%s", filename)
            except OSError as error:
                QMessageBox.warning(self, "导出失败", str(error))

    def connect_network(self):
        dialog = QDialog(self)
        dialog.setWindowTitle("连接网络设备")
        layout = QVBoxLayout(dialog)
        layout.addWidget(label("设备和电脑需处于同一局域网，\n且设备已开启 HDC 网络调试。", "Muted"))
        address = QLineEdit()
        address.setPlaceholderText("192.168.1.20:5555")
        address.setMinimumWidth(320)
        layout.addWidget(address)
        connect = button("连接", dialog.accept, "Primary")
        layout.addWidget(connect)
        if dialog.exec_() == QDialog.Accepted:
            hdc = self.hdc
            endpoint = address.text()
            self.submit(lambda: hdc.connect(endpoint), lambda result: self.refresh_devices(), "网络连接")

    def open_settings(self):
        dialog = QDialog(self)
        dialog.setWindowTitle("HarmonyCast 设置")
        dialog.setMinimumWidth(600)
        layout = QVBoxLayout(dialog)
        form = QFormLayout()
        mode = QComboBox()
        mode.addItems(["自动 · 优先连续视频流", "视频投屏 · OHScrcpy 服务", "兼容截图 · 低帧率"])
        mode.setCurrentIndex(self.mode.currentIndex())
        frame_rate = QComboBox()
        frame_rate.addItems(["15 FPS", "30 FPS", "60 FPS"])
        frame_rate.setCurrentIndex(self.fps.currentIndex())
        bitrate = QComboBox()
        bitrate.addItems(["1.5 Mbps", "4 Mbps", "8 Mbps"])
        bitrate.setCurrentIndex(self.bitrate.currentIndex())
        quality = QComboBox()
        quality.addItems(["720P · 极速流畅（推荐）", "1080P · 原画高清"])
        quality.setCurrentIndex(self.quality.currentIndex())
        form.addRow("投屏方式", mode)
        form.addRow("画面清晰度", quality)
        form.addRow("目标帧率", frame_rate)
        form.addRow("视频码率", bitrate)
        auto_mirror_cb = QCheckBox("连接设备后默认自动开启投屏")
        auto_mirror_cb.setChecked(self.auto_start_mirror)
        form.addRow("自动投屏", auto_mirror_cb)
        hdc = QLineEdit(str(self.settings.value("hdc", "")))
        hdc.setPlaceholderText("自动检测 DevEco Studio，或使用内置 HDC")
        server = QLineEdit(str(self.settings.value("server", "")))
        server.setPlaceholderText("可选：匹配设备架构和系统版本的 ohscrcpy_server")
        for title, field in [("HDC 路径", hdc), ("自定义服务端", server)]:
            row = QHBoxLayout()
            row.addWidget(field)
            def browse(checked=False, target=field):
                filename, _ = QFileDialog.getOpenFileName(dialog, "选择工具文件")
                if filename:
                    target.setText(filename)
            row.addWidget(button("选择…", browse))
            form.addRow(title, row)
        layout.addLayout(form)
        layout.addWidget(label(f"当前 HDC：{self.hdc.executable if self.hdc else '未找到'}", "Muted", True))
        layout.addWidget(label(f"自动日志目录：{self.log_directory}", "Muted", True))
        layout.addWidget(button("打开日志目录", lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(str(self.log_directory)))))
        layout.addWidget(label("投屏与解码能力参考 OpenHarmony_Scrcpy v2.3。\n内置服务支持 ARM32 OpenHarmony 和部分 ARM64 HarmonyOS 开发设备。", "Muted", True))
        layout.addWidget(button("查看开源项目", lambda: QDesktopServices.openUrl(QUrl("https://gitcode.com/OpenHarmony_Tools/OpenHarmony_Scrcpy"))))
        save = button("保存设置", dialog.accept, "Primary")
        save.setEnabled(not self.projecting and not self.deploying and not self.scan_busy)
        layout.addWidget(save)
        if dialog.exec_() == QDialog.Accepted:
            try:
                find_hdc(hdc.text().strip())
                if server.text().strip() and not Path(server.text().strip()).expanduser().is_file():
                    raise ValueError("自定义服务端文件不存在")
            except (HDCError, ValueError) as error:
                QMessageBox.warning(self, "设置未保存", str(error))
                return
            self.stop_hilog()
            self.settings.setValue("hdc", hdc.text().strip())
            self.settings.setValue("server", server.text().strip())
            self.mode.setCurrentIndex(mode.currentIndex())
            self.quality.setCurrentIndex(quality.currentIndex())
            self.quality_combo.blockSignals(True)
            self.quality_combo.setCurrentIndex(quality.currentIndex())
            self.quality_combo.blockSignals(False)
            self.settings.setValue("quality", quality.currentIndex())
            self.fps.setCurrentIndex(frame_rate.currentIndex())
            self.bitrate.setCurrentIndex(bitrate.currentIndex())
            self.settings.setValue("fps", frame_rate.currentIndex())
            self.settings.setValue("bitrate", bitrate.currentIndex())
            self.auto_start_mirror = auto_mirror_cb.isChecked()
            self.settings.setValue("auto_start_mirror", self.auto_start_mirror)
            self.load_hdc()
            self.refresh_devices()

    def closeEvent(self, event):
        self.closing = True
        self.log_timer.stop()
        self.hilog_token = ""
        self.stop_mirror()
        try:
            self._gesture_queue.put_nowait(None)
        except Exception:
            pass
        if self.hilog:
            self.hilog.stop()
            self.hilog = None
        with self._hdc_proc_lock:
            if self._current_hdc_proc:
                try:
                    self._current_hdc_proc.kill()
                except Exception:
                    pass
                self._current_hdc_proc = None
        # Keep the application alive until owned services and forwards are cleaned.
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            with self.worker_lock:
                workers = list(self.workers)
            if not workers:
                break
            for worker in workers:
                worker.join(timeout=0.1)
        self.settings.sync()
        self.logger.removeHandler(self.queue_handler)
        self.logger.removeHandler(self.file_handler)
        self.file_handler.close()
        super().closeEvent(event)


def run() -> int:
    app = QApplication([])
    app.setApplicationName("HarmonyCast")
    app.setOrganizationName("HarmonyCast")
    app.setStyle("Fusion")
    app.setStyleSheet(STYLE)
    app.setWindowIcon(QIcon(str(resource_dir() / "app-icon.svg")))
    window = MainWindow()
    window.show()
    return app.exec_()
