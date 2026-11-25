#include "imageprocessor.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <vector>
#include <QPolygonF>
#include <QElapsedTimer>
#include <QLinearGradient>
#include <QDir>
#include <QImageReader>
#include <QTransform>
#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using scan::ColorMode;
using scan::EditOptions;
using scan::ImageProcessor;

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<class Callable> void requireFailure(Callable operation, const char *message)
{
    bool failed = false;
    try { operation(); } catch (const std::exception &) { failed = true; }
    require(failed, message);
}

void testCornerValidation()
{
    const QVector<QPointF> full = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    require(ImageProcessor::validCorners(full), "full image corners should be valid");
    require(!ImageProcessor::validCorners({{0, 0}, {1, 1}, {1, 0}, {0, 1}}),
            "bow-tie crop must be rejected");
    require(!ImageProcessor::validCorners({{0, 0}, {1, 0}, {0.2, 0.2}, {0, 1}}),
            "concave crop must be rejected");
    require(!ImageProcessor::validCorners({{0, 0}, {1, 0}, {1, 0}, {0, 1}}),
            "duplicate corner must be rejected");
    require(!ImageProcessor::validCorners({{0, 0}, {1.1, 0}, {1, 1}, {0, 1}}),
            "out-of-image corner must be rejected");
    require(!ImageProcessor::validCorners({{0, 0}, {1, 0},
            {std::numeric_limits<double>::quiet_NaN(), 1}, {0, 1}}),
            "NaN crop must be rejected");
    require(!ImageProcessor::validCorners({{0, 0}, {0.001, 0}, {0.001, 0.001}, {0, 0.001}}),
            "degenerate crop must be rejected");
}

void testDetectionAndPerspective()
{
    QImage image(1000, 800, QImage::Format_RGB888);
    image.fill(QColor(35, 45, 55));
    const QPolygonF paper({QPointF(140, 100), QPointF(840, 155),
                           QPointF(800, 700), QPointF(100, 650)});
    {
        QPainter painter(&image);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawPolygon(paper);
        painter.setBrush(Qt::black);
        for (int row = 0; row < 8; ++row)
            painter.drawRect(220, 240 + row * 35, 460, 7);
    }
    const auto corners = ImageProcessor::detectCorners(image);
    require(corners.size() == 4, "slanted white document should be detected");
    for (int i = 0; i < 4; ++i) {
        const QPointF actual(corners[i].x() * 999, corners[i].y() * 799);
        const QPointF delta = actual - paper[i];
        require(std::hypot(delta.x(), delta.y()) < 16.0,
                "detected document edge should match visible paper");
    }
    const QImage processed = ImageProcessor::process(image, EditOptions{});
    require(processed.width() > 680 && processed.width() < 730,
            "perspective correction should retain native paper width");
    require(processed.height() > 530 && processed.height() < 580,
            "perspective correction should retain native paper height");
    require(processed.pixelColor(20, 20).lightness() > 230,
            "corrected paper margin should exclude the desk");

    QImage blank(128, 96, QImage::Format_RGB888);
    blank.fill(Qt::white);
    require(ImageProcessor::detectCorners(blank).isEmpty(),
            "uniform image should not invent a document outline");
    require(ImageProcessor::process(blank, EditOptions{}).size() == blank.size(),
            "failed auto detection should preserve the whole image");
}

