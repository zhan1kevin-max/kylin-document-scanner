#pragma once
#include "types.h"
#include "captureframe.h"
#include <QObject>
#include <memory>
#include <vector>

namespace scan {
// One worker owns the device. No camera I/O on the UI thread.
class Camera : public QObject {
    Q_OBJECT
public:
    explicit Camera(QObject *parent = nullptr);
    ~Camera() override;
    static QVector<CameraDevice> devices();
    static QVector<CameraFormat> formats(const QString &deviceId);
    void start(const QString &deviceId, const CameraFormat &captureFormat);
    void stop();
    void capture();
    bool isRunning() const;
signals:
    void preview(const QImage &image);
    void frameCaptured(const scan::CapturedFrame &frame);
    void captured(const QImage &image);
    void opened(const QString &description);
    void error(const QString &message);
    void stopped();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
