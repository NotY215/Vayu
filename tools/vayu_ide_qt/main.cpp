// Vayu-IDE (Qt6) — no foreground console, input/read_int via dialogs, loadable examples
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
#include <QHBoxLayout>
#include <QTabWidget>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QFile>
#include <QIcon>
#include <QDir>
#include <QCoreApplication>
#include <QKeySequence>
#include <QRegularExpression>
#ifdef Q_OS_WIN
#  include <windows.h>
#endif

struct Example {
    QString title;
    QString code;
};

static QList<Example> builtinExamples() {
    return {
        {QStringLiteral("Hello + input"),
         QStringLiteral(
             "print(\"Hello World\")\n"
             "c = input(\"say my name: \")\n"
             "if c == \"heisenberg\":\n"
             "    print(\"You are God damn Right\")\n"
             "else:\n"
             "    print(\"try again\")\n")},
        {QStringLiteral("Age (read_int)"),
         QStringLiteral(
             "name = input(\"Name: \")\n"
             "print(\"Hello, \" + name)\n"
             "print(\"Enter an integer:\")\n"
             "age = read_int()\n"
             "print(\"Your age is:\")\n"
             "print(age)\n"
             "print(\"Next year you will be:\")\n"
             "print(age + 1)\n")},
        {QStringLiteral("Loops range / list"),
         QStringLiteral(
             "print(\"range:\")\n"
             "for i in range(5):\n"
             "    print(i)\n"
             "print(\"range(2,6):\")\n"
             "for i in range(2, 6):\n"
             "    print(i)\n"
             "print(\"list:\")\n"
             "for x in [10, 20, 30]:\n"
             "    print(x)\n")},
        {QStringLiteral("Strings split / join"),
         QStringLiteral(
             "s = \"a,b,c\"\n"
             "parts = s.split(\",\")\n"
             "print(parts)\n"
             "print(\",\".join(parts))\n"
             "print(\"Hello\".upper())\n"
             "print(\"  x  \".strip())\n")},
        {QStringLiteral("Functions + arithmetic"),
         QStringLiteral(
             "def add(a, b):\n"
             "    return a + b\n"
             "print(add(2, 3))\n"
             "print(2 ** 8)\n"
             "print(10 // 3)\n"
             "print(12 & 10)\n")},
        {QStringLiteral("While + break"),
         QStringLiteral(
             "n = 5\n"
             "while n > 0:\n"
             "    print(n)\n"
             "    n = n - 1\n"
             "    if n == 2:\n"
             "        break\n"
             "print(\"done\")\n")},
    };
}

static const char* kHelpSyntax = R"MD(
# Syntax (with examples)

Compare with `==`. Strings need quotes. Indent bodies.

```
print("Hello World")
c = input("say my name: ")
if c == "heisenberg":
    print("You are God damn Right")
else:
    print("try again")
```

Use **Help → Load example…** to paste a known-good program into the editor.
)MD";

static const char* kHelpFunctions = R"MD(
# Functions

| Name | Example |
|------|---------|
| print | `print("hi")` |
| input | `n = input("Name: ")` |
| read_int | `a = read_int()` |
| len | `len("ab")` / `len([1,2])` |
| str/int/float/bool | `int("10")+1` |
| range | `for i in range(5):` or `range(2,6)` |
)MD";

static const char* kHelpStrings = R"MD(
# Strings

```
s = "a,b,c"
print(s.split(","))
print("-".join(["x","y"]))
print("Hi".upper())
print("  z  ".strip())
print(s.contains("b"))
```
)MD";

class HelpDialog : public QDialog {
    QPlainTextEdit* editorTarget_ = nullptr;
public:
    HelpDialog(QPlainTextEdit* editor, QWidget* parent = nullptr)
        : QDialog(parent), editorTarget_(editor) {
        setWindowTitle(QStringLiteral("Vayu Help"));
        resize(820, 560);
        auto* tabs = new QTabWidget(this);
        auto make = [](const char* md) {
            auto* w = new QTextEdit;
            w->setReadOnly(true);
            w->setMarkdown(QString::fromUtf8(md));
            return w;
        };
        tabs->addTab(make(kHelpSyntax), QStringLiteral("Syntax"));
        tabs->addTab(make(kHelpFunctions), QStringLiteral("Functions"));
        tabs->addTab(make(kHelpStrings), QStringLiteral("Strings"));

        auto* exPage = new QWidget;
        auto* layEx = new QVBoxLayout(exPage);
        auto* list = new QListWidget;
        for (const Example& e : builtinExamples())
            list->addItem(e.title);
        auto* btnLoad = new QPushButton(QStringLiteral("Load selected example into editor"));
        layEx->addWidget(new QLabel(QStringLiteral("Prebuilt programs (run error-free on Integrator):")));
        layEx->addWidget(list);
        layEx->addWidget(btnLoad);
        tabs->addTab(exPage, QStringLiteral("Examples"));

        connect(btnLoad, &QPushButton::clicked, this, [this, list]{
            int row = list->currentRow();
            if (row < 0) return;
            const auto examples = builtinExamples();
            if (row >= examples.size()) return;
            if (editorTarget_) {
                editorTarget_->setPlainText(examples[row].code);
            }
            accept();
        });
        connect(list, &QListWidget::itemDoubleClicked, btnLoad, &QPushButton::click);

        auto* lay = new QVBoxLayout(this);
        lay->addWidget(tabs);
    }
};

