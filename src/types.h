#pragma once
#include <QImage>
#include <QMetaType>
#include <QPointF>
#include <QString>
#include <QVector>

namespace scan {
enum class ColorMode { Color = 0, Gray = 1, BlackWhite = 2 };
inline bool hasOffscreenCorner(const QVector<QPointF> &corners) {
    for (const auto &p : corners) {
        if (p.x() < 0 || p.y() < 0 || p.x() > 1 || p.y() > 1) return true;
    }
    return false;
}
struct EditOptions {
    bool autoCrop = true;
    QVector<QPointF> corners; // TL, TR, BR, BL; an inferred corner may lie outside [0,1].
    int rotation = 0; // clockwise degrees: 0, 90, 180, 270, applied after crop
    ColorMode color = ColorMode::Color;
    QVector<QPointF> missingTriangle; // inferred corner and two visible endpoints, in original coordinates
};
struct Page {
    QString id;
    QString originalFile; // path relative to session directory
    QString processedFile;
    EditOptions edits;
    QString ocrText;
};
struct CameraDevice { QString id; QString name; };
struct CameraFormat {
    int width = 0;
    int height = 0;
    unsigned int fourcc = 0;
    QString description;
};
}
Q_DECLARE_METATYPE(scan::CameraFormat)
