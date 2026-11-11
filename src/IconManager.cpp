#include "harmonycast/IconManager.h"
#include "harmonycast/ScrollBar.h"

#include <QtSvg/QSvgRenderer>
#include <QPainter>
#include <QPixmap>
#include <QColor>
#include <QByteArray>
#include <QAbstractItemView>
#include <QListView>
#include <QWidget>
#include <QLayout>
#include <QHash>
#include <QDir>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QScreen>
#include <QGuiApplication>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QScrollBar>
#include <QWheelEvent>

namespace harmonycast {

namespace {

class PrecisionScrollFilter : public QObject {
public:
    explicit PrecisionScrollFilter(QAbstractItemView* view) : QObject(view), m_view(view) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != m_view->viewport() || event->type() != QEvent::Wheel) return false;
        auto* wheel = static_cast<QWheelEvent*>(event);
        if (wheel->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) return false;
        QPoint pixels = wheel->pixelDelta();
        const bool horizontal = wheel->modifiers() & Qt::ShiftModifier;
        if (pixels.isNull() && !horizontal) return false;
        if (horizontal) {
            if (pixels.isNull()) {
                pixels.setX(qRound(wheel->angleDelta().y() / 120.0 * m_view->horizontalScrollBar()->singleStep() * QApplication::wheelScrollLines()));
            } else if (pixels.x() == 0) {
                pixels = QPoint(pixels.y(), 0);
            }
        }
        auto* x = m_view->horizontalScrollBar();
        auto* y = m_view->verticalScrollBar();
        const int oldX = x->value(), oldY = y->value();
        x->setValue(oldX - pixels.x());
        y->setValue(oldY - pixels.y());
        if (x->value() == oldX && y->value() == oldY && !horizontal) return false;
        wheel->accept();
        return true;
    }

private:
    QAbstractItemView* m_view;
};

