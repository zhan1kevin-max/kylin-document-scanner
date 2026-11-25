#include "exporter.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <stdexcept>

namespace scan {
namespace {
[[noreturn]] void fail(const QString &message) {
    throw std::runtime_error(message.toUtf8().constData());
}
void checkCancel(const std::atomic_bool &cancel) {
    if (cancel.load()) fail(QStringLiteral("操作已取消，已保存的扫描页面不受影响。"));
}
void report(const Progress &progress, int done, int total, const QString &message) {
    if (progress) progress(done, total, message);
}

QImage loadImage(const QString &path) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && qint64(size.width()) * size.height() > 64000000)
        fail(QStringLiteral("图片超过 6400 万像素，请先缩小后再导出：") + path);
    QImage image = reader.read();
    if (image.isNull()) fail(QStringLiteral("无法读取页面图片：%1\n%2").arg(path, reader.errorString()));
    return image;
}

QString tessdataDirectory() {
    const QString configured = qEnvironmentVariable("SCANNER_TESSDATA_PREFIX").trimmed();
    if (!configured.isEmpty()) return configured;
    const QDir app(QCoreApplication::applicationDirPath());
    const QStringList candidates = {app.filePath(QStringLiteral("tessdata")),
        app.filePath(QStringLiteral("../share/kylin-document-scanner/tessdata"))};
    for (const QString &candidate : candidates)
        if (QDir(candidate).exists()) return QFileInfo(candidate).absoluteFilePath();
    return {}; // Respect the installed Tesseract default (and inherited TESSDATA_PREFIX).
}

void stopProcess(QProcess &process) {
    if (process.state() == QProcess::NotRunning) return;
    process.terminate();
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning && timer.elapsed() < 500)
        process.waitForFinished(100);
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        timer.restart();
        while (process.state() != QProcess::NotRunning && timer.elapsed() < 1500)
            process.waitForFinished(100);
    }
}

struct ProcessOutput { QByteArray output; QByteArray errors; };
ProcessOutput runProcess(const QString &executable, const QStringList &arguments,
                         std::atomic_bool &cancel, int timeoutMs) {
    checkCancel(cancel);
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(executable, arguments, QIODevice::ReadOnly);
    QElapsedTimer timer;
    timer.start();
    ProcessOutput result;
    auto collect = [&]() {
        result.output += process.readAllStandardOutput();
        result.errors += process.readAllStandardError();
        if (result.output.size() > 1024 * 1024) result.output = result.output.right(1024 * 1024);
        if (result.errors.size() > 16384) result.errors = result.errors.right(16384);
    };
    try {
        while (process.state() == QProcess::Starting) {
            checkCancel(cancel);
            if (timer.elapsed() > 5000) fail(QStringLiteral("启动文字识别程序超时。"));
            process.waitForStarted(100);
        }
        if (process.error() == QProcess::FailedToStart)
            fail(QStringLiteral("无法启动文字识别程序：%1\n%2").arg(executable, process.errorString()));
        while (process.state() != QProcess::NotRunning) {
            checkCancel(cancel);
            if (timer.elapsed() > timeoutMs) fail(QStringLiteral("文字识别超时，请减少页数或检查图片质量。"));
            process.waitForFinished(100);
            collect();
        }
        collect();
        checkCancel(cancel);
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            QString detail = QString::fromUtf8(result.errors).trimmed();
            if (detail.isEmpty()) detail = process.errorString();
            fail(QStringLiteral("文字识别程序失败（退出码 %1）：\n%2").arg(process.exitCode()).arg(detail));
        }
    } catch (...) {
        stopProcess(process);
        throw;
    }
    return result;
}

QStringList languageList(const QString &executable, const QString &dataDirectory,
                         std::atomic_bool &cancel) {
    QStringList arguments;
    if (!dataDirectory.isEmpty()) arguments << QStringLiteral("--tessdata-dir") << dataDirectory;
    arguments << QStringLiteral("--list-langs");
    const auto result = runProcess(executable, arguments, cancel, 10000);
    QStringList languages;
    const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_./-]+$"));
    for (const QString &line : QString::fromUtf8(result.output).split(QLatin1Char('\n'))) {
        const QString name = line.trimmed();
        if (valid.match(name).hasMatch() && !languages.contains(name)) languages.append(name);
    }
    return languages;
}

