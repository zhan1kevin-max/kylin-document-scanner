#include "session.h"
#include <QCoreApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {
void check(bool result, const char *expression, int line) {
    if (!result) throw std::runtime_error(std::string("CHECK failed at line ") +
                                         std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(bool(expression), #expression, __LINE__)

template<class F> void mustThrow(F function) {
    bool threw = false;
    try { function(); } catch (const std::exception &) { threw = true; }
    CHECK(threw);
}
QByteArray readFile(const QString &path) {
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
void writeFile(const QString &path, const QByteArray &contents) {
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(file.write(contents) == contents.size());
}
QByteArray digest(const QString &path) {
    return QCryptographicHash::hash(readFile(path), QCryptographicHash::Sha256);
}
QImage solid(QRgb color, int width = 320, int height = 240) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    return image;
}
QVector<QString> ids(const scan::Session &session) {
    QVector<QString> result;
    for (const auto &page : session.pages) result.append(page.id);
    return result;
}
void testPersistenceAndEditing(const QString &base) {
    scan::Session session;
    session.create(base + "/任务-中文路径");
    scan::EditOptions initial;
    initial.autoCrop = false;
    const auto first = session.add(solid(qRgb(220, 30, 20)), solid(qRgb(220, 30, 20)), initial);
    const auto second = session.add(solid(qRgb(20, 200, 30)), solid(qRgb(20, 200, 30)), initial);
    const auto third = session.add(solid(qRgb(10, 40, 220)), solid(qRgb(10, 40, 220)), initial);
    session.setOcr(first.id, QString::fromUtf8("中文识别结果 ABC 123"));

    scan::Session reopened;
    reopened.load(session.directory + "/session.json");
    CHECK(ids(reopened) == ids(session));
    CHECK(reopened.pages[0].ocrText == QString::fromUtf8("中文识别结果 ABC 123"));
    CHECK(QImage(reopened.absolute(first.originalFile)) == solid(qRgb(220, 30, 20)));
    const auto originalDigest = digest(reopened.absolute(first.originalFile));
    const auto oldDerivative = reopened.pages[0].processedFile;

    scan::EditOptions edited;
    edited.autoCrop = false;
    edited.rotation = 90;
    edited.color = scan::ColorMode::Gray;
    edited.corners = {{.1,.1}, {.9,.1}, {.9,.9}, {.1,.9}};
    const auto editedImage = solid(qRgb(140, 140, 140), 192, 256);
    reopened.update(first.id, editedImage, edited);
    CHECK(reopened.pages[0].originalFile == first.originalFile);
    CHECK(digest(reopened.absolute(first.originalFile)) == originalDigest);
    CHECK(reopened.pages[0].ocrText.isEmpty());
    CHECK(QFileInfo::exists(reopened.absolute(oldDerivative)));

    scan::Session afterEdit;
    afterEdit.load(reopened.directory);
    CHECK(afterEdit.pages[0].edits.rotation == 90);
    CHECK(afterEdit.pages[0].edits.color == scan::ColorMode::Gray);
    CHECK(afterEdit.pages[0].edits.corners == edited.corners);
    CHECK(afterEdit.pages[0].ocrText.isEmpty());
    CHECK(QImage(afterEdit.absolute(afterEdit.pages[0].processedFile)) == editedImage);
    afterEdit.move(0, 2);
    CHECK(ids(afterEdit) == QVector<QString>({second.id, third.id, first.id}));
    afterEdit.remove({third.id});
    scan::Session afterRemove;
    afterRemove.load(afterEdit.directory);
    CHECK(ids(afterRemove) == QVector<QString>({second.id, first.id}));
    CHECK(QFileInfo::exists(afterRemove.absolute(third.originalFile)));
}

void testPartialA4RoundTrip(const QString &base) {
    scan::Session session;
    session.create(base + "/partial-a4");
    const auto image=solid(Qt::white);
    scan::EditOptions cut;
    cut.corners={{.1,.1},{.9,.1},{.9,.9},{.1,.9}};
    cut.missingTriangle={{.9,.1},{.73,.1},{.9,.27}};
    session.add(image,image,cut);

    scan::EditOptions clipped;
    clipped.corners={{.1,.15},{.8,-.04},{.94,.83},{.18,.91}};
    session.add(image,image,clipped);
    scan::Session reopened;
    reopened.load(session.directory);
    CHECK(reopened.pages[0].edits.missingTriangle==cut.missingTriangle);
    CHECK(reopened.pages[0].edits.corners==cut.corners);
    CHECK(reopened.pages[1].edits.corners==clipped.corners);
    CHECK(reopened.pages[1].edits.missingTriangle.isEmpty());
}

void testInvalidManifestDoesNotReplaceSession(const QString &base) {
    scan::Session current;
    current.create(base + "/current");
    scan::EditOptions options;
    options.autoCrop = false;
    const auto page = current.add(solid(qRgb(255, 255, 255)), solid(qRgb(255, 255, 255)), options);
    const auto oldDirectory = current.directory;
    const auto oldIds = ids(current);
    const auto originalBytes = readFile(current.directory + "/session.json");
    const auto badDirectory = base + "/broken";
    CHECK(QDir().mkpath(badDirectory));
    writeFile(badDirectory + "/session.json", "{broken JSON");
    mustThrow([&] { current.load(badDirectory); });
    CHECK(current.directory == oldDirectory && ids(current) == oldIds);

    auto object = QJsonDocument::fromJson(originalBytes).object();
    auto pages = object.value("pages").toArray();
    auto item = pages[0].toObject();
    item["original"] = "../outside.png";
    pages[0] = item;
    object["pages"] = pages;
    writeFile(base + "/outside.png", readFile(current.absolute(page.originalFile)));
    writeFile(badDirectory + "/session.json", QJsonDocument(object).toJson());
    mustThrow([&] { current.load(badDirectory); });
    CHECK(current.directory == oldDirectory && ids(current) == oldIds);

    object = QJsonDocument::fromJson(originalBytes).object();
    pages = object.value("pages").toArray();
    pages.append(pages[0]);
    object["pages"] = pages;
    // Use the original task's image files so duplicate-ID validation, rather
    // than a missing-file check, is what rejects the altered manifest.
    writeFile(current.directory + "/session.json", QJsonDocument(object).toJson());
    mustThrow([&] { current.load(current.directory); });
    CHECK(current.directory == oldDirectory && ids(current) == oldIds);
    writeFile(current.directory + "/session.json", originalBytes);
    mustThrow([&] { current.create(current.directory); });
    CHECK(current.directory == oldDirectory && ids(current) == oldIds);
}

void testNativeJpegCapture(const QString &base) {
    scan::Session task;
    task.create(base + "/相机原始文件");
    scan::CapturedFrame frame;
    const auto source = solid(qRgb(25, 91, 156), 321, 243);
    frame.size = source.size();
    QBuffer buffer(&frame.jpeg);
    CHECK(buffer.open(QIODevice::WriteOnly));
    CHECK(source.save(&buffer, "JPEG", 93));
    buffer.close();
    CHECK(frame.decode().size() == source.size());
    CHECK(frame.preview(80).width() == 80);
    scan::EditOptions unedited;
    unedited.autoCrop = false;
    const auto first = task.addCapture(frame, QImage(), unedited);
    CHECK(first.originalFile.endsWith(".jpg"));
    CHECK(first.processedFile == first.originalFile);
    CHECK(readFile(task.absolute(first.originalFile)) == frame.jpeg);

    scan::EditOptions edits;
    edits.color = scan::ColorMode::BlackWhite;
    QImage blackAndWhite(80, 52, QImage::Format_Grayscale8);
    blackAndWhite.fill(Qt::white);
    blackAndWhite.setPixelColor(4, 6, Qt::black);
    const auto second = task.addCapture(frame, blackAndWhite, edits);
    const QImage restored(task.absolute(second.processedFile));
    CHECK(restored.size() == blackAndWhite.size());
    CHECK(restored.pixelColor(4, 6) == QColor(Qt::black));
    CHECK(restored.pixelColor(5, 6) == QColor(Qt::white));
    CHECK(readFile(task.absolute(second.originalFile)) == frame.jpeg);
    scan::Session reopened;
    reopened.load(task.directory);
    CHECK(reopened.pages.size() == 2);
    CHECK(reopened.pages[0].processedFile == first.originalFile);

    frame.jpeg.chop(2);
    mustThrow([&] { task.addCapture(frame, QImage(), unedited); });
    CHECK(task.pages.size() == 2);
}

void testFailedCommitKeepsOldPages(const QString &base) {
    scan::Session session;
    session.create(base + "/commit-failure");
    scan::EditOptions initial;
    initial.autoCrop = false;
    const auto first = session.add(solid(qRgb(9, 21, 37)), solid(qRgb(9, 21, 37)), initial);
    const auto second = session.add(solid(qRgb(18, 42, 74)), solid(qRgb(18, 42, 74)), initial);
    session.setOcr(first.id, "retained OCR");
    const auto oldIds = ids(session);
    const auto originalHash = digest(session.absolute(first.originalFile));
    const auto derivativeHash = digest(session.absolute(first.processedFile));
    const auto manifest = session.directory + "/session.json";
    const auto backup = session.directory + "/old-manifest.json";
    const auto oldBytes = readFile(manifest);
    CHECK(QFile::rename(manifest, backup));
    // A directory at the manifest filename reliably fails even when tests run
    // as root; a read-only file would not provide that guarantee.
    CHECK(QDir().mkdir(manifest));
    scan::EditOptions updated = initial;
    updated.rotation = 180;
    mustThrow([&] { session.update(first.id, solid(qRgb(255, 0, 0)), updated); });
    CHECK(session.pages[0].processedFile == first.processedFile);
    CHECK(session.pages[0].ocrText == "retained OCR");
    CHECK(session.pages[0].edits.rotation == 0);
    mustThrow([&] { session.add(solid(qRgb(0, 255, 0)), solid(qRgb(0, 255, 0)), initial); });
    CHECK(ids(session) == oldIds);
    mustThrow([&] { session.setOcr(first.id, "lost OCR"); });
    CHECK(session.pages[0].ocrText == "retained OCR");
    mustThrow([&] { session.move(0, 1); });
    CHECK(ids(session) == oldIds);
    mustThrow([&] { session.remove({second.id}); });
    CHECK(ids(session) == oldIds);
    CHECK(digest(session.absolute(first.originalFile)) == originalHash);
    CHECK(digest(session.absolute(first.processedFile)) == derivativeHash);
    CHECK(QDir().rmdir(manifest));
    CHECK(QFile::rename(backup, manifest));
    CHECK(readFile(manifest) == oldBytes);
    scan::Session restored;
    restored.load(session.directory);
    CHECK(ids(restored) == oldIds);
    CHECK(restored.pages[0].ocrText == "retained OCR");
}

void testSymlinkDoesNotEscape(const QString &base) {
#ifdef Q_OS_UNIX
    QTemporaryDir outside;
    CHECK(outside.isValid());
    const auto task = base + "/symlink-task";
    CHECK(QDir().mkpath(task));
    const auto link = task + "/originals";
    CHECK(::symlink(QFile::encodeName(outside.path()).constData(),
                    QFile::encodeName(link).constData()) == 0);
    scan::Session session;
    // Creation itself may reject the unsafe directory; otherwise the first
    // attempted image write must reject it before leaving the task root.
    bool rejected = false;
    try {
        session.create(task);
        scan::EditOptions options;
        session.add(solid(qRgb(255, 255, 255)), solid(qRgb(255, 255, 255)), options);
    } catch (const std::exception &) { rejected = true; }
    CHECK(rejected);
    CHECK(session.pages.isEmpty());
    CHECK(QDir(outside.path()).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
#else
    Q_UNUSED(base)
    std::cout << "SKIP: POSIX symlink test on non-POSIX platform\n";
#endif
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        testPersistenceAndEditing(directory.path());
        testPartialA4RoundTrip(directory.path());
        testNativeJpegCapture(directory.path());
        testInvalidManifestDoesNotReplaceSession(directory.path());
        testFailedCommitKeepsOldPages(directory.path());
        testSymlinkDoesNotEscape(directory.path());
        std::cout << "Session persistence and rollback tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
