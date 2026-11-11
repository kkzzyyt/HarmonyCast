#include "harmonycast/ScrollBar.h"
#include <QPainter>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOptionSlider>
#include <QEvent>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <algorithm>

namespace harmonycast {
namespace {

class ScrollBarStyle : public QProxyStyle {
public:
    ScrollBarStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {}

    int pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const override {
        if (metric == PM_ScrollBarExtent) return 16;
        if (metric == PM_ScrollBarSliderMin) return 40;
        if (metric == PM_ScrollView_ScrollBarOverlap) return 0;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* result) const override {
        if (hint == SH_ScrollBar_Transient) return 0;
        if (hint == SH_ScrollBar_LeftClickAbsolutePosition) return 1;
        return QProxyStyle::styleHint(hint, option, widget, result);
    }

};

QPoint mousePosition(QMouseEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position().toPoint();
#else
    return event->pos();
#endif
}

} // namespace

ScrollBar::ScrollBar(Qt::Orientation orientation, QWidget* parent) : QScrollBar(orientation, parent) {
    auto* drawingStyle = new ScrollBarStyle;
    drawingStyle->setParent(this);
    setStyle(drawingStyle);
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
    m_hover_animation.setDuration(120);
    m_hover_animation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_hover_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        m_hover = value.toReal();
        update();
    });
    connect(this, &QScrollBar::sliderPressed, this, [this] { animateHover(true); });
    connect(this, &QScrollBar::sliderReleased, this, [this] { animateHover(m_hovered); });
}

QSize ScrollBar::sizeHint() const {
    return orientation() == Qt::Horizontal ? QSize(96, 16) : QSize(16, 96);
}

void ScrollBar::animateHover(bool hovered) {
    m_hover_animation.stop();
    m_hover_animation.setStartValue(m_hover);
    m_hover_animation.setEndValue(hovered ? 1.0 : 0.0);
    m_hover_animation.start();
}

bool ScrollBar::event(QEvent* event) {
    if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter) {
        m_hovered = true;
        animateHover(true);
    } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave) {
        m_hovered = false;
        if (!isSliderDown()) animateHover(false);
    } else if ((event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate ||
                event->type() == QEvent::Hide || (event->type() == QEvent::EnabledChange && !isEnabled())) && m_dragging) {
        m_dragging = false;
        setSliderDown(false);
        setValue(sliderPosition());
        animateHover(false);
    }
    return QScrollBar::event(event);
}

QRect ScrollBar::grooveRect() const {
    return orientation() == Qt::Horizontal ? rect().adjusted(2, 0, -2, 0) : rect().adjusted(0, 2, 0, -2);
}

QRect ScrollBar::handleRect() const {
    const QRect groove = grooveRect();
    const bool horizontal = orientation() == Qt::Horizontal;
    const int available = std::max(0, horizontal ? groove.width() : groove.height());
    const qint64 total = qint64(maximum()) - minimum() + pageStep();
    const int length = total > 0 ? std::clamp(int(qint64(available) * pageStep() / total), std::min(40, available), available) : available;
    QStyleOptionSlider option;
    initStyleOption(&option);
    const int position = QStyle::sliderPositionFromValue(minimum(), maximum(), sliderPosition(), available - length, option.upsideDown);
    return horizontal ? QRect(groove.left() + position, groove.top(), length, groove.height()) :
                        QRect(groove.left(), groove.top() + position, groove.width(), length);
}

int ScrollBar::valueAt(const QPoint& point, int offset) const {
    const QRect groove = grooveRect(), handle = handleRect();
    const bool horizontal = orientation() == Qt::Horizontal;
    const int travel = std::max(0, horizontal ? groove.width() - handle.width() : groove.height() - handle.height());
    const int position = horizontal ? point.x() - groove.left() - offset : point.y() - groove.top() - offset;
    QStyleOptionSlider option;
    initStyleOption(&option);
    return QStyle::sliderValueFromPosition(minimum(), maximum(), std::clamp(position, 0, travel), travel, option.upsideDown);
}

void ScrollBar::mousePressEvent(QMouseEvent* event) {
    if ((event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton) || maximum() <= minimum()) {
        event->ignore();
        return;
    }
    const QPoint point = mousePosition(event);
    const QRect handle = handleRect();
    const bool horizontal = orientation() == Qt::Horizontal;
    m_drag_offset = handle.contains(point) ? (horizontal ? point.x() - handle.left() : point.y() - handle.top()) :
                                           (horizontal ? handle.width() : handle.height()) / 2;
    m_dragging = true;
    setSliderDown(true);
    setSliderPosition(valueAt(point, m_drag_offset));
    event->accept();
    update();
}

void ScrollBar::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging) { QScrollBar::mouseMoveEvent(event); return; }
    setSliderPosition(valueAt(mousePosition(event), m_drag_offset));
    event->accept();
}

void ScrollBar::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_dragging) { event->ignore(); return; }
    setSliderPosition(valueAt(mousePosition(event), m_drag_offset));
    m_dragging = false;
    setSliderDown(false);
    setValue(sliderPosition());
    animateHover(m_hovered);
    event->accept();
    update();
}

void ScrollBar::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    menu.addAction("滚动到此处", this, [this, point = event->pos()] {
        const auto handle = handleRect();
        setValue(valueAt(point, (orientation() == Qt::Horizontal ? handle.width() : handle.height()) / 2));
    });
    menu.addSeparator();
    menu.addAction("移到开头", this, [this] { triggerAction(SliderToMinimum); });
    menu.addAction("移到末尾", this, [this] { triggerAction(SliderToMaximum); });
    menu.addSeparator();
    menu.addAction("上一页", this, [this] { triggerAction(SliderPageStepSub); });
    menu.addAction("下一页", this, [this] { triggerAction(SliderPageStepAdd); });
    menu.exec(event->globalPos());
}

void ScrollBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().brush(QPalette::Base));
    if (maximum() <= minimum()) return;
    QRectF handle = handleRect();
    QRectF groove = grooveRect();
    const bool horizontal = orientation() == Qt::Horizontal;
    const qreal thickness = isSliderDown() ? 10.0 : 6.0 + 2.0 * m_hover;
    if (horizontal) {
        handle.setTop((height() - thickness) / 2.0);
        handle.setHeight(thickness);
        groove.setTop((height() - 10.0) / 2.0);
        groove.setHeight(10.0);
    } else {
        handle.setLeft((width() - thickness) / 2.0);
        handle.setWidth(thickness);
        groove.setLeft((width() - 10.0) / 2.0);
        groove.setWidth(10.0);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(36, 103, 214, int(14 * m_hover)));
    painter.drawRoundedRect(groove, 5, 5);
    const QColor idle("#879bb5"), hover("#557dab");
    QColor color = isSliderDown() ? QColor("#2467d6") : QColor(
        qRound(idle.red() + (hover.red() - idle.red()) * m_hover),
        qRound(idle.green() + (hover.green() - idle.green()) * m_hover),
        qRound(idle.blue() + (hover.blue() - idle.blue()) * m_hover));
    if (!isEnabled()) color.setAlpha(80);
    painter.setBrush(color);
    painter.drawRoundedRect(handle, thickness / 2.0, thickness / 2.0);
}

} // namespace harmonycast
