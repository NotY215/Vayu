// Vayu-IDE V0.2-Alpha — VSCode-style layout (Qt6)
#include <QApplication>
#include <QMainWindow>
#include <QSplitter>
#include <QTreeWidget>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QToolBar>
#include <QMenuBar>
#include <QStatusBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QProcess>
#include <QFont>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QDesktopServices>
#include <QUrl>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QListWidget>
#include <QStackedWidget>
#include <QToolButton>
#include <QFrame>
#include <QWheelEvent>
#include <QShortcut>
#include <QTextCursor>
#include <QInputDialog>
#include <QFile>
#include <QIcon>
#include <QCoreApplication>
#include <QStyle>
#ifdef Q_OS_WIN
#  include <windows.h>
#endif

static const char* kVer = "V0.2-Alpha";
static const char* kRepo = "https://github.com/NotY215/Vayu";

struct Example { QString title, code; };
static QList<Example> examples() {
    return {
        {"Hello + if",
         "print(\"Hello World\")\nc = input(\"name: \")\nif c == \"heisenberg\":\n    print(\"You are God damn Right\")\nelse:\n    print(\"try again\")\n"},
        {"Split / join",
         "print(\"a,b,c\".split(\",\"))\nprint(\",\".join([\"x\",\"y\"]))\n"},
        {"in + append",
         "print(2 in [1,2,3])\nxs = [1]\nxs.append(9)\nprint(xs)\n"},
        {"range / while",
         "for i in range(2, 6):\n    print(i)\nn = 3\nwhile n > 0:\n    print(n)\n    n = n - 1\n"},
        {"shell (cmd)",
         "code = shell(\"cmd /c echo Hello from cmd\")\nprint(code)\n"},
        {"shell (PowerShell)",
         "shell(\"powershell -NoProfile -Command \\\"Write-Host 'hi from pwsh'\\\"\")\n"},
        {"functions",
         "def add(a, b):\n    return a + b\nprint(add(10, 32))\n"},
    };
}

class ZoomEdit : public QPlainTextEdit {
public:
    using QPlainTextEdit::QPlainTextEdit;
protected:
    void wheelEvent(QWheelEvent* e) override {
        if (e->modifiers() & Qt::ControlModifier) {
            QFont f = font();
            int p = f.pointSize();
            if (e->angleDelta().y() > 0) f.setPointSize(p + 1);
            else if (p > 6) f.setPointSize(p - 1);
            setFont(f);
            e->accept();
            return;
        }
        QPlainTextEdit::wheelEvent(e);
    }
};

