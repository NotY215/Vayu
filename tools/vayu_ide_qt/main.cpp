// Vayu-IDE V0.2-Alpha — VS Code–style, pure PE host exe (no MSVC), interactive terminal
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
#include <QToolBar>
#include <QStyle>
#ifdef Q_OS_WIN
#  include <windows.h>
#endif
#include "../emit_exe/PeLauncher.hpp"

static const char* kVer = "V0.2-Alpha";
static const char* kRepo = "https://github.com/NotY215/Vayu";

struct Example { QString title, code; };
static QList<Example> examples() {
    return {
        {"Loops", "for i in range(5):\n    print(i)\nfor x in [10, 20, 30]:\n    print(x)\n"},
        {"shell", "print(shell(\"cmd /c echo hello\"))\n"},
        {"strings", "print(\"a,b\".split(\",\"))\nprint(\",\".join([\"x\",\"y\"]))\nprint(\"Hi\".upper())\n"},
        {"in + append", "print(2 in [1,2,3])\nxs=[1]\nxs.append(7)\nprint(xs)\n"},
        {"functions", "def add(a,b):\n    return a+b\nprint(add(20,22))\n"},
        {"if input", "n = input(\"name: \")\nif n == \"vayu\":\n    print(\"hi\")\nelse:\n    print(\"ok\")\n"},
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
            setFont(f); e->accept(); return;
        }
        QPlainTextEdit::wheelEvent(e);
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->matches(QKeySequence::Copy) && textCursor().hasSelection()) {
            copy(); e->accept(); return;
        }
        QPlainTextEdit::keyPressEvent(e);
    }
    void contextMenuEvent(QContextMenuEvent* e) override {
        QMenu* m = createStandardContextMenu();
        m->addSeparator();
        connect(m->addAction("Copy All"), &QAction::triggered, this, [this]{
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
    QToolButton* runBtn_ = nullptr;
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
        resize(1440, 900);
        QIcon ic(":/IDE.ico");
        if (!ic.isNull()) setWindowIcon(ic);

        // --- Activity bar (VS Code left) ---
        auto* activity = new QFrame;
        activity->setObjectName("activity");
        activity->setFixedWidth(48);
        auto* actLay = new QVBoxLayout(activity);
        actLay->setContentsMargins(4, 8, 4, 8);
        actLay->setSpacing(4);
        auto mkAct = [&](const QString& glyph, const QString& tip, auto fn) {
            auto* b = new QToolButton;
            b->setText(glyph);
            b->setToolTip(tip);
            b->setFixedSize(40, 40);
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(
                "QToolButton{color:#cbd5e1;border:none;border-radius:6px;font-size:18px;}"
                "QToolButton:hover{background:#2a2a2a;color:#fff;}");
            connect(b, &QToolButton::clicked, this, fn);
            actLay->addWidget(b);
            return b;
        };
        mkAct(QStringLiteral("📂"), "Explorer (Ctrl+Shift+E)", [this]{ sideStack_->setCurrentIndex(0); });
        mkAct(QStringLiteral("🔎"), "Search (Ctrl+Shift+F)", [this]{ sideStack_->setCurrentIndex(1); });
        mkAct(QStringLiteral("⎇"), "Source Control — GitHub Coming soon", [this]{
            QMessageBox::information(this, "GitHub",
                QString("Source Control: Coming soon.\n\n%1").arg(kRepo));
        });
        actLay->addStretch();
        mkAct(QStringLiteral("⚙"), "Settings", [this]{
            QMessageBox::information(this, "Settings",
                QString("Vayu-IDE %1\n%2").arg(kVer).arg(kRepo));
        });

        explorer_ = new QTreeWidget;
        explorer_->setHeaderLabel("EXPLORER");
        explorer_->setCursor(Qt::PointingHandCursor);
        connect(explorer_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int){
            QString p = it->data(0, Qt::UserRole).toString();
            if (QFileInfo(p).isFile()) openFilePath(p);
        });

        auto* searchPane = new QWidget;
        auto* sp = new QVBoxLayout(searchPane);
        auto* findBox = new QLineEdit;
        findBox->setPlaceholderText("Search");
        sp->addWidget(findBox);
        connect(findBox, &QLineEdit::returnPressed, this, [this, findBox]{
            lastFind_ = findBox->text();
            if (auto* ed = currentEditor()) ed->find(lastFind_);
        });

        sideStack_ = new QStackedWidget;
        sideStack_->addWidget(explorer_);
        sideStack_->addWidget(searchPane);

        // --- Editor tabs + top run toolbar (VS Code style) ---
        auto* editorCol = new QWidget;
        auto* editorLay = new QVBoxLayout(editorCol);
        editorLay->setContentsMargins(0, 0, 0, 0);
        editorLay->setSpacing(0);

        auto* topBar = new QFrame;
        topBar->setObjectName("topBar");
        topBar->setFixedHeight(36);
        auto* topLay = new QHBoxLayout(topBar);
        topLay->setContentsMargins(8, 2, 8, 2);
        topLay->addStretch();
        runBtn_ = new QToolButton;
        runBtn_->setText(QStringLiteral("▶ Run"));
        runBtn_->setToolTip("Run --native (F5)");
        runBtn_->setCursor(Qt::PointingHandCursor);
        runBtn_->setStyleSheet(
            "QToolButton{background:#0e639c;color:white;padding:4px 12px;border:none;border-radius:4px;font-weight:bold;}"
            "QToolButton:hover{background:#1177bb;}");
        connect(runBtn_, &QToolButton::clicked, this, [this]{ runArgs({"--native"}); });
        topLay->addWidget(runBtn_);

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
        editorLay->addWidget(topBar);
        editorLay->addWidget(tabs_, 1);

        // --- Bottom: Output + Terminal ---
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
        terminal_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        termIn_ = new QLineEdit;
        termIn_->setPlaceholderText("Run command here (like VS Code terminal) — Enter · Select text above + Ctrl+C to copy");
        tl->addWidget(terminal_, 1);
        tl->addWidget(termIn_);
        botTabs->addTab(termPage, "Terminal");
        connect(termIn_, &QLineEdit::returnPressed, this, [this]{ terminalSubmit(); });

        // Application-wide copy
        auto* copySc = new QShortcut(QKeySequence::Copy, this);
        copySc->setContext(Qt::ApplicationShortcut);
        connect(copySc, &QShortcut::activated, this, [this]{ copySelection(); });

        auto* centerSplit = new QSplitter(Qt::Vertical);
        centerSplit->addWidget(editorCol);
        centerSplit->addWidget(botTabs);
        centerSplit->setStretchFactor(0, 3);
        centerSplit->setStretchFactor(1, 1);
        centerSplit->setSizes({620, 260});

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
        statusBar()->showMessage(QString("Vayu-IDE %1  |  F5 Run  |  Ctrl+C copy  |  Terminal ready").arg(kVer));

        QString lastFolder = settings_.value("lastFolder").toString();
        if (!lastFolder.isEmpty() && QDir(lastFolder).exists())
            openFolderPath(lastFolder);

        // VS Code-ish shortcuts
        bindShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), [this]{ sideStack_->setCurrentIndex(0); });
        bindShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), [this]{ sideStack_->setCurrentIndex(1); });
        bindShortcut(QKeySequence(Qt::CTRL | Qt::Key_S), [this]{ saveCurrent(); });
        bindShortcut(QKeySequence(Qt::CTRL | Qt::Key_N), [this]{ newFile(); });
        bindShortcut(QKeySequence(Qt::CTRL | Qt::Key_O), [this]{ openFile(); });
        bindShortcut(QKeySequence(Qt::CTRL | Qt::Key_P), [this]{ openFile(); }); // quick open ≈ open
        bindShortcut(QKeySequence(Qt::CTRL | Qt::Key_W), [this]{
            int i = tabs_->currentIndex();
            if (i >= 0) tabs_->tabCloseRequested(i);
        });
        bindShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), [this]{ findInEditor(); });
        bindShortcut(QKeySequence(Qt::Key_F5), [this]{ runArgs({"--native"}); });
        bindShortcut(QKeySequence(Qt::ALT | Qt::Key_Z), [this]{ toggleWrap(); });
        bindShortcut(QKeySequence(Qt::CTRL | Qt::Key_Grave), [this]{ // Ctrl+`
            botTabs->setCurrentIndex(1);
            termIn_->setFocus();
        });
    }

    ~MainWindow() override {
        if (runProc_) { runProc_->kill(); runProc_->waitForFinished(300); }
        if (shellProc_) { shellProc_->kill(); shellProc_->waitForFinished(300); }
    }

