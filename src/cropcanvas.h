#pragma once
#include "types.h"
#include <QWidget>
namespace scan {
class CropCanvas : public QWidget {
public:
    explicit CropCanvas(QWidget *parent = nullptr);
    void setImage(const QImage &image);
    void setCorners(const QVector<QPointF> &corners, const QVector<QPointF> &missingTriangle={});
    QVector<QPointF> corners() const { return corners_; }
    QVector<QPointF> missingTriangle() const { return missingTriangle_; }
    void setZoom(double factor);
    void setOverlayText(const QString &text);
    void setEditing(bool enabled);
    void setMessage(const QString &message);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
private:
    void fitEditingArea();
    QRectF imageRect() const;
    QRectF editingArea_{0,0,1,1};
    QImage image_;
    QVector<QPointF> corners_,missingTriangle_;
    QString message_, overlayText_;
    double zoom_=1.0;
    bool editing_ = false;
    int dragging_ = -1;
};
}