void testSingleCornerOutsideFrame()
{
    QImage frame(960, 680, QImage::Format_RGB32);
    frame.fill(QColor(108, 81, 55));
    QPolygonF paper({{290,130}, {640,212}, {525,702}, {175,620}});
    {
        QPainter painter(&frame);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(229, 230, 225));
        painter.drawPolygon(paper);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(45, 45, 45), 3));
        painter.drawRect(QRect(310, 270, 170, 230));
        for (int y = 290; y < 500; y += 20)
            painter.drawLine(310, y, 480, y);
        painter.fillRect(QRect(383, 391, 50, 50), QColor(184, 50, 42));
    }

    // Rotate the camera and independent ground truth together. The missing
    // region touches each of the four sensor borders in turn.
    for (int turn = 0; turn < 4; ++turn) {
        const auto found = ImageProcessor::detectCorners(frame);
        require(found.size() == 4, "one clipped corner should still return four paper corners");
        int outside = -1;
        for (int i = 0; i < found.size(); ++i) {
            const QPointF p(found[i].x() * (frame.width()-1), found[i].y() * (frame.height()-1));
            double nearest = std::numeric_limits<double>::max();
            for (const QPointF &actual : paper)
                nearest = std::min(nearest, std::hypot(p.x()-actual.x(), p.y()-actual.y()));
            require(nearest < 12.0, "inferred quad must follow the sheet rather than the printed table");
            if (!QRectF(0, 0, 1, 1).contains(found[i])) {
                require(outside == -1, "only one paper corner is outside this scene");
                outside = i;
            }
        }
        require(outside >= 0, "the clipped corner must be extrapolated past the frame");
        EditOptions edits;
        edits.corners = found;
        const auto cropped = ImageProcessor::process(frame, edits);
        const double aspect = double(std::max(cropped.width(), cropped.height())) /
                              std::min(cropped.width(), cropped.height());
        require(std::abs(aspect - 297.0/210.0) < .006, "the restored page must have A4 proportions");
        const std::array<QPoint,4> tips{{{3,3}, {cropped.width()-4,3},
                            {cropped.width()-4,cropped.height()-4}, {3,cropped.height()-4}}};
        require(cropped.pixelColor(tips[outside]) == QColor(Qt::white),
                "unseen pixels must be white on the output page");
        const auto middle = cropped.pixelColor(cropped.width()/2, cropped.height()/2);
        require(middle.red() > middle.green()+65, "visible color marks must survive the partial crop");

        for (auto &p : paper)
            p = QPointF(frame.height()-1-p.y(), p.x());
        frame = frame.transformed(QTransform().rotate(90));
    }
}