class MainWindow : public QMainWindow {
    QTreeWidget* explorer_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    ZoomEdit* output_ = nullptr;
    QLineEdit* termIn_ = nullptr;
    QStackedWidget* sideStack_ = nullptr;
    QWidget* welcome_ = nullptr;
    QString folder_;
    QSettings settings_;
    QProcess* proc_ = nullptr;
    bool wrap_ = false;
    QString lastFind_;

public:
    MainWindow()
        : settings_(QSettings::IniFormat, QSettings::UserScope, "VayuIDE", "VayuIDE") {
        setWindowTitle(QString("Vayu-IDE %1").arg(kVer));
        resize(1280, 800);
        QIcon ic(":/IDE.ico");
        if (!ic.isNull()) setWindowIcon(ic);

        // --- Activity bar (left) ---
        auto* activity = new QFrame;
        activity->setFixedWidth(48);
        activity->setObjectName("activity");
        auto* actLay = new QVBoxLayout(activity);
        actLay->setContentsMargins(4, 8, 4, 8);
        actLay->setSpacing(8);
        auto mkAct = [&](const QString& tip, auto fn) {
            auto* b = new QToolButton;
            b->setToolTip(tip);
            b->setFixedSize(40, 40);
            b->setText(tip.left(1));
            connect(b, &QToolButton::clicked, this, fn);
            actLay->addWidget(b);
            return b;
        };
        mkAct(QStringLiteral("Explorer"), [this]{ showExplorer(); });
        mkAct(QStringLiteral("Search"), [this]{ showSearch(); });
        mkAct(QStringLiteral("Source Control"), [this]{ showGitComingSoon(); });
        // NO Extensions button
        actLay->addStretch();
        mkAct(QStringLiteral("Settings"), [this]{ showSettings(); });

        // --- Side: explorer ---
        explorer_ = new QTreeWidget;
        explorer_->setHeaderLabel("EXPLORER");
        connect(explorer_, &QTreeWidget::itemDoubleClicked, this, &MainWindow::openTreeItem);

        auto* searchPane = new QWidget;
        auto* sp = new QVBoxLayout(searchPane);
        auto* findBox = new QLineEdit;
        findBox->setPlaceholderText("Search in file…");
        sp->addWidget(findBox);
        connect(findBox, &QLineEdit::returnPressed, this, [this, findBox]{
            lastFind_ = findBox->text();
            if (auto* ed = currentEditor()) ed->find(lastFind_);
        });

        sideStack_ = new QStackedWidget;
        sideStack_->addWidget(explorer_);
        sideStack_->addWidget(searchPane);

        // --- Center: tabs + welcome ---
        tabs_ = new QTabWidget;
        tabs_->setTabsClosable(true);
        tabs_->setDocumentMode(true);
        connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int i){
            QWidget* w = tabs_->widget(i);
            tabs_->removeTab(i);
            delete w;
            if (tabs_->count() == 0) showWelcome();
        });
        welcome_ = buildWelcome();
        tabs_->addTab(welcome_, "Welcome");

        // --- Bottom: terminal/output ---
        auto* bottom = new QWidget;
        auto* bl = new QVBoxLayout(bottom);
        bl->setContentsMargins(0, 0, 0, 0);
        auto* botTabs = new QTabWidget;
        output_ = new ZoomEdit;
        output_->setReadOnly(true);
        botTabs->addTab(output_, "Output");
        auto* termPage = new QWidget;
        auto* tl = new QVBoxLayout(termPage);
        termIn_ = new QLineEdit;
        termIn_->setPlaceholderText("stdin for input()/read_int() — Enter to send");
        tl->addWidget(termIn_);
        botTabs->addTab(termPage, "Terminal");
        bl->addWidget(botTabs);
        connect(termIn_, &QLineEdit::returnPressed, this, [this]{
            if (!proc_) return;
            proc_->write((termIn_->text() + "\n").toUtf8());
            output_->appendPlainText("> " + termIn_->text());
            termIn_->clear();
        });

        auto* centerSplit = new QSplitter(Qt::Vertical);
        centerSplit->addWidget(tabs_);
        centerSplit->addWidget(bottom);
        centerSplit->setStretchFactor(0, 4);
        centerSplit->setStretchFactor(1, 1);

        auto* midSplit = new QSplitter(Qt::Horizontal);
        midSplit->addWidget(sideStack_);
        midSplit->addWidget(centerSplit);
        midSplit->setStretchFactor(0, 0);
        midSplit->setStretchFactor(1, 1);
        midSplit->setSizes({220, 900});

        auto* root = new QWidget;
        auto* rootLay = new QHBoxLayout(root);
        rootLay->setContentsMargins(0, 0, 0, 0);
        rootLay->setSpacing(0);
        rootLay->addWidget(activity);
        rootLay->addWidget(midSplit, 1);
        setCentralWidget(root);

        buildMenus();
        applyTheme();
        statusBar()->showMessage(QString("Vayu-IDE %1").arg(kVer));

        QString lastFolder = settings_.value("lastFolder").toString();
        if (!lastFolder.isEmpty() && QDir(lastFolder).exists())
            openFolderPath(lastFolder);
        else {
            QString last = settings_.value("lastFile").toString();
            if (!last.isEmpty() && QFile::exists(last)) openFilePath(last);
        }

        auto* scWrap = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Z), this);
        connect(scWrap, &QShortcut::activated, this, [this]{ toggleWrap(); });
    }

    ~MainWindow() override {
        if (proc_) { proc_->kill(); proc_->waitForFinished(300); }
    }

