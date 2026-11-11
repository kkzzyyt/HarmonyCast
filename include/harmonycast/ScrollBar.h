#pragma once

#include <QScrollBar>
#include <QVariantAnimation>

namespace harmonycast {

class ScrollBar : public QScrollBar {
public:
    explicit ScrollBar(Qt::Orientation orientation, QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void animateHover(bool hovered);
    QRect grooveRect() const;
    QRect handleRect() const;
    int valueAt(const QPoint& point, int offset) const;
    QVariantAnimation m_hover_animation;
    qreal m_hover{0};
    bool m_hovered{false};
    bool m_dragging{false};
    int m_drag_offset{0};
};

} // namespace harmonycast
