#pragma once

#include "Types.h"
#include <QTransform>

namespace harmonycast {

// Same transformation as ScreenView's fallback, prepared outside the GUI thread.
inline void prepareFramePresentation(DecodedFrame& frame, QSize viewport, int rotation, qreal dpr) {
    if (frame.image.isNull() || viewport.isEmpty() || dpr <= 0) return;
    QSize oriented = frame.image.size();
    if (rotation % 180 != 0) oriented.transpose();
    const QSize physical = oriented.scaled(viewport, Qt::KeepAspectRatio) * dpr;
    if (physical.isEmpty()) return;
    QImage source = frame.image;
    if (rotation != 0) {
        QTransform transform;
        transform.rotate(rotation);
        source = source.transformed(transform);
    }
    frame.presentation_image = source.size() == physical ? source : source.scaled(physical, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    frame.presentation_rotation = rotation;
}

} // namespace harmonycast