struct OcrConfig { QString executable; QString dataDirectory; };
OcrConfig ensureOcr(std::atomic_bool &cancel) {
    const OcrConfig config{Exporter::tesseractExecutable(), tessdataDirectory()};
    if (config.executable.isEmpty())
        fail(QStringLiteral("未找到 Tesseract 离线文字识别程序。请安装随包提供的 OCR 组件；普通 PDF 和图片导出仍可使用。"));
    const auto languages = languageList(config.executable, config.dataDirectory, cancel);
    QStringList missing;
    for (const QString &language : {QStringLiteral("chi_sim"), QStringLiteral("eng")})
        if (!languages.contains(language)) missing.append(language);
    if (!missing.isEmpty())
        fail(QStringLiteral("缺少离线识别语言数据：%1。请安装简体中文和英文模型；普通 PDF 和图片导出仍可使用。")
                 .arg(missing.join(QStringLiteral("、"))));
    return config;
}

QStringList ocrArguments(const OcrConfig &config, const QString &input, const QString &outputBase) {
    QStringList arguments{input, outputBase};
    if (!config.dataDirectory.isEmpty()) arguments << QStringLiteral("--tessdata-dir") << config.dataDirectory;
    arguments << QStringLiteral("-l") << QStringLiteral("chi_sim+eng")
              << QStringLiteral("--dpi") << QStringLiteral("300")
              << QStringLiteral("--psm") << QStringLiteral("3");
    return arguments;
}

QString readRecognizedText(const QString &path) {
    QFile text(path);
    if (!text.open(QIODevice::ReadOnly)) fail(QStringLiteral("识别完成，但无法读取文字结果：") + text.errorString());
    if (text.size() > 32 * 1024 * 1024) fail(QStringLiteral("文字识别结果过大，无法安全读取。"));
    return QString::fromUtf8(text.readAll());
}

QString recognizeWithConfig(const QString &imagePath, const OcrConfig &config, std::atomic_bool &cancel) {
    checkCancel(cancel);
    QTemporaryDir directory;
    if (!directory.isValid()) fail(QStringLiteral("无法创建文字识别临时目录，请检查磁盘空间和权限。"));
    const QString input = directory.filePath(QStringLiteral("page.png"));
    QImage image = loadImage(imagePath);
    if (!image.save(input, "PNG")) fail(QStringLiteral("无法写入文字识别临时图片，请检查磁盘空间。"));
    image = QImage();
    const QString output = directory.filePath(QStringLiteral("result"));
    runProcess(config.executable, ocrArguments(config, input, output), cancel, 120000);
    return readRecognizedText(output + QStringLiteral(".txt"));
}

void validateFileDestination(const ExportRequest &request) {
    const QFileInfo destination(request.destination);
    if (!destination.absoluteDir().exists()) fail(QStringLiteral("导出文件夹不存在，请重新选择保存位置。"));
    if (destination.exists() && !destination.isFile()) fail(QStringLiteral("保存位置不是普通文件。"));
    for (const QString &path : request.images) {
        const QFileInfo source(path);
        if (source.absoluteFilePath() == destination.absoluteFilePath() ||
            (source.exists() && destination.exists() && source.canonicalFilePath() == destination.canonicalFilePath()))
            fail(QStringLiteral("导出文件不能覆盖扫描任务中的页面图片。"));
    }
}

void atomicCopy(const QString &sourcePath, const QString &destination,
                std::atomic_bool &cancel, bool requirePdf) {
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) fail(QStringLiteral("无法读取临时导出结果：") + source.errorString());
    if (requirePdf && source.peek(5) != QByteArrayLiteral("%PDF-"))
        fail(QStringLiteral("文字识别程序未生成有效的 PDF 文件。"));
    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) fail(QStringLiteral("无法保存导出文件：") + output.errorString());
    while (!source.atEnd()) {
        checkCancel(cancel);
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() && source.error() != QFile::NoError) fail(QStringLiteral("读取导出结果失败：") + source.errorString());
        if (output.write(chunk) != chunk.size()) fail(QStringLiteral("写入导出文件失败：") + output.errorString());
    }
    checkCancel(cancel);
    if (!output.commit()) fail(QStringLiteral("无法完成文件保存：") + output.errorString());
}

QPageLayout pageLayout(const QImage &image, bool a4) {
    if (a4)
        return QPageLayout(QPageSize(QPageSize::A4), image.width() > image.height() ? QPageLayout::Landscape : QPageLayout::Portrait,
                           QMarginsF(10, 10, 10, 10), QPageLayout::Millimeter);
    const QSizeF points(image.width() * 72.0 / 300.0, image.height() * 72.0 / 300.0);
    return QPageLayout(QPageSize(points, QPageSize::Point, QStringLiteral("原始比例"), QPageSize::ExactMatch),
                       QPageLayout::Portrait, QMarginsF(), QPageLayout::Point);
}

