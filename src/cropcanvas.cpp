#include "cropcanvas.h"
#include "imageprocessor.h"
#include <QPainter>
#include <QMouseEvent>
#include <QLineF>
#include <algorithm>
namespace scan {
CropCanvas::CropCanvas(QWidget *parent):QWidget(parent) { setMinimumSize(320,300); setMouseTracking(true); }
void CropCanvas::setImage(const QImage &image) {
    image_=image.width()>1800||image.height()>1800?image.scaled(1800,1800,Qt::KeepAspectRatio,Qt::SmoothTransformation):image;
    update();
}
void CropCanvas::setCorners(const QVector<QPointF> &corners,const QVector<QPointF> &missingTriangle) {
    corners_=corners;missingTriangle_=missingTriangle;
    if(editing_) fitEditingArea();
    update();
}
void CropCanvas::setEditing(bool enabled) {editing_=enabled;dragging_=-1;if(enabled) fitEditingArea();setCursor(enabled?Qt::CrossCursor:Qt::ArrowCursor);update();}
void CropCanvas::setMessage(const QString &message) {message_=message;update();}
void CropCanvas::setZoom(double factor) { zoom_=std::clamp(factor,.4,3.0); update(); }
void CropCanvas::setOverlayText(const QString &text){overlayText_=text;update();}
void CropCanvas::fitEditingArea() {
    double left=0,top=0,right=1,bottom=1;
    for (const auto &point : corners_) {
        left=std::min(left,point.x()); top=std::min(top,point.y());
        right=std::max(right,point.x()); bottom=std::max(bottom,point.y());
    }
    // Leave space to move a corner past the camera image. Keep this viewport
    // fixed during a drag so the handle stays under the pointer.
    editingArea_=QRectF(QPointF(left-.06,top-.06),QPointF(right+.06,bottom+.06));
}
QRectF CropCanvas::imageRect() const {
    if(image_.isNull()) return {};
    const auto area=editing_ ? editingArea_ : QRectF(0,0,1,1);
    const double scale=zoom_*std::min(std::max(1,width()-40)/(image_.width()*area.width()),
                                     std::max(1,height()-40)/(image_.height()*area.height()));
    const QSizeF imageSize(image_.width()*scale,image_.height()*scale);
    return QRectF(QPointF(width()*.5-area.center().x()*imageSize.width(),
                          height()*.5-area.center().y()*imageSize.height()),imageSize);
}
void CropCanvas::paintEvent(QPaintEvent *) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),QColor("#414447"));
    if(image_.isNull()) {
        p.setPen(QColor("#e3e5e7")); QFont f=font();f.setPointSize(13);p.setFont(f);
        p.drawText(rect().adjusted(30,30,-30,-30),Qt::AlignCenter|Qt::TextWordWrap,message_.isEmpty()?QStringLiteral("连接高拍仪，或导入已有图片"):message_);return;
    }
    auto r=imageRect();p.setRenderHint(QPainter::SmoothPixmapTransform);p.drawImage(r,image_);
    if(corners_.size()==4) {
        QPolygonF quad; for(auto c:corners_) quad<<QPointF(r.left()+c.x()*r.width(),r.top()+c.y()*r.height());
        const QColor estimated("#ffc35c");
        if(hasOffscreenCorner(corners_)){
            p.setPen(QPen(estimated,2,Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawPolygon(quad);
        }
        p.save();
        p.setClipRect(r);
        p.setPen(QPen(QColor("#51e89c"),3));p.setBrush(QColor(0,140,55,9));p.drawPolygon(quad);
        if(missingTriangle_.size()==3) {
            QPolygonF missing;
            for(const auto &c:missingTriangle_) missing << QPointF(r.left()+c.x()*r.width(),r.top()+c.y()*r.height());
            p.setPen(QPen(estimated,2,Qt::DashLine));
            p.setBrush(QColor(255,195,92,52));
            p.drawPolygon(missing);
        }
        p.restore();
        if(editing_) for(int i=0;i<4;++i) {
            p.setBrush(Qt::white);p.setPen(QPen(r.contains(quad[i])?QColor("#0b8d78"):estimated,3));
            p.drawEllipse(quad[i],9,9);p.setPen(QColor("#064e3b"));
            p.drawText(QRectF(quad[i].x()-8,quad[i].y()-9,16,18),Qt::AlignCenter,QString::number(i+1));
        }
    }
    if(!overlayText_.isEmpty()){
        QFontMetrics metrics(p.font());
        QRect label(12,12,metrics.horizontalAdvance(overlayText_)+22,30);
        p.fillRect(label,QColor(24,28,31,218));p.setPen(Qt::white);
        p.drawText(label,Qt::AlignCenter,overlayText_);
    }
}
void CropCanvas::mousePressEvent(QMouseEvent *e) {
    if(!editing_ || e->button()!=Qt::LeftButton) return;
    auto r=imageRect();for(int i=0;i<corners_.size();++i) {auto point=QPointF(r.left()+corners_[i].x()*r.width(),r.top()+corners_[i].y()*r.height());if(QLineF(point,e->localPos()).length()<20){dragging_=i;break;}}
}
void CropCanvas::mouseMoveEvent(QMouseEvent *e) {
    auto r=imageRect();if(dragging_<0 || r.isEmpty())return;
    constexpr double extra=.25;
    corners_[dragging_]=QPointF(std::clamp((e->localPos().x()-r.left())/r.width(),-extra,1+extra),
                                std::clamp((e->localPos().y()-r.top())/r.height(),-extra,1+extra));
    update();
}
void CropCanvas::mouseReleaseEvent(QMouseEvent *) {
    if(dragging_<0) return;
    dragging_=-1;
    if(!missingTriangle_.isEmpty() && !ImageProcessor::validMissingTriangle(corners_,missingTriangle_)) {
        missingTriangle_.clear();
    }
    fitEditingArea();
    update();
}
}
