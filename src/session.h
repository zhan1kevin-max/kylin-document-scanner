#pragma once
#include "types.h"
#include "captureframe.h"
#include <QJsonObject>
#include <memory>
namespace scan {
class Session {
public:
    QString directory;
    QVector<Page> pages;
    bool active() const { return !directory.isEmpty(); }
    void create(const QString &path);
    void load(const QString &path);
    void save() const;
    QString absolute(const QString &relative) const;
    Page add(const QImage &original, const QImage &processed, const EditOptions &edits);
    // A null processed image shares the immutable original file.
    Page addCapture(const CapturedFrame &frame, const QImage &processed, const EditOptions &edits);
    void update(const QString &id, const QImage &processed, const EditOptions &edits);
    void setOcr(const QString &id, const QString &text);
    void remove(const QVector<QString> &ids);
    void move(int from, int to);
    static void saveImage(const QString &path, const QImage &image);
    static QJsonObject editsJson(const EditOptions &edits);
    static EditOptions editsFromJson(const QJsonObject &object);
    int taskDirectoryFd() const { return lease_ ? *lease_ : -1; }
private:
    std::shared_ptr<int> lease_;
    void holdDirectory();
};
}
