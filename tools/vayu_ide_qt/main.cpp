// Vayu-IDE V0.2-Alpha — VSCode-like UI, selectable terminal, host .exe without manual MSVC setup
#include <QApplication>
#include <QMainWindow>
#include <QSplitter>
#include <QTreeWidget>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QTextEdit>
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
#include <QClipboard>
#include <QGuiApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QScrollBar>
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
        {"Loops",
         "for i in range(5):\n    print(i)\nfor i in range(2, 6):\n    print(i)\nfor x in [10, 20, 30]:\n    print(x)\n"},
        {"shell cmd",
         "print(shell(\"cmd /c echo hello\"))\n"},
        {"shell powershell",
         "shell(\"powershell -NoProfile -Command \\\"Write-Host hi\\\"\")\n"},
        {"functions",
         "def add(a, b):\n    return a + b\nprint(add(2, 40))\n"},
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
    void contextMenuEvent(QContextMenuEvent* e) override {
        QMenu* m = createStandardContextMenu();
        m->addSeparator();
        QAction* copyAll = m->addAction("Copy All");
        connect(copyAll, &QAction::triggered, this, [this]{
            QGuiApplication::clipboard()->setText(toPlainText());
        });
        m->exec(e->globalPos());
        delete m;
    }
};

class MainWindow : public QMainWindow {
    QTreeWidget* explorer_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    ZoomEdit* output_ = nullptr;
    ZoomEdit* terminal_ = nullptr;
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
        resize(1360, 860);
        QIcon ic(":/IDE.ico");
        if (!ic.isNull()) setWindowIcon(ic);

        // Activity bar
        auto* activity = new QFrame;
        activity->setObjectName("activity");
        activity->setFixedWidth(52);
        auto* actLay = new QVBoxLayout(activity);
        actLay->setContentsMargins(6, 10, 6, 10);
        actLay->setSpacing(10);
        auto mkAct = [&](const QString& glyph, const QString& tip, auto fn) {
            auto* b = new QToolButton;
            b->setText(glyph);
            b->setToolTip(tip);
            b->setFixedSize(40, 40);
            b->setStyleSheet("QToolButton{font-size:18px;border-radius:8px;}"
                             "QToolButton:hover{background:#313244;}");
            connect(b, &QToolButton::clicked, this, fn);
            actLay->addWidget(b);
            return b;
        };
        mkAct(QString::fromUtf8("\xF0\x9F\x93\x81"), "Explorer", [this]{ sideStack_->setCurrentIndex(0); });
        mkAct(QString::fromUtf8("\xF0\x9F\x94\x8D"), "Search", [this]{ sideStack_->setCurrentIndex(1); });
        mkAct(QString::fromUtf8("\xF0\x9F\x93\x9A"), "Source Control", [this]{
            QMessageBox::information(this, "Source Control", "Coming soon.");
        });
        // no Extensions
        actLay->addStretch();
        mkAct(QString::fromUtf8("\xE2\x9A\x99"), "Settings", [this]{
            QMessageBox::information(this, "Settings",
                QString("Vayu-IDE %1\nAppData: VayuIDE\n%2").arg(kVer).arg(kRepo));
        });