void testCutPaperCorner()
{
    // The entire sheet is in the frame. One physical corner is cut off,
    // exposing a triangle of the desk next to printed material on the page.
    QImage frame(960, 720, QImage::Format_RGB32);
    frame.fill(QColor(107, 68, 46));
    {
        QPainter painter(&frame);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(QColor(228, 227, 225));
        painter.setPen(Qt::NoPen);
        painter.drawPolygon(QPolygonF({{275,65},{612,65},{695,181},{695,659},{275,659}}));
        painter.setPen(QPen(QColor(49, 51, 54), 3));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRect(360, 170, 256, 410));
        for (int y = 200; y < 580; y += 30)
            painter.drawLine(360, y, 616, y);
        painter.fillRect(QRect(645, 172, 14, 18), QColor(174, 37, 42));
    }
    QPointF missing(.975, .025);
    QPointF mark((652.-275)/420, (181.-65)/594);
    QImage printed = frame.copy();
    {
        QPainter painter(&printed);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(24, 24, 24));
        painter.drawPolygon(QPolygonF({{612,65}, {695,65}, {695,181}}));
    }
    EditOptions intact;
    intact.corners = ImageProcessor::detectCorners(printed, &intact.missingTriangle);
    require(intact.missingTriangle.isEmpty(),
            "a dark logo printed to the edges of intact paper must not be erased as a tear");
    QImage held = frame.copy();
    {
        QPainter painter(&held);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(226, 217, 194), 28));
        painter.drawLine(585, 718, 780, 445);
        painter.setBrush(QColor(24, 25, 26));
        painter.setPen(QPen(QColor(165, 161, 154), 3));
        painter.drawEllipse(QPoint(688, 574), 31, 31);
    }
    EditOptions heldEdits;
    heldEdits.corners = ImageProcessor::detectCorners(held, &heldEdits.missingTriangle);
    require(heldEdits.corners.size() == 4 && heldEdits.missingTriangle.size() == 3,
            "a band crossing the opposite edges must not hide a clearly visible diagonal paper cut");
    const auto heldPage = ImageProcessor::process(held, heldEdits);
    require(heldPage.pixelColor(qRound(.976*(heldPage.width()-1)),
                                qRound(.856*(heldPage.height()-1))).lightness() < 65,
            "an object on the visible page must be preserved outside the repaired corner");
    const auto rotatePoint = [](const QPointF &p) { return QPointF(1-p.y(), p.x()); };
    for (int i = 0; i < 4; ++i) {
        EditOptions edits;
        edits.corners = ImageProcessor::detectCorners(frame, &edits.missingTriangle);
        require(edits.corners.size() == 4 && edits.missingTriangle.size() == 3,
                "a physical corner cut must recover the page rectangle and the triangular desk mask");
        require(ImageProcessor::validCorners(edits.corners),
                "the restored corner of a torn page still lies inside the photograph");
        const QImage result = ImageProcessor::process(frame, edits);
        const auto colorAt = [&result](QPointF p) {
            return result.pixelColor(qRound(p.x() * (result.width()-1)), qRound(p.y() * (result.height()-1)));
        };
        require(colorAt(missing) == QColor(Qt::white), "desk inside the missing corner must become white");
        const auto ink = colorAt(mark);
        require(ink.red() > ink.green()+80, "printed marks beside the missing corner must survive");
        require(std::abs(double(std::max(result.height(), result.width())) /
                         std::min(result.height(), result.width()) - 297.0/210.0) < .006,
                "physical corner repair must preserve the A4 page ratio");
        missing = rotatePoint(missing);
        mark = rotatePoint(mark);
        frame = frame.transformed(QTransform().rotate(90));
    }
}

struct CropScene {
    const char *name;
    QImage frame;
    QPolygonF boundary;   // independently specified source-image corners
};

std::vector<CropScene> cropScenes()
{
    std::vector<CropScene> scenes;
    const QPolygonF upright({{290,42},{704,73},{667,642},{248,610}});
    const QPolygonF landscape({{118,122},{828,91},{851,557},{143,596}});
    const QPolygonF skewed({{125,172},{718,55},{849,507},{287,625}});
    for(int scene=0;scene<9;++scene){
        const auto boundary=scene==1?landscape:(scene==2?skewed:upright);
        QImage frame(960,680,QImage::Format_RGB32);
        frame.fill(scene==3?QColor(204,202,199):QColor(144,103,69));
        QPainter paint(&frame);
        paint.setRenderHint(QPainter::Antialiasing);
        if(scene!=3){
            // Deterministic desk grain and pen marks outside the sheet.
            for(int j=0;j<120;++j){
                paint.setPen(QColor(114+j%35,76+j%28,50+j%29));
                int y=(j*37)%680;
                paint.drawLine((j*97)%980-100,y,(j*97)%980+220,y+j%23);
            }
        }
        if(scene==4){
            // A larger rectangular black scanner mat should not win.
            paint.fillRect(QRect(35,13,886,650),QColor(26,30,33));
        }
        if(scene==7){
            paint.setPen(QPen(Qt::black,4));
            paint.setBrush(Qt::NoBrush);
            paint.drawRect(QRect(170,110,596,420));
        }else{
            auto sheet=boundary;
            if(scene==6) sheet=QPolygonF({{-20,30},{572,80},{539,638},{-58,610}});
            if(scene==8) sheet=QPolygonF({{244,129},{684,147},{661,790},{216,768}});
            QLinearGradient gradient(0,0,960,680);
            gradient.setColorAt(0,scene==2?QColor(108,109,101):QColor(235,235,233));
            gradient.setColorAt(1,scene==3?QColor(220,220,216):QColor(250,248,240));
            paint.setPen(Qt::NoPen);
            paint.setBrush(gradient);
            paint.drawPolygon(sheet);
            paint.save();
            QPainterPath paperPath;
            paperPath.addPolygon(sheet);
            paint.setClipPath(paperPath);
            paint.setPen(QPen(QColor(55,53,50),3));
            paint.setBrush(Qt::NoBrush);
            paint.drawRect(QRectF(320,182,226,290));
            for(int row=0;row<14;++row) paint.drawLine(325,194+18*row,537,194+18*row);
            paint.restore();
            if(scene==5){
                // Finger occludes the top-right corner; the three long
                // remaining edge segments still establish its intersection.
                paint.setPen(Qt::NoPen);
                paint.setBrush(QColor(191,140,111));
                paint.drawEllipse(QPointF(704,73),38,34);
            }
        }
        paint.end();
        const std::array<const char*,9> names={"portrait","landscape","shadow-perspective",
             "pale-desk","dark-mat","occlusion","clipped-page","desk-only","bottom-clipped"};
        scenes.push_back({names[scene],frame,scene>=6?QPolygonF():boundary});
    }
    return scenes;
}

