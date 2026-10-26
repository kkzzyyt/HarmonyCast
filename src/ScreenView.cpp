#include "harmonycast/ScreenView.h"
#include <QPainter>
#include <QMouseEvent>
#include <QPen>
#include <QColor>
#include <QFont>
#include <cmath>

namespace harmonycast {

ScreenView::ScreenView(QWidget* parent)
    : QWidget(parent), m_fade_timer(new QTimer(this)) {
    setMinimumSize(120, 120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);

    m_fade_timer->setInterval(16);
    connect(m_fade_timer, &QTimer::timeout, this, &ScreenView::onFadeTick);
}

void ScreenView::clear() {
    m_image = QImage();
    m_target_rect = QRect();
    m_touch_active = false;
    m_ripple_opacity = 0.0f;
    update();
}

void ScreenView::setImage(const QImage& img, int dev_w, int dev_h) {
    m_image = img;
    m_dev_width = (dev_w > 0) ? dev_w : img.width();
    m_dev_height = (dev_h > 0) ? dev_h : img.height();
    updateTargetRect();
    update();
}

void ScreenView::updateFrame(const DecodedFrame& frame) {
    if (frame.data.empty()) return;

    QImage::Format fmt = QImage::Format_RGB888;
    if (frame.format == 4) {
        fmt = QImage::Format_RGB32;
    } else if (frame.format == 5 || frame.format == 6) {
        fmt = QImage::Format_ARGB32_Premultiplied;
    } else if (frame.stride == frame.width * 4 || frame.format == 12 || frame.format == 26) {
        fmt = QImage::Format_RGBA8888;
    }

    // Zero-copy wrapping: create QImage directly referencing frame data
    QImage img(frame.data.data(), frame.width, frame.height, frame.stride, fmt);
    m_image = img.copy(); // shallow copy if shared or deep copy once to survive callback
    m_dev_width = frame.width;
    m_dev_height = frame.height;

    updateTargetRect();
    update();
}

void ScreenView::updateTargetRect() {
    if (m_image.isNull() || width() <= 0 || height() <= 0) {
        m_target_rect = QRect();
        return;
    }

    QSize img_size = m_image.size();
    if (m_rotation % 180 != 0) {
        img_size.transpose();
    }

    QSize scaled = img_size.scaled(size(), Qt::KeepAspectRatio);
    int left = (width() - scaled.width()) / 2;
    int top = (height() - scaled.height()) / 2;
    m_target_rect = QRect(left, top, scaled.width(), scaled.height());
}

void ScreenView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateTargetRect();
}

void ScreenView::onFadeTick() {
    m_ripple_radius += 1.8f;
    m_ripple_opacity -= 0.10f;
    if (m_ripple_opacity <= 0.0f) {
        m_ripple_opacity = 0.0f;
        m_trail.clear();
        m_fade_timer->stop();
    }
    update();
}

std::pair<int, int> ScreenView::mapToDevice(const QPoint& pos) const {
    if (!m_target_rect.isValid() || m_target_rect.width() <= 0 || m_target_rect.height() <= 0) {
        return {-1, -1};
    }

    if (!m_target_rect.contains(pos)) {
        return {-1, -1};
    }

    int rel_x = pos.x() - m_target_rect.left();
    int rel_y = pos.y() - m_target_rect.top();

    int dev_w = (m_dev_width > 0) ? m_dev_width : m_image.width();
    int dev_h = (m_dev_height > 0) ? m_dev_height : m_image.height();

    double norm_x = static_cast<double>(rel_x) / m_target_rect.width();
    double norm_y = static_cast<double>(rel_y) / m_target_rect.height();

    int final_x = static_cast<int>(norm_x * dev_w);
    int final_y = static_cast<int>(norm_y * dev_h);

    if (m_rotation == 90) {
        return {final_y, dev_h - 1 - final_x};
    } else if (m_rotation == 180) {
        return {dev_w - 1 - final_x, dev_h - 1 - final_y};
    } else if (m_rotation == 270) {
        return {dev_w - 1 - final_y, final_x};
    }
    return {final_x, final_y};
}

