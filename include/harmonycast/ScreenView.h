#pragma once

#include "Types.h"
#include <QWidget>
#include <QImage>
#include <QPoint>
#include <QTimer>
#include <functional>
#include <vector>

namespace harmonycast {

class ScreenView : public QWidget {
    Q_OBJECT
public:
    using TouchCallback = std::function<void(TouchAction action, int x, int y, int dev_w, int dev_h)>;

    explicit ScreenView(QWidget* parent = nullptr);
    ~ScreenView() override = default;

    void updateFrame(const DecodedFrame& frame);
    void setImage(const QImage& img, int dev_w = 0, int dev_h = 0);
    void clear();

    void setTouchCallback(TouchCallback cb) { m_touch_cb = std::move(cb); }
    void setRotation(int degrees);
    [[nodiscard]] int rotation() const { return m_rotation; }

    void setActive(bool active) { m_active = active; update(); }
    [[nodiscard]] bool isActive() const { return m_active; }

    void setStatusMessage(const QString& msg) { m_status_msg = msg; update(); }
    [[nodiscard]] const QImage& image() const { return m_image; }
    [[nodiscard]] int deviceWidth() const { return m_dev_width; }
    [[nodiscard]] int deviceHeight() const { return m_dev_height; }
    [[nodiscard]] const QRect& targetRect() const { return m_target_rect; }

    std::pair<int, int> mapToDevice(const QPoint& pos) const;

signals:
    void startRequested();
    void presentationChanged(QSize viewport, int rotation, qreal dpr);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void onFadeTick();

private:
    QImage m_image;
    QImage m_scaled_cache;
    bool m_cache_dirty{true};
    int m_dev_width{0};
    int m_dev_height{0};
    int m_rotation{0};
    bool m_active{false};
    QString m_status_msg;

    QRect m_target_rect;
    TouchCallback m_touch_cb;

    // Visual Touch Feedback
    bool m_touch_active{false};
    QPoint m_touch_pos;
    std::vector<QPoint> m_trail;
    float m_ripple_radius{16.0f};
    float m_ripple_opacity{0.0f};
    QTimer* m_fade_timer{nullptr};

    void updateTargetRect();
    void regenerateDisplayCache();
};

} // namespace harmonycast