class ComboPopupFilter : public QObject {
public:
    ComboPopupFilter(QComboBox* combo, int minWidth, QObject* parent = nullptr)
        : QObject(parent), m_combo(combo), m_minWidth(minWidth) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if ((event->type() == QEvent::Show || event->type() == QEvent::Polish) && m_combo) {
            auto* view = m_combo->view();
            if (view) {
                QFontMetrics fm = view->fontMetrics();
                int max_w = m_minWidth;
                for (int i = 0; i < m_combo->count(); ++i) {
                    // Item text width + icon + item padding + scrollbar space + safety margin
                    int w = fm.horizontalAdvance(m_combo->itemText(i)) + 56;
                    if (w > max_w) {
                        max_w = w;
                    }
                }
                if (m_combo->width() > max_w) {
                    max_w = m_combo->width();
                }
                view->setMinimumWidth(max_w);
                auto* container = view->window();
                if (container && container != m_combo) {
                    container->setMinimumWidth(max_w);
                    container->resize(qMax(container->width(), max_w), container->height());

                    // Prevent container from extending outside the available screen boundary
                    if (auto* screen = m_combo->screen()) {
                        QRect avail = screen->availableGeometry();
                        if (!avail.isEmpty() && (container->x() + container->width() > avail.right())) {
                            int new_x = qMax(avail.left(), avail.right() - container->width() - 4);
                            container->move(new_x, container->y());
                        }
                    }
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QComboBox* m_combo;
    int m_minWidth;
};

} // anonymous namespace

static QHash<QString, QIcon> s_iconCache;

QIcon IconManager::makeSvgIcon(const QString& svgInner, const QString& stroke, const QString& fill, double strokeWidth, int size) {
    QString xml = QString(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" width=\"%1\" height=\"%1\" "
        "fill=\"%2\" stroke=\"%3\" stroke-width=\"%4\" stroke-linecap=\"round\" stroke-linejoin=\"round\">"
        "%5"
        "</svg>"
    ).arg(size).arg(fill).arg(stroke).arg(strokeWidth).arg(svgInner);

    QSvgRenderer renderer(xml.toUtf8());
    QPixmap pix(size, size);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    renderer.render(&painter);
    painter.end();
    return QIcon(pix);
}

QIcon IconManager::getIcon(const QString& name) {
    if (s_iconCache.contains(name)) {
        return s_iconCache.value(name);
    }

    QIcon icon;
    if (name == "connect") {
        icon = makeSvgIcon("<polygon points=\"13 2 3 14 12 14 11 22 21 10 12 10 13 2\" fill=\"#607b99\" stroke=\"#607b99\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "connect_dark" || name == "connect_primary") {
        icon = makeSvgIcon("<polygon points=\"13 2 3 14 12 14 11 22 21 10 12 10 13 2\" fill=\"#ffffff\" stroke=\"#ffffff\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "disconnect") {
        icon = makeSvgIcon("<path d=\"M18.36 6.64a9 9 0 1 1-12.73 0M12 2v10\" stroke=\"#bc4251\" stroke-width=\"2.2\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "delete") {
        icon = makeSvgIcon("<polyline points=\"3 6 5 6 21 6\"/><path d=\"M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2\"/><line x1=\"10\" y1=\"11\" x2=\"10\" y2=\"17\"/><line x1=\"14\" y1=\"11\" x2=\"14\" y2=\"17\"/>", "#c24552", "none", 2.0, 16);
    } else if (name == "mirror" || name == "workspace") {
        icon = makeSvgIcon("<rect x=\"2\" y=\"3\" width=\"20\" height=\"14\" rx=\"2\" ry=\"2\" stroke=\"#607b99\"/><line x1=\"8\" y1=\"21\" x2=\"16\" y2=\"21\" stroke=\"#607b99\"/><line x1=\"12\" y1=\"17\" x2=\"12\" y2=\"21\" stroke=\"#607b99\"/><polygon points=\"10 7 15 10 10 13\" fill=\"#607b99\" stroke=\"#607b99\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "refresh" || name == "rotate") {
        icon = makeSvgIcon("<polyline points=\"23 4 23 10 17 10\"/><path d=\"M20.49 15a9 9 0 1 1-2.12-9.36L23 10\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "play") {
        icon = makeSvgIcon("<polygon points=\"5 3 19 12 5 21 5 3\" fill=\"#607b99\" stroke=\"#607b99\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "play_dark" || name == "play_primary") {
        icon = makeSvgIcon("<polygon points=\"5 3 19 12 5 21 5 3\" fill=\"#ffffff\" stroke=\"#ffffff\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "stop") {
        icon = makeSvgIcon("<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\" rx=\"2\" ry=\"2\" fill=\"#bc4251\" stroke=\"#bc4251\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "camera") {
        icon = makeSvgIcon("<path d=\"M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z\"/><circle cx=\"12\" cy=\"13\" r=\"4\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "1to1" || name == "pixel_perfect") {
        icon = makeSvgIcon("<circle cx=\"11\" cy=\"11\" r=\"8\"/><line x1=\"21\" y1=\"21\" x2=\"16.65\" y2=\"16.65\"/><path d=\"M11 8v6M8 11h6\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "fullscreen") {
        icon = makeSvgIcon("<path d=\"M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "more") {
        icon = makeSvgIcon("<circle cx=\"12\" cy=\"12\" r=\"1.5\" fill=\"#63768e\"/><circle cx=\"5\" cy=\"12\" r=\"1.5\" fill=\"#63768e\"/><circle cx=\"19\" cy=\"12\" r=\"1.5\" fill=\"#63768e\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "hub") {
        icon = makeSvgIcon("<circle cx=\"12\" cy=\"12\" r=\"3\"/><circle cx=\"5\" cy=\"6\" r=\"2\"/><circle cx=\"19\" cy=\"6\" r=\"2\"/><circle cx=\"12\" cy=\"20\" r=\"2\"/><line x1=\"7\" y1=\"7.5\" x2=\"10\" y2=\"10.5\"/><line x1=\"17\" y1=\"7.5\" x2=\"14\" y2=\"10.5\"/><line x1=\"12\" y1=\"15\" x2=\"12\" y2=\"18\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "search") {
        icon = makeSvgIcon("<circle cx=\"11\" cy=\"11\" r=\"7\"/><line x1=\"21\" y1=\"21\" x2=\"16.65\" y2=\"16.65\"/>", "#70839a", "none", 2.0, 14);
    } else if (name == "clear") {
        icon = makeSvgIcon("<line x1=\"18\" y1=\"6\" x2=\"6\" y2=\"18\"/><line x1=\"6\" y1=\"6\" x2=\"18\" y2=\"18\"/>", "#63768e", "none", 2.0, 14);
    } else if (name == "copy") {
        icon = makeSvgIcon("<rect x=\"9\" y=\"9\" width=\"13\" height=\"13\" rx=\"2\" ry=\"2\"/><path d=\"M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1\"/>", "#63768e", "none", 2.0, 14);
    } else if (name == "export") {
        icon = makeSvgIcon("<path d=\"M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4\"/><polyline points=\"7 10 12 15 17 10\"/><line x1=\"12\" y1=\"15\" x2=\"12\" y2=\"3\"/>", "#63768e", "none", 2.0, 14);
    } else if (name == "chrome") {
        icon = makeSvgIcon("<circle cx=\"12\" cy=\"12\" r=\"10\"/><circle cx=\"12\" cy=\"12\" r=\"4\"/><line x1=\"21.17\" y1=\"8\" x2=\"12\" y2=\"8\"/><line x1=\"3.95\" y1=\"6.06\" x2=\"8.54\" y2=\"14\"/><line x1=\"10.88\" y1=\"21.94\" x2=\"15.46\" y2=\"14\"/>", "#63768e", "none", 2.0, 14);
    } else if (name == "layout_h") {
        icon = makeSvgIcon("<rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"2\"/><line x1=\"12\" y1=\"3\" x2=\"12\" y2=\"21\"/>", "#63768e", "none", 2.0, 14);
    } else if (name == "layout_v") {
        icon = makeSvgIcon("<rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"2\"/><line x1=\"3\" y1=\"12\" x2=\"21\" y2=\"12\"/>", "#63768e", "none", 2.0, 14);
    } else if (name == "layout_screen") {
        icon = makeSvgIcon("<rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"2\"/><polygon points=\"10 8 16 12 10 16 10 8\" fill=\"#607b99\" stroke=\"#607b99\"/>", "#63768e", "none", 2.0, 14);
    } else if (name == "layout_logs") {
        icon = makeSvgIcon("<rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"2\"/><line x1=\"7\" y1=\"8\" x2=\"17\" y2=\"8\"/><line x1=\"7\" y1=\"12\" x2=\"17\" y2=\"12\"/><line x1=\"7\" y1=\"16\" x2=\"13\" y2=\"16\"/>", "#63768e", "none", 2.0, 14);
    } else if (name == "device") {
        icon = makeSvgIcon("<rect x=\"5\" y=\"2\" width=\"14\" height=\"20\" rx=\"2\" ry=\"2\"/><line x1=\"12\" y1=\"18\" x2=\"12.01\" y2=\"18\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "command" || name == "terminal") {
        icon = makeSvgIcon("<polyline points=\"4 17 10 11 4 5\"/><line x1=\"12\" y1=\"19\" x2=\"20\" y2=\"19\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "logs") {
        icon = makeSvgIcon("<path d=\"M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z\"/><polyline points=\"14 2 14 8 20 8\"/><line x1=\"16\" y1=\"13\" x2=\"8\" y2=\"13\"/><line x1=\"16\" y1=\"17\" x2=\"8\" y2=\"17\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "web") {
        icon = makeSvgIcon("<circle cx=\"12\" cy=\"12\" r=\"10\"/><line x1=\"2\" y1=\"12\" x2=\"22\" y2=\"12\"/><path d=\"M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10z\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "pause") {
        icon = makeSvgIcon("<rect x=\"6\" y=\"4\" width=\"4\" height=\"16\"/><rect x=\"14\" y=\"4\" width=\"4\" height=\"16\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "back") {
        icon = makeSvgIcon("<polyline points=\"9 14 4 9 9 4\"/><path d=\"M20 20v-7a4 4 0 0 0-4-4H4\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "home") {
        icon = makeSvgIcon("<path d=\"M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z\"/><polyline points=\"9 22 9 12 15 12 15 22\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "sun") {
        icon = makeSvgIcon("<circle cx=\"12\" cy=\"12\" r=\"5\"/><line x1=\"12\" y1=\"1\" x2=\"12\" y2=\"3\"/><line x1=\"12\" y1=\"21\" x2=\"12\" y2=\"23\"/><line x1=\"4.22\" y1=\"4.22\" x2=\"5.64\" y2=\"5.64\"/><line x1=\"18.36\" y1=\"18.36\" x2=\"19.78\" y2=\"19.78\"/><line x1=\"1\" y1=\"12\" x2=\"3\" y2=\"12\"/><line x1=\"21\" y1=\"12\" x2=\"23\" y2=\"12\"/><line x1=\"4.22\" y1=\"19.78\" x2=\"5.64\" y2=\"18.36\"/><line x1=\"18.36\" y1=\"5.64\" x2=\"19.78\" y2=\"4.22\"/>", "#8b6717", "none", 2.0, 16);
    } else if (name == "rocket") {
        icon = makeSvgIcon("<path d=\"M4.5 16.5c-1.5 1.26-2 5-2 5s3.74-.5 5-2c.71-.84.7-2.13-.09-2.91a2.18 2.18 0 0 0-2.91-.09z\"/><path d=\"M12 15l-3-3a22 22 0 0 1 2-3.95A12.88 12.88 0 0 1 22 2c0 2.72-.78 7.5-3.05 11a22.5 22.5 0 0 1-3.95 2z\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "app") {
        icon = makeSvgIcon("<polygon points=\"12 2 2 7 12 12 22 7 12 2\"/><polyline points=\"2 17 12 22 22 17\"/><polyline points=\"2 12 12 17 22 12\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "package") {
        icon = makeSvgIcon("<line x1=\"16.5\" y1=\"9.4\" x2=\"7.5\" y2=\"4.21\"/><path d=\"M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z\"/><polyline points=\"3.27 6.96 12 12.01 20.73 6.96\"/><line x1=\"12\" y1=\"22.08\" x2=\"12\" y2=\"12\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "shield") {
        icon = makeSvgIcon("<path d=\"M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z\"/>", "#607b99", "none", 2.0, 16);
    } else if (name == "bone") {
        icon = makeSvgIcon("<path d=\"M17 10c.7-.7 1.6-1 2.5-.7 1.5.5 2 2.3 1.1 3.5-.3.4-.4.9-.3 1.4.3 1.4-.7 2.8-2.1 2.8-.7 0-1.4-.3-1.9-.8L6.3 6.2C5.8 5.7 5.1 5.4 4.4 5.4 3 5.4 2 6.8 2.3 8.2c.1.5 0 1-.3 1.4-.9 1.2-.4 3 1.1 3.5.9.3 1.8 0 2.5-.7L17 10z\"/>", "#63768e", "none", 2.0, 16);
    } else if (name == "settings") {
        icon = makeSvgIcon(
            "<circle cx=\"12\" cy=\"12\" r=\"3\"/>"
            "<path d=\"M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z\"/>",
            "#63768e", "none", 2.0, 16
        );
    }

    s_iconCache.insert(name, icon);
    return icon;
}

QPushButton* IconManager::button(const QString& text, const QString& name, const QString& iconName) {
    auto* btn = new QPushButton(text);
    if (!name.isEmpty()) {
        btn->setObjectName(name);
    }
    if (!iconName.isEmpty()) {
        QString effIcon = (name == "Primary" && (iconName == "connect" || iconName == "play")) ? iconName + "_dark" : iconName;
        btn->setIcon(getIcon(effIcon));
        btn->setIconSize(QSize(14, 14));
    }
    btn->setCursor(Qt::PointingHandCursor);
    return btn;
}

QPushButton* IconManager::iconButton(const QString& iconName, const QString& tooltip, const QString& name, int width, int height) {
    auto* btn = new QPushButton();
    btn->setIcon(getIcon(iconName));
    btn->setIconSize(QSize(15, 15));
    btn->setObjectName(name);
    btn->setFixedSize(width, height);
    if (!tooltip.isEmpty()) {
        btn->setToolTip(tooltip);
    }
    btn->setCursor(Qt::PointingHandCursor);
    return btn;
}

QLabel* IconManager::label(const QString& text, const QString& name, bool wrap) {
    auto* lbl = new QLabel(text);
    if (!name.isEmpty()) {
        lbl->setObjectName(name);
    }
    lbl->setWordWrap(wrap);
    return lbl;
}

void IconManager::setupComboBoxPopup(QComboBox* combo, int minWidth) {
    if (!combo) return;

    // Explicitly set QListView to bypass macOS Cocoa native NSMenu and ensure unified dark styling
    auto* listView = new QListView(combo);
    listView->setTextElideMode(Qt::ElideNone);
    listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setupScrollArea(listView);
    combo->setView(listView);

    if (minWidth > 0) {
        listView->setMinimumWidth(minWidth);
    }
    listView->setStyleSheet(
        "background-color: #ffffff; color: #25374d; border: 1px solid #d6e1ee; "
        "border-radius: 6px; selection-background-color: #e8f0fc; outline: none; padding: 2px;"
    );
    auto* container = listView->window();
    if (container && container != combo) {
        container->setAttribute(Qt::WA_TranslucentBackground, true);
        container->setStyleSheet(
            "background-color: #ffffff; color: #25374d; border: 1px solid #d6e1ee; border-radius: 6px;"
        );
        auto* layout = container->layout();
        if (layout) {
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(0);
            while (layout->count() > 0 && layout->itemAt(0)->spacerItem()) {
                layout->takeAt(0);
            }
            while (layout->count() > 0 && layout->itemAt(layout->count() - 1)->spacerItem()) {
                layout->takeAt(layout->count() - 1);
            }
        }
    }

    // Dynamic auto-sizing filter to calculate maximum required item width upon popup display
    auto* filter = new ComboPopupFilter(combo, minWidth, combo);
    listView->installEventFilter(filter);
    if (container && container != combo) {
        container->installEventFilter(filter);
    }
}

void IconManager::setupScrollArea(QAbstractScrollArea* area) {
    if (!area || area->property("hcScrollConfigured").toBool()) return;
    area->setProperty("hcScrollConfigured", true);
    area->setHorizontalScrollBar(new ScrollBar(Qt::Horizontal, area));
    area->setVerticalScrollBar(new ScrollBar(Qt::Vertical, area));
    if (auto* view = qobject_cast<QAbstractItemView*>(area)) {
        view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        view->viewport()->installEventFilter(new PrecisionScrollFilter(view));
    }
    area->horizontalScrollBar()->setSingleStep(32);
    // Text documents use line units internally; retain Qt's native text scrolling.
    if (qobject_cast<QAbstractItemView*>(area)) area->verticalScrollBar()->setSingleStep(24);
}

QPalette IconManager::getAppPalette() {
    QPalette p;
    p.setColor(QPalette::Window, QColor("#f4f7fb"));
    p.setColor(QPalette::WindowText, QColor("#25374d"));
    p.setColor(QPalette::Base, QColor("#ffffff"));
    p.setColor(QPalette::AlternateBase, QColor("#ffffff"));
    p.setColor(QPalette::ToolTipBase, QColor("#e8f0fc"));
    p.setColor(QPalette::ToolTipText, QColor("#ffffff"));
    p.setColor(QPalette::Text, QColor("#25374d"));
    p.setColor(QPalette::Button, QColor("#edf2f8"));
    p.setColor(QPalette::ButtonText, QColor("#25374d"));
    p.setColor(QPalette::BrightText, QColor("#607b99"));
    p.setColor(QPalette::Highlight, QColor("#e8f0fc"));
    p.setColor(QPalette::HighlightedText, QColor("#187a54"));
    return p;
}

QString IconManager::getAppStyleSheet(const QString& resourceDir) {
    QString chevronPath;
    QString checkPath;

    for (const QString& d : {
        resourceDir,
        QCoreApplication::applicationDirPath() + "/../Resources",
        QDir::currentPath() + "/resources",
        QDir::currentPath() + "/../resources"
    }) {
        if (d.isEmpty()) continue;
        if (chevronPath.isEmpty() && QFile::exists(d + "/chevron-down.svg")) {
            chevronPath = QFileInfo(d + "/chevron-down.svg").absoluteFilePath();
        }
        if (checkPath.isEmpty() && QFile::exists(d + "/check.svg")) {
            checkPath = QFileInfo(d + "/check.svg").absoluteFilePath();
        }
    }

    QString qss = R"css(
QWidget { background: #f4f7fb; color: #25374d; font-size: 13px; }
QMainWindow { background: #f4f7fb; }
QFrame#Sidebar { background: #ffffff; border-right: 1px solid #eef2f7; }
QFrame#Header { background: #ffffff; border-bottom: 1px solid #eef2f7; }
QLabel { background: transparent; }
QLabel#Title { font-size: 24px; font-weight: 500; }
QLabel#Brand { font-size: 16px; font-weight: 500; }
QLabel#Muted { color: #63768e; font-size: 12px; }
QLabel#Section { color: #70839a; font-size: 11px; font-weight: 600; }
QLabel#Badge { color: #607b99; background: #eaf7ef; padding: 4px 10px; border-radius: 9px; font-size: 12px; }
QPushButton { background: #edf2f8; border: 1px solid #d6e1ee; border-radius: 8px; padding: 4px 12px; font-size: 12px; }
QPushButton:hover { background: #edf4ff; border-color: #97b5da; }
QPushButton:pressed { background: #e4efff; }
QPushButton:disabled { color: #8797ab; border-color: #e2e9f2; background: #ffffff; }
QPushButton#Primary, QPushButton#InspectionStart { background: #2467d6; border: 1px solid #2467d6; color: #ffffff; font-weight: 500; }
QPushButton#Primary:hover, QPushButton#InspectionStart:hover { background: #2d70dc; border-color: #2d70dc; }
QPushButton#Primary:disabled, QPushButton#InspectionStart:disabled { background: #eef2f7; border-color: #eef2f7; color: #8898ac; }
QPushButton#Danger { color: #bc4251; border-color: #edcbd1; background: #fff3f4; }
QPushButton#Danger:hover { background: #fde9ed; border-color: #d89ca7; }
QPushButton#Danger:disabled { color: #8797ab; }
QPushButton#Navigation { text-align: left; background: transparent; border: none; color: #52677f; font-size: 14px; padding-left: 10px; }
QPushButton#Navigation:checked { background: #e7effd; color: #1f5fc5; font-weight: 500; }
QPushButton#Navigation:hover { background: #edf2f8; }
QPushButton#IconBtn { background: #f6f9fd; border: 1px solid #d6e1ee; border-radius: 5px; padding: 0px; }
QPushButton#IconBtn:hover { background: #e9f1fb; border-color: #97b5da; }
QPushButton#IconBtnPrimary { background: #edf6f2; border: 1px solid #b9d7c8; border-radius: 5px; padding: 0px; }
QPushButton#IconBtnPrimary:hover { background: #dcefe6; border-color: #25374d; }
QPushButton#IconBtnDanger { background: #fff1f3; border: 1px solid #edcbd1; border-radius: 5px; padding: 0px; }
QPushButton#IconBtnDanger:hover { background: #fbe6eb; border-color: #c24552; }
QPushButton#IconBtnMuted { background: #eef3f9; border: 1px solid #dce6f1; border-radius: 5px; padding: 0px; }
QPushButton#IconBtnMuted:hover { background: #dce6f1; border-color: #97b5da; }
QPushButton#IconBtnSettings { background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 0px; }
QPushButton#IconBtnSettings:hover { background: #edf2f8; border-color: #d6e1ee; }
QMenu { background-color: #ffffff; border: 1px solid #d6e1ee; border-radius: 6px; padding: 4px; color: #25374d; }
QMenu::item { padding: 6px 18px 6px 10px; border-radius: 4px; color: #314760; font-size: 12px; }
QMenu::item:selected { background-color: #e8f0fc; color: #607b99; }
QMenu::separator { height: 1px; background: #dfe7f0; margin: 4px 6px; }
QListWidget { background: transparent; border: none; outline: none; }
QListWidget::item { background: #ffffff; margin-bottom: 8px; padding: 14px 10px; border: 1px solid #dfe7f0; border-radius: 8px; }
QListWidget::item:selected { background: #e8f0fc; border-color: #25374d; color: #175d44; }
QListView, QAbstractItemView { background-color: #ffffff; color: #25374d; border: 1px solid #d6e1ee; border-radius: 6px; selection-background-color: #e8f0fc; outline: none; }
QLineEdit, QComboBox, QSpinBox { background-color: #ffffff; border: 1px solid #d6e1ee; border-radius: 5px; padding: 3px 6px; min-height: 18px; font-size: 12px; color: #25374d; }
QLineEdit:focus, QComboBox:focus { border-color: #2467d6; }
QComboBox { padding-right: 22px; padding-left: 6px; }
QComboBox:editable { background-color: #ffffff; color: #25374d; }
QComboBox QLineEdit { background: transparent; border: none; border-radius: 0; padding: 0; min-height: 0; color: #25374d; selection-background-color: #e8f0fc; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width: 20px; border: none; border-left: 1px solid #d6e1ee; background: #edf3fb; border-top-right-radius: 4px; border-bottom-right-radius: 4px; }
QComboBox::drop-down:hover { background: #d5e0ed; }
QComboBox::down-arrow { image: url("__CHEVRON__"); width: 10px; height: 10px; }
QComboBox QAbstractItemView { background-color: #ffffff; color: #25374d; border: 1px solid #d6e1ee; border-radius: 6px; selection-background-color: #e8f0fc; outline: none; padding: 2px; }
QComboBox QAbstractItemView::item { min-height: 24px; padding: 4px 8px; border-radius: 4px; color: #314760; }
QComboBox QAbstractItemView::item:selected { background-color: #e8f0fc; color: #607b99; font-weight: 600; }
QComboBox QAbstractItemView::item:hover { background-color: #edf3fb; color: #ffffff; }
QComboBoxPrivateContainer { background-color: #ffffff; border: 1px solid #d6e1ee; border-radius: 6px; color: #25374d; }
QComboBoxPrivateScroller { background-color: #ffffff; color: #63768e; height: 16px; }
QFrame#MirrorToolbar { background: #ffffff; border-bottom: 1px solid #edf3fb; }
QTextEdit, QPlainTextEdit { background: #ffffff; color: #314760; border: 1px solid #dfe7f0; border-radius: 7px; padding: 8px; selection-background-color: #d7e7fb; }
QCheckBox { background: transparent; spacing: 6px; font-size: 12px; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #97b5da; border-radius: 3px; background: #ffffff; }
QCheckBox::indicator:hover { border-color: #25374d; }
QCheckBox::indicator:checked { background: #2467d6; border-color: #2467d6; image: url("__CHECK__"); }
QCheckBox::indicator:checked:hover { background: #3479e7; border-color: #ffffff; }
QSplitter::handle:horizontal { background: #eef3f9; border-left: 1px solid #e2e9f2; border-right: 1px solid #e2e9f2; }
QSplitter::handle:horizontal:hover { background: #77a3e6; }
QSplitter::handle:vertical { background: #eef3f9; border-top: 1px solid #e2e9f2; border-bottom: 1px solid #e2e9f2; }
QSplitter::handle:vertical:hover { background: #77a3e6; }
QStatusBar { background: #ffffff; border-top: 1px solid #eef2f7; color: #63768e; }
QToolTip { background: #263b55; color: white; border: 1px solid #263b55; padding: 4px; }
QFrame#InfoCard { background: #ffffff; border: 1px solid #dfe7f0; border-radius: 12px; }
QFrame#AppBox { background: #ffffff; border: 1px solid #dfe7f0; border-radius: 6px; }
QLabel#CardTitle { font-size: 14px; font-weight: 500; color: #25374d; }
QLabel#CardTitleWarn { font-size: 15px; font-weight: 600; color: #8b6717; }
QLabel#InfoKey { color: #63768e; font-size: 12px; }
QLabel#InfoVal { color: #25374d; font-size: 13px; }
QLabel#InfoValMono { color: #187a54; font-size: 13px; font-family: 'Menlo', 'Courier New', monospace; font-weight: 600; }
QLabel#TagSuccess { color: #607b99; background: #eaf7ef; border: 1px solid #b9d7c8; padding: 2px 8px; border-radius: 4px; font-size: 12px; }
QLabel#TagWarning { color: #8b6717; background: #fff6dc; border: 1px solid #ebd495; padding: 2px 8px; border-radius: 4px; font-size: 12px; }
QLabel#TagMuted { color: #63768e; background: #edf2f8; border: 1px solid #d6e1ee; padding: 2px 8px; border-radius: 4px; font-size: 12px; }
QLabel#AppVersion { color: #607b99; font-size: 16px; font-weight: 600; font-family: 'Menlo', 'Courier New', monospace; }
QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
QAbstractScrollArea::corner { background: #ffffff; border: none; }
QTableWidget { alternate-background-color: #f6f9fd; background: #ffffff; border: 1px solid #dfe7f0; border-radius: 6px; gridline-color: #edf2f8; selection-background-color: #e8f0fc; color: #25374d; }
QTableWidget::item { padding: 4px 8px; }
QTableWidget::item:selected { background: #e8f0fc; color: #187a54; }
QHeaderView::section { background: #eff4fa; color: #52677f; font-weight: 500; border: none; border-bottom: 1px solid #dfe7f0; padding: 5px 8px; }
QPushButton#LogSubTab { background: #f5f8fc; color: #637b97; border: 1px solid #e2e9f2; border-radius: 5px; padding: 3px 10px; font-size: 12px; }
QPushButton#LogSubTab:hover { background: #edf4fd; color: #263b55; border-color: #b9cce3; }
QPushButton#LogSubTab:checked { background: #e7effc; color: #607b99; border-color: #93b6a7; font-weight: bold; }
QPushButton#LayoutSwitchBtn { background: transparent; border: none; border-radius: 4px; padding: 2px 6px; color: #637b97; font-size: 11px; }
QPushButton#LayoutSwitchBtn:hover { background: #edf4fd; color: #263b55; }
QPushButton#LayoutSwitchBtn:checked { background: #e7effc; color: #607b99; font-weight: bold; }
)css";

    qss.replace("__CHEVRON__", chevronPath);
    qss.replace("__CHECK__", checkPath);
    return qss;
}

} // namespace harmonycast