void ScreenView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#09111a"));

    // 1. Placeholder screen if no video
    if (m_image.isNull()) {
        painter.setRenderHint(QPainter::Antialiasing);
        int cx = width() / 2;
        int cy = height() / 2 - 40;

        // Device Silhouette
        painter.setPen(QPen(QColor("#355a68"), 2));
        painter.drawRoundedRect(cx - 26, cy - 42, 52, 84, 8, 8);
        painter.drawLine(cx - 10, cy + 28, cx + 10, cy + 28);

        // Text labels
        painter.setPen(QColor("#cfdfeb"));
        painter.setFont(QFont("PingFang SC", 15, QFont::Medium));
        if (!m_status_msg.isEmpty()) {
            painter.drawText(0, cy + 65, width(), 30, Qt::AlignCenter, m_status_msg);
        } else if (m_active) {
            painter.drawText(0, cy + 65, width(), 30, Qt::AlignCenter, "正在连接设备画面…");
        } else {
            painter.drawText(0, cy + 65, width(), 30, Qt::AlignCenter, "让设备画面来到桌面");
            painter.setFont(QFont("PingFang SC", 12));
            painter.setPen(QColor("#748ca2"));
            painter.drawText(0, cy + 98, width(), 25, Qt::AlignCenter, "连接 USB 设备，点击下方「开始投屏」");
        }
        return;
    }

    // 2. Hardware/Smooth Video Frame Drawing
    if (!m_target_rect.isValid()) {
        updateTargetRect();
    }

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (m_rotation != 0) {
        painter.save();
        painter.translate(m_target_rect.center());
        painter.rotate(m_rotation);

        double dw = (m_rotation % 180 != 0) ? m_target_rect.height() : m_target_rect.width();
        double dh = (m_rotation % 180 != 0) ? m_target_rect.width() : m_target_rect.height();

        painter.drawImage(QRectF(-dw / 2.0, -dh / 2.0, dw, dh), m_image);
        painter.restore();
    } else {
        painter.drawImage(m_target_rect, m_image);
    }

    // 3. Touch Ripple & Gesture Trail
    if (m_touch_active || m_ripple_opacity > 0.0f) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        if (m_target_rect.isValid()) painter.setClipRect(m_target_rect);

        // Gesture Trail
        if (m_trail.size() >= 2) {
            for (size_t i = 0; i < m_trail.size() - 1; ++i) {
                float ratio = static_cast<float>(i + 1) / m_trail.size();
                int alpha = static_cast<int>(140 * ratio * (m_touch_active ? 1.0f : m_ripple_opacity));
                painter.setPen(QPen(QColor(83, 214, 181, alpha), 3.0f + ratio * 3.0f, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(m_trail[i], m_trail[i + 1]);
            }
        }

        // Ripple Glow
        float eff_op = m_touch_active ? 1.0f : m_ripple_opacity;
        int outer_alpha = std::clamp(static_cast<int>(160 * eff_op), 0, 255);
        int bg_alpha = std::clamp(static_cast<int>(45 * eff_op), 0, 255);
        painter.setPen(QPen(QColor(83, 214, 181, outer_alpha), 2.0f));
        painter.setBrush(QColor(83, 214, 181, bg_alpha));
        painter.drawEllipse(m_touch_pos, static_cast<int>(m_ripple_radius), static_cast<int>(m_ripple_radius));

        // Inner Touch Dot
        painter.setPen(QPen(QColor(83, 214, 181, 240), 1.5f));
        painter.setBrush(QColor(255, 255, 255, 240));
        painter.drawEllipse(m_touch_pos, 5, 5);

        painter.restore();
    }
}

void ScreenView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (m_image.isNull()) {
            emit startRequested();
            return;
        }
        m_touch_active = true;
        m_touch_pos = event->pos();
        m_trail.clear();
        m_trail.push_back(m_touch_pos);
        m_ripple_radius = 16.0f;
        m_ripple_opacity = 1.0f;

        auto [dev_x, dev_y] = mapToDevice(event->pos());
        if (dev_x >= 0 && dev_y >= 0 && m_touch_cb) {
            m_touch_cb(TouchAction::Down, dev_x, dev_y, m_dev_width, m_dev_height);
        }
        update();
    }
}

void ScreenView::mouseMoveEvent(QMouseEvent* event) {
    if (m_touch_active) {
        m_touch_pos = event->pos();
        m_trail.push_back(m_touch_pos);
        if (m_trail.size() > 24) {
            m_trail.erase(m_trail.begin());
        }

        auto [dev_x, dev_y] = mapToDevice(event->pos());
        if (dev_x >= 0 && dev_y >= 0 && m_touch_cb) {
            m_touch_cb(TouchAction::Move, dev_x, dev_y, m_dev_width, m_dev_height);
        }
        update();
    }
}

void ScreenView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_touch_active) {
        m_touch_active = false;
        m_fade_timer->start();

        auto [dev_x, dev_y] = mapToDevice(event->pos());
        if (dev_x >= 0 && dev_y >= 0 && m_touch_cb) {
            m_touch_cb(TouchAction::Up, dev_x, dev_y, m_dev_width, m_dev_height);
        }
        update();
    }
}

} // namespace harmonycast
