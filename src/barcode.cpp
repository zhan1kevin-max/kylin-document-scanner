#include "barcode.h"
#include <QLibrary>
#include <memory>
#include <cstring>
#include <stdexcept>

namespace {
struct zbar_image_s;
struct zbar_symbol_s;
struct zbar_image_scanner_s;
using Image = zbar_image_s;
using Symbol = zbar_symbol_s;
using Scanner = zbar_image_scanner_s;
using Cleanup = void (*)(Image *);

template <class Function>
Function resolve(QLibrary &library, const char *name) {
    auto pointer = library.resolve(name);
    if (!pointer)
        throw std::runtime_error(QString("条码识别库缺少函数 %1。").arg(QString::fromLatin1(name)).toUtf8().constData());
    return reinterpret_cast<Function>(pointer);
}
}

namespace scan {
QStringList scanBarcodes(const QImage &image) {
    if (image.isNull() || qint64(image.width()) * image.height() > 64000000)
        throw std::runtime_error("条码图片无法读取或尺寸过大。");
    QLibrary library("zbar", 0);
    if (!library.load()) throw std::runtime_error("未找到条码识别组件 libzbar0，请重新运行离线安装程序。");
    auto createScanner = resolve<Scanner *(*)()>(library, "zbar_image_scanner_create");
    auto destroyScanner = resolve<void (*)(Scanner *)>(library, "zbar_image_scanner_destroy");
    auto createImage = resolve<Image *(*)()>(library, "zbar_image_create");
    auto destroyImage = resolve<void (*)(Image *)>(library, "zbar_image_destroy");
    auto setFormat = resolve<void (*)(Image *, unsigned long)>(library, "zbar_image_set_format");
    auto setSize = resolve<void (*)(Image *, unsigned, unsigned)>(library, "zbar_image_set_size");
    auto setData = resolve<void (*)(Image *, const void *, unsigned long, Cleanup)>(library, "zbar_image_set_data");
    auto scan = resolve<int (*)(Scanner *, Image *)>(library, "zbar_scan_image");
    auto first = resolve<const Symbol *(*)(const Image *)>(library, "zbar_image_first_symbol");
    auto next = resolve<const Symbol *(*)(const Symbol *)>(library, "zbar_symbol_next");
    auto symbolType = resolve<int (*)(const Symbol *)>(library, "zbar_symbol_get_type");
    auto symbolName = resolve<const char *(*)(int)>(library, "zbar_get_symbol_name");
    auto value = resolve<const char *(*)(const Symbol *)>(library, "zbar_symbol_get_data");
    auto length = resolve<unsigned (*)(const Symbol *)>(library, "zbar_symbol_get_data_length");

    const QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull()) throw std::runtime_error("内存不足，无法转换条码图片。");
    QByteArray packed;
    const uchar *data = gray.constBits();
    if (gray.bytesPerLine() != gray.width()) {
        packed.resize(gray.width() * gray.height());
        for (int row = 0; row < gray.height(); ++row)
            std::memcpy(packed.data() + row * gray.width(), gray.constScanLine(row), size_t(gray.width()));
        data = reinterpret_cast<const uchar *>(packed.constData());
    }
    std::unique_ptr<Scanner, decltype(destroyScanner)> scanner(createScanner(), destroyScanner);
    std::unique_ptr<Image, decltype(destroyImage)> input(createImage(), destroyImage);
    if (!scanner || !input) throw std::runtime_error("内存不足，无法启动条码识别。");
    constexpr unsigned long y800 = 'Y' | ('8' << 8) | ('0' << 16) | ('0' << 24);
    setFormat(input.get(), y800);
    setSize(input.get(), unsigned(gray.width()), unsigned(gray.height()));
    setData(input.get(), data, static_cast<unsigned long>(gray.width()) * gray.height(), nullptr);
    if (scan(scanner.get(), input.get()) < 0) throw std::runtime_error("条码识别失败，请使用清晰的正面图像。");
    QStringList lines;
    for (const Symbol *code = first(input.get()); code; code = next(code)) {
        const QByteArray raw(value(code), int(length(code)));
        const QString text = QString::fromUtf8(raw);
        const QString printable = text.contains(QChar::ReplacementCharacter) || raw.contains('\0')
            ? QString::fromLatin1(raw.toHex(' ')) : text;
        lines << QString("%1  %2").arg(QString::fromLatin1(symbolName(symbolType(code))), printable);
    }
    lines.removeDuplicates();
    return lines;
}
}
