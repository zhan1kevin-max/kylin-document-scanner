#pragma once
#include <QMap>
#include <QStringList>
#include <QVector>

class QWidget;
namespace scan {
class Session;

struct SnapshotFile {
    quint64 device=0, inode=0;
    qint64 bytes=0, size=0, modifiedSeconds=0, modifiedNanos=0;
    bool referenced=false;
};
struct SnapshotTask {
    QString path;
    QByteArray manifestHash;
    int pages=0;
    QMap<QString,SnapshotFile> files;
    qint64 bytes(bool referenced) const;
};
struct CleanupResult {
    qint64 bytes=0;
    int files=0;
    bool clearedPages=false;
    QStringList errors;
};
class ScanStorage {
public:
    static void registerTask(const QString &path);
    static QVector<SnapshotTask> discover(const QString &output,const QString &current);
    static SnapshotTask inspect(const QString &path);
    static CleanupResult clean(const SnapshotTask &shown,bool obsolete,bool clearPages,const Session &active);
    static bool showDialog(QWidget *parent,const QString &output,const Session &active);
};
}
