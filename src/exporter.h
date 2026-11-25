#pragma once
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
namespace scan {
using Progress = std::function<void(int done, int total, const QString &message)>;
enum class ExportKind { Pdf, SearchablePdf, Text, Jpeg, Png };
struct ExportRequest {
    ExportKind kind = ExportKind::Pdf;
    QStringList images; // processed image absolute paths, in page order
    QString destination; // output file, or new output directory for Jpeg/Png
    bool a4 = true;
};
class Exporter {
public:
    static QString tesseractExecutable();
    static QStringList ocrLanguages();
    static QString recognize(const QString &imagePath, std::atomic_bool &cancel);
    static void run(const ExportRequest &request, std::atomic_bool &cancel, Progress progress = {});
};
}