private:
    void showExplorer() { sideStack_->setCurrentIndex(0); }
    void showSearch() { sideStack_->setCurrentIndex(1); }
    void showGitComingSoon() {
        QMessageBox::information(this, "Source Control", "Coming soon.");
    }
    void showSettings() {
        QMessageBox::information(this, "Settings",
            QString("Data: %%AppData%%/VayuIDE\nVersion: %1\n%2").arg(kVer).arg(kRepo));
    }

private:
    void applyTheme() {
        qApp->setStyleSheet(
            "QMainWindow,QWidget{background:#1e1e2e;color:#cdd6f4;}"
            "#activity{background:#11111b;}"
            "QToolButton{background:#11111b;color:#89b4fa;border:none;border-radius:8px;font-weight:bold;}"
            "QToolButton:hover{background:#313244;}"
            "QTreeWidget,QPlainTextEdit,QLineEdit,QListWidget,QTextEdit{"
            " background:#181825;color:#cdd6f4;border:1px solid #313244;}"
            "QTabWidget::pane{border:1px solid #313244;}"
            "QTabBar::tab{background:#181825;color:#a6adc8;padding:8px 14px;}"
            "QTabBar::tab:selected{background:#313244;color:#cdd6f4;}"
            "QMenuBar{background:#11111b;} QMenu{background:#181825;}"
            "QStatusBar{background:#89b4fa;color:#11111b;}"
            "QPushButton{background:#89b4fa;color:#11111b;border:none;padding:8px 14px;border-radius:6px;}"
            "QSplitter::handle{background:#313244;}");
    }

    QWidget* buildWelcome() {
        auto* w = new QWidget;
        auto* lay = new QVBoxLayout(w);
        auto* title = new QLabel(QString("<h1>Vayu IDE</h1><p>Code · Build · Create · %1</p>").arg(kVer));
        title->setAlignment(Qt::AlignCenter);
        lay->addWidget(title);
        auto* row = new QHBoxLayout;
        auto* qs = new QVBoxLayout;
        auto* bNew = new QPushButton("New File…");
        auto* bOpen = new QPushButton("Open Folder…");
        auto* bRepo = new QPushButton("GitHub Repository");
        qs->addWidget(new QLabel("<b>Quick Start</b>"));
        qs->addWidget(bNew);
        qs->addWidget(bOpen);
        qs->addWidget(bRepo);
        auto* doc = new QVBoxLayout;
        doc->addWidget(new QLabel("<b>Documentation</b>"));
        auto* bHelp = new QPushButton("Syntax Help");
        auto* bEx = new QPushButton("Load Examples");
        doc->addWidget(bHelp);
        doc->addWidget(bEx);
        row->addLayout(qs);
        row->addLayout(doc);
        lay->addLayout(row);
        lay->addStretch();
        connect(bNew, &QPushButton::clicked, this, [this]{ newFile(); });
        connect(bOpen, &QPushButton::clicked, this, [this]{ openFolder(); });
        connect(bRepo, &QPushButton::clicked, this, []{ QDesktopServices::openUrl(QUrl(kRepo)); });
        connect(bHelp, &QPushButton::clicked, this, [this]{ showHelpSyntax(); });
        connect(bEx, &QPushButton::clicked, this, [this]{ showHelpExamples(); });
        return w;
    }

    void buildMenus() {
        auto* m = menuBar();
        auto add = [this](QMenu* menu, const QString& t, QKeySequence ks, auto fn) {
            QAction* a = menu->addAction(t);
            a->setShortcut(ks);
            connect(a, &QAction::triggered, this, fn);
        };
        auto* file = m->addMenu("&File");
        add(file, "&New File", QKeySequence::New, [this]{ newFile(); });
        add(file, "Open &File…", QKeySequence::Open, [this]{ openFile(); });
        add(file, "Open F&older…", QKeySequence(), [this]{ openFolder(); });
        add(file, "&Save", QKeySequence::Save, [this]{ saveCurrent(); });
        file->addSeparator();
        add(file, "E&xit", QKeySequence::Quit, [this]{ close(); });

        auto* edit = m->addMenu("&Edit");
        add(edit, "&Undo", QKeySequence::Undo, [this]{ if (auto* e=currentEditor()) e->undo(); });
        add(edit, "&Redo", QKeySequence::Redo, [this]{ if (auto* e=currentEditor()) e->redo(); });
        edit->addSeparator();
        add(edit, "Select &All", QKeySequence::SelectAll, [this]{ if (auto* e=currentEditor()) e->selectAll(); });
        add(edit, "Cu&t", QKeySequence::Cut, [this]{ if (auto* e=currentEditor()) e->cut(); });
        add(edit, "&Copy", QKeySequence::Copy, [this]{ if (auto* e=currentEditor()) e->copy(); });
        add(edit, "&Paste", QKeySequence::Paste, [this]{ if (auto* e=currentEditor()) e->paste(); });
        edit->addSeparator();
        add(edit, "&Find…", QKeySequence::Find, [this]{ findInEditor(); });
        add(edit, "Word wrap (Alt+Z)", QKeySequence(), [this]{ toggleWrap(); });

        auto* run = m->addMenu("&Run");
        add(run, "Run --native", QKeySequence(Qt::Key_F5), [this]{ runArgs({"--native"}); });
        add(run, "Run interpreter", QKeySequence(), [this]{ runArgs({}); });
        add(run, "Run --vm", QKeySequence(), [this]{ runArgs({"--vm"}); });
        add(run, "Dump IR", QKeySequence(), [this]{ runArgs({"--dump-ir"}); });
        add(run, "Stop", QKeySequence(), [this]{ if (proc_) proc_->kill(); });

        auto* compile = m->addMenu("&Compile");
        add(compile, "Emit .vye", QKeySequence(Qt::Key_F7), [this]{ emitVye(); });
        add(compile, "Run .vye…", QKeySequence(), [this]{ runVye(); });
        add(compile, "Create host .exe (MSVC)…", QKeySequence(), [this]{ emitHostExe(); });

        auto* help = m->addMenu("&Help");
        add(help, "&Syntax (detailed)…", QKeySequence(), [this]{ showHelpSyntax(); });
        add(help, "Load &Examples…", QKeySequence(), [this]{ showHelpExamples(); });
        add(help, "&Version", QKeySequence(), [this]{
            QMessageBox::information(this, "Version",
                QString("Vayu-IDE %1\nVayu %1 Release\n%2").arg(kVer).arg(kRepo));
        });
        add(help, "Open &Repo", QKeySequence(), []{ QDesktopServices::openUrl(QUrl(kRepo)); });
    }

    ZoomEdit* currentEditor() {
        QWidget* w = tabs_->currentWidget();
        if (!w || w == welcome_) return nullptr;
        if (!w->property("vayuEditor").toBool()) return nullptr;
        return static_cast<ZoomEdit*>(w);
    }
    QString currentPath() const {
        int i = tabs_->currentIndex();
        if (i < 0) return {};
        return tabs_->tabToolTip(i);
    }

    void showWelcome() {
        if (tabs_->indexOf(welcome_) < 0)
            tabs_->addTab(welcome_, "Welcome");
        tabs_->setCurrentWidget(welcome_);
    }

    void newFile() {
        auto* ed = new ZoomEdit;
        QFont mono("Cascadia Mono", 11);
        if (!mono.exactMatch()) mono = QFont("Consolas", 11);
        ed->setFont(mono);
        ed->setProperty("vayuEditor", true);
        int i = tabs_->addTab(ed, "untitled.vyu");
        tabs_->setTabToolTip(i, "");
        tabs_->setCurrentIndex(i);
    }

    void openFile() {
        QString f = QFileDialog::getOpenFileName(this, "Open", folder_, "Vayu (*.vyu);;All (*.*)");
        if (!f.isEmpty()) openFilePath(f);
    }
    void openFilePath(const QString& path) {
        // reuse tab if open
        for (int i = 0; i < tabs_->count(); ++i) {
            if (tabs_->tabToolTip(i) == path) {
                tabs_->setCurrentIndex(i);
                return;
            }
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        auto* ed = new ZoomEdit;
        QFont mono("Cascadia Mono", 11);
        if (!mono.exactMatch()) mono = QFont("Consolas", 11);
        ed->setFont(mono);
        ed->setPlainText(QString::fromUtf8(file.readAll()));
        ed->setProperty("vayuEditor", true);
        int i = tabs_->addTab(ed, QFileInfo(path).fileName());
        tabs_->setTabToolTip(i, path);
        tabs_->setCurrentIndex(i);
        settings_.setValue("lastFile", path);
    }

    void openFolder() {
        QString d = QFileDialog::getExistingDirectory(this, "Open Folder", folder_);
        if (!d.isEmpty()) openFolderPath(d);
    }
    void openFolderPath(const QString& d) {
        folder_ = d;
        settings_.setValue("lastFolder", d);
        explorer_->clear();
        auto* root = new QTreeWidgetItem(explorer_, QStringList{QFileInfo(d).fileName()});
        root->setData(0, Qt::UserRole, d);
        populateTree(root, d);
        root->setExpanded(true);
        showExplorer();
    }
    void populateTree(QTreeWidgetItem* parent, const QString& dir) {
        QDir d(dir);
        for (const QFileInfo& fi : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            auto* it = new QTreeWidgetItem(parent, QStringList{fi.fileName()});
            it->setData(0, Qt::UserRole, fi.absoluteFilePath());
            populateTree(it, fi.absoluteFilePath());
        }
        for (const QFileInfo& fi : d.entryInfoList(QStringList{"*.vyu", "*.md", "*.txt", "*.ps1", "*.cmd", "*.sh"},
                                                   QDir::Files, QDir::Name)) {
            auto* it = new QTreeWidgetItem(parent, QStringList{fi.fileName()});
            it->setData(0, Qt::UserRole, fi.absoluteFilePath());
        }
    }
    void openTreeItem(QTreeWidgetItem* item, int) {
        QString p = item->data(0, Qt::UserRole).toString();
        if (QFileInfo(p).isFile()) openFilePath(p);
    }

    bool saveCurrent() {
        auto* ed = currentEditor();
        if (!ed) return false;
        QString path = currentPath();
        if (path.isEmpty()) {
            path = QFileDialog::getSaveFileName(this, "Save", folder_, "Vayu (*.vyu)");
            if (path.isEmpty()) return false;
            int i = tabs_->currentIndex();
            tabs_->setTabToolTip(i, path);
            tabs_->setTabText(i, QFileInfo(path).fileName());
        }
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
        f.write(ed->toPlainText().toUtf8());
        settings_.setValue("lastFile", path);
        statusBar()->showMessage("Saved " + path);
        return true;
    }

    QString vayuBin() const {
        QDir d(QCoreApplication::applicationDirPath());
        QString p = d.filePath("vayu.exe");
        if (QFile::exists(p)) return p;
        return "vayu.exe";
    }

    void runArgs(const QStringList& extra) {
        if (proc_) {
            QMessageBox::information(this, "Busy", "Already running.");
            return;
        }
        if (!saveCurrent()) return;
        QString path = currentPath();
        if (path.isEmpty()) return;
        output_->clear();
        output_->appendPlainText(QString("[run %1]").arg(extra.join(' ')));
        proc_ = new QProcess(this);
        proc_->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
        proc_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
        });
