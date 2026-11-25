#include "mainwindow.h"
#include "cropcanvas.h"
#include "imageprocessor.h"
#include "scanstorage.h"
#include <QtWidgets>
#include <QtConcurrent>
#include <algorithm>

namespace scan {
namespace {
QPushButton *action(QBoxLayout *layout, const QString &name, const QString &hint = {}) {
    auto *b=new QPushButton(name);
    b->setToolTip(hint);
    b->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(b);
    return b;
}
QToolButton *popupButton(const QString &label) {
    auto *button=new QToolButton;
    button->setText(label);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setMenu(new QMenu(button));
    return button;
}
void field(QGridLayout *grid, int row, int column, const QString &title, QWidget *editor) {
    auto *label=new QLabel(title);
    label->setBuddy(editor);
    editor->setAccessibleName(title);
    grid->addWidget(label,row,column);
    grid->addWidget(editor,row,column+1);
}
QImage screenImage(const QString &path, int edge) {
    QImageReader reader(path);
    const QSize size=reader.size();
    if(size.isValid())reader.setScaledSize(size.scaled(edge,edge,Qt::KeepAspectRatio));
    return reader.read();
}
}
void MainWindow::createUi() {
    setWindowTitle(tr("高拍仪拍照"));
    setWindowIcon(QIcon(":/scanner.svg"));
    setMinimumSize(760,540);
    const QSize available=QGuiApplication::primaryScreen()->availableGeometry().size();
    resize(QSize(1280,790).boundedTo(available-QSize(40,75)));
    setStyleSheet(R"(
      QWidget#base {background:#fff}
      QWidget {font-size:14px;color:#29343d}
      QLabel#sectionTitle {font-weight:bold;color:#354450}
      QPushButton,QToolButton {border:1px solid #cbd2d8;border-radius:4px;padding:5px 10px;background:#fff;min-height:20px}
      QPushButton:hover,QToolButton:hover {background:#f0f5f9;border-color:#8ea4b5}
      QPushButton:focus,QToolButton:focus,QComboBox:focus,QLineEdit:focus,QSpinBox:focus {border:1px solid #5283aa}
      QPushButton:disabled,QToolButton:disabled {color:#9aabb7;background:#f3f5f6}
      QToolButton:checked {color:#20577f;background:#eaf2f8;border-color:#a6c0d3}
      QToolButton[popupMode="2"] {padding-right:19px}
      QToolButton::menu-indicator {subcontrol-origin:padding;subcontrol-position:center right;right:4px}
      QPushButton#scanButton {background:#215f8a;border:1px solid #215f8a;color:white;font-weight:600;padding:5px 18px}
      QPushButton#scanButton:hover {background:#184c70}
      QPushButton#scanButton:disabled {background:#91adbf;border-color:#91adbf}
      QComboBox,QLineEdit,QSpinBox {background:white;border:1px solid #cbd2d8;border-radius:4px;min-height:24px;padding:2px 5px}
      QFrame#captureBar {background:white;border-bottom:1px solid #dce2e6}
      QFrame#captureOptions {background:#f5f7f9;border-bottom:1px solid #dce2e6}
      QFrame#pagePane {background:#f7f9fa}
      QTabWidget#workspaceTabs::pane {border:0;background:#fff}
      QTabBar::tab {border:0;border-bottom:2px solid transparent;background:white;color:#596775;padding:9px 13px}
      QTabBar::tab:selected {color:#215f8a;border-bottom:2px solid #215f8a;font-weight:bold}
      QTabBar::tab:hover:!selected {background:#f2f5f7}
      QListWidget {background:#fff;border:0;outline:none}
      QListWidget#pageList {background:#f7f9fa}
      QListWidget::item {padding:5px;border-radius:3px}
      QListWidget::item:selected {background:#deebf4;color:#193d57}
      QSplitter::handle {background:#d9dfe3}
      QFrame#statusRow {background:#f7f9fa;border-top:1px solid #dce2e6}
      QLabel#statusText {color:#43535e}
      QMenu {background:#fff;border:1px solid #cbd2d8;padding:4px}
      QMenu::item {padding:6px 24px 6px 12px}
      QMenu::item:selected {background:#e5eff6;color:#184c70}
      QMenu::separator {height:1px;background:#dce2e6;margin:4px}
    )");
    auto *base=new QWidget;
    base->setObjectName("base");
    setCentralWidget(base);
    auto *outer=new QVBoxLayout(base);
    outer->setContentsMargins(0,0,0,0);
    outer->setSpacing(0);
    controls_=new QWidget;
    outer->addWidget(controls_,1);
    auto *layout=new QVBoxLayout(controls_);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(0);

    auto *bar=new QFrame;
    bar->setObjectName("captureBar");
    auto *tools=new QHBoxLayout(bar);
    tools->setContentsMargins(14,10,14,10);
    tools->setSpacing(8);
    modeBar_=new QComboBox;
    modeBar_->setObjectName("captureMode");
    modeBar_->setAccessibleName("工作模式");
    modeBar_->addItems({"快速使用","拍照","文档","书籍","条码","票证","电邮","复印","录像"});
    tools->addWidget(new QLabel("模式"));
    tools->addWidget(modeBar_);
    tools->addSpacing(4);
    cropChoice_=new QComboBox;
    cropChoice_->setObjectName("cropChoice");
    cropChoice_->setAccessibleName("裁切方式");
    cropChoice_->addItems({"自动裁切 A4","手动四角","整幅取景"});
    tools->addWidget(new QLabel("裁切"));
    tools->addWidget(cropChoice_);
    auto *optionButton=new QToolButton;
    optionButton->setObjectName("captureOptionsButton");
    optionButton->setText("拍摄选项");
    optionButton->setCheckable(true);
    optionButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    optionButton->setArrowType(Qt::RightArrow);
    tools->addWidget(optionButton);
    tools->addStretch();
    shoot_=action(tools,"扫描");
    shoot_->setObjectName("scanButton");
    shoot_->setMinimumWidth(90);
    auto *exportBtn=action(tools,"导出");
    exportBtn->setObjectName("exportButton");
    auto *menuButton=popupButton("文件与设置");
    menuButton->setObjectName("fileMenuButton");
    auto *menu=menuButton->menu();
    menu->addAction("新建扫描任务",this,&MainWindow::newSession);
    menu->addAction("打开扫描任务…",this,[this]{
        stopBatch();
        const auto path=QFileDialog::getOpenFileName(this,"打开任务",exportDirectory(),"扫描任务 (session.json)");
        if(!path.isEmpty())openSession(path);
    });
    menu->addAction("导入图片…",this,&MainWindow::importImages);
    menu->addSeparator();
    menu->addAction("拍照快捷键…",this,&MainWindow::configureCaptureShortcut)->setObjectName("shortcutSettings");
    menu->addAction("清理拍摄缓存…",this,&MainWindow::cleanCaptureStorage)->setObjectName("cleanStorageAction");
    menu->addAction("设备检测",this,&MainWindow::detectDeviceReport);
    tools->addWidget(menuButton);
    layout->addWidget(bar);

    auto *advanced=new QFrame;
    advanced->setObjectName("captureOptions");
    auto *options=new QGridLayout(advanced);
    options->setContentsMargins(16,11,16,12);
    options->setHorizontalSpacing(10);
    options->setVerticalSpacing(6);
    scanChoice_=new QComboBox;
    scanChoice_->addItems({"扫描多页","扫描单页"});
    trigger_=new QComboBox;
    trigger_->addItems({"手动","定时连拍"});
    interval_=new QSpinBox;
    interval_->setRange(1,120);
    interval_->setValue(3);
    interval_->setSuffix(" 秒");
    exportKind_=new QComboBox;
    exportKind_->addItems({"PDF","可搜索 PDF","TXT","JPG","PNG"});
    color_=new QComboBox;
    color_->addItems({"彩色（原色）","灰度","黑白（文档增强）"});
    prefix_=new QLineEdit("IMG");
    prefix_->setMaxLength(50);
    prefix_->setValidator(new QRegularExpressionValidator(QRegularExpression(R"rx([^/\\:*?"<>|]{0,50})rx"),prefix_));
    field(options,0,0,"保存方式",scanChoice_);
    field(options,0,2,"拍摄方式",trigger_);
    field(options,0,4,"连拍间隔",interval_);
    field(options,1,0,"输出格式",exportKind_);
    field(options,1,2,"图像效果",color_);
    field(options,1,4,"文件前缀",prefix_);
    devices_=new QComboBox;
    devices_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    devices_->setMinimumContentsLength(7);
    resolution_=new QComboBox;
    resolution_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    resolution_->setMinimumContentsLength(10);
    field(options,2,0,"设备",devices_);
    field(options,2,2,"分辨率",resolution_);
    auto *deviceActions=new QHBoxLayout;
    deviceActions->setSpacing(6);
    connect_=action(deviceActions,"连接设备");
    connect_->setObjectName("connectButton");
    auto *refresh=action(deviceActions,"刷新");
    options->addLayout(deviceActions,2,4,1,2);
    a4_=new QCheckBox("导出为 A4 页面");
    a4_->setChecked(true);
    options->addWidget(a4_,3,1,1,2);
    options->setColumnStretch(1,1);
    options->setColumnStretch(3,1);
    options->setColumnStretch(5,1);
    layout->addWidget(advanced);
    advanced->hide();
    connect(optionButton,&QToolButton::toggled,this,[advanced,optionButton](bool open){
        advanced->setVisible(open);
        optionButton->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });

    auto *split=new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);
    layout->addWidget(split,1);
    auto *left=new QFrame;
    left->setObjectName("pagePane");
    left->setMinimumWidth(145);
    auto *strip=new QVBoxLayout(left);
    strip->setContentsMargins(9,6,9,10);
    strip->setSpacing(5);
    auto *pageHeader=new QHBoxLayout;
    pageCount_=new QLabel("扫描页 · 0");
    pageCount_->setObjectName("sectionTitle");
    pageHeader->addWidget(pageCount_,1);
    auto *organize=popupButton("整理");
    organize->setAccessibleName("整理扫描页");
    organize->menu()->addAction("上移一页",this,[this]{movePage(-1);});
    organize->menu()->addAction("下移一页",this,[this]{movePage(1);});
    organize->menu()->addAction("从任务中移除",this,&MainWindow::removePages);
    pageHeader->addWidget(organize);
    strip->addLayout(pageHeader);
    pages_=new QListWidget;
    pages_->setObjectName("pageList");
    pages_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    pages_->setIconSize(QSize(79,88));
    pages_->setSpacing(2);
    strip->addWidget(pages_,1);
    modeAction_=new QPushButton;
    modeAction_->setObjectName("modeAction");
    strip->addWidget(modeAction_);
    split->addWidget(left);

    tabs_=new QTabWidget;
    tabs_->setObjectName("workspaceTabs");
    tabs_->setDocumentMode(true);
    live_=new CropCanvas;
    page_=new CropCanvas;
    page_->setMessage("选择已拍摄的页面即可调整");
    tabs_->addTab(live_,"实时取景");
    tabs_->addTab(page_,"页面调整");
    auto *filePage=new QWidget;
    auto *fileLayout=new QVBoxLayout(filePage);
    fileLayout->setContentsMargins(14,12,14,12);
    auto *folderTools=new QHBoxLayout;
    taskLabel_=new QLabel;
    taskLabel_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    folderTools->addWidget(taskLabel_,1);
    auto *choose=action(folderTools,"更改目录");
    auto *open=action(folderTools,"打开目录");
    fileLayout->addLayout(folderTools);
    files_=new QListWidget;
    fileLayout->addWidget(files_,1);
    tabs_->addTab(filePage,"输出文件");
    ocr_=new QPlainTextEdit(base);
    ocr_->setReadOnly(true);
    ocr_->hide();
    auto *viewButton=popupButton("视图工具");
    viewButton->setObjectName("viewToolsButton");
    auto *viewMenu=viewButton->menu();
    auto *larger=viewMenu->addAction("放大");
    auto *smaller=viewMenu->addAction("缩小");
    auto *fit=viewMenu->addAction("适应窗口");
    viewMenu->addSeparator();
    auto *rotateLeft=viewMenu->addAction("向左旋转 90°");
    auto *rotateRight=viewMenu->addAction("向右旋转 90°");
    auto *editGroup=viewMenu->addSeparator();
    auto *edit=viewMenu->addAction("调整四角…",this,&MainWindow::editPage);
    auto *apply=viewMenu->addAction("应用当前效果",this,&MainWindow::applyEdits);
    auto *undo=viewMenu->addAction("恢复原图",this,&MainWindow::resetEdits);
    const QList<QAction *> pageActions{editGroup,edit,apply,undo};
    for(auto *item : pageActions)item->setVisible(false);
    tabs_->setCornerWidget(viewButton);
    split->addWidget(tabs_);
    split->setStretchFactor(0,0);
    split->setStretchFactor(1,1);
    split->setSizes({188,1020});

    auto *statusRow=new QFrame;
    statusRow->setObjectName("statusRow");
    auto *bottom=new QHBoxLayout(statusRow);
    bottom->setContentsMargins(12,4,12,4);
    status_=new QLabel("就绪");
    status_->setObjectName("statusText");
    status_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    status_->setWordWrap(true);
    bottom->addWidget(status_,1);
    rotationLabel_=new QLabel("旋转 0°");
    options->addWidget(rotationLabel_,3,5);
    progress_=new QProgressBar;
    progress_->setMaximumWidth(140);
    progress_->hide();
    bottom->addWidget(progress_);
    cancelButton_=action(bottom,"取消");
    cancelButton_->hide();
    outer->addWidget(statusRow);
    connect(cancelButton_,&QPushButton::clicked,this,[this]{stopBatch();cancel_.store(true);});
    connect(refresh,&QPushButton::clicked,this,&MainWindow::refreshDevices);
    connect(devices_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{refreshFormats();});
    connect(connect_,&QPushButton::clicked,this,&MainWindow::toggleCamera);
    connect(shoot_,&QPushButton::clicked,this,&MainWindow::scanAction);
    connect(exportBtn,&QPushButton::clicked,this,&MainWindow::exportPages);
    connect(pages_,&QListWidget::itemClicked,this,[this]{if(!updating_&&!busy_)showCurrent();});
    connect(pages_,&QListWidget::currentRowChanged,this,[this]{if(!updating_&&!busy_)showCurrent();});
    auto rotate=[this](int amount){
        if(tabs_->currentIndex()==1 && currentIndex()>=0){rotation_=(rotation_+amount)%360;applyEdits();}
        else {rotation_=(rotation_+amount)%360;rotationLabel_->setText(QString("拍照旋转 %1°").arg(rotation_));}
    };
    connect(rotateLeft,&QAction::triggered,this,[rotate]{rotate(270);});connect(rotateRight,&QAction::triggered,this,[rotate]{rotate(90);});
    auto zoom=std::make_shared<double>(1.);
    auto rescale=[this,zoom](double amount){*zoom=amount==0?1.:std::clamp(*zoom*amount,.4,3.);page_->setZoom(*zoom);live_->setZoom(*zoom);};
    connect(larger,&QAction::triggered,this,[rescale]{rescale(1.2);});connect(smaller,&QAction::triggered,this,[rescale]{rescale(1/1.2);});connect(fit,&QAction::triggered,this,[rescale]{rescale(0);});
    connect(choose,&QPushButton::clicked,this,[this]{stopBatch();auto path=QFileDialog::getExistingDirectory(this,"选择输出目录",exportDirectory());if(!path.isEmpty()){outputDir_=path;QSettings().setValue("output",path);browseResults();}});
    connect(open,&QPushButton::clicked,this,[this]{QDir().mkpath(exportDirectory());QDesktopServices::openUrl(QUrl::fromLocalFile(exportDirectory()));});
    connect(files_,&QListWidget::itemDoubleClicked,this,[](QListWidgetItem *item){QDesktopServices::openUrl(QUrl::fromLocalFile(item->data(Qt::UserRole).toString()));});
    connect(trigger_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int i){stopBatch();interval_->setEnabled(i==1);});
    connect(interval_,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int n){if(timer_.isActive())timer_.setInterval(n*1000);});
    connect(cropChoice_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int i){
        ++outlineGeneration_;
        auto points=live_->corners();
        if(i!=1) points.clear();
        else if(!ImageProcessor::validCorners(points,true)) points={{.12,.10},{.88,.10},{.88,.90},{.12,.90}};
        live_->setCorners(points);
        live_->setEditing(i==1);
        live_->setOverlayText(i==1?tr("拖动四角设置扫描区域，可将一个纸角放在画外"):QString());
    });
    connect(modeBar_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,&MainWindow::switchMode);
    connect(modeAction_,&QPushButton::clicked,this,[this]{
        if(mode_==Identity)composeIdentity();
        else if(mode_==Barcode)readBarcode();
        else if(mode_==Copy)printPages();
        else if(mode_==Email)draftEmail();
        else if(mode_==Book)scanAction();
        else if(mode_==Quick||mode_==Photo)importImages();
        else recognizePage();
    });
    connect(tabs_,&QTabWidget::currentChanged,this,[this,viewButton,pageActions](int index){
        viewButton->setVisible(index!=2);
        viewButton->setText(index==1?"页面工具":"视图工具");
        for(auto *item : pageActions)item->setVisible(index==1);
        if(index==1&&!updating_&&!busy_&&currentIndex()>=0)showCurrent();
        if(index==2)browseResults();
    });
    setupRecorder();
    outputDir_=QSettings().value("output").toString();
    interval_->setEnabled(false);browseResults();modeBar_->setCurrentIndex(Document);
}
void MainWindow::switchMode(int index) {
    stopBatch();if(recording_)toggleRecording();mode_=index;rotation_=0;
    rotationLabel_->setText("旋转 0°");tabs_->setCurrentIndex(0);
    cropChoice_->setCurrentIndex(index==Document||index==Quick||index==Copy||index==Email?0:2);
    color_->setCurrentIndex(0);
    exportKind_->setCurrentIndex(index==Photo||index==Barcode||index==Quick?3:0);
    scanChoice_->setCurrentIndex(index==Photo||index==Quick?1:0);
    static const QStringList actions={"导入图片","导入图片","识别文字","扫描双页","合并正反面","合并正反面","新建邮件","打印所选页",""};
    modeAction_->setText(index==Barcode?"读取条码":actions.at(index));modeAction_->setVisible(index!=Video && index!=Book);
    shoot_->setText(index==Video?"开始录像":index==Book?"扫描双页":index==Photo?"拍照":"扫描");
    message(index==Book?"将书脊放在取景中央，拍摄后按中线分为左右两页。":index==Identity?"分别拍摄正反面，选中两页后点击“合并正反面”。":index==Barcode?"拍摄后点击“读取条码”，可复制识别结果。":"就绪");
}
void MainWindow::stopBatch(){
    timer_.stop();shoot_->setText(mode_==Photo?"拍照":mode_==Book?"扫描双页":mode_==Video?"开始录像":"扫描");
}
void MainWindow::scanAction() {
    if(mode_==Video){toggleRecording();return;}
    if(timer_.isActive()){stopBatch();message("定时连拍已停止");return;}
    if(trigger_->currentIndex()==1 && camera_.isRunning() && ensureSession()){
        timer_.start(interval_->value()*1000);shoot_->setText("停止连拍");
    }
    capture();
}
QString MainWindow::exportDirectory() const {
    return outputDir_.isEmpty()?QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath("高拍仪输出"):outputDir_;
}
bool MainWindow::ensureDirectory(const QString &path) {
    try{
        auto id=QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz")+"_"+QUuid::createUuid().toString(QUuid::Id128).left(4);
        session_.create(QDir(path).filePath("任务_"+id));
        ScanStorage::registerTask(session_.directory);
        browseResults();
        return true;
    }catch(const std::exception &e){fail(QString::fromUtf8(e.what()));return false;}
}
void MainWindow::browseResults(){
    files_->clear();const QDir directory(exportDirectory());taskLabel_->setText(QDir::toNativeSeparators(directory.absolutePath()));taskLabel_->setToolTip(directory.absolutePath());
    const auto list=directory.entryInfoList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot,QDir::Time);
    for(const auto &entry:list){if(entry.fileName().startsWith("任务_"))continue;
        auto *item=new QListWidgetItem(entry.fileName(),files_);item->setData(Qt::UserRole,entry.absoluteFilePath());item->setToolTip(entry.absoluteFilePath());}
}
void MainWindow::previewFrame(const QImage &image){
    live_->setImage(image);
    if(busy_ || cropChoice_->currentIndex()!=0 || outlineJob_.isRunning() || outlineClock_.elapsed()<250 || tabs_->currentIndex()!=0)return;
    outlineClock_.restart();outlineLatency_.restart();submittedGeneration_=outlineGeneration_;
    outlineJob_.setFuture(QtConcurrent::run([image]{
        EditOptions outline;
        try { outline.corners=ImageProcessor::detectCorners(image,&outline.missingTriangle); }
        catch(...) { return EditOptions(); }
        return outline;
    }));
}
void MainWindow::editPage(){
    if(currentIndex()<0){message("先选择需要调整的页面");return;}
    stopBatch();auto p=session_.pages.at(currentIndex());
    QDialog dialog(this);dialog.setWindowTitle("调整四角");dialog.resize(850,650);auto *layout=new QVBoxLayout(&dialog);
    auto *canvas=new CropCanvas;const auto image=screenImage(session_.absolute(p.originalFile),1400);canvas->setImage(image);
    auto missing=p.edits.missingTriangle;
    auto corners=p.edits.corners;if(corners.isEmpty())corners=ImageProcessor::detectCorners(image,&missing);
    if(corners.isEmpty())corners={{.04,.04},{.96,.04},{.96,.96},{.04,.96}};
    canvas->setCorners(corners,missing);canvas->setEditing(true);layout->addWidget(canvas);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);layout->addWidget(buttons);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted)return;
    auto options=p.edits;options.corners=canvas->corners();options.missingTriangle=canvas->missingTriangle();options.autoCrop=false;
    if(!ImageProcessor::validCorners(options.corners,true)){fail("请让至少三个纸角留在画内，四角不能交叉或重叠。");return;}
    runJob([this,p,options]{auto out=ImageProcessor::process(QImage(session_.absolute(p.originalFile)),options);if(!cancel_.load())session_.update(p.id,out,options);},[this]{refreshPages();message("四角调整已保存");});
}
void MainWindow::showOcr(){
    auto *dialog=new QDialog(this);dialog->setWindowTitle("识别结果");dialog->resize(560,460);auto *layout=new QVBoxLayout(dialog);
    auto *text=new QPlainTextEdit(ocr_->toPlainText());text->setReadOnly(true);layout->addWidget(text);
    auto *copy=new QPushButton("复制文字");layout->addWidget(copy);connect(copy,&QPushButton::clicked,dialog,[text]{QApplication::clipboard()->setText(text->toPlainText());});
    dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->show();
}
QStringList MainWindow::outputSelection() const{
    QStringList paths;auto indices=selectedIndices();
    if(indices.isEmpty())for(int i=0;i<session_.pages.size();++i)indices<<i;
    for(int i:indices)paths<<session_.absolute(session_.pages.at(i).processedFile);
    return paths;
}

void MainWindow::appendCapture(const Page &p,const QImage &thumbnail){
    const auto key=session_.absolute(p.processedFile);
    auto icon=QIcon(QPixmap::fromImage(thumbnail));
    thumbnailCache_.insert(key,new QIcon(icon));
    const int index=pages_->count()+1;
    auto *item=new QListWidgetItem(icon,QString("第 %1 页").arg(index),pages_);
    item->setData(Qt::UserRole,p.id);
    QSignalBlocker block(pages_);
    pages_->setCurrentItem(item);pages_->scrollToItem(item);
    pageCount_->setText(QString("页面 · %1").arg(index));
}
void MainWindow::receiveFrame(const CapturedFrame &frame){
    capturePending_=false;
    if(busy_ || !session_.active()) return;
    const int mode=mode_;
    const auto options=pendingOptions_;
    auto added=std::make_shared<QVector<QPair<Page,QImage>>>();
    auto note=std::make_shared<QString>();
    const bool single=scanChoice_->currentIndex()==1;
    const auto kind=ExportKind(exportKind_->currentIndex());
    const bool paperA4=a4_->isChecked();
    const auto outputPath=exportDirectory();
    const QString stem=prefix_->text().trimmed().isEmpty()?QString("IMG"):prefix_->text().trimmed();
    const QString uniqueName=stem+"_"+QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz")+"_"+QUuid::createUuid().toString(QUuid::Id128).left(4);
    runJob([this,frame,options,mode,added,note,single,kind,paperA4,outputPath,uniqueName]{
        EditOptions applied=options;
        const QImage preview=frame.preview(800);
        if(preview.isNull())throw std::runtime_error("无法解码取景画面");
        if(applied.autoCrop){
            applied.corners=ImageProcessor::detectCorners(preview,&applied.missingTriangle);
            if(applied.corners.isEmpty())*note=" · 纸边不足以确定 A4，已保留整幅";
        }
        if(hasOffscreenCorner(applied.corners))*note=" · 已按 A4 裁切，画外区域已补白";
        if(!applied.missingTriangle.isEmpty())*note=" · 已按 A4 裁切，纸张缺角已补白";
        const bool edit=!applied.corners.isEmpty()||applied.color!=ColorMode::Color||applied.rotation!=0;
        QImage output;
        if(edit && mode!=Book){
            auto render=applied;render.autoCrop=false;
            output=ImageProcessor::process(frame.decode(),render);
        }
        if(cancel_.load())throw std::runtime_error("已取消拍摄保存");
        auto persist=[&](EditOptions opt,const QImage &image){
            auto page=session_.addCapture(frame,image,opt);
            auto thumbnail=(image.isNull()?preview:image).scaled(98,95,Qt::KeepAspectRatio,Qt::FastTransformation);
            added->append(qMakePair(page,thumbnail));
        };
        if(mode==Book){
            // Store each half as a crop of its own original, so corners remain editable.
            auto raw=frame.decode();const auto spread=applied;
            for(int side=0;side<2;++side){
                applied=spread;
                applied.missingTriangle.clear();
                double start=side?.5:0.,end=side?1.:.5;
                if(!applied.corners.isEmpty()){
                    auto c=applied.corners;
                    const auto top=(c[0]+c[1])*.5, bottom=(c[3]+c[2])*.5;
                    applied.corners=side?QVector<QPointF>{top,c[1],c[2],bottom}:QVector<QPointF>{c[0],top,bottom,c[3]};
                }else applied.corners={{start,0},{end,0},{end,1},{start,1}};
                applied.autoCrop=false;persist(applied,ImageProcessor::process(raw,applied));
            }

        }else persist(applied,output);
        if(single){
            QStringList paths;
            for(const auto &item:*added)paths<<session_.absolute(item.first.processedFile);
            if(kind==ExportKind::Png||kind==ExportKind::Jpeg){
                const QString extension=kind==ExportKind::Png?"png":"jpg";
                for(int i=0;i<paths.size();++i){
                    const auto dest=QDir(outputPath).filePath(uniqueName+(paths.size()>1?QString("_%1").arg(i+1):QString())+"."+extension);
                    if(QFileInfo(paths[i]).suffix().compare(extension,Qt::CaseInsensitive)==0){
                        if(!QFile::copy(paths[i],dest))throw std::runtime_error("页面已保存，输出图片复制失败");
                    }else{
                        QFile file(dest);
                        if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly))throw std::runtime_error("页面已保存，无法创建输出文件");
                        if(!QImage(paths[i]).save(&file,extension.toLatin1().constData(),kind==ExportKind::Jpeg?95:-1)||!file.flush()){
                            file.remove();throw std::runtime_error("页面已保存，输出图片写入失败");
                        }
                    }
                }
            }else{
                ExportRequest req;req.images=paths;req.kind=kind;req.a4=paperA4;
                req.destination=QDir(outputPath).filePath(uniqueName+(kind==ExportKind::Text?".txt":".pdf"));
                Exporter::run(req,cancel_);
            }
        }
    },[this,added,note,single]{
        for(const auto &result:*added)appendCapture(result.first,result.second);
        tabs_->setCurrentIndex(0);
        if(single)browseResults();
        message(QString("已保存第 %1 页 · %2 秒%3").arg(pages_->count()).arg(shutterClock_.elapsed()/1000.,0,'f',2).arg(*note));
    });
}
} // namespace scan
