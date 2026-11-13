#pragma once

#include "Types.h"
#include "ControlChannel.h"
#include <QObject>
#include <memory>

namespace harmonycast {

struct StreamTimeouts {
    int startup_ms{30000};
    int frame_ms{15000};
};

class MirrorSession : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool start() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool isRunning() const = 0;
    [[nodiscard]] virtual bool isActive() const = 0;
    [[nodiscard]] virtual int deviceWidth() const = 0;
    [[nodiscard]] virtual int deviceHeight() const = 0;
    [[nodiscard]] virtual bool isFallbackSnapshot() const = 0;
    [[nodiscard]] virtual std::shared_ptr<ControlChannel> controlChannel() const = 0;
    virtual void setQuality(int quality, int bitrate) = 0;
    virtual void setBitrate(int bitrate) = 0;
    virtual void setPresentation(QSize viewport, int rotation, qreal dpr) = 0;
signals:
    void frameReady(const DecodedFrame& frame);
    void statusChanged(const QString& status);
    void streamFinished(const QString& error);
};

} // namespace harmonycast
