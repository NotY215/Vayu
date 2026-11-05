// Vayu-IDE V0.2-Alpha — IDLE-style UI, Tools/Compile, AppData recents, no dialogs for input
#include <QApplication>
#include <QMainWindow>
#include <QSplitter>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QToolBar>
#include <QMenuBar>
#include <QMenu>
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
#include <QFile>
#include <QIcon>
#include <QDir>
#include <QCoreApplication>
#include <QKeySequence>
#include <QSettings>
#include <QDesktopServices>
#include <QUrl>
#include <QWheelEvent>
#include <QShortcut>
#include <QTextCursor>
#include <QFileInfo>
#include <QInputDialog>
#ifdef Q_OS_WIN
#  include <windows.h>
#endif

static const char* kVersion = "V0.2-Alpha";
static const char* kRepo = "https://github.com/NotY215/Vayu";

struct Example { QString title; QString code; };
static QList<Example> builtinExamples() {
    return {
        {"Hello + input",
         "print(\"Hello World\")\n"
         "c = input(\"say my name: \")\n"
         "if c == \"heisenberg\":\n"
         "    print(\"You are God damn Right\")\n"
         "else:\n"
         "    print(\"try again\")\n"},
        {"Strings split / join",
         "s = \"a,b,c\"\n"
         "parts = s.split(\",\")\n"
         "print(parts)\n"
         "print(\",\".join(parts))\n"
         "print(\"Hello\".upper())\n"
         "print(\"  x  \".strip())\n"},
        {"Loops range / list",
         "for i in range(5):\n"
         "    print(i)\n"
         "for i in range(2, 6):\n"
         "    print(i)\n"
         "for x in [10, 20, 30]:\n"
         "    print(x)\n"},
        {"Functions",
         "def add(a, b):\n"
         "    return a + b\n"
         "print(add(2, 3))\n"
         "print(2 ** 10)\n"},
        {"While break",
         "n = 5\n"
         "while n > 0:\n"
         "    print(n)\n"
         "    n = n - 1\n"
         "    if n == 2:\n"
         "        break\n"},
    };
}

class ZoomPlainText : public QPlainTextEdit {
public:
    explicit ZoomPlainText(QWidget* p = nullptr) : QPlainTextEdit(p) {}
protected:
    void wheelEvent(QWheelEvent* e) override {
        if (e->modifiers() & Qt::ControlModifier) {
            QFont f = font();
            int pt = f.pointSize();
            if (e->angleDelta().y() > 0) f.setPointSize(pt + 1);
            else if (pt > 6) f.setPointSize(pt - 1);
            setFont(f);
            e->accept();
            return;
        }
        QPlainTextEdit::wheelEvent(e);
    }
};

class HelpDialog : public QDialog {
    ZoomPlainText* editor_ = nullptr;
public:
    HelpDialog(ZoomPlainText* ed, QWidget* parent = nullptr) : QDialog(parent), editor_(ed) {
        setWindowTitle(QString("Vayu-IDE %1 — Help").arg(kVersion));
        resize(860, 580);
        auto* tabs = new QTabWidget(this);
        auto md = [](const QString& s) {
            auto* w = new QTextEdit; w->setReadOnly(true); w->setMarkdown(s); return w;
        };
        tabs->addTab(md(QString(
            "# Syntax (brief)\n\n"
            "- Compare with `==`; strings need quotes.\n"
            "- Indent if/else/while/for bodies.\n"
            "- Methods on literals OK: `\"a,b\".split(\",\")`, `\",\".join(list)`.\n"
            "- `for i in range(N)` and `range(start,end)`.\n\n"
            "Repo: %1").arg(kRepo)), "Syntax");
        tabs->addTab(md(
            "# Functions\n\n"
            "`print` `input` `read_int` `len` `str` `int` `float` `bool` `range`\n"), "Functions");
        tabs->addTab(md(
            "# Strings\n\n"
            "concat `+`, methods: upper lower strip split join contains find "
            "starts_with ends_with replace char_at to_int is_digit is_alpha is_space\n"), "Strings");

        auto* exPage = new QWidget;
        auto* lay = new QVBoxLayout(exPage);
        auto* list = new QListWidget;
        for (const auto& e : builtinExamples()) list->addItem(e.title);
        auto* btn = new QPushButton("Load selected example into editor");
        lay->addWidget(new QLabel("Prebuilt examples (Integrator-safe):"));
        lay->addWidget(list);
        lay->addWidget(btn);
        tabs->addTab(exPage, "Examples");
        connect(btn, &QPushButton::clicked, this, [this, list]{
            int r = list->currentRow();
            if (r < 0 || !editor_) return;
            editor_->setPlainText(builtinExamples()[r].code);
            accept();
        });
        connect(list, &QListWidget::itemDoubleClicked, btn, &QPushButton::click);

        auto* outer = new QVBoxLayout(this);
        outer->addWidget(tabs);
        auto* link = new QPushButton("Open Vayu GitHub repo");
        connect(link, &QPushButton::clicked, this, []{ QDesktopServices::openUrl(QUrl(kRepo)); });
        outer->addWidget(link);
    }
};