        explorer_ = new QTreeWidget;
        explorer_->setHeaderLabel("EXPLORER");
        connect(explorer_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int){
            QString p = it->data(0, Qt::UserRole).toString();
            if (QFileInfo(p).isFile()) openFilePath(p);
        });

        auto* searchPane = new QWidget;
        auto* sp = new QVBoxLayout(searchPane);
        auto* findBox = new QLineEdit;
        findBox->setPlaceholderText("Search in current file…");
        sp->addWidget(findBox);
        connect(findBox, &QLineEdit::returnPressed, this, [this, findBox]{
            lastFind_ = findBox->text();
            if (auto* ed = currentEditor()) ed->find(lastFind_);
        });

        sideStack_ = new QStackedWidget;
        sideStack_->addWidget(explorer_);
        sideStack_->addWidget(searchPane);

        tabs_ = new QTabWidget;
        tabs_->setTabsClosable(true);
        tabs_->setDocumentMode(true);
        tabs_->setMovable(true);
        connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int i){
            QWidget* w = tabs_->widget(i);
            tabs_->removeTab(i);
            if (w != welcome_) delete w;
            if (tabs_->count() == 0) showWelcome();
        });
        welcome_ = buildWelcome();
        tabs_->addTab(welcome_, "Welcome");

        // Bottom panel: Output + Terminal (both selectable / copyable)
        auto* botTabs = new QTabWidget;
        output_ = new ZoomEdit;
        output_->setReadOnly(true);
        output_->setUndoRedoEnabled(false);
        output_->setPlaceholderText("Run output appears here — select text and Ctrl+C to copy");
        botTabs->addTab(output_, "Output");

        auto* termPage = new QWidget;
        auto* tl = new QVBoxLayout(termPage);
        tl->setContentsMargins(0, 0, 0, 0);
        terminal_ = new ZoomEdit;
        terminal_->setReadOnly(true);
        terminal_->setPlaceholderText("Terminal console — selectable, copy with Ctrl+C");
        termIn_ = new QLineEdit;
        termIn_->setPlaceholderText("Type input for input()/read_int() and press Enter");
        tl->addWidget(terminal_, 1);
        tl->addWidget(termIn_);
        botTabs->addTab(termPage, "Terminal");
        connect(termIn_, &QLineEdit::returnPressed, this, [this]{
            QString line = termIn_->text();
            termIn_->clear();
            terminal_->appendPlainText(line);
            if (proc_ && proc_->state() == QProcess::Running) {
                proc_->write((line + "\n").toUtf8());
            }
        });

        auto* centerSplit = new QSplitter(Qt::Vertical);
        centerSplit->addWidget(tabs_);
        centerSplit->addWidget(botTabs);
        centerSplit->setStretchFactor(0, 3);
        centerSplit->setStretchFactor(1, 1);
        centerSplit->setSizes({560, 240});

        auto* midSplit = new QSplitter(Qt::Horizontal);
        midSplit->addWidget(sideStack_);
        midSplit->addWidget(centerSplit);
        midSplit->setSizes({240, 1000});

        auto* root = new QWidget;
        auto* rootLay = new QHBoxLayout(root);
        rootLay->setContentsMargins(0, 0, 0, 0);
        rootLay->setSpacing(0);
        rootLay->addWidget(activity);
        rootLay->addWidget(midSplit, 1);
        setCentralWidget(root);

        buildMenus();
        applyTheme();
        statusBar()->showMessage(QString("Vayu-IDE %1  ·  F5 Run  ·  Ctrl+C copy output").arg(kVer));

        QString lastFolder = settings_.value("lastFolder").toString();
        if (!lastFolder.isEmpty() && QDir(lastFolder).exists())
            openFolderPath(lastFolder);

        auto* scWrap = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Z), this);
        connect(scWrap, &QShortcut::activated, this, [this]{ toggleWrap(); });
    }

    ~MainWindow() override {
        if (proc_) { proc_->kill(); proc_->waitForFinished(400); }
    }

