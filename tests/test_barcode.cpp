#include "barcode.h"
#include <QCoreApplication>
#include <QPainter>
#include <iostream>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const char *qr[] = {"#######....#..#######",
        "#.....#..##...#.....#",
        "#.###.#.##.#..#.###.#",
        "#.###.#.#.#.#.#.###.#",
        "#.###.#.#.#.#.#.###.#",
        "#.....#.##.#..#.....#",
        "#######.#.#.#.#######",
        "........##...........",
        "#.#####...##..#####..",
        "###.##.##.######.##.#",
        ".###.##...#.#.##.#.#.",
        ".#..#..#...#####....#",
        "##.#.##.#...#..#.#..#",
        "........###.#..######",
        "#######....#.#.##.#.#",
        "#.....#.#.#....#..#..",
        "#.###.#.####.#.##.##.",
        "#.###.#.#######..#...",
        "#.###.#.###.#.###.#..",
        "#.....#..#.####.....#",
        "#######.##..#..#..#.."};
    try {
        // Non-multiple-of-four width exercises the grayscale row-stride handling.
        QImage sheet(347, 400, QImage::Format_RGB32);
        sheet.fill(Qt::white);
        if (!scan::scanBarcodes(sheet).isEmpty()) return 1;
        QPainter painter(&sheet);
        for (int y = 0; y < 21; ++y)
            for (int x = 0; x < 21; ++x)
                if (qr[y][x] == '#') painter.fillRect(64 + x * 8, 32 + y * 8, 8, 8, Qt::black);
        const QByteArray ean13("10100011010100111010111101111010001001011001101010100001010000101000010111010010000101100110101");
        for (int x = 0; x < ean13.size(); ++x)
            if (ean13[x] == '1') painter.fillRect(30 + 3 * x, 260, 3, 90, Qt::black);
        painter.end();
        const auto results = scan::scanBarcodes(sheet).join('\n');
        if (!results.contains("KYL-15165") || !results.contains("4006381333931")) {
            std::cerr << results.toStdString() << '\n';
            return 1;
        }
        std::cout << "EAN-13, QR and blank barcode tests passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