private:
    template <typename Fn>
    void bindShortcut(const QKeySequence& ks, Fn fn) {
        auto* sc = new QShortcut(ks, this);
        sc->setContext(Qt::ApplicationShortcut);
        connect(sc, &QShortcut::activated, this, fn);
    }

    void copySelection() {
        auto take = [](ZoomEdit* ed) -> bool {
            if (!ed) return false;
            if (ed->textCursor().hasSelection()) { ed->copy(); return true; }
            return false;
        };
        QWidget* f = QApplication::focusWidget();
        if (f == output_ && take(output_)) return;
        if ((f == terminal_ || f == termIn_) && take(terminal_)) return;
        if (auto* ed = currentEditor()) if (take(ed)) return;
        if (take(terminal_)) return;
        if (take(output_)) return;
        // No selection: copy nothing (VS Code does nothing / interrupt — we no-op)
    }

    void applyTheme() {
        qApp->setStyleSheet(
            "QMainWindow,QWidget{background:#1e1e1e;color:#d4d4d4;}"
            "#activity{background:#333333;}"
            "#topBar{background:#252526;border-bottom:1px solid #3c3c3c;}"
            "QTreeWidget,QPlainTextEdit,QLineEdit,QListWidget,QTextEdit{"
            " background:#1e1e1e;color:#d4d4d4;border:none;"
            " selection-background-color:#264f78;selection-color:#ffffff;}"
            "QTabWidget::pane{border-top:1px solid #3c3c3c;}"
            "QTabBar::tab{background:#2d2d2d;color:#969696;padding:8px 14px;}"
            "QTabBar::tab:selected{background:#1e1e1e;color:#fff;}"
            "QMenuBar{background:#3c3c3c;color:#ccc;}"
            "QMenu{background:#252526;color:#ccc;}"
            "QMenu::item:selected{background:#094771;}"
            "QStatusBar{background:#007acc;color:#fff;}"
            "QPushButton{background:#0e639c;color:#fff;border:none;padding:8px 14px;}"
            "QPushButton:hover{background:#1177bb;}"
            "QSplitter::handle{background:#3c3c3c;}"
            "QHeaderView::section{background:#252526;color:#bbb;border:none;padding:4px;}");
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
        // Interactive cmd is more reliable with redirected pipes than PowerShell -NoExit
        shellProc_->setProgram("cmd.exe");
        shellProc_->setArguments({"/Q", "/K", "prompt $P$G"});
        // Do NOT use CREATE_NO_WINDOW with interactive cmd on some hosts — keep hidden via no window + pipes
        shellProc_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
        });
