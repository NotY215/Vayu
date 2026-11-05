// Vayu-IDE (Qt6) — professional dark UI, Help browser, Run/Compile.
#include <QApplication>
#include <QMainWindow>
#include <QSplitter>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QToolBar>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QProcess>
#include <QStatusBar>
#include <QFont>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QTabWidget>
#include <QFile>
#include <QTextStream>
#include <QKeySequence>
#include <QDir>
#include <QCoreApplication>

static const char* kHelpSyntax = R"MD(
# Vayu syntax (Integrator-supported subset)

## Basics
- Variables: `x = 1`, `s = "hi"`
- Types (optional annotations): `x: int = 1`
- Comments: `# comment`

## Control flow
```
if cond:
    ...
elif other:
    ...
else:
    ...

while cond:
    ...

for i in range(n):
    ...

for x in xs:   # list
    ...

break
continue
```

## Functions
```
def add(a: int, b: int) -> int:
    return a + b

print(add(1, 2))
```

## Operators
- Arithmetic: `+ - * / // % **`
- Bitwise: `& | ^ << >> ~`
- Logic: `and or not`
- Compare: `== != < <= > >=`
)MD";

static const char* kHelpFunctions = R"MD(
# Built-in functions (Integrator)

| Name | Use |
|------|-----|
| `print(...)` | Print values, space-separated, newline |
| `input(prompt?)` | Read a line (str). Prompt on same line |
| `read_int()` | Read a line as int |
| `len(x)` | Length of str / list / map / tuple / set |
| `str(x)` | Convert to string |
| `int(x)` | Convert to int (str/float/bool) |
| `float(x)` | Convert to float |
| `bool(x)` | Truthiness |
| `range(n)` | Used in `for i in range(n)` |
)MD";

static const char* kHelpStrings = R"MD(
# Strings (Integrator)

- Literals: `"hello"`, concat `s + t`, repeat `s * n`
- Compare: `== != <`
- Methods (on a string value):
  - `upper()` `lower()` `strip()`
  - `contains(sub)` `find(sub)`
  - `starts_with(p)` `ends_with(s)`
  - `is_digit()` `is_alpha()` `is_space()`
  - `replace(a, b)` `char_at(i)` `to_int()`
- `len(s)` length
)MD";

class HelpDialog : public QDialog {
public:
    explicit HelpDialog(QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle(QStringLiteral("Vayu Help"));
        resize(720, 520);
        auto* tabs = new QTabWidget(this);
        auto make = [](const char* md) {
            auto* w = new QTextEdit;
            w->setReadOnly(true);
            w->setMarkdown(QString::fromUtf8(md));
            w->setStyleSheet(QStringLiteral("QTextEdit { background:#1e1e1e; color:#d4d4d4; }"));
            return w;
        };
        tabs->addTab(make(kHelpSyntax), QStringLiteral("Syntax"));
        tabs->addTab(make(kHelpFunctions), QStringLiteral("Functions"));
        tabs->addTab(make(kHelpStrings), QStringLiteral("Strings"));
        auto* lay = new QVBoxLayout(this);
        lay->addWidget(tabs);
    }
};

class MainWindow : public QMainWindow {
    QPlainTextEdit* editor_ = nullptr;
    QPlainTextEdit* output_ = nullptr;
    QString path_;
    bool dirty_ = false;

public:
    MainWindow() {
        setWindowTitle(QStringLiteral("Vayu-IDE"));
        resize(1100, 750);

        editor_ = new QPlainTextEdit;
        output_ = new QPlainTextEdit;
        output_->setReadOnly(true);
        QFont mono(QStringLiteral("Cascadia Mono"), 11);
        if (!mono.exactMatch()) mono = QFont(QStringLiteral("Consolas"), 11);
        editor_->setFont(mono);
        output_->setFont(mono);

        auto* split = new QSplitter(Qt::Vertical);
        split->addWidget(editor_);
        split->addWidget(output_);
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 1);
        setCentralWidget(split);

        auto* tb = addToolBar(QStringLiteral("Main"));
        tb->setMovable(false);
        auto* actOpen = tb->addAction(QStringLiteral("Open"));
        auto* actSave = tb->addAction(QStringLiteral("Save"));
        tb->addSeparator();
        auto* actRun = tb->addAction(QStringLiteral("Run"));
        auto* actCompile = tb->addAction(QStringLiteral("Compile .vye"));
        tb->addSeparator();
        auto* actHelp = tb->addAction(QStringLiteral("Help"));
        actRun->setShortcut(QKeySequence(Qt::Key_F5));
        actCompile->setShortcut(QKeySequence(Qt::Key_F7));
        actOpen->setShortcut(QKeySequence::Open);
        actSave->setShortcut(QKeySequence::Save);