// Collect stdin lines for each input()/read_int() in source order
static QByteArray collectStdinForSource(const QString& src, QWidget* parent) {
    QStringList linesOut;
    QRegularExpression re(
        QStringLiteral("\\b(input|read_int)\\s*\\("),
        QRegularExpression::MultilineOption);
    auto it = re.globalMatch(src);
    int n = 0;
    while (it.hasNext()) {
        auto m = it.next();
        QString kind = m.captured(1);
        ++n;
        bool ok = false;
        if (kind == QStringLiteral("read_int")) {
            int v = QInputDialog::getInt(parent, QStringLiteral("read_int()"),
                QStringLiteral("Integer input #%1:").arg(n), 0, -2147483647, 2147483647, 1, &ok);
            if (!ok) return {};
            linesOut << QString::number(v);
        } else {
            // try extract prompt string roughly
            QString prompt = QStringLiteral("input #%1:").arg(n);
            int pos = m.capturedEnd();
            if (pos < src.size() && src[pos] == QLatin1Char('"')) {
                int end = src.indexOf(QLatin1Char('"'), pos + 1);
                if (end > pos)
                    prompt = src.mid(pos + 1, end - pos - 1);
            }
            QString v = QInputDialog::getText(parent, QStringLiteral("input()"),
                prompt, QLineEdit::Normal, QString(), &ok);
            if (!ok) return {};
            linesOut << v;
        }
    }
    QByteArray data;
    for (const QString& s : linesOut) {
        data += s.toUtf8();
        data += '\n';
    }
    return data;
}

class MainWindow : public QMainWindow {
    QPlainTextEdit* editor_ = nullptr;
    QPlainTextEdit* output_ = nullptr;
    QString path_;
    bool dirty_ = false;
    QProcess* running_ = nullptr;

public:
    MainWindow() {
        setWindowTitle(QStringLiteral("Vayu-IDE"));
        resize(1100, 750);
        QIcon appIcon(QStringLiteral(":/IDE.ico"));
        if (!appIcon.isNull()) setWindowIcon(appIcon);

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
        auto* actStop = tb->addAction(QStringLiteral("Stop"));
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
        connect(actStop, &QAction::triggered, this, [this]{
            if (running_) running_->kill();
        });
        connect(actHelp, &QAction::triggered, this, [this]{
            HelpDialog dlg(editor_, this);
            dlg.exec();
        });
        connect(editor_, &QPlainTextEdit::textChanged, this, [this]{ dirty_ = true; });

        qApp->setStyleSheet(QStringLiteral(
            "QMainWindow, QWidget { background-color: #1e1e1e; color: #d4d4d4; }"
            "QPlainTextEdit, QTextEdit, QListWidget { background-color: #252526; color: #d4d4d4; border: 1px solid #3c3c3c; }"
            "QToolBar { background: #2d2d2d; spacing: 6px; padding: 4px; border: none; }"
            "QToolButton, QPushButton { background: #0e639c; color: white; padding: 6px 12px; border: none; border-radius: 4px; }"
            "QToolButton:hover, QPushButton:hover { background: #1177bb; }"
            "QStatusBar { background: #007acc; color: white; }"
            "QTabBar::tab { background: #2d2d2d; color: #d4d4d4; padding: 8px 14px; }"
            "QTabBar::tab:selected { background: #0e639c; }"
        ));
        statusBar()->showMessage(QStringLiteral("Ready · F5 Run (no console window) · Help → Examples"));
    }

    ~MainWindow() override {
        if (running_) {
            running_->kill();
            running_->waitForFinished(1000);
        }
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
        if (running_) {
            QMessageBox::information(this, QStringLiteral("Busy"),
                QStringLiteral("A run is already in progress. Press Stop first."));
            return;
        }
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
            // Prefer Integrator so input/print match CLI --native behaviour
            args << QStringLiteral("--native");
            output_->appendPlainText(QStringLiteral("[run --native]"));
        }

        QByteArray stdinData;
        if (!compileVye) {
            stdinData = collectStdinForSource(editor_->toPlainText(), this);
            // empty with cancelled dialog → abort
            QRegularExpression re(QStringLiteral("\\b(input|read_int)\\s*\\("));
            if (re.match(editor_->toPlainText()).hasMatch() && stdinData.isNull()) {
                output_->appendPlainText(QStringLiteral("[cancelled]"));
                return;
            }
        }

        auto* proc = new QProcess(this);
        running_ = proc;
        proc->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
        // Prevent Windows from opening a foreground console for vayuc.exe
        proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* cpa) {
            cpa->flags |= CREATE_NO_WINDOW;
        });
#endif
        connect(proc, &QProcess::readyRead, this, [this, proc]{
            output_->appendPlainText(QString::fromUtf8(proc->readAll()));
        });
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc](int code, QProcess::ExitStatus) {
                QByteArray rest = proc->readAll();
                if (!rest.isEmpty())
                    output_->appendPlainText(QString::fromUtf8(rest));
                output_->appendPlainText(QStringLiteral("—— exit %1 ——").arg(code));
                statusBar()->showMessage(code == 0
                    ? QStringLiteral("Finished") : QStringLiteral("Finished with errors"));
                if (running_ == proc) running_ = nullptr;
                proc->deleteLater();
            });

        proc->start(vayucPath(), args);
        if (!proc->waitForStarted(5000)) {
            output_->appendPlainText(QStringLiteral("[error] could not start vayuc.exe"));
            running_ = nullptr;
            proc->deleteLater();
            return;
        }
        if (!stdinData.isEmpty()) {
            proc->write(stdinData);
            proc->closeWriteChannel();
        } else {
            proc->closeWriteChannel();
        }
        statusBar()->showMessage(QStringLiteral("Running in background…"));
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Vayu-IDE"));
    QIcon appIcon(QStringLiteral(":/IDE.ico"));
    if (!appIcon.isNull()) app.setWindowIcon(appIcon);
    MainWindow w;
    w.show();
    return app.exec();
}