#endif
        connect(proc_, &QProcess::readyRead, this, [this]{
            output_->appendPlainText(QString::fromUtf8(proc_->readAll()));
        });
        connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
                output_->appendPlainText(QString("—— exit %1 ——").arg(code));
                proc_->deleteLater();
                proc_ = nullptr;
            });
        QStringList args;
        args << path << extra;
        proc_->start(vayuBin(), args);
        if (!proc_->waitForStarted(4000)) {
            output_->appendPlainText("[error] cannot start vayu.exe");
            proc_->deleteLater();
            proc_ = nullptr;
        }
    }

    void emitVye() {
        if (!saveCurrent()) return;
        QString path = currentPath();
        QString out = path;
        if (out.endsWith(".vyu")) out = out.left(out.size() - 4) + ".vye";
        else out += ".vye";
        runArgs({"--output", out});
    }
    void runVye() {
        QString f = QFileDialog::getOpenFileName(this, "Run .vye", folder_, "Vye (*.vye)");
        if (f.isEmpty()) return;
        output_->clear();
        proc_ = new QProcess(this);
        proc_->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
        proc_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
        });
#endif
        connect(proc_, &QProcess::readyRead, this, [this]{
            output_->appendPlainText(QString::fromUtf8(proc_->readAll()));
        });
        connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int c, QProcess::ExitStatus) {
                output_->appendPlainText(QString("—— exit %1 ——").arg(c));
                proc_->deleteLater(); proc_ = nullptr;
            });
        proc_->start(vayuBin(), QStringList{f});
    }

    void emitHostExe() {
        // Real PE via host MSVC cl.exe (same architecture as this PC)
        if (!saveCurrent()) return;
        QString path = currentPath();
        QString base = path;
        if (base.endsWith(".vyu")) base = base.left(base.size() - 4);
        QString vye = base + ".vye";
        QString exe = base + ".exe";
        QString cfile = base + "_launch.c";

        QProcess emitP;
#ifdef Q_OS_WIN
        emitP.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
        });
