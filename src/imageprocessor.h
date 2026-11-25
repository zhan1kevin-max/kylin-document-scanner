#pragma once
#include "types.h"
namespace scan {
class ImageProcessor {
public:
    static QVector<QPointF> detectCorners(const QImage &image, QVector<QPointF> *missingTriangle = nullptr);
    static bool validMissingTriangle(const QVector<QPointF> &corners, const QVector<QPointF> &triangle);
    // Automatic or adjusted A4 pages may have one inferred corner off sensor.
    static bool validCorners(const QVector<QPointF> &corners, bool allowPartial = false);
    static QImage process(const QImage &image, const EditOptions &options);
    static QImage composeIdCard(const QImage &front, const QImage &back);
};
}
