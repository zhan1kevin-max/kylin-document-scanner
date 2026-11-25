#include "mainwindow.h"
#include "imageprocessor.h"
#include "exporter.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QLocale>
#include <QPainter>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>
#include <QTranslator>
#include <iostream>

using namespace scan;
static void print(const QString &s){QTextStream out(stdout);out.setCodec("UTF-8");out<<s<<'\n';out.flush();}
static QImage samplePage(int n){
    QImage image(1480,2100,QImage::Format_RGB32);image.fill(Qt::white);QPainter p(&image);p.setPen(QColor("#183746"));
    QFont f("Noto Sans CJK SC",44);f.setBold(true);p.setFont(f);p.drawText(QRect(120,130,1240,120),Qt::AlignLeft|Qt::AlignVCenter,"离线文档扫描测试");
    f.setPointSize(24);f.setBold(false);p.setFont(f);p.drawText(120,350,"Document Scanner 15165");p.drawText(120,420,"Kylin ARM64 - Offline OCR");
    p.setPen(QColor("#087f6d"));p.drawLine(120,490,1360,490);p.setPen(Qt::black);
    const QStringList lines={"文件编号：2026-001", "这是程序生成的测试样张，不包含个人资料。", "支持拍照、自动裁切、旋转和黑白增强。", "所有处理在本机完成，支持导出多页 PDF。", "中文识别结果可以搜索、选择和复制。", "Please verify the text against the original image.", "Page order and document images are preserved.", "1234567890  ABCDEFGHIJKLMNOPQRSTUVWXYZ"};
    int y=610;for(auto line:lines){p.drawText(120,y,line);y+=100;}p.drawText(120,1930,QString("第 %1 页 / Page %1").arg(n));p.end();return image;
}
static int cameraTest(QApplication &app,const QString &path,int count){
    auto devices=Camera::devices();if(devices.isEmpty()){print("ERROR: 未发现视频采集设备。");return 2;}
    auto formats=Camera::formats(devices.first().id);if(formats.isEmpty()){print("ERROR: 设备没有支持的图像格式。");return 2;}
    if(!QDir().mkpath(path)){print("ERROR: 无法创建测试输出目录。");return 2;}
    Session task;
    try {task.create(QDir(path).filePath("capture-session"));} catch(const std::exception &e){print(QString::fromUtf8(e.what()));return 2;}
    QElapsedTimer shutter;
    Camera camera;int captures=0,frames=0;bool pending=false;QJsonArray records;
    QObject::connect(&camera,&Camera::opened,&app,[](const QString&s){print(s);});
    QObject::connect(&camera,&Camera::preview,&app,[&](const QImage&){if(++frames>=3&&!pending&&captures<count){pending=true;shutter.restart();camera.capture();}});
    QObject::connect(&camera,&Camera::frameCaptured,&app,[&](const CapturedFrame &frame){
        try{
            const auto acquired=shutter.elapsed();
            EditOptions opts;opts.corners=ImageProcessor::detectCorners(frame.preview(800),&opts.missingTriangle);
            QImage processed;
            if(!opts.corners.isEmpty())processed=ImageProcessor::process(frame.decode(),opts);
            auto page=task.addCapture(frame,processed,opts);
            records.append(QJsonObject{{"file",page.processedFile},{"width",frame.size.width()},
                {"height",frame.size.height()},{"acquireMs",acquired},{"commitMs",shutter.elapsed()},
                {"cropDetected",!opts.corners.isEmpty()},{"missingPaperCorner",!opts.missingTriangle.isEmpty()},
                {"cornerOutsideFrame",hasOffscreenCorner(opts.corners)}});
            ++captures;
            print(QString("CAPTURE %1: %2 x %3 · %4 ms").arg(captures).arg(frame.size.width()).arg(frame.size.height()).arg(shutter.elapsed()));
            frames=0;pending=false;
            if(captures>=count){camera.stop();QSaveFile report(QDir(path).filePath("camera-test.json"));auto data=QJsonDocument(QJsonObject{{"device",devices.first().id},{"format",formats.first().description},{"captures",records}}).toJson();if(!report.open(QIODevice::WriteOnly)||report.write(data)!=data.size()||!report.commit())throw std::runtime_error("Cannot save camera report");app.exit(0);}
        }catch(const std::exception&e){print(QString("ERROR: ")+QString::fromUtf8(e.what()));camera.stop();app.exit(2);}
    });
    QObject::connect(&camera,&Camera::error,&app,[&](const QString&s){print("ERROR: "+s);camera.stop();app.exit(2);});
    QTimer::singleShot(std::max(120000,count*30000),&app,[&]{print("ERROR: camera test timed out");camera.stop();app.exit(2);});
    camera.start(devices.first().id,formats.first());return app.exec();
}
int main(int argc,char *argv[]){
    // QApplication consumes --session for X11 session management before our
    // parser runs. Preserve our existing long option and Qt's native -session.
    QVector<QByteArray> translatedArgs;
    translatedArgs.reserve(argc);
    const QByteArray oldOption("--session");
    for(int i=1;i<argc;++i) {
        QByteArray argument(argv[i]);
        if(argument=="--") break;
        if(argument==oldOption || argument.startsWith("--session=")) {
            translatedArgs.append(QByteArray("--scan-task")+argument.mid(oldOption.size()));
            argv[i]=translatedArgs.last().data();
        }
    }
    for(int i=1;i<argc;++i)if(QString::fromLocal8Bit(argv[i])=="--smoke-test"||QString::fromLocal8Bit(argv[i])=="--camera-test"||QString::fromLocal8Bit(argv[i])=="--probe")qputenv("QT_QPA_PLATFORM","offscreen");
    QApplication app(argc,argv);QCoreApplication::setApplicationName("kylin-document-scanner");QCoreApplication::setApplicationVersion(SCANNER_VERSION);QCoreApplication::setOrganizationName("DocumentTools");QLocale::setDefault(QLocale(QLocale::Chinese,QLocale::China));
    QTranslator translator;translator.load("qt_zh_CN",QLibraryInfo::location(QLibraryInfo::TranslationsPath));app.installTranslator(&translator);
    QTranslator baseTranslator;baseTranslator.load("qtbase_zh_CN",QLibraryInfo::location(QLibraryInfo::TranslationsPath));app.installTranslator(&baseTranslator);
    QCommandLineParser parser;parser.setApplicationDescription("银河麒麟离线文档扫描与中文OCR");parser.addHelpOption();parser.addVersionOption();
    parser.addOption({"connect-camera","打开已有任务后连接高拍仪并恢复取景"});
    parser.addOption({QStringList{"scan-task","session"},"打开已有任务目录或session.json","path"});parser.addOption({"smoke-test","生成样张、界面截图和PDF/OCR自检输出","directory"});parser.addOption({"camera-test","真实摄像头测试，保存实际高清帧","directory"});parser.addOption({"count","摄像头测试采集张数（1至30）","number","3"});parser.addOption({"probe","输出摄像头与OCR能力JSON"});parser.process(app);
    if(parser.isSet("probe")){QJsonArray ds;for(auto d:Camera::devices()){QJsonArray fs;for(auto f:Camera::formats(d.id))fs.append(QJsonObject{{"width",f.width},{"height",f.height},{"description",f.description}});ds.append(QJsonObject{{"id",d.id},{"name",d.name},{"formats",fs}});}print(QString::fromUtf8(QJsonDocument(QJsonObject{{"devices",ds},{"tesseract",Exporter::tesseractExecutable()}}).toJson()));return 0;}
    if(parser.isSet("camera-test")){bool ok=false;int count=parser.value("count").toInt(&ok);if(!ok||count<1||count>30){print("--count 必须为1至30。");return 2;}return cameraTest(app,parser.value("camera-test"),count);}
    MainWindow window;
    if(parser.isSet("smoke-test")){
        const QString root=QDir(parser.value("smoke-test")).absolutePath();
        try{Session s;s.create(QDir(root).filePath("sample-session"));EditOptions o;o.autoCrop=false;for(int i=1;i<=2;++i){auto im=samplePage(i);s.add(im,im,o);}window.openSession(s.directory);window.show();
            QTimer::singleShot(900,&app,[&app,&window,s,root]{try{
                if(!window.grab().save(QDir(root).filePath("interface.png")))throw std::runtime_error("Cannot save UI screenshot");
                std::atomic_bool cancel{false};ExportRequest req;for(auto p:s.pages)req.images<<s.absolute(p.processedFile);req.destination=QDir(root).filePath("sample.pdf");Exporter::run(req,cancel);req.kind=ExportKind::SearchablePdf;req.destination=QDir(root).filePath("sample-searchable.pdf");Exporter::run(req,cancel);req.kind=ExportKind::Text;req.destination=QDir(root).filePath("sample.txt");Exporter::run(req,cancel);print("SMOKE TEST PASSED: interface, PDF, searchable PDF, Chinese/English OCR");app.exit(0);
            }catch(const std::exception&e){print(QString("ERROR: ")+QString::fromUtf8(e.what()));app.exit(2);}});
        }catch(const std::exception&e){print(QString("ERROR: ")+QString::fromUtf8(e.what()));return 2;}
    }else{if(parser.isSet("scan-task"))window.openSession(parser.value("scan-task"));window.show();if(!parser.isSet("scan-task") || parser.isSet("connect-camera"))window.connectPreferredCamera();}
    return app.exec();
}