bool evaluateCrops(bool enforce)
{
    bool good=true;
    for (const auto &scene:cropScenes()) {
        QElapsedTimer timer;timer.start();
        const auto detected=ImageProcessor::detectCorners(scene.frame);
        const double ms=timer.nsecsElapsed()/1e6;
        double worst=0;
        bool ok=detected.size()==scene.boundary.size();
        if(ok) for(int i=0;i<detected.size();++i){
            QPointF xy(detected[i].x()*(scene.frame.width()-1),detected[i].y()*(scene.frame.height()-1));
            const auto delta=xy-scene.boundary[i];
            worst=std::max(worst,std::hypot(delta.x(),delta.y()));
        }
        ok=ok && worst<18;
        std::cout<<"A4_CASE "<<scene.name<<" quad="<<detected.size()
                 <<" max_corner_error_px="<<(detected.size()==scene.boundary.size()?worst:-1)
                 <<" elapsed_ms="<<ms<<" pass="<<ok<<'\n';
        good &= ok;
    }
    if(enforce) require(good,"A4 software fixtures failed");
    return good;
}

void testCropRotationAndColor()
{
    QImage image(201, 101, QImage::Format_RGB888);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        painter.fillRect(0, 0, 100, 50, Qt::red);
        painter.fillRect(100, 0, 101, 50, Qt::green);
        painter.fillRect(0, 50, 100, 51, Qt::blue);
        painter.fillRect(100, 50, 101, 51, Qt::yellow);
    }
    EditOptions options;
    options.autoCrop = false;
    options.corners = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    options.rotation = 90;
    const QImage rotated = ImageProcessor::process(image, options);
    require(rotated.size() == QSize(101, 201), "90 degree rotation must swap native dimensions");
    require(rotated.pixelColor(10, 10) == QColor(Qt::blue),
            "clockwise rotation must move original bottom-left to top-left");
    require(rotated.pixelColor(90, 10) == QColor(Qt::red),
            "clockwise rotation must move original top-left to top-right");
    options.rotation = 0;
    options.corners = {{0.25, 0.2}, {0.75, 0.2}, {0.75, 0.8}, {0.25, 0.8}};
    require(ImageProcessor::process(image, options).size() == QSize(101, 61),
            "manual crop dimensions should be based on source samples");
    options.corners.clear();
    options.color = ColorMode::Gray;
    const QImage gray = ImageProcessor::process(image, options);
    const QColor pixel = gray.pixelColor(10, 10);
    require(pixel.red() == pixel.green() && pixel.green() == pixel.blue(),
            "grayscale must have equal color components");
    require(gray.pixelColor(150, 10).red() > gray.pixelColor(10, 10).red(),
            "grayscale luminance must distinguish green and red correctly");
    options.color = ColorMode::Color;
    options.rotation = 45;
    requireFailure([&] { ImageProcessor::process(image, options); },
                   "unsupported rotation must not silently be ignored");
    options.rotation = 0;
    options.corners = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
    requireFailure([&] { ImageProcessor::process(image, options); },
                   "bad manual crop must not silently destroy an image");
    requireFailure([] { ImageProcessor::process(QImage(), EditOptions{}); },
                   "empty source must report failure");
}