private:
    void applyTheme() {
        qApp->setStyleSheet(
            "QMainWindow,QWidget{background:#1e1e2e;color:#cdd6f4;}"
            "#activity{background:#11111b;border-right:1px solid #313244;}"
            "QTreeWidget,QPlainTextEdit,QLineEdit,QListWidget,QTextEdit{"
            " background:#181825;color:#cdd6f4;border:none;selection-background-color:#45475a;}"
            "QTabWidget::pane{border-top:1px solid #313244;}"
            "QTabBar::tab{background:#181825;color:#a6adc8;padding:8px 16px;margin-right:2px;}"
            "QTabBar::tab:selected{background:#313244;color:#cdd6f4;}"
            "QMenuBar{background:#11111b;color:#cdd6f4;}"
            "QMenu{background:#181825;color:#cdd6f4;}"
            "QMenu::item:selected{background:#45475a;}"
            "QStatusBar{background:#89b4fa;color:#11111b;}"
            "QPushButton{background:#89b4fa;color:#11111b;border:none;padding:8px 14px;border-radius:6px;}"
            "QPushButton:hover{background:#b4befe;}"
            "QSplitter::handle{background:#313244;}"
            "QHeaderView::section{background:#11111b;color:#a6adc8;border:none;padding:6px;}");
        QFont mono("Cascadia Mono", 12);
        if (!mono.exactMatch()) mono = QFont("Consolas", 12);
        output_->setFont(mono);
        terminal_->setFont(mono);
    }

    QWidget* buildWelcome() {
        auto* w = new QWidget;
        auto* lay = new QVBoxLayout(w);
        auto* title = new QLabel(
            QString("<div style='text-align:center'>"
                    "<h1 style='color:#89b4fa'>Vayu IDE</h1>"
                    "<p style='color:#a6adc8'>Code · Build · Create · %1</p></div>").arg(kVer));
        lay->addWidget(title);
        auto* row = new QHBoxLayout;
        auto col = [&](const QString& h) {
            auto* v = new QVBoxLayout;
            v->addWidget(new QLabel(QString("<b>%1</b>").arg(h)));
            return v;
        };
        auto* qs = col("Quick Start");
        auto* b1 = new QPushButton("New File…");
        auto* b2 = new QPushButton("Open Folder…");
        auto* b3 = new QPushButton("GitHub Repository");
        qs->addWidget(b1); qs->addWidget(b2); qs->addWidget(b3);
        auto* doc = col("Help");
        auto* b4 = new QPushButton("Syntax (detailed)");
        auto* b5 = new QPushButton("Load Examples");
        doc->addWidget(b4); doc->addWidget(b5);
        row->addLayout(qs); row->addLayout(doc);
        lay->addLayout(row);
        lay->addStretch();
        connect(b1, &QPushButton::clicked, this, [this]{ newFile(); });
        connect(b2, &QPushButton::clicked, this, [this]{ openFolder(); });
        connect(b3, &QPushButton::clicked, this, []{ QDesktopServices::openUrl(QUrl(kRepo)); });
        connect(b4, &QPushButton::clicked, this, [this]{ showHelpSyntax(); });
        connect(b5, &QPushButton::clicked, this, [this]{ showHelpExamples(); });
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
        add(compile, "Create host .exe…", QKeySequence(), [this]{ emitHostExe(); });

        auto* help = m->addMenu("&Help");
        add(help, "&Syntax (detailed)…", QKeySequence(), [this]{ showHelpSyntax(); });
        add(help, "Load &Examples…", QKeySequence(), [this]{ showHelpExamples(); });
        add(help, "&Version", QKeySequence(), [this]{
            QMessageBox::information(this, "Version",
                QString("Vayu-IDE %1\nVayu %1\n%2").arg(kVer).arg(kRepo));
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
        if (tabs_->indexOf(welcome_) < 0) tabs_->addTab(welcome_, "Welcome");
        tabs_->setCurrentWidget(welcome_);
    }
    void newFile() {
        auto* ed = new ZoomEdit;
        ed->setProperty("vayuEditor", true);
        ed->setFont(output_->font());
        int i = tabs_->addTab(ed, "untitled.vyu");
        tabs_->setTabToolTip(i, "");
        tabs_->setCurrentIndex(i);
    }
    void openFile() {
        QString f = QFileDialog::getOpenFileName(this, "Open", folder_, "Vayu (*.vyu);;All (*.*)");
        if (!f.isEmpty()) openFilePath(f);
    }
    void openFilePath(const QString& path) {
        for (int i = 0; i < tabs_->count(); ++i) {
            if (tabs_->tabToolTip(i) == path) { tabs_->setCurrentIndex(i); return; }
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        auto* ed = new ZoomEdit;
        ed->setProperty("vayuEditor", true);
        ed->setFont(output_->font());
        ed->setPlainText(QString::fromUtf8(file.readAll()));
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
        sideStack_->setCurrentIndex(0);
    }
    void populateTree(QTreeWidgetItem* parent, const QString& dir) {
        QDir d(dir);
        for (const QFileInfo& fi : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            auto* it = new QTreeWidgetItem(parent, QStringList{fi.fileName()});
            it->setData(0, Qt::UserRole, fi.absoluteFilePath());
            populateTree(it, fi.absoluteFilePath());
        }
        for (const QFileInfo& fi : d.entryInfoList(
                 QStringList{"*.vyu", "*.md", "*.txt", "*.ps1", "*.cmd", "*.sh", "*.vye"},
                 QDir::Files, QDir::Name)) {
            auto* it = new QTreeWidgetItem(parent, QStringList{fi.fileName()});
            it->setData(0, Qt::UserRole, fi.absoluteFilePath());
        }
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

    void appendOut(const QString& s) {
        output_->appendPlainText(s);
        terminal_->appendPlainText(s);
        output_->verticalScrollBar()->setValue(output_->verticalScrollBar()->maximum());
    }

    void runArgs(const QStringList& extra) {
        if (proc_) { QMessageBox::information(this, "Busy", "Already running — Run → Stop."); return; }
        if (!saveCurrent()) return;
        QString path = currentPath();
        if (path.isEmpty()) return;
        output_->clear();
        terminal_->appendPlainText(QString("\n$ vayu %1 %2").arg(path, extra.join(' ')));
        appendOut(QString("[run %1]").arg(extra.join(' ')));
        proc_ = new QProcess(this);
        proc_->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
        proc_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
        });
#endif
        connect(proc_, &QProcess::readyRead, this, [this]{
            QString chunk = QString::fromUtf8(proc_->readAll());
            output_->moveCursor(QTextCursor::End);
            output_->insertPlainText(chunk);
            terminal_->moveCursor(QTextCursor::End);
            terminal_->insertPlainText(chunk);
        });
        connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
                appendOut(QString("—— exit %1 ——").arg(code));
                proc_->deleteLater();
                proc_ = nullptr;
            });
        QStringList args; args << path << extra;
        proc_->start(vayuBin(), args);
        if (!proc_->waitForStarted(4000)) {
            appendOut("[error] cannot start vayu.exe");
            proc_->deleteLater();
            proc_ = nullptr;
        }
    }

    void emitVye() {
        if (!saveCurrent()) return;
        QString path = currentPath();
        QString out = path.endsWith(".vyu") ? path.left(path.size()-4)+".vye" : path+".vye";
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
            appendOut(QString::fromUtf8(proc_->readAll()));
        });
        connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int c, QProcess::ExitStatus) {
                appendOut(QString("—— exit %1 ——").arg(c));
                proc_->deleteLater(); proc_ = nullptr;
            });
        proc_->start(vayuBin(), QStringList{f});
    }

    // Host .exe without requiring user to open MSVC prompt:
    // 1) emit .vye  2) write launcher .c  3) find VS via vswhere + vcvars64  OR gcc/clang
    void emitHostExe() {
        if (!saveCurrent()) return;
        QString path = currentPath();
        QString dir = QFileInfo(path).absolutePath();
        QString base = QFileInfo(path).completeBaseName();
        QString vye = dir + "/" + base + ".vye";
        QString exe = dir + "/" + base + ".exe";
        QString cfile = dir + "/" + base + "_launch.c";

        QProcess emitP;
#ifdef Q_OS_WIN
        emitP.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
        });
