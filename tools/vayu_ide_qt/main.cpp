// Vayu-IDE V0.2-Alpha — VS Code–style, interactive Terminal, Ctrl+C copy
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
#include <QKeyEvent>
#include <QTimer>
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
         "for i in range(5):\n    print(i)\nfor x in [10, 20, 30]:\n    print(x)\n"},
        {"shell / system",
         "print(shell(\"cmd /c echo hello-from-cmd\"))\nprint(system(\"cmd /c echo via-system\"))\n"},
        {"shell PowerShell",
         "shell(\"powershell -NoProfile -Command \\\"Write-Host 'hi from pwsh'\\\"\")\n"},
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
    void keyPressEvent(QKeyEvent* e) override {
        // Ctrl+C with selection → copy (VS Code behavior in read-only panes)
        if (e->matches(QKeySequence::Copy)) {
            if (textCursor().hasSelection()) {
                copy();
                e->accept();
                return;
            }
        }
        QPlainTextEdit::keyPressEvent(e);
    }
    void contextMenuEvent(QContextMenuEvent* e) override {
        QMenu* m = createStandardContextMenu();
        m->addSeparator();
        QAction* a = m->addAction("Copy All");
        connect(a, &QAction::triggered, this, [this]{
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
    QProcess* runProc_ = nullptr;
    QProcess* shellProc_ = nullptr;
    bool wrap_ = false;
    QString lastFind_;

public:
    MainWindow()
        : settings_(QSettings::IniFormat, QSettings::UserScope, "VayuIDE", "VayuIDE") {
        setWindowTitle(QString("Vayu-IDE %1").arg(kVer));
        resize(1400, 900);
        QIcon ic(":/IDE.ico");
        if (!ic.isNull()) setWindowIcon(ic);

        auto* activity = new QFrame;
        activity->setObjectName("activity");
        activity->setFixedWidth(48);
        auto* actLay = new QVBoxLayout(activity);
        actLay->setContentsMargins(4, 8, 4, 8);
        actLay->setSpacing(6);
        auto mkAct = [&](const QString& text, const QString& tip, auto fn) {
            auto* b = new QToolButton;
            b->setText(text);
            b->setToolTip(tip);
            b->setFixedSize(40, 40);
            b->setStyleSheet(
                "QToolButton{color:#cccccc;border:none;border-radius:6px;font-size:16px;font-weight:bold;}"
                "QToolButton:hover{background:#2a2d2e;color:#ffffff;}");
            connect(b, &QToolButton::clicked, this, fn);
            actLay->addWidget(b);
            return b;
        };
        mkAct("📁", "Explorer", [this]{ sideStack_->setCurrentIndex(0); });
        mkAct("🔍", "Search", [this]{ sideStack_->setCurrentIndex(1); });
        mkAct("⌥", "Source Control (GitHub) — Coming soon", [this]{
            QMessageBox::information(this, "GitHub",
                "Source Control integration: Coming soon.\n\n"
                "Repo: " + QString(kRepo));
        });
        actLay->addStretch();
        mkAct("⚙", "Settings", [this]{
            QMessageBox::information(this, "Settings",
                QString("Vayu-IDE %1\nData: %%AppData%%/VayuIDE\n%2").arg(kVer).arg(kRepo));
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
        findBox->setPlaceholderText("Search in current file");
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

        auto* botTabs = new QTabWidget;
        output_ = new ZoomEdit;
        output_->setReadOnly(true);
        output_->setUndoRedoEnabled(false);
        output_->setFocusPolicy(Qt::StrongFocus);
        botTabs->addTab(output_, "Output");

        auto* termPage = new QWidget;
        auto* tl = new QVBoxLayout(termPage);
        tl->setContentsMargins(0, 0, 0, 0);
        tl->setSpacing(0);
        terminal_ = new ZoomEdit;
        terminal_->setReadOnly(true);
        terminal_->setUndoRedoEnabled(false);
        terminal_->setFocusPolicy(Qt::StrongFocus);
        termIn_ = new QLineEdit;
        termIn_->setPlaceholderText("Terminal command (PowerShell) — Enter to run · select output then Ctrl+C to copy");
        tl->addWidget(terminal_, 1);
        tl->addWidget(termIn_);
        botTabs->addTab(termPage, "Terminal");
        connect(termIn_, &QLineEdit::returnPressed, this, [this]{ terminalSubmit(); });

        // Global Ctrl+C: if Output/Terminal focused with selection → copy
        auto* copySc = new QShortcut(QKeySequence::Copy, this);
        connect(copySc, &QShortcut::activated, this, [this]{
            QWidget* f = QApplication::focusWidget();
            auto hasSel = [](ZoomEdit* ed) {
                return ed && ed->textCursor().hasSelection();
            };
            if (f == output_ && hasSel(output_)) { output_->copy(); return; }
            if ((f == terminal_ || f == termIn_) && hasSel(terminal_)) { terminal_->copy(); return; }
            if (auto* ed = currentEditor()) {
                if (ed->textCursor().hasSelection()) { ed->copy(); return; }
            }
            // Fallback: prefer terminal, then output
            if (hasSel(terminal_)) { terminal_->copy(); return; }
            if (hasSel(output_)) { output_->copy(); return; }
        });

        auto* centerSplit = new QSplitter(Qt::Vertical);
        centerSplit->addWidget(tabs_);
        centerSplit->addWidget(botTabs);
        centerSplit->setStretchFactor(0, 3);
        centerSplit->setStretchFactor(1, 1);
        centerSplit->setSizes({600, 280});

        auto* midSplit = new QSplitter(Qt::Horizontal);
        midSplit->addWidget(sideStack_);
        midSplit->addWidget(centerSplit);
        midSplit->setSizes({260, 1100});

        auto* root = new QWidget;
        auto* rootLay = new QHBoxLayout(root);
        rootLay->setContentsMargins(0, 0, 0, 0);
        rootLay->setSpacing(0);
        rootLay->addWidget(activity);
        rootLay->addWidget(midSplit, 1);
        setCentralWidget(root);

        buildMenus();
        applyTheme();
        startShell();
        statusBar()->showMessage(QString("Vayu-IDE %1 · Terminal ready · Ctrl+C copies selection").arg(kVer));

        QString lastFolder = settings_.value("lastFolder").toString();
        if (!lastFolder.isEmpty() && QDir(lastFolder).exists())
            openFolderPath(lastFolder);

        auto* scWrap = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Z), this);
        connect(scWrap, &QShortcut::activated, this, [this]{ toggleWrap(); });
    }

    ~MainWindow() override {
        if (runProc_) { runProc_->kill(); runProc_->waitForFinished(300); }
        if (shellProc_) { shellProc_->kill(); shellProc_->waitForFinished(300); }
    }

private:
    void applyTheme() {
        qApp->setStyleSheet(
            "QMainWindow,QWidget{background:#1e1e1e;color:#d4d4d4;}"
            "#activity{background:#333333;}"
            "QTreeWidget,QPlainTextEdit,QLineEdit,QListWidget,QTextEdit{"
            " background:#1e1e1e;color:#d4d4d4;border:none;"
            " selection-background-color:#264f78;selection-color:#ffffff;}"
            "QTabWidget::pane{border-top:1px solid #252526;}"
            "QTabBar::tab{background:#2d2d2d;color:#969696;padding:8px 14px;}"
            "QTabBar::tab:selected{background:#1e1e1e;color:#ffffff;}"
            "QMenuBar{background:#3c3c3c;color:#cccccc;}"
            "QMenu{background:#252526;color:#cccccc;}"
            "QMenu::item:selected{background:#094771;}"
            "QStatusBar{background:#007acc;color:#ffffff;}"
            "QPushButton{background:#0e639c;color:#ffffff;border:none;padding:8px 14px;}"
            "QPushButton:hover{background:#1177bb;}"
            "QSplitter::handle{background:#252526;}"
            "QHeaderView::section{background:#252526;color:#bbbbbb;border:none;padding:4px;}");
        QFont mono("Cascadia Mono", 12);
        if (!mono.exactMatch()) mono = QFont("Consolas", 12);
        output_->setFont(mono);
        terminal_->setFont(mono);
        termIn_->setFont(mono);
    }

    void startShell() {
        if (shellProc_) return;
        shellProc_ = new QProcess(this);
        shellProc_->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
        shellProc_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
        });
        shellProc_->setProgram("powershell.exe");
        shellProc_->setArguments({"-NoLogo", "-NoExit"});
#else
        shellProc_->setProgram("/bin/bash");
#endif
        connect(shellProc_, &QProcess::readyRead, this, [this]{
            QString chunk = QString::fromLocal8Bit(shellProc_->readAll());
            terminal_->moveCursor(QTextCursor::End);
            terminal_->insertPlainText(chunk);
            terminal_->moveCursor(QTextCursor::End);
        });
        connect(shellProc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
                terminal_->appendPlainText("\n[shell exited — restarting]\n");
                shellProc_->deleteLater();
                shellProc_ = nullptr;
                QTimer::singleShot(200, this, [this]{ startShell(); });
            });
        shellProc_->start();
        if (!shellProc_->waitForStarted(3000)) {
            terminal_->appendPlainText("[error] could not start PowerShell/shell\n");
            shellProc_->deleteLater();
            shellProc_ = nullptr;
        } else {
            terminal_->appendPlainText("Vayu-IDE Terminal (PowerShell)\n");
        }
    }

    void terminalSubmit() {
        QString line = termIn_->text();
        termIn_->clear();
        terminal_->moveCursor(QTextCursor::End);
        terminal_->insertPlainText(line + "\n");
        if (shellProc_ && shellProc_->state() == QProcess::Running) {
            shellProc_->write((line + "\n").toUtf8());
        } else {
            startShell();
            if (shellProc_ && shellProc_->state() == QProcess::Running)
                shellProc_->write((line + "\n").toUtf8());
        }
    }

    QWidget* buildWelcome() {
        auto* w = new QWidget;
        auto* lay = new QVBoxLayout(w);
        lay->addWidget(new QLabel(
            QString("<h1 style='color:#3794ff'>Vayu IDE</h1>"
                    "<p style='color:#989898'>Code · Build · Create · %1</p>").arg(kVer)));
        auto* row = new QHBoxLayout;
        auto* qs = new QVBoxLayout;
        qs->addWidget(new QLabel("<b>Quick Start</b>"));
        auto* b1 = new QPushButton("New File…");
        auto* b2 = new QPushButton("Open Folder…");
        auto* b3 = new QPushButton("GitHub Repository");
        qs->addWidget(b1); qs->addWidget(b2); qs->addWidget(b3);
        auto* doc = new QVBoxLayout;
        doc->addWidget(new QLabel("<b>Help</b>"));
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
        add(edit, "Select &All", QKeySequence::SelectAll, [this]{
            if (auto* e = currentEditor()) e->selectAll();
            else if (QApplication::focusWidget() == output_) output_->selectAll();
            else if (QApplication::focusWidget() == terminal_ ||
                     QApplication::focusWidget() == termIn_) terminal_->selectAll();
        });
        add(edit, "Cu&t", QKeySequence::Cut, [this]{ if (auto* e=currentEditor()) e->cut(); });
        add(edit, "&Copy", QKeySequence::Copy, [this]{
            if (auto* e = currentEditor()) { if (e->textCursor().hasSelection()) { e->copy(); return; } }
            if (output_->textCursor().hasSelection()) { output_->copy(); return; }
            if (terminal_->textCursor().hasSelection()) { terminal_->copy(); return; }
        });
        add(edit, "&Paste", QKeySequence::Paste, [this]{ if (auto* e=currentEditor()) e->paste(); });
        edit->addSeparator();
        add(edit, "&Find…", QKeySequence::Find, [this]{ findInEditor(); });
        add(edit, "Word wrap (Alt+Z)", QKeySequence(), [this]{ toggleWrap(); });

        auto* run = m->addMenu("&Run");
        add(run, "Run --native", QKeySequence(Qt::Key_F5), [this]{ runArgs({"--native"}); });
        add(run, "Run interpreter", QKeySequence(), [this]{ runArgs({}); });
        add(run, "Run --vm", QKeySequence(), [this]{ runArgs({"--vm"}); });
        add(run, "Dump IR", QKeySequence(), [this]{ runArgs({"--dump-ir"}); });
        add(run, "Stop run", QKeySequence(), [this]{ if (runProc_) runProc_->kill(); });

        auto* compile = m->addMenu("&Compile");
        add(compile, "Emit .vye", QKeySequence(Qt::Key_F7), [this]{ emitVye(); });
        add(compile, "Run .vye…", QKeySequence(), [this]{ runVye(); });
        add(compile, "Create host .exe…", QKeySequence(), [this]{ emitHostExe(); });

        auto* help = m->addMenu("&Help");
        add(help, "&Syntax (detailed)…", QKeySequence(), [this]{ showHelpSyntax(); });
        add(help, "Load &Examples…", QKeySequence(), [this]{ showHelpExamples(); });
        add(help, "&Version", QKeySequence(), [this]{
            QMessageBox::information(this, "Version",
                QString("Vayu-IDE %1\n%2").arg(kVer).arg(kRepo));
        });
        add(help, "Open GitHub Repo", QKeySequence(), []{ QDesktopServices::openUrl(QUrl(kRepo)); });
    }

    ZoomEdit* currentEditor() {
        QWidget* w = tabs_->currentWidget();
        if (!w || w == welcome_ || !w->property("vayuEditor").toBool()) return nullptr;
        return static_cast<ZoomEdit*>(w);
    }
    QString currentPath() const {
        int i = tabs_->currentIndex();
        return i < 0 ? QString() : tabs_->tabToolTip(i);
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
        for (int i = 0; i < tabs_->count(); ++i)
            if (tabs_->tabToolTip(i) == path) { tabs_->setCurrentIndex(i); return; }
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
        if (shellProc_) shellProc_->setWorkingDirectory(d);
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
        return QFile::exists(p) ? p : QString("vayu.exe");
    }

    void appendOut(const QString& s) {
        output_->moveCursor(QTextCursor::End);
        output_->insertPlainText(s.endsWith('\n') ? s : s + "\n");
        output_->moveCursor(QTextCursor::End);
    }

    void runArgs(const QStringList& extra) {
        if (runProc_) {
            QMessageBox::information(this, "Busy", "A program is already running.");
            return;
        }
        if (!saveCurrent()) return;
        QString path = currentPath();
        if (path.isEmpty()) return;
        output_->clear();
        appendOut(QString("[run %1]\n").arg(extra.join(' ')));
        runProc_ = new QProcess(this);
        runProc_->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
        runProc_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
        });
