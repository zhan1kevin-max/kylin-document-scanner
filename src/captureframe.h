#pragma once

#include <QByteArray>
#include <QImage>
#include <QMetaType>

namespace scan {
// Owned memory: the camera can return the V4L2 buffer before notifying the UI.
struct CapturedFrame {
    QByteArray jpeg;
    QImage pixels;
    QSize size;

    QImage decode() const;
    // Scaled JPEG IDCT for paper detection or a thumbnail of this exact exposure.
    QImage preview(int maxEdge = 1000) const;
};
}
Q_DECLARE_METATYPE(scan::CapturedFrame)