void exportPdf(const ExportRequest &request, std::atomic_bool &cancel, const Progress &progress) {
    QSaveFile output(request.destination);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) fail(QStringLiteral("无法保存 PDF：") + output.errorString());
    {
        QPdfWriter writer(&output);
        writer.setResolution(300);
        writer.setCreator(QStringLiteral("麒麟文档拍照"));
        writer.setTitle(QFileInfo(request.destination).completeBaseName());
        QPainter painter;
        for (int index = 0; index < request.images.size(); ++index) {
            checkCancel(cancel);
            QImage image = loadImage(request.images.at(index));
            if (!writer.setPageLayout(pageLayout(image, request.a4))) fail(QStringLiteral("无法设置 PDF 页面尺寸。"));
            if (index == 0) {
                if (!painter.begin(&writer)) fail(QStringLiteral("无法开始生成 PDF。"));
            } else if (!writer.newPage()) fail(QStringLiteral("无法添加 PDF 页面，请检查磁盘空间。"));
            const QRectF available(0, 0, writer.width(), writer.height());
            const QSizeF size = image.size().scaled(available.size().toSize(), Qt::KeepAspectRatio);
            const QRectF target(QPointF((available.width() - size.width()) / 2.0, (available.height() - size.height()) / 2.0), size);
            painter.fillRect(available, Qt::white);
            painter.drawImage(target, image);
            report(progress, std::min(index + 1, request.images.size() - 1), request.images.size(),
                   QStringLiteral("正在生成 PDF：第 %1 / %2 页").arg(index + 1).arg(request.images.size()));
        }
        if (!painter.end()) fail(QStringLiteral("完成 PDF 绘制失败。"));
    } // QPdfWriter finalizes its cross-reference table before committing QSaveFile.
    checkCancel(cancel);
    if (!output.commit()) fail(QStringLiteral("保存 PDF 失败：") + output.errorString());
    report(progress, request.images.size(), request.images.size(), QStringLiteral("PDF 已保存"));
}

QImage ocrPage(const QImage &source, bool a4) {
    if (!a4) return source;
    const QSize paper = source.width() > source.height() ? QSize(3508, 2480) : QSize(2480, 3508);
    QImage page(paper, QImage::Format_RGB32);
    page.fill(Qt::white);
    const int margin = 118; // 10 mm at 300 DPI
    const QSize available = paper - QSize(2 * margin, 2 * margin);
    const QSize fitted = source.size().scaled(available, Qt::KeepAspectRatio);
    QPainter painter(&page);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRect(QPoint((paper.width() - fitted.width()) / 2, (paper.height() - fitted.height()) / 2), fitted), source);
    return page;
}

void exportSearchablePdf(const ExportRequest &request, std::atomic_bool &cancel, const Progress &progress) {
    const OcrConfig config = ensureOcr(cancel);
    QTemporaryDir directory;
    if (!directory.isValid()) fail(QStringLiteral("无法创建 PDF 识别临时目录。"));
    QFile list(directory.filePath(QStringLiteral("images.txt")));
    if (!list.open(QIODevice::WriteOnly)) fail(QStringLiteral("无法创建识别页面列表。"));
    for (int index = 0; index < request.images.size(); ++index) {
        checkCancel(cancel);
        const QString path = directory.filePath(QStringLiteral("page_%1.png").arg(index + 1, 6, 10, QLatin1Char('0')));
        const QImage page = ocrPage(loadImage(request.images.at(index)), request.a4);
        if (!page.save(path, "PNG")) fail(QStringLiteral("写入 PDF 临时页面失败，请检查磁盘空间。"));
        const QByteArray entry = path.toUtf8() + '\n';
        if (list.write(entry) != entry.size()) fail(QStringLiteral("写入识别页面列表失败。"));
        report(progress, 0, request.images.size(), QStringLiteral("准备识别：第 %1 / %2 页").arg(index + 1).arg(request.images.size()));
    }
    if (!list.flush()) fail(QStringLiteral("保存识别页面列表失败。"));
    list.close();
    report(progress, 0, request.images.size(), QStringLiteral("正在离线识别 %1 页，请稍候…").arg(request.images.size()));
    const QString base = directory.filePath(QStringLiteral("searchable"));
    QStringList arguments = ocrArguments(config, list.fileName(), base);
    arguments << QStringLiteral("pdf");
    const int timeout = int(std::min<qint64>(30 * 60 * 1000, 30000ll + qint64(request.images.size()) * 120000));
    runProcess(config.executable, arguments, cancel, timeout);
    atomicCopy(base + QStringLiteral(".pdf"), request.destination, cancel, true);
    report(progress, request.images.size(), request.images.size(), QStringLiteral("可搜索 PDF 已保存"));
}