#endif
        connect(runProc_, &QProcess::readyRead, this, [this]{
            appendOut(QString::fromLocal8Bit(runProc_->readAll()));
        });
        connect(runProc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
                appendOut(QString("—— exit %1 ——\n").arg(code));
                runProc_->deleteLater();
                runProc_ = nullptr;
            });
        QStringList args; args << path << extra;
        runProc_->start(vayuBin(), args);
        if (!runProc_->waitForStarted(4000)) {
            appendOut("[error] cannot start vayu.exe\n");
            runProc_->deleteLater();
            runProc_ = nullptr;
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
        runProc_ = new QProcess(this);
        runProc_->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
        runProc_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
        });
#endif
        connect(runProc_, &QProcess::readyRead, this, [this]{
            appendOut(QString::fromLocal8Bit(runProc_->readAll()));
        });
        connect(runProc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int c, QProcess::ExitStatus) {
                appendOut(QString("—— exit %1 ——\n").arg(c));
                runProc_->deleteLater(); runProc_ = nullptr;
            });
        runProc_->start(vayuBin(), QStringList{f});
    }

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
        appendOut(QString::fromLocal8Bit(emitP.readAllStandardOutput()));
        if (emitP.exitCode() != 0) {
            appendOut("[emit-vye failed]\n");
            return;
        }
        QString vyeName = base + ".vye";
        QString src = QString(
            "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
            "#ifdef _WIN32\n#include <windows.h>\n#endif\n"
            "int main(void){\n"
            "  const char* vye=\"%1\";\n"
            "  char cmd[1024];\n"
            "#ifdef _WIN32\n"
            "  char self[MAX_PATH]; GetModuleFileNameA(NULL,self,MAX_PATH);\n"
            "  char* s=strrchr(self,'\\\\'); if(s) *(s+1)=0; else self[0]=0;\n"
            "  snprintf(cmd,sizeof(cmd),\"\\\"%%svayu.exe\\\" \\\"%%s%%s\\\"\",self,self,vye);\n"
            "#else\n"
            "  snprintf(cmd,sizeof(cmd),\"vayu \\\"%%s\\\"\",vye);\n"
            "#endif\n"
            "  return system(cmd);\n}\n").arg(vyeName);
        QFile cf(cfile);
        if (!cf.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        cf.write(src.toUtf8());
        cf.close();

#ifdef Q_OS_WIN
        QString vswhere = QString(qEnvironmentVariable("ProgramFiles(x86)"))
            + "/Microsoft Visual Studio/Installer/vswhere.exe";
        QString clCmd;
        if (QFile::exists(vswhere)) {
            QProcess vw;
            vw.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
                a->flags |= CREATE_NO_WINDOW;
            });
            vw.start(vswhere, QStringList{"-latest","-products","*",
                "-requires","Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-property","installationPath"});
            vw.waitForFinished(15000);
            QString install = QString::fromUtf8(vw.readAllStandardOutput()).trimmed();
            if (!install.isEmpty()) {
                QString vcvars = install + "/VC/Auxiliary/Build/vcvars64.bat";
                if (QFile::exists(vcvars))
                    clCmd = QString("call \"%1\" && cd /d \"%2\" && cl /nologo /O2 /Fe:%3.exe %3_launch.c")
                        .arg(vcvars, dir, base);
            }
        }
        if (clCmd.isEmpty()) {
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
            appendOut("[exe] No C compiler auto-detected. Left " + cfile + " + " + vye + "\n");
            return;
        }
        QProcess build;
        build.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
        });
        build.start("cmd.exe", QStringList{"/C", clCmd});
        build.waitForFinished(180000);
        appendOut(QString::fromLocal8Bit(build.readAllStandardOutput()));
        appendOut(QString::fromLocal8Bit(build.readAllStandardError()));
        if (build.exitCode() == 0 && QFile::exists(exe))
            appendOut("[exe] " + exe + " (keep vayu.exe beside it)\n");
        else
            appendOut("[exe] build failed\n");
