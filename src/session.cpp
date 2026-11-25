#include "session.h"
#include "imageprocessor.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <QFile>
#include <QBuffer>
#include <QImageReader>
#include <QSet>
#include <stdexcept>
#ifdef Q_OS_LINUX
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace scan {
namespace {
[[noreturn]] void fail(const QString &text) { throw std::runtime_error(text.toUtf8().constData()); }
bool safeRelative(const QString &s) {
    return !s.isEmpty() && !QDir::isAbsolutePath(s) && !s.contains('\\') &&
           !s.contains(':') && !s.split('/').contains("..") && !s.startsWith('/');
}
QJsonArray pointsToJson(const QVector<QPointF> &points) {
    QJsonArray result;
    for (const auto &p : points) result.append(QJsonArray{p.x(),p.y()});
    return result;
}
QVector<QPointF> pointsFromJson(const QJsonValue &value) {
    QVector<QPointF> result;
    if(value.isUndefined()) return result;
    if(!value.isArray()) fail("任务中的裁切坐标无效。");
    for(const auto &entry:value.toArray()) {
        const auto point=entry.toArray();
        if(point.size()!=2 || !point[0].isDouble() || !point[1].isDouble()) fail("任务中的裁切坐标无效。");
        result.append(QPointF(point[0].toDouble(),point[1].toDouble()));
    }
    return result;
}
}
QJsonObject Session::editsJson(const EditOptions &o) {
    QJsonObject result={{"autoCrop",o.autoCrop},{"rotation",o.rotation},{"color",int(o.color)},{"corners",pointsToJson(o.corners)}};
    if(!o.missingTriangle.isEmpty()) result.insert("missingTriangle",pointsToJson(o.missingTriangle));
    return result;
}
EditOptions Session::editsFromJson(const QJsonObject &j) {
    EditOptions o;
    o.autoCrop=j.value("autoCrop").toBool(true);
    o.rotation=j.value("rotation").toInt();
    int color=j.value("color").toInt();
    if (o.rotation%90!=0 || o.rotation<0 || o.rotation>270 || color<0 || color>2) fail("任务中的图像设置无效。");
    o.color=ColorMode(color);
    o.corners=pointsFromJson(j.value("corners"));
    o.missingTriangle=pointsFromJson(j.value("missingTriangle"));
    if(!o.corners.isEmpty() && !ImageProcessor::validCorners(o.corners,true)) fail("任务中的裁切四角无效。");
    if(!ImageProcessor::validMissingTriangle(o.corners,o.missingTriangle)) fail("任务中的缺角补白坐标无效。");
    return o;
}
QString Session::absolute(const QString &relative) const {
    if(!active() || !safeRelative(relative)) fail("任务文件路径无效。");
    const auto path=QDir(directory).absoluteFilePath(relative);
    const auto base=QFileInfo(directory).canonicalFilePath();
    QFileInfo info(path);
    const auto resolved=info.exists()?info.canonicalFilePath():QFileInfo(info.absolutePath()).canonicalFilePath();
    if(base.isEmpty() || resolved.isEmpty() || (resolved!=base &&
       !resolved.startsWith(base+QDir::separator()) && !resolved.startsWith(base+"/"))) fail("任务文件必须位于任务目录内。");
    return path;
}
void Session::holdDirectory() {
#ifdef Q_OS_LINUX
    const int fd=::open(QFile::encodeName(directory).constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0) fail("无法访问扫描任务目录。");
    if(::flock(fd,LOCK_SH|LOCK_NB)!=0) {
        const int error=errno;
        ::close(fd);
        // A filesystem without advisory locks can still be used to scan.
        // The cleaner requires a working exclusive lock before deleting.
        if(error==ENOTSUP || error==ENOSYS) return;
        fail("扫描任务正在清理或无法锁定，请稍后重试。");
    }
    lease_=std::shared_ptr<int>(new int(fd),[](int *p){::close(*p);delete p;});
#endif
}
void Session::create(const QString &path) {
    if(QFileInfo::exists(QDir(path).filePath("session.json"))) fail("该文件夹已有扫描任务，请使用“打开任务”。");
    if(!QDir().mkpath(QDir(path).filePath("originals")) || !QDir().mkpath(QDir(path).filePath("processed"))) fail("无法创建任务目录，请检查写入权限和磁盘空间。");
    Session candidate;
    candidate.directory=QFileInfo(path).absoluteFilePath();
    candidate.holdDirectory();
    candidate.save();
    *this=candidate;
}
void Session::save() const {
    if(!active()) fail("请先创建或打开扫描任务。");
    QJsonArray list;
    for(const auto &p:pages) list.append(QJsonObject{{"id",p.id},{"original",p.originalFile},{"processed",p.processedFile},{"edits",editsJson(p.edits)},{"ocrText",p.ocrText}});
    QJsonDocument doc(QJsonObject{{"schemaVersion",1},{"application","kylin-document-scanner"},{"pages",list}});
    QSaveFile file(QDir(directory).filePath("session.json"));
    if(!file.open(QIODevice::WriteOnly)) fail("无法保存扫描任务："+file.errorString());
    const auto bytes=doc.toJson();
    if(file.write(bytes)!=bytes.size() || !file.commit()) fail("扫描任务保存失败，请检查磁盘空间："+file.errorString());
}
void Session::load(const QString &path) {
    Session candidate;
    candidate.directory=QFileInfo(path).isDir()?QFileInfo(path).absoluteFilePath():QFileInfo(path).absolutePath();
    candidate.holdDirectory();
    QFile file(QDir(candidate.directory).filePath("session.json"));
    if(!file.open(QIODevice::ReadOnly)) fail("无法打开扫描任务："+file.errorString());
    if(file.size()>32*1024*1024) fail("任务清单异常过大。");
    QJsonParseError error;
    const auto doc=QJsonDocument::fromJson(file.readAll(),&error);
    if(error.error!=QJsonParseError::NoError || !doc.isObject() || doc.object().value("schemaVersion").toInt()!=1 || !doc.object().value("pages").isArray()) fail("任务格式损坏或版本不受支持。");
    QSet<QString> ids;
    for(auto v:doc.object().value("pages").toArray()) {
        auto j=v.toObject(); Page p;
        p.id=j.value("id").toString(); p.originalFile=j.value("original").toString(); p.processedFile=j.value("processed").toString();
        if(p.id.isEmpty() || ids.contains(p.id)) fail("任务中出现重复或缺失的页面编号。");
        ids.insert(p.id);
        if(!j.value("edits").isObject()) fail("任务中的页面设置格式无效。");
        p.edits=editsFromJson(j.value("edits").toObject()); p.ocrText=j.value("ocrText").toString();
        if(!QFileInfo(candidate.absolute(p.originalFile)).isFile() || !QFileInfo(candidate.absolute(p.processedFile)).isFile()) fail("任务图片缺失，请完整复制任务文件夹。");
        candidate.pages.append(p);
    }
    *this=candidate;
}
void Session::saveImage(const QString &path,const QImage &image) {
    if(image.isNull()) fail("无法保存空图像。");
    QSaveFile out(path);
    if(!out.open(QIODevice::WriteOnly)) fail("无法写入图片："+out.errorString());
    if(!image.save(&out,"PNG") || !out.commit()) fail("图片保存失败，请检查磁盘空间和权限。");
}
Page Session::add(const QImage &original,const QImage &processed,const EditOptions &edits) {
    Page p; p.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.originalFile="originals/"+p.id+".png";
    p.processedFile="processed/"+p.id+".png"; p.edits=edits;
    saveImage(absolute(p.originalFile),original); saveImage(absolute(p.processedFile),processed);
    pages.append(p);
    try {save();} catch(...) {pages.removeLast(); throw;}
    return p;
}

Page Session::addCapture(const CapturedFrame &frame, const QImage &processed, const EditOptions &edits) {
    if (frame.size.isEmpty() || qint64(frame.size.width()) * frame.size.height() > 64000000)
        fail("拍摄图像尺寸无效。");

    const bool nativeJpeg = !frame.jpeg.isEmpty();
    Page page;
    page.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    page.edits = edits;
    page.originalFile = "originals/" + page.id + (nativeJpeg ? ".jpg" : ".png");

    if (nativeJpeg) {
        QBuffer header;
        header.setData(frame.jpeg);
        header.open(QIODevice::ReadOnly);
        QImageReader reader(&header, "JPEG");
        if (!frame.jpeg.endsWith(QByteArray::fromHex("ffd9")) || reader.size() != frame.size)
            fail("拍摄图像不完整，或实际分辨率与设置不符。");
        QSaveFile image(absolute(page.originalFile));
        if (!image.open(QIODevice::WriteOnly) || image.write(frame.jpeg) != frame.jpeg.size() || !image.commit())
            fail("无法写入原始拍摄图片，请检查任务目录和可用空间。");
    } else {
        saveImage(absolute(page.originalFile), frame.decode());
    }

    page.processedFile = page.originalFile;
    if (!processed.isNull()) {
        const bool binary = edits.color == ColorMode::BlackWhite;
        page.processedFile = "processed/" + page.id + (binary ? ".png" : ".jpg");
        QSaveFile image(absolute(page.processedFile));
        if (!image.open(QIODevice::WriteOnly))
            fail("无法创建处理后的页面，原图已保留在 originals 文件夹。");
        // 1-bit PNG is compact and lossless for binary text. Colour derivatives
        // use JPEG; future edits always start from the original camera bytes.
        const bool written = binary
            ? processed.convertToFormat(QImage::Format_Mono, Qt::ThresholdDither).save(&image, "PNG")
            : processed.save(&image, "JPEG", 95);
        if (!written || !image.commit())
            fail("处理结果写入失败，原图已保留在 originals 文件夹。");
    }

    pages.append(page);
    try { save(); }
    catch (...) { pages.removeLast(); throw; }
    return page;
}
void Session::update(const QString &id,const QImage &processed,const EditOptions &edits) {
    for(int i=0;i<pages.size();++i) if(pages[i].id==id) {
        Page previous=pages[i];
        // Versioned derivatives keep the old manifest usable if its atomic commit fails.
        auto next="processed/"+id+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".png";
        saveImage(absolute(next),processed);
        pages[i].processedFile=next; pages[i].edits=edits; pages[i].ocrText.clear();
        try {save();} catch(...) {pages[i]=previous; throw;}
        return;
    }
    fail("待修改页面不存在。");
}
void Session::setOcr(const QString &id,const QString &text) {
    for(auto &p:pages) if(p.id==id) {auto old=p.ocrText; p.ocrText=text; try{save();}catch(...){p.ocrText=old;throw;} return;}
    fail("待识别页面不存在。");
}
void Session::remove(const QVector<QString> &ids) {
    auto old=pages;
    for(int i=pages.size()-1;i>=0;--i) if(ids.contains(pages[i].id)) pages.remove(i);
    try{save();}catch(...){pages=old;throw;}
}
void Session::move(int from,int to) {
    if(from<0 || to<0 || from>=pages.size() || to>=pages.size()) return;
    pages.move(from,to); try{save();}catch(...){pages.move(to,from);throw;}
}
}
