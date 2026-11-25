#include "exporter.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
template<class Action> void requireFailure(Action action, const char *message) {
    bool failed = false;
    try { action(); } catch (const std::exception &) { failed = true; }
    require(failed, message);
}
QByteArray readFile(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "cannot read output");
    return file.readAll();
}
void writeFile(const QString &path, const QByteArray &contents) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "cannot write fixture");
    require(file.write(contents) == contents.size(), "fixture write failed");
}
struct Environment {
    QByteArray old = qgetenv("SCANNER_TESSERACT");
    bool present = qEnvironmentVariableIsSet("SCANNER_TESSERACT");
    ~Environment() { if (present) qputenv("SCANNER_TESSERACT", old); else qunsetenv("SCANNER_TESSERACT"); }
};

void testExports(const QString &root, const QStringList &images) {
    using namespace scan;
    std::atomic_bool cancel{false};
    const QString pdf = root + QStringLiteral("/顺序文档.pdf");
    Exporter::run({ExportKind::Pdf, images, pdf, true}, cancel);
    const QByteArray bytes = readFile(pdf);
    require(bytes.startsWith("%PDF-"), "ordinary PDF needs a PDF header");
    require(bytes.contains("%%EOF"), "ordinary PDF must be finalized before save");
    // Qt 5's QByteArray overload stops at an embedded NUL in a PDF stream.
    // The explicit length is required to inspect objects after binary images.
    const QString raw = QString::fromLatin1(bytes.constData(), bytes.size());
    auto pageMatches = QRegularExpression(QStringLiteral("/Type\\s*/Page\\b")).globalMatch(raw);
    int count = 0;
    while (pageMatches.hasNext()) { pageMatches.next(); ++count; }
    require(count == images.size(), "ordinary PDF must retain every ordered page");
    const QString originalPdf = root + QStringLiteral("/原比例.pdf");
    Exporter::run({ExportKind::Pdf, images, originalPdf, false}, cancel);
    require(readFile(originalPdf).startsWith("%PDF-"), "original-ratio PDF failed");

    const QString pngs = root + QStringLiteral("/图片 导出");
    int lastProgress = 0;
    Exporter::run({ExportKind::Png, images, pngs, true}, cancel,
                  [&](int done, int total, const QString &) {
                      require(total == 2 && done == lastProgress + 1, "image progress must count saved pages");
                      lastProgress = done;
                  });
    require(lastProgress == 2, "image progress must finish");
    QImage first(pngs + QStringLiteral("/page_001.png")), second(pngs + QStringLiteral("/page_002.png"));
    require(first.size() == QSize(240, 320) && second.size() == QSize(400, 200), "image dimensions/order changed");
    require(first.pixelColor(20, 20) == QColor(Qt::red), "first image should remain red");
    require(second.pixelColor(20, 20) == QColor(Qt::blue), "second image should remain blue");
    const QByteArray firstBefore = readFile(pngs + QStringLiteral("/page_001.png"));
    requireFailure([&]() { Exporter::run({ExportKind::Png, images, pngs, true}, cancel); }, "image export must refuse overwriting");
    require(readFile(pngs + QStringLiteral("/page_001.png")) == firstBefore, "existing image was changed");

    const QString jpgs = root + QStringLiteral("/JPEG");
    Exporter::run({ExportKind::Jpeg, images, jpgs, true}, cancel);
    QImage jpg(jpgs + QStringLiteral("/page_001.jpg"));
    require(!jpg.isNull() && jpg.pixelColor(10, 10).red() > 240, "JPEG should be readable and retain content");
}

void testFailureAtomicity(const QString &root, const QStringList &images) {
    using namespace scan;
    const QString existing = root + QStringLiteral("/不能损坏.pdf");
    const QByteArray sentinel("existing-important-document");
    writeFile(existing, sentinel);
    std::atomic_bool cancel{true};
    requireFailure([&]() { Exporter::run({ExportKind::Pdf, images, existing, true}, cancel); }, "pre-cancel must fail");
    require(readFile(existing) == sentinel, "cancel replaced existing PDF");
    cancel.store(false);
    QStringList bad = images;
    bad.append(root + QStringLiteral("/missing.png"));
    requireFailure([&]() { Exporter::run({ExportKind::Pdf, bad, existing, true}, cancel); }, "unreadable page must fail");
    require(readFile(existing) == sentinel, "partial PDF replaced existing file");
    requireFailure([&]() { Exporter::run({ExportKind::Pdf, images, images[0], true}, cancel); }, "source image overwrite must fail");

    requireFailure([&]() {
        Exporter::run({ExportKind::Pdf, images, existing, true}, cancel,
                      [&](int, int, const QString &) { cancel.store(true); });
    }, "mid-export PDF cancel must fail");
    require(readFile(existing) == sentinel, "mid-export cancellation replaced original PDF");
    cancel.store(false);
    const QString partialImages = root + QStringLiteral("/partial-images");
    requireFailure([&]() {
        Exporter::run({ExportKind::Png, images, partialImages, true}, cancel,
                      [&](int done, int, const QString &) { if (done == 1) cancel.store(true); });
    }, "image export must observe cancellation between pages");
    require(QFile::exists(partialImages + QStringLiteral("/page_001.png")), "completed image should remain after cancel");
    require(!QFile::exists(partialImages + QStringLiteral("/page_002.png")), "cancelled second image should not exist");
}