#endif
        emitP.start(vayuBin(), QStringList{path, "--output", vye});
        emitP.waitForFinished(60000);
        appendOut(QString::fromUtf8(emitP.readAllStandardOutput()));
        if (emitP.exitCode() != 0) {
            appendOut("[emit-vye failed]");
            return;
        }

        QString vyeName = base + ".vye";
        QString src = QString(
            "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
            "#ifdef _WIN32\n#include <windows.h>\n#endif\n"
            "int main(void){\n"
            "  char cmd[1024];\n"
            "#ifdef _WIN32\n"
            "  char self[MAX_PATH]; GetModuleFileNameA(NULL,self,MAX_PATH);\n"
            "  char* s=strrchr(self,'\\\\'); if(s) *(s+1)=0; else self[0]=0;\n"
            "  snprintf(cmd,sizeof(cmd),\"\\\"%%s%s\\\" \\\"%%s%s\\\"\",self,self);\n"
            "#else\n"
            "  snprintf(cmd,sizeof(cmd),\"vayu \\\"%s\\\"\");\n"
            "#endif\n"
            "  return system(cmd);\n}\n")
            .arg(QStringLiteral("vayu.exe"), vyeName, vyeName);

        // Fix format string carefully
        src = QString(
            "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
            "#ifdef _WIN32\n#include <windows.h>\n#endif\n"
            "int main(void){\n"
            "  const char* vye = \"%1\";\n"
            "  char cmd[1024];\n"
            "#ifdef _WIN32\n"
            "  char self[MAX_PATH]; GetModuleFileNameA(NULL,self,MAX_PATH);\n"
            "  char* s=strrchr(self,'\\\\'); if(s) *(s+1)=0; else self[0]=0;\n"
            "  snprintf(cmd,sizeof(cmd),\"\\\"%%svayu.exe\\\" \\\"%%s%%s\\\"\", self, self, vye);\n"
            "#else\n"
            "  snprintf(cmd,sizeof(cmd),\"vayu \\\"%%s\\\"\", vye);\n"
            "#endif\n"
            "  return system(cmd);\n}\n").arg(vyeName);

        QFile cf(cfile);
        if (!cf.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        cf.write(src.toUtf8());
        cf.close();

#ifdef Q_OS_WIN
        // Locate VS with vswhere, then cl under vcvars64 (fixes "stdio.h no include path")
        QString vswhere = QString(qEnvironmentVariable("ProgramFiles(x86)"))
            + "/Microsoft Visual Studio/Installer/vswhere.exe";
        QString clCmd;
        if (QFile::exists(vswhere)) {
            QProcess vw;
            vw.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
                a->flags |= CREATE_NO_WINDOW;
            });
            vw.start(vswhere, QStringList{
                "-latest", "-products", "*",
                "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-property", "installationPath"});
            vw.waitForFinished(15000);
            QString install = QString::fromUtf8(vw.readAllStandardOutput()).trimmed();
            if (!install.isEmpty()) {
                QString vcvars = install + "/VC/Auxiliary/Build/vcvars64.bat";
                if (QFile::exists(vcvars)) {
                    // cmd /c "call vcvars64 && cd /d dir && cl /nologo /O2 /Fe:base.exe base_launch.c"
                    clCmd = QString("call \"%1\" && cd /d \"%2\" && cl /nologo /O2 /Fe:%3.exe %3_launch.c")
                        .arg(vcvars, dir, base);
                }
            }
        }
        if (clCmd.isEmpty()) {
            // try gcc / clang on PATH
            for (const char* cc : {"gcc", "clang"}) {
                QProcess which;
                which.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
                    a->flags |= CREATE_NO_WINDOW;
                });
                which.start("where", QStringList{cc});
                which.waitForFinished(3000);
                if (which.exitCode() == 0) {
                    clCmd = QString("cd /d \"%1\" && %2 -O2 -o %3.exe %3_launch.c")
                        .arg(dir).arg(cc).arg(base);
                    break;
                }
            }
        }
        if (clCmd.isEmpty()) {
            appendOut(
                "[exe] No C compiler found automatically.\n"
                "Generated: " + cfile + " and " + vye + "\n"
                "Install VS Build Tools (C++) or MinGW, then use Compile → Create host .exe again.\n"
                "VIB emits IR/packages; linking a PE uses the host C toolchain without opening a special prompt when VS is installed.");
            return;
        }
        QProcess build;
        build.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
        });
        build.start("cmd.exe", QStringList{"/C", clCmd});
        build.waitForFinished(180000);
        appendOut(QString::fromUtf8(build.readAllStandardOutput()));
        appendOut(QString::fromUtf8(build.readAllStandardError()));
        if (build.exitCode() == 0 && QFile::exists(exe)) {
            appendOut("[exe] Built host PE: " + exe + " (arch = this PC toolchain)");
            appendOut("Place vayu.exe next to it (or on PATH). Ships with " + vyeName);
        } else {
            appendOut("[exe] Build failed. Files left: " + cfile + ", " + vye);
        }