class MainWindow : public QMainWindow {
    ZoomPlainText* editor_ = nullptr;
    ZoomPlainText* output_ = nullptr;
    QLineEdit* termIn_ = nullptr;
    QString path_;
    bool dirty_ = false;
    bool wrap_ = false;
    QProcess* running_ = nullptr;
    QSettings settings_;
    QMenu* recentMenu_ = nullptr;

public:
    MainWindow()
        : settings_(QSettings::IniFormat, QSettings::UserScope,
                    QStringLiteral("VayuIDE"), QStringLiteral("VayuIDE")) {
        setWindowTitle(QString("Vayu-IDE %1").arg(kVersion));
        resize(1100, 750);
        QIcon appIcon(QStringLiteral(":/IDE.ico"));
        if (!appIcon.isNull()) setWindowIcon(appIcon);

        editor_ = new ZoomPlainText;
        output_ = new ZoomPlainText;
        output_->setReadOnly(true);
        termIn_ = new QLineEdit;
        termIn_->setPlaceholderText(QStringLiteral("Program input (stdin) — type line, Enter to send"));
        QFont mono("Cascadia Mono", 11);
        if (!mono.exactMatch()) mono = QFont("Consolas", 11);
        editor_->setFont(mono);
        output_->setFont(mono);

        auto* bottom = new QWidget;
        auto* bl = new QVBoxLayout(bottom);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->addWidget(new QLabel("  OUTPUT / TERMINAL"));
        bl->addWidget(output_);
        bl->addWidget(termIn_);

        auto* split = new QSplitter(Qt::Vertical);
        split->addWidget(editor_);
        split->addWidget(bottom);
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 1);
        setCentralWidget(split);

        buildMenus();
        connect(editor_, &QPlainTextEdit::textChanged, this, [this]{ dirty_ = true; });
        connect(termIn_, &QLineEdit::returnPressed, this, [this]{
            if (!running_) return;
            QString line = termIn_->text() + "\n";
            running_->write(line.toUtf8());
            termIn_->clear();
            output_->appendPlainText(QStringLiteral("> ") + line.trimmed());
        });

        qApp->setStyleSheet(
            "QMainWindow,QWidget{background:#1e1e1e;color:#d4d4d4;}"
            "QPlainTextEdit,QLineEdit,QListWidget,QTextEdit{background:#252526;color:#d4d4d4;border:1px solid #3c3c3c;}"
            "QMenuBar{background:#2d2d2d;} QMenu{background:#2d2d2d;}"
            "QStatusBar{background:#007acc;color:white;}"
            "QPushButton{background:#0e639c;color:white;padding:6px 12px;border:none;}");

        // Restore last file
        QString last = settings_.value("lastFile").toString();
        if (!last.isEmpty() && QFile::exists(last)) {
            QFile f(last);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                editor_->setPlainText(QString::fromUtf8(f.readAll()));
                path_ = last;
                dirty_ = false;
                statusBar()->showMessage(QString("Opened last file: %1").arg(last));
            }
        } else {
            statusBar()->showMessage(QString("Vayu-IDE %1 — ready").arg(kVersion));
        }
        rebuildRecentMenu();

        // Alt+Z wrap
        auto* scWrap = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Z), this);
        connect(scWrap, &QShortcut::activated, this, [this]{
            wrap_ = !wrap_;
            auto mode = wrap_ ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap;
            editor_->setLineWrapMode(mode);
            output_->setLineWrapMode(mode);
            statusBar()->showMessage(wrap_ ? "Word wrap ON" : "Word wrap OFF");
        });
    }

    ~MainWindow() override {
        if (running_) { running_->kill(); running_->waitForFinished(500); }
    }