#else
        Q_UNUSED(exe);
#endif
    }

    void toggleWrap() {
        wrap_ = !wrap_;
        auto mode = wrap_ ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap;
        if (auto* e = currentEditor()) e->setLineWrapMode(mode);
        output_->setLineWrapMode(mode);
        terminal_->setLineWrapMode(mode);
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
        dlg.setWindowTitle("Vayu Syntax — detailed brief");
        dlg.resize(820, 620);
        auto* t = new QTextEdit(&dlg);
        t->setReadOnly(true);
        t->setMarkdown(QString::fromUtf8(u8R"MD(
# Vayu language brief (Integrator `--native`)

## How execution works
1. Parse `.vyu` → AST  
2. Type-check (sema)  
3. Lower to **VIR** (VIB)  
4. Interpret VIR in-process  

Use: `vayu file.vyu --native`

## Variables & types
```
x = 1
s = "text"
b = true
```
Optional annotations: `x: int = 1`

## Compare & if
Use **`==`** for equality (not `=`). Strings need quotes. Indent bodies.
```
if c == "heisenberg":
    print("ok")
elif c == "other":
    print("elif")
else:
    print("no")
```

## Strings
| Feature | Example |
|--------|---------|
| Concat / repeat | `"a"+"b"`, `"ab"*2` |
| split / join | `"a,b".split(",")`, `",".join(["a","b"])` |
| Case / trim | `.upper()` `.lower()` `.strip()` |
| Search | `.contains("x")` `.find("x")` `.starts_with` `.ends_with` |
| Other | `.replace(a,b)` `.char_at(i)` `.to_int()` `.is_digit()` |

Methods work on **literals and variables**.

## Membership & lists
```
print(2 in [1, 2, 3])
print("ab" in "xabz")
xs = [1]
xs.append(9)
print(len(xs))
print(xs[0])
```

## Loops
```
for i in range(5):      # 0..4
    print(i)
for i in range(2, 6):   # 2..5
    print(i)
for x in [10, 20]:
    print(x)
while n > 0:
    n = n - 1
    break  # / continue
```

## Functions
```
def add(a, b):
    return a + b
print(add(2, 3))
```

Builtins: `print`, `input(prompt?)`, `read_int`, `len`, `str`/`int`/`float`/`bool`,
`range`, **`shell` / `system` / `run_command`** (host command, returns exit code).

## Host shell from Vayu
```
shell("cmd /c echo hello")
system("cmd /c dir")
shell("powershell -NoProfile -Command \"Write-Host hi\"")
```
These call the OS shell (`std::system`). Prefer simple one-liners.

## Packages
```
vayu file.vyu --output out.vye
vayu out.vye
```

## Not on Integrator yet
Exceptions, classes, full generics, generators, lambdas, PE AOT (Phase 10–12).

Repo: %1
)MD").arg(kRepo));
        auto* lay = new QVBoxLayout(&dlg);
        lay->addWidget(t);
        dlg.exec();
    }

    void showHelpExamples() {
        QDialog dlg(this);
        dlg.setWindowTitle("Load example");
        dlg.resize(440, 360);
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
