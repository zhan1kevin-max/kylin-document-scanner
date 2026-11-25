#include "captureframe.h"

#include <QBuffer>
#include <QImageReader>
#include <stdexcept>

namespace scan {
namespace {
QImage readJpeg(const CapturedFrame &frame, int maxEdge)
{
    QBuffer input;
    input.setData(frame.jpeg);
    input.open(QIODevice::ReadOnly);
    QImageReader reader(&input, "JPEG");
    if (reader.size() != frame.size || frame.size.isEmpty() ||
        qint64(frame.size.width()) * frame.size.height() > 64000000) {
        throw std::runtime_error("高拍仪 JPEG 帧头与拍摄尺寸不符。");
    }
    if (maxEdge > 0 && qMax(frame.size.width(), frame.size.height()) > maxEdge)
        reader.setScaledSize(frame.size.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull() || (maxEdge == 0 && image.size() != frame.size))
        throw std::runtime_error(QString("无法解码高拍仪帧：%1").arg(reader.errorString()).toUtf8().constData());
    return image;
}
}

QImage CapturedFrame::decode() const
{
    if (!jpeg.isEmpty()) return readJpeg(*this, 0);
    if (pixels.isNull() || pixels.size() != size)
        throw std::runtime_error("高拍仪没有返回有效图像。");
    return pixels;
}

QImage CapturedFrame::preview(int maxEdge) const
{
    if (maxEdge < 1) throw std::runtime_error("预览尺寸无效。");
    if (!jpeg.isEmpty()) return readJpeg(*this, maxEdge);
    const auto image = decode();
    if (qMax(image.width(), image.height()) <= maxEdge) return image;
    return image.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio, Qt::FastTransformation);
}
}
