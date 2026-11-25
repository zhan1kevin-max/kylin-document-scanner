#include "mainwindow.h"
#include "barcode.h"
#include "cropcanvas.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QPageSize>
#include <QPlainTextEdit>
#include <QPrintDialog>
#include <QPrinter>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QUuid>
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace scan {
namespace {
QString uniqueOutput(const QString &directory, const QString &prefix, const QString &suffix) {
    if (!QDir().mkpath(directory))
        throw std::runtime_error("无法创建输出目录，请检查写入权限。");
    const auto stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    const auto id = QUuid::createUuid().toString(QUuid::Id128).left(8);
    return QDir(directory).absoluteFilePath(prefix + "_" + stamp + "_" + id + "." + suffix);
}

QImage readPage(const QString &path) {
    QImageReader reader(path);
    const QSize size = reader.size();
    if (qint64(size.width()) * size.height() > 64000000)
        throw std::runtime_error("图片超过 6400 万像素。");
    reader.setAutoTransform(true);
    const auto image = reader.read();
    if (image.isNull())
        throw std::runtime_error(QString("无法读取 %1：%2").arg(QFileInfo(path).fileName(), reader.errorString()).toUtf8().constData());
    return image;
}

QString recordingInputFormat(quint32 fourcc) {
    QByteArray code;
    for (int i = 0; i < 4; ++i) code.append(char(fourcc >> (i * 8)));
    if (code == "MJPG" || code == "JPEG") return "mjpeg";
    if (code == "YUYV") return "yuyv422";
    if (code == "UYVY") return "uyvy422";
    if (code == "RGB3") return "rgb24";
    if (code == "BGR3") return "bgr24";
    return {};
}


}

void MainWindow::printPages() try {
    if (busy_ || capturePending_ || recording_) return;
    stopBatch();
    const auto paths = outputSelection();
    if (paths.isEmpty()) { fail("请先拍摄或选择需要复印的页面。"); return; }

    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName("高拍仪复印");
    printer.setPageSize(QPageSize(QPageSize::A4));
    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle("复印所选页面");
    dialog.setMinMax(1, paths.size());
    dialog.setOption(QAbstractPrintDialog::PrintPageRange);
    if (dialog.exec() != QDialog::Accepted) return;

    int first = 0, last = paths.size() - 1;
    if (printer.printRange() == QPrinter::PageRange) {
        first = std::clamp(printer.fromPage() - 1, 0, last);
        last = std::clamp(printer.toPage() - 1, first, last);
    }
    QVector<int> pages;
    for (int n = first; n <= last; ++n) pages.append(n);
    if (printer.pageOrder() == QPrinter::LastPageFirst) std::reverse(pages.begin(), pages.end());
    // Some PDF and printer backends require the application to emit each copy.
    const int copies = printer.supportsMultipleCopies() ? 1 : qMax(1, printer.copyCount());
    QVector<int> order;
    if (printer.collateCopies()) {
        for (int i = 0; i < copies; ++i) order += pages;
    } else {
        for (int page : pages)
            for (int i = 0; i < copies; ++i) order.append(page);
    }
    // Verify the first page before starting the print job.
    auto image = readPage(paths[order.first()]);
    QPainter painter;
    if (!painter.begin(&printer)) { fail("无法开始打印，请检查所选打印机。"); return; }
    try {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        for (int i = 0; i < order.size(); ++i) {
            if (i > 0) {
                image = readPage(paths[order[i]]);
                if (!printer.newPage()) throw std::runtime_error("打印机无法开始下一页。");
            }
            const QRect paper = painter.viewport();
            QRect content(QPoint(0, 0), image.size().scaled(paper.size(), Qt::KeepAspectRatio));
            content.moveCenter(paper.center());
            painter.drawImage(content, image);
        }
    } catch (...) {
        printer.abort();
        painter.end();
        throw;
    }
    if (!painter.end() || printer.printerState() == QPrinter::Error)
        throw std::runtime_error("打印作业提交失败，请检查打印队列。");
    message(QString("已向打印队列提交 %1 页。").arg(order.size()));
} catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); }

