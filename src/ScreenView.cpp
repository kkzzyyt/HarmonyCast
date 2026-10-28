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
    m_scaled_cache = QImage();
    m_cache_dirty = false;
    m_target_rect = QRect();
    m_touch_active = false;
    m_ripple_opacity = 0.0f;
    update();
}

void ScreenView::setRotation(int degrees) {
    int r = (degrees % 360 + 360) % 360;
    if (m_rotation != r) {
        m_rotation = r;
        m_cache_dirty = true;
        updateTargetRect();
        emit presentationChanged(size(), m_rotation, devicePixelRatioF());
        update();
    }
}

void ScreenView::setImage(const QImage& img, int dev_w, int dev_h) {
    m_image = img;
    m_dev_width = (dev_w > 0) ? dev_w : img.width();
    m_dev_height = (dev_h > 0) ? dev_h : img.height();
    m_cache_dirty = true;
    updateTargetRect();
    update();
}

void ScreenView::updateFrame(const DecodedFrame& frame) {
    if (!frame.image.isNull()) {
        m_image = frame.image;
    } else {
        // Preserve compatibility with callers supplying an external byte buffer.
        if (frame.width <= 0 || frame.height <= 0 || frame.stride <= 0 ||
            size_t(frame.stride) * size_t(frame.height) > frame.data.size()) return;
        QImage::Format fmt = QImage::Format_RGB888;
        if (frame.format == 4) fmt = QImage::Format_RGB32;
        else if (frame.format == 5 || frame.format == 6) fmt = QImage::Format_ARGB32_Premultiplied;
        else if (frame.stride == frame.width * 4 || frame.format == 12 || frame.format == 26) fmt = QImage::Format_RGBA8888;
        m_image = QImage(frame.data.data(), frame.width, frame.height, frame.stride, fmt).copy();
    }
    m_dev_width = frame.source_width > 0 ? frame.source_width : m_image.width();
    m_dev_height = frame.source_height > 0 ? frame.source_height : m_image.height();
    m_status_msg.clear();
    m_cache_dirty = true;
    updateTargetRect();
    if (!frame.presentation_image.isNull() && frame.presentation_rotation == m_rotation &&
        frame.presentation_image.size() == m_target_rect.size() * devicePixelRatioF()) {
        m_scaled_cache = frame.presentation_image;
        m_cache_dirty = false;
    }
    update();
}

void ScreenView::updateTargetRect() {
    if (m_image.isNull() || width() <= 0 || height() <= 0) {
        m_target_rect = QRect();
        m_scaled_cache = QImage();
        m_cache_dirty = false;
        return;
    }

    QSize img_size = m_image.size();
    if (m_rotation % 180 != 0) {
        img_size.transpose();
    }

    QSize scaled = img_size.scaled(size(), Qt::KeepAspectRatio);
    int left = (width() - scaled.width()) / 2;
    int top = (height() - scaled.height()) / 2;
    QRect new_rect(left, top, scaled.width(), scaled.height());
    if (m_target_rect != new_rect) {
        m_target_rect = new_rect;
        m_cache_dirty = true;
    }
}

void ScreenView::regenerateDisplayCache() {
    if (m_image.isNull() || !m_target_rect.isValid() || m_target_rect.isEmpty()) {
        m_scaled_cache = QImage();
        m_cache_dirty = false;
        return;
    }

    const qreal dpr = devicePixelRatioF();
    QSize physical_target = m_target_rect.size() * dpr;
    if (physical_target.width() <= 0 || physical_target.height() <= 0) {
        m_scaled_cache = QImage();
        m_cache_dirty = false;
        return;
    }

    // Handle rotation first
    QImage source = m_image;
    if (m_rotation != 0) {
        QTransform transform;
        transform.rotate(m_rotation);
        source = m_image.transformed(transform);
    }

    // High-DPI physical pixel scaling:
    // If physical target matches source size, keep 1:1 without resampling.
    // Otherwise use multi-tap area-averaged SmoothTransformation (Lanczos) for pixel-crisp Retina text.
    if (source.size() == physical_target) {
        m_scaled_cache = source;
    } else {
        m_scaled_cache = source.scaled(physical_target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    m_cache_dirty = false;
}

void ScreenView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateTargetRect();
    m_cache_dirty = true;
    emit presentationChanged(isVisible() ? size() : QSize{}, m_rotation, devicePixelRatioF());
}

void ScreenView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    m_scaled_cache = QImage();
    m_cache_dirty = true;
    emit presentationChanged({}, m_rotation, devicePixelRatioF());
}

void ScreenView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    emit presentationChanged(size(), m_rotation, devicePixelRatioF());
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

    if (m_rotation == 90) {
        int final_x = static_cast<int>(norm_y * dev_w);
        int final_y = static_cast<int>((1.0 - norm_x) * dev_h);
        return {std::clamp(final_x, 0, dev_w - 1), std::clamp(final_y, 0, dev_h - 1)};
    } else if (m_rotation == 180) {
        int final_x = static_cast<int>((1.0 - norm_x) * dev_w);
        int final_y = static_cast<int>((1.0 - norm_y) * dev_h);
        return {std::clamp(final_x, 0, dev_w - 1), std::clamp(final_y, 0, dev_h - 1)};
    } else if (m_rotation == 270) {
        int final_x = static_cast<int>((1.0 - norm_y) * dev_w);
        int final_y = static_cast<int>(norm_x * dev_h);
        return {std::clamp(final_x, 0, dev_w - 1), std::clamp(final_y, 0, dev_h - 1)};
    }
    int final_x = static_cast<int>(norm_x * dev_w);
    int final_y = static_cast<int>(norm_y * dev_h);
    return {std::clamp(final_x, 0, dev_w - 1), std::clamp(final_y, 0, dev_h - 1)};
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
            painter.drawText(0, cy + 98, width(), 25, Qt::AlignCenter, "连接设备，点击上方「开始投屏」");
        }
        return;
    }

    // 2. Hardware/Smooth Video Frame Drawing
    if (!m_target_rect.isValid()) {
        updateTargetRect();
    }

    if (m_cache_dirty || m_scaled_cache.isNull()) {
        regenerateDisplayCache();
    }

    if (!m_scaled_cache.isNull()) {
        painter.drawImage(m_target_rect, m_scaled_cache);
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
