#pragma once
#include "camera.h"
#include "session.h"
#include "exporter.h"
#include <QMainWindow>
#include <QFutureWatcher>
#include <QTimer>
#include <QElapsedTimer>
#include <QProcess>
#include <QCache>
#include <QIcon>
#include <atomic>
#include <functional>
class QLineEdit; class QShortcut; class QComboBox; class QPushButton; class QSpinBox; class QListWidget;
class QCheckBox; class QLabel; class QPlainTextEdit; class QTabWidget; class QProgressBar;
namespace scan {
class CropCanvas;
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent=nullptr);
    ~MainWindow() override;
    void connectPreferredCamera();
    void openSession(const QString &directory);
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    enum Mode { Quick, Photo, Document, Book, Barcode, Identity, Email, Copy, Video };
    void createUi();
    void installCaptureShortcut();
    void configureCaptureShortcut();
    void cleanCaptureStorage();
    void switchMode(int mode);
    void scanAction();
    void stopBatch();
    void previewFrame(const QImage &image);
    void receiveFrame(const CapturedFrame &frame);
    void appendCapture(const Page &page, const QImage &thumbnail);
    void browseResults();
    void printPages();
    void draftEmail();
    void readBarcode();
    void toggleRecording();
    void setupRecorder();
    void editPage();
    void showOcr();
    QStringList outputSelection() const;
    QString exportDirectory() const;
    bool ensureDirectory(const QString &path);
    QComboBox *modeBar_=nullptr;
    QShortcut *captureShortcut_=nullptr;
    QComboBox *cropChoice_=nullptr, *trigger_=nullptr, *scanChoice_=nullptr;
    QLineEdit *prefix_=nullptr;
    QPushButton *modeAction_=nullptr;
    QListWidget *files_=nullptr;
    QFutureWatcher<EditOptions> outlineJob_;
    QElapsedTimer outlineClock_, shutterClock_;
    quint64 outlineGeneration_=0, submittedGeneration_=0;
    QProcess recorder_;
    QString recordingPath_, outputDir_;
    bool resumeCamera_=false, closing_=false, recording_=false;
    int mode_=Document;
    EditOptions pendingOptions_;
    QElapsedTimer outlineLatency_;
    void refreshDevices();
    void refreshFormats();
    void toggleCamera();
    void capture();
    bool ensureSession();
    void newSession();
    void importImages();
    void refreshPages(const QString &focusId={});
    void showCurrent();
    int currentIndex() const;
    QVector<int> selectedIndices() const;
    void applyEdits();
    void resetEdits();
    void removePages();
    void movePage(int delta);
    void recognizePage();
    void exportPages();
    void composeIdentity();
    void detectDeviceReport();
    void runJob(std::function<void()> work,std::function<void()> done={});
    void setBusy(bool busy);
    void message(const QString &text);
    void fail(const QString &text);
    void progress(int done,int total,const QString &text);
    EditOptions currentEdits() const;
    Session session_;
    Camera camera_;
    QFutureWatcher<QString> watcher_;
    std::atomic_bool cancel_{false};
    std::function<void()> done_;
    bool busy_=false, updating_=false, capturePending_=false;
    bool cleaningStorage_=false;
    int rotation_=0;
    QTimer timer_;
    QVector<CameraFormat> formats_;
    QCache<QString,QIcon> thumbnailCache_{256};
    QWidget *controls_=nullptr;
    CropCanvas *live_=nullptr,*page_=nullptr;
    QListWidget *pages_=nullptr;
    QComboBox *devices_=nullptr,*resolution_=nullptr,*color_=nullptr,*exportKind_=nullptr;
    QPushButton *connect_=nullptr,*shoot_=nullptr,*cancelButton_=nullptr;
    QSpinBox *interval_=nullptr;
    QCheckBox *a4_=nullptr;
    QPlainTextEdit *ocr_=nullptr;
    QTabWidget *tabs_=nullptr;
    QLabel *status_=nullptr,*taskLabel_=nullptr,*pageCount_=nullptr,*rotationLabel_=nullptr;
    QProgressBar *progress_=nullptr;
};
}