void testOcrFailure(const QString &root, const QStringList &images) {
    using namespace scan;
    Environment restore;
    qputenv("SCANNER_TESSERACT", (root + QStringLiteral("/no-such-ocr")).toUtf8());
    require(Exporter::tesseractExecutable().isEmpty(), "invalid configured executable must not silently use system OCR");
    require(Exporter::ocrLanguages().isEmpty(), "missing OCR must return no languages");
    std::atomic_bool cancel{false};
    requireFailure([&]() { Exporter::recognize(images[0], cancel); }, "missing OCR must produce an error");
    Exporter::run({ExportKind::Pdf, images, root + QStringLiteral("/无需OCR.pdf"), true}, cancel);

#ifdef Q_OS_UNIX
    const QString fake = root + QStringLiteral("/假 OCR 程序.sh");
    const QByteArray script =
        "#!/bin/sh\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$arg\" = --list-langs ]; then printf 'List of available languages (2):\\nchi_sim\\neng\\n'; exit 0; fi\n"
        "done\n"
        "[ -f \"$1\" ] || exit 19\n"
        "printf 'intentional test failure\\n' >&2\n"
        "exit 7\n";
    writeFile(fake, script);
    require(QFile::setPermissions(fake, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner), "cannot make OCR fixture executable");
    qputenv("SCANNER_TESSERACT", fake.toUtf8());
    require(Exporter::ocrLanguages().contains(QStringLiteral("chi_sim")), "OCR discovery must support spaces and Chinese paths");
    const QString protectedPdf = root + QStringLiteral("/OCR失败保留.pdf");
    writeFile(protectedPdf, QByteArrayLiteral("keep-me"));
    requireFailure([&]() { Exporter::run({ExportKind::SearchablePdf, images, protectedPdf, true}, cancel); }, "failed OCR process must propagate error");
    require(readFile(protectedPdf) == QByteArrayLiteral("keep-me"), "OCR process failure damaged existing output");

    const QByteArray textScript =
        "#!/bin/sh\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$arg\" = --list-langs ]; then printf 'chi_sim\\neng\\n'; exit 0; fi\n"
        "done\n"
        "[ -f \"$1\" ] || exit 19\n"
        "printf '识别文字 UTF-8\\n' > \"$2.txt\"\n";
    writeFile(fake, textScript);
    const QString recognized = Exporter::recognize(images[0], cancel);
    require(recognized.contains(QStringLiteral("识别文字")), "OCR text must be decoded as UTF-8");
    const QString textOut = root + QStringLiteral("/文字.txt");
    Exporter::run({ExportKind::Text, images, textOut, true}, cancel);
    const QString text = QString::fromUtf8(readFile(textOut));
    require(text.indexOf(QStringLiteral("第 1 页")) < text.indexOf(QStringLiteral("第 2 页")), "TXT page order must match document");
    require(text.count(QStringLiteral("识别文字")) == 2, "TXT must include all recognized pages");

    const QByteArray slowScript =
        "#!/bin/sh\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$arg\" = --list-langs ]; then printf 'chi_sim\\neng\\n'; exit 0; fi\n"
        "done\n"
        "exec sleep 20\n";
    writeFile(fake, slowScript);
    const auto started = std::chrono::steady_clock::now();
    std::thread cancelThread([&]() { std::this_thread::sleep_for(std::chrono::milliseconds(400)); cancel.store(true); });
    bool wasCancelled = false;
    try { Exporter::recognize(images[0], cancel); } catch (const std::exception &) { wasCancelled = true; }
    cancelThread.join();
    require(wasCancelled, "running OCR must be cancellable");
    require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5), "OCR cancellation took too long");
#endif
}
}

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "cannot create test directory");
        const QString root = directory.path() + QStringLiteral("/中文 路径");
        require(QDir().mkpath(root), "cannot create unicode fixture directory");
        const QStringList images{root + QStringLiteral("/一.png"), root + QStringLiteral("/二.png")};
        QImage first(240, 320, QImage::Format_RGB32), second(400, 200, QImage::Format_RGB32);
        first.fill(Qt::red); second.fill(Qt::blue);
        require(first.save(images[0]) && second.save(images[1]), "cannot save image fixtures");
        testExports(root, images);
        testFailureAtomicity(root, images);
        testOcrFailure(root, images);
        std::cout << "export pipeline: passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "export pipeline: FAILED: " << error.what() << '\n';
        return 1;
    }
}