private:
    void buildMenus() {
        auto* mb = menuBar();
        // File
        auto* file = mb->addMenu("&File");
        auto add = [this](QMenu* m, const QString& t, const QKeySequence& ks, auto fn) {
            QAction* a = m->addAction(t); a->setShortcut(ks); connect(a, &QAction::triggered, this, fn); return a;
        };
        add(file, "&New", QKeySequence::New, [this]{ path_.clear(); editor_->clear(); dirty_=false; });
        add(file, "&Open...", QKeySequence::Open, [this]{ openFile(); });
        recentMenu_ = file->addMenu("Open &Recent");
        add(file, "&Save", QKeySequence::Save, [this]{ saveFile(); });
        add(file, "Save &As...", QKeySequence(), [this]{ path_.clear(); saveFile(); });
        file->addSeparator();
        add(file, "E&xit", QKeySequence::Quit, [this]{ close(); });

        // Edit (IDLE-like)
        auto* edit = mb->addMenu("&Edit");
        add(edit, "&Undo", QKeySequence::Undo, [this]{ editor_->undo(); });
        add(edit, "&Redo", QKeySequence::Redo, [this]{ editor_->redo(); });
        edit->addSeparator();
        add(edit, "Select &All", QKeySequence::SelectAll, [this]{ editor_->selectAll(); });
        add(edit, "Cu&t", QKeySequence::Cut, [this]{ editor_->cut(); });
        add(edit, "&Copy", QKeySequence::Copy, [this]{ editor_->copy(); });
        add(edit, "&Paste", QKeySequence::Paste, [this]{ editor_->paste(); });
        edit->addSeparator();
        add(edit, "&Find...", QKeySequence::Find, [this]{ findText(); });
        add(edit, "Find A&gain", QKeySequence(Qt::CTRL | Qt::Key_G), [this]{ findAgain(); });
        add(edit, "&Replace...", QKeySequence(Qt::CTRL | Qt::Key_H), [this]{ replaceText(); });
        add(edit, "Go to &Line...", QKeySequence(Qt::ALT | Qt::Key_G), [this]{ gotoLine(); });
        edit->addSeparator();
        add(edit, "Word wrap (Alt+Z)", QKeySequence(), [this]{
            wrap_ = !wrap_;
            auto mode = wrap_ ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap;
            editor_->setLineWrapMode(mode); output_->setLineWrapMode(mode);
        });

        // Tools
        auto* tools = mb->addMenu("&Tools");
        add(tools, "Run (&native)", QKeySequence(Qt::Key_F5), [this]{ runArgs({"--native"}); });
        add(tools, "Run (interpreter)", QKeySequence(), [this]{ runArgs({}); });
        add(tools, "Run (VM)", QKeySequence(), [this]{ runArgs({"--vm"}); });
        add(tools, "Dump IR", QKeySequence(), [this]{ runArgs({"--dump-ir"}); });
        add(tools, "Stop process", QKeySequence(), [this]{ if (running_) running_->kill(); });

        // Compile
        auto* compile = mb->addMenu("&Compile");
        add(compile, "Emit &.vye", QKeySequence(Qt::Key_F7), [this]{ emitVye(); });
        add(compile, "Run existing .vye...", QKeySequence(), [this]{ runVye(); });
        add(compile, "Create host .exe launcher...", QKeySequence(), [this]{ emitExeLauncher(); });

        // Help
        auto* help = mb->addMenu("&Help");
        add(help, "&Help / Syntax...", QKeySequence(), [this]{
            HelpDialog dlg(editor_, this); dlg.exec();
        });
        add(help, "&Load Examples...", QKeySequence(), [this]{
            HelpDialog dlg(editor_, this); dlg.exec();
        });
        add(help, "&Version", QKeySequence(), [this]{
            QMessageBox::information(this, "Version",
                QString("Vayu-IDE %1\nVayu compiler %1 Release\n%2").arg(kVersion).arg(kRepo));
        });
        add(help, "Open &Repo", QKeySequence(), []{ QDesktopServices::openUrl(QUrl(kRepo)); });
    }

    QString lastFind_;
    void findText() {
        bool ok = false;
        QString s = QInputDialog::getText(this, "Find", "Find:", QLineEdit::Normal, lastFind_, &ok);
        if (!ok || s.isEmpty()) return;
        lastFind_ = s;
        if (!editor_->find(s))
            statusBar()->showMessage("Not found");
    }
    void findAgain() {
        if (lastFind_.isEmpty()) { findText(); return; }
        if (!editor_->find(lastFind_)) {
            // wrap
            QTextCursor c = editor_->textCursor(); c.movePosition(QTextCursor::Start);
            editor_->setTextCursor(c);
            editor_->find(lastFind_);
        }
    }
    void replaceText() {
        bool ok = false;
        QString a = QInputDialog::getText(this, "Replace", "Find:", QLineEdit::Normal, lastFind_, &ok);
        if (!ok) return;
        QString b = QInputDialog::getText(this, "Replace", "Replace with:", QLineEdit::Normal, QString(), &ok);
        if (!ok) return;
        lastFind_ = a;
        QString t = editor_->toPlainText();
        t.replace(a, b);
        editor_->setPlainText(t);
    }
    void gotoLine() {
        bool ok = false;
        int line = QInputDialog::getInt(this, "Go to Line", "Line:", 1, 1, 1000000, 1, &ok);
        if (!ok) return;
        QTextCursor c = editor_->textCursor();
        c.movePosition(QTextCursor::Start);
        c.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor, line - 1);
        editor_->setTextCursor(c);
        editor_->setFocus();
    }

    QString vayuBin() const {
        QDir d(QCoreApplication::applicationDirPath());
        for (const char* n : {"vayu.exe", "vayuc.exe"}) {
            QString p = d.filePath(n);
            if (QFile::exists(p)) return p;
        }
        return QStringLiteral("vayu.exe");
    }

    void rememberFile(const QString& p) {
        path_ = p;
        settings_.setValue("lastFile", p);
        QStringList rec = settings_.value("recent").toStringList();
        rec.removeAll(p);
        rec.prepend(p);
        while (rec.size() > 12) rec.removeLast();
        settings_.setValue("recent", rec);
        rebuildRecentMenu();
    }
    void rebuildRecentMenu() {
        if (!recentMenu_) return;
        recentMenu_->clear();
        for (const QString& p : settings_.value("recent").toStringList()) {
            QAction* a = recentMenu_->addAction(p);
            connect(a, &QAction::triggered, this, [this, p]{
                QFile f(p);
                if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
                editor_->setPlainText(QString::fromUtf8(f.readAll()));
                rememberFile(p);
                dirty_ = false;
            });
        }
    }

    void openFile() {
        QString f = QFileDialog::getOpenFileName(this, "Open", path_, "Vayu (*.vyu);;All (*.*)");
        if (f.isEmpty()) return;
        QFile file(f);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        editor_->setPlainText(QString::fromUtf8(file.readAll()));
        rememberFile(f);
        dirty_ = false;
    }
    bool saveFile() {
        if (path_.isEmpty()) {
            path_ = QFileDialog::getSaveFileName(this, "Save", QString(), "Vayu (*.vyu)");
            if (path_.isEmpty()) return false;
        }
        QFile file(path_);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
        file.write(editor_->toPlainText().toUtf8());
        dirty_ = false;
        rememberFile(path_);
        statusBar()->showMessage("Saved");
        return true;
    }

    void runArgs(const QStringList& extra) {
        if (running_) {
            QMessageBox::information(this, "Busy", "Already running — use Tools → Stop.");
            return;
        }
        if (path_.isEmpty() && !saveFile()) return;
        if (dirty_ && !saveFile()) return;
        output_->clear();
        QStringList args;
        args << path_ << extra;
        output_->appendPlainText(QString("[run %1]").arg(extra.join(' ')));

        auto* proc = new QProcess(this);
        running_ = proc;
        proc->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
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
                if (!rest.isEmpty()) output_->appendPlainText(QString::fromUtf8(rest));
                output_->appendPlainText(QString("—— exit %1 ——").arg(code));
                if (running_ == proc) running_ = nullptr;
                proc->deleteLater();
                statusBar()->showMessage(code == 0 ? "Finished" : "Finished with errors");
            });
        proc->start(vayuBin(), args);
        if (!proc->waitForStarted(5000)) {
            output_->appendPlainText("[error] could not start vayu.exe / vayuc.exe");
            running_ = nullptr;
            proc->deleteLater();
            return;
        }
        // Keep stdin open for Terminal line (input/read_int)
        statusBar()->showMessage("Running in background — use Terminal input for input()/read_int()");
    }

    void emitVye() {
        if (path_.isEmpty() && !saveFile()) return;
        if (dirty_ && !saveFile()) return;
        QString out = path_;
        if (out.endsWith(".vyu")) out = out.left(out.size() - 4) + ".vye";
        else out += ".vye";
        runArgs({"--output", out});
    }
    void runVye() {
        QString f = QFileDialog::getOpenFileName(this, "Run .vye", path_, "Vaye (*.vye)");
        if (f.isEmpty()) return;
        output_->clear();
        output_->appendPlainText("[run-vye]");
        auto* proc = new QProcess(this);
        running_ = proc;
        proc->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
        proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* cpa) {
            cpa->flags |= CREATE_NO_WINDOW;
        });
