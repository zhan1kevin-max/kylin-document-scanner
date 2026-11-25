#pragma once
#include <QImage>
#include <QStringList>

namespace scan {
// Scans pixels locally and returns one "symbol type: content" line per result.
QStringList scanBarcodes(const QImage &image);
}