#else
        Q_UNUSED(exe);
        appendOut("[exe] Host PE emit is implemented for Windows in this drop.");
#endif
    }

    void toggleWrap() {
        wrap_ = !wrap_;
        auto mode = wrap_ ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap;
        if (auto* e = currentEditor()) e->setLineWrapMode(mode);
        output_->setLineWrapMode(mode);
        terminal_->setLineWrapMode(mode);
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
        dlg.setWindowTitle("Vayu Syntax");
        dlg.resize(760, 560);
        auto* t = new QTextEdit(&dlg);
        t->setReadOnly(true);
        t->setMarkdown(
            "# Vayu syntax (Integrator / `--native`)\n\n"
            "## if / compare\n`if c == \"name\":` — use `==` and quotes; indent.\n\n"
            "## Strings\n`\"a,b\".split(\",\")` · `\",\".join([\"a\",\"b\"])` · upper lower strip "
            "contains find starts_with ends_with replace char_at to_int\n\n"
            "## Membership / lists\n`2 in [1,2,3]` · `xs.append(3)` · `len(xs)`\n\n"
            "## Loops\n`for i in range(5):` · `range(2,6)` · `while` · `break`\n\n"
            "## Shell\n`shell(\"cmd /c echo hi\")` · PowerShell via `powershell -Command ...`\n\n"
            "## Functions\n`def f(a,b): return a+b`\n\n"
            "See `VIB/Docs/Implementation.md` · " + QString(kRepo));
        auto* lay = new QVBoxLayout(&dlg);
        lay->addWidget(t);
        dlg.exec();
    }
    void showHelpExamples() {
        QDialog dlg(this);
        dlg.setWindowTitle("Examples");
        dlg.resize(420, 340);
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
            if (auto* ed = currentEditor()) ed->setPlainText(examples()[r].code);
            dlg.accept();
        });
        dlg.exec();
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Vayu-IDE");
    QApplication::setApplicationVersion(kVer);
    QApplication::setOrganizationName("VayuIDE");
    MainWindow w;
    w.show();
    return app.exec();
}