#endif
        emitP.start(vayuBin(), QStringList{path, "--output", vye});
        emitP.waitForFinished(60000);
        output_->appendPlainText(QString::fromUtf8(emitP.readAllStandardOutput()));
        if (emitP.exitCode() != 0) {
            output_->appendPlainText("[emit-vye failed]");
            return;
        }

        QString vyeName = QFileInfo(vye).fileName();
        QString src = QString(
            "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
            "#ifdef _WIN32\n#include <windows.h>\n#endif\n"
            "int main(void){\n"
            "  const char* vye=\"%1\";\n"
            "  const char* vayu=\"vayu.exe\";\n"
            "  char cmd[2048];\n"
            "#ifdef _WIN32\n"
            "  char self[MAX_PATH]; GetModuleFileNameA(NULL,self,MAX_PATH);\n"
            "  char* s=strrchr(self,'\\\\'); if(s) *(s+1)=0; else self[0]=0;\n"
            "  snprintf(cmd,sizeof(cmd),\"\\\"%%s%%s\\\" \\\"%%s%%s\\\"\",self,vayu,self,vye);\n"
            "#else\n"
            "  snprintf(cmd,sizeof(cmd),\"\\\"%%s\\\" \\\"%%s\\\"\",vayu,vye);\n"
            "#endif\n"
            "  return system(cmd);\n}\n").arg(vyeName);
        QFile cf(cfile);
        if (!cf.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        cf.write(src.toUtf8());
        cf.close();

        QProcess cl;
        cl.setWorkingDirectory(QFileInfo(path).absolutePath());
#ifdef Q_OS_WIN
        cl.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
        });