        connect(actOpen, &QAction::triggered, this, &MainWindow::openFile);
        connect(actSave, &QAction::triggered, this, &MainWindow::saveFile);
        connect(actRun, &QAction::triggered, this, [this]{ run(false); });
        connect(actCompile, &QAction::triggered, this, [this]{ run(true); });
        connect(actHelp, &QAction::triggered, this, [this]{
            HelpDialog dlg(this); dlg.exec();
        });
        connect(editor_, &QPlainTextEdit::textChanged, this, [this]{ dirty_ = true; });

        // Dark theme
        qApp->setStyleSheet(QStringLiteral(
            "QMainWindow, QWidget { background-color: #1e1e1e; color: #d4d4d4; }"
            "QPlainTextEdit { background-color: #252526; color: #d4d4d4; border: 1px solid #3c3c3c; }"
            "QToolBar { background: #2d2d2d; spacing: 6px; padding: 4px; border: none; }"
            "QToolButton, QPushButton { background: #0e639c; color: white; padding: 6px 12px; border: none; border-radius: 4px; }"
            "QToolButton:hover, QPushButton:hover { background: #1177bb; }"
            "QStatusBar { background: #007acc; color: white; }"
            "QSplitter::handle { background: #3c3c3c; }"
            "QDialog { background: #1e1e1e; }"
            "QTabWidget::pane { border: 1px solid #3c3c3c; }"
            "QTabBar::tab { background: #2d2d2d; color: #d4d4d4; padding: 8px 14px; }"
            "QTabBar::tab:selected { background: #0e639c; }"
        ));
        statusBar()->showMessage(QStringLiteral("Ready · F5 Run · F7 Compile · Help for syntax"));
    }

private:
    QString vayucPath() const {
        QDir d(QCoreApplication::applicationDirPath());
        QString p = d.filePath(QStringLiteral("vayuc.exe"));
        if (QFile::exists(p)) return p;
        return QStringLiteral("vayuc.exe");
    }

    void openFile() {
        QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Open"),
            QString(), QStringLiteral("Vayu (*.vyu);;All (*.*)"));
        if (f.isEmpty()) return;
        QFile file(f);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        editor_->setPlainText(QString::fromUtf8(file.readAll()));
        path_ = f;
        dirty_ = false;
        statusBar()->showMessage(path_);
    }

    bool saveFile() {
        if (path_.isEmpty()) {
            path_ = QFileDialog::getSaveFileName(this, QStringLiteral("Save"),
                QString(), QStringLiteral("Vayu (*.vyu)"));
            if (path_.isEmpty()) return false;
        }
        QFile file(path_);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
        file.write(editor_->toPlainText().toUtf8());
        dirty_ = false;
        statusBar()->showMessage(QStringLiteral("Saved"));
        return true;
    }

    void run(bool compileVye) {
        if (path_.isEmpty() && !saveFile()) return;
        if (dirty_ && !saveFile()) return;
        output_->clear();
        QStringList args;
        args << path_;
        if (compileVye) {
            QString out = path_;
            if (out.endsWith(QStringLiteral(".vyu")))
                out = out.left(out.size() - 4) + QStringLiteral(".vye");
            else
                out += QStringLiteral(".vye");
            args << QStringLiteral("--output") << out;
            output_->appendPlainText(QStringLiteral("[compile .vye]"));
        } else {
            output_->appendPlainText(QStringLiteral("[run]"));
        }
        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start(vayucPath(), args);
        if (!proc.waitForStarted(5000)) {
            output_->appendPlainText(QStringLiteral("[error] could not start vayuc.exe"));
            return;
        }
        proc.waitForFinished(-1);
        output_->appendPlainText(QString::fromUtf8(proc.readAllStandardOutput()));
        output_->appendPlainText(QStringLiteral("—— exit %1 ——").arg(proc.exitCode()));
        statusBar()->showMessage(proc.exitCode() == 0
            ? QStringLiteral("Finished") : QStringLiteral("Finished with errors"));
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Vayu-IDE"));
    QApplication::setOrganizationName(QStringLiteral("Vayu"));
    MainWindow w;
    w.show();
    return app.exec();
}
