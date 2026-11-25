#include "mainwindow.h"
#include "cropcanvas.h"
#include "imageprocessor.h"
#include "scanstorage.h"
#include <QtConcurrent/QtConcurrent>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextEdit>
#include <QUrl>
#include <QUuid>
#include <QProcess>
#include <QDialog>
#include <algorithm>
#include <stdexcept>

namespace scan {
namespace {

QString errorText(const std::exception &e) {return QString::fromUtf8(e.what());}
void canceled(std::atomic_bool &flag) {if(flag.load())throw std::runtime_error("操作已取消，已保存的页面仍然保留。");}
QImage renderPage(const QImage &image,EditOptions &applied) {
    if(applied.autoCrop && applied.corners.isEmpty()) {
        applied.corners=ImageProcessor::detectCorners(image,&applied.missingTriangle);
    }
    auto render=applied;
    render.autoCrop=false;
    return ImageProcessor::process(image,render);
}
}
MainWindow::MainWindow(QWidget *parent):QMainWindow(parent),camera_(this) {
    createUi();
    installCaptureShortcut();
    outlineClock_.start();
    connect(&outlineJob_, &QFutureWatcher<EditOptions>::finished, this, [this]{
        if(submittedGeneration_ != outlineGeneration_ || !camera_.isRunning() || cropChoice_->currentIndex()!=0) return;
        if(outlineLatency_.elapsed()>850){live_->setCorners({});live_->setOverlayText("正在定位纸边");return;}
        const auto outline=outlineJob_.result();
        live_->setCorners(outline.corners,outline.missingTriangle);
        if (outline.corners.isEmpty()) live_->setOverlayText(tr("寻找纸边 · 请尽量露出三处纸角和纸边"));
        else if (!outline.missingTriangle.isEmpty()) live_->setOverlayText(tr("已识别缺角 · 黄色区域将补白"));
        else if (hasOffscreenCorner(outline.corners)) live_->setOverlayText(tr("已推算画外纸角 · 画外区域将补白"));
        else live_->setOverlayText(tr("已找到纸边"));
    });
    connect(&watcher_,&QFutureWatcher<QString>::finished,this,[this]{
        const auto error=watcher_.result();auto done=done_;done_={};setBusy(false);
        try {if(!error.isEmpty()){refreshPages();fail(error);}else if(done)done();}
        catch(const std::exception &e){updating_=false;fail(errorText(e));}
    });
    connect(&camera_,&Camera::preview,this,[this](const QImage &im){previewFrame(im);});
    connect(&camera_,&Camera::opened,this,[this](const QString &s){connect_->setText("断开设备");devices_->setEnabled(false);resolution_->setEnabled(false);message(s);});
    connect(&camera_,&Camera::frameCaptured,this,&MainWindow::receiveFrame);
    connect(&camera_,&Camera::error,this,[this](const QString &s){capturePending_=false;++outlineGeneration_;if(!busy_&&!cleaningStorage_)controls_->setEnabled(true);stopBatch();live_->setImage({});live_->setMessage(s);message(s);});
    connect(&camera_,&Camera::stopped,this,[this]{
        capturePending_=false;
        ++outlineGeneration_;
        if(cropChoice_->currentIndex()!=1)live_->setCorners({});
        live_->setOverlayText({});
        if(recording_)return;
        connect_->setText("连接设备");
        devices_->setEnabled(true);
        resolution_->setEnabled(true);
    });
    connect(&timer_,&QTimer::timeout,this,[this]{if(!busy_ && !capturePending_)capture();});
    refreshDevices();
}
MainWindow::~MainWindow(){closing_=true;if(recording_)toggleRecording();timer_.stop();cancel_.store(true);camera_.stop();watcher_.waitForFinished();outlineJob_.waitForFinished();}
void MainWindow::message(const QString &text){status_->setText(text);}
void MainWindow::fail(const QString &text){stopBatch();message(text);QMessageBox::warning(this,"操作提示",text);}
void MainWindow::setBusy(bool busy){busy_=busy;controls_->setEnabled(!busy);progress_->setVisible(busy);cancelButton_->setVisible(busy);if(busy){progress_->setRange(0,0);message("正在处理，请稍候……");}}
void MainWindow::runJob(std::function<void()> work,std::function<void()> done){
    if(busy_)return;
    cancel_.store(false);done_=std::move(done);setBusy(true);
    watcher_.setFuture(QtConcurrent::run([work]{try{work();return QString();}catch(const std::exception &e){return errorText(e);}catch(...){return QStringLiteral("操作失败，已保存的页面仍然保留。");}}));
}
void MainWindow::progress(int n,int total,const QString &text){QMetaObject::invokeMethod(this,[this,n,total,text]{progress_->setRange(0,std::max(1,total));progress_->setValue(n);message(text);},Qt::QueuedConnection);}
void MainWindow::refreshDevices(){if(busy_||recording_||camera_.isRunning())return;devices_->clear();for(auto d:Camera::devices())devices_->addItem(d.name,d.id);if(!devices_->count()){live_->setImage({});live_->setMessage("未发现高拍仪视频设备\n请接入 USB 后点击“刷新”\n也可导入图片处理文档");}refreshFormats();}
void MainWindow::refreshFormats(){resolution_->clear();formats_.clear();if(devices_->currentIndex()<0)return;formats_=Camera::formats(devices_->currentData().toString());for(const auto &f:formats_)resolution_->addItem(f.description);}
void MainWindow::connectPreferredCamera(){
    if(devices_->currentText().contains("2e5a:1719",Qt::CaseInsensitive) && resolution_->count()>0)toggleCamera();
}
void MainWindow::toggleCamera(){
    if(recording_)return;
    if(camera_.isRunning()){stopBatch();camera_.stop();live_->setImage({});message("设备已断开。");return;}
    int i=resolution_->currentIndex();if(devices_->currentIndex()<0||i<0||i>=formats_.size()){fail("没有可用的视频设备或图像格式，请检查 USB 连接并点击“设备检测”。");return;}
    camera_.start(devices_->currentData().toString(),formats_[i]);tabs_->setCurrentIndex(0);message("正在连接设备……");
}
void MainWindow::capture(){if(busy_||capturePending_||recording_||cleaningStorage_)return;if(!camera_.isRunning()){fail("请先连接高拍仪。");return;}if(!ensureSession())return;pendingOptions_={}; pendingOptions_.autoCrop=cropChoice_->currentIndex()==0;
    if(cropChoice_->currentIndex()==1) pendingOptions_.corners=live_->corners();
    pendingOptions_.color=ColorMode(color_->currentIndex());
    pendingOptions_.rotation=rotation_;
    if(cropChoice_->currentIndex()==1&&!ImageProcessor::validCorners(pendingOptions_.corners,true)){fail("请让至少三个纸角留在画内，并按顺序拖动四角，避免交叉。");return;}
    shutterClock_.restart();capturePending_=true;controls_->setEnabled(false);camera_.capture();message("正在采集高清画面，请保持纸张稳定……");}
bool MainWindow::ensureSession(){if(session_.active())return true;return ensureDirectory(exportDirectory());}
void MainWindow::newSession(){
    stopBatch();
    auto parent=QFileDialog::getExistingDirectory(this,"选择保存位置（将在此新建任务文件夹）",QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));if(parent.isEmpty())return;
    stopBatch();
    auto path=QDir(parent).filePath("扫描任务_"+QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")+"_"+QUuid::createUuid().toString(QUuid::Id128).left(4));
    try{session_.create(path);ScanStorage::registerTask(session_.directory);refreshPages();message("新任务已创建；每页拍摄后自动保存。");}catch(const std::exception&e){fail(errorText(e));}
}
void MainWindow::openSession(const QString &directory){if(busy_)return;stopBatch();try{session_.load(directory);ScanStorage::registerTask(session_.directory);refreshPages();message("扫描任务已打开。");}catch(const std::exception&e){fail(errorText(e));}}
void MainWindow::cleanCaptureStorage() {
    if (busy_ || capturePending_ || cleaningStorage_) return;
    if (recording_) { message(tr("请先停止录像，再清理拍摄缓存。")); return; }
    stopBatch();
    QScopedValueRollback<bool> cleaning(cleaningStorage_, true);
    controls_->setEnabled(false);
    try {
        if (ScanStorage::showDialog(this, exportDirectory(), session_)) {
            session_.load(session_.directory);
            thumbnailCache_.clear();
            refreshPages();
            if (session_.pages.isEmpty()) tabs_->setCurrentIndex(0);
        }
        browseResults();
    } catch (const std::exception &error) {
        fail(errorText(error));
    }
    controls_->setEnabled(true);
}
void MainWindow::importImages(){
    stopBatch();
    auto files=QFileDialog::getOpenFileNames(this,"导入图片",{},"图片 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff)");if(files.isEmpty()||!ensureSession())return;
    EditOptions o;o.autoCrop=cropChoice_->currentIndex()==0;o.color=ColorMode(color_->currentIndex());
    runJob([this,files,o]{
        int i=0;
        for(const auto &path:files) {
            canceled(cancel_);
            QImageReader reader(path);reader.setAutoTransform(true);
            const auto image=reader.read();
            if(image.isNull())throw std::runtime_error(QString("无法读取图片 %1：%2").arg(path,reader.errorString()).toUtf8().constData());
            auto applied=o;
            const auto processed=renderPage(image,applied);
            canceled(cancel_);
            session_.add(image,processed,applied);
            ++i;progress(i,files.size(),QString("已导入 %1 / %2 页").arg(i).arg(files.size()));
        }
    },[this]{refreshPages(session_.pages.isEmpty()?QString():session_.pages.last().id);tabs_->setCurrentIndex(1);message("图片已导入并保存。");});
}
int MainWindow::currentIndex() const {return pages_->currentRow();}
QVector<int> MainWindow::selectedIndices() const {QVector<int> result;for(int i=0;i<pages_->count();++i)if(pages_->item(i)->isSelected())result.append(i);return result;}
void MainWindow::refreshPages(const QString &focusId){
    auto oldId=focusId;int old=currentIndex();if(oldId.isEmpty()&&old>=0&&old<pages_->count())oldId=pages_->item(old)->data(Qt::UserRole).toString();
    updating_=true;pages_->clear();int focus=-1;
    for(int i=0;i<session_.pages.size();++i){
        const auto &p=session_.pages[i];const auto key=session_.absolute(p.processedFile);
        if(!thumbnailCache_.contains(key)){
            QImageReader reader(key);auto size=reader.size();size.scale(100,125,Qt::KeepAspectRatio);reader.setScaledSize(size);
            thumbnailCache_.insert(key,new QIcon(QPixmap::fromImage(reader.read())));
        }
        auto *item=new QListWidgetItem(*thumbnailCache_.object(key),QString("第 %1 页").arg(i+1));
        item->setData(Qt::UserRole,p.id);pages_->addItem(item);if(p.id==oldId)focus=i;
    }
    if(focus<0&&!session_.pages.isEmpty())focus=std::clamp(old,0,session_.pages.size()-1);
    pages_->setCurrentRow(focus);updating_=false;pageCount_->setText(QString("页面 · %1").arg(session_.pages.size()));
    browseResults();showCurrent();
}
void MainWindow::showCurrent() try {
    int i=currentIndex();updating_=true;page_->setEditing(false);
    if(i<0||i>=session_.pages.size()){page_->setImage({});ocr_->clear();rotation_=0;rotationLabel_->setText("旋转：0°");updating_=false;return;}
    const auto &p=session_.pages[i];color_->setCurrentIndex(int(p.edits.color));rotation_=p.edits.rotation;rotationLabel_->setText(QString("旋转：%1°").arg(rotation_));QImageReader r(session_.absolute(p.processedFile));auto size=r.size();if(size.isValid())r.setScaledSize(size.scaled(1600,1600,Qt::KeepAspectRatio));page_->setImage(r.read());page_->setCorners({});ocr_->setPlainText(p.ocrText);tabs_->setCurrentIndex(1);updating_=false;
}
catch(const std::exception &e){updating_=false;page_->setImage({});ocr_->clear();message(errorText(e));}
EditOptions MainWindow::currentEdits() const {
    EditOptions o;int i=currentIndex();if(i>=0&&i<session_.pages.size())o=session_.pages[i].edits;
    o.autoCrop=cropChoice_->currentIndex()==0;o.color=ColorMode(color_->currentIndex());o.rotation=rotation_;
    if(cropChoice_->currentIndex()==2){o.autoCrop=false;o.corners.clear();}
    if(o.autoCrop && (i<0 || session_.pages.at(i).edits.autoCrop))o.corners.clear();
    if(o.corners.isEmpty())o.missingTriangle.clear();
    return o;
}
void MainWindow::applyEdits(){
    const int i=currentIndex();if(i<0)return;
    auto p=session_.pages[i];auto options=currentEdits();
    runJob([this,p,options]() mutable {
        auto image=QImage(session_.absolute(p.originalFile));
        auto out=renderPage(image,options);
        canceled(cancel_);
        session_.update(p.id,out,options);
    },[this]{refreshPages();message("处理已保存。图像已改变，需要时可重新识别文字。");});
}
void MainWindow::resetEdits(){int i=currentIndex();if(i<0)return;auto p=session_.pages[i];EditOptions o;o.autoCrop=false;runJob([this,p,o]{auto image=QImage(session_.absolute(p.originalFile));canceled(cancel_);session_.update(p.id,image,o);},[this]{refreshPages();message("已恢复原始画面。");});}
void MainWindow::removePages(){stopBatch();auto ids=selectedIndices();if(ids.isEmpty())return;if(QMessageBox::question(this,"移除页面",QString("从当前任务移除所选 %1 页？原始图片仍保留在任务文件夹。").arg(ids.size()),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;QVector<QString> keys;for(auto i:ids)keys.append(session_.pages[i].id);try{session_.remove(keys);refreshPages();message("页面已从任务移除。");}catch(const std::exception&e){fail(errorText(e));}}
void MainWindow::movePage(int delta){int i=currentIndex();if(i<0||i+delta<0||i+delta>=session_.pages.size())return;auto id=session_.pages[i].id;try{session_.move(i,i+delta);refreshPages(id);}catch(const std::exception&e){fail(errorText(e));}}
void MainWindow::recognizePage(){int i=currentIndex();if(i<0){fail("请先选择需要识别的页面。");return;}auto p=session_.pages[i];runJob([this,p]{auto text=Exporter::recognize(session_.absolute(p.processedFile),cancel_);canceled(cancel_);session_.setOcr(p.id,text);},[this]{refreshPages();showOcr();message("识别已完成，右侧文字可选择、复制。请对照图片核对关键内容。");});}
void MainWindow::exportPages() try {
    stopBatch();
    if(session_.pages.isEmpty()){fail("请先拍摄或导入图片。");return;}auto selected=selectedIndices();if(selected.isEmpty())for(int i=0;i<session_.pages.size();++i)selected.append(i);
    ExportRequest req;req.kind=ExportKind(exportKind_->currentIndex());req.a4=a4_->isChecked();for(int i:selected)req.images.append(session_.absolute(session_.pages[i].processedFile));
    QString base=prefix_->text().trimmed()+"_"+QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    if(req.kind==ExportKind::Jpeg||req.kind==ExportKind::Png){auto dir=QFileDialog::getExistingDirectory(this,"选择导出位置（将创建新的图片文件夹）",exportDirectory());if(dir.isEmpty())return;req.destination=QDir(dir).filePath(base+"_"+QUuid::createUuid().toString(QUuid::Id128).left(4));}
    else{auto ext=req.kind==ExportKind::Text?"txt":"pdf";req.destination=QFileDialog::getSaveFileName(this,QString("导出 %1 页").arg(selected.size()),QDir(exportDirectory()).filePath(base+"."+ext),req.kind==ExportKind::Text?"文本文档 (*.txt)":"PDF 文档 (*.pdf)");if(req.destination.isEmpty())return;if(!req.destination.endsWith("."+QString(ext),Qt::CaseInsensitive))req.destination+="."+QString(ext);}
    const QFileInfo destination(req.destination);
    QString resolvedDestination=destination.exists()?destination.canonicalFilePath():QDir(QFileInfo(destination.absolutePath()).canonicalFilePath()).filePath(destination.fileName());
    const auto taskRoot=QFileInfo(session_.directory).canonicalFilePath();
    if(resolvedDestination==taskRoot+"/session.json" || resolvedDestination.startsWith(taskRoot+"/originals/") || resolvedDestination.startsWith(taskRoot+"/processed/")){
        fail("请选择任务根目录或其他导出位置，原图和处理图文件夹用于保存扫描任务。");return;
    }
    stopBatch();
    runJob([this,req]{Exporter::run(req,cancel_,[this](int n,int total,const QString&t){progress(n,total,t);});},[this,req]{outputDir_=QFileInfo(req.destination).absolutePath();browseResults();message("已导出："+QDir::toNativeSeparators(req.destination));QMessageBox::information(this,"导出完成","文件已保存到：\n"+QDir::toNativeSeparators(req.destination));});
}
catch(const std::exception &e){fail(errorText(e));}
void MainWindow::composeIdentity(){auto indices=selectedIndices();if(indices.size()!=2){fail("请按住 Ctrl 选择正面和反面两页，再点击合并；顺序按左侧页面排列。");return;}auto first=session_.pages[indices[0]],second=session_.pages[indices[1]];runJob([this,first,second]{auto result=ImageProcessor::composeIdCard(QImage(session_.absolute(first.processedFile)),QImage(session_.absolute(second.processedFile)));canceled(cancel_);EditOptions o;o.autoCrop=false;session_.add(result,result,o);},[this]{refreshPages(session_.pages.last().id);message("正反面已合并为新的 A4 页面，原来的两页仍保留。");});}
void MainWindow::detectDeviceReport(){
    QString report="文档高拍工具 · 设备检测\n"+QDateTime::currentDateTime().toString(Qt::ISODate)+"\n\n";
    report+="应用版本："+QCoreApplication::applicationVersion()+"\nQt版本："+QString(qVersion())+"\n";
    for(auto d:Camera::devices()){report+="\n设备："+d.name+" ["+d.id+"]\n";for(auto f:Camera::formats(d.id))report+="  "+f.description+"\n";}
    if(Camera::devices().isEmpty())report+="\n未发现可用的视频采集节点。请检查USB连接；Linux下可运行随包附带的probe-device.sh。\n";
    auto tess=Exporter::tesseractExecutable();report+="\nOCR程序："+(tess.isEmpty()?"未找到":tess)+"\n";
    try{report+="OCR语言："+Exporter::ocrLanguages().join(", ")+"\n";}catch(const std::exception&e){report+=errorText(e)+"\n";}
    auto *dialog=new QDialog(this);dialog->setWindowTitle("设备检测结果");dialog->resize(720,500);auto *v=new QVBoxLayout(dialog);auto *text=new QPlainTextEdit;text->setReadOnly(true);text->setPlainText(report);v->addWidget(text);auto *save=new QPushButton("保存检测报告");v->addWidget(save);connect(save,&QPushButton::clicked,dialog,[this,report]{auto path=QFileDialog::getSaveFileName(this,"保存检测报告","设备检测.txt","文本 (*.txt)");if(path.isEmpty())return;QSaveFile out(path);auto bytes=report.toUtf8();if(!out.open(QIODevice::WriteOnly)||out.write(bytes)!=bytes.size()||!out.commit())fail("无法保存报告，请检查权限和磁盘空间。");});dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->show();
}
void MainWindow::closeEvent(QCloseEvent *event){if(cleaningStorage_){event->ignore();return;}stopBatch();if(busy_&&QMessageBox::question(this,"退出程序","当前操作尚未完成，取消操作并退出？已保存页面将保留。",QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes){event->ignore();return;}closing_=true;timer_.stop();if(recording_)toggleRecording();cancel_.store(true);camera_.stop();watcher_.waitForFinished();event->accept();}
}