#endif
        // Host cl.exe → matching machine arch
        cl.start("cl", QStringList{"/nologo", "/O2", "/Fe:" + QFileInfo(exe).fileName(), QFileInfo(cfile).fileName()});
        if (!cl.waitForStarted(3000)) {
            output_->appendPlainText(
                "[exe] cl.exe not on PATH. Open \"x64 Native Tools Command Prompt\" or add MSVC to PATH, then retry.\n"
                "Generated: " + cfile + " + " + vye);
            return;
        }
        cl.waitForFinished(120000);
        output_->appendPlainText(QString::fromUtf8(cl.readAllStandardOutput()));
        output_->appendPlainText(QString::fromUtf8(cl.readAllStandardError()));
        if (cl.exitCode() == 0)
            output_->appendPlainText("[exe] Host PE built: " + exe + " (architecture = this PC's toolchain)");
        else
            output_->appendPlainText("[exe] cl failed — ensure MSVC x64/x86 tools match your OS.");
    }

    void toggleWrap() {
        wrap_ = !wrap_;
        auto mode = wrap_ ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap;
        if (auto* e = currentEditor()) e->setLineWrapMode(mode);
        output_->setLineWrapMode(mode);
        statusBar()->showMessage(wrap_ ? "Word wrap ON" : "Word wrap OFF");
    }
    void findInEditor() {
        bool ok = false;
        QString s = QInputDialog::getText(this, "Find", "Text:", QLineEdit::Normal, lastFind_, &ok);
        if (!ok || s.isEmpty()) return;
        lastFind_ = s;
        if (auto* e = currentEditor()) e->find(s);
    }

    void showHelpSyntax() {
        QDialog dlg(this);
        dlg.setWindowTitle("Vayu Syntax — detailed");
        dlg.resize(720, 520);
        auto* t = new QTextEdit(&dlg);
        t->setReadOnly(true);
        QFile f;
        // Embed summary from Implementation brief
        t->setMarkdown(
            "# Vayu syntax (Integrator)\n\n"
            "## Compare / if\n`if c == \"name\":` — use `==` and quotes; indent body.\n\n"
            "## Strings\n`\"a,b\".split(\",\")` · `\",\".join([\"a\",\"b\"])` · upper/lower/strip/…\n\n"
            "## Membership\n`2 in [1,2,3]` · `\"ab\" in \"xabz\"`\n\n"
            "## Lists\n`xs.append(3)` · `xs[0]` · `len(xs)`\n\n"
            "## Loops\n`for i in range(5):` · `range(2,6)` · `while` · `break`\n\n"
            "## Shell (cmd / PowerShell / sh text)\n"
            "```\ncode = shell(\"cmd /c echo hi\")\n"
            "shell(\"powershell -NoProfile -Command \\\"Write-Host 1\\\"\")\n```\n\n"
            "## Functions\n`def f(a,b): return a+b`\n\n"
            "Full brief: `VIB/Docs/Implementation.md`\n"
            "Repo: " + QString(kRepo));
        auto* lay = new QVBoxLayout(&dlg);
        lay->addWidget(t);
        dlg.exec();
    }
    void showHelpExamples() {
        QDialog dlg(this);
        dlg.setWindowTitle("Load example");
        dlg.resize(480, 360);
        auto* list = new QListWidget(&dlg);
        for (const auto& e : examples()) list->addItem(e.title);
        auto* btn = new QPushButton("Load into new tab");
        auto* lay = new QVBoxLayout(&dlg);
        lay->addWidget(list);
        lay->addWidget(btn);
        connect(btn, &QPushButton::clicked, this, [this, list, &dlg]{
            int r = list->currentRow();
            if (r < 0) return;
            newFile();
            if (auto* ed = currentEditor())
                ed->setPlainText(examples()[r].code);
            dlg.accept();
        });
        dlg.exec();
    }
};

// MOC: for slots without full moc when single file — use functor-only if needed.

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Vayu-IDE");
    QApplication::setApplicationVersion(kVer);
    QApplication::setOrganizationName("VayuIDE");
    MainWindow w;
    w.show();
    return app.exec();
}

