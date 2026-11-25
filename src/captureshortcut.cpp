#include "mainwindow.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QTextEdit>
#include <QVBoxLayout>

namespace scan {
namespace {
QString shortcutError(const QKeySequence &key)
{
    if (key.count() != 1 || key[0] == Qt::Key_unknown)
        return QStringLiteral("请按一个键，或同时按下一组组合键。");
    if (key == QKeySequence(Qt::Key_Tab) || key == QKeySequence(Qt::Key_Backtab) ||
        key == QKeySequence(Qt::Key_Escape) || key == QKeySequence(Qt::ALT | Qt::Key_F4) ||
        key == QKeySequence(Qt::CTRL | Qt::Key_Q) ||
        QKeySequence::keyBindings(QKeySequence::SelectAll).contains(key))
        return QStringLiteral("这个键用于窗口操作或选择页面，请换一个。");
    return {};
}

QString shutterTip(const QKeySequence &key)
{
    const QString display = key == QKeySequence(Qt::Key_Space)
        ? QStringLiteral("空格") : key.toString(QKeySequence::NativeText);
    return QStringLiteral("执行当前拍摄操作（%1）").arg(display);
}

bool editingText(QWidget *widget)
{
    return qobject_cast<QLineEdit *>(widget) || qobject_cast<QAbstractSpinBox *>(widget) ||
           qobject_cast<QPlainTextEdit *>(widget) || qobject_cast<QTextEdit *>(widget) ||
           qobject_cast<QKeySequenceEdit *>(widget);
}
}

void MainWindow::installCaptureShortcut()
{
    auto key = QKeySequence::fromString(QSettings().value("captureShortcut", "Space").toString(),
                                        QKeySequence::PortableText);
    if (!shortcutError(key).isEmpty()) key = QKeySequence(Qt::Key_Space);
    captureShortcut_ = new QShortcut(key, this);
    captureShortcut_->setObjectName("shutterShortcut");
    captureShortcut_->setContext(Qt::WindowShortcut);
    captureShortcut_->setAutoRepeat(false);
    shoot_->setToolTip(shutterTip(key));
    // Give editable fields the actual keystroke, including printable keys and spaces.
    auto updateFocus = [this](QWidget *, QWidget *now) {
        captureShortcut_->setEnabled(!editingText(now));
    };
    connect(qApp, &QApplication::focusChanged, this, updateFocus);
    updateFocus(nullptr, QApplication::focusWidget());
    connect(captureShortcut_, &QShortcut::activated, this, [this] {
        if (!busy_ && !capturePending_ && controls_->isEnabled() &&
            !QApplication::activeModalWidget() && !QApplication::activePopupWidget())
            scanAction();
    });
    connect(captureShortcut_, &QShortcut::activatedAmbiguously, this, [this] {
        message(tr("拍照快捷键与其他操作冲突，请在“文件与设置”中更换。"));
    });
}

void MainWindow::configureCaptureShortcut()
{
    if (busy_ || capturePending_ || recording_) return;
    stopBatch();
    QDialog dialog(this);
    dialog.setObjectName("captureShortcutDialog");
    dialog.setWindowTitle(tr("拍照快捷键"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *editor = new QKeySequenceEdit(captureShortcut_->key(), &dialog);
    editor->setObjectName("captureShortcutEdit");
    form->addRow(tr("拍照键"), editor);
    layout->addLayout(form);
    auto *hint = new QLabel(tr("点输入框，然后按想用的键，例如 F8 或 Ctrl+Shift+P。"), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *error = new QLabel(&dialog);
    error->setObjectName("shortcutError");
    error->setStyleSheet("color: #a1352c");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setObjectName("captureShortcutSave");
    auto *restore = buttons->addButton(tr("恢复空格"), QDialogButtonBox::ResetRole);
    restore->setObjectName("restoreDefaultShortcut");
    layout->addWidget(buttons);
    connect(restore, &QPushButton::clicked, &dialog, [editor, error] {
        editor->setKeySequence(QKeySequence(Qt::Key_Space));
        error->hide();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [this, &dialog, editor, error] {
        const auto key = editor->keySequence();
        auto issue = shortcutError(key);
        if (issue.isEmpty()) {
            QSettings settings;
            settings.setValue("captureShortcut", key.toString(QKeySequence::PortableText));
            settings.sync();
            if (settings.status() != QSettings::NoError)
                issue = tr("无法保存快捷键，请检查用户配置目录的写入权限。");
        }
        if (!issue.isEmpty()) {
            error->setText(issue);
            error->show();
            editor->setFocus();
            return;
        }
        captureShortcut_->setKey(key);
        shoot_->setToolTip(shutterTip(key));
        dialog.accept();
    });
    dialog.setMinimumWidth(335);
    dialog.exec();
}
}
