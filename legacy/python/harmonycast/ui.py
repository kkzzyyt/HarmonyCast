from __future__ import annotations

from collections import deque
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

from PyQt5.QtCore import QBuffer, QByteArray, QEvent, QIODevice, QObject, QPoint, QPointF, QPropertyAnimation, QRect, QSettings, QSize, Qt, QTimer, QUrl, pyqtSignal
from PyQt5.QtGui import QColor, QDesktopServices, QFont, QIcon, QImage, QKeySequence, QPainter, QPalette, QPen, QPixmap, QSyntaxHighlighter, QTextBlockFormat, QTextCharFormat, QTextCursor, QTransform
from PyQt5.QtSvg import QSvgRenderer
from PyQt5.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QCompleter, QDialog, QFileDialog, QFormLayout,
    QFrame, QGraphicsOpacityEffect, QGridLayout, QHBoxLayout, QHeaderView, QInputDialog, QLabel, QLayout, QLineEdit, QMenu, QShortcut, QSizePolicy, QSpinBox, QSplitter, QStackedWidget,
    QMainWindow, QMessageBox, QPlainTextEdit, QPushButton, QScrollArea, QTableWidget, QTableWidgetItem, QTextEdit,
    QVBoxLayout, QWidget,
)

from . import __version__
from .backend import DeviceTools, HiLogSession, NativeSession, StreamSession, format_bytes
from .core import (
    DEFAULT_NETWORK_PORT, Device, HDC, HDCError, LogEntry, LogStore, data_dir,
    delete_saved_device, find_hdc, load_saved_devices, make_hilog_entry, map_coordinates,
    resource_dir, save_device, validate_package_filter,
)
from .webview import DEFAULT_DEVTOOLS_PORT, WebViewSession, generate_curl_command


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
QLabel#Badge { color: #5ce0c1; background: #12352f; padding: 4px 10px; border-radius: 9px; font-size: 12px; }
QPushButton { background: #1d2938; border: 1px solid #304257; border-radius: 5px; padding: 4px 8px; font-size: 12px; }
QPushButton:hover { background: #293b4e; border-color: #5f829a; }
QPushButton:pressed { background: #33495e; }
QPushButton:disabled { color: #526375; border-color: #233141; background: #16212d; }
QPushButton#Primary { background: #53d6b5; border: 1px solid #53d6b5; color: #09231e; font-weight: 600; }
QPushButton#Primary:hover { background: #78ebcb; }
QPushButton#Primary:disabled { background: #24463f; border-color: #24463f; color: #66847c; }
QPushButton#Danger { color: #ff9f9f; border-color: #4a252f; background: #26161d; }
QPushButton#Danger:hover { background: #381a24; border-color: #7a3544; }
QPushButton#Danger:disabled { color: #526375; }
QPushButton#Navigation { text-align: left; background: transparent; border: none; color: #9caec0; font-size: 14px; padding-left: 10px; }
QPushButton#Navigation:checked { background: #1b3e35; color: #79e6c9; font-weight: 600; }
QPushButton#Navigation:hover { background: #1d2938; }
QPushButton#IconBtn { background: #1a2533; border: 1px solid #2d3e52; border-radius: 5px; padding: 0px; }
QPushButton#IconBtn:hover { background: #283a4e; border-color: #5f829a; }
QPushButton#IconBtnPrimary { background: #133830; border: 1px solid #1f564b; border-radius: 5px; padding: 0px; }
QPushButton#IconBtnPrimary:hover { background: #1c5246; border-color: #53d6b5; }
QPushButton#IconBtnDanger { background: #301b22; border: 1px solid #572932; border-radius: 5px; padding: 0px; }
QPushButton#IconBtnDanger:hover { background: #4a212b; border-color: #ff8080; }
QPushButton#IconBtnMuted { background: #16202c; border: 1px solid #263548; border-radius: 5px; padding: 0px; }
QPushButton#IconBtnMuted:hover { background: #263548; border-color: #4a637f; }
QPushButton#IconBtnSettings { background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 0px; }
QPushButton#IconBtnSettings:hover { background: #1d2938; border-color: #304257; }
QMenu { background-color: #141f2c; border: 1px solid #304257; border-radius: 6px; padding: 4px; color: #e7edf4; }
QMenu::item { padding: 6px 18px 6px 10px; border-radius: 4px; color: #cdd9e5; font-size: 12px; }
QMenu::item:selected { background-color: #1b3e35; color: #5ce0c1; }
QMenu::separator { height: 1px; background: #25354a; margin: 4px 6px; }
QListWidget { background: transparent; border: none; outline: none; }
QListWidget::item { background: #141f2c; margin-bottom: 8px; padding: 14px 10px; border: 1px solid #25354a; border-radius: 8px; }
QListWidget::item:selected { background: #15372f; border-color: #53d6b5; color: #e9fff8; }
QListView, QAbstractItemView { background-color: #141f2c; color: #e7edf4; border: 1px solid #304257; border-radius: 6px; selection-background-color: #1b3e35; outline: none; }
QLineEdit, QComboBox, QSpinBox { background-color: #0c131c; border: 1px solid #304257; border-radius: 5px; padding: 3px 6px; min-height: 18px; font-size: 12px; color: #e7edf4; }
QLineEdit:focus, QComboBox:focus { border-color: #53d6b5; }
QComboBox { padding-right: 22px; padding-left: 6px; }
QComboBox:editable { background-color: #0c131c; color: #e7edf4; }
QComboBox QLineEdit { background: transparent; border: none; border-radius: 0; padding: 0; min-height: 0; color: #e7edf4; selection-background-color: #1b3e35; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width: 20px; border: none; border-left: 1px solid #304257; background: #17222f; border-top-right-radius: 4px; border-bottom-right-radius: 4px; }
QComboBox::drop-down:hover { background: #233345; }
QComboBox::down-arrow { image: url("__CHEVRON_DOWN__"); width: 10px; height: 10px; }
QComboBox QAbstractItemView { background-color: #141f2c; color: #e7edf4; border: 1px solid #304257; border-radius: 6px; selection-background-color: #1b3e35; outline: none; padding: 2px; }
QComboBox QAbstractItemView::item { min-height: 24px; padding: 4px 8px; border-radius: 4px; color: #cdd9e5; }
QComboBox QAbstractItemView::item:selected { background-color: #1b3e35; color: #5ce0c1; font-weight: 600; }
QComboBox QAbstractItemView::item:hover { background-color: #1c2b3d; color: #ffffff; }
QComboBoxPrivateContainer { background-color: #141f2c; border: 1px solid #304257; border-radius: 6px; color: #e7edf4; }
QComboBoxPrivateScroller { background-color: #141f2c; color: #8295a9; height: 16px; }
QFrame#MirrorToolbar { background: #0c131c; border-bottom: 1px solid #1c2b3d; }
QTextEdit { background: #0a1119; color: #c2d1df; border: 1px solid #25354a; border-radius: 7px; padding: 8px; selection-background-color: #245546; }
QCheckBox { background: transparent; spacing: 5px; font-size: 12px; }
QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #4b6176; border-radius: 3px; background: #0c131c; }
QCheckBox::indicator:hover { border-color: #53d6b5; }
QCheckBox::indicator:checked { background: #53d6b5; border-color: #53d6b5; image: url("__CHECK__"); }
QCheckBox::indicator:checked:hover { background: #78ebcb; border-color: #78ebcb; }
QSplitter::handle:horizontal { background: #141c26; border-left: 1px solid #1e2c3d; border-right: 1px solid #1e2c3d; }
QSplitter::handle:horizontal:hover { background: #388bfd; }
QSplitter::handle:vertical { background: #141c26; border-top: 1px solid #1e2c3d; border-bottom: 1px solid #1e2c3d; }
QSplitter::handle:vertical:hover { background: #388bfd; }
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
QPushButton#LogSubTab { background: #111a24; color: #8da2b5; border: 1px solid #1e2c3d; border-radius: 5px; padding: 3px 10px; font-size: 12px; }
QPushButton#LogSubTab:hover { background: #192736; color: #e2edf7; border-color: #354a61; }
QPushButton#LogSubTab:checked { background: #162a3f; color: #5ce0c1; border-color: #3b7e74; font-weight: bold; }
QPushButton#LayoutSwitchBtn { background: transparent; border: none; border-radius: 4px; padding: 2px 6px; color: #8da2b5; font-size: 11px; }
QPushButton#LayoutSwitchBtn:hover { background: #192736; color: #e2edf7; }
QPushButton#LayoutSwitchBtn:checked { background: #162a3f; color: #5ce0c1; font-weight: bold; }
""".replace("__CHEVRON_DOWN__", str(resource_dir() / "chevron-down.svg")).replace("__CHECK__", str(resource_dir() / "check.svg"))


def label(text: str, name: str = "", wrap: bool = False) -> QLabel:
    item = QLabel(text)
    item.setObjectName(name)
    item.setWordWrap(wrap)
    return item


def make_svg_icon(svg_inner: str, stroke: str = "#8295a9", fill: str = "none", stroke_width: float = 2.0, size: int = 18) -> QIcon:
    xml = (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="{size}" height="{size}" '
        f'fill="{fill}" stroke="{stroke}" stroke-width="{stroke_width}" stroke-linecap="round" stroke-linejoin="round">'
        f'{svg_inner}'
        f'</svg>'
    )
    renderer = QSvgRenderer(QByteArray(xml.encode("utf-8")))
    pix = QPixmap(size, size)
    pix.fill(QColor(0, 0, 0, 0))
    painter = QPainter(pix)
    renderer.render(painter)
    painter.end()
    return QIcon(pix)


_CACHED_ICONS: dict[str, QIcon] = {}


def get_ui_icon(name: str) -> QIcon:
    if name in _CACHED_ICONS:
        return _CACHED_ICONS[name]

    icon = QIcon()
    if name == "connect":
        icon = make_svg_icon('<polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2" fill="#5ce0c1" stroke="#5ce0c1"/>', size=16)
    elif name in ("connect_dark", "connect_primary"):
        icon = make_svg_icon('<polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2" fill="#09231e" stroke="#09231e"/>', size=16)
    elif name == "disconnect":
        icon = make_svg_icon('<path d="M18.36 6.64a9 9 0 1 1-12.73 0M12 2v10" stroke="#ff9f9f" stroke-width="2.2"/>', size=16)
    elif name == "delete":
        icon = make_svg_icon('<polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/><line x1="10" y1="11" x2="10" y2="17"/><line x1="14" y1="11" x2="14" y2="17"/>', stroke="#ff8080", size=16)
    elif name in ("mirror", "workspace"):
        icon = make_svg_icon('<rect x="2" y="3" width="20" height="14" rx="2" ry="2" stroke="#5ce0c1"/><line x1="8" y1="21" x2="16" y2="21" stroke="#5ce0c1"/><line x1="12" y1="17" x2="12" y2="21" stroke="#5ce0c1"/><polygon points="10 7 15 10 10 13" fill="#5ce0c1" stroke="#5ce0c1"/>', size=16)
    elif name == "refresh":
        icon = make_svg_icon('<polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/>', stroke="#8295a9", size=16)
    elif name == "play":
        icon = make_svg_icon('<polygon points="5 3 19 12 5 21 5 3" fill="#5ce0c1" stroke="#5ce0c1"/>', size=16)
    elif name in ("play_dark", "play_primary"):
        icon = make_svg_icon('<polygon points="5 3 19 12 5 21 5 3" fill="#09231e" stroke="#09231e"/>', size=16)
    elif name == "stop":
        icon = make_svg_icon('<rect x="4" y="4" width="16" height="16" rx="2" ry="2" fill="#ff9f9f" stroke="#ff9f9f"/>', size=16)
    elif name == "rotate":
        icon = make_svg_icon('<polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/>', stroke="#8295a9", size=16)
    elif name == "camera":
        icon = make_svg_icon('<path d="M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z"/><circle cx="12" cy="13" r="4"/>', stroke="#8295a9", size=16)
    elif name == "fullscreen":
        icon = make_svg_icon('<path d="M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3"/>', stroke="#8295a9", size=16)
    elif name == "more":
        icon = make_svg_icon('<circle cx="12" cy="12" r="1.5" fill="#8295a9"/><circle cx="5" cy="12" r="1.5" fill="#8295a9"/><circle cx="19" cy="12" r="1.5" fill="#8295a9"/>', stroke="#8295a9", size=16)
    elif name == "hub":
        icon = make_svg_icon('<circle cx="12" cy="12" r="3"/><circle cx="5" cy="6" r="2"/><circle cx="19" cy="6" r="2"/><circle cx="12" cy="20" r="2"/><line x1="7" y1="7.5" x2="10" y2="10.5"/><line x1="17" y1="7.5" x2="14" y2="10.5"/><line x1="12" y1="15" x2="12" y2="18"/>', stroke="#8295a9", size=16)
    elif name == "search":
        icon = make_svg_icon('<circle cx="11" cy="11" r="7"/><line x1="21" y1="21" x2="16.65" y2="16.65"/>', stroke="#627588", size=14)
    elif name == "clear":
        icon = make_svg_icon('<line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/>', stroke="#8295a9", size=14)
    elif name == "export":
        icon = make_svg_icon('<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/>', stroke="#8295a9", size=14)
    elif name == "chrome":
        icon = make_svg_icon('<circle cx="12" cy="12" r="10"/><circle cx="12" cy="12" r="4"/><line x1="21.17" y1="8" x2="12" y2="8"/><line x1="3.95" y1="6.06" x2="8.54" y2="14"/><line x1="10.88" y1="21.94" x2="15.46" y2="14"/>', stroke="#8295a9", size=14)
    elif name == "layout_h":
        icon = make_svg_icon('<rect x="3" y="3" width="18" height="18" rx="2"/><line x1="12" y1="3" x2="12" y2="21"/>', stroke="#8295a9", size=14)
    elif name == "layout_v":
        icon = make_svg_icon('<rect x="3" y="3" width="18" height="18" rx="2"/><line x1="3" y1="12" x2="21" y2="12"/>', stroke="#8295a9", size=14)
    elif name == "layout_screen":
        icon = make_svg_icon('<rect x="3" y="3" width="18" height="18" rx="2"/><polygon points="10 8 16 12 10 16 10 8" fill="#5ce0c1" stroke="#5ce0c1"/>', stroke="#8295a9", size=14)
    elif name == "layout_logs":
        icon = make_svg_icon('<rect x="3" y="3" width="18" height="18" rx="2"/><line x1="7" y1="8" x2="17" y2="8"/><line x1="7" y1="12" x2="17" y2="12"/><line x1="7" y1="16" x2="13" y2="16"/>', stroke="#8295a9", size=14)
    elif name == "device":
        icon = make_svg_icon('<rect x="5" y="2" width="14" height="20" rx="2" ry="2"/><line x1="12" y1="18" x2="12.01" y2="18"/>', stroke="#8295a9", size=16)
    elif name in ("command", "terminal"):
        icon = make_svg_icon('<polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/>', stroke="#8295a9", size=16)
    elif name == "logs":
        icon = make_svg_icon('<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/><line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/>', stroke="#8295a9", size=16)
    elif name == "web":
        icon = make_svg_icon('<circle cx="12" cy="12" r="10"/><line x1="2" y1="12" x2="22" y2="12"/><path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10z"/>', stroke="#8295a9", size=16)
    elif name == "pause":
        icon = make_svg_icon('<rect x="6" y="4" width="4" height="16"/><rect x="14" y="4" width="4" height="16"/>', stroke="#8295a9", size=16)
    elif name == "back":
        icon = make_svg_icon('<polyline points="9 14 4 9 9 4"/><path d="M20 20v-7a4 4 0 0 0-4-4H4"/>', stroke="#8295a9", size=16)
    elif name == "home":
        icon = make_svg_icon('<path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><polyline points="9 22 9 12 15 12 15 22"/>', stroke="#8295a9", size=16)
    elif name == "sun":
        icon = make_svg_icon('<circle cx="12" cy="12" r="5"/><line x1="12" y1="1" x2="12" y2="3"/><line x1="12" y1="21" x2="12" y2="23"/><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"/><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"/><line x1="1" y1="12" x2="3" y2="12"/><line x1="21" y1="12" x2="23" y2="12"/><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"/><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"/>', stroke="#f3c969", size=16)
    elif name == "rocket":
        icon = make_svg_icon('<path d="M4.5 16.5c-1.5 1.26-2 5-2 5s3.74-.5 5-2c.71-.84.7-2.13-.09-2.91a2.18 2.18 0 0 0-2.91-.09z"/><path d="M12 15l-3-3a22 22 0 0 1 2-3.95A12.88 12.88 0 0 1 22 2c0 2.72-.78 7.5-3.05 11a22.5 22.5 0 0 1-3.95 2z"/>', stroke="#8295a9", size=16)
    elif name == "app":
        icon = make_svg_icon('<polygon points="12 2 2 7 12 12 22 7 12 2"/><polyline points="2 17 12 22 22 17"/><polyline points="2 12 12 17 22 12"/>', stroke="#8295a9", size=16)
    elif name == "package":
        icon = make_svg_icon('<line x1="16.5" y1="9.4" x2="7.5" y2="4.21"/><path d="M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"/><polyline points="3.27 6.96 12 12.01 20.73 6.96"/><line x1="12" y1="22.08" x2="12" y2="12"/>', stroke="#8295a9", size=16)
    elif name == "shield":
        icon = make_svg_icon('<path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/>', stroke="#5ce0c1", size=16)
    elif name == "bone":
        icon = make_svg_icon('<path d="M17 10c.7-.7 1.6-1 2.5-.7 1.5.5 2 2.3 1.1 3.5-.3.4-.4.9-.3 1.4.3 1.4-.7 2.8-2.1 2.8-.7 0-1.4-.3-1.9-.8L6.3 6.2C5.8 5.7 5.1 5.4 4.4 5.4 3 5.4 2 6.8 2.3 8.2c.1.5 0 1-.3 1.4-.9 1.2-.4 3 1.1 3.5.9.3 1.8 0 2.5-.7L17 10z"/>', stroke="#8295a9", size=16)
    elif name == "settings":
        icon = make_svg_icon(
            '<circle cx="12" cy="12" r="3"/>'
            '<path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"/>',
            stroke="#8295a9",
            size=16,
        )

    _CACHED_ICONS[name] = icon
    return icon


def button(text: str, callback=None, name: str = "", icon_name: str = "") -> QPushButton:
    item = QPushButton(text)
    if icon_name:
        effective_icon = f"{icon_name}_dark" if name == "Primary" and f"{icon_name}_dark" in ("connect_dark", "play_dark") else icon_name
        item.setIcon(get_ui_icon(effective_icon))
        item.setIconSize(QSize(14, 14))
        item.setProperty("icon_name", icon_name)
    item.setProperty("full_text", text)
    item.setObjectName(name)
    item.setAccessibleName(text.strip())
    if callback:
        item.clicked.connect(callback)
    return item


def icon_button(icon_name: str, callback, tooltip: str = "", name: str = "IconBtn", width: int = 30, height: int = 24) -> QPushButton:
    item = QPushButton()
    item.setIcon(get_ui_icon(icon_name))
    item.setIconSize(QSize(15, 15))
    item.setObjectName(name)
    item.setFixedSize(width, height)
    if tooltip:
        item.setToolTip(tooltip)
        item.setAccessibleName(tooltip)
    item.setCursor(Qt.PointingHandCursor)
    item.clicked.connect(callback)
    return item


def get_dark_palette() -> QPalette:
    palette = QPalette()
    palette.setColor(QPalette.Window, QColor("#111923"))
    palette.setColor(QPalette.WindowText, QColor("#e7edf4"))
    palette.setColor(QPalette.Base, QColor("#0c131c"))
    palette.setColor(QPalette.AlternateBase, QColor("#141f2c"))
    palette.setColor(QPalette.ToolTipBase, QColor("#243549"))
    palette.setColor(QPalette.ToolTipText, QColor("#ffffff"))
    palette.setColor(QPalette.Text, QColor("#e7edf4"))
    palette.setColor(QPalette.Button, QColor("#1d2938"))
    palette.setColor(QPalette.ButtonText, QColor("#e7edf4"))
    palette.setColor(QPalette.BrightText, QColor("#5ce0c1"))
    palette.setColor(QPalette.Highlight, QColor("#1b3e35"))
    palette.setColor(QPalette.HighlightedText, QColor("#79e6c9"))
    return palette


def setup_combobox_popup(combo: QComboBox, min_width: int = 0) -> None:
    """消除 QComboBox 在 macOS/Qt 下原生弹出层的白边框与浅色底色瑕疵"""
    if combo is None:
        return
    view = combo.view()
    if not view:
        return
    if min_width > 0:
        view.setMinimumWidth(min_width)
    view.setStyleSheet(
        "background-color: #141f2c; color: #e7edf4; border: 1px solid #304257; "
        "border-radius: 6px; selection-background-color: #1b3e35; outline: none; padding: 2px;"
    )
    container = view.window()
    if container and container != combo:
        container.setAttribute(Qt.WA_TranslucentBackground, True)
        container.setStyleSheet(
            "background-color: #141f2c; color: #e7edf4; border: 1px solid #304257; border-radius: 6px;"
        )
        layout = container.layout()
        if layout:
            layout.setContentsMargins(0, 0, 0, 0)
            layout.setSpacing(0)
            while layout.count() > 0 and layout.itemAt(0).spacerItem():
                layout.takeAt(0)
            while layout.count() > 0 and layout.itemAt(layout.count() - 1).spacerItem():
                layout.takeAt(layout.count() - 1)


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


class ConsoleInput(QLineEdit):
    """支持历史命令上下箭头上翻的控制台输入框"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.history: list[str] = []
        self.history_index: int = -1
        self._temp_edit: str = ""

    def push_history(self, command: str):
        cmd = command.strip()
        if cmd:
            if not self.history or self.history[-1] != cmd:
                self.history.append(cmd)
            self.history_index = len(self.history)
            self._temp_edit = ""

    def keyPressEvent(self, event):
        if event.key() == Qt.Key_Up:
            if self.history:
                if self.history_index == len(self.history):
                    self._temp_edit = self.text()
                if self.history_index > 0:
                    self.history_index -= 1
                    self.setText(self.history[self.history_index])
            return
        elif event.key() == Qt.Key_Down:
            if self.history:
                if self.history_index < len(self.history) - 1:
                    self.history_index += 1
                    self.setText(self.history[self.history_index])
                elif self.history_index == len(self.history) - 1:
                    self.history_index = len(self.history)
                    self.setText(self._temp_edit)
            return
        super().keyPressEvent(event)


class FlowLayout(QLayout):
    """自适应流式多行换行布局，当容器宽度不足时子控件自动折行排列，支持 Expanding 控件按行吸收剩余空间"""
    def __init__(self, parent=None, margin=0, hspacing=-1, vspacing=-1):
        super().__init__(parent)
        if parent is not None:
            self.setContentsMargins(margin, margin, margin, margin)
        self._hspacing = hspacing
        self._vspacing = vspacing
        self._item_list = []

    def __del__(self):
        item = self.takeAt(0)
        while item:
            item = self.takeAt(0)

    def addItem(self, item):
        self._item_list.append(item)

    def horizontalSpacing(self):
        return self._hspacing if self._hspacing >= 0 else 6

    def verticalSpacing(self):
        return self._vspacing if self._vspacing >= 0 else 6

    def count(self):
        return len(self._item_list)

    def itemAt(self, index):
        if 0 <= index < len(self._item_list):
            return self._item_list[index]
        return None

    def takeAt(self, index):
        if 0 <= index < len(self._item_list):
            return self._item_list.pop(index)
        return None

    def expandingDirections(self):
        for item in self._item_list:
            wid = item.widget()
            if wid and (wid.sizePolicy().horizontalPolicy() & QSizePolicy.ExpandFlag):
                return Qt.Horizontal
        return Qt.Orientations(0)

    def hasHeightForWidth(self):
        return True

    def heightForWidth(self, width):
        return self._do_layout(QRect(0, 0, width, 0), True)

    def setGeometry(self, rect):
        super().setGeometry(rect)
        self._do_layout(rect, False)

    def sizeHint(self):
        return self.minimumSize()

    def minimumSize(self):
        size = QSize()
        for item in self._item_list:
            size = size.expandedTo(item.minimumSize())
        margins = self.contentsMargins()
        size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom())
        return size

    def _calculate_lines(self, target_width):
        lines = []
        current_line = []
        current_width = 0
        h_spacing = self.horizontalSpacing()

        for item in self._item_list:
            wid = item.widget()
            if wid and wid.isHidden():
                continue
            if wid and wid.minimumWidth() == wid.maximumWidth() and wid.maximumWidth() < 16777215:
                item_w = wid.width()
            elif wid and wid.minimumWidth() > 0:
                item_w = wid.minimumWidth()
            else:
                item_w = item.sizeHint().width()

            space = h_spacing if current_line else 0
            if current_line and (current_width + space + item_w) > target_width:
                lines.append(current_line)
                current_line = [item]
                current_width = item_w
            else:
                current_line.append(item)
                current_width += space + item_w

        if current_line:
            lines.append(current_line)
        return lines

    def _do_layout(self, rect, test_only):
        margins = self.contentsMargins()
        effective_rect = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom())
        target_width = max(1, effective_rect.width())
        lines = self._calculate_lines(target_width)

        y = effective_rect.y()
        total_height = 0
        h_spacing = self.horizontalSpacing()
        v_spacing = self.verticalSpacing()

        for line_items in lines:
            line_height = 0
            fixed_width = 0
            expanding_items = []

            for item in line_items:
                wid = item.widget()
                hint = item.sizeHint()
                min_s = item.minimumSize()
                max_s = item.maximumSize()
                if wid and wid.maximumHeight() < 16777215:
                    h = wid.maximumHeight()
                else:
                    h = max(min_s.height(), hint.height())
                line_height = max(line_height, h)

                is_expanding = wid and bool(wid.sizePolicy().horizontalPolicy() & QSizePolicy.ExpandFlag)
                if is_expanding:
                    expanding_items.append(item)
                    fixed_width += max(0, wid.minimumWidth())
                else:
                    if wid and wid.minimumWidth() == wid.maximumWidth() and wid.maximumWidth() < 16777215:
                        w = wid.width()
                    else:
                        w = max(min_s.width(), hint.width())
                    fixed_width += w

            num_spacings = max(0, len(line_items) - 1)
            extra_w = max(0, target_width - fixed_width - (num_spacings * h_spacing))
            add_per_expanding = (extra_w // len(expanding_items)) if expanding_items else 0

            x = effective_rect.x()
            for item in line_items:
                wid = item.widget()
                hint = item.sizeHint()
                min_s = item.minimumSize()
                max_s = item.maximumSize()
                if wid and wid.maximumHeight() < 16777215:
                    h = wid.maximumHeight()
                else:
                    h = max(min_s.height(), hint.height())

                is_expanding = item in expanding_items
                if is_expanding:
                    w = max(wid.minimumWidth(), wid.minimumWidth() + add_per_expanding)
                else:
                    if wid and wid.minimumWidth() == wid.maximumWidth() and wid.maximumWidth() < 16777215:
                        w = wid.width()
                    else:
                        w = max(min_s.width(), hint.width())

                if not test_only:
                    item.setGeometry(QRect(QPoint(x, y), QSize(w, h)))
                x += w + h_spacing

            y += line_height + v_spacing
            total_height += line_height + v_spacing

        if lines:
            total_height -= v_spacing
        return total_height + margins.top() + margins.bottom()


class LogView(QTextEdit):
    clear_requested = pyqtSignal()

    def __init__(self):
        super().__init__()
        self.setAcceptRichText(False)
        self.setUndoRedoEnabled(False)
        self.document().setMaximumBlockCount(10000)
        self.line_format = QTextBlockFormat()
        self.line_format.setLineHeight(140, QTextBlockFormat.ProportionalHeight)

    def contextMenuEvent(self, event):
        menu = self.createStandardContextMenu()
        menu.addSeparator()
        clear_action = menu.addAction("清空日志 (Clear)", self._on_clear_requested)
        clear_action.setIcon(get_ui_icon("delete"))
        clear_action.setShortcut(QKeySequence("Ctrl+K"))
        menu.exec_(event.globalPos())

    def _on_clear_requested(self):
        self.clear_requested.emit()

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
    back_requested = pyqtSignal()
    start_requested = pyqtSignal()

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

        self.setMinimumSize(80, 80)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.setAccessibleName("设备投屏画面")
        self.switching_message = ""
        self.setMouseTracking(False)
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
        if not image.isNull():
            self.switching_message = ""
        else:
            self._last_image_size = (0, 0)
            self._last_size = (0, 0)
        self._update_target_rect()
        self.update()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._update_target_rect()

    def _update_target_rect(self):
        if self.image.isNull() or self.width() <= 0 or self.height() <= 0:
            self._target_rect = QRect()
            self._last_image_size = (0, 0)
            self._last_size = (0, 0)
            return
        img_size = (self.image.width(), self.image.height())
        widget_size = (self.width(), self.height())
        if self._target_rect.isValid() and img_size == self._last_image_size and widget_size == self._last_size:
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
            if getattr(self, "switching_message", ""):
                painter.drawText(0, cy + 68, self.width(), 30, Qt.AlignCenter, self.switching_message)
                painter.setFont(QFont("PingFang SC", 12))
                painter.setPen(QColor("#748ca2"))
                painter.drawText(0, cy + 103, self.width(), 25, Qt.AlignCenter, "正在排他连接目标设备并同步各模块，请稍候…")
            elif self._active:
                painter.drawText(0, cy + 68, self.width(), 30, Qt.AlignCenter, "正在连接设备画面…")
                painter.setFont(QFont("PingFang SC", 12))
                painter.setPen(QColor("#748ca2"))
                painter.drawText(0, cy + 103, self.width(), 25, Qt.AlignCenter, "已发送唤醒与捕获请求，画面即将同步")
            else:
                painter.drawText(0, cy + 68, self.width(), 30, Qt.AlignCenter, "让设备画面来到桌面")
                painter.setFont(QFont("PingFang SC", 12))
                painter.setPen(QColor("#748ca2"))
                painter.drawText(0, cy + 103, self.width(), 25, Qt.AlignCenter, "连接 USB 设备，点击屏幕或下方「开始投屏」")
            return
        if not self._target_rect.isValid() or self._target_rect.width() <= 0 or self._target_rect.height() <= 0:
            self._update_target_rect()
        if not self._target_rect.isValid() or self._target_rect.width() <= 0 or self._target_rect.height() <= 0:
            scaled = self.image.size().scaled(self.size(), Qt.KeepAspectRatio)
            left = (self.width() - scaled.width()) // 2
            top = (self.height() - scaled.height()) // 2
            self._target_rect = QRect(left, top, max(1, scaled.width()), max(1, scaled.height()))
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
        if event.button() == Qt.RightButton:
            if self._active and not self.image.isNull():
                self.back_requested.emit()
            return

        if event.button() == Qt.LeftButton:
            if not self._active or self.image.isNull():
                self.start_requested.emit()
                return
            coords = self.coordinates(event.pos())
            if coords is None:
                if not self._active:
                    self.start_requested.emit()
                return
            self.drag_start = coords
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


class WorkspaceStackedWidget(QStackedWidget):
    """
    工作台堆叠容器：统一管理 3 个主模块页面 (0: 设备信息, 1: 投屏工作台, 2: 常用命令)。
    """
    def __init__(self, main_window, parent=None):
        super().__init__(parent)
        self.main_window = main_window

    def currentWidget(self):
        return super().currentWidget()

    def count(self):
        return 3

    def setCurrentIndex(self, index: int):
        if index == 0:
            super().setCurrentWidget(self.main_window.info_panel)
        elif index == 2:
            super().setCurrentWidget(self.main_window.hdc_panel)
        else:
            super().setCurrentWidget(self.main_window.workspace_page)

    def currentIndex(self):
        w = super().currentWidget()
        if w is getattr(self.main_window, "info_panel", None):
            return 0
        if w is getattr(self.main_window, "hdc_panel", None):
            return 2
        return 1

    def setCurrentWidget(self, widget):
        if widget is getattr(self.main_window, "info_panel", None):
            super().setCurrentWidget(self.main_window.info_panel)
        elif widget is getattr(self.main_window, "hdc_panel", None):
            super().setCurrentWidget(self.main_window.hdc_panel)
        elif widget in (getattr(self.main_window, "workspace_page", None),
                        getattr(self.main_window, "mirror_page", None),
                        getattr(self.main_window, "log_panel", None),
                        getattr(self.main_window, "webview_panel", None)):
            super().setCurrentWidget(self.main_window.workspace_page)
        else:
            super().setCurrentWidget(widget)


class ToastWidget(QFrame):
    """浮动半透明气泡 Toast 提示组件，位于主窗口顶部居中，带平滑淡入淡出动效。"""

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setAttribute(Qt.WA_TransparentForMouseEvents, True)
        self.setAttribute(Qt.WA_ShowWithoutActivating, True)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(18, 9, 18, 9)
        layout.setSpacing(8)

        self.icon_label = QLabel(self)
        self.icon_label.setStyleSheet("background: transparent; font-size: 14px;")
        layout.addWidget(self.icon_label)

        self.text_label = QLabel(self)
        self.text_label.setFont(QFont("PingFang SC", 12))
        self.text_label.setStyleSheet("color: #eaf2fa; background: transparent; font-weight: 500;")
        layout.addWidget(self.text_label)

        self.setStyleSheet("""
            ToastWidget {
                background-color: rgba(18, 28, 40, 0.96);
                border: 1px solid #3c546e;
                border-radius: 19px;
            }
        """)

        self._opacity_effect = QGraphicsOpacityEffect(self)
        self.setGraphicsEffect(self._opacity_effect)
        self._opacity_effect.setOpacity(0.0)

        self._anim = QPropertyAnimation(self._opacity_effect, b"opacity", self)
        self._anim.setDuration(220)

        self._dismiss_timer = QTimer(self)
        self._dismiss_timer.setSingleShot(True)
        self._dismiss_timer.timeout.connect(self._fade_out)

        self.hide()

    def reposition(self):
        if self.parent():
            parent_rect = self.parent().rect()
            x = (parent_rect.width() - self.width()) // 2
            y = 52
            self.move(x, y)

    def show_message(self, message: str, level: str = "info", duration_ms: int = 3200):
        icons = {
            "info": "ℹ️",
            "warning": "⚠️",
            "error": "❌",
            "success": "✅",
        }
        self.icon_label.setText(icons.get(level, "ℹ️"))
        self.text_label.setText(message)
        self.adjustSize()
        self.reposition()

        self._dismiss_timer.stop()
        self._anim.stop()

        self.show()
        self.raise_()

        self._anim.setStartValue(self._opacity_effect.opacity())
        self._anim.setEndValue(1.0)
        self._anim.start()

        self._dismiss_timer.start(duration_ms)

    def _fade_out(self):
        self._anim.stop()
        self._anim.setStartValue(self._opacity_effect.opacity())
        self._anim.setEndValue(0.0)
        self._anim.finished.connect(self._on_fade_out_finished)
        self._anim.start()

    def _on_fade_out_finished(self):
        try:
            self._anim.finished.disconnect(self._on_fade_out_finished)
        except Exception:
            pass
        if self._opacity_effect.opacity() == 0.0:
            self.hide()


class WebViewConsoleView(QPlainTextEdit):
    clear_requested = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setReadOnly(True)
        self.setUndoRedoEnabled(False)
        self.setMaximumBlockCount(3000)
        self.setLineWrapMode(QPlainTextEdit.NoWrap)

    def contextMenuEvent(self, event):
        menu = self.createStandardContextMenu()
        menu.addSeparator()
        clear_act = menu.addAction("清空控制台 (Clear)", self.clear_requested.emit)
        clear_act.setIcon(get_ui_icon("delete"))
        clear_act.setShortcut(QKeySequence("Ctrl+K"))
        menu.exec_(event.globalPos())


class NetworkDeviceDialog(QDialog):
    def __init__(self, parent=None, on_connect=None, on_disconnect=None, on_reconnect=None, on_select=None, connected_targets=None, current_target=None, hdc=None):
        super().__init__(parent)
        self.on_connect = on_connect
        self.on_disconnect = on_disconnect
        self.on_reconnect = on_reconnect
        self.on_select = on_select
        self.current_target = current_target
        self.hdc = hdc
        self.connected_targets = set(connected_targets or [])
        self._displayed_devices = []
        self.setWindowTitle("设备维护与连接中心")
        self.setMinimumSize(580, 440)
        self.resize(640, 480)
        self._init_ui()
        self._load_data()

    def _init_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(12)

        hdr_layout = QVBoxLayout()
        hdr_layout.setSpacing(6)

        hdr_top = QHBoxLayout()
        hdr_top.addWidget(label("设备维护与连接中心", "CardTitle"))
        hdr_top.addStretch()
        hdr_layout.addLayout(hdr_top)

        self.target_status_label = label(
            "默认端口 8710 · 单设备独占连接 · 双击列表项快速切换",
            "Muted",
        )
        hdr_layout.addWidget(self.target_status_label)
        layout.addLayout(hdr_layout)

        self.table = QTableWidget()
        self.table.setColumnCount(4)
        self.table.setHorizontalHeaderLabels(["设备 IP", "端口", "状态", "操作"])
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(QHeaderView.Interactive)
        header.setStretchLastSection(True)
        header.setDefaultAlignment(Qt.AlignCenter)
        self.table.setColumnWidth(0, 210)
        self.table.setColumnWidth(1, 85)
        self.table.setColumnWidth(2, 105)
        self.table.verticalHeader().setVisible(False)
        self.table.setSelectionBehavior(QTableWidget.SelectRows)
        self.table.setSelectionMode(QTableWidget.SingleSelection)
        self.table.cellDoubleClicked.connect(self._on_table_double_clicked)
        layout.addWidget(self.table, 1)

        card = QFrame()
        card.setObjectName("InfoCard")
        card_layout = QVBoxLayout(card)
        card_layout.setContentsMargins(14, 12, 14, 12)
        card_layout.setSpacing(10)
        card_layout.addWidget(label("快速连接与断开", "Section"))

        input_row = QHBoxLayout()
        input_row.setSpacing(8)

        self.ip_input = QLineEdit()
        self.ip_input.setPlaceholderText("设备 IP 地址，例如 10.10.220.41")
        self.ip_input.returnPressed.connect(self._on_connect_and_save)
        input_row.addWidget(self.ip_input, 1)

        input_row.addWidget(label(":", "Muted"))
        self.port_input = QLineEdit(str(DEFAULT_NETWORK_PORT))
        self.port_input.setPlaceholderText(str(DEFAULT_NETWORK_PORT))
        self.port_input.setFixedWidth(68)
        self.port_input.setAlignment(Qt.AlignCenter)
        self.port_input.setToolTip("连接端口 (默认 8710，支持自定义修改)")
        self.port_input.returnPressed.connect(self._on_connect_and_save)
        input_row.addWidget(self.port_input)

        self.name_input = QLineEdit()

        self.btn_disconnect_input = button("断开", self._on_disconnect_input, "Danger", icon_name="disconnect")
        self.btn_disconnect_input.setToolTip("断开目标连接并从底层 HDC 中彻底删除 (-remove)")
        input_row.addWidget(self.btn_disconnect_input)

        self.btn_connect = button("连接", self._on_connect_and_save, "Primary", icon_name="connect")
        self.btn_connect.setToolTip("连接该设备并自动存入列表")
        self.btn_connect_save = self.btn_connect
        self.btn_connect_only = self.btn_connect
        self.btn_save_only = self.btn_connect
        input_row.addWidget(self.btn_connect)

        card_layout.addLayout(input_row)

        self.error_label = label("", "Muted")
        self.error_label.setStyleSheet("color: #ff9f9f; font-size: 12px;")
        card_layout.addWidget(self.error_label)

        layout.addWidget(card)

    def _parse_input_endpoint(self) -> tuple[str, int]:
        raw_ip = self.ip_input.text().strip()
        if not raw_ip:
            raise ValueError("请输入设备 IP 地址")
        if ":" in raw_ip:
            clean_ip, port_str = raw_ip.rsplit(":", 1)
        else:
            clean_ip = raw_ip
            port_str = self.port_input.text().strip() if hasattr(self, "port_input") else str(DEFAULT_NETWORK_PORT)
        port = int(port_str) if port_str.isdigit() and 1 <= int(port_str) <= 65535 else DEFAULT_NETWORK_PORT
        return clean_ip, port

    def _on_refresh_targets(self):
        hdc_inst = self.hdc or (self.parent() and getattr(self.parent(), "hdc", None))
        if hdc_inst:
            try:
                self.connected_targets = set(hdc_inst.devices())
            except Exception:
                pass
        self._load_data()

    def _load_data(self):
        saved_list = load_saved_devices()
        saved_map = {d["ip"]: d for d in saved_list}

        # 核心：自动合并当前 hdc list targets 中的在线网络设备，确保已连设备绝不漏显
        merged = list(saved_list)
        for target in sorted(self.connected_targets):
            if ":" in target:
                host, port_str = target.split(":", 1)
                port_val = int(port_str) if port_str.isdigit() else DEFAULT_NETWORK_PORT
                if host not in saved_map:
                    merged.append({
                        "ip": host,
                        "port": port_val,
                        "_uncommitted": True,
                    })

        conn_count = sum(1 for t in self.connected_targets if ":" in t)
        if hasattr(self, "target_status_label"):
            self.target_status_label.setText(
                f"在线目标: {conn_count} 台 · 端口固定 8710 · 单设备独占连接 · 双击列表项快速切换"
            )

        if not merged:
            self.table.clearSpans()
            self.table.setRowCount(1)
            item = QTableWidgetItem("（暂无设备，请在下方输入 IP 连接）")
            item.setForeground(QColor("#8295a9"))
            item.setTextAlignment(Qt.AlignCenter)
            item.setFlags(Qt.NoItemFlags)
            self.table.setItem(0, 0, item)
            self.table.setSpan(0, 0, 1, 4)
            self._displayed_devices = []
            return

        self._displayed_devices = merged
        self.table.clearSpans()
        self.table.setRowCount(len(merged))
        for row, dev in enumerate(merged):
            ip = dev.get("ip", "")
            port = str(dev.get("port", DEFAULT_NETWORK_PORT))
            ep = f"{ip}:{port}"
            is_conn = ep in self.connected_targets or ip in self.connected_targets

            item_ip = QTableWidgetItem(ip)
            item_ip.setForeground(QColor("#e7edf4"))
            item_ip.setTextAlignment(Qt.AlignCenter)
            font = item_ip.font()
            font.setBold(True)
            item_ip.setFont(font)
            self.table.setItem(row, 0, item_ip)

            item_port = QTableWidgetItem(port)
            item_port.setTextAlignment(Qt.AlignCenter)
            item_port.setForeground(QColor("#8295a9"))
            self.table.setItem(row, 1, item_port)

            item_status = QTableWidgetItem("● 已连接" if is_conn else "○ 未连接")
            item_status.setTextAlignment(Qt.AlignCenter)
            item_status.setForeground(QColor("#5ce0c1" if is_conn else "#8295a9"))
            self.table.setItem(row, 2, item_status)

            actions = QWidget()
            act_layout = QHBoxLayout(actions)
            act_layout.setContentsMargins(4, 0, 4, 0)
            act_layout.setSpacing(6)
            act_layout.setAlignment(Qt.AlignCenter)

            if is_conn:
                btn_select = icon_button("mirror", lambda _, e=ep: self._select_target(e), tooltip=f"切换至 {ep} 并投屏", name="IconBtnPrimary", width=34, height=28)
                act_layout.addWidget(btn_select)

                btn_disconn = icon_button("disconnect", lambda _, e=ep: self._disconnect_target(e), tooltip=f"断开 {ep} 连接并从 HDC 目标彻底删除 (-remove)", name="IconBtnDanger", width=34, height=28)
                act_layout.addWidget(btn_disconn)
            else:
                btn_conn = icon_button("connect", lambda _, e=ep: self._connect_target(e), tooltip=f"连接设备 {ep}", name="IconBtnPrimary", width=34, height=28)
                act_layout.addWidget(btn_conn)

            btn_del = icon_button("delete", lambda _, target_ip=ip: self._delete_target(target_ip), tooltip=f"从设备列表中删除 {ip}", name="IconBtnMuted", width=34, height=28)
            act_layout.addWidget(btn_del)

            self.table.setCellWidget(row, 3, actions)
            self.table.setRowHeight(row, 44)

    def _quick_save_uncommitted(self, ip: str, port: int = DEFAULT_NETWORK_PORT):
        try:
            save_device(ip, name="", port=port)
            self.error_label.setStyleSheet("color: #79e6c9; font-size: 12px;")
            self.error_label.setText(f"已将 {ip}:{port} 存入列表")
        except Exception as e:
            self.error_label.setText(str(e))
        self._load_data()

    def _select_target(self, endpoint: str):
        self.accept()
        if callable(self.on_select):
            self.on_select(endpoint)
        elif callable(self.on_connect):
            self.on_connect(endpoint)

    def _on_table_double_clicked(self, row: int, column: int):
        devices = getattr(self, "_displayed_devices", load_saved_devices())
        if 0 <= row < len(devices):
            dev = devices[row]
            endpoint = f"{dev['ip']}:{dev.get('port', DEFAULT_NETWORK_PORT)}"
            if endpoint in self.connected_targets or dev["ip"] in self.connected_targets:
                if callable(self.on_select):
                    self._select_target(endpoint)
                else:
                    self._reconnect_target(endpoint)
            else:
                self._connect_target(endpoint)

    def _on_connect_and_save(self):
        try:
            clean_ip, port = self._parse_input_endpoint()
        except ValueError as err:
            self.error_label.setText(str(err))
            return
        name = getattr(self, "name_input", None) and self.name_input.text().strip() or ""
        try:
            save_device(clean_ip, name=name, port=port)
        except Exception as err:
            self.error_label.setText(str(err))
            return
        self.error_label.setText("")
        self._load_data()
        self._connect_target(f"{clean_ip}:{port}")

    def _on_connect_only(self):
        try:
            clean_ip, port = self._parse_input_endpoint()
        except ValueError as err:
            self.error_label.setText(str(err))
            return
        self.error_label.setText("")
        self._connect_target(f"{clean_ip}:{port}")

    def _on_disconnect_input(self):
        try:
            clean_ip, port = self._parse_input_endpoint()
        except ValueError as err:
            self.error_label.setText(str(err))
            return
        endpoint = f"{clean_ip}:{port}"
        self._disconnect_target(endpoint)

    def _on_save_only(self):
        try:
            clean_ip, port = self._parse_input_endpoint()
        except ValueError as err:
            self.error_label.setText(str(err))
            return
        name = getattr(self, "name_input", None) and self.name_input.text().strip() or ""
        try:
            save_device(clean_ip, name=name, port=port)
        except Exception as err:
            self.error_label.setText(str(err))
            return
        self.error_label.setText("")
        self.ip_input.clear()
        self._load_data()
        self.ip_input.setFocus()
        self.ip_input.setFocus()

    def _delete_target(self, ip: str):
        reply = QMessageBox.question(
            self,
            "确认删除设备",
            f"确定要从列表中删除设备 {ip} 吗？\n删除后该设备将不再出现在设备列表和快捷下拉菜单中。",
            QMessageBox.Yes | QMessageBox.No,
            QMessageBox.No,
        )
        if reply != QMessageBox.Yes:
            return
        delete_saved_device(ip)
        self.error_label.setStyleSheet("color: #79e6c9; font-size: 12px;")
        self.error_label.setText(f"已删除设备 {ip}")
        self._load_data()

    def _connect_target(self, endpoint: str):
        self.accept()
        if callable(self.on_connect):
            self.on_connect(endpoint)

    def _disconnect_target(self, endpoint: str):
        if callable(self.on_disconnect):
            self.on_disconnect(endpoint)
        self.connected_targets.discard(endpoint)
        self.connected_targets.discard(endpoint.rsplit(":", 1)[0])
        self.error_label.setStyleSheet("color: #79e6c9; font-size: 12px;")
        self.error_label.setText(f"已执行 hdc tconn {endpoint} -remove")
        self._load_data()

    def _reconnect_target(self, endpoint: str):
        self.accept()
        if callable(self.on_reconnect):
            self.on_reconnect(endpoint)
        elif callable(self.on_connect):
            self.on_connect(endpoint)


DeviceHubDialog = NetworkDeviceDialog


class AppLauncherDialog(QDialog):
    """设备应用拉起与运行管理对话框 (解决黑屏或程序退出恢复)"""

    APP_LABELS = {
        "com.yarward.etnroomoh": ("医护房间分机 (RoomTerminal)", True),
        "com.yarward.etnbedsideoh": ("医护床旁分机 (BedsideTerminal)", True),
        "com.yarward.factorytest": ("硬件与工厂测试", True),
        "com.yarward.ohos.factorytest": ("工厂测试工具 (OHOS)", True),
        "com.ohos.settings": ("系统设置", False),
        "com.ohos.photos": ("系统相册与媒体", False),
        "com.ohos.camera": ("系统相机", False),
        "com.ohos.mms": ("信息中心", False),
        "com.ohos.contacts": ("通讯录", False),
        "com.ohos.filepicker": ("文件管理器", False),
        "com.ohos.smartperf": ("性能监控工具 (SmartPerf)", False),
    }

    def __init__(self, parent=None, hdc=None, serial: str = ""):
        super().__init__(parent)
        self.hdc = hdc
        self.serial = serial
        self.tools = DeviceTools(hdc) if hdc else None
        self.setWindowTitle(f"拉起设备应用 · {serial}")
        self.setMinimumSize(580, 500)
        self.resize(640, 540)
        self._all_apps: list[str] = []
        self._primary_app = ""
        self._displayed_apps = []
        self._init_ui()
        self._load_apps()

    def _init_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(12)

        hdr_layout = QVBoxLayout()
        hdr_layout.setSpacing(4)
        hdr_layout.addWidget(label("设备应用拉起与运行管理", "CardTitle"))
        hdr_layout.addWidget(label("支持一键拉起前台、恢复主程序或强制停止指定应用；双击列表项即可直接启动。", "Muted"))
        layout.addLayout(hdr_layout)

        # 快速拉起主要业务程序卡片
        self.primary_card = QFrame()
        self.primary_card.setObjectName("InfoCard")
        p_layout = QHBoxLayout(self.primary_card)
        p_layout.setContentsMargins(14, 10, 14, 10)
        self.primary_label = label("正在检测现场业务程序…", "Section")
        p_layout.addWidget(self.primary_label, 1)
        self.btn_launch_primary = button("拉起业务主程序", self._launch_detected_primary, "Primary", icon_name="play")
        self.btn_launch_primary.setEnabled(False)
        p_layout.addWidget(self.btn_launch_primary)
        layout.addWidget(self.primary_card)

        # 搜索框
        search_row = QHBoxLayout()
        search_row.setSpacing(8)
        self.search_input = QLineEdit()
        self.search_input.setPlaceholderText("搜索应用包名或关键字 (如 yarward, settings)...")
        self.search_input.textChanged.connect(self._filter_apps)
        search_row.addWidget(self.search_input, 1)
        self.btn_refresh = button("刷新", self._load_apps, icon_name="refresh")
        search_row.addWidget(self.btn_refresh)
        layout.addLayout(search_row)

        # 应用列表表格
        self.table = QTableWidget()
        self.table.setColumnCount(3)
        self.table.setHorizontalHeaderLabels(["应用名称", "包名", "操作"])
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        self.table.horizontalHeader().setSectionResizeMode(2, QHeaderView.Fixed)
        self.table.setColumnWidth(2, 130)
        self.table.verticalHeader().setVisible(False)
        self.table.setSelectionBehavior(QTableWidget.SelectRows)
        self.table.setSelectionMode(QTableWidget.SingleSelection)
        self.table.cellDoubleClicked.connect(self._on_table_double_clicked)
        layout.addWidget(self.table, 1)

        # 底部状态栏
        bottom_row = QHBoxLayout()
        self.status_label = label("", "Muted")
        bottom_row.addWidget(self.status_label, 1)
        btn_close = button("关闭", self.accept)
        bottom_row.addWidget(btn_close)
        layout.addLayout(bottom_row)

    def _load_apps(self):
        if not self.hdc or not self.serial:
            return
        self.status_label.setText("正在读取已安装应用列表…")
        try:
            apps = self.hdc.applications(self.serial)
            self._all_apps = sorted(apps)
            primary = [a for a in self._all_apps if a.startswith("com.yarward.") and "test" not in a]
            if primary:
                p_app = primary[0]
                lbl_text, _ = self.APP_LABELS.get(p_app, (p_app, True))
                self.primary_label.setText(f"识别到终端业务主程序：{lbl_text}")
                self._primary_app = p_app
                self.btn_launch_primary.setEnabled(True)
            else:
                self.primary_label.setText("未检测到特定的 Yarward 医疗业务主程序")
                self._primary_app = ""
                self.btn_launch_primary.setEnabled(False)
            self._filter_apps()
            self.status_label.setText(f"共发现 {len(self._all_apps)} 个已安装应用")
        except Exception as e:
            self.status_label.setText(f"读取应用失败: {e}")

    def _filter_apps(self):
        query = self.search_input.text().strip().lower()
        if not query:
            filtered = self._all_apps
        else:
            filtered = [
                a for a in self._all_apps
                if query in a.lower() or query in self.APP_LABELS.get(a, ("", False))[0].lower()
            ]

        def sort_key(b):
            is_priority = 0 if (b.startswith("com.yarward.") and "test" not in b) else (1 if b.startswith("com.yarward.") else 2)
            return (is_priority, b)

        filtered = sorted(filtered, key=sort_key)
        self.table.clearSpans()
        self.table.setRowCount(len(filtered))
        self._displayed_apps = filtered

        for row, bundle in enumerate(filtered):
            friendly_name, is_biz = self.APP_LABELS.get(bundle, (bundle.split(".")[-1], False))
            item_name = QTableWidgetItem(friendly_name)
            item_name.setIcon(get_ui_icon("app"))
            if is_biz:
                item_name.setForeground(QColor("#79e6c9"))
                f = item_name.font()
                f.setBold(True)
                item_name.setFont(f)
            else:
                item_name.setForeground(QColor("#cfdfeb"))
            self.table.setItem(row, 0, item_name)

            item_bundle = QTableWidgetItem(bundle)
            item_bundle.setForeground(QColor("#8295a9"))
            self.table.setItem(row, 1, item_bundle)

            actions = QWidget()
            act_layout = QHBoxLayout(actions)
            act_layout.setContentsMargins(4, 2, 4, 2)
            act_layout.setSpacing(6)

            btn_start = button("启动", lambda _, b=bundle: self._start_app(b), "Primary", icon_name="play")
            btn_start.setToolTip(f"拉起 {bundle} 至前台运行")
            btn_start.setFixedHeight(22)
            btn_start.setStyleSheet("padding: 0 6px; font-size: 11px;")
            act_layout.addWidget(btn_start)

            btn_stop = button("停止", lambda _, b=bundle: self._stop_app(b), "Danger", icon_name="stop")
            btn_stop.setToolTip(f"强制停止 {bundle}")
            btn_stop.setFixedHeight(22)
            btn_stop.setStyleSheet("padding: 0 6px; font-size: 11px;")
            act_layout.addWidget(btn_stop)

            self.table.setCellWidget(row, 2, actions)
            self.table.setRowHeight(row, 30)

    def _start_app(self, bundle: str):
        if not self.tools:
            return
        self.status_label.setText(f"正在拉起 {bundle}…")
        try:
            res = self.tools.start_application(self.serial, bundle)
            self.status_label.setStyleSheet("color: #79e6c9;")
            self.status_label.setText(f"已拉起 {bundle}：{res}")
        except Exception as e:
            self.status_label.setStyleSheet("color: #ff9f9f;")
            self.status_label.setText(f"拉起失败：{e}")

    def _stop_app(self, bundle: str):
        if not self.tools:
            return
        try:
            self.tools.stop_application(self.serial, bundle)
            self.status_label.setStyleSheet("color: #79e6c9;")
            self.status_label.setText(f"已停止应用 {bundle}")
        except Exception as e:
            self.status_label.setStyleSheet("color: #ff9f9f;")
            self.status_label.setText(f"停止失败：{e}")

    def _launch_detected_primary(self):
        if self._primary_app:
            self._start_app(self._primary_app)

    def _on_table_double_clicked(self, row: int, col: int):
        if 0 <= row < len(self._displayed_apps):
            self._start_app(self._displayed_apps[row])


class MainWindow(QMainWindow):
    def __init__(self, settings: QSettings | None = None):
        super().__init__()
        app = QApplication.instance()
        if app:
            app.setPalette(get_dark_palette())
        self.setPalette(get_dark_palette())
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
        self._mirror_worker: threading.Thread | None = None
        self._switch_generation = 0
        self._device_switch_lock = threading.Lock()
        self._reconnect_attempts = 0
        self._gesture_queue: queue.Queue = queue.Queue(maxsize=10)
        self.webview_session = WebViewSession(parent=self)
        self.webview_session.instances_discovered.connect(self._on_webview_instances_discovered)
        self.webview_session.forward_status_changed.connect(self._on_webview_forward_changed)
        self.webview_session.pages_discovered.connect(self._on_webview_pages_discovered)
        self.webview_session.console_message.connect(self._on_webview_console_message)
        self.webview_session.console_entry.connect(self._on_webview_console_entry)
        self.webview_session.screencast_frame.connect(self._on_webview_screencast_frame)
        self.webview_session.eval_result.connect(self._on_webview_eval_result)
        self.webview_session.ws_state_changed.connect(self._on_webview_ws_state_changed)
        self.webview_session.network_entry.connect(self._on_webview_network_entry)
        self._init_webview_log_state()
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
        side.setContentsMargins(12, 20, 12, 16)
        side.setSpacing(8)

        brand_box = QHBoxLayout()
        brand_box.setContentsMargins(2, 0, 2, 0)
        brand_box.setSpacing(8)
        brand_icon = QLabel()
        brand_icon.setPixmap(get_ui_icon("workspace").pixmap(18, 18))
        brand_box.addWidget(brand_icon)
        brand_box.addWidget(label("HarmonyCast", "Brand"))
        brand_box.addStretch()
        side.addLayout(brand_box)
        side.addSpacing(16)

        self.info_nav = button("设备信息", lambda: None, "Navigation", icon_name="device")
        self.mirror_nav = button("投屏工作台", lambda: None, "Navigation", icon_name="mirror")
        self.workspace_nav = self.mirror_nav
        self.hdc_nav = button("常用命令", lambda: None, "Navigation", icon_name="command")

        # 仅将 3 个主模块加入侧边栏：设备信息、投屏工作台、常用命令
        for item in (self.info_nav, self.mirror_nav, self.hdc_nav):
            item.setCheckable(True)
            item.setAutoExclusive(True)
            item.setFixedHeight(38)
            item.setIconSize(QSize(16, 16))
            side.addWidget(item)

        self.info_nav.toggled.connect(lambda selected: self.switch_module(0) if selected else None)
        self.mirror_nav.toggled.connect(lambda selected: self.switch_module(1) if selected else None)
        self.hdc_nav.toggled.connect(lambda selected: self.switch_module(2) if selected else None)

        # 兼容性代理（旧导航引用自动指向投屏工作台对应 Tab，不占用侧边栏位置）
        self.logs_nav = button("日志", lambda: None, "Navigation", icon_name="logs")
        self.logs_nav.setCheckable(True)
        self.logs_nav.toggled.connect(lambda selected: self.switch_module(3) if selected else None)
        self.logs_nav.hide()

        self.webview_nav = button("WebView", lambda: None, "Navigation", icon_name="web")
        self.webview_nav.setCheckable(True)
        self.webview_nav.toggled.connect(lambda selected: self.switch_module(4) if selected else None)
        self.webview_nav.hide()
        side.addStretch()
        bottom_box = QHBoxLayout()
        bottom_box.setContentsMargins(0, 0, 0, 0)
        bottom_box.setSpacing(6)
        version_label = label(f"HarmonyCast\nv{__version__}", "Muted")
        bottom_box.addWidget(version_label, 1)
        self.settings_button = icon_button("settings", self.open_settings, tooltip="系统设置", name="IconBtnSettings", width=30, height=30)
        self.settings_button.setAccessibleName("系统设置")
        bottom_box.addWidget(self.settings_button, 0, Qt.AlignVCenter)
        side.addLayout(bottom_box)
        horizontal.addWidget(self.sidebar)
        self.main_area = QWidget()
        content = QVBoxLayout(self.main_area)
        content.setContentsMargins(0, 0, 0, 0)
        content.setSpacing(0)
        self.header = QFrame()
        self.header.setObjectName("Header")
        heading = QHBoxLayout(self.header)
        heading.setContentsMargins(18, 12, 18, 12)
        heading.setSpacing(8)
        self.page_title = label("设备信息", "Title")
        heading.addWidget(self.page_title)
        heading.addStretch()

        # 工作台布局切换器 (左右分屏 / 上下分屏 / 仅投屏 / 仅日志)
        self.layout_switcher_widget = QWidget()
        self.layout_switcher_widget.setObjectName("LayoutSwitcher")
        self.layout_switcher_widget.setFixedHeight(28)
        self.layout_switcher_widget.setStyleSheet(
            "QWidget#LayoutSwitcher { background: #0c1219; border: 1px solid #1c2b3d; border-radius: 6px; padding: 2px; } "
            "QPushButton { background: transparent; border: none; border-radius: 4px; padding: 2px 7px; color: #7d8f9f; font-size: 12px; } "
            "QPushButton:hover { background: #162436; color: #c9d1d9; } "
            "QPushButton:checked { background: #1f3b60; color: #58a6ff; font-weight: bold; }"
        )
        layout_box = QHBoxLayout(self.layout_switcher_widget)
        layout_box.setContentsMargins(2, 2, 2, 2)
        layout_box.setSpacing(2)

        self.btn_layout_horizontal = QPushButton("左右")
        self.btn_layout_horizontal.setIcon(get_ui_icon("layout_h"))
        self.btn_layout_horizontal.setIconSize(QSize(13, 13))
        self.btn_layout_horizontal.setObjectName("LayoutSwitchBtn")
        self.btn_layout_horizontal.setToolTip("左右分屏 (投屏在左，日志在右，可自由拖拽)")
        self.btn_layout_horizontal.setCheckable(True)
        self.btn_layout_horizontal.setFixedHeight(24)
        self.btn_layout_horizontal.setCursor(Qt.PointingHandCursor)
        self.btn_layout_horizontal.clicked.connect(lambda: self.set_workspace_layout("horizontal"))
        layout_box.addWidget(self.btn_layout_horizontal)

        self.btn_layout_vertical = QPushButton("上下")
        self.btn_layout_vertical.setIcon(get_ui_icon("layout_v"))
        self.btn_layout_vertical.setIconSize(QSize(13, 13))
        self.btn_layout_vertical.setObjectName("LayoutSwitchBtn")
        self.btn_layout_vertical.setToolTip("上下分屏 (投屏在上，日志在下，可自由拖拽)")
        self.btn_layout_vertical.setCheckable(True)
        self.btn_layout_vertical.setFixedHeight(24)
        self.btn_layout_vertical.setCursor(Qt.PointingHandCursor)
        self.btn_layout_vertical.clicked.connect(lambda: self.set_workspace_layout("vertical"))
        layout_box.addWidget(self.btn_layout_vertical)

        self.btn_layout_screen_only = QPushButton("投屏")
        self.btn_layout_screen_only.setIcon(get_ui_icon("layout_screen"))
        self.btn_layout_screen_only.setIconSize(QSize(13, 13))
        self.btn_layout_screen_only.setObjectName("LayoutSwitchBtn")
        self.btn_layout_screen_only.setToolTip("仅投屏模式")
        self.btn_layout_screen_only.setCheckable(True)
        self.btn_layout_screen_only.setFixedHeight(24)
        self.btn_layout_screen_only.setCursor(Qt.PointingHandCursor)
        self.btn_layout_screen_only.clicked.connect(lambda: self.set_workspace_layout("mirror_only"))
        layout_box.addWidget(self.btn_layout_screen_only)

        self.btn_layout_logs_only = QPushButton("日志")
        self.btn_layout_logs_only.setIcon(get_ui_icon("layout_logs"))
        self.btn_layout_logs_only.setIconSize(QSize(13, 13))
        self.btn_layout_logs_only.setObjectName("LayoutSwitchBtn")
        self.btn_layout_logs_only.setToolTip("仅日志模式")
        self.btn_layout_logs_only.setCheckable(True)
        self.btn_layout_logs_only.setFixedHeight(24)
        self.btn_layout_logs_only.setCursor(Qt.PointingHandCursor)
        self.btn_layout_logs_only.clicked.connect(lambda: self.set_workspace_layout("logs_only"))
        layout_box.addWidget(self.btn_layout_logs_only)

        heading.addWidget(self.layout_switcher_widget)
        heading.addSpacing(4)

        self.device_list = QComboBox()
        self.device_list.setMinimumWidth(140)
        self.device_list.setMaximumWidth(280)
        self.device_list.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.device_list.setFixedHeight(28)
        self.device_list.setAccessibleName("选择设备")
        setup_combobox_popup(self.device_list, 180)
        self.device_list.currentIndexChanged.connect(self.device_changed)
        heading.addWidget(self.device_list)
        self.refresh_button = button("刷新", self.refresh_devices, icon_name="refresh")
        self.refresh_button.setFixedHeight(28)
        self.refresh_button.setShortcut("F5")
        heading.addWidget(self.refresh_button)
        self.reconnect_button = button("断开", self.disconnect_current_device, "Danger", icon_name="disconnect")
        self.reconnect_button.setFixedHeight(28)
        self.reconnect_button.setToolTip("断开当前设备连接 (网络设备执行 hdc tconn xx:8710 -remove 并停止投屏)")
        self.disconnect_button = self.reconnect_button
        heading.addWidget(self.disconnect_button)
        self.network_button = button("设备中心", self.connect_network, icon_name="hub")
        self.network_button.setFixedHeight(28)
        self.network_button.setToolTip("设备维护中心：查看在线 targets、快速连接与断开管理 (固定端口 8710)")
        self.device_hub_button = self.network_button
        heading.addWidget(self.network_button)
        content.addWidget(self.header)
        self.pages = WorkspaceStackedWidget(self)
        self.info_panel = self._build_info_panel()
        self.pages.addWidget(self.info_panel)
        self.mirror_page = QWidget()
        self.mirror_page.setMinimumSize(80, 80)
        self.mirror_page.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        mirror = QVBoxLayout(self.mirror_page)
        mirror.setContentsMargins(0, 0, 0, 0)
        mirror.setSpacing(0)

        # 1. 投屏操作栏置于模块上方显示 (采用自适应流式多行换行布局)
        self.mirror_toolbar = QFrame()
        self.mirror_toolbar.setObjectName("MirrorToolbar")
        self.mirror_toolbar.setMinimumHeight(40)
        self.mirror_toolbar.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Minimum)
        controls = FlowLayout(self.mirror_toolbar, margin=6, hspacing=5, vspacing=5)

        self.start_button = button("开始投屏", self.start_mirror, "Primary", icon_name="play")
        self.start_button.setFixedHeight(28)
        self.start_button.setShortcut("Ctrl+Return")
        self.stop_button = button("停止投屏", lambda: self.stop_mirror(user_initiated=True), "Danger", icon_name="stop")
        self.stop_button.setFixedHeight(28)
        self.stop_button.hide()
        controls.addWidget(self.start_button)
        controls.addWidget(self.stop_button)

        self.quality_combo = QComboBox()
        self.quality_combo.addItems(["标准 (4M)", "流畅 (1.5M)", "超清 (8M)"])
        self.quality_combo.setToolTip("投屏画质：1080P 完整全屏，支持动态调节视频流码率")
        self.quality_combo.setFixedHeight(28)
        self.quality_combo.setFixedWidth(112)
        setup_combobox_popup(self.quality_combo, 112)
        bitrate_idx = int(self.settings.value("bitrate", 1))
        combo_idx = {1: 0, 0: 1, 2: 2}.get(bitrate_idx, 0)
        self.quality_combo.setCurrentIndex(combo_idx)
        self.quality_combo.currentIndexChanged.connect(self.on_quality_changed)
        controls.addWidget(self.quality_combo)

        self.rotate_button = button("旋转", self.rotate_display, icon_name="rotate")
        self.rotate_button.setFixedHeight(28)
        self.rotate_button.setShortcut("Ctrl+R")
        self.rotate_button.setToolTip("旋转画面 0° / 90° / 180° / 270° (快捷键: ⌘R)")
        controls.addWidget(self.rotate_button)

        self.screenshot_button = button("截图", self.save_screenshot, icon_name="camera")
        self.screenshot_button.setFixedHeight(28)
        self.screenshot_button.setShortcut("Ctrl+Shift+S")
        self.screenshot_button.setToolTip("保存高清截图 (快捷键: ⌘⇧S)")
        controls.addWidget(self.screenshot_button)

        # 核心快捷操作独立按钮 (返回、主页、主程序、打包诊断，简化文案并直接展示)
        self.btn_key_back = button("返回", lambda: self.send_key("back"), icon_name="back")
        self.btn_key_back.setFixedHeight(28)
        self.btn_key_back.setToolTip("返回键 (Back, 快捷键: Alt+Left)")
        controls.addWidget(self.btn_key_back)

        self.btn_key_home = button("主页", lambda: self.send_key("home"), icon_name="home")
        self.btn_key_home.setFixedHeight(28)
        self.btn_key_home.setToolTip("回到主屏幕 (Home, 快捷键: Alt+Home)")
        controls.addWidget(self.btn_key_home)

        self.restore_primary_button = button("主程序", self.launch_primary_app_shortcut, icon_name="app")
        self.restore_primary_button.setFixedHeight(28)
        self.restore_primary_button.setToolTip("拉起业务主程序")
        controls.addWidget(self.restore_primary_button)

        self.fault_report_button = button("打包诊断", self.export_fault_report, icon_name="package")
        self.fault_report_button.setFixedHeight(28)
        self.fault_report_button.setToolTip("打包导出诊断现场数据包")
        controls.addWidget(self.fault_report_button)

        # 快捷操作菜单隐藏保留 (保证现有测试与向下兼容)
        self.btn_device_ops = QPushButton("操作 ▾")
        self.btn_device_ops.setObjectName("Secondary")
        self.btn_device_ops.setFixedHeight(28)
        self.btn_device_ops.setIcon(get_ui_icon("more"))
        self.btn_device_ops.setIconSize(QSize(14, 14))
        self.btn_device_ops.setToolTip("常用设备操作与快捷菜单")
        self.btn_device_ops.setCursor(Qt.PointingHandCursor)
        ops_menu = QMenu(self.btn_device_ops)
        ops_menu.addAction(get_ui_icon("back"), "返回键 (Back)", lambda: self.send_key("back"))
        ops_menu.addAction(get_ui_icon("home"), "主屏幕 (Home)", lambda: self.send_key("home"))
        ops_menu.addSeparator()
        ops_menu.addAction(get_ui_icon("app"), "拉起业务主程序", self.launch_primary_app_shortcut)
        ops_menu.addAction(get_ui_icon("package"), "打包诊断现场", self.export_fault_report)
        self.btn_device_ops.setMenu(ops_menu)
        self.btn_device_ops.hide()
        controls.addWidget(self.btn_device_ops)

        self.launch_app_button = button("拉起应用", self.open_app_launcher)
        self.launch_app_button.hide()

        self.key_buttons = [self.rotate_button, self.launch_app_button, self.restore_primary_button, self.btn_key_back, self.btn_key_home]

        self.fullscreen_button = button("全屏", self.toggle_fullscreen, icon_name="fullscreen")
        self.fullscreen_button.setFixedHeight(28)
        self.fullscreen_button.setToolTip("全屏演示模式 (F11)")
        controls.addWidget(self.fullscreen_button)

        self.state_badge = label("待连接", "Muted")
        self.state_badge.setFixedHeight(28)
        self.state_badge.setAlignment(Qt.AlignCenter)
        controls.addWidget(self.state_badge)

        self.frame_info = label("F11 全屏", "Muted")
        self.frame_info.hide()

        mirror.addWidget(self.mirror_toolbar)

        # 2. 投屏显示画面置于下方
        self.screen = ScreenView()
        self.screen.user_rotation = self.user_rotation
        self.screen.gesture.connect(self.send_gesture)
        self.screen.fullscreen_requested.connect(self.toggle_fullscreen)
        self.screen.back_requested.connect(lambda: self.send_key("back"))
        self.screen.start_requested.connect(self._on_screen_clicked_to_start)
        self.screen.show_touches = bool(int(self.settings.value("show_touches", 1)))
        mirror.addWidget(self.screen, 1)

        self.hdc_panel = self._build_hdc_panel()

        # 统一日志中心 (支持 Tab 切换: 设备日志 HiLog / WebView 控制台)
        self.log_panel = QWidget()
        self.log_panel.setObjectName("LogPanel")
        self.log_panel.setMinimumSize(80, 80)
        self.log_panel.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        log_panel_layout = QVBoxLayout(self.log_panel)
        log_panel_layout.setContentsMargins(6, 6, 6, 6)
        log_panel_layout.setSpacing(4)

        log_tab_bar = QHBoxLayout()
        log_tab_bar.setContentsMargins(0, 0, 0, 0)
        log_tab_bar.setSpacing(4)

        self.btn_log_tab_device = QPushButton("设备日志")
        self.btn_log_tab_device.setIcon(get_ui_icon("logs"))
        self.btn_log_tab_device.setIconSize(QSize(14, 14))
        self.btn_log_tab_device.setObjectName("LogSubTab")
        self.btn_log_tab_device.setCheckable(True)
        self.btn_log_tab_device.setChecked(True)
        self.btn_log_tab_device.setFixedHeight(28)
        self.btn_log_tab_device.setCursor(Qt.PointingHandCursor)
        self.btn_log_tab_device.clicked.connect(lambda: self.switch_log_subtab(0))
        log_tab_bar.addWidget(self.btn_log_tab_device)

        self.btn_log_tab_webview_console = QPushButton("WebView 控制台")
        self.btn_log_tab_webview_console.setIcon(get_ui_icon("web"))
        self.btn_log_tab_webview_console.setIconSize(QSize(14, 14))
        self.btn_log_tab_webview_console.setObjectName("LogSubTab")
        self.btn_log_tab_webview_console.setCheckable(True)
        self.btn_log_tab_webview_console.setFixedHeight(28)
        self.btn_log_tab_webview_console.setCursor(Qt.PointingHandCursor)
        self.btn_log_tab_webview_console.clicked.connect(lambda: self.switch_log_subtab(1))
        log_tab_bar.addWidget(self.btn_log_tab_webview_console)

        self.btn_log_tab_webview_network = QPushButton("接口网络")
        self.btn_log_tab_webview_network.setObjectName("LogSubTab")
        self.btn_log_tab_webview_network.setCheckable(True)
        self.btn_log_tab_webview_network.setFixedHeight(26)
        self.btn_log_tab_webview_network.setCursor(Qt.PointingHandCursor)
        self.btn_log_tab_webview_network.clicked.connect(lambda: self.switch_log_subtab(2))
        self.btn_log_tab_webview_network.hide()

        log_tab_bar.addStretch(1)
        log_panel_layout.addLayout(log_tab_bar)

        self.log_subtab_stack = QStackedWidget()
        self.hilog_widget = self._build_hilog_widget()
        self.log_subtab_stack.addWidget(self.hilog_widget)

        self.webview_panel = self._build_webview_panel()
        self.log_subtab_stack.addWidget(self.webview_panel)

        log_panel_layout.addWidget(self.log_subtab_stack, 1)

        # 核心工作台页面: 同屏容纳【主投屏】与【统一日志】模块
        self.workspace_page = QWidget()
        ws_layout = QVBoxLayout(self.workspace_page)
        ws_layout.setContentsMargins(0, 0, 0, 0)
        ws_layout.setSpacing(0)

        self.workspace_splitter = QSplitter(Qt.Horizontal)
        self.workspace_splitter.setObjectName("WorkspaceSplitter")
        self.workspace_splitter.setHandleWidth(8)
        self.workspace_splitter.setChildrenCollapsible(False)
        self.workspace_splitter.addWidget(self.mirror_page)
        self.workspace_splitter.addWidget(self.log_panel)
        self.workspace_splitter.setStretchFactor(0, 5)
        self.workspace_splitter.setStretchFactor(1, 5)

        handle = self.workspace_splitter.handle(1)
        if handle:
            handle.setCursor(Qt.SplitHCursor)
        self.workspace_splitter.splitterMoved.connect(self._on_workspace_splitter_moved)

        ws_layout.addWidget(self.workspace_splitter, 1)

        self.pages.addWidget(self.workspace_page)
        self.pages.addWidget(self.hdc_panel)
        content.addWidget(self.pages, 1)

        self.current_workspace_layout = "horizontal"
        saved_layout = str(self.settings.value("workspace/layout", "horizontal"))
        self.set_workspace_layout(saved_layout)
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
            setup_combobox_popup(option, 120)
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
        self.escape_shortcut.activated.connect(self.on_escape_pressed)
        self.back_alt_left = QShortcut(QKeySequence("Alt+Left"), self)
        self.back_alt_left.activated.connect(lambda: self.send_key("back"))
        self.home_alt_home = QShortcut(QKeySequence("Alt+Home"), self)
        self.home_alt_home.activated.connect(lambda: self.send_key("home"))
        self.statusBar().showMessage("连接设备后查看基本信息；点击「投屏」开始投屏")
        self.switch_module(0)
        self.update_actions()
        self._update_responsive_toolbars()

    def _on_workspace_splitter_moved(self, pos: int, index: int):
        sizes = self.workspace_splitter.sizes()
        if len(sizes) == 2 and sizes[0] > 0 and sizes[1] > 0:
            if self.workspace_splitter.orientation() == Qt.Horizontal:
                self.settings.setValue("workspace/splitter_h", f"{sizes[0]},{sizes[1]}")
            else:
                self.settings.setValue("workspace/splitter_v", f"{sizes[0]},{sizes[1]}")
        self._update_responsive_toolbars()

    def on_escape_pressed(self):
        if self.isFullScreen():
            self.exit_fullscreen()
        elif self.projecting:
            self.send_key("back")

    def _on_screen_clicked_to_start(self):
        if not self.selected_device():
            self.refresh_devices()
        elif not self.projecting:
            self.start_mirror()

    def wakeup_current_device(self):
        device = self.selected_device()
        if not device or not self.hdc:
            return
        tools = DeviceTools(self.hdc)
        def do_wake():
            tools.wakeup(device.serial)
            return "ok"
        def on_done(_):
            self.statusBar().showMessage(f"已唤醒屏幕：{device.serial}", 2500)
        self.submit(do_wake, on_done, "唤醒屏幕", alert=False)

    def set_workspace_layout(self, mode: str):
        """设置工作台布局模式: horizontal (左右分屏), vertical (上下分屏), mirror_only (仅投屏), logs_only (仅日志)"""
        self.current_workspace_layout = mode
        self.settings.setValue("workspace/layout", mode)

        if hasattr(self, "btn_layout_horizontal"):
            self.btn_layout_horizontal.setChecked(mode == "horizontal")
            self.btn_layout_vertical.setChecked(mode == "vertical")
            self.btn_layout_screen_only.setChecked(mode == "mirror_only")
            self.btn_layout_logs_only.setChecked(mode == "logs_only")

        if mode == "horizontal":
            self.workspace_splitter.setOrientation(Qt.Horizontal)
            self.mirror_page.show()
            self.log_panel.show()
            saved_h = str(self.settings.value("workspace/splitter_h", ""))
            applied = False
            if saved_h and "," in saved_h:
                try:
                    s0, s1 = [int(x) for x in saved_h.split(",")[:2]]
                    if s0 > 50 and s1 > 50:
                        self.workspace_splitter.setSizes([s0, s1])
                        applied = True
                except Exception:
                    pass
            if not applied:
                w = self.workspace_splitter.width() or 1000
                self.workspace_splitter.setSizes([int(w * 0.45), int(w * 0.55)])
            handle = self.workspace_splitter.handle(1)
            if handle:
                handle.setCursor(Qt.SplitHCursor)
            if getattr(self, "_mirror_suspended", False) and not self.projecting and self.selected_device() and not getattr(self, "_user_stopped_mirror", False):
                self._mirror_suspended = False
                self.start_mirror()
        elif mode == "vertical":
            self.workspace_splitter.setOrientation(Qt.Vertical)
            self.mirror_page.show()
            self.log_panel.show()
            saved_v = str(self.settings.value("workspace/splitter_v", ""))
            applied = False
            if saved_v and "," in saved_v:
                try:
                    s0, s1 = [int(x) for x in saved_v.split(",")[:2]]
                    if s0 > 50 and s1 > 50:
                        self.workspace_splitter.setSizes([s0, s1])
                        applied = True
                except Exception:
                    pass
            if not applied:
                h = self.workspace_splitter.height() or 800
                self.workspace_splitter.setSizes([int(h * 0.5), int(h * 0.5)])
            handle = self.workspace_splitter.handle(1)
            if handle:
                handle.setCursor(Qt.SplitVCursor)
            if getattr(self, "_mirror_suspended", False) and not self.projecting and self.selected_device() and not getattr(self, "_user_stopped_mirror", False):
                self._mirror_suspended = False
                self.start_mirror()
        elif mode == "mirror_only":
            self.mirror_page.show()
            self.log_panel.hide()
            if getattr(self, "_mirror_suspended", False) and not self.projecting and self.selected_device() and not getattr(self, "_user_stopped_mirror", False):
                self._mirror_suspended = False
                self.start_mirror()
        elif mode == "logs_only":
            self.mirror_page.hide()
            self.log_panel.show()
            if self.projecting:
                self._mirror_suspended = True
                self.stop_mirror(user_initiated=False)
        self._update_responsive_toolbars()

    def switch_log_subtab(self, index: int, auto_connect: bool = True):
        """切换日志模块内的标签页: 0: 设备日志 (HiLog), 1: WebView 控制台, 2: WebView 接口网络"""
        self.current_log_subtab = index
        if hasattr(self, "btn_log_tab_device"):
            self.btn_log_tab_device.setChecked(index == 0)
            self.btn_log_tab_webview_console.setChecked(index in (1, 2))
            if hasattr(self, "btn_log_tab_webview_network"):
                self.btn_log_tab_webview_network.setChecked(index == 2)

        if hasattr(self, "log_subtab_stack"):
            if index == 0:
                self.log_subtab_stack.setCurrentWidget(self.hilog_widget)
            else:
                self.log_subtab_stack.setCurrentWidget(self.webview_panel)
                if hasattr(self, "webview_right_stack"):
                    self._set_webview_subtab(0 if index == 1 else 1)
                if auto_connect and index in (1, 2):
                    QTimer.singleShot(60, self._auto_match_and_connect_webview)
        self.sync_hilog()
        self._update_responsive_toolbars()

    def _auto_match_and_connect_webview(self):
        """自动匹配并连接当前设备的活跃 WebView 页面；若无可用页面则提示并返回设备日志"""
        if getattr(self, "closing", False) or getattr(self, "current_log_subtab", 0) == 0:
            return
        device = self.selected_device()
        if not device or not self.hdc:
            self.switch_log_subtab(0, auto_connect=False)
            self.show_toast("当前设备未连接或无可用 Web 页面", level="warning")
            return

        if hasattr(self, "webview_session") and getattr(self.webview_session, "connected", False):
            return

        self._webview_automatch_gen = getattr(self, "_webview_automatch_gen", 0) + 1
        current_gen = self._webview_automatch_gen

        def worker():
            if getattr(self, "closing", False):
                return False, []
            # 1. 若当前端口转发已处于开启状态，先快速拉取页面
            if getattr(self.webview_session, "forwarded", False):
                pages = self.webview_session.fetch_pages()
                if pages:
                    return True, pages

            # 2. 扫描设备上的 Unix domain sockets
            instances = self.webview_session.scan_instances(self.hdc, device.serial)
            targets = []
            for inst in instances:
                t = inst.get("target") or inst.get("socket")
                if t and t not in targets:
                    targets.append(t)
            if "tcp:9222" not in targets:
                targets.append("tcp:9222")

            for target in targets:
                if current_gen != getattr(self, "_webview_automatch_gen", 0) or getattr(self, "closing", False):
                    return False, []
                ok, _ = self.webview_session.start_forward(self.hdc, device.serial, target_or_pid=target, port=DEFAULT_DEVTOOLS_PORT)
                if ok:
                    pages = self.webview_session.fetch_pages()
                    if pages:
                        return True, pages

            if getattr(self, "closing", False):
                return False, []
            self.webview_session.stop_forward()
            return False, []

        def on_done(result):
            if current_gen != getattr(self, "_webview_automatch_gen", 0) or getattr(self, "closing", False):
                return
            success, pages = result
            if success and pages:
                self.webview_page_combo.blockSignals(True)
                self.webview_page_combo.clear()
                for p in pages:
                    title = p.get("title") or "未命名网页"
                    url = p.get("url", "")
                    short_url = url.split("?")[0]
                    self.webview_page_combo.addItem(f"{title} ({short_url})", p)
                self.webview_page_combo.blockSignals(False)
                self.btn_webview_forward.setText("断开")
                self.btn_webview_forward.setObjectName("Danger")
                self.btn_webview_forward.setStyle(self.btn_webview_forward.style())
                self._on_webview_page_selected(0)
                first_title = pages[0].get("title") or "未命名网页"
                self.statusBar().showMessage(f"已自动匹配连接: {first_title}", 3000)
            else:
                self.switch_log_subtab(0, auto_connect=False)
                self.statusBar().showMessage("当前设备无可用 Web 页面", 3000)
                self.show_toast("当前设备未检测到运行中的 WebView 界面", level="warning", duration_ms=3500)

        self.submit(worker, on_done, "自动匹配 WebView 页面", alert=False)

    def is_log_active(self) -> bool:
        """判断当前日志模块是否处于激活可见状态（支持同屏工作台及独立日志视图）"""
        if not hasattr(self, "pages") or not hasattr(self, "log_panel"):
            return False
        current = self.pages.currentWidget()
        if current is getattr(self, "log_panel", None):
            return True
        if current is getattr(self, "workspace_page", None) and self.log_panel.isVisible():
            return getattr(self, "current_log_subtab", 0) == 0
        return False

    def _build_hilog_widget(self) -> QWidget:
        """构建 HiLog 设备日志视图"""
        widget = QWidget()
        log_layout = QVBoxLayout(widget)
        log_layout.setContentsMargins(0, 2, 0, 2)
        log_layout.setSpacing(4)

        # 日志分类隐藏保留（供包名联动及测试兼容，界面上不显示）
        self.category = QComboBox()
        self.category.setAccessibleName("日志分类")
        self.category.addItem("全部日志", "")
        self.category.addItem("应用日志", "app")
        self.category.addItem("系统日志", "system")
        saved_category = self.category.findData(str(self.settings.value("logs/category", "app")))
        self.category.setCurrentIndex(max(0, saved_category))
        self.category.currentIndexChanged.connect(self.log_category_changed)
        self.category.setFixedHeight(26)
        setup_combobox_popup(self.category, 100)
        self.category.hide()

        # 自适应多行流式控制台工具栏 (当空间不足时按钮自动换行，包名紧凑，优先保证搜索框宽度)
        toolbar = FlowLayout(margin=0, hspacing=6, vspacing=4)

        # 1. 应用包名 (紧凑宽度适度收敛，弹窗下拉补全宽裕)
        self.package_row = QWidget()
        self.package_row.setFixedWidth(135)
        self.package_row.setFixedHeight(28)
        package_layout = QHBoxLayout(self.package_row)
        package_layout.setContentsMargins(0, 0, 0, 0)
        package_layout.setSpacing(0)
        self.package_combo = QComboBox()
        self.package_combo.setAccessibleName("应用包名")
        self.package_combo.setEditable(True)
        self.package_combo.setMaxVisibleItems(12)
        self.package_combo.setInsertPolicy(QComboBox.NoInsert)
        self.package_combo.addItems([DEFAULT_PACKAGE_FILTER, "全部应用"])
        self.package_combo.lineEdit().setPlaceholderText("包名或前缀…")
        self.package_combo.completer().setCompletionMode(QCompleter.PopupCompletion)
        self.package_combo.completer().setFilterMode(Qt.MatchContains)
        self.package_combo.completer().setCaseSensitivity(Qt.CaseInsensitive)
        setup_combobox_popup(self.package_combo, 260)
        if self.package_combo.completer() and self.package_combo.completer().popup():
            self.package_combo.completer().popup().setStyleSheet("background-color: #141f2c; color: #e7edf4; border: 1px solid #304257; border-radius: 6px; selection-background-color: #1b3e35; outline: none; padding: 2px;")
            self.package_combo.completer().popup().setMinimumWidth(260)
        self.package_combo.activated.connect(self.commit_package)
        self.package_combo.lineEdit().editingFinished.connect(self.commit_package)
        self.package_combo.setFixedWidth(135)
        self.package_combo.setFixedHeight(28)
        package_layout.addWidget(self.package_combo)
        self.package_hint = label("应用列表自动更新", "Muted")
        self.package_hint.hide()
        self.package_row.setVisible(self.category.currentData() == "app")
        toolbar.addWidget(self.package_row)

        # 2. 级别选择 (紧凑筛选下拉)
        self.level = QComboBox()
        self.level.addItems(["全部级别", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"])
        self.level.setFixedHeight(28)
        self.level.setFixedWidth(96)
        setup_combobox_popup(self.level, 120)
        self.level.currentIndexChanged.connect(self.render_logs)
        toolbar.addWidget(self.level)

        # 3. 搜索框 (弹性自适应拉伸，优先保证充裕宽度)
        self.search = QLineEdit()
        self.search.setPlaceholderText("搜索日志内容、Tag 或 PID…")
        self.search.addAction(get_ui_icon("search"), QLineEdit.LeadingPosition)
        self.search.setAccessibleName("搜索日志")
        self.search.setMinimumWidth(180)
        self.search.setFixedHeight(28)
        self.search.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.search.textChanged.connect(self.render_logs)
        toolbar.addWidget(self.search)

        # 4. 自动滚动与暂停
        self.autoscroll = QCheckBox("滚动")
        self.autoscroll.setChecked(True)
        self.autoscroll.setToolTip("新日志到达时自动滚动到底部")
        self.autoscroll.setFixedHeight(28)
        self.autoscroll.toggled.connect(self._on_autoscroll_toggled)
        self.pause = QCheckBox("暂停")
        self.pause.setToolTip("暂停日志刷新显示")
        self.pause.setFixedHeight(28)
        self.pause.toggled.connect(self.render_logs)
        toolbar.addWidget(self.autoscroll)
        toolbar.addWidget(self.pause)

        # 5. 操作按钮: 清空、导出日志
        self.clear_button = button("清空", self.clear_log_view, icon_name="clear")
        self.clear_button.setFixedHeight(28)
        self.export_button = button("导出日志", self.export_logs, icon_name="export")
        self.export_button.setFixedHeight(28)
        toolbar.addWidget(self.clear_button)
        toolbar.addWidget(self.export_button)

        # 保留辅助属性以确保兼容
        self.fetch_button = button("拉取投屏服务日志", self.fetch_logs)
        self.fetch_button.hide()
        self.source = QComboBox(self)
        self.source.addItems(["全部来源", "HiLog", "工具"])
        setup_combobox_popup(self.source, 100)
        self.source.currentIndexChanged.connect(self.render_logs)
        self.source.hide()

        log_layout.addLayout(toolbar)
        self.log_view = LogView()
        self.log_view.setReadOnly(True)
        self.log_view.setAccessibleName("实时日志内容")
        self.log_view.setFont(QFont("Menlo", 11))
        self.log_view.setLineWrapMode(QTextEdit.NoWrap)
        self.log_view.document().setMaximumBlockCount(10000)
        self.log_view.clear_requested.connect(self.clear_log_view)
        QShortcut(QKeySequence("Ctrl+K"), self.log_view, self.clear_log_view)
        self.log_highlighter = LogHighlighter(self.log_view.document())
        log_layout.addWidget(self.log_view, 1)
        # 底部状态栏: 计数统计 + 字号调节 (采集中状态隐藏)
        log_footer = QHBoxLayout()
        log_footer.setContentsMargins(0, 0, 0, 0)
        log_footer.setSpacing(8)
        self.log_state = label("连接设备后自动采集", "Badge")
        self.log_state.hide()
        self.log_count = label("0 条日志", "Muted")
        log_footer.addWidget(self.log_count)
        log_footer.addStretch(1)
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
        return widget

    def switch_module(self, index):
        if isinstance(index, str):
            mapping = {
                "info": 0, "device": 0,
                "mirror": 1, "screen": 1, "workspace": 1,
                "hdc": 2, "cmd": 2, "commands": 2, "command": 2,
                "logs": 3, "log": 3, "hilog": 3,
                "webview": 4, "web": 4,
            }
            index = mapping.get(index.lower(), 0)

        if self.isFullScreen():
            self.exit_fullscreen()

        self.current_nav_module = index

        self.info_nav.setChecked(index == 0)
        self.mirror_nav.setChecked(index in (1, 3, 4))
        self.hdc_nav.setChecked(index == 2)
        if hasattr(self, "logs_nav") and self.logs_nav is not self.mirror_nav:
            self.logs_nav.setChecked(index == 3)
        if hasattr(self, "webview_nav") and self.webview_nav is not self.mirror_nav:
            self.webview_nav.setChecked(index == 4)

        titles = {
            0: "设备信息",
            1: "投屏工作台",
            2: "常用命令",
            3: "投屏工作台",
            4: "投屏工作台"
        }
        self.page_title.setText(titles.get(index, "设备信息"))

        if hasattr(self, "log_font_shortcuts"):
            for shortcut in self.log_font_shortcuts:
                shortcut.setEnabled(index == 3)

        if index != 1 and self.projecting:
            self._mirror_suspended = True
            self.stop_mirror(user_initiated=False)

        if index == 0:
            self.pages.setCurrentWidget(self.info_panel)
            if hasattr(self, "layout_switcher_widget"):
                self.layout_switcher_widget.hide()
            self.refresh_device_info()
        elif index == 2:
            self.pages.setCurrentWidget(self.hdc_panel)
            if hasattr(self, "layout_switcher_widget"):
                self.layout_switcher_widget.hide()
            self.refresh_hdc_target_label()
        else:
            # 1 (投屏), 3 (日志), 4 (WebView) 统一在同屏工作台中展示
            self.pages.setCurrentWidget(self.workspace_page)
            if hasattr(self, "layout_switcher_widget"):
                self.layout_switcher_widget.show()

            if index == 1:
                # 投屏入口: 确保投屏面板可见
                if getattr(self, "current_workspace_layout", "") == "logs_only":
                    self.set_workspace_layout("mirror_only")
                else:
                    self.mirror_page.show()
                should_resume = getattr(self, "_mirror_suspended", False)
                self._mirror_suspended = False
                if (should_resume or self.auto_start_mirror) and not self.projecting and self.selected_device() and not getattr(self, "_user_stopped_mirror", False):
                    self.start_mirror()
                self.screen.update()
            elif index == 3:
                # 日志入口: 切换至设备日志 (HiLog)
                self.switch_log_subtab(0)
                if getattr(self, "current_workspace_layout", "") not in ("horizontal", "vertical"):
                    self.set_workspace_layout("logs_only")
                else:
                    self.log_panel.show()
                self.render_logs()
                self.refresh_packages()
            elif index == 4:
                # WebView 入口: 切换至 WebView 控制台
                self.switch_log_subtab(1)
                if getattr(self, "current_workspace_layout", "") not in ("horizontal", "vertical"):
                    self.set_workspace_layout("logs_only")
                else:
                    self.log_panel.show()
                self.fetch_webview_pages_action(show_toast_if_empty=True)

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

        # 0. 未连接状态提示卡片
        self.card_empty = QFrame()
        self.card_empty.setObjectName("InfoCard")
        self.empty_layout = QVBoxLayout(self.card_empty)
        self.empty_layout.setContentsMargins(20, 20, 20, 20)
        self.empty_layout.setSpacing(14)
        self.info_cards_layout.addWidget(self.card_empty)

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

    def _render_empty_info(self, hint: str = "未检测到已连接的设备"):
        self.info_badge.setText("未连接设备")
        self.info_badge.setStyleSheet("color: #ff9f9f; background: #2b1419; border: 1px solid #572932; padding: 4px 10px; border-radius: 9px; font-size: 12px;")
        self.info_device_label.setText(hint)
        if hasattr(self, "btn_refresh_info"):
            self.btn_refresh_info.setEnabled(False)
        if hasattr(self, "btn_copy_all_info"):
            self.btn_copy_all_info.setEnabled(False)

        _clear_layout(self.yarward_layout)
        _clear_layout(self.icu_layout)
        _clear_layout(self.network_layout)
        _clear_layout(self.device_layout)
        _clear_layout(self.packages_layout)

        if hasattr(self, "card_yarward"):
            self.card_yarward.hide()
            self.card_icu.hide()
            self.card_network.hide()
            self.card_device.hide()
            self.card_packages.hide()

        if hasattr(self, "card_empty"):
            self.card_empty.show()
            _clear_layout(self.empty_layout)

            hdr = QVBoxLayout()
            hdr.setSpacing(6)
            title = label("未检测到已连接的设备", "CardTitleWarn")
            title.setStyleSheet("font-size: 17px; font-weight: 600; color: #f3c969;")
            sub = label("当前没有处于在线调试状态的 OpenHarmony / 鸿蒙设备。请先连接设备以查看硬件详情与应用信息。", "Muted")
            sub.setStyleSheet("font-size: 13px; color: #8ba2b9;")
            hdr.addWidget(title)
            hdr.addWidget(sub)
            self.empty_layout.addLayout(hdr)

            guide_box = QHBoxLayout()
            guide_box.setSpacing(14)

            usb_card = QFrame()
            usb_card.setObjectName("AppBox")
            u_layout = QVBoxLayout(usb_card)
            u_layout.setContentsMargins(16, 14, 16, 14)
            u_layout.setSpacing(8)
            u_title = label("🔌 USB 有线连接指引", "Brand")
            u_title.setStyleSheet("font-size: 14px; font-weight: bold; color: #79e6c9;")
            u_layout.addWidget(u_title)
            u_steps = (
                "1. 使用 USB 数据线连接开发板或真机至电脑\n"
                "2. 在设备上开启「开发者选项」与「USB 调试」\n"
                "3. 屏幕弹窗勾选「始终允许此电脑调试」并确认\n"
                "4. 点击下方「刷新设备列表」按钮或按 F5"
            )
            u_desc = label(u_steps, "Muted")
            u_desc.setStyleSheet("line-height: 160%; color: #9bb0c3; font-size: 12px;")
            u_layout.addWidget(u_desc)
            guide_box.addWidget(usb_card, 1)

            net_card = QFrame()
            net_card.setObjectName("AppBox")
            n_layout = QVBoxLayout(net_card)
            n_layout.setContentsMargins(16, 14, 16, 14)
            n_layout.setSpacing(8)
            n_title = label("🌐 网络调试连接指引", "Brand")
            n_title.setStyleSheet("font-size: 14px; font-weight: bold; color: #58a6ff;")
            n_layout.addWidget(n_title)
            n_steps = (
                "1. 确保设备与电脑接入同一局域网或直连网口\n"
                "2. 确认设备的固定 IP 地址（默认调试端口 8710）\n"
                "3. 点击右上角或下方「设备中心」\n"
                "4. 输入目标 IP 点击连接即可自动同步"
            )
            n_desc = label(n_steps, "Muted")
            n_desc.setStyleSheet("line-height: 160%; color: #9bb0c3; font-size: 12px;")
            n_layout.addWidget(n_desc)
            guide_box.addWidget(net_card, 1)

            self.empty_layout.addLayout(guide_box)

            btn_bar = QHBoxLayout()
            btn_bar.setSpacing(10)
            btn_refresh = button("刷新设备列表 (F5)", self.refresh_devices, "Primary", icon_name="refresh")
            btn_refresh.setFixedHeight(30)
            btn_hub = button("打开设备中心", self.connect_network, icon_name="hub")
            btn_hub.setFixedHeight(30)
            btn_bar.addWidget(btn_refresh)
            btn_bar.addWidget(btn_hub)
            btn_bar.addStretch()
            self.empty_layout.addLayout(btn_bar)

    def refresh_device_info(self):
        device = self.selected_device()
        if device is None or self.hdc is None or not self.is_device_connected(device):
            hint = f"设备 {device.serial} 尚未连接" if device else "未检测到已连接的设备"
            self._render_empty_info(hint)
            return
        if self.info_loading:
            return
        self.info_loading = True
        self.info_badge.setText("正在查询…")
        self.info_badge.setStyleSheet("color: #f3c969; background: #3d2f10; border: 1px solid #735515; padding: 4px 10px; border-radius: 9px; font-size: 12px;")
        self.btn_refresh_info.setEnabled(False)
        self.statusBar().showMessage("正在获取设备基本信息及应用版本…")
        serial = device.serial
        tools = DeviceTools(self.hdc)

        def query():
            return tools.get_device_details(serial)

        def on_done(result):
            self.info_loading = False
            cur = self.selected_device()
            connected = bool(cur and self.is_device_connected(cur))
            if hasattr(self, "btn_refresh_info"):
                self.btn_refresh_info.setEnabled(connected)
            if self.closing or not connected or cur.serial != serial:
                return
            self.render_device_info(result)
            self.statusBar().showMessage(f"设备信息已更新 ({serial[-6:]})", 3000)

        self.submit(query, on_done, "获取设备信息", alert=False)

    def render_device_info(self, data: dict):
        self.device_info_data = data
        if hasattr(self, "card_empty"):
            self.card_empty.hide()
        if hasattr(self, "card_yarward"):
            self.card_yarward.show()
            self.card_icu.show()
            self.card_network.show()
            self.card_device.show()
            self.card_packages.show()
        if hasattr(self, "btn_refresh_info"):
            self.btn_refresh_info.setEnabled(True)
        if hasattr(self, "btn_copy_all_info"):
            self.btn_copy_all_info.setEnabled(True)
        if hasattr(self, "info_badge"):
            self.info_badge.setStyleSheet("")
        model_name = data.get("model") or "未知设备"
        serial = data.get("serial") or ""
        self.info_badge.setText(f"{model_name} · 已更新")
        self.info_device_label.setText(f"序列号: {serial}  |  更新于 {datetime.now().strftime('%H:%M:%S')}")

        # 1. Yarward 专属应用卡片
        _clear_layout(self.yarward_layout)
        hdr_box = QHBoxLayout()
        hdr_box.addWidget(label("Yarward 专属应用", "CardTitle"))
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
        icu_hdr.addWidget(label("亚华医疗 ICU 硬件与看门狗管控", "CardTitle"))

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
            btn_stop_wd = button("暂停看门狗 (调试防复位)", lambda: self.set_watchdog_status_action(True), icon_name="pause")
            btn_stop_wd.setToolTip("向 /sys/class/ICU/version/StopWatchDog 写入 1，暂停看门狗计时，防止调试或升级时死机重启")

            btn_start_wd = button("开启看门狗 (启用防护)", lambda: self.set_watchdog_status_action(False), icon_name="play")
            btn_start_wd.setToolTip("向 /sys/class/ICU/version/StopWatchDog 写入 0，恢复看门狗监控，超时未喂狗将自动复位")

            btn_feed_wd = button("手动喂狗一次", self.feed_watchdog_action, icon_name="bone")
            btn_feed_wd.setToolTip("向 /sys/class/ICU/version/FeedWatchdog 写入 1，执行单次手动喂狗重置计时器")

            btn_refresh_icu = button("刷新 ICU 状态", self.refresh_device_info, icon_name="refresh")
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
        net_hdr.addWidget(label("网络接口与 MAC 地址", "CardTitle"))
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

                    btn_save_ip = button("存入设备表", lambda _, ip=ip_val, ifc=net.get('interface'): self.quick_save_device(ip, ifc))
                    btn_save_ip.setFixedHeight(24)
                    btn_save_ip.setStyleSheet("padding: 1px 6px; font-size: 11px;")
                    ip_box.addWidget(btn_save_ip)
                net_grid.addLayout(ip_box, row_idx, 2)

                tag_lbl = label("已联网" if net.get("connected") else "未连接", "TagSuccess" if net.get("connected") else "TagMuted")
                net_grid.addWidget(tag_lbl, row_idx, 3)

            self.network_layout.addLayout(net_grid)

        # 3. 设备硬件与系统信息卡片
        _clear_layout(self.device_layout)
        dev_hdr = QHBoxLayout()
        dev_hdr.addWidget(label("设备基本硬件与系统信息", "CardTitle"))
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
        pkg_hdr.addWidget(label(f"已安装应用清单 ({len(all_pkgs)} 个)", "CardTitle"))
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
        sys_layout.addWidget(label("设备与系统控制", "CardTitle"))

        btn_grid_sys = QGridLayout()
        btn_grid_sys.setSpacing(6)
        btn_home = button("桌面", lambda: self.quick_hdc_key("home", "返回桌面"))
        btn_home.setToolTip("返回系统主桌面（深度适配，兼容通用应用及以 Home 属性注册的沉浸式/Kiosk医疗终端分机）")
        btn_back = button("返回", lambda: self.quick_hdc_key("back", "返回上一级"))
        btn_back.setToolTip("执行 uinput -K -d 2 -u 2，返回上一级页面")
        btn_power = button("电源 / 息屏", lambda: self.quick_hdc_key("power", "电源/锁屏"))
        btn_power.setToolTip("执行 uinput -K -d 18 -u 18，唤醒或关闭屏幕")

        btn_mount = button("挂载读写", self.quick_hdc_mount)
        btn_mount.setToolTip("执行 hdc target mount，将系统/根分区重新挂载为读写状态")
        btn_screenshot = button("屏幕截图", self.quick_hdc_screenshot)
        btn_screenshot.setToolTip("调用硬件 snapshot_display 截屏并自动拉取到本地保存")
        btn_reboot = button("重启设备", self.quick_hdc_reboot, "Danger")
        btn_reboot.setToolTip("向设备发送 shell reboot 指令重启系统")

        btn_stop_wd = button("禁用看门狗", lambda: self.set_watchdog_status_action(True))
        btn_stop_wd.setToolTip("执行 echo -n 1 > /sys/class/ICU/version/StopWatchDog，停止看门狗计时，防止调试重启")
        btn_start_wd = button("开启看门狗", lambda: self.set_watchdog_status_action(False))
        btn_start_wd.setToolTip("执行 echo -n 0 > /sys/class/ICU/version/StopWatchDog，恢复看门狗监控")
        btn_feed_wd = button("手动喂狗", self.feed_watchdog_action)
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
        app_layout.addWidget(label("应用与文件", "CardTitle"))

        btn_grid_app = QGridLayout()
        btn_grid_app.setSpacing(6)
        btn_install = button("安装应用...", self.quick_hdc_install_hap)
        btn_install.setToolTip("选择本地 .hap 文件通过 hdc app install -r 安装到设备")
        btn_uninstall = button("卸载应用...", self.quick_hdc_uninstall_app)
        btn_uninstall.setToolTip("输入 Bundle 名称通过 hdc app uninstall 卸载应用")
        btn_clean = button("清除缓存...", self.quick_hdc_clean_app)
        btn_clean.setToolTip("执行 bm clean -c -b 清除应用缓存与用户数据")

        btn_send = button("推送文件...", self.quick_hdc_send_file)
        btn_send.setToolTip("选择本地文件并推送到设备指定目录 (例如 /data/local/tmp/)")
        btn_recv = button("拉取文件...", self.quick_hdc_recv_file)
        btn_recv.setToolTip("从设备拉取指定文件保存到本地 Downloads 目录")
        btn_net = button("网络配置...", lambda: self.run_hdc_cmd_async(["shell", "ifconfig"], title="查看网络配置"))
        btn_net.setToolTip("执行 ifconfig 查看设备各网卡 IP 地址与连接状态")

        btn_pull_disk_log = button("拉取应用日志...", lambda: self.fetch_app_logs_action("com.yarward.*"))
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
        setup_combobox_popup(self.hdc_preset_combo, 260)
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

    def alert(self, title: str, message: str):
        """通用警告提示弹窗辅助方法"""
        QMessageBox.warning(self, title, message)

    def show_toast(self, message: str, level: str = "info", duration_ms: int = 3200):
        """在窗口顶部居中展示非阻塞的悬浮 Toast 气泡提示"""
        if not hasattr(self, "_toast_widget") or self._toast_widget is None:
            self._toast_widget = ToastWidget(self)
        self._toast_widget.show_message(message, level=level, duration_ms=duration_ms)

    def _init_webview_log_state(self):
        """初始化 WebView 控制台日志及网络请求状态"""
        self.webview_logs: deque[dict[str, Any]] = deque(maxlen=3000)
        self.webview_filter_level = "all"
        self.webview_filter_keyword = ""
        self.webview_log_counts = {"all": 0, "log": 0, "info": 0, "warn": 0, "error": 0}
        self.webview_network_items: list[dict[str, Any]] = []
        self.webview_network_id_map: dict[str, int] = {}
        self.webview_network_filter = "all"
        self.webview_network_keyword = ""
        self.webview_network_counts = {"all": 0, "xhr": 0, "doc": 0, "other": 0}

    def _build_webview_panel(self) -> QWidget:
        panel = QWidget()
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(0, 2, 0, 2)
        layout.setSpacing(4)

        # 单行精简控制台与接口网络操作栏 (保持与设备日志相同风格)
        toolbar = QHBoxLayout()
        toolbar.setContentsMargins(0, 0, 0, 0)
        toolbar.setSpacing(6)

        # 1. 控制台 / 接口网络 切换按钮 (单行紧凑集成)
        self.btn_subtab_console = QPushButton("控制台")
        self.btn_subtab_console.setObjectName("Primary")
        self.btn_subtab_console.setFixedHeight(28)
        self.btn_subtab_console.setCursor(Qt.PointingHandCursor)
        self.btn_subtab_console.clicked.connect(lambda: self._set_webview_subtab(0))
        toolbar.addWidget(self.btn_subtab_console)

        self.btn_subtab_network = QPushButton("接口网络")
        self.btn_subtab_network.setFixedHeight(28)
        self.btn_subtab_network.setCursor(Qt.PointingHandCursor)
        self.btn_subtab_network.clicked.connect(lambda: self._set_webview_subtab(1))
        toolbar.addWidget(self.btn_subtab_network)

        # 2. 级别 / 类型 过滤下拉框 (置于左侧筛选)
        self.webview_filter_combo = QComboBox()
        self.webview_filter_combo.setAccessibleName("过滤级别或类型")
        self.webview_filter_combo.setFixedHeight(28)
        self.webview_filter_combo.setFixedWidth(112)
        setup_combobox_popup(self.webview_filter_combo, 112)
        self.webview_filter_combo.currentIndexChanged.connect(self._on_webview_filter_combo_changed)
        toolbar.addWidget(self.webview_filter_combo)

        # 3. 过滤输入框 (弹性自适应拉伸)
        self.webview_filter_input = QLineEdit()
        self.webview_filter_input.setPlaceholderText("过滤日志内容或请求 URL…")
        self.webview_filter_input.addAction(get_ui_icon("search"), QLineEdit.LeadingPosition)
        self.webview_filter_input.setAccessibleName("过滤 WebView 日志与网络")
        self.webview_filter_input.setFixedHeight(28)
        self.webview_filter_input.setClearButtonEnabled(True)
        self.webview_filter_input.textChanged.connect(self._on_unified_webview_filter_changed)
        toolbar.addWidget(self.webview_filter_input, 1)

        # 4. 自动滚动与暂停 (对齐设备日志功能区域)
        self.cb_webview_autoscroll = QCheckBox("滚动")
        self.cb_webview_autoscroll.setChecked(True)
        self.cb_webview_autoscroll.setFixedHeight(28)
        self.cb_webview_autoscroll.setToolTip("自动滚动到最新日志或请求")
        toolbar.addWidget(self.cb_webview_autoscroll)

        self.cb_webview_pause = QCheckBox("暂停")
        self.cb_webview_pause.setToolTip("暂停日志与网络捕获刷新")
        self.cb_webview_pause.setFixedHeight(28)
        self.cb_webview_pause.toggled.connect(self._on_webview_pause_toggled)
        toolbar.addWidget(self.cb_webview_pause)

        # 5. 操作按钮: 清空、导出日志、Chrome
        self.btn_webview_clear = button("清空", self._on_unified_webview_clear, icon_name="clear")
        self.btn_webview_clear.setFixedHeight(28)
        toolbar.addWidget(self.btn_webview_clear)

        self.btn_webview_export = button("导出日志", self._on_export_webview_logs, icon_name="export")
        self.btn_webview_export.setFixedHeight(28)
        self.btn_webview_export.setToolTip("导出当前控制台日志或网络请求")
        toolbar.addWidget(self.btn_webview_export)

        self.btn_webview_chrome = button("Chrome", self.open_chrome_inspect, icon_name="chrome")
        self.btn_webview_chrome.setFixedHeight(28)
        self.btn_webview_chrome.setToolTip("在系统 Chrome 浏览器中打开 DevTools 调试")
        toolbar.addWidget(self.btn_webview_chrome)

        # 辅助属性保留 (保证兼容性与测试用例，但不占用界面空间)
        self.webview_page_combo = QComboBox()
        self.webview_page_combo.setAccessibleName("调试网页列表")
        self.webview_page_combo.currentIndexChanged.connect(self._on_webview_page_selected)
        setup_combobox_popup(self.webview_page_combo, 200)
        self.webview_page_combo.hide()

        self.btn_webview_forward = button("连接", self.toggle_webview_forward, "Primary")
        self.btn_webview_forward.hide()

        self.btn_webview_reload = button("刷新", self.reload_webview_page, icon_name="refresh")
        self.btn_webview_reload.hide()

        # 辅助属性保留 (保证兼容性与测试用例)
        self.webview_state_badge = label("未连接", "Badge")
        self.webview_state_badge.hide()
        self.webview_instance_combo = QComboBox()
        self.webview_instance_combo.setAccessibleName("WebView 目标")
        self.webview_instance_combo.setEditable(True)
        setup_combobox_popup(self.webview_instance_combo, 160)
        self.webview_instance_combo.hide()
        self.webview_port_input = QLineEdit(str(DEFAULT_DEVTOOLS_PORT))
        self.webview_port_input.hide()
        self.btn_webview_scan = button("扫描套接字", self.scan_webview_instances)
        self.btn_webview_scan.hide()
        self.mode_header_widget = QWidget()
        self.mode_header_widget.hide()
        self.btn_filter_all = QPushButton("全部 0")
        self.btn_filter_all.hide()
        self.btn_filter_log = QPushButton("日志 0")
        self.btn_filter_log.hide()
        self.btn_filter_warn = QPushButton("警告 0")
        self.btn_filter_warn.hide()
        self.btn_filter_error = QPushButton("错误 0")
        self.btn_filter_error.hide()
        self.btn_net_filter_all = QPushButton("全部 0")
        self.btn_net_filter_all.hide()
        self.btn_net_filter_xhr = QPushButton("Fetch/XHR 0")
        self.btn_net_filter_xhr.hide()
        self.btn_net_filter_doc = QPushButton("文档 0")
        self.btn_net_filter_doc.hide()
        self.btn_net_filter_other = QPushButton("其他 0")
        self.btn_net_filter_other.hide()
        self.webview_net_filter_input = QLineEdit()
        self.webview_net_filter_input.hide()
        self.btn_webview_net_clear = button("清空", self.clear_webview_network)
        self.btn_webview_net_clear.hide()

        layout.addLayout(toolbar)

        self.webview_right_stack = QStackedWidget()
        layout.addWidget(self.webview_right_stack, 1)

        # --- 页面 0: 控制台 ---
        console_page = QWidget()
        console_page_layout = QVBoxLayout(console_page)
        console_page_layout.setContentsMargins(0, 0, 0, 0)
        console_page_layout.setSpacing(4)

        self.webview_console_view = WebViewConsoleView()
        self.webview_console_view.clear_requested.connect(self.clear_webview_console)
        self.webview_console_view.setFont(QFont("Menlo", 11))
        self.webview_console_view.setStyleSheet(
            "background: #090e15; color: #c2d1df; border: 1px solid #1e2c3d; border-radius: 7px; padding: 8px; font-family: 'Menlo', monospace;"
        )
        console_page_layout.addWidget(self.webview_console_view, 1)

        js_bar = QHBoxLayout()
        js_bar.setContentsMargins(0, 0, 0, 0)
        js_bar.setSpacing(6)
        js_prompt = label("JS >", "TagSuccess")
        js_prompt.setStyleSheet("color: #53d6b5; font-weight: 700; font-family: 'Menlo'; padding: 2px 4px;")
        js_bar.addWidget(js_prompt)

        self.webview_js_input = ConsoleInput()
        self.webview_js_input.setPlaceholderText("输入 JavaScript 执行 (按 Enter 运行)…")
        self.webview_js_input.returnPressed.connect(self.execute_webview_js)
        self.webview_js_input.setFixedHeight(26)
        js_bar.addWidget(self.webview_js_input, 1)

        self.btn_webview_eval = button("运行", self.execute_webview_js, "Primary")
        self.btn_webview_eval.setFixedHeight(26)
        js_bar.addWidget(self.btn_webview_eval)

        console_page_layout.addLayout(js_bar)
        self.webview_right_stack.addWidget(console_page)

        # --- 页面 1: 接口网络 ---
        network_page = QWidget()
        net_page_layout = QVBoxLayout(network_page)
        net_page_layout.setContentsMargins(0, 0, 0, 0)
        net_page_layout.setSpacing(4)

        self.webview_net_splitter = QSplitter(Qt.Vertical)
        self.webview_net_splitter.setHandleWidth(4)

        self.webview_network_table = QTableWidget(0, 5)
        self.webview_network_table.setHorizontalHeaderLabels(["状态", "方式", "接口 / URL", "类型", "耗时"])
        self.webview_network_table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeToContents)
        self.webview_network_table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeToContents)
        self.webview_network_table.horizontalHeader().setSectionResizeMode(2, QHeaderView.Stretch)
        self.webview_network_table.horizontalHeader().setSectionResizeMode(3, QHeaderView.ResizeToContents)
        self.webview_network_table.horizontalHeader().setSectionResizeMode(4, QHeaderView.ResizeToContents)
        self.webview_network_table.setSelectionBehavior(QTableWidget.SelectRows)
        self.webview_network_table.setEditTriggers(QTableWidget.NoEditTriggers)
        self.webview_network_table.setAlternatingRowColors(True)
        self.webview_network_table.verticalHeader().setVisible(False)
        self.webview_network_table.setContextMenuPolicy(Qt.CustomContextMenu)
        self.webview_network_table.customContextMenuRequested.connect(self._on_network_table_context_menu)
        self.webview_network_table.itemSelectionChanged.connect(self._on_network_table_selection_changed)
        self.webview_network_table.setStyleSheet(
            "QTableWidget { background: #090e15; alternate-background-color: #0d141e; color: #c2d1df; border: 1px solid #1e2c3d; border-radius: 7px; font-family: 'Menlo', monospace; font-size: 11px; } "
            "QHeaderView::section { background: #121c29; color: #8ba2b9; padding: 4px; border: none; font-weight: bold; font-size: 11px; }"
        )
        self.webview_net_splitter.addWidget(self.webview_network_table)

        self.webview_network_detail_widget = self._create_network_detail_widget()
        self.webview_net_splitter.addWidget(self.webview_network_detail_widget)
        self.webview_network_detail_widget.hide()

        net_page_layout.addWidget(self.webview_net_splitter, 1)
        self.webview_right_stack.addWidget(network_page)

        # 底部状态栏: 统计条数信息 + 统一字号调节 (与设备日志 HiLog 相同风格及字号同步)
        net_bottom_bar = QHBoxLayout()
        net_bottom_bar.setContentsMargins(0, 0, 0, 0)
        net_bottom_bar.setSpacing(8)
        self.webview_net_status_label = label("0 条日志 · 0 个请求", "Muted")
        net_bottom_bar.addWidget(self.webview_net_status_label)
        net_bottom_bar.addStretch(1)

        font_hint = label("字号  ⌘+/⌘−", "Muted")
        font_hint.setToolTip("⌘ + 放大 · ⌘ − 缩小 · ⌘ 0 恢复默认字号")
        net_bottom_bar.addWidget(font_hint)

        self.webview_font_size = QSpinBox()
        self.webview_font_size.setAccessibleName("WebView 字号")
        self.webview_font_size.setRange(8, 28)
        self.webview_font_size.setSuffix(" pt")
        self.webview_font_size.setFixedWidth(82)
        self.webview_font_size.setKeyboardTracking(False)
        self.webview_font_size.setToolTip(font_hint.toolTip())
        try:
            saved_font_size = int(self.settings.value("logs/font_size", 11))
        except (TypeError, ValueError):
            saved_font_size = 11
        self.webview_font_size.setValue(saved_font_size)
        self.webview_font_size.valueChanged.connect(self.set_log_font_size)
        net_bottom_bar.addWidget(self.webview_font_size)

        layout.addLayout(net_bottom_bar)

        self._reset_webview_instance_combo()
        self._set_webview_subtab(0)

        return panel

    def _reset_webview_instance_combo(self):
        """重置实例列表并保持默认 tcp:9222 选项"""
        if not hasattr(self, "webview_instance_combo"):
            return
        self.webview_instance_combo.blockSignals(True)
        self.webview_instance_combo.clear()
        self.webview_instance_combo.addItem(
            "tcp:9222 (默认直连模式)",
            {"target": "tcp:9222", "pid": None, "socket": "tcp:9222"}
        )
        self.webview_instance_combo.blockSignals(False)

    def _update_log_filter_counts(self):
        """仅更新数量统计文本，不重置样式，极速无闪烁"""
        if not hasattr(self, "btn_filter_all"):
            return
        counts = self.webview_log_counts
        self.btn_filter_all.setText(f"全部 {counts['all']}")
        self.btn_filter_log.setText(f"日志 {counts['log']}")
        self.btn_filter_warn.setText(f"警告 {counts['warn']}")
        self.btn_filter_error.setText(f"错误 {counts['error']}")
        if hasattr(self, "webview_filter_combo") and getattr(self, "webview_right_stack", None) and self.webview_right_stack.currentIndex() == 0:
            cur_idx = self.webview_filter_combo.currentIndex()
            self.webview_filter_combo.blockSignals(True)
            self.webview_filter_combo.setItemText(0, f"全部级别 ({counts['all']})")
            self.webview_filter_combo.setItemText(1, f"日志 ({counts['log']})")
            self.webview_filter_combo.setItemText(2, f"警告 ({counts['warn']})")
            self.webview_filter_combo.setItemText(3, f"错误 ({counts['error']})")
            if cur_idx >= 0:
                self.webview_filter_combo.setCurrentIndex(cur_idx)
            self.webview_filter_combo.blockSignals(False)
        self._update_webview_status_label()

    def _update_log_filter_styles(self):
        """刷新过滤按钮的高亮选中样式"""
        if not hasattr(self, "btn_filter_all"):
            return
        buttons = [
            ("all", self.btn_filter_all),
            ("log", self.btn_filter_log),
            ("warn", self.btn_filter_warn),
            ("error", self.btn_filter_error),
        ]
        for lvl, btn in buttons:
            if lvl == self.webview_filter_level:
                btn.setStyleSheet(
                    "background: #1b354f; color: #5ce0c1; border: 1px solid #3d79b2; border-radius: 4px; font-weight: bold; padding: 2px 7px;"
                )
            else:
                btn.setStyleSheet(
                    "background: #121a24; color: #8295a9; border: 1px solid #233142; border-radius: 4px; padding: 2px 7px;"
                )

    def _update_log_filter_badges(self):
        """刷新日志过滤按钮上的数量与高亮样式"""
        self._update_log_filter_counts()
        self._update_log_filter_styles()

    def _set_webview_log_level(self, level: str):
        """切换日志等级过滤"""
        if self.webview_filter_level == level:
            return
        self.webview_filter_level = level
        self._update_log_filter_styles()
        if hasattr(self, "webview_filter_combo") and getattr(self, "webview_right_stack", None) and self.webview_right_stack.currentIndex() == 0:
            mapping = {"all": 0, "log": 1, "warn": 2, "error": 3}
            self.webview_filter_combo.blockSignals(True)
            self.webview_filter_combo.setCurrentIndex(mapping.get(level, 0))
            self.webview_filter_combo.blockSignals(False)
        self._rebuild_webview_console_display()

    def _on_webview_filter_text_changed(self, text: str):
        """日志关键词过滤更新"""
        self.webview_filter_keyword = text.strip()
        self._rebuild_webview_console_display()

    def _entry_matches_filter(self, entry: dict[str, Any]) -> bool:
        """检查单个日志对象是否符合当前的过滤条件"""
        t = entry.get("type", "log").lower()
        if t in ("warn", "warning"):
            norm_type = "warn"
        elif t in ("error", "err"):
            norm_type = "error"
        elif t == "info":
            norm_type = "log"
        elif t in ("eval_in", "eval_out"):
            norm_type = "log"
        else:
            norm_type = "log"

        if self.webview_filter_level != "all" and norm_type != self.webview_filter_level:
            return False

        if self.webview_filter_keyword:
            kw = self.webview_filter_keyword.lower()
            text = str(entry.get("text", "")).lower()
            source = str(entry.get("source", "")).lower()
            if kw not in text and kw not in source:
                return False

        return True

    def _format_entry_html(self, entry: dict[str, Any]) -> str:
        """将日志字典格式化为轻量彩色 HTML 片段"""
        t = entry.get("type", "log").lower()
        time_str = html.escape(str(entry.get("time", "")))
        text = html.escape(str(entry.get("text", "")))
        source = html.escape(str(entry.get("source", "")))

        tag_styles = {
            "log": ("LOG", "#c2d1df"),
            "info": ("INFO", "#4fc3f7"),
            "warn": ("WARN", "#ffb74d"),
            "warning": ("WARN", "#ffb74d"),
            "error": ("ERR", "#ef5350"),
            "eval_in": ("JS >", "#81c784"),
            "eval_out": ("< JS", "#a7ffeb"),
        }
        tag, fg = tag_styles.get(t, ("LOG", "#c2d1df"))
        source_html = f" <span style='color:#627588;'>({source})</span>" if source else ""
        return (
            f"<span style='color:#556677;'>{time_str}</span> "
            f"<span style='color:{fg}; font-weight:bold;'>[{tag}]</span> "
            f"<span style='color:{fg};'>{text}</span>"
            f"{source_html}"
        )

    def _render_log_entry_to_view(self, entry: dict[str, Any]):
        """将符合条件的单条日志追加渲染到控制台视口"""
        html_chunk = self._format_entry_html(entry)
        self.webview_console_view.appendHtml(html_chunk)
        if hasattr(self, "cb_webview_autoscroll") and self.cb_webview_autoscroll.isChecked():
            scroll = self.webview_console_view.verticalScrollBar()
            if (scroll.maximum() - scroll.value()) <= 4:
                scroll.setValue(scroll.maximum())

    def _rebuild_webview_console_display(self):
        """按当前过滤条件全量重构控制台显示内容"""
        self.webview_console_view.clear()
        matching = [e for e in self.webview_logs if self._entry_matches_filter(e)]
        if matching:
            to_render = matching[-1500:]
            html_content = "<br>".join(self._format_entry_html(e) for e in to_render)
            self.webview_console_view.appendHtml(html_content)
        if hasattr(self, "cb_webview_autoscroll") and self.cb_webview_autoscroll.isChecked():
            scroll = self.webview_console_view.verticalScrollBar()
            scroll.setValue(scroll.maximum())

    def _append_webview_entry(self, entry: dict[str, Any]):
        """收录日志并刷新界面"""
        self.webview_logs.append(entry)
        if not isinstance(self.webview_logs, deque) and len(self.webview_logs) > 3000:
            self.webview_logs.pop(0)

        t = entry.get("type", "log").lower()
        if t in ("warn", "warning"):
            norm_type = "warn"
        elif t in ("error", "err"):
            norm_type = "error"
        else:
            norm_type = "log"

        self.webview_log_counts["all"] = len(self.webview_logs)
        self.webview_log_counts[norm_type] = self.webview_log_counts.get(norm_type, 0) + 1
        if not getattr(self, "_webview_counts_pending", False):
            self._webview_counts_pending = True
            QTimer.singleShot(150, self._flush_webview_log_counts)

        if self._entry_matches_filter(entry):
            if not (hasattr(self, "cb_webview_pause") and self.cb_webview_pause.isChecked()):
                self._render_log_entry_to_view(entry)

    def _on_webview_pause_toggled(self, paused: bool):
        """处理 WebView 控制台与网络请求的暂停/继续捕获"""
        if not paused:
            if getattr(self, "webview_right_stack", None) and self.webview_right_stack.currentIndex() == 0:
                self._rebuild_webview_console_display()
            else:
                self._rebuild_network_table_display()

    def _rebuild_network_table_display(self):
        """全量重建网络请求表格显示"""
        self.webview_network_table.setRowCount(0)
        for row, item in enumerate(self.webview_network_items):
            self.webview_network_table.insertRow(row)
            self._render_network_item_to_row(row, item)
        self._filter_network_table()

    def _on_export_webview_logs(self):
        """导出当前 WebView 控制台日志或接口网络数据"""
        is_console = getattr(self, "webview_right_stack", None) and self.webview_right_stack.currentIndex() == 0
        default_name = f"WebView_Console_{datetime.now():%Y%m%d_%H%M%S}.log" if is_console else f"WebView_Network_{datetime.now():%Y%m%d_%H%M%S}.tsv"
        filename, _ = QFileDialog.getSaveFileName(
            self,
            "导出 WebView 控制台日志" if is_console else "导出 WebView 网络请求",
            str(Path.home() / "Downloads" / default_name),
            "日志文件 (*.log *.txt)" if is_console else "数据文件 (*.tsv *.csv *.txt)",
        )
        if not filename:
            return
        try:
            with open(filename, "w", encoding="utf-8") as f:
                if is_console:
                    f.write(f"# HarmonyCast WebView Console Logs - {datetime.now():%Y-%m-%d %H:%M:%S}\n\n")
                    for e in getattr(self, "webview_logs", []):
                        time_str = e.get("time", "")
                        typ = e.get("type", "LOG").upper()
                        text = e.get("text", "")
                        src = f" ({e['source']})" if e.get("source") else ""
                        f.write(f"[{time_str}] [{typ}] {text}{src}\n")
                else:
                    f.write("Status\tMethod\tURL\tType\tDuration\n")
                    for item in getattr(self, "webview_network_items", []):
                        status = str(item.get("status", "-"))
                        method = str(item.get("method", "GET")).upper()
                        url = str(item.get("url", ""))
                        res_type = str(item.get("type", "Other"))
                        dur = str(item.get("duration", "-"))
                        f.write(f"{status}\t{method}\t{url}\t{res_type}\t{dur}\n")
            self.statusBar().showMessage(f"已导出到 {filename}", 3500)
        except Exception as exc:
            QMessageBox.warning(self, "导出失败", str(exc))

    def _flush_webview_log_counts(self):
        self._webview_counts_pending = False
        self._update_log_filter_counts()

    def _append_webview_system_line(self, text: str, level: str = "info"):
        """向 WebView 控制台输出一条系统提示"""
        now_str = datetime.now().strftime("%H:%M:%S")
        entry = {
            "type": level,
            "text": f"[HarmonyCast] {text}",
            "source": "",
            "time": now_str,
        }
        self._append_webview_entry(entry)

    def _append_webview_console_line(self, line: str, color: str = "#c2d1df"):
        """兼容旧版纯文本行追加"""
        now_str = datetime.now().strftime("%H:%M:%S")
        entry = {
            "type": "log",
            "text": line,
            "source": "",
            "time": now_str,
        }
        self._append_webview_entry(entry)

    def _on_webview_screencast_frame(self, image: QImage, metadata: dict):
        """接收实时页面推流帧"""
        if hasattr(self, "webview_screen"):
            self.webview_screen.set_image(image, metadata)

    def _on_webview_screen_clicked(self, x: int, y: int):
        """点击网页画面，转发点击事件"""
        self.webview_session.dispatch_mouse_click(x, y)

    def refresh_webview_screen(self):
        """主动截屏刷新网页画面"""
        self.webview_session.capture_screenshot()

    def _on_webview_console_entry(self, entry: dict[str, Any]):
        """接收 CDP 结构化日志"""
        self._append_webview_entry(entry)

    def _on_webview_console_message(self, msg_type: str, text: str, color: str):
        """兼容旧版信号"""
        pass

    def _on_webview_eval_result(self, req_id: int, text: str, is_error: bool):
        """接收 Runtime.evaluate 结果输出"""
        now_str = datetime.now().strftime("%H:%M:%S")
        entry = {
            "type": "error" if is_error else "eval_out",
            "text": f"<- {text}",
            "source": "",
            "time": now_str,
        }
        self._append_webview_entry(entry)

    def _on_webview_forward_changed(self, active: bool, info: str):
        """端口映射状态变更处理"""
        if active:
            port = self.webview_session.local_port
            target = self.webview_session.current_target or f"tcp:{port}"
            self.webview_state_badge.setText(f"已连接 (:{port})")
            self.webview_state_badge.setStyleSheet(
                "color: #5ce0c1; background: #12352f; padding: 4px 10px; border-radius: 9px; font-weight: bold;"
            )
        else:
            self.webview_state_badge.setText("未连接")
            self.webview_state_badge.setStyleSheet("")

    def _on_webview_ws_state_changed(self, connected: bool, msg: str):
        """WebSocket 调试连接状态变更"""
        if connected:
            page = self.webview_page_combo.currentData()
            title = page.get("title", "网页") if page else "网页"
            self.webview_state_badge.setText(f"已连接 · {title[:14]}")
            self.webview_state_badge.setStyleSheet(
                "color: #5ce0c1; background: #12352f; padding: 4px 10px; border-radius: 9px; font-weight: bold;"
            )
        else:
            if self.webview_session.forwarded:
                port = self.webview_session.local_port
                self.webview_state_badge.setText(f"已映射 (:{port})")
                self.webview_state_badge.setStyleSheet(
                    "color: #f3c969; background: #352f12; padding: 4px 10px; border-radius: 9px;"
                )
            else:
                self.webview_state_badge.setText("未连接")
                self.webview_state_badge.setStyleSheet("")

    def _on_webview_instances_discovered(self, instances):
        pass

    def _on_webview_pages_discovered(self, pages):
        pass

    def _set_webview_subtab(self, index: int):
        """切换控制台 / 接口网络选项卡"""
        self.webview_right_stack.setCurrentIndex(index)
        if hasattr(self, "btn_subtab_console") and hasattr(self, "btn_subtab_network"):
            if index == 0:
                self.btn_subtab_console.setObjectName("Primary")
                self.btn_subtab_network.setObjectName("")
            else:
                self.btn_subtab_network.setObjectName("Primary")
                self.btn_subtab_console.setObjectName("")
            self.btn_subtab_console.setStyle(self.btn_subtab_console.style())
            self.btn_subtab_network.setStyle(self.btn_subtab_network.style())
        self._update_webview_filter_combo_for_subtab(index)

    def _update_webview_filter_combo_for_subtab(self, index: int):
        if not hasattr(self, "webview_filter_combo"):
            return
        self.webview_filter_combo.blockSignals(True)
        self.webview_filter_combo.clear()
        if index == 0:
            counts = getattr(self, "webview_log_counts", {"all": 0, "log": 0, "warn": 0, "error": 0})
            self.webview_filter_combo.addItems([
                f"全部级别 ({counts.get('all', 0)})",
                f"日志 ({counts.get('log', 0)})",
                f"警告 ({counts.get('warn', 0)})",
                f"错误 ({counts.get('error', 0)})",
            ])
            level_map = {"all": 0, "log": 1, "warn": 2, "error": 3}
            self.webview_filter_combo.setCurrentIndex(level_map.get(getattr(self, "webview_filter_level", "all"), 0))
            if hasattr(self, "webview_filter_input"):
                self.webview_filter_input.setPlaceholderText("过滤日志内容或来源…")
                self.webview_filter_input.blockSignals(True)
                self.webview_filter_input.setText(getattr(self, "webview_filter_keyword", ""))
                self.webview_filter_input.blockSignals(False)
        else:
            total = len(getattr(self, "webview_network_items", []))
            xhr_c = sum(1 for e in self.webview_network_items if str(e.get("type", "")).lower() in ("fetch", "xhr", "xmlhttprequest"))
            doc_c = sum(1 for e in self.webview_network_items if str(e.get("type", "")).lower() in ("document", "doc"))
            other_c = total - xhr_c - doc_c
            self.webview_filter_combo.addItems([
                f"全部类型 ({total})",
                f"Fetch/XHR ({xhr_c})",
                f"文档 ({doc_c})",
                f"其他 ({other_c})",
            ])
            type_map = {"all": 0, "xhr": 1, "doc": 2, "other": 3}
            self.webview_filter_combo.setCurrentIndex(type_map.get(getattr(self, "webview_network_filter", "all"), 0))
            if hasattr(self, "webview_filter_input"):
                self.webview_filter_input.setPlaceholderText("过滤接口 URL 或方法…")
                self.webview_filter_input.blockSignals(True)
                self.webview_filter_input.setText(getattr(self, "webview_network_keyword", ""))
                self.webview_filter_input.blockSignals(False)
        self.webview_filter_combo.blockSignals(False)

    def _on_webview_filter_combo_changed(self, index: int):
        subtab = self.webview_right_stack.currentIndex()
        if subtab == 0:
            level_keys = ["all", "log", "warn", "error"]
            if 0 <= index < len(level_keys):
                self._set_webview_log_level(level_keys[index])
        else:
            type_keys = ["all", "xhr", "doc", "other"]
            if 0 <= index < len(type_keys):
                self._set_webview_net_filter(type_keys[index])

    def _on_unified_webview_filter_changed(self, text: str):
        subtab = self.webview_right_stack.currentIndex()
        if subtab == 0:
            self._on_webview_filter_text_changed(text)
        else:
            self._on_webview_net_filter_changed(text)

    def _on_unified_webview_clear(self):
        if self.webview_right_stack.currentIndex() == 0:
            self.clear_webview_console()
        else:
            self.clear_webview_network()

    def _update_webview_status_label(self):
        if hasattr(self, "webview_net_status_label"):
            log_cnt = getattr(self, "webview_log_counts", {}).get("all", 0)
            net_cnt = len(getattr(self, "webview_network_items", []))
            self.webview_net_status_label.setText(f"{log_cnt} 条日志 · {net_cnt} 个请求")

    def _set_webview_net_filter(self, flt: str):
        """切换网络请求类型过滤"""
        if self.webview_network_filter == flt:
            return
        self.webview_network_filter = flt
        if hasattr(self, "webview_filter_combo") and getattr(self, "webview_right_stack", None) and self.webview_right_stack.currentIndex() == 1:
            mapping = {"all": 0, "xhr": 1, "doc": 2, "other": 3}
            self.webview_filter_combo.blockSignals(True)
            self.webview_filter_combo.setCurrentIndex(mapping.get(flt, 0))
            self.webview_filter_combo.blockSignals(False)
        self._filter_network_table()

    def _on_webview_net_filter_changed(self, text: str):
        """网络请求关键字搜索"""
        self.webview_network_keyword = text.strip()
        self._filter_network_table()

    def _filter_network_table(self):
        """根据类型和关键词过滤显示/隐藏网络请求行"""
        flt = self.webview_network_filter
        kw = self.webview_network_keyword.lower()
        for row in range(self.webview_network_table.rowCount()):
            if row < len(self.webview_network_items):
                item = self.webview_network_items[row]
                t = str(item.get("type", "other")).lower()
                url = str(item.get("url", "")).lower()

                type_ok = True
                if flt == "xhr" and t not in ("fetch", "xhr", "xmlhttprequest"):
                    type_ok = False
                elif flt == "doc" and t not in ("document", "doc"):
                    type_ok = False
                elif flt == "other" and t in ("fetch", "xhr", "xmlhttprequest", "document", "doc"):
                    type_ok = False

                kw_ok = True
                if kw and kw not in url:
                    kw_ok = False

                self.webview_network_table.setRowHidden(row, not (type_ok and kw_ok))

    def _update_network_filter_counts(self):
        """更新网络请求分类统计"""
        if not hasattr(self, "btn_net_filter_all"):
            return
        total = len(self.webview_network_items)
        xhr_c = sum(1 for e in self.webview_network_items if str(e.get("type", "")).lower() in ("fetch", "xhr", "xmlhttprequest"))
        doc_c = sum(1 for e in self.webview_network_items if str(e.get("type", "")).lower() in ("document", "doc"))
        other_c = total - xhr_c - doc_c
        self.btn_net_filter_all.setText(f"全部 {total}")
        self.btn_net_filter_xhr.setText(f"Fetch/XHR {xhr_c}")
        self.btn_net_filter_doc.setText(f"文档 {doc_c}")
        self.btn_net_filter_other.setText(f"其他 {other_c}")
        if hasattr(self, "webview_filter_combo") and getattr(self, "webview_right_stack", None) and self.webview_right_stack.currentIndex() == 1:
            cur_idx = self.webview_filter_combo.currentIndex()
            self.webview_filter_combo.blockSignals(True)
            self.webview_filter_combo.setItemText(0, f"全部类型 ({total})")
            self.webview_filter_combo.setItemText(1, f"Fetch/XHR ({xhr_c})")
            self.webview_filter_combo.setItemText(2, f"文档 ({doc_c})")
            self.webview_filter_combo.setItemText(3, f"其他 ({other_c})")
            if cur_idx >= 0:
                self.webview_filter_combo.setCurrentIndex(cur_idx)
            self.webview_filter_combo.blockSignals(False)
        self._update_webview_status_label()

    def _create_network_detail_widget(self) -> QWidget:
        """创建网络请求详细面板 (标头、载荷、响应体)"""
        widget = QFrame()
        widget.setObjectName("NetworkDetailFrame")
        widget.setStyleSheet(
            "#NetworkDetailFrame { background: #0c121b; border: 1px solid #1e2c3d; border-radius: 6px; } "
            "QPushButton { background: #131e2b; color: #a2b4c7; border: 1px solid #233549; border-radius: 4px; padding: 2px 8px; font-size: 11px; } "
            "QPushButton:hover { background: #1c2c3e; color: #e1ebf5; border-color: #3b536e; } "
            "QPushButton:checked { background: #162a3f; color: #73d6c1; border-color: #388075; font-weight: bold; }"
        )
        layout = QVBoxLayout(widget)
        layout.setContentsMargins(6, 6, 6, 6)
        layout.setSpacing(6)

        # 详情顶部栏
        top_bar = QHBoxLayout()
        top_bar.setSpacing(6)

        self.net_detail_badge_method = QLabel("GET")
        self.net_detail_badge_method.setStyleSheet("background: #172d27; color: #73d6c1; font-weight: bold; border-radius: 3px; padding: 1px 5px; font-size: 11px;")
        top_bar.addWidget(self.net_detail_badge_method)

        self.net_detail_badge_status = QLabel("200")
        self.net_detail_badge_status.setStyleSheet("background: #152b27; color: #5ce0c1; font-weight: bold; border-radius: 3px; padding: 1px 5px; font-size: 11px;")
        top_bar.addWidget(self.net_detail_badge_status)

        self.net_detail_url_label = QLabel("")
        self.net_detail_url_label.setStyleSheet("color: #c2d1df; font-size: 11px; font-family: 'Menlo', monospace;")
        top_bar.addWidget(self.net_detail_url_label, 1)

        # Tab 切换按钮组 (标头 / 载荷 / 响应)
        self.btn_net_tab_headers = QPushButton("标头")
        self.btn_net_tab_headers.setCheckable(True)
        self.btn_net_tab_headers.setChecked(True)
        self.btn_net_tab_headers.setFixedHeight(22)
        self.btn_net_tab_headers.setCursor(Qt.PointingHandCursor)
        self.btn_net_tab_headers.clicked.connect(lambda: self._switch_net_detail_tab(0))
        top_bar.addWidget(self.btn_net_tab_headers)

        self.btn_net_tab_payload = QPushButton("载荷")
        self.btn_net_tab_payload.setCheckable(True)
        self.btn_net_tab_payload.setFixedHeight(22)
        self.btn_net_tab_payload.setCursor(Qt.PointingHandCursor)
        self.btn_net_tab_payload.clicked.connect(lambda: self._switch_net_detail_tab(1))
        top_bar.addWidget(self.btn_net_tab_payload)

        self.btn_net_tab_response = QPushButton("响应")
        self.btn_net_tab_response.setCheckable(True)
        self.btn_net_tab_response.setFixedHeight(22)
        self.btn_net_tab_response.setCursor(Qt.PointingHandCursor)
        self.btn_net_tab_response.clicked.connect(lambda: self._switch_net_detail_tab(2))
        top_bar.addWidget(self.btn_net_tab_response)

        top_bar.addSpacing(4)
        btn_copy_resp = button("复制响应", self._on_copy_network_response)
        btn_copy_resp.setFixedHeight(22)
        btn_copy_resp.setStyleSheet("padding: 1px 6px; font-size: 11px;")
        top_bar.addWidget(btn_copy_resp)

        btn_close = icon_button("clear", self._on_close_network_detail, tooltip="关闭详情面板", name="IconBtnSettings", width=22, height=22)
        top_bar.addWidget(btn_close)

        layout.addLayout(top_bar)

        # Stacked Widget 容纳 3 个 Tab 页面
        self.net_detail_stack = QStackedWidget()

        # Page 0: 标头
        self.webview_net_headers_view = QPlainTextEdit()
        self.webview_net_headers_view.setReadOnly(True)
        self.webview_net_headers_view.setStyleSheet("QPlainTextEdit { background: #080c13; color: #c2d1df; border: 1px solid #1a2737; border-radius: 4px; font-family: 'Menlo', monospace; font-size: 11px; }")
        self.net_detail_stack.addWidget(self.webview_net_headers_view)

        # Page 1: 载荷
        self.webview_net_payload_view = QPlainTextEdit()
        self.webview_net_payload_view.setReadOnly(True)
        self.webview_net_payload_view.setStyleSheet("QPlainTextEdit { background: #080c13; color: #c2d1df; border: 1px solid #1a2737; border-radius: 4px; font-family: 'Menlo', monospace; font-size: 11px; }")
        self.net_detail_stack.addWidget(self.webview_net_payload_view)

        # Page 2: 响应
        self.webview_net_response_view = QPlainTextEdit()
        self.webview_net_response_view.setReadOnly(True)
        self.webview_net_response_view.setStyleSheet("QPlainTextEdit { background: #080c13; color: #c2d1df; border: 1px solid #1a2737; border-radius: 4px; font-family: 'Menlo', monospace; font-size: 11px; }")
        self.net_detail_stack.addWidget(self.webview_net_response_view)

        layout.addWidget(self.net_detail_stack, 1)
        return widget

    def _switch_net_detail_tab(self, index: int):
        """切换详细面板标签页"""
        self.btn_net_tab_headers.setChecked(index == 0)
        self.btn_net_tab_payload.setChecked(index == 1)
        self.btn_net_tab_response.setChecked(index == 2)
        self.net_detail_stack.setCurrentIndex(index)

    def _on_close_network_detail(self):
        """关闭详细面板"""
        if hasattr(self, "webview_network_detail_widget"):
            self.webview_network_detail_widget.hide()

    def _on_network_table_selection_changed(self):
        """用户点击表格行时加载并展示详细面板"""
        rows = self.webview_network_table.selectionModel().selectedRows()
        if not rows:
            return
        row = rows[0].row()
        if not (0 <= row < len(self.webview_network_items)):
            return
        item = self.webview_network_items[row]
        self._show_network_detail(item)

    def _show_network_detail(self, item: dict[str, Any]):
        """在详细面板中渲染当前请求的标头、载荷及响应"""
        if not hasattr(self, "webview_network_detail_widget"):
            return

        self.current_selected_network_item = item
        self.current_selected_network_item_id = item.get("id", "")
        self.webview_network_detail_widget.show()

        # 调整 Splitter 分割高度，确保详情区可见
        sizes = self.webview_net_splitter.sizes()
        if len(sizes) >= 2 and sizes[1] < 80:
            total = sum(sizes) or 500
            self.webview_net_splitter.setSizes([int(total * 0.5), int(total * 0.5)])

        # 1. 顶部 Badge
        method = str(item.get("method", "GET")).upper()
        status = str(item.get("status", "pending"))
        url = str(item.get("url", ""))

        self.net_detail_badge_method.setText(method)
        if method == "POST":
            self.net_detail_badge_method.setStyleSheet("background: #2d2415; color: #ffb74d; font-weight: bold; border-radius: 3px; padding: 1px 5px; font-size: 11px;")
        elif method == "GET":
            self.net_detail_badge_method.setStyleSheet("background: #172d27; color: #73d6c1; font-weight: bold; border-radius: 3px; padding: 1px 5px; font-size: 11px;")
        else:
            self.net_detail_badge_method.setStyleSheet("background: #1a2737; color: #81c784; font-weight: bold; border-radius: 3px; padding: 1px 5px; font-size: 11px;")

        self.net_detail_badge_status.setText(status)
        if status.startswith("2") or status == "304":
            self.net_detail_badge_status.setStyleSheet("background: #152b27; color: #5ce0c1; font-weight: bold; border-radius: 3px; padding: 1px 5px; font-size: 11px;")
        elif status.startswith("4") or status.startswith("5") or status == "failed":
            self.net_detail_badge_status.setStyleSheet("background: #331818; color: #ff8080; font-weight: bold; border-radius: 3px; padding: 1px 5px; font-size: 11px;")
        else:
            self.net_detail_badge_status.setStyleSheet("background: #2b2515; color: #f3c969; font-weight: bold; border-radius: 3px; padding: 1px 5px; font-size: 11px;")

        self.net_detail_url_label.setText(url)
        self.net_detail_url_label.setToolTip(url)

        # 2. 标头 (Headers) 内容拼接
        lines = []
        lines.append("【常规 (General)】")
        lines.append(f"请求 URL: {url}")
        lines.append(f"请求方法: {method}")
        status_text = item.get("status_text", "")
        lines.append(f"状态代码: {status} {status_text}".strip())
        if item.get("protocol"):
            lines.append(f"版本协议: {item['protocol']}")
        if item.get("mimeType"):
            lines.append(f"资源类型: {item['mimeType']}")
        lines.append("")

        res_headers = item.get("response_headers", {})
        lines.append("【响应标头 (Response Headers)】")
        if res_headers:
            for k, v in res_headers.items():
                lines.append(f"{k}: {v}")
        else:
            lines.append("(尚未接收响应标头)")
        lines.append("")

        req_headers = item.get("request_headers", {})
        lines.append("【请求标头 (Request Headers)】")
        if req_headers:
            for k, v in req_headers.items():
                lines.append(f"{k}: {v}")
        else:
            lines.append("(无显式请求标头)")

        self.webview_net_headers_view.setPlainText("\n".join(lines))

        # 3. 载荷 (Payload)
        p_lines = []
        post_data = item.get("post_data", "")
        if post_data:
            p_lines.append("【请求体 (Request Body)】")
            try:
                parsed_json = json.loads(post_data)
                p_lines.append(json.dumps(parsed_json, indent=2, ensure_ascii=False))
            except Exception:
                p_lines.append(str(post_data))
            p_lines.append("")

        if "?" in url:
            qs = url.split("?", 1)[1]
            p_lines.append("【查询字符串参数 (Query String Parameters)】")
            try:
                from urllib.parse import parse_qsl
                params = parse_qsl(qs, keep_blank_values=True)
                for pk, pv in params:
                    p_lines.append(f"{pk}: {pv}")
            except Exception:
                p_lines.append(qs)

        if not p_lines:
            p_lines.append("(此请求没有请求载荷或查询参数)")

        self.webview_net_payload_view.setPlainText("\n".join(p_lines))

        # 4. 响应体 (Response)
        req_id = item.get("id", "")
        cached_body = item.get("response_body")
        if cached_body is not None and cached_body != "":
            self._render_response_body_text(cached_body, bool(item.get("response_base64", False)))
        elif status == "pending":
            self.webview_net_response_view.setPlainText("(请求进行中，等待响应...)")
        else:
            self.webview_net_response_view.setPlainText("正在拉取接口响应体 (Network.getResponseBody)...")
            if hasattr(self, "webview_session"):
                self.webview_session.get_response_body(
                    req_id,
                    lambda body, is_b64, err, target_id=req_id: self._on_response_body_received(target_id, body, is_b64, err)
                )

    def _on_response_body_received(self, req_id: str, body: str, is_b64: bool, err: str):
        """异步接收并渲染接口返回体"""
        for it in self.webview_network_items:
            if it.get("id") == req_id:
                it["response_body"] = body
                it["response_base64"] = is_b64
                break

        cur_id = getattr(self, "current_selected_network_item_id", "")
        if cur_id == req_id:
            if err:
                self.webview_net_response_view.setPlainText(f"(获取接口响应体失败: {err})")
            else:
                self._render_response_body_text(body, is_b64)

    def _render_response_body_text(self, body: str, is_b64: bool):
        """美化并渲染响应体内容"""
        if is_b64:
            self.webview_net_response_view.setPlainText(f"[二进制媒体数据 (Base64 编码), 长度 {len(body)} 字符]")
            return
        if not body:
            self.webview_net_response_view.setPlainText("(响应体为空)")
            return
        try:
            parsed = json.loads(body)
            formatted = json.dumps(parsed, indent=2, ensure_ascii=False)
            self.webview_net_response_view.setPlainText(formatted)
        except Exception:
            self.webview_net_response_view.setPlainText(body)

    def _on_copy_network_response(self):
        """复制当前接口的响应体到剪贴板"""
        text = self.webview_net_response_view.toPlainText()
        if not text or text.startswith("正在拉取") or text.startswith("(请求进行中"):
            cur = getattr(self, "current_selected_network_item", None)
            text = cur.get("response_body", "") if cur else ""
        if text:
            QApplication.clipboard().setText(text)
            self.statusBar().showMessage(f"已复制接口响应体 ({len(text)} 字符)", 3000)
        else:
            self.statusBar().showMessage("响应体为空，无法复制", 2000)

    def _flush_network_filter_counts(self):
        self._network_counts_pending = False
        self._update_network_filter_counts()

    def _on_webview_network_entry(self, entry: dict[str, Any]):
        """接收实时网络接口请求"""
        req_id = entry.get("id")
        if not req_id:
            return

        if not hasattr(self, "webview_network_id_map"):
            self.webview_network_id_map = {it["id"]: idx for idx, it in enumerate(self.webview_network_items) if it.get("id")}

        existing_idx = self.webview_network_id_map.get(req_id)
        is_paused = hasattr(self, "cb_webview_pause") and self.cb_webview_pause.isChecked()
        if existing_idx is not None and existing_idx < len(self.webview_network_items):
            self.webview_network_items[existing_idx].update(entry)
            if not is_paused:
                self._render_network_item_to_row(existing_idx, self.webview_network_items[existing_idx])
                cur_id = getattr(self, "current_selected_network_item_id", "")
                if cur_id == req_id:
                    self._show_network_detail(self.webview_network_items[existing_idx])
        else:
            if len(self.webview_network_items) >= 1200:
                trim_count = len(self.webview_network_items) - 1000
                self.webview_network_items = self.webview_network_items[trim_count:]
                for _ in range(min(trim_count, self.webview_network_table.rowCount())):
                    self.webview_network_table.removeRow(0)
                self.webview_network_id_map = {it["id"]: idx for idx, it in enumerate(self.webview_network_items) if it.get("id")}

            row = len(self.webview_network_items)
            self.webview_network_id_map[req_id] = row
            self.webview_network_items.append(entry)
            if not is_paused:
                self.webview_network_table.insertRow(row)
                self._render_network_item_to_row(row, entry)

        if not getattr(self, "_network_counts_pending", False):
            self._network_counts_pending = True
            QTimer.singleShot(200, self._flush_network_filter_counts)

    def _render_network_item_to_row(self, row: int, item: dict[str, Any]):
        """渲染单条网络请求到表格行"""
        if row >= self.webview_network_table.rowCount():
            return
        status = str(item.get("status", "pending"))
        method = str(item.get("method", "GET")).upper()
        url = str(item.get("url", ""))
        req_type = str(item.get("type", "other"))
        duration = str(item.get("duration", "-"))

        status_item = QTableWidgetItem(status)
        if status.startswith("2") or status == "304":
            status_item.setForeground(QColor("#5ce0c1"))
        elif status.startswith("4") or status.startswith("5") or status == "failed":
            status_item.setForeground(QColor("#ff8080"))
        else:
            status_item.setForeground(QColor("#f3c969"))
        status_item.setTextAlignment(Qt.AlignCenter)

        method_item = QTableWidgetItem(method)
        if method == "POST":
            method_item.setForeground(QColor("#ffb74d"))
        elif method == "GET":
            method_item.setForeground(QColor("#73d6c1"))
        else:
            method_item.setForeground(QColor("#81c784"))
        method_item.setTextAlignment(Qt.AlignCenter)
        font = method_item.font()
        font.setBold(True)
        method_item.setFont(font)

        parsed = url.split("?")[0]
        name = parsed.split("/")[-1] or parsed
        if not name:
            name = url
        url_item = QTableWidgetItem(name)
        url_item.setToolTip(url)

        type_item = QTableWidgetItem(req_type)
        type_item.setForeground(QColor("#8ba2b9"))
        type_item.setTextAlignment(Qt.AlignCenter)

        dur_item = QTableWidgetItem(duration)
        dur_item.setForeground(QColor("#8ba2b9"))
        dur_item.setTextAlignment(Qt.AlignRight | Qt.AlignVCenter)

        self.webview_network_table.setItem(row, 0, status_item)
        self.webview_network_table.setItem(row, 1, method_item)
        self.webview_network_table.setItem(row, 2, url_item)
        self.webview_network_table.setItem(row, 3, type_item)
        self.webview_network_table.setItem(row, 4, dur_item)

    def _on_network_table_context_menu(self, pos):
        """网络请求表格右键菜单"""
        menu = QMenu(self)
        detail_act = menu.addAction("查看接口详情")
        copy_url_act = menu.addAction("复制接口完整 URL")
        copy_resp_act = menu.addAction("复制接口响应体 (Response)")
        copy_curl_act = menu.addAction("复制为 cURL 命令")
        menu.addSeparator()
        clear_act = menu.addAction("清空网络请求列表")

        act = menu.exec_(self.webview_network_table.viewport().mapToGlobal(pos))
        row = self.webview_network_table.currentRow()
        item = self.webview_network_items[row] if (0 <= row < len(self.webview_network_items)) else None

        if act == detail_act and item:
            self._show_network_detail(item)
        elif act == copy_url_act and item:
            url = item.get("url", "")
            QApplication.clipboard().setText(url)
            self.statusBar().showMessage(f"已复制接口 URL: {url[:60]}", 3000)
        elif act == copy_resp_act and item:
            resp = item.get("response_body", "")
            if resp:
                QApplication.clipboard().setText(resp)
                self.statusBar().showMessage("已复制接口响应体", 3000)
            else:
                self.webview_session.get_response_body(
                    item.get("id", ""),
                    lambda b, is_b64, err: (
                        QApplication.clipboard().setText(b),
                        self.statusBar().showMessage("已复制接口响应体", 3000)
                    ) if b else self.statusBar().showMessage("接口响应体为空", 2000)
                )
        elif act == copy_curl_act and item:
            curl_cmd = generate_curl_command(item)
            QApplication.clipboard().setText(curl_cmd)
            self.statusBar().showMessage("已复制为 cURL 命令到剪贴板", 3000)
        elif act == clear_act:
            self.clear_webview_network()

    def clear_webview_network(self):
        """清空网络请求"""
        self.webview_network_items.clear()
        if hasattr(self, "webview_network_id_map"):
            self.webview_network_id_map.clear()
        self.webview_network_table.setRowCount(0)
        if hasattr(self, "webview_network_detail_widget"):
            self.webview_network_detail_widget.hide()
        if hasattr(self, "webview_session"):
            self.webview_session.clear_network()
        self.current_selected_network_item = None
        self.current_selected_network_item_id = ""
        self._update_network_filter_counts()

    def scan_webview_instances(self):
        """异步扫描设备套接字"""
        device = self.selected_device()
        if not device or not self.hdc:
            self.statusBar().showMessage("请先选择已连接的设备")
            return

        self._scan_generation = getattr(self, "_scan_generation", 0) + 1
        scan_gen = self._scan_generation
        self.btn_webview_scan.setEnabled(False)
        self.webview_state_badge.setText("扫描中…")

        def worker():
            return self.webview_session.scan_instances(self.hdc, device.serial)

        def on_done(instances):
            if scan_gen != getattr(self, "_scan_generation", 0):
                return
            self.btn_webview_scan.setEnabled(True)
            self.webview_instance_combo.blockSignals(True)
            self.webview_instance_combo.clear()
            self.webview_instance_combo.addItem(
                "tcp:9222 (默认直连模式)",
                {"target": "tcp:9222", "pid": None, "socket": "tcp:9222"}
            )
            if instances:
                for inst in instances:
                    desc = f"PID {inst['pid']}"
                    if inst.get("bundle"):
                        desc += f" · {inst['bundle']}"
                    desc += f" ({inst.get('socket', '')})"
                    self.webview_instance_combo.addItem(desc, inst)
                self.statusBar().showMessage(f"已扫描到 {len(instances)} 个套接字", 3000)
                self._append_webview_system_line(f"套接字扫描完成: 发现 {len(instances)} 个实例")
            else:
                self.statusBar().showMessage("未检测到 Unix 套接字，使用默认直连 tcp:9222 即可", 3000)
                self.show_toast("未检测到运行中的 WebView 套接字，可使用默认直连", level="info", duration_ms=3000)
            self.webview_instance_combo.blockSignals(False)

        self.submit(worker, on_done, "扫描 WebView 实例", alert=False)

    def toggle_webview_forward(self):
        """完全异步建立或断开 HDC 端口转发，保证 UI 界面绝不卡顿或卡死"""
        device = self.selected_device()
        if not device or not self.hdc:
            self.show_toast("请先选择已连接的设备", level="warning")
            return

        if self.webview_session.forwarded:
            self.btn_webview_forward.setEnabled(False)
            def stop_worker():
                self.webview_session.stop_forward()

            def on_stopped(_):
                self.btn_webview_forward.setEnabled(True)
                self.btn_webview_forward.setText("连接")
                self.btn_webview_forward.setObjectName("Primary")
                self.btn_webview_forward.setStyle(self.btn_webview_forward.style())
                self.webview_page_combo.clear()
                if hasattr(self, "webview_screen"):
                    self.webview_screen.clear()
                if hasattr(self, "webview_url_label"):
                    self.webview_url_label.setText("")
                self._append_webview_system_line("已断开端口映射与画面监听")

            self.submit(stop_worker, on_stopped, "断开端口转发", alert=False)
            return

        inst = self.webview_instance_combo.currentData()
        target = "tcp:9222"
        if isinstance(inst, dict):
            target = inst.get("target") or inst.get("socket") or "tcp:9222"
        else:
            typed = self.webview_instance_combo.currentText().strip()
            if typed:
                target = typed.split()[0] if " " in typed else typed

        if not target or target.startswith("tcp:9222"):
            target = "tcp:9222"

        self.btn_webview_forward.setEnabled(False)
        self.webview_state_badge.setText("正在连接…")

        def start_worker():
            return self.webview_session.start_forward(
                self.hdc, device.serial, target_or_pid=target, port=DEFAULT_DEVTOOLS_PORT
            )

        def on_started(result):
            self.btn_webview_forward.setEnabled(True)
            ok, info = result
            if ok:
                self.btn_webview_forward.setText("断开")
                self.btn_webview_forward.setObjectName("Danger")
                self.btn_webview_forward.setStyle(self.btn_webview_forward.style())
                self._append_webview_system_line(f"{info}")
                self.fetch_webview_pages_action(show_toast_if_empty=True)
            else:
                self.show_toast(f"建立映射失败：{info}", level="error", duration_ms=3500)

        self.submit(start_worker, on_started, "建立端口转发", alert=False)

    def fetch_webview_pages_action(self, show_toast_if_empty: bool = True):
        """异步拉取当前映射端口下的可用网页列表 (/json)"""
        if getattr(self, "closing", False):
            return

        def worker():
            if getattr(self, "closing", False):
                return []
            return self.webview_session.fetch_pages()

        def on_done(pages):
            if getattr(self, "closing", False):
                return
            self.webview_page_combo.blockSignals(True)
            self.webview_page_combo.clear()
            if pages:
                for p in pages:
                    title = p.get("title") or "未命名网页"
                    url = p.get("url", "")
                    short_url = url.split("?")[0]
                    self.webview_page_combo.addItem(f"{title} ({short_url})", p)
                self.webview_page_combo.blockSignals(False)
                self.btn_webview_forward.setText("断开")
                self.btn_webview_forward.setObjectName("Danger")
                self.btn_webview_forward.setStyle(self.btn_webview_forward.style())
                self._append_webview_system_line(f"发现 {len(pages)} 个网页，正在同步画面与控制台…")
                self._on_webview_page_selected(0)
            else:
                self.webview_page_combo.blockSignals(False)
                self._append_webview_system_line(
                    "端口已就绪，但尚未检测到加载的网页。请在设备上打开页面后点击【刷新网页】",
                    level="info"
                )
                if show_toast_if_empty:
                    self.show_toast("当前设备未检测到运行中的 WebView 界面", level="warning", duration_ms=3500)

        self.submit(worker, on_done, "拉取 WebView 页面", alert=False)

    def _on_webview_page_selected(self, index: int):
        """选中指定目标网页后自动连接 CDP WebSocket"""
        page = self.webview_page_combo.currentData()
        if page and "webSocketDebuggerUrl" in page:
            ws_url = page["webSocketDebuggerUrl"]
            title = page.get("title", "")
            url = page.get("url", "")
            if hasattr(self, "webview_url_label"):
                self.webview_url_label.setText(url[:48] + "…" if len(url) > 48 else url)
            self._append_webview_system_line(f"已连接目标网页: {title}")
            self.webview_session.connect_page(ws_url, page.get("id", ""))
            QTimer.singleShot(400, self._on_scheduled_screenshot)

    def _on_scheduled_screenshot(self):
        if getattr(self, "closing", False):
            return
        if hasattr(self, "webview_session"):
            try:
                self.webview_session.capture_screenshot()
            except Exception:
                pass

    def execute_webview_js(self):
        """在当前网页上下文执行 JavaScript 表达式"""
        if not getattr(self.webview_session, "connected", False):
            self.show_toast("当前无连接的 WebView 界面", level="warning")
        code = self.webview_js_input.text().strip()
        if not code:
            return
        self.webview_js_input.push_history(code)
        self.webview_js_input.clear()

        now_str = datetime.now().strftime("%H:%M:%S")
        entry = {
            "type": "eval_in",
            "text": f"> {code}",
            "source": "",
            "time": now_str,
        }
        self._append_webview_entry(entry)

        try:
            self.webview_session.evaluate(code)
        except Exception as exc:
            self._append_webview_system_line(f"执行失败: {exc}", level="error")

    def run_webview_snippet(self, code: str):
        """填入常用片段并运行"""
        self.webview_js_input.setText(code)
        self.execute_webview_js()

    def reload_webview_page(self):
        """向当前页面发送重载命令"""
        if not getattr(self.webview_session, "connected", False):
            self.show_toast("当前无连接的 WebView 界面", level="warning")
        try:
            self.webview_session.reload_page()
            self._append_webview_system_line("已向页面发送重载命令 (Page.reload)")
        except Exception as exc:
            self._append_webview_system_line(f"刷新失败: {exc}", level="error")

    def clear_webview_console(self):
        """清空控制台日志"""
        self.webview_logs.clear()
        self.webview_log_counts = {"all": 0, "log": 0, "info": 0, "warn": 0, "error": 0}
        self._update_log_filter_badges()
        self.webview_console_view.clear()
        self.statusBar().showMessage("控制台已清空", 2000)

    def copy_all_webview_logs(self):
        """将当前筛选的日志导出复制至剪贴板"""
        matching = [e for e in self.webview_logs if self._entry_matches_filter(e)]
        if not matching:
            self.statusBar().showMessage("没有可复制的日志", 2000)
            return
        lines = []
        for e in matching:
            t = e.get("type", "log").upper()
            time_str = e.get("time", "")
            src = f" ({e['source']})" if e.get("source") else ""
            lines.append(f"{time_str} [{t:<5}] {e.get('text', '')}{src}")
        content = "\n".join(lines)
        self.copy_to_clipboard(content, "WebView 控制台日志")

    def toggle_webview_guide(self):
        """帮助"""
        pass

    def open_chrome_inspect(self):
        """打开 Chrome 的 chrome://inspect/#devices 页面"""
        device = self.selected_device()
        if not device or not self.hdc:
            self.show_toast("请先选择已连接的设备", level="warning")
            return
        if not self.webview_session.forwarded:
            if device and self.hdc:
                self.toggle_webview_forward()
            else:
                self.alert("未选择设备", "请先选择已连接的设备")
                return

        port = self.webview_session.local_port if self.webview_session.forwarded else DEFAULT_DEVTOOLS_PORT
        target_url = "chrome://inspect/#devices"
        self.copy_to_clipboard(target_url, "Chrome 调试地址")

        opened = False
        if sys.platform == "darwin":
            try:
                subprocess.Popen(["open", "-a", "Google Chrome", target_url])
                opened = True
            except Exception:
                pass
        if not opened:
            try:
                import webbrowser
                webbrowser.open(target_url)
            except Exception:
                pass

        self._append_webview_system_line(
            f"已在 Chrome 中唤起 {target_url}（地址已复制到剪贴板）。"
            f"在 Chrome 中勾选 Discover network targets 并配置 localhost:{port}，点击 inspect 即可弹出原生独立手机画面与调试窗口！",
            level="info"
        )
        self.statusBar().showMessage(f"已唤起 Chrome 调试页，已将 {target_url} 复制到剪贴板", 5000)

    def open_target_window(self):
        """在浏览器中直接打开针对当前页面的 DevTools 审查窗口"""
        self.open_chrome_inspect()

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
        if hasattr(self, "mirror_page"):
            self.mirror_page.show()
        if hasattr(self, "log_panel"):
            self.log_panel.hide()
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
        if hasattr(self, "set_workspace_layout"):
            self.set_workspace_layout(getattr(self, "current_workspace_layout", "horizontal"))
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
            if hasattr(self, "mirror_page"):
                self.mirror_page.show()
            if hasattr(self, "log_panel"):
                self.log_panel.hide()
        elif self.presentation_fullscreen:
            self.presentation_fullscreen = False
            self.sidebar.show()
            self.header.show()
            self.mirror_toolbar.show()
            self.statusBar().show()
            if hasattr(self, "set_workspace_layout"):
                self.set_workspace_layout(getattr(self, "current_workspace_layout", "horizontal"))
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
        data = self.device_list.currentData()
        return data if isinstance(data, Device) else None

    def is_device_connected(self, device: Device | None = None) -> bool:
        dev = device if device is not None else self.selected_device()
        if not dev or not getattr(self, "devices", None):
            return False
        return any(d.serial == dev.serial for d in self.devices)

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
        worker = threading.Thread(target=task, name=name, daemon=True)
        with self.worker_lock:
            self.workers.add(worker)
        worker.start()
        return worker

    def task_failed(self, title, message, alert):
        self.statusBar().showMessage(f"{title}：{message}")
        if "切换" in title or "设备" in title:
            if hasattr(self, "device_list"):
                self.device_list.setEnabled(True)
            if hasattr(self, "state_badge"):
                self.state_badge.setStyleSheet("")
            if hasattr(self, "screen"):
                self.screen.switching_message = ""
                self.screen.update()
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
        if self.hdc is None or self.scan_busy or self.deploying:
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

    def populate_devices(self, devices, notify_changed: bool = True):
        old = self.selected_device()
        self.devices = devices
        self.device_list.blockSignals(True)
        self.device_list.clear()
        saved_names = {}
        saved_list = []
        try:
            saved_list = load_saved_devices()
            saved_names = {d["ip"]: d["name"] for d in saved_list if d.get("name")}
        except Exception:
            pass
        connected_serials = set()
        for device in devices:
            connected_serials.add(device.serial)
            if ":" in device.serial:
                host = device.serial.split(":")[0]
                connected_serials.add(host)
                try:
                    save_device(host, name="", port=DEFAULT_NETWORK_PORT)
                except Exception:
                    pass
                text = f"🟢 {device.serial}"
            else:
                id_part = device.serial[-6:] if len(device.serial) > 8 else device.serial
                text = f"🟢 USB: {id_part}"
            self.device_list.addItem(text, device)
            self.device_list.setItemData(self.device_list.count() - 1,
                                         f"{device.serial}\n型号: {device.model}\n架构: {device.architecture}\n版本: {device.version}", Qt.ToolTipRole)
            if old and old.serial == device.serial:
                self.device_list.setCurrentIndex(self.device_list.count() - 1)

        # 核心：下拉框读取设备中心保存的 IP 设备列表
        for saved in saved_list:
            ip = saved.get("ip")
            if not ip:
                continue
            port = saved.get("port", DEFAULT_NETWORK_PORT)
            ep = f"{ip}:{port}"
            if ip not in connected_serials and ep not in connected_serials:
                dev_placeholder = Device(ep, "网络设备", "OpenHarmony")
                text = f"⚪ {ep}"
                self.device_list.addItem(text, dev_placeholder)
                self.device_list.setItemData(self.device_list.count() - 1,
                                             f"{ep}\n选择此项将切换至 {ep} (自动断开其他设备)", Qt.ToolTipRole)

        self.device_list.blockSignals(False)
        self.scan_busy = False
        self.refresh_button.setEnabled(True)
        self.device_note.setText(f"发现 {len(devices)} 台设备" if devices else "未发现设备。请打开 USB 调试，\n并在设备上授权此电脑。")
        self.statusBar().showMessage(f"已连接 {len(devices)} 台设备")
        self.logger.info("扫描完成，发现 %d 台设备", len(devices))
        self.update_actions()
        self.sync_hilog()

        if not notify_changed:
            return

        selected = self.selected_device()
        if old is None or selected is None or old.serial != selected.serial:
            self._active_device_serial = selected.serial if selected else ""
            self.device_changed(self.device_list.currentIndex())
        elif not self.is_device_connected(selected) and hasattr(self, "info_panel") and self.pages.currentWidget() is self.info_panel:
            hint = f"设备 {selected.serial} 尚未连接" if selected else "未检测到已连接的设备"
            self._render_empty_info(hint)
        if self.hilog and self.hilog.serial not in {device.serial for device in devices}:
            self.stop_hilog()
        if not devices and self.projecting:
            self.logger.info("无在线 HDC 设备，投屏中断，不进行自动重连")
            self._reconnect_attempts = 0
            self.stop_mirror(user_initiated=False)
            self.state_badge.setText("已停止")
            self.statusBar().showMessage("无连接设备，投屏已中断", 3000)
        elif devices and self.auto_start_mirror and not self.projecting and not getattr(self, "_user_stopped_mirror", False):
            QTimer.singleShot(250, self._auto_start_mirror_if_needed)

    def _auto_start_mirror_if_needed(self):
        if self.closing or self.projecting or getattr(self, "_user_stopped_mirror", False):
            return
        device = self.selected_device()
        if device and self.hdc and self.is_device_connected(device):
            self.logger.info("默认自动开启投屏：%s", device.serial)
            self.start_mirror()

    def device_changed(self, current, previous=None):
        device = self.selected_device()
        if not device:
            if hasattr(self, "info_panel") and self.pages.currentWidget() is self.info_panel:
                self._render_empty_info("未检测到已连接的设备")
            return

        last_serial = getattr(self, "_active_device_serial", "")
        # 如果是涉及网络设备的切换，统一由 switch_to_single_device 处理断开旧连接、建立新连接并启动新投屏
        if last_serial and last_serial != device.serial and (":" in device.serial or ":" in last_serial):
            self._active_device_serial = device.serial
            self.switch_to_single_device(device.serial)
            return

        self._active_device_serial = device.serial

        was_projecting = self.projecting
        if was_projecting:
            self.logger.info("检测到设备状态变动，迁移投屏至：%s", device.serial if device else "None")
            self.stop_mirror(user_initiated=False)

        if self.hilog and (device is None or self.hilog.serial != device.serial):
            self.stop_hilog()
        if hasattr(self, "webview_session") and self.webview_session.forwarded:
            self.webview_session.stop_forward()
            if hasattr(self, "btn_webview_forward"):
                self.btn_webview_forward.setText("建立映射 (hdc fport)")
                self.btn_webview_forward.setObjectName("Primary")
                self.btn_webview_forward.setStyle(self.btn_webview_forward.style())
            if hasattr(self, "webview_instance_combo"):
                self._reset_webview_instance_combo()
            if hasattr(self, "webview_page_combo"):
                self.webview_page_combo.clear()
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
        is_online = self.is_device_connected(device)
        if not is_online:
            self.subtitle.setText(f"未连接 · {device.serial}")
        else:
            self.subtitle.setText(f"{device.model}  ·  {device.serial}" if device else "选择设备，开始你的调试工作")
        self.update_actions()
        self.refresh_hdc_target_label()
        if device and is_online and self.is_log_active() and self.category.currentData() == "app":
            self.refresh_packages()
        elif device and is_online and self.pages.currentWidget() is getattr(self, "info_panel", None):
            self.refresh_device_info()
        elif hasattr(self, "info_panel") and self.pages.currentWidget() is self.info_panel:
            hint = f"设备 {device.serial} 尚未连接，请在设备中心发起连接" if device else "未检测到已连接的设备"
            self._render_empty_info(hint)
        self.sync_hilog()
        if was_projecting and device and is_online:
            QTimer.singleShot(250, self.start_mirror)

    def update_actions(self):
        connected = self.hdc is not None and self.selected_device() is not None and self.is_device_connected()
        busy = self.projecting or self.deploying
        self.start_button.setEnabled(connected and not busy and not self.scan_busy)
        self.stop_button.setEnabled(self.projecting)
        self.start_button.setVisible(not self.projecting)
        self.stop_button.setVisible(self.projecting)
        self.screenshot_button.setEnabled(connected)
        if hasattr(self, "fault_report_button"):
            self.fault_report_button.setEnabled(connected)
        if hasattr(self, "btn_key_back"):
            self.btn_key_back.setEnabled(connected)
        if hasattr(self, "btn_key_home"):
            self.btn_key_home.setEnabled(connected)
        if hasattr(self, "restore_primary_button"):
            self.restore_primary_button.setEnabled(connected)
        if hasattr(self, "deploy_button"):
            self.deploy_button.setEnabled(connected and not busy and not self.scan_busy)
        self.package_combo.setEnabled(connected)
        self.fetch_button.setEnabled(connected)
        for item in self.key_buttons:
            item.setEnabled(connected)
        if hasattr(self, "btn_device_ops"):
            self.btn_device_ops.setEnabled(connected)
        self.rotate_button.setEnabled(True)
        self.device_list.setEnabled(not self.scan_busy and not self.deploying)
        self.mode.setEnabled(not busy)
        self.refresh_button.setEnabled(not self.scan_busy and not self.deploying)
        if hasattr(self, "reconnect_button"):
            self.reconnect_button.setEnabled(connected and not self.scan_busy)
        self.network_button.setEnabled(self.hdc is not None and not self.deploying)
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
        old_worker = getattr(self, "_mirror_worker", None)
        if old_worker and old_worker.is_alive() and old_worker != threading.current_thread():
            old_worker.join(timeout=0.05)
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
        stream = self.new_stream(device)
        is_stream_installed = False
        if self.mode.currentIndex() == 0:
            try:
                is_stream_installed = stream.installed()
            except Exception:
                is_stream_installed = False

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
                            image._ref = rgb
                            self.publish_frame(token, orient_native_image(image, session.device_size, hw_rot, self.user_rotation), session.device_size, from_video=True)
                        else:
                            width, height, stride, source_width, source_height, pixels = frame
                            image = QImage(pixels, width, height, stride, QImage.Format_RGBA8888)
                            image._ref = pixels
                            dev_size = session.device_size or (source_width, source_height)
                            self.publish_frame(token, orient_native_image(image, dev_size, hw_rot, self.user_rotation), dev_size, from_video=True)
                    session.run(on_frame, lambda state: self.signals.stream_status.emit(token, state))
                except Exception as error:
                    if not session.stop_event.is_set():
                        error_message = str(error)
                if not self.closing:
                    self.signals.stream_finished.emit(token, error_message)
            self._mirror_worker = self.start_worker(native_video, "harmonycast-native")
        elif (self.mode.currentIndex() == 1) or (self.mode.currentIndex() == 0 and is_stream_installed):
            self.stream = stream
            session = self.stream
            def video():
                error_message = ""
                try:
                    if not session.installed():
                        raise HDCError("设备尚未安装投屏服务，请先点击「部署投屏服务」，或切换到兼容预览")
                    def on_video_frame(rgb):
                        hw_rot = getattr(session, "rotation", 0)
                        image = QImage(rgb.data, rgb.shape[1], rgb.shape[0], rgb.strides[0], QImage.Format_RGB888)
                        image._ref = rgb
                        self.publish_frame(token, orient_native_image(image, session.device_size, hw_rot, self.user_rotation), session.device_size, from_video=True)
                    session.run(on_video_frame, lambda state: self.signals.stream_status.emit(token, state))
                except Exception as error:
                    if not session.stop_event.is_set():
                        error_message = str(error)
                if not self.closing:
                    self.signals.stream_finished.emit(token, error_message)
            self._mirror_worker = self.start_worker(video, "harmonycast-video")
        else:
            self.preview_stop = threading.Event()
            stopped = self.preview_stop
            tools = DeviceTools(self.hdc)
            def preview():
                error_message = ""
                consecutive_errors = 0
                try:
                    tools.wakeup(device.serial)
                    while not stopped.is_set():
                        try:
                            try:
                                raw = tools.capture(device.serial, fast=True, stop_event=stopped)
                            except TypeError:
                                try:
                                    raw = tools.capture(device.serial, fast=True)
                                except TypeError:
                                    raw = tools.capture(device.serial)
                            if stopped.is_set() or self.active_token != token:
                                break
                            if not raw:
                                consecutive_errors += 1
                                if consecutive_errors >= 6 or self.closing or self.active_token != token:
                                    raise HDCError("未能从设备获取截图数据")
                                stopped.wait(0.2)
                                continue
                            image = QImage.fromData(raw)
                            if image.isNull():
                                if stopped.is_set() or self.active_token != token:
                                    break
                                raise HDCError("设备返回的截图无法显示")
                            if stopped.is_set():
                                break
                            if self.user_rotation:
                                image = image.transformed(QTransform().rotate(self.user_rotation))
                            self.publish_frame(token, image)
                            self.signals.stream_status.emit(token, "兼容预览 · 截图刷新")
                            consecutive_errors = 0
                        except Exception as frame_err:
                            if stopped.is_set() or self.active_token != token or self.closing:
                                break
                            consecutive_errors += 1
                            if consecutive_errors >= 6 or self.closing or self.active_token != token:
                                raise frame_err
                            self.logger.warning("截图单帧获取失败 (重试 %d/6)：%s", consecutive_errors, frame_err)
                            tools.wakeup(device.serial)
                            stopped.wait(0.2)
                            continue
                        stopped.wait(0.03)
                except Exception as error:
                    if not stopped.is_set() and self.active_token == token:
                        error_message = str(error)
                finally:
                    if not stopped.is_set() and self.active_token == token:
                        try:
                            tools.clean_capture(device.serial)
                        except Exception:
                            pass
                if not self.closing and self.active_token == token and not stopped.is_set():
                    self.signals.stream_finished.emit(token, error_message)
            self._mirror_worker = self.start_worker(preview, "harmonycast-preview")
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
        while not self.frames.empty():
            try:
                self.frames.get_nowait()
            except queue.Empty:
                break
        self.set_projecting(False)
        self.frame_timer.stop()
        if user_initiated:
            self.screen.set_image(QImage())
            self.screen.switching_message = ""
        self.state_badge.setText("已休眠" if not user_initiated else "已停止")
        self.logger.info("投屏已暂停休眠 (切换标签页)" if not user_initiated else "投屏已停止")

    def on_stream_status(self, token, state):
        if token == self.active_token:
            self.state_badge.setText("投屏中" if "投屏" in state or "兼容预览" in state else "正在连接…")
            self.statusBar().showMessage(state)

    def on_stream_finished(self, token, error_message):
        if token != self.active_token:
            return

        device = self.selected_device()
        has_device = bool(device and self.hdc and getattr(self, "devices", None) and any(d.serial == device.serial for d in self.devices))

        # 如果没有 hdc 设备连接，投屏中断，不需要自动重连接
        if not has_device:
            self.logger.info("当前无有效 HDC 设备连接，投屏中断，不执行自动重连")
            self._reconnect_attempts = 0
            self.stop_mirror(user_initiated=False)
            self.screen.switching_message = ""
            self.screen.update()
            self.state_badge.setText("已停止")
            self.statusBar().showMessage("无可用连接设备，投屏已中断", 3000)
            return

        # 仅在设备仍在连接但数据流临时中断时，尝试最多 2 次轻量恢复
        if error_message and not self._user_stopped_mirror and not self.closing:
            if self._reconnect_attempts < 2:
                self._reconnect_attempts += 1
                self.logger.warning("投屏连接中断：%s，正在尝试重连 (%d/2)...", error_message, self._reconnect_attempts)
                self.state_badge.setText(f"重连中 ({self._reconnect_attempts}/2)…")
                self.statusBar().showMessage(f"投屏中断，正在重连 ({self._reconnect_attempts}/2)…")
                if self.stream:
                    self.stream.stop()
                    self.stream = None
                if self.preview_stop:
                    self.preview_stop.set()
                    self.preview_stop = None
                self.set_projecting(False)
                QTimer.singleShot(800, self._auto_reconnect)
                return

        self._reconnect_attempts = 0
        self.stop_mirror(user_initiated=False)
        self.screen.switching_message = ""
        self.screen.update()
        self.state_badge.setText("已停止")
        if error_message:
            self.logger.warning("投屏中断：%s", error_message)
            self.statusBar().showMessage(f"投屏已停止：{error_message}", 3500)

    def _auto_reconnect(self):
        if self._user_stopped_mirror or self.closing:
            return
        device = self.selected_device()
        if not device or not self.hdc or not getattr(self, "devices", None) or not any(d.serial == device.serial for d in self.devices):
            self._reconnect_attempts = 0
            self.stop_mirror(user_initiated=False)
            self.state_badge.setText("未连接设备")
            self.statusBar().showMessage("当前无连接设备，已取消自动重连", 3000)
            return
        self.logger.info("执行自动重连投屏服务 (尝试 %d/2)...", self._reconnect_attempts)
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
        if self.is_log_active():
            if self.hdc and self.selected_device() is None:
                self.refresh_devices()
            self.refresh_packages()

    def set_log_font_size(self, size):
        self.log_view.setStyleSheet(f"QTextEdit {{ font-family: 'Menlo'; font-size: {size}pt; }}")
        if hasattr(self, "webview_console_view"):
            self.webview_console_view.setStyleSheet(
                f"background: #090e15; color: #c2d1df; border: 1px solid #1e2c3d; border-radius: 7px; padding: 8px; font-family: 'Menlo', monospace; font-size: {size}pt;"
            )
        if hasattr(self, "log_font_size") and self.log_font_size.value() != size:
            self.log_font_size.blockSignals(True)
            self.log_font_size.setValue(size)
            self.log_font_size.blockSignals(False)
        if hasattr(self, "webview_font_size") and self.webview_font_size.value() != size:
            self.webview_font_size.blockSignals(True)
            self.webview_font_size.setValue(size)
            self.webview_font_size.blockSignals(False)
        self.settings.setValue("logs/font_size", size)

    def sync_hilog(self):
        if self.closing:
            return
        device = self.selected_device()
        if not self.is_log_active() or not device or not self.hdc:
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
            target_bundle = package if package is not None else bundle
            entry = make_hilog_entry(line, bundle=target_bundle)
            self.file_handler.handle(logging.LogRecord("HarmonyCast", getattr(logging, {"WARN": "WARNING", "FATAL": "CRITICAL"}.get(entry.level, entry.level)), "", 0, "[HiLog %s] %s", (device.serial, line), None))
            try:
                self.messages.put_nowait(entry)
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
        if application_logs and device and self.is_log_active():
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
                or not self.is_log_active()
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
            clean_status = "采集中" if "采集中" in status else status
            self.log_state.setText(clean_status)
            self.log_state.setToolTip(clean_status)
            if hasattr(self, "package_hint"):
                self.package_hint.setText(status)
                self.package_hint.setToolTip(clean_status)
            self.statusBar().showMessage(clean_status)

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
            if isinstance(record, LogEntry):
                new_entries.append(self.store.append_entry(record))
            else:
                level, source, message = record[:3]
                bundle = record[3] if len(record) > 3 else ""
                if "\x1b" in message:
                    message = re.sub(r"\x1b\[[0-9;]*m", "", message)
                new_entries.append(self.store.append(level, source, message, bundle=bundle))
        if new_entries and not self.pause.isChecked() and self.is_log_active():
            filters = self.filters()
            selected = [entry.text() for entry in new_entries if entry.matches(*filters)]
            scroll = self.log_view.verticalScrollBar()
            position = scroll.value()
            is_dragging = scroll.isSliderDown()
            was_at_bottom = (scroll.maximum() - position) <= 4

            anchor_block = None
            doc_layout = None
            offset_within_block = 0
            if not was_at_bottom and not is_dragging:
                top_cursor = self.log_view.cursorForPosition(self.log_view.rect().topLeft())
                anchor_block = top_cursor.block()
                doc_layout = self.log_view.document().documentLayout()
                block_rect = doc_layout.blockBoundingRect(anchor_block)
                offset_within_block = position - int(block_rect.top())

            if selected:
                self.log_view.appendPlainText("\n".join(selected))

            if not is_dragging:
                if self.autoscroll.isChecked() and was_at_bottom:
                    scroll.setValue(scroll.maximum())
                elif anchor_block is not None and anchor_block.isValid():
                    new_rect = doc_layout.blockBoundingRect(anchor_block)
                    new_val = max(0, int(new_rect.top()) + offset_within_block)
                    scroll.setValue(new_val)
                else:
                    scroll.setValue(position)
            self.log_count.setText(f"缓存 {len(self.store.entries)} 条 · 文件自动保存")

    def _on_autoscroll_toggled(self, checked: bool):
        if checked:
            scroll = self.log_view.verticalScrollBar()
            scroll.setValue(scroll.maximum())

    def render_logs(self):
        if self.pause.isChecked():
            return
        entries = self.store.filtered(*self.filters())
        scroll = self.log_view.verticalScrollBar()
        position = scroll.value()
        was_at_bottom = (scroll.maximum() - position) <= 4
        self.log_view.setPlainText("\n".join(entry.text() for entry in entries))
        if self.autoscroll.isChecked() and was_at_bottom:
            scroll.setValue(scroll.maximum())
        else:
            scroll.setValue(position)
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
        connected_serials = self.hdc.devices() if self.hdc else []
        current_serial = self.selected_device().serial if self.selected_device() else None
        dialog = NetworkDeviceDialog(
            self,
            on_connect=self._on_network_connect,
            on_disconnect=self._on_network_disconnect,
            on_reconnect=self._on_network_reconnect,
            on_select=self._on_select_device_from_hub,
            connected_targets=connected_serials,
            current_target=current_serial,
            hdc=self.hdc,
        )
        dialog.exec_()

    def switch_to_single_device(self, endpoint: str):
        if not self.hdc:
            return
        target = endpoint.strip()
        clean_target = target.rsplit(":", 1)[0] if ":" in target else target

        # 递增代数标记，使先前未完成的切换任务及回调自动作废
        self._switch_generation += 1
        gen = self._switch_generation
        self._active_device_serial = target

        # 1. 投屏画面立即断开并展示切换等待加载提示
        self.logger.info("切换设备至 %s (代数 %d)：立即断开当前投屏服务", target, gen)
        self._user_stopped_mirror = False
        self.stop_mirror(user_initiated=False)
        self.screen.set_image(QImage())
        self.screen.switching_message = f"正在连接设备 {target}…"
        self.screen.update()

        # 2. 状态栏与状态徽章展示加载中，暂时禁用下拉框防重复点击
        self.state_badge.setText("正在切换…")
        self.state_badge.setStyleSheet("color: #f3c969; background: #3d2f10; border: 1px solid #735515; padding: 5px 12px; border-radius: 11px;")
        self.statusBar().showMessage(f"正在断开旧设备并连接至 {target}，请稍候…")
        self.device_list.setEnabled(False)

        # 3. 设备信息模块同步置为加载状态，清除旧设备数据
        self.device_info_data = None
        if hasattr(self, "info_badge"):
            self.info_badge.setText("正在加载…")
        if hasattr(self, "card_device"):
            self._render_empty_info(f"正在查询设备 {target} 的硬件与应用信息…")
        if hasattr(self, "btn_refresh_info"):
            self.btn_refresh_info.setEnabled(False)

        # 4. 日志模块同步置为加载状态，停止旧日志采集，重置包名筛选
        self.stop_hilog()
        self.package_token = ""
        self.packages_device = ""
        self.packages_loading = False
        if hasattr(self, "log_state"):
            self.log_state.setText(f"正在连接 {target} 日志…")
        if hasattr(self, "package_hint"):
            self.package_hint.setText(f"正在连接 {target} 并刷新应用列表…")
        self._selected_bundle = str(self.settings.value(f"logs/package/{target}", DEFAULT_PACKAGE_FILTER))
        if hasattr(self, "package_combo"):
            self.package_combo.blockSignals(True)
            self.package_combo.clear()
            self.package_combo.addItems([DEFAULT_PACKAGE_FILTER, "全部应用"])
            if self._selected_bundle and self._selected_bundle not in [DEFAULT_PACKAGE_FILTER, "全部应用"]:
                self.package_combo.addItem(self._selected_bundle)
            self.package_combo.setCurrentText(self._selected_bundle or DEFAULT_PACKAGE_FILTER)
            self.package_combo.blockSignals(False)

        # 5. 常用命令模块同步置为连接中状态
        if hasattr(self, "hdc_target_badge"):
            self.hdc_target_badge.setText(f"目标: {target} (连接中…)")

        def do_switch():
            with self._device_switch_lock:
                if gen != self._switch_generation:
                    self.logger.info("设备切换已被新请求覆盖，取消当前连接任务：%s", target)
                    return []

                # 在后台工作线程等待旧投屏线程安全退出并注销资源，绝不卡死 Qt GUI 主线程
                old_worker = getattr(self, "_mirror_worker", None)
                if old_worker and old_worker.is_alive() and old_worker != threading.current_thread():
                    self.logger.info("后台等待旧投屏线程安全退出后再执行切换...")
                    old_worker.join(timeout=1.5)

                if gen != self._switch_generation:
                    self.logger.info("设备切换已被新请求覆盖，取消当前连接任务：%s", target)
                    return []

                current_targets = []
                try:
                    current_targets = self.hdc.devices()
                except Exception:
                    pass

                # 单设备独占策略：主动断开除目标外的其他网络设备
                for t in current_targets:
                    if ":" in t:
                        clean_t = t.rsplit(":", 1)[0]
                        if clean_t != clean_target and t != target:
                            self.logger.info("单设备独占策略：主动断开旧网络设备 %s", t)
                            try:
                                self.hdc.disconnect(t)
                            except Exception as e:
                                self.logger.warning("断开设备 %s 失败：%s", t, e)

                if gen != self._switch_generation:
                    return []

                # 连接目标网络设备
                if ":" in target:
                    self.logger.info("单设备独占策略：连接目标网络设备 %s", target)
                    try:
                        self.hdc.connect(target)
                    except Exception as e:
                        self.logger.warning("连接目标设备 %s 返回：%s", target, e)
                    try:
                        save_device(clean_target, name="", port=DEFAULT_NETWORK_PORT)
                    except Exception:
                        pass

                if gen != self._switch_generation:
                    return []

                # 获取最新在线设备列表
                online_list = []
                try:
                    online_serials = self.hdc.devices()
                except Exception:
                    online_serials = []

                for serial in online_serials:
                    try:
                        dev = self.hdc.info(serial)
                        online_list.append(dev)
                    except Exception as err:
                        self.logger.warning("获取设备 %s 信息异常: %s", serial, err)
                        online_list.append(Device(serial, "OpenHarmony 设备", "OpenHarmony"))

                if target and not any(d.serial == target or (d.serial.rsplit(":", 1)[0] if ":" in d.serial else d.serial) == clean_target for d in online_list):
                    online_list.append(Device(target, "OpenHarmony 网络设备", "OpenHarmony"))

                return online_list

        def on_switch_done(devices):
            if gen != self._switch_generation:
                self.logger.info("丢弃已作废的设备切换回调：%s (当前代数: %d, 请求代数: %d)", target, self._switch_generation, gen)
                return
            self.logger.info("设备切换完成 (代数 %d)，共 %d 台在线设备，立即同步各模块并启动投屏", gen, len(devices))
            self.device_list.setEnabled(True)
            self.state_badge.setStyleSheet("")

            # 阻止设置过程中重复触发 device_changed
            self.device_list.blockSignals(True)
            self.populate_devices(devices, notify_changed=False)

            target_idx = -1
            for idx in range(self.device_list.count()):
                dev = self.device_list.itemData(idx)
                if isinstance(dev, Device):
                    clean_dev = dev.serial.rsplit(":", 1)[0] if ":" in dev.serial else dev.serial
                    if dev.serial == target or clean_dev == clean_target:
                        target_idx = idx
                        break

            if target_idx >= 0:
                self.device_list.setCurrentIndex(target_idx)
            self.device_list.blockSignals(False)

            self.update_actions()
            self.refresh_hdc_target_label()
            cur_dev = self.selected_device()
            self.subtitle.setText(f"{cur_dev.model if cur_dev else ''}  ·  {target}")

            # 6. 设备信息模块：同步在后台获取并渲染新设备详情
            self.refresh_device_info()

            # 7. 日志模块：更新应用包名并重新开始日志采集
            if self.is_log_active():
                self.refresh_packages()
            self.sync_hilog()

            # 8. 保持屏幕同步提示，直到首帧到达由 set_image 自动清除，杜绝黑屏空白
            self._reconnect_attempts = 0
            self._user_stopped_mirror = False
            self.screen.switching_message = f"已连接 {target}，正在同步首帧画面…"
            self.screen.update()
            self.statusBar().showMessage(f"已稳定连接设备 {target}，各模块同步完成，正在投屏", 3500)
            self.start_mirror()

        self.submit(do_switch, on_switch_done, f"切换至设备 {target}")

    def _on_select_device_from_hub(self, endpoint: str):
        self.switch_to_single_device(endpoint)

    def open_app_launcher(self):
        device = self.selected_device()
        if not device or not self.hdc:
            QMessageBox.information(self, "提示", "请先连接并选择目标设备")
            return
        dialog = AppLauncherDialog(self, hdc=self.hdc, serial=device.serial)
        dialog.exec_()

    def launch_primary_app_shortcut(self):
        device = self.selected_device()
        if not device or not self.hdc:
            return
        self.statusBar().showMessage("正在检测并拉起业务主程序…")
        tools = DeviceTools(self.hdc)
        def do_launch():
            return tools.launch_primary_app(device.serial)
        def on_done(res):
            if res:
                self.statusBar().showMessage(f"已成功拉起业务主程序：{res}", 3500)
            else:
                self.statusBar().showMessage("未检测到预置业务程序，已为您打开应用拉起列表", 3000)
                self.open_app_launcher()
        self.submit(do_launch, on_done, "拉起业务主程序")

    def _on_network_connect(self, endpoint: str):
        if not self.hdc:
            QMessageBox.warning(self, "连接失败", "未找到 HDC 工具，请在设置中配置")
            return
        self.switch_to_single_device(endpoint)

    def _on_network_disconnect(self, endpoint: str):
        if not self.hdc:
            return
        cur = self.selected_device()
        if cur and (cur.serial == endpoint or endpoint.startswith(cur.serial)):
            if self.projecting:
                self.stop_mirror(user_initiated=True)
        self.submit(lambda: self.hdc.disconnect(endpoint), lambda result: self.refresh_devices(), f"断开设备 {endpoint}")

    def _on_network_reconnect(self, endpoint: str):
        if not self.hdc:
            return
        was_projecting = self.projecting
        cur = self.selected_device()
        if cur and (cur.serial == endpoint or endpoint.startswith(cur.serial)) and was_projecting:
            self.stop_mirror(user_initiated=False)

        def do_reconn():
            return self.hdc.reconnect(endpoint)

        def on_done(res):
            self.refresh_devices()
            if was_projecting:
                QTimer.singleShot(600, self.start_mirror)

        self.submit(do_reconn, on_done, f"重连设备 {endpoint}")

    def disconnect_current_device(self):
        device = self.selected_device()
        if not device or not self.hdc:
            self.statusBar().showMessage("当前无可用设备", 3000)
            return
        serial = device.serial
        is_network = ":" in serial
        was_projecting = self.projecting

        self.logger.info("用户主动断开设备：%s (网络设备: %s, 投屏中: %s)", serial, is_network, was_projecting)
        if was_projecting:
            self.stop_mirror(user_initiated=True)

        if is_network:
            self.state_badge.setText("已断开")
            self.statusBar().showMessage(f"正在断开网络设备 {serial} (-remove)…")

            def do_disconnect():
                return self.hdc.disconnect(serial)

            def on_disconnect_done(result):
                self.statusBar().showMessage(f"已断开网络设备：{serial}", 3500)
                self.refresh_devices()

            self.submit(do_disconnect, on_disconnect_done, f"断开设备 {serial}")
        else:
            self.state_badge.setText("已停止")
            self.statusBar().showMessage(f"已停止设备 {serial} 投屏", 3000)
            self.refresh_devices()

    def reconnect_current_device(self):
        device = self.selected_device()
        if not device or not self.hdc:
            return
        serial = device.serial
        is_network = ":" in serial
        was_projecting = self.projecting

        self.logger.info("执行断开重连：%s (网络设备: %s, 投屏中: %s)", serial, is_network, was_projecting)
        self.state_badge.setText("重连中…")
        self.statusBar().showMessage(f"正在断开重连 {serial}…")

        if was_projecting:
            self.stop_mirror(user_initiated=False)

        def do_reconnect():
            if is_network:
                return self.hdc.reconnect(serial)
            else:
                time.sleep(0.4)
                return "ok"

        def on_reconnect_done(result):
            self.statusBar().showMessage(f"已完成 {serial} 断开重连", 3000)
            self.refresh_devices()
            if was_projecting:
                QTimer.singleShot(600, self.start_mirror)

        self.submit(do_reconnect, on_reconnect_done, f"断开重连 {serial}")

    def quick_save_device(self, ip: str, iface: str = ""):
        dev = self.selected_device()
        model = dev.model if dev else "OpenHarmony 设备"
        name = f"{model} ({iface})" if iface else model
        try:
            save_device(ip, name=name, port=DEFAULT_NETWORK_PORT)
            self.statusBar().showMessage(f"已将 {name} ({ip}:8710) 存入常用设备列表", 4000)
        except Exception as error:
            QMessageBox.warning(self, "保存失败", str(error))

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
        for option in (mode, frame_rate, bitrate, quality):
            setup_combobox_popup(option, 160)
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

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._update_responsive_toolbars()
        if hasattr(self, "_toast_widget") and self._toast_widget and self._toast_widget.isVisible():
            self._toast_widget.reposition()

    def _update_responsive_toolbars(self):
        """动态响应窗口及各分屏面板宽度变化，平滑折叠辅助按钮文字，杜绝截断与溢出"""
        # 1. 投屏顶部工具栏
        if hasattr(self, "mirror_page") and hasattr(self, "fullscreen_button") and hasattr(self, "screenshot_button"):
            mw = self.mirror_page.width()
            if mw < 460:
                self.fullscreen_button.setText("")
                self.fullscreen_button.setFixedWidth(28)
                self.fullscreen_button.setToolTip("全屏演示 (F11)")
                self.screenshot_button.setText("")
                self.screenshot_button.setFixedWidth(28)
                self.screenshot_button.setToolTip("保存高清截图 (⌘⇧S)")
            else:
                self.fullscreen_button.setText("全屏")
                self.fullscreen_button.setMinimumWidth(0)
                self.fullscreen_button.setMaximumWidth(16777215)
                self.screenshot_button.setText("截图")
                self.screenshot_button.setMinimumWidth(0)
                self.screenshot_button.setMaximumWidth(16777215)

        # 2. WebView 工具栏
        if hasattr(self, "webview_panel") and hasattr(self, "btn_webview_chrome") and hasattr(self, "btn_webview_reload"):
            ww = self.webview_panel.width()
            if ww < 660:
                self.btn_webview_chrome.setText("")
                self.btn_webview_chrome.setFixedWidth(28)
                self.btn_webview_chrome.setToolTip("Chrome DevTools")
                self.btn_webview_reload.setText("")
                self.btn_webview_reload.setFixedWidth(28)
                self.btn_webview_reload.setToolTip("刷新页面")
                if hasattr(self, "btn_webview_export"):
                    self.btn_webview_export.setText("")
                    self.btn_webview_export.setFixedWidth(28)
                    self.btn_webview_export.setToolTip("导出日志")
                self.btn_webview_clear.setText("")
                self.btn_webview_clear.setFixedWidth(28)
                self.btn_webview_clear.setToolTip("清空记录")
            else:
                self.btn_webview_chrome.setText("Chrome")
                self.btn_webview_chrome.setMinimumWidth(0)
                self.btn_webview_chrome.setMaximumWidth(16777215)
                self.btn_webview_reload.setText("刷新")
                self.btn_webview_reload.setMinimumWidth(0)
                self.btn_webview_reload.setMaximumWidth(16777215)
                if hasattr(self, "btn_webview_export"):
                    self.btn_webview_export.setText("导出日志")
                    self.btn_webview_export.setMinimumWidth(0)
                    self.btn_webview_export.setMaximumWidth(16777215)
                self.btn_webview_clear.setText("清空")
                self.btn_webview_clear.setMinimumWidth(0)
                self.btn_webview_clear.setMaximumWidth(16777215)

        # 3. 顶部 Header
        if hasattr(self, "header") and hasattr(self, "refresh_button") and hasattr(self, "disconnect_button"):
            hw = self.header.width()
            if hw < 820:
                self.refresh_button.setText("")
                self.refresh_button.setFixedWidth(28)
                self.refresh_button.setToolTip("刷新设备列表 (F5)")
                self.disconnect_button.setText("")
                self.disconnect_button.setFixedWidth(28)
                self.disconnect_button.setToolTip("断开当前设备")
            else:
                self.refresh_button.setText("刷新")
                self.refresh_button.setMinimumWidth(0)
                self.refresh_button.setMaximumWidth(16777215)
                self.disconnect_button.setText("断开")
                self.disconnect_button.setMinimumWidth(0)
                self.disconnect_button.setMaximumWidth(16777215)

        # 4. HiLog 日志操作栏
        if hasattr(self, "hilog_widget") and hasattr(self, "clear_button"):
            hw = self.hilog_widget.width()
            if hw < 560:
                self.clear_button.setText("")
                self.clear_button.setFixedWidth(28)
                self.clear_button.setToolTip("清空日志")
            else:
                self.clear_button.setText("清空")
                self.clear_button.setMinimumWidth(0)
                self.clear_button.setMaximumWidth(16777215)

    def closeEvent(self, event):
        self.closing = True
        if hasattr(self, "webview_session"):
            try:
                self.webview_session.disconnect_page()
                self.webview_session.stop_forward()
            except Exception:
                pass
        if hasattr(self, "timer") and self.timer:
            self.timer.stop()
        if hasattr(self, "frame_timer") and self.frame_timer:
            self.frame_timer.stop()
        if hasattr(self, "log_timer") and self.log_timer:
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


def _activate_macos_app():
    if sys.platform != "darwin":
        return
    try:
        import ctypes, ctypes.util
        objc = ctypes.cdll.LoadLibrary(ctypes.util.find_library("objc"))
        ctypes.cdll.LoadLibrary(ctypes.util.find_library("AppKit"))
        objc.objc_getClass.restype = ctypes.c_void_p
        objc.sel_registerName.restype = ctypes.c_void_p
        NSApplication = objc.objc_getClass(b"NSApplication")
        sharedAppSel = objc.sel_registerName(b"sharedApplication")
        objc.objc_msgSend.restype = ctypes.c_void_p
        objc.objc_msgSend.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        app = objc.objc_msgSend(NSApplication, sharedAppSel)
        activateSel = objc.sel_registerName(b"activateIgnoringOtherApps:")
        objc.objc_msgSend.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_bool]
        objc.objc_msgSend(app, activateSel, True)
    except Exception:
        pass


def run() -> int:
    app = QApplication([])
    app.setApplicationName("HarmonyCast")
    app.setOrganizationName("HarmonyCast")
    app.setStyle("Fusion")
    app.setPalette(get_dark_palette())
    app.setStyleSheet(STYLE)
    app.setWindowIcon(QIcon(str(resource_dir() / "app-icon.svg")))
    window = MainWindow()
    window.show()
    window.raise_()
    window.activateWindow()
    _activate_macos_app()
    return app.exec_()
