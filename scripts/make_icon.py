"""Render the code-native icon into Apple's .iconset input."""
import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PyQt5.QtCore import Qt
from PyQt5.QtGui import QImage, QPainter
from PyQt5.QtSvg import QSvgRenderer
from PyQt5.QtWidgets import QApplication

root = Path(__file__).resolve().parent.parent
app = QApplication([])
renderer = QSvgRenderer(str(root / "resources/app-icon.svg"))
directory = root / "build/HarmonyCast.iconset"
directory.mkdir(parents=True, exist_ok=True)
for size in (16, 32, 128, 256, 512):
    for scale in (1, 2):
        image = QImage(size * scale, size * scale, QImage.Format_ARGB32)
        image.fill(Qt.transparent)
        painter = QPainter(image)
        renderer.render(painter)
        painter.end()
        suffix = "@2x" if scale == 2 else ""
        image.save(str(directory / f"icon_{size}x{size}{suffix}.png"))