void MainWindow::draftEmail() try {
    if (busy_ || capturePending_ || recording_) return;
    stopBatch();
    const auto paths = outputSelection();
    if (paths.isEmpty()) { fail("请先拍摄或选择要作为附件的页面。"); return; }
    const auto mailer = QStandardPaths::findExecutable("xdg-email");
    if (mailer.isEmpty()) { fail("找不到 xdg-email，请安装离线包中的 xdg-utils。"); return; }

    ExportRequest request;
    request.kind = ExportKind::Pdf;
    request.images = paths;
    request.destination = uniqueOutput(exportDirectory(), "电邮附件", "pdf");
    runJob([this, request] {
        Exporter::run(request, cancel_, [this](int i, int total, const QString &s) { progress(i, total, s); });
    }, [this, mailer, file = request.destination] {
        // Attachments stay on disk because a mail client may read them later.
        auto *process = new QProcess(qApp);
        connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) process->deleteLater();
        });
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                process, &QObject::deleteLater);
        connect(process, &QProcess::errorOccurred, this, [this, file](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                message("无法启动默认邮件程序。附件已保存到 " + file);
            }
        });
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, file](int code, QProcess::ExitStatus status) {
            if (code || status != QProcess::NormalExit) message("默认邮件程序无法打开。附件已保存到 " + file);
            else message("邮件草稿已交给默认邮件程序。附件位置：" + file);
        });
        process->start(mailer, {"--utf8", "--subject", "扫描文档", "--attach", file});
        message("正在打开邮件草稿。附件位置：" + file);
    });
} catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); }

void MainWindow::readBarcode() try {
    if (busy_ || capturePending_ || recording_) return;
    stopBatch();
    const auto paths = outputSelection();
    if (paths.isEmpty()) { fail("请先拍摄或选择包含条码、二维码的图片。"); return; }
    auto result = std::make_shared<QStringList>();
    runJob([this, paths, result] {
        for (int i = 0; i < paths.size(); ++i) {
            if (cancel_.load()) throw std::runtime_error("条码识别已取消。");
            const auto lines = scanBarcodes(readPage(paths[i]));
            result->append(QString("第 %1 页").arg(i + 1));
            if (lines.isEmpty()) result->append("未识别到条码");
            else result->append(lines);
            result->append(QString());
            progress(i + 1, paths.size(), QString("已检查 %1 / %2 页").arg(i + 1).arg(paths.size()));
        }
    }, [this, result] {
        const auto text = result->join('\n').trimmed();
        ocr_->setPlainText(text);
        showOcr();
        message("条码识别完成，可在结果窗口选择和复制。");
    });
} catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); }

void MainWindow::setupRecorder() {
    // The second FFmpeg output is a bounded stream of small JPEG preview frames.
    auto preview = std::make_shared<QByteArray>();
    auto errors = std::make_shared<QByteArray>();
    connect(&recorder_, &QProcess::started, this, [this, preview, errors] {
        preview->clear(); errors->clear();
        message("正在录像 · 点击“停止录像”保存 MKV 文件。");
    });
    connect(&recorder_, &QProcess::readyReadStandardError, this, [this, errors] {
        errors->append(recorder_.readAllStandardError());
        if (errors->size() > 4096) errors->remove(0, errors->size() - 4096);
    });
    connect(&recorder_, &QProcess::readyReadStandardOutput, this, [this, preview] {
        preview->append(recorder_.readAllStandardOutput());
        static const QByteArray soi = QByteArray::fromHex("ffd8");
        static const QByteArray eoi = QByteArray::fromHex("ffd9");
        const int end = preview->lastIndexOf(eoi);
        if (end >= 2) {
            const int begin = preview->lastIndexOf(soi, end - 2);
            if (begin >= 0) {
                const auto image = QImage::fromData(reinterpret_cast<const uchar *>(preview->constData() + begin),
                                                    end + 2 - begin, "JPEG");
                if (!image.isNull() && !closing_) live_->setImage(image);
            }
            preview->remove(0, end + 2);
        }
        if (preview->size() > 4 * 1024 * 1024) preview->clear();
    });
    const auto finish = [this, errors](bool success) {
        if (!recording_) return;
        recording_ = false;
        modeBar_->setEnabled(true);
        shoot_->setEnabled(true);
        connect_->setEnabled(true);
        devices_->setEnabled(true); resolution_->setEnabled(true);
        const bool resume = resumeCamera_;
        resumeCamera_ = false;
        if (closing_) return;
        switchMode(mode_);
        if (resume && !camera_.isRunning()) toggleCamera();
        if (success && QFileInfo(recordingPath_).size() > 0) {
            message("录像已保存：" + recordingPath_);
            browseResults();
        } else {
            const QString detail = QString::fromLocal8Bit(*errors).trimmed().right(500);
            fail("录像未能完成。请确认设备可用、输出目录有空间。\n" + detail);
        }
    };
    connect(&recorder_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [finish](int code, QProcess::ExitStatus status) { finish(code == 0 && status == QProcess::NormalExit); });
    connect(&recorder_, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(false);
    });
}

