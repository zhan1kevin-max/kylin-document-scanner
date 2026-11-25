#include "scanstorage.h"
#include "session.h"
#include <QtWidgets>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
void write(const QString &path,const QByteArray &data){
    QFile f(path);check(f.open(QIODevice::WriteOnly|QIODevice::Truncate),"cannot open fixture");
    check(f.write(data)==data.size(),"short fixture write");
}
QByteArray read(const QString &path){QFile f(path);check(f.open(QIODevice::ReadOnly),"read fixture");return f.readAll();}
void testFiles(const QString &root) {
    scan::Session task;task.create(root+"/任务_fixture");
    QImage image(140,180,QImage::Format_RGB32);image.fill(Qt::white);
    const auto one=task.add(image,image,{});
    const auto two=task.add(image,image,{});
    task.update(one.id,image,{});
    task.remove({two.id});
    const QString exportPath=task.directory+"/saved.pdf",notes=task.directory+"/originals/notes.txt";
    write(exportPath,"exported");write(notes,"personal");
    const auto renamed=task.directory+"/originals/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee.jpg";
    check(QFile::link(exportPath,renamed),"create symlink fixture");
    scan::ScanStorage::registerTask(task.directory);
    auto discovered=scan::ScanStorage::discover(root,task.directory);
    check(discovered.size()==1,"only one owned task");
    auto state=discovered.first();
    check(state.pages==1 && state.files.size()==5,"three obsolete and two live images");
    check(state.bytes(false)>0 && state.bytes(true)>0,"snapshot bytes");
    {
        scan::Session other;other.load(task.directory);
        const auto busy=scan::ScanStorage::clean(state,true,true,task);
        check(!busy.errors.isEmpty() && !busy.clearedPages,"another session protects the task");
    }
    auto obsolete=scan::ScanStorage::clean(state,true,false,task);
    check(obsolete.files==3 && !obsolete.clearedPages && obsolete.errors.isEmpty(),"only stale snapshots deleted");
    check(QFileInfo::exists(task.absolute(task.pages[0].originalFile)),"current original survives");
    check(QFileInfo::exists(task.absolute(task.pages[0].processedFile)),"current image survives");
    auto beforeEdit=scan::ScanStorage::inspect(task.directory);
    task.setOcr(one.id,"modified");
    check(!scan::ScanStorage::clean(beforeEdit,true,true,task).errors.isEmpty(),"changed manifest must be re-reviewed");
    auto canonical=scan::ScanStorage::inspect(task.directory);
    auto full=scan::ScanStorage::clean(canonical,true,true,task);
    check(full.clearedPages && full.errors.isEmpty() && full.files==2,"full task purge");
    scan::Session after;after.load(task.directory);check(after.pages.isEmpty(),"cleared manifest reopens");
    check(read(exportPath)=="exported" && read(notes)=="personal" && QFileInfo(renamed).isSymLink(),
          "exports, foreign names, and symbolic links survive");
    scan::Session invalid;invalid.create(root+"/任务_invalid");
    auto manifest=read(invalid.directory+"/session.json");
    manifest.replace("kylin-document-scanner","another-app");write(invalid.directory+"/session.json",manifest);
    check(scan::ScanStorage::inspect(invalid.directory).path.isEmpty(),"foreign manifest rejected");
    scan::Session escape;escape.create(root+"/任务_escape");const auto p=escape.add(image,image,{});
    auto contents=read(escape.directory+"/session.json");
    contents.replace(p.originalFile.toUtf8(),"../saved.pdf");write(escape.directory+"/session.json",contents);
    check(scan::ScanStorage::inspect(escape.directory).path.isEmpty(),"escape rejected");
}
void testConfirmation(const QString &root) {
    const QString bin=root+"/tools";QDir().mkpath(bin);
    // This command double only changes a sentinel in QTemporaryDir. Never
    // invoke system gio trash --empty, even when running on a real desktop.
    const QString command=bin+"/gio",sentinel=root+"/fake-trash-count";
    write(sentinel,"2");
    write(command,"#!/bin/sh\ncase \"$1\" in\ninfo) printf '  trash::item-count: '; cat \"$FAKE_TRASH_COUNT\";;\n"
                  "trash) [ \"$2\" = --empty ] || exit 3; printf 0 > \"$FAKE_TRASH_COUNT\";;\n*) exit 4;;\nesac\n");
    check(QFile::setPermissions(command,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner),"executable test double");
    qputenv("PATH",QFile::encodeName(bin)+":"+qgetenv("PATH"));qputenv("FAKE_TRASH_COUNT",QFile::encodeName(sentinel));
    check(QStandardPaths::findExecutable("gio")==command,"only mock gio allowed for this test");
    scan::Session task;task.create(root+"/dialog");
    QImage img(100,120,QImage::Format_RGB32);img.fill(Qt::black);const auto page=task.add(img,img,{});
    int step=0;QTimer poll;
    QObject::connect(&poll,&QTimer::timeout,[&]{
        auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog)return;
        if(step==0 && dialog->objectName()=="storageCleanupDialog"){
            auto *empty=dialog->findChild<QCheckBox*>("emptyTrashCheck");
            auto *table=dialog->findChild<QTableWidget*>("taskCleanupTable");
            check(empty && empty->isEnabled() && !empty->isChecked(),"trash deletion opt-in");
            bool chosen=false;
            for(int i=0;i<table->rowCount();++i)if(table->item(i,0)->toolTip()==task.directory){
                check(table->item(i,0)->checkState()==Qt::Unchecked,"live pages opt-in");
                table->item(i,0)->setCheckState(Qt::Checked);chosen=true;
            }
            check(chosen,"current task listed");empty->setChecked(true);step=1;
            QTimer::singleShot(0,dialog->findChild<QPushButton*>("permanentDeleteButton"),&QPushButton::click);
        }else if(step==1 && dialog->objectName()=="confirmStorageCleanup"){
            auto *box=qobject_cast<QMessageBox*>(dialog);
            check(box->defaultButton()==box->button(QMessageBox::Cancel),"cancel by default");
            check(read(sentinel)=="2" && QFileInfo::exists(task.absolute(page.originalFile)),"no eager deletion");
            step=2;box->button(QMessageBox::Cancel)->click();
        }else if(step==2 && dialog->objectName()=="storageCleanupDialog"){
            check(read(sentinel)=="2","cancelling kept foreign trash");
            step=3;QTimer::singleShot(0,dialog->findChild<QPushButton*>("permanentDeleteButton"),&QPushButton::click);
        }else if(step==3 && dialog->objectName()=="confirmStorageCleanup"){
            auto *box=qobject_cast<QMessageBox*>(dialog);
            for(auto *button:box->buttons())if(box->buttonRole(button)==QMessageBox::DestructiveRole){
                step=4;button->click();break;
            }
        }else if(step==4 && dialog->windowTitle()=="清理结果"){
            check(read(sentinel)=="0","explicit confirmation empties simulated trash");
            ++step;dialog->accept();
        }
    });
    poll.start(20);
    check(scan::ScanStorage::showDialog(nullptr,root,task),"GUI reports active task changed");
    check(step==5 && !QFileInfo::exists(task.absolute(page.originalFile)),"GUI confirmed snapshot erasure");
}
}
int main(int argc,char **argv){
    QApplication app(argc,argv);QCoreApplication::setOrganizationName("isolated-storage-test");
    QTemporaryDir dir;
    try{
        check(dir.isValid(),"fixture directory");
        qputenv("XDG_CONFIG_HOME",QFile::encodeName(dir.path()+"/config"));
        qputenv("XDG_DATA_HOME",QFile::encodeName(dir.path()+"/data"));
        qputenv("HOME",QFile::encodeName(dir.path()));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path()+"/config");
        testFiles(dir.path());testConfirmation(dir.path());std::cout<<"Storage cleanup tests passed.\n";
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
    return 0;
}
