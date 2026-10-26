import sys
import os
from PyQt5.QtWidgets import QApplication
from PyQt5.QtGui import (
    QImage, QPainter, QLinearGradient, QRadialGradient, 
    QColor, QFont, QPen, QBrush, QPolygonF
)
from PyQt5.QtCore import Qt, QPointF, QRectF

def generate_background(output_path, width=800, height=480):
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    app = QApplication(['-platform', 'offscreen'])
    
    img = QImage(width, height, QImage.Format_ARGB32)
    img.fill(QColor("#f4f7fa"))
    
    p = QPainter(img)
    p.setRenderHint(QPainter.Antialiasing, True)
    p.setRenderHint(QPainter.TextAntialiasing, True)
    p.setRenderHint(QPainter.SmoothPixmapTransform, True)
    
    # 1. Base Gradient - Clean Apple Light Tech Theme
    bg_grad = QLinearGradient(0, 0, 0, height)
    bg_grad.setColorAt(0.0, QColor("#fbfcfd"))
    bg_grad.setColorAt(0.5, QColor("#f3f6f9"))
    bg_grad.setColorAt(1.0, QColor("#e8edf2"))
    p.fillRect(0, 0, width, height, bg_grad)
    
    # 2. Subtle radial glow in the middle
    glow = QRadialGradient(330.0, 190.0, 260.0)
    glow.setColorAt(0.0, QColor(0, 166, 136, 25))
    glow.setColorAt(0.7, QColor(0, 166, 136, 8))
    glow.setColorAt(1.0, QColor(243, 246, 249, 0))
    p.fillRect(0, 0, width, height, glow)
    
    # 3. Icon Pedestal / Drop Target Cards (Frosted Glass pedestals)
    # Icon 1 center at x=170, y=190
    card1_rect = QRectF(170 - 75, 190 - 72, 150, 172)
    card_pen = QPen(QColor(215, 226, 235, 180), 1.2)
    card_brush = QBrush(QColor(255, 255, 255, 220))
    
    p.setPen(card_pen)
    p.setBrush(card_brush)
    p.drawRoundedRect(card1_rect, 16, 16)
    
    # Icon 2 center at x=490, y=190
    card2_rect = QRectF(490 - 75, 190 - 72, 150, 172)
    p.drawRoundedRect(card2_rect, 16, 16)
    
    # 4. Header Text (Visible & crisp dark text)
    font_title = QFont("PingFang SC", 20, QFont.Bold)
    p.setFont(font_title)
    
    p.setPen(QColor("#0d2e2b"))
    p.drawText(QRectF(0, 32, 660, 32), Qt.AlignCenter, "HarmonyCast  鸿蒙投屏")
    
    font_sub = QFont("PingFang SC", 11)
    p.setFont(font_sub)
    p.setPen(QColor("#546e82"))
    p.drawText(QRectF(0, 68, 660, 22), Qt.AlignCenter, "新一代 OpenHarmony / 鸿蒙设备高性能投屏与研发联调平台")
    
    # 5. Middle Guidance Arrow (between x=265 and x=395, y=190)
    arrow_y = 190.0
    arrow_x1 = 265.0
    arrow_x2 = 395.0
    
    arrow_grad = QLinearGradient(arrow_x1, arrow_y, arrow_x2, arrow_y)
    arrow_grad.setColorAt(0.0, QColor(0, 166, 136, 100))
    arrow_grad.setColorAt(1.0, QColor(0, 166, 136, 255))
    
    # Glow behind arrow
    p.setPen(QPen(QColor(0, 166, 136, 40), 10, Qt.SolidLine, Qt.RoundCap))
    p.drawLine(QPointF(arrow_x1, arrow_y), QPointF(arrow_x2, arrow_y))
    
    p.setPen(QPen(QBrush(arrow_grad), 4.0, Qt.SolidLine, Qt.RoundCap))
    p.drawLine(QPointF(arrow_x1, arrow_y), QPointF(arrow_x2, arrow_y))
    
    # Arrow Head
    arrow_head = QPolygonF([
        QPointF(arrow_x2 - 5.0, arrow_y - 10.0),
        QPointF(arrow_x2 + 12.0, arrow_y),
        QPointF(arrow_x2 - 5.0, arrow_y + 10.0),
        QPointF(arrow_x2 + 1.0, arrow_y),
    ])
    p.setPen(Qt.NoPen)
    p.setBrush(QColor("#00a688"))
    p.drawPolygon(arrow_head)
    
    # Text below Arrow
    font_hint = QFont("PingFang SC", 11, QFont.Bold)
    p.setFont(font_hint)
    p.setPen(QColor("#008f75"))
    p.drawText(QRectF(0, 220, 660, 24), Qt.AlignCenter, "拖拽至 Applications 完成安装")
    
    # 6. Bottom footer info
    p.setPen(QPen(QColor(215, 226, 235), 1))
    p.drawLine(QPointF(40, 355), QPointF(620, 355))
    
    font_bot = QFont("Helvetica Neue", 10)
    p.setFont(font_bot)
    p.setPen(QColor("#70889b"))
    p.drawText(QRectF(0, 362, 660, 22), Qt.AlignCenter, "macOS (Apple Silicon & Intel) · 现代 C++20 + Qt6 原生架构 · 极低延迟 60 FPS")
    
    p.end()
    ok = img.save(output_path, "PNG")
    print(f"Background saved to {output_path} ({width}x{height}): {ok}")
    return ok

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else "resources/dmg_background.png"
    generate_background(out)