void MainWindow::toggleRecording() try {
    if (recording_) {
        shoot_->setEnabled(false);
        message("正在保存录像……");
        recorder_.write("q\n");
        if (closing_) {
            if (!recorder_.waitForFinished(3500) && recorder_.state() != QProcess::NotRunning) {
                recorder_.terminate();
                if (!recorder_.waitForFinished(1500)) { recorder_.kill(); recorder_.waitForFinished(1000); }
            }
        } else {
            const auto path = recordingPath_;
            QTimer::singleShot(6000, this, [this, path] {
                if (recording_ && recordingPath_ == path && recorder_.state() != QProcess::NotRunning)
                    recorder_.terminate();
            });
            QTimer::singleShot(9000, this, [this, path] {
                if (recording_ && recordingPath_ == path && recorder_.state() != QProcess::NotRunning)
                    recorder_.kill();
            });
        }
        return;
    }
    if (busy_ || capturePending_ || closing_) return;
    stopBatch();
    const auto ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) { fail("找不到录像程序，请安装离线包中的 FFmpeg。"); return; }
    const int index = resolution_->currentIndex();
    if (index < 0 || index >= formats_.size() || devices_->currentIndex() < 0) {
        fail("请先选择摄像头和录像分辨率。"); return;
    }
    const CameraFormat format = formats_[index];
    const auto pixelFormat = recordingInputFormat(format.fourcc);
    if (pixelFormat.isEmpty()) { fail("该视频格式无法录像，请选择 MJPG 或 YUYV。"); return; }
    recordingPath_ = uniqueOutput(exportDirectory(), "录像", "mkv");
    const auto device = devices_->currentData().toString();
    resumeCamera_ = camera_.isRunning();
    camera_.stop();
    recording_ = true;
    modeBar_->setEnabled(false);
    devices_->setEnabled(false); resolution_->setEnabled(false); connect_->setEnabled(false);
    shoot_->setText("停止录像");
    tabs_->setCurrentIndex(0);
    live_->setImage({});
    live_->setMessage("正在打开录像设备……");
    const QStringList input = {"-hide_banner", "-loglevel", "error", "-n", "-filter_threads", "1",
                               "-f", "video4linux2", "-input_format", pixelFormat, "-video_size",
                               QString("%1x%2").arg(format.width).arg(format.height), "-i", device};
    QStringList arguments = input;
    arguments << "-map" << "0:v:0" << "-an" << "-c:v" << (pixelFormat == "mjpeg" ? "copy" : "mpeg4")
              << "-f" << "matroska" << recordingPath_;
    arguments << "-map" << "0:v:0" << "-an" << "-vf" << "fps=4,scale=640:-2"
              << "-c:v" << "mjpeg" << "-threads" << "1" << "-q:v" << "5" << "-f" << "image2pipe" << "pipe:1";
    recorder_.start(ffmpeg, arguments, QIODevice::ReadWrite);
} catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); }
}
