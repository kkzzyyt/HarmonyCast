import sys
import os
from PyQt5.QtWidgets import QApplication
from PyQt5.QtGui import (
    QImage, QPainter, QLinearGradient, QRadialGradient, 
    QColor, QFont, QPen, QBrush, QPolygonF
)
from PyQt5.QtCore import Qt, QPointF, QRectF

def generate_background(output_path, width=660, height=400):
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    app = QApplication(['-platform', 'offscreen'])
    
    img = QImage(width, height, QImage.Format_ARGB32)
    img.fill(QColor("#080d14"))
    
    p = QPainter(img)
    p.setRenderHint(QPainter.Antialiasing, True)
    p.setRenderHint(QPainter.TextAntialiasing, True)
    p.setRenderHint(QPainter.SmoothPixmapTransform, True)
    
    # 1. Background Gradient
    bg_grad = QLinearGradient(0, 0, 0, height)
    bg_grad.setColorAt(0.0, QColor("#0d1724"))
    bg_grad.setColorAt(0.5, QColor("#0a121c"))
    bg_grad.setColorAt(1.0, QColor("#060a0f"))
    p.fillRect(0, 0, width, height, bg_grad)
    
    # 2. Ambient Glow in center (behind the arrow)
    glow = QRadialGradient(width / 2.0, 190.0, 220.0)
    glow.setColorAt(0.0, QColor(92, 224, 193, 35))
    glow.setColorAt(0.6, QColor(25, 60, 75, 15))
    glow.setColorAt(1.0, QColor(10, 18, 28, 0))
    p.fillRect(0, 0, width, height, glow)
    
    # 3. Outer subtle frame
    p.setPen(QPen(QColor(30, 52, 75, 120), 1))
    p.setBrush(Qt.NoBrush)
    p.drawRoundedRect(QRectF(1.5, 1.5, width - 3.0, height - 3.0), 12, 12)
    
    # 4. Header Text
    font_title = QFont("PingFang SC", 20, QFont.Bold)
    p.setFont(font_title)
    
    # Draw HarmonyCast title
    p.setPen(QColor("#5ce0c1"))
    p.drawText(QRectF(0, 36, width, 32), Qt.AlignCenter, "HarmonyCast  鸿蒙投屏")
    
    font_sub = QFont("PingFang SC", 11)
    p.setFont(font_sub)
    p.setPen(QColor("#7d95ab"))
    p.drawText(QRectF(0, 72, width, 22), Qt.AlignCenter, "新一代 OpenHarmony / 鸿蒙设备高性能投屏与研发联调平台")
    
    # 5. Drag-and-Drop Guidance Arrow (between x=260 and x=400, y=190)
    arrow_y = 190.0
    arrow_x1 = 265.0
    arrow_x2 = 385.0
    
    # Arrow Line with gradient
    arrow_grad = QLinearGradient(arrow_x1, arrow_y, arrow_x2, arrow_y)
    arrow_grad.setColorAt(0.0, QColor(92, 224, 193, 80))
    arrow_grad.setColorAt(1.0, QColor(92, 224, 193, 255))
    
    # Glow behind arrow
    p.setPen(QPen(QColor(92, 224, 193, 50), 8, Qt.SolidLine, Qt.RoundCap))
    p.drawLine(QPointF(arrow_x1, arrow_y), QPointF(arrow_x2, arrow_y))
    
    p.setPen(QPen(QBrush(arrow_grad), 3.5, Qt.SolidLine, Qt.RoundCap))
    p.drawLine(QPointF(arrow_x1, arrow_y), QPointF(arrow_x2, arrow_y))
    
    # Arrow Head
    arrow_head = QPolygonF([
        QPointF(arrow_x2 - 4.0, arrow_y - 9.0),
        QPointF(arrow_x2 + 10.0, arrow_y),
        QPointF(arrow_x2 - 4.0, arrow_y + 9.0),
        QPointF(arrow_x2 + 1.0, arrow_y),
    ])
    p.setPen(Qt.NoPen)
    p.setBrush(QColor("#5ce0c1"))
    p.drawPolygon(arrow_head)
    
    # Text below Arrow
    font_hint = QFont("PingFang SC", 11, QFont.Bold)
    p.setFont(font_hint)
    p.setPen(QColor("#5ce0c1"))
    p.drawText(QRectF(0, 222, width, 24), Qt.AlignCenter, "拖拽至此处完成安装")
    
    # 6. Bottom info
    p.setPen(QPen(QColor(25, 45, 65, 100), 1))
    p.drawLine(QPointF(40, height - 42), QPointF(width - 40, height - 42))
    
    font_bot = QFont("Helvetica Neue", 10)
    p.setFont(font_bot)
    p.setPen(QColor("#486278"))
    p.drawText(QRectF(0, height - 36, width, 22), Qt.AlignCenter, "macOS (Apple Silicon & Intel) · 现代 C++20 + Qt6 原生架构 · 极低延迟 60 FPS")
    
    p.end()
    ok = img.save(output_path, "PNG")
    print(f"Background saved to {output_path}: {ok}")
    return ok

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else "resources/dmg_background.png"
    generate_background(out)