#else
        shellProc_->setProgram("/bin/bash");
#endif
        connect(shellProc_, &QProcess::readyRead, this, [this]{
            QByteArray data = shellProc_->readAll();
            QString chunk = QString::fromLocal8Bit(data);
            terminal_->moveCursor(QTextCursor::End);
            terminal_->insertPlainText(chunk);
            terminal_->moveCursor(QTextCursor::End);
        });
        connect(shellProc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
                terminal_->appendPlainText("\n[shell exited — restarting]\n");
                shellProc_->deleteLater();
                shellProc_ = nullptr;
                QTimer::singleShot(150, this, [this]{ startShell(); });
            });
        if (!folder_.isEmpty()) shellProc_->setWorkingDirectory(folder_);
        shellProc_->start();
        if (!shellProc_->waitForStarted(4000)) {
            terminal_->appendPlainText("[error] could not start shell\n");
            shellProc_->deleteLater();
            shellProc_ = nullptr;
        } else {
            terminal_->appendPlainText("Vayu-IDE Terminal\nType commands below and press Enter.\n");
        }
    }

    void terminalSubmit() {
        QString line = termIn_->text();
        termIn_->clear();
        terminal_->moveCursor(QTextCursor::End);
        terminal_->insertPlainText(line + "\n");
        terminal_->moveCursor(QTextCursor::End);
        if (!shellProc_ || shellProc_->state() != QProcess::Running) {
            startShell();
        }
        if (shellProc_ && shellProc_->state() == QProcess::Running) {
            QByteArray payload = (line + "\r\n").toLocal8Bit();
            shellProc_->write(payload);
            shellProc_->waitForBytesWritten(1000);
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
        qs->addWidget(new QLabel("<b>Start</b>"));
        auto* b1 = new QPushButton("New File…");
        auto* b2 = new QPushButton("Open Folder…");
        auto* b3 = new QPushButton("GitHub");
        qs->addWidget(b1); qs->addWidget(b2); qs->addWidget(b3);
        auto* doc = new QVBoxLayout;
        doc->addWidget(new QLabel("<b>Help</b>"));
        auto* b4 = new QPushButton("Syntax");
        auto* b5 = new QPushButton("Examples");
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
            else if (QApplication::focusWidget() == terminal_ || QApplication::focusWidget() == termIn_)
                terminal_->selectAll();
            else if (QApplication::focusWidget() == output_)
                output_->selectAll();
        });
        add(edit, "Cu&t", QKeySequence::Cut, [this]{ if (auto* e=currentEditor()) e->cut(); });
        add(edit, "&Copy", QKeySequence::Copy, [this]{ copySelection(); });
        add(edit, "&Paste", QKeySequence::Paste, [this]{
            if (auto* e = currentEditor()) e->paste();
            else if (QApplication::focusWidget() == termIn_) termIn_->paste();
        });
        edit->addSeparator();
        add(edit, "&Find…", QKeySequence::Find, [this]{ findInEditor(); });
        add(edit, "Word wrap (Alt+Z)", QKeySequence(), [this]{ toggleWrap(); });

        auto* run = m->addMenu("&Run");
        add(run, "Run --native", QKeySequence(Qt::Key_F5), [this]{ runArgs({"--native"}); });
        add(run, "Run interpreter", QKeySequence(), [this]{ runArgs({}); });
        add(run, "Run --vm", QKeySequence(), [this]{ runArgs({"--vm"}); });
        add(run, "Dump IR", QKeySequence(), [this]{ runArgs({"--dump-ir"}); });
        add(run, "Stop", QKeySequence(), [this]{ if (runProc_) runProc_->kill(); });

        auto* compile = m->addMenu("&Compile");
        add(compile, "Emit .vye", QKeySequence(Qt::Key_F7), [this]{ emitVye(); });
        add(compile, "Run .vye…", QKeySequence(), [this]{ runVye(); });
        add(compile, "Create host .exe (no MSVC)…", QKeySequence(), [this]{ emitHostExe(); });

        auto* help = m->addMenu("&Help");
        add(help, "&Syntax…", QKeySequence(), [this]{ showHelpSyntax(); });
        add(help, "Load &Examples…", QKeySequence(), [this]{ showHelpExamples(); });
        add(help, "&Version", QKeySequence(), [this]{
            QMessageBox::information(this, "Version", QString("Vayu-IDE %1\n%2").arg(kVer).arg(kRepo));
        });
        add(help, "Open GitHub", QKeySequence(), []{ QDesktopServices::openUrl(QUrl(kRepo)); });
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
                 QStringList{"*.vyu","*.md","*.txt","*.ps1","*.cmd","*.sh","*.vye","*.exe"},
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
            QMessageBox::information(this, "Busy", "Already running.");
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

    // Pure PE — no MSVC, no .c file
    void emitHostExe() {
        if (!saveCurrent()) return;
        QString path = currentPath();
        QString dir = QFileInfo(path).absolutePath();
        QString base = QFileInfo(path).completeBaseName();
        QString vye = dir + "/" + base + ".vye";
        QString exe = dir + "/" + base + ".exe";

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

        // WinExec command: run vayu.exe with the .vye in the same folder
        // Use cmd /c so relative path works when user double-clicks the exe
        QString cmd = QString("cmd.exe /c vayu.exe \"%1.vye\"").arg(base);
        std::string cmdStd = cmd.toStdString();
        std::string exeStd = exe.toStdString();
        if (!vayu_pe::writeHostLauncherExe(exeStd, cmdStd)) {
            appendOut("[exe] failed to write PE\n");
            return;
        }
        appendOut("[exe] Wrote native PE (no MSVC): " + exe + "\n");
        appendOut("Keep vayu.exe and " + base + ".vye next to it, then double-click the .exe\n");
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
        dlg.setWindowTitle("Vayu Syntax");
        dlg.resize(840, 640);
        auto* t = new QTextEdit(&dlg);
        t->setReadOnly(true);
        t->setMarkdown(QString(
            "# Vayu brief (`--native` / Integrator)\n\n"
            "## Run\n`vayu file.vyu --native` · IDE **▶ Run** / **F5**\n\n"
            "## if\n`if x == \"a\":` — use `==` and quotes; indent.\n\n"
            "## Strings\n`split` `join` `upper` `lower` `strip` `contains` `find` "
            "`starts_with` `ends_with` `replace` `char_at` `to_int` — works on literals.\n\n"
            "## Lists / in\n`xs.append(v)` · `2 in [1,2]` · `\"ab\" in \"xabz\"` · `len`\n\n"
            "## Loops\n`for i in range(5):` · `range(2,6)` · `for x in list:` · `while` · `break`\n\n"
            "## Functions\n`def f(a,b): return a+b`\n\n"
            "## Shell\n`shell(\"cmd /c echo hi\")` · `system(...)` · `run_command(...)`\n\n"
            "## Packages / host exe\n"
            "`--output file.vye` · **Compile → Create host .exe** writes a **real PE** "
            "(no MSVC). Place `vayu.exe` + `.vye` beside the `.exe`.\n\n"
            "Repo: %1").arg(kRepo));
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
        auto* btn = new QPushButton("Load");
        auto* lay = new QVBoxLayout(&dlg);
        lay->addWidget(list); lay->addWidget(btn);
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