void exportText(const ExportRequest &request, std::atomic_bool &cancel, const Progress &progress) {
    const OcrConfig config = ensureOcr(cancel);
    QSaveFile output(request.destination);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) fail(QStringLiteral("无法保存文字文件：") + output.errorString());
    for (int index = 0; index < request.images.size(); ++index) {
        checkCancel(cancel);
        report(progress, index, request.images.size(), QStringLiteral("正在识别：第 %1 / %2 页").arg(index + 1).arg(request.images.size()));
        const QString text = recognizeWithConfig(request.images.at(index), config, cancel);
        const QByteArray page = (QStringLiteral("===== 第 %1 页 =====\n").arg(index + 1) + text + QStringLiteral("\n\n")).toUtf8();
        if (output.write(page) != page.size()) fail(QStringLiteral("写入文字文件失败：") + output.errorString());
    }
    checkCancel(cancel);
    if (!output.commit()) fail(QStringLiteral("保存文字文件失败：") + output.errorString());
    report(progress, request.images.size(), request.images.size(), QStringLiteral("文字文件已保存"));
}

void exportImages(const ExportRequest &request, std::atomic_bool &cancel, const Progress &progress) {
    QDir directory(request.destination);
    if (!directory.exists() && !QDir().mkpath(request.destination)) fail(QStringLiteral("无法创建图片导出文件夹。"));
    if (!QFileInfo(request.destination).isDir()) fail(QStringLiteral("图片导出位置不是文件夹。"));
    const QByteArray format = request.kind == ExportKind::Jpeg ? QByteArrayLiteral("JPG") : QByteArrayLiteral("PNG");
    const QString extension = request.kind == ExportKind::Jpeg ? QStringLiteral("jpg") : QStringLiteral("png");
    for (int index = 0; index < request.images.size(); ++index) {
        checkCancel(cancel);
        const QImage image = loadImage(request.images.at(index));
        const QString path = directory.filePath(QStringLiteral("page_%1.%2").arg(index + 1, 3, 10, QLatin1Char('0')).arg(extension));
        QFile output(path);
        // NewOnly makes the no-overwrite policy atomic, including another process creating the file.
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            fail(QStringLiteral("无法创建图片（不会覆盖已有文件）：%1\n%2").arg(path, output.errorString()));
        QImageWriter writer(&output, format);
        if (request.kind == ExportKind::Jpeg) writer.setQuality(95);
        if (!writer.write(image) || !output.flush()) {
            const QString detail = writer.error() == QImageWriter::UnknownError ? output.errorString() : writer.errorString();
            output.close();
            QFile::remove(path); // Only the file created by this iteration is removed.
            fail(QStringLiteral("保存图片失败：") + detail);
        }
        output.close();
        report(progress, index + 1, request.images.size(), QStringLiteral("已导出 %1 / %2 页").arg(index + 1).arg(request.images.size()));
    }
}
} // namespace

QString Exporter::tesseractExecutable() {
    const QString configured = qEnvironmentVariable("SCANNER_TESSERACT").trimmed();
    if (!configured.isEmpty()) {
        const QFileInfo file(configured);
        if (file.isFile() && file.isExecutable()) return file.absoluteFilePath();
        return QStandardPaths::findExecutable(configured);
    }
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("tesseract.exe");
#else
    const QString name = QStringLiteral("tesseract");
#endif
    const QString local = QDir(QCoreApplication::applicationDirPath()).filePath(name);
    if (QFileInfo(local).isFile() && QFileInfo(local).isExecutable()) return local;
    return QStandardPaths::findExecutable(name);
}

QStringList Exporter::ocrLanguages() {
    const QString executable = tesseractExecutable();
    if (executable.isEmpty()) return {};
    std::atomic_bool cancel{false};
    return languageList(executable, tessdataDirectory(), cancel);
}

QString Exporter::recognize(const QString &imagePath, std::atomic_bool &cancel) {
    checkCancel(cancel);
    return recognizeWithConfig(imagePath, ensureOcr(cancel), cancel);
}

void Exporter::run(const ExportRequest &request, std::atomic_bool &cancel, Progress progress) {
    checkCancel(cancel);
    if (request.images.isEmpty()) fail(QStringLiteral("请先选择至少一页扫描图片。"));
    if (request.destination.trimmed().isEmpty()) fail(QStringLiteral("请选择导出位置。"));
    if (request.kind == ExportKind::Jpeg || request.kind == ExportKind::Png) {
        exportImages(request, cancel, progress);
        return;
    }
    validateFileDestination(request);
    switch (request.kind) {
    case ExportKind::Pdf: exportPdf(request, cancel, progress); break;
    case ExportKind::SearchablePdf: exportSearchablePdf(request, cancel, progress); break;
    case ExportKind::Text: exportText(request, cancel, progress); break;
    default: fail(QStringLiteral("未知的导出格式。"));
    }
}
} // namespace scan