#endif
        connect(proc, &QProcess::readyRead, this, [this, proc]{
            output_->appendPlainText(QString::fromUtf8(proc->readAll()));
        });
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc](int code, QProcess::ExitStatus) {
                if (running_ == proc) running_ = nullptr;
                output_->appendPlainText(QString("—— exit %1 ——").arg(code));
                proc->deleteLater();
            });
        proc->start(vayuBin(), QStringList{f});
        if (!proc->waitForStarted(3000)) {
            output_->appendPlainText("[error] start failed");
            running_ = nullptr;
            proc->deleteLater();
        }
    }
    void emitExeLauncher() {
        // Host-arch launcher: .cmd next to .vye that invokes matching vayu.exe
        if (path_.isEmpty() && !saveFile()) return;
        if (dirty_ && !saveFile()) return;
        QString vye = path_;
        if (vye.endsWith(".vyu")) vye = vye.left(vye.size() - 4) + ".vye";
        else vye += ".vye";
        // First emit vye synchronously-ish via one-shot process
        QProcess emitProc;
#ifdef Q_OS_WIN
        emitProc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* cpa) {
            cpa->flags |= CREATE_NO_WINDOW;
        });
#endif
        emitProc.start(vayuBin(), QStringList{path_, "--output", vye});
        emitProc.waitForFinished(60000);
        output_->appendPlainText(QString::fromUtf8(emitProc.readAllStandardOutput()));
        output_->appendPlainText(QString::fromUtf8(emitProc.readAllStandardError()));
        if (emitProc.exitCode() != 0) {
            output_->appendPlainText("[emit-vye failed]");
            return;
        }
        QString launcher = vye;
        if (launcher.endsWith(".vye")) launcher = launcher.left(launcher.size() - 4) + ".cmd";
        else launcher += ".cmd";
        QString bin = vayuBin();
        QString body = QString("@echo off\r\n"
            "REM Vayu host launcher %1 — same arch as this PC\r\n"
            "\"%2\" \"%~dp0%3\"\r\n")
            .arg(kVersion).arg(bin).arg(QFileInfo(vye).fileName());
        QFile f(launcher);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write(body.toUtf8());
            output_->appendPlainText(QString("[launcher] wrote %1 (host arch via local vayu)").arg(launcher));
            statusBar()->showMessage("Host launcher created (architecture-matched via local runtime)");
        }
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Vayu-IDE");
    QApplication::setApplicationVersion(kVersion);
    QApplication::setOrganizationName("VayuIDE");
    QIcon appIcon(QStringLiteral(":/IDE.ico"));
    if (!appIcon.isNull()) app.setWindowIcon(appIcon);
    MainWindow w;
    w.show();
    return app.exec();
}
