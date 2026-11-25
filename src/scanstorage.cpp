#include "scanstorage.h"
#include "session.h"
#include <QtWidgets>
#include <QtConcurrent>
#include <QCryptographicHash>
#include <stdexcept>
#ifdef Q_OS_LINUX
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scan {
namespace {
const QString appId="kylin-document-scanner";
const QString uuid="[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}";
QString canonical(const QString &path) {
    return path.isEmpty() ? QString() : QFileInfo(path).canonicalFilePath();
}
QString bytesText(qint64 amount) {
    return QLocale().formattedDataSize(amount,1,QLocale::DataSizeTraditionalFormat);
}
bool generated(const QString &folder,const QString &name) {
    static const QRegularExpression original("^"+uuid+"\\.(?:jpg|png)$");
    static const QRegularExpression processed("^"+uuid+"(?:-"+uuid+")?\\.(?:jpg|png)$");
    return (folder=="originals"?original:processed).match(name).hasMatch();
}
bool validReference(const QString &path,const QString &id,bool original) {
    const auto parts=path.split('/');
    return parts.size()==2 && (parts[0]=="originals" || (!original && parts[0]=="processed"))
        && generated(parts[0],parts[1]) && parts[1].left(36)==id;
}
QByteArray hash(const QByteArray &data) {
    return QCryptographicHash::hash(data,QCryptographicHash::Sha256);
}
#ifdef Q_OS_LINUX
struct FileHandle {
    explicit FileHandle(int value):fd(value){}
    ~FileHandle(){if(fd>=0)::close(fd);}
    FileHandle(const FileHandle&)=delete;
    int fd;
};
// Use opened directory descriptors throughout. A renamed directory or replaced
// symlink cannot redirect an unlink into the user's exports or another folder.
int openDir(int parent,const QByteArray &name) {
    return ::openat(parent,name.constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
}
bool same(const SnapshotFile &a,const SnapshotFile &b) {
    return a.device==b.device && a.inode==b.inode && a.size==b.size &&
        a.modifiedSeconds==b.modifiedSeconds && a.modifiedNanos==b.modifiedNanos;
}
bool stamp(int parent,const QByteArray &entry,SnapshotFile &file) {
    struct stat st{};
    if(::fstatat(parent,entry.constData(),&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) ||
       st.st_nlink!=1 || st.st_uid!=::geteuid()) return false;
    file.device=st.st_dev;file.inode=st.st_ino;
    file.bytes=qint64(st.st_blocks)*512;file.size=st.st_size;
    file.modifiedSeconds=st.st_mtim.tv_sec;file.modifiedNanos=st.st_mtim.tv_nsec;
    return true;
}
QByteArray readManifest(int fd) {
    const int file=::openat(fd,"session.json",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(file<0) return {};
    FileHandle guard(file);
    struct stat st{};
    if(::fstat(file,&st) || !S_ISREG(st.st_mode) || st.st_uid!=::geteuid() ||
       st.st_nlink!=1 || st.st_size<1 || st.st_size>32*1024*1024) return {};
    QFile input;
    if(!input.open(file,QIODevice::ReadOnly,QFileDevice::DontCloseHandle))return {};
    return input.read(32*1024*1024+1);
}
bool readFiles(int root,const QString &sub,const QSet<QString> &live,SnapshotTask &task) {
    FileHandle folder(openDir(root,sub.toLatin1()));
    if(folder.fd<0)return false;
    const int scanFd=::dup(folder.fd);
    if(scanFd<0)return false;
    DIR *stream=::fdopendir(scanFd);
    if(!stream){::close(scanFd);return false;}
    errno=0;
    for(;;){
        errno=0;
        dirent *item=::readdir(stream);
        if(!item)break;
        const auto name=QFile::decodeName(item->d_name);
        if(!generated(sub,name))continue;
        SnapshotFile entry;
        if(!stamp(folder.fd,item->d_name,entry))continue;
        const auto key=sub+"/"+name;
        entry.referenced=live.contains(key);
        task.files.insert(key,entry);
    }
    const int error=errno;
    ::closedir(stream);
    return error==0;
}
SnapshotTask inspected(int fd,const QString &path) {
    if(fd<0)return {};
    const auto data=readManifest(fd);
    const auto json=QJsonDocument::fromJson(data).object();
    if(json.value("application")!=appId || json.value("schemaVersion").toInt()!=1 ||
       !json.value("pages").isArray())return {};
    QSet<QString> live;
    const auto pages=json.value("pages").toArray();
    for(const auto &page:pages){
        const auto item=page.toObject();
        const auto id=item.value("id").toString(),original=item.value("original").toString(),
            processed=item.value("processed").toString();
        if(id.size()!=36 || !validReference(original,id,true) || !validReference(processed,id,false))return {};
        live.insert(original);live.insert(processed);
    }
    SnapshotTask task;
    task.path=path;task.manifestHash=hash(data);task.pages=pages.size();
    if(!readFiles(fd,"originals",live,task) || !readFiles(fd,"processed",live,task))return {};
    // An invalid reference (including symlinks or missing images) is not a
    // license to discard the remaining documents in a damaged task.
    for(const auto &item:live)if(!task.files.contains(item))return {};
    return task;
}
struct DirectoryUnlock {
    int fd;
    bool shared;
    ~DirectoryUnlock(){::flock(fd,shared?LOCK_SH:LOCK_UN);}
};
#endif

struct TrashState { int count=-1; QString error; };
class CleanupProgress final : public QProgressDialog {
public:
    using QProgressDialog::QProgressDialog;
    void reject() override {}
protected:
    void closeEvent(QCloseEvent *event) override {event->ignore();}
};
struct CommandResult { bool ok; QString output; };
CommandResult gio(const QStringList &arguments) {
    const auto program=QStandardPaths::findExecutable("gio");
    if(program.isEmpty())return {false,"找不到系统回收站工具 gio。"};
    QProcess process;
    auto env=QProcessEnvironment::systemEnvironment();env.insert("LC_ALL","C");
    process.setProcessEnvironment(env);process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program,arguments);
    if(!process.waitForStarted(5000))return {false,"无法启动系统回收站服务。"};
    if(!process.waitForFinished(arguments.first()=="info"?10000:-1)){
        process.kill();process.waitForFinished();
        return {false,"系统回收站服务没有响应。"};
    }
    return {process.exitStatus()==QProcess::NormalExit && !process.exitCode(),
        QString::fromLocal8Bit(process.readAll()).trimmed()};
}
TrashState trashState(){
    auto info=gio({"info","-a","trash::item-count","trash:///"});
    const auto match=QRegularExpression("trash::item-count:\\s*(\\d+)").match(info.output);
    if(info.ok && match.hasMatch())return {match.captured(1).toInt(),{}};
    return {-1,info.ok?QString("系统未返回回收站项目数。"):info.output.left(350)};
}
template<typename F> auto withProgress(QWidget *parent,const QString &label,F fn)->decltype(fn()) {
    CleanupProgress progress(label,QString(),0,0,parent);
    progress.setWindowModality(Qt::ApplicationModal);progress.setCancelButton(nullptr);
    progress.setWindowFlag(Qt::WindowCloseButtonHint,false);progress.setMinimumDuration(0);
    QFutureWatcher<decltype(fn())> watcher;
    QEventLoop loop;
    QObject::connect(&watcher,&QFutureWatcherBase::finished,&loop,&QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(fn));
    progress.show();
    if(!watcher.isFinished())loop.exec();
    return watcher.result();
}
}

qint64 SnapshotTask::bytes(bool referenced) const {
    qint64 sum=0;for(const auto &f:files)if(f.referenced==referenced)sum+=f.bytes;
    return sum;
}
void ScanStorage::registerTask(const QString &path) {
    const auto dir=canonical(path);
    if(!dir.isEmpty())QSettings().setValue("scanHistory/"+hash(dir.toUtf8()).toHex(),dir);
}
QVector<SnapshotTask> ScanStorage::discover(const QString &output,const QString &current) {
    QSet<QString> paths;
    QSettings settings;settings.beginGroup("scanHistory");
    for(const auto &key:settings.childKeys())paths.insert(settings.value(key).toString());
    if(!current.isEmpty())paths.insert(canonical(current));
    const auto documents=QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QSet<QString> roots={output,documents,QDir(documents).filePath("高拍仪输出")};
    for(const auto &root:roots)if(!root.isEmpty()){
        const QDir d(root);
        for(const auto &entry:d.entryInfoList({"任务_*","扫描任务_*"},QDir::Dirs|QDir::NoSymLinks|QDir::NoDotAndDotDot))
            paths.insert(entry.canonicalFilePath());
    }
    auto sorted=paths.values();std::sort(sorted.begin(),sorted.end());
    QVector<SnapshotTask> result;
    for(const auto &path:sorted){auto task=inspect(path);if(!task.path.isEmpty() && !task.files.isEmpty())result.append(task);}
    return result;
}

bool ScanStorage::showDialog(QWidget *parent,const QString &output,const Session &active) {
    QDialog dialog(parent);
    dialog.setObjectName("storageCleanupDialog");dialog.setWindowTitle("清理拍摄快照");
    dialog.resize(650,440);
    const auto inventory=withProgress(parent,"正在统计拍摄快照…",[&]{
        return qMakePair(discover(output,active.directory),trashState());
    });
    const auto &tasks=inventory.first;
    const auto &trash=inventory.second;
    const auto current=canonical(active.directory);
    auto *layout=new QVBoxLayout(&dialog);
    auto *obsolete=new QCheckBox("清除已移除页面和旧编辑版本的快照",&dialog);
    obsolete->setObjectName("obsoleteSnapshotsCheck");obsolete->setChecked(true);
    layout->addWidget(obsolete);
    auto *prompt=new QLabel("勾选任务可一并删除该任务的全部扫描页和原图。已导出的文件会保留。",&dialog);
    prompt->setWordWrap(true);layout->addWidget(prompt);
    auto *table=new QTableWidget(tasks.size(),3,&dialog);
    table->setObjectName("taskCleanupTable");
    table->setHorizontalHeaderLabels({"同时清除全部页面","扫描页","废弃快照"});
    table->setSelectionMode(QAbstractItemView::NoSelection);table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->hide();table->setShowGrid(false);
    table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2,QHeaderView::ResizeToContents);
    qint64 oldBytes=0;
    for(int i=0;i<tasks.size();++i){
        const auto &task=tasks.at(i);
        auto title=QFileInfo(task.path).fileName();
        if(task.path==current)title+="（当前）";
        auto *name=new QTableWidgetItem(title);
        name->setToolTip(task.path);name->setFlags(Qt::ItemIsEnabled|Qt::ItemIsUserCheckable);
        name->setCheckState(Qt::Unchecked);
        table->setItem(i,0,name);
        table->setItem(i,1,new QTableWidgetItem(QString("%1 页 · %2").arg(task.pages).arg(bytesText(task.bytes(true)))));
        table->setItem(i,2,new QTableWidgetItem(bytesText(task.bytes(false))));
        oldBytes+=task.bytes(false);
    }
    if(tasks.isEmpty())layout->addWidget(new QLabel("当前没有可清理的扫描快照。",&dialog));
    else layout->addWidget(table);
    if(tasks.isEmpty())table->hide();
    auto *empty=new QCheckBox("同时清空系统回收站（包括其他应用删除的文件）",&dialog);
    empty->setObjectName("emptyTrashCheck");
    if(trash.count>=0)empty->setText(empty->text()+QString(" · %1 项").arg(trash.count));
    empty->setEnabled(trash.count!=0 && trash.error.isEmpty());
    if(!trash.error.isEmpty()){
        empty->setText("系统回收站暂不可用");empty->setToolTip(trash.error);
    }
    layout->addWidget(empty);
    auto *summary=new QLabel(&dialog);summary->setWordWrap(true);layout->addWidget(summary);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel,&dialog);
    auto *erase=buttons->addButton("永久删除…",QDialogButtonBox::DestructiveRole);
    erase->setObjectName("permanentDeleteButton");
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    layout->addWidget(buttons);
    qint64 bytes=0;int pages=0;
    const QStorageInfo disk(current.isEmpty()?output:current);
    auto refresh=[&]{
        bytes=obsolete->isChecked()?oldBytes:0;pages=0;
        for(int i=0;i<tasks.size();++i)if(table->item(i,0)->checkState()==Qt::Checked){
            bytes+=tasks[i].bytes(true);pages+=tasks[i].pages;
        }
        QString estimate="预计释放快照空间 "+bytesText(bytes);
        if(empty->isChecked())estimate+="（回收站空间另计）";
        if(disk.isValid() && disk.isReady())estimate+="\n"+QString(current.isEmpty()?"输出目录":"当前任务")+"所在磁盘可用 "+bytesText(disk.bytesAvailable());
        summary->setText(estimate);
        erase->setEnabled(bytes>0 || pages>0 || empty->isChecked());
    };
    QObject::connect(obsolete,&QCheckBox::toggled,&dialog,refresh);
    QObject::connect(empty,&QCheckBox::toggled,&dialog,refresh);
    QObject::connect(table,&QTableWidget::itemChanged,&dialog,refresh);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(erase,&QPushButton::clicked,&dialog,[&]{
        QString detail=QString("将永久删除 %1 的扫描快照。").arg(bytesText(bytes));
        if(pages)detail+=QString("\n其中包含 %1 个仍在任务中的页面及其原图，未导出的内容将丢失。").arg(pages);
        if(empty->isChecked())detail+="\n还将永久清空当前用户的整个系统回收站，包括其他应用删除的文件。";
        detail+="\n\n这些文件无法从回收站还原。";
        QMessageBox confirm(QMessageBox::Warning,"永久删除所选内容",detail,QMessageBox::Cancel,&dialog);
        confirm.setTextFormat(Qt::PlainText);
        confirm.setObjectName("confirmStorageCleanup");auto *yes=confirm.addButton("永久删除",QMessageBox::DestructiveRole);
        confirm.setDefaultButton(QMessageBox::Cancel);confirm.setEscapeButton(QMessageBox::Cancel);
        confirm.exec();if(confirm.clickedButton()==yes)dialog.accept();
    });
    refresh();
    if(dialog.exec()!=QDialog::Accepted)return false;
    QVector<bool> clearPages;
    for(int i=0;i<tasks.size();++i)clearPages.append(table->item(i,0)->checkState()==Qt::Checked);
    const bool removeOld=obsolete->isChecked(),emptyTrash=empty->isChecked();
    struct Outcome{CleanupResult result;bool activeChanged=false,trashEmptied=false;};
    const auto outcome=withProgress(parent,"正在永久删除所选项目…",[&]{
        Outcome out;
        for(int i=0;i<tasks.size();++i){
            if(!clearPages[i] && (!removeOld || !tasks[i].bytes(false)))continue;
            auto r=clean(tasks[i],removeOld,clearPages[i],active);
            out.result.bytes+=r.bytes;out.result.files+=r.files;out.result.errors.append(r.errors);
            if(current==tasks[i].path && r.clearedPages)out.activeChanged=true;
        }
        if(emptyTrash){
            const auto result=gio({"trash","--empty"});
            const auto after=trashState();
            out.trashEmptied=result.ok && after.count==0;
            if(!out.trashEmptied)
                out.result.errors.append("回收站未确认清空："+(result.ok?after.error:result.output).left(400));
        }
        return out;
    });
    QString message=QString("已永久删除 %1 个快照文件，大小共 %2。").arg(outcome.result.files).arg(bytesText(outcome.result.bytes));
    if(outcome.trashEmptied)message+="\n系统回收站已清空。";
    if(!outcome.result.errors.isEmpty())
        message+="\n\n"+outcome.result.errors.mid(0,8).join('\n');
    QMessageBox done(outcome.result.errors.isEmpty()?QMessageBox::Information:QMessageBox::Warning,
                     "清理结果",message,QMessageBox::Ok,parent);
    done.setTextFormat(Qt::PlainText);done.exec();
    return outcome.activeChanged;
}
SnapshotTask ScanStorage::inspect(const QString &path) {
#ifdef Q_OS_LINUX
    if(path.isEmpty() || canonical(path)!=QDir::cleanPath(QFileInfo(path).absoluteFilePath()))return {};
    FileHandle dir(openDir(AT_FDCWD,QFile::encodeName(path)));
    return inspected(dir.fd,path);
#else
    Q_UNUSED(path);return {};
#endif
}
CleanupResult ScanStorage::clean(const SnapshotTask &shown,bool obsolete,bool clearPages,const Session &active) {
    CleanupResult result;
#ifdef Q_OS_LINUX
    if(shown.path.isEmpty() || (!obsolete && !clearPages))return result;
    auto reject=[&](const QString &reason){result.errors.append(shown.path+" — "+reason);return result;};
    const bool current=!active.directory.isEmpty() && canonical(active.directory)==shown.path;
    FileHandle folder(openDir(AT_FDCWD,QFile::encodeName(shown.path)));
    if(folder.fd<0 || canonical(shown.path)!=shown.path)return reject("任务位置已变更");
    const int fd=current?active.taskDirectoryFd():folder.fd;
    if(fd<0)return reject("该文件系统不支持安全清理");
    struct stat opened{},named{};
    if(::fstat(fd,&opened) || ::fstat(folder.fd,&named) ||
       opened.st_dev!=named.st_dev || opened.st_ino!=named.st_ino)
        return reject("任务目录已移动，请重新查看");
    // flock upgrades can drop their shared lock briefly on failure. Keep
    // two cleaners from upgrading simultaneously while other windows have
    // the task open. Normal scanning only needs the read lease.
    QLockFile cleanupGate(QString("/proc/self/fd/%1/.scanner-cleanup.lock").arg(fd));
    cleanupGate.setStaleLockTime(0);
    if(!cleanupGate.tryLock())return reject("任务正在清理或目录不可写");
    if(::flock(fd,LOCK_EX|LOCK_NB)){
        if(current)::flock(fd,LOCK_SH);
        return reject("任务正在其他窗口使用，已跳过");
    }
    DirectoryUnlock unlock{fd,current};
    const auto fresh=inspected(fd,shown.path);
    if(fresh.path.isEmpty() || fresh.manifestHash!=shown.manifestHash)return reject("任务已变更，请重新查看");
    if(clearPages && fresh.pages>0){
        FileHandle originals(openDir(fd,"originals")),processed(openDir(fd,"processed"));
        if(originals.fd<0 || processed.fd<0 ||
           ::faccessat(originals.fd,".",W_OK|X_OK,0) || ::faccessat(processed.fd,".",W_OK|X_OK,0))
            return reject("原图或处理图目录不可写");
        for(auto i=fresh.files.cbegin();i!=fresh.files.cend();++i)
            if(i->referenced && (!shown.files.contains(i.key()) || !same(i.value(),shown.files.value(i.key()))))
                return reject("页面已变更，请重新查看");
        auto json=QJsonDocument::fromJson(readManifest(fd)).object();
        json.insert("pages",QJsonArray{});
        QSaveFile file(QString("/proc/self/fd/%1/session.json").arg(fd));
        const auto data=QJsonDocument(json).toJson();
        if(!file.open(QIODevice::WriteOnly) || file.write(data)!=data.size() || !file.commit())
            return reject("无法更新任务清单");
        result.clearedPages=true;
    }
    for(const auto &sub:{QStringLiteral("originals"),QStringLiteral("processed")}){
        FileHandle dir(openDir(fd,sub.toLatin1()));
        if(dir.fd<0)return reject("无法访问图像目录");
        for(auto i=shown.files.cbegin();i!=shown.files.cend();++i){
            if(!i.key().startsWith(sub+"/") || (i->referenced?!clearPages:!obsolete))continue;
            const auto name=QFile::encodeName(i.key().section('/',1));
            if(!fresh.files.contains(i.key()) || !same(i.value(),fresh.files.value(i.key())))continue;
            SnapshotFile now;
            if(!stamp(dir.fd,name,now) || !same(now,i.value()) || ::unlinkat(dir.fd,name.constData(),0)!=0){
                result.errors.append("未删除 "+i.key());continue;
            }
            result.bytes+=now.bytes;++result.files;
        }
    }
#else
    Q_UNUSED(shown);Q_UNUSED(obsolete);Q_UNUSED(clearPages);Q_UNUSED(active);
    result.errors.append("仅支持在麒麟桌面清理快照。");
#endif
    return result;
}
} // namespace scan