void testBlackWhiteAndOwnership()
{
    QImage result;
    {
        QImage source(240, 120, QImage::Format_RGB888);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x) {
                const int value = 170 + x / 4;
                source.setPixelColor(x, y, QColor(value, value, value));
            }
        QPainter painter(&source);
        painter.fillRect(40, 25, 8, 70, Qt::black);
        painter.fillRect(40, 25, 90, 8, Qt::black);
        painter.end();
        EditOptions options;
        options.autoCrop = false;
        options.color = ColorMode::BlackWhite;
        result = ImageProcessor::process(source, options);
        source.fill(Qt::red);
    }
    require(result.pixelColor(44, 60).red() == 0, "document strokes should remain black");
    require(result.pixelColor(200, 100).red() == 255, "uneven paper background should become white");
    for (int y = 0; y < result.height(); ++y)
        for (int x = 0; x < result.width(); ++x) {
            const int value = result.pixelColor(x, y).red();
            require(value == 0 || value == 255, "black-white output must contain only binary values");
        }
    QImage tiny(1, 1, QImage::Format_RGB888);
    tiny.fill(Qt::black);
    EditOptions options;
    options.autoCrop = false;
    options.color = ColorMode::BlackWhite;
    require(!ImageProcessor::process(tiny, options).isNull(),
            "tiny valid images should not crash adaptive thresholding");
}

void testIdComposition()
{
    QImage front(400, 250, QImage::Format_RGB888);
    front.fill(Qt::red);
    QImage back(300, 200, QImage::Format_RGB888);
    back.fill(Qt::blue);
    const QImage sheet = ImageProcessor::composeIdCard(front, back);
    require(sheet.size() == QSize(2480, 3508), "ID composition should use 300 dpi A4 dimensions");
    require(sheet.pixelColor(1240, 1000) == QColor(Qt::red), "front should occupy upper box");
    require(sheet.pixelColor(1240, 2500) == QColor(Qt::blue), "back should occupy lower box");
    require(sheet.pixelColor(1240, 1754) == QColor(Qt::white), "ID sides should have a white gutter");
    require(sheet.pixelColor(20, 20) == QColor(Qt::white), "A4 composition should retain white margins");
    requireFailure([&] { ImageProcessor::composeIdCard(front, QImage()); },
                   "missing ID side must report failure");
}
} // namespace

int main(int argc,char **argv)
{
    try {
        if(argc>1 && std::string(argv[1])=="--crop-benchmark"){
            evaluateCrops(false);
            for(int i=2;i<argc;++i){
                QImageReader reader(QString::fromLocal8Bit(argv[i]));
                reader.setAutoTransform(true);
                QImage im=reader.read();
                if(im.isNull()) throw std::runtime_error("Cannot read benchmark image");
                auto corners=ImageProcessor::detectCorners(im);
                std::cout<<"PHOTO "<<argv[i]<<" ";
                for(auto p:corners) std::cout<<p.x()<<","<<p.y()<<" ";
                std::cout<<'\n';
            }
            return 0;
        }
        evaluateCrops(true);
        testCornerValidation();
        testDetectionAndPerspective();
        testSingleCornerOutsideFrame();
        testCutPaperCorner();
        testCropRotationAndColor();
        testBlackWhiteAndOwnership();
        testIdComposition();
        std::cout << "All image processing tests passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Image processing test failure: " << error.what() << '\n';
        return 1;
    }
}
