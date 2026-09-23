#include "MainWindow.h"

#include <QCryptographicHash>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QUrl>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QStyleHints>
#include <QTabBar>
#include <QtConcurrent>
#include <QThreadPool>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <memory>
#include <numeric>

#include "BacklinksPane.h"
#include "Hotkeys.h"
#include "Icons.h"
#include "MarkdownEditor.h"
#include "BookmarksPane.h"
#include "DailyNotes.h"
#include "AudioRecorder.h"
#ifdef NOTEAPPC_HAVE_NOTEBOOK
#include "NotebookController.h"
#endif
#include "Frontmatter.h"
#include "MomentFormat.h"
#include "OutgoingLinksPane.h"
#include "OutlinePane.h"
#include "TemplatePicker.h"
#include "Templates.h"
#include "PublishDialog.h"
#include "ExportPdfDialog.h"
#include "PagePreview.h"
#include "PdfPage.h"
#include "RecoveryDialog.h"
#include "Snapshots.h"
#include "Parser.h"
#include "QuickSwitcher.h"
#include "SearchPane.h"
#include "Workspaces.h"
#ifdef NOTEAPPC_HAVE_GRAPH
#include "GraphView.h"
#endif
#ifdef NOTEAPPC_HAVE_EPUB
#include "EpubView.h"
#endif
#ifdef NOTEAPPC_HAVE_PUBLISH
#include "Publisher.h"
#endif
#ifdef NOTEAPPC_HAVE_PLUGINS
#include "PluginRegistry.h"
#endif
#ifdef NOTEAPPC_HAVE_SYNC
#include "SyncEngine.h"
#endif
#ifdef NOTEAPPC_HAVE_PDFVIEW
#include "PdfRecovery.h"
#include "PdfView.h"
#endif
#ifdef NOTEAPPC_HAVE_CANVAS
#include "CanvasView.h"
#endif
#ifdef NOTEAPPC_HAVE_BASES
#include "BaseEditorTable.h"
#include "BaseFile.h"
#include "BaseView.h"
#endif
#ifdef NOTEAPPC_HAVE_HISTORY
#include "NoteHistory.h"
#endif
#include "NoteHistoryDialog.h"
#include "Scheme.h"
#include "SettingsPage.h"
#include "PropertiesPane.h"
#include "SlidesView.h"
#include "TagsPane.h"

#ifdef NOTEAPPC_HAVE_SPELLCHECK
#include "SpellChecker.h"
#include <QFutureWatcher>
#include <QtConcurrent>
#endif

namespace noteAppC {

namespace {

/// "image", "audio", "video", or empty: the files Obsidian opens in a tab
/// of their own, by the lists the editor's embeds use (layoutEmbed).
QString mediaKindOf(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QByteArrayList images = QImageReader::supportedImageFormats();
    if (suffix != QLatin1String("pdf") && images.contains(suffix.toUtf8()))
        return QStringLiteral("image");
    static const QStringList audio = {QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("m4a"),
                                      QStringLiteral("ogg"), QStringLiteral("3gp"), QStringLiteral("flac"),
                                      QStringLiteral("opus"), QStringLiteral("oga")};
    if (audio.contains(suffix))
        return QStringLiteral("audio");
    static const QStringList video = {QStringLiteral("mp4"), QStringLiteral("webm"), QStringLiteral("ogv"),
                                      QStringLiteral("mov"), QStringLiteral("mkv")};
    return video.contains(suffix) ? QStringLiteral("video") : QString();
}
constexpr auto kGeometry = "window/geometry";
/// Narrower than this there is no room for a sidebar beside a readable line:
/// a phone held upright, or a window tiled to a sliver.
constexpr int kNarrowWidth = 720;
constexpr auto kSplitter = "window/splitter";
constexpr auto kRightSidebar = "window/rightSidebar";

/// Obsidian's file explorer is text only; QFileSystemModel wants to draw
/// desktop icons, so hand it a provider that draws nothing.
class NoIcons : public QFileIconProvider {
public:
    QIcon icon(IconType) const override { return {}; }
    QIcon icon(const QFileInfo &) const override { return {}; }
};

NoIcons *noIcons()
{
    static NoIcons provider;
    return &provider;
}

QString rgbaString(const QColor &color, double alpha)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(alpha, 0, 'f', 3);
}
} // namespace

MainWindow::~MainWindow()
{
    // The indexer's scan, which posts to the index, stops first. Searches
    // for ```query blocks run on the pool against m_index, and the history's
    // last saves may still be on their way to disk.
    m_indexer->stop();
    m_sync.reset(); // its thread stops and joins here
#ifdef NOTEAPPC_HAVE_PLUGINS
    // Its pending requests fail through callbacks that reach this window.
    if (m_plugins)
        m_plugins->shutdownAll();
    delete m_plugins;
    m_plugins = nullptr;
#endif
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    if (m_notebook)
        m_notebook->shutdownAll(); // no kernel outlives the window
#endif
    QThreadPool::globalInstance()->waitForDone();
    m_historyPool.waitForDone();
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_autosave.setSingleShot(true);
    m_autosave.setInterval(2000); // Obsidian's pause, near enough
    connect(&m_autosave, &QTimer::timeout, this, &MainWindow::autosave);
    m_historyPool.setMaxThreadCount(1);
    m_historyPool.setExpiryTimeout(-1);
    m_theme = themeFromSettings();

    auto *central = new QWidget(this);
    auto *rows = new QVBoxLayout(central);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(0);

    auto *splitter = new QSplitter(Qt::Horizontal, central);
    splitter->setObjectName(QStringLiteral("mainSplitter"));
    splitter->addWidget(buildSidebar());
    splitter->addWidget(buildMainArea());
    splitter->addWidget(buildRightSidebar());
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    splitter->setHandleWidth(1);
    splitter->setChildrenCollapsible(false);

    rows->addWidget(buildHeaderBar());
    rows->addWidget(splitter, 1);
    setCentralWidget(central);

    m_wordCount = new QLabel(this);
    m_vaultLabel = new QLabel(this);
    m_syncStatus = new QLabel(this);
    m_syncStatus->setObjectName(QStringLiteral("syncStatus"));
    m_syncStatus->hide();
    statusBar()->addPermanentWidget(m_syncStatus);
    // Never recording unseen: the status bar says so while it does.
    m_recordingStatus = new QLabel(this);
    m_recordingStatus->setObjectName(QStringLiteral("recordingStatus"));
    m_recordingStatus->hide();
    statusBar()->addPermanentWidget(m_recordingStatus);
    statusBar()->addPermanentWidget(m_wordCount);
    statusBar()->addPermanentWidget(m_vaultLabel);
    statusBar()->setSizeGripEnabled(false);

    m_indexer = new Indexer(&m_index, this);
    connect(m_indexer, &Indexer::progress, this, [this](int done, int total) {
        statusBar()->showMessage(tr("Indexing %1 of %2...").arg(done).arg(total));
    });
    connect(m_indexer, &Indexer::finished, this, [this](int indexed, qint64 milliseconds) {
        statusBar()->showMessage(indexed > 0
                                     ? tr("Indexed %n note(s) in %1 ms", nullptr, indexed)
                                           .arg(milliseconds)
                                     : tr("Index up to date"),
                                 4000);
        // No onIndexChanged() here: Index::changed has already fired, from
        // open() for a vault that was up to date and from the batch for one
        // that was not. Both ran it, 210 ms apiece on 20 000 notes.
    });
    connect(&m_index, &Index::changed, this, &MainWindow::onIndexChanged);

    m_switcher = new QuickSwitcher(this);
    connect(m_switcher, &QuickSwitcher::accepted, this, &MainWindow::openRelative);

    buildActions();
    // Installed on the application, not just this window, because the
    // mouse's back/forward side buttons have to work no matter which child
    // widget - editor, tree, a sidebar pane - happens to be under the cursor.
    qApp->installEventFilter(this);
    applySettings();
    // Baseline context so the settings page works even before any vault
    // ever opens; openVault()/closeVault()/openSingleFile() replace it.
    m_settingsPage->setContext(&m_settings, QString());
    resize(1180, 760);

    QSettings settings;
    restoreGeometry(settings.value(kGeometry).toByteArray());
    splitter->restoreState(settings.value(kSplitter).toByteArray());
    if (splitter->sizes().value(0) <= 0)
        splitter->setSizes({260, 640, 260});
    m_rightSidebar->setVisible(settings.value(kRightSidebar, true).toBool());

    updateTitle();
    // Nothing is opened here on purpose. Reading a vault from the constructor
    // meant the process did all of its work before the first pixel appeared;
    // openTarget() does it now, and openWhenShown() waits for the paint.
}

QToolButton *MainWindow::iconButton(const QString &objectName, const QString &icon,
                                    const QString &tooltip, int size)
{
    auto *button = new QToolButton(this);
    button->setObjectName(objectName);
    button->setToolTip(tooltip);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setIconSize(QSize(size, size));
    button->setProperty("iconName", icon);
    return button;
}

QWidget *MainWindow::buildHeaderBar()
{
    m_headerBar = new QWidget(this);
    m_headerBar->setObjectName(QStringLiteral("headerBar"));
    m_headerBar->setFixedHeight(44);

    auto *layout = new QHBoxLayout(m_headerBar);
    layout->setContentsMargins(8, 4, 10, 4);
    layout->setSpacing(6);

    QToolButton *sidebarToggle = iconButton(QStringLiteral("headerSidebarToggle"),
                                            QStringLiteral("panel-left"),
                                            tr("Toggle sidebar (Ctrl+Alt+S)"), 18);
    connect(sidebarToggle, &QToolButton::clicked, this, &MainWindow::toggleSidebar);

    m_backButton = iconButton(QStringLiteral("headerBackButton"), QStringLiteral("arrow-left"),
                              tr("Back (Alt+Left)"), 18);
    connect(m_backButton, &QToolButton::clicked, this, &MainWindow::goBack);
    m_backButton->setEnabled(false);

    m_forwardButton = iconButton(QStringLiteral("headerForwardButton"), QStringLiteral("arrow-right"),
                                 tr("Forward (Alt+Right)"), 18);
    connect(m_forwardButton, &QToolButton::clicked, this, &MainWindow::goForward);
    m_forwardButton->setEnabled(false);

    m_vaultMenuButton = new QToolButton(m_headerBar);
    m_vaultMenuButton->setObjectName(QStringLiteral("vaultMenuButton"));
    m_vaultMenuButton->setText(tr("No vault"));
    m_vaultMenuButton->setCursor(Qt::PointingHandCursor);
    m_vaultMenuButton->setAutoRaise(true);
    m_vaultMenuButton->setPopupMode(QToolButton::InstantPopup);
    m_vaultMenu = new QMenu(m_vaultMenuButton);
    connect(m_vaultMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildVaultMenu);
    m_vaultMenuButton->setMenu(m_vaultMenu);

    m_headerSearch = new QLineEdit(m_headerBar);
    m_headerSearch->setObjectName(QStringLiteral("headerSearch"));
    m_headerSearch->setPlaceholderText(tr("Search or jump to..."));
    m_headerSearch->setMaximumWidth(420);
    m_headerSearch->setClearButtonEnabled(true);
    // No installEventFilter() call of its own needed: the application-wide
    // filter installed in the constructor already sees every widget's
    // events, and installing a second one here on top of it would run
    // eventFilter() - and so openQuickSwitcher() - twice for the same event.
    connect(m_headerSearch, &QLineEdit::returnPressed, this, &MainWindow::openQuickSwitcher);

    QToolButton *newNote = iconButton(QStringLiteral("headerNewNoteButton"), QStringLiteral("plus"),
                                      tr("New note (Ctrl+N)"), 18);
    connect(newNote, &QToolButton::clicked, this, [this] { newNoteIn(QString()); });

    QToolButton *rightSidebarToggle = iconButton(QStringLiteral("headerRightSidebarToggle"),
                                                 QStringLiteral("panel-right"),
                                                 tr("Toggle right sidebar (Ctrl+Alt+R)"), 18);
    connect(rightSidebarToggle, &QToolButton::clicked, this, &MainWindow::toggleRightSidebar);

    QToolButton *settingsButton = iconButton(QStringLiteral("headerSettingsButton"),
                                             QStringLiteral("settings"), tr("Settings (Ctrl+,)"),
                                             18);
    connect(settingsButton, &QToolButton::clicked, this, &MainWindow::openSettings);

    layout->addWidget(sidebarToggle);
    layout->addWidget(m_backButton);
    layout->addWidget(m_forwardButton);
    layout->addWidget(m_vaultMenuButton);
    layout->addStretch(1);
    layout->addWidget(m_headerSearch);
    layout->addStretch(1);
    layout->addWidget(newNote);
    layout->addWidget(rightSidebarToggle);
    layout->addWidget(settingsButton);
    return m_headerBar;
}

void MainWindow::rebuildVaultMenu()
{
    m_vaultMenu->clear();

    if (m_openMode == OpenMode::SingleFile) {
        QAction *openFolder = m_vaultMenu->addAction(tr("Open folder as vault"));
        connect(openFolder, &QAction::triggered, this,
               [this] { openVault(QFileInfo(m_active->notePath).absolutePath()); });
        return;
    }

    QAction *open = m_vaultMenu->addAction(tr("Open folder..."));
    connect(open, &QAction::triggered, this, &MainWindow::chooseVault);

    QStringList others;
    for (const QString &candidate : m_settings.recentVaults()) {
        if (candidate != m_vault.root() && QFileInfo(candidate).isDir())
            others << candidate;
    }
    if (!others.isEmpty()) {
        QMenu *recentMenu = m_vaultMenu->addMenu(tr("Open recent"));
        for (const QString &vaultPath : std::as_const(others)) {
            QAction *action = recentMenu->addAction(QDir(vaultPath).dirName());
            connect(action, &QAction::triggered, this, [this, vaultPath] { openVault(vaultPath); });
        }
    }

    if (m_vault.isValid()) {
        m_vaultMenu->addSeparator();
        QAction *close = m_vaultMenu->addAction(tr("Close vault"));
        connect(close, &QAction::triggered, this, &MainWindow::closeVault);
    }
}

QWidget *MainWindow::buildSidebar()
{
    m_sidebar = new QWidget(this);
    m_sidebar->setObjectName(QStringLiteral("sidebar"));
    m_sidebar->setMinimumWidth(180);

    auto *header = new QWidget(m_sidebar);
    header->setObjectName(QStringLiteral("sidebarHeader"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 10, 8, 6);
    headerLayout->setSpacing(2);

    auto *filesLabel = new QLabel(tr("FILES"), header);
    filesLabel->setObjectName(QStringLiteral("filesLabel"));

    QToolButton *newNote = iconButton(QStringLiteral("sidebarNewNoteButton"),
                                      QStringLiteral("file-plus"), tr("New note"), 14);
    connect(newNote, &QToolButton::clicked, this, [this] { newNoteIn(QString()); });
    QToolButton *newFolder = iconButton(QStringLiteral("sidebarNewFolderButton"),
                                        QStringLiteral("folder-plus"), tr("New folder"), 14);
    connect(newFolder, &QToolButton::clicked, this, [this] { newFolderIn(QString()); });

    headerLayout->addWidget(filesLabel, 1);
    headerLayout->addWidget(newNote);
    headerLayout->addWidget(newFolder);

    m_files = new QFileSystemModel(this);
    m_files->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    m_files->setReadOnly(false);
    m_files->setIconProvider(noIcons());

    m_tree = new QTreeView(m_sidebar);
    m_tree->setObjectName(QStringLiteral("fileExplorer"));
    m_tree->setModel(m_files);
    m_tree->setHeaderHidden(true);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(0, Qt::AscendingOrder);
    m_tree->setAnimated(true);
    m_tree->setIndentation(16);
    m_tree->setUniformRowHeights(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    // Obsidian toggles a folder on a single click; Qt's own double-click
    // expand would add a third toggle on top of the two clicks a double-click
    // is already made of (see onTreeClicked()), so it is switched off here
    // rather than fought with afterwards.
    m_tree->setExpandsOnDoubleClick(false);
    // Enter/Return toggling a folder is handled in eventFilter(), which the
    // application-wide filter installed in the constructor already routes
    // this widget's key events through - a second, tree-specific
    // installEventFilter() here would just run that same check twice.
    for (int column = 1; column < m_files->columnCount(); ++column)
        m_tree->hideColumn(column);
    connect(m_tree, &QTreeView::activated, this, &MainWindow::onTreeActivated);
    connect(m_tree, &QTreeView::clicked, this, &MainWindow::onTreeClicked);
    connect(m_tree, &QTreeView::customContextMenuRequested, this,
           &MainWindow::showTreeContextMenu);
    // QFileSystemModel fills a directory on a worker thread, so the root
    // index is not usable until it says so.
    connect(m_files, &QFileSystemModel::directoryLoaded, this, [this](const QString &path) {
        if (m_vault.isValid() && path == m_vault.root())
            m_tree->setRootIndex(m_files->index(m_vault.root()));
    });

    auto *layout = new QVBoxLayout(m_sidebar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(m_tree, 1);
    return m_sidebar;
}

QWidget *MainWindow::buildMainArea()
{
    auto *notesPage = new QWidget(this);
    notesPage->setObjectName(QStringLiteral("mainArea"));

    const auto resolver = [this](const QString &target) -> QString {
        if (!m_vault.isValid())
            return {};
        // An attachment may not be in the index (it only holds notes), so fall
        // back on the path the note actually wrote.
        const QString resolved = m_index.resolve(target, m_active->notePath);
        if (!resolved.isEmpty())
            return m_vault.absolute(resolved);
        const QString beside =
            QFileInfo(m_active->notePath).absolutePath() + QLatin1Char('/') + target;
        if (QFileInfo::exists(beside))
            return beside;
        const QString fromRoot = m_vault.absolute(target);
        return QFileInfo::exists(fromRoot) ? fromRoot : QString();
    };
    m_resolveLink = resolver;

    m_previewDelay.setSingleShot(true);
    m_previewDelay.setInterval(300);
    connect(&m_previewDelay, &QTimer::timeout, this, &MainWindow::showPagePreview);
    m_templatePicker = new TemplatePicker(this);
    connect(m_templatePicker, &TemplatePicker::templateChosen, this,
            [this](const QString &text) { m_active->editor->insertAtCaret(text); });

    m_workspace = new QSplitter(Qt::Horizontal, notesPage);
    m_workspace->setObjectName(QStringLiteral("workspace"));
    m_workspace->setChildrenCollapsible(false);
    TabGroup *group = newGroup();
    m_workspace->addWidget(group->widget);
    activateLeaf(newLeaf(group));

    auto *notesLayout = new QVBoxLayout(notesPage);
    notesLayout->setContentsMargins(0, 0, 0, 0);
    notesLayout->setSpacing(0);
    notesLayout->addWidget(m_workspace, 1);

    m_settingsPage = new SettingsPage(this);
    connect(m_settingsPage, &SettingsPage::settingChanged, this, &MainWindow::applySettings);
    connect(m_settingsPage, &SettingsPage::hotkeysChanged, this, &MainWindow::applyHotkeys);
    connect(m_settingsPage, &SettingsPage::recoveryRequested, this, &MainWindow::openRecovery);
    connect(m_settingsPage, &SettingsPage::closeRequested, this, &MainWindow::closeSettings);
    connect(m_settingsPage, &SettingsPage::pluginsChanged, this, [this] { syncPlugins(true); });

    m_mainStack = new QStackedWidget(this);
    m_mainStack->setObjectName(QStringLiteral("mainStack"));
    m_mainStack->addWidget(notesPage);
    m_mainStack->addWidget(m_settingsPage);
    return m_mainStack;
}

QString MainWindow::tabBarStyle() const
{
    return QStringLiteral("QTabBar::tab { background: transparent; color: %1; "
                          "padding: 8px 16px 10px 16px; margin: 0; min-width: 90px; max-width: 260px; "
                          "border: none; border-bottom: 2px solid transparent; }"
                          "QTabBar::tab:hover { color: %2; }"
                          "QTabBar::tab:selected { color: %2; border-bottom: 2px solid %3; }"
                          // Only the focused split's tab is marked, as in Obsidian;
                          // the others keep theirs, unaccented.
                          "QTabBar[focusedSplit=\"false\"]::tab:selected { border-bottom-color: %4; }")
        .arg(m_theme.value(QStringLiteral("--text-muted")),
             m_theme.value(QStringLiteral("--text-normal")),
             m_theme.value(QStringLiteral("--interactive-accent")),
             m_theme.value(QStringLiteral("--background-modifier-border")));
}

MainWindow::TabGroup *MainWindow::newGroup()
{
    auto owned = std::make_unique<TabGroup>();
    TabGroup *group = owned.get();
    group->widget = new QWidget;
    group->widget->setObjectName(QStringLiteral("tabGroup"));

    group->strip = new QWidget(group->widget);
    group->strip->setObjectName(QStringLiteral("tabStrip"));
    auto *stripLayout = new QHBoxLayout(group->strip);
    stripLayout->setContentsMargins(6, 4, 6, 0);
    stripLayout->setSpacing(0);
    group->tabs = new QTabBar(group->strip);
    group->tabs->setObjectName(QStringLiteral("noteTabs"));
    group->tabs->setDrawBase(false);
    group->tabs->setExpanding(false);
    group->tabs->setMovable(true);
    group->tabs->setElideMode(Qt::ElideRight);
    group->tabs->setStyleSheet(tabBarStyle());
    stripLayout->addWidget(group->tabs);
    stripLayout->addStretch(1);

    group->stack = new QStackedWidget(group->widget);
    auto *layout = new QVBoxLayout(group->widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(group->strip);
    layout->addWidget(group->stack, 1);

    connect(group->tabs, &QTabBar::currentChanged, this, [this, group](int index) {
        if (m_syncingTabs || index < 0 || index >= group->leaves.size())
            return;
        activateLeaf(group->leaves.at(index));
        m_active->editor->setFocus();
    });
    connect(group->tabs, &QTabBar::tabMoved, this,
            [group](int from, int to) { group->leaves.move(from, to); });
    group->tabs->installEventFilter(this); // a middle click closes a tab

    m_groups.push_back(std::move(owned));
    return group;
}

MainWindow::Leaf *MainWindow::newLeaf(TabGroup *group)
{
    auto owned = std::make_unique<Leaf>();
    Leaf *leaf = owned.get();
    leaf->group = group;
    leaf->area = new QWidget(group->stack);
    leaf->area->setObjectName(QStringLiteral("editorArea"));
    auto *layout = new QVBoxLayout(leaf->area);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ponytail: the inline title is a fixed label above the editor, so it does
    // not scroll away like Obsidian's. It becomes the first block once the
    // Phase 2 engine owns the layout.
    leaf->inlineTitle = new QLabel(leaf->area);
    leaf->inlineTitle->setObjectName(QStringLiteral("inlineTitle"));
    leaf->inlineTitle->setVisible(false);

    // Its Resize and FocusIn events reach eventFilter() through the
    // application-wide filter installed in the constructor.
    MarkdownEditor *editor = new MarkdownEditor(leaf->area);
    leaf->editor = editor;
    editor->setEnabled(false);
    connect(editor, &MarkdownEditor::modificationChanged, this, [this, leaf] {
        if (leaf == m_active)
            updateTitle();
        else
            syncTabs(leaf->group);
    });
    // One note in several tabs is one document, as in Obsidian: an edit in
    // one is made in the others, so neither save can undo the other's work.
    connect(editor, &MarkdownEditor::edited, this,
            [this, leaf](qsizetype at, qsizetype removed, const QString &inserted) {
                for (Leaf *other : viewsOfSameNote(leaf))
                    other->editor->applyExternalEdit(at, removed, inserted);
            });
    // A moment after typing stops, everything unsaved is saved.
    connect(editor, &MarkdownEditor::edited, this, [this] {
        if (m_settings.boolean(QStringLiteral("files.autosave")))
            m_autosave.start();
    });
    // The count comes with each parse. Taking it from textChanged recounted
    // the whole note on the UI thread for every keystroke.
    connect(editor, &MarkdownEditor::wordCountChanged, this, [this, leaf] {
        if (leaf == m_active)
            updateWordCount();
    });
    editor->setLinkResolver(m_resolveLink);
    connect(editor, &MarkdownEditor::linkActivated, this, &MainWindow::followLink);
    connect(editor, &MarkdownEditor::linkHovered, this,
            [this](const QString &target, const QPoint &globalPos) {
                if (!featureOn("page-preview"))
                    return;
                m_previewTarget = target;
                m_previewPos = globalPos;
                m_previewDelay.start();
            });
    connect(editor, &MarkdownEditor::linkHoverEnded, this, [this] {
        m_previewDelay.stop();
        if (m_preview)
            m_preview->leaveSoon();
    });
    // The outline follows the parse and the caret; m_outline is built with the
    // right sidebar, after the first tab, so these look it up when they fire.
    connect(editor, &MarkdownEditor::headingsChanged, this, [this, leaf] {
        if (m_outline && leaf == m_active)
            m_outline->setHeadings(leaf->editor->headings());
    });
    connect(editor, &MarkdownEditor::cursorMoved, this, [this, leaf](qsizetype offset) {
        if (m_outline && leaf == m_active)
            m_outline->setCursorOffset(offset);
    });
    // ```query blocks: Index::search() opens its own connection, so it runs
    // on the thread pool, and the editor brings the results back to its own
    // thread. ~MainWindow waits for the pool, so a search never outlives the
    // index it reads.
    Index *index = &m_index;
    editor->setQueryRunner([index](const QString &query, const MarkdownEditor::QueryDeliver &deliver) {
        (void)QtConcurrent::run([index, query, deliver] {
            QueryResults results;
            for (const SearchResult &found : index->search(query)) {
                QueryResult result{found.path, found.matchCount, found.snippet, {},
                                   found.matches.isEmpty() ? 0 : found.matches.constFirst().offset};
                for (const SearchMatch &match : found.snippetHighlights)
                    result.snippetHighlights.append({match.offset, match.length});
                results.append(result);
            }
            deliver(results);
        });
    });
    connect(editor, &MarkdownEditor::queryResultActivated, this,
            [this](const QString &relativePath, qsizetype offset) {
                openRelative(relativePath);
                m_active->editor->setCursorOffset(offset);
                m_active->editor->setFocus();
            });
    connect(&m_index, &Index::changed, editor, &MarkdownEditor::refreshQueries);
    if (m_applyPluginRenderer)
        m_applyPluginRenderer(editor); // plugin fences in a new tab too
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    // Code blocks through a Jupyter kernel. A lookup (forceRun false) only
    // reads the output cache; the controller, and any kernel, exist only
    // once something is run.
    editor->setNotebookRunner([this](const QString &language, const QString &source, bool forceRun,
                                     const MarkdownEditor::NotebookDeliver &deliver) {
        if (!m_notebook)
            m_notebook = new notebook::NotebookController(this);
        notebook::NotebookController::RunRequest request;
        request.language = language;
        request.source = source;
        m_notebook->run(request, forceRun, [deliver](const notebook::NotebookRunResult &result) {
            MarkdownEditor::NotebookRunResult shown;
            shown.hasRun = result.hasRun;
            shown.ok = result.ok;
            shown.wallMs = result.wallMs;
            shown.kernelError = result.kernelError;
            for (const notebook::NotebookOutput &output : result.outputs) {
                MarkdownEditor::NotebookOutput cell;
                cell.kind = MarkdownEditor::NotebookOutput::Kind(int(output.kind));
                cell.text = output.kind == notebook::NotebookOutput::Kind::Error ? output.evalue : output.text;
                cell.imageBytes = output.imageBytes;
                cell.ename = output.ename;
                cell.traceback = output.traceback;
                shown.outputs.append(cell);
            }
            deliver(shown);
        });
    });
#endif
#ifdef NOTEAPPC_HAVE_BASES
    // ```base blocks: the notes and the evaluation both on the pool (the
    // notes are 54 ms on 20 000); the vault is copied here, on the UI
    // thread, where the editor asks.
    editor->setBaseRunner([this, index](const QString &YAMLtext, const MarkdownEditor::BaseDeliver &deliver) {
        (void)QtConcurrent::run([index, vault = m_vault, YAMLtext, deliver] {
            const bases::BaseEditorTable table =
                bases::evaluateBaseForEditor(YAMLtext, index->allNotesForBases(), &vault);
            BaseTableData result;
            result.viewNames = table.viewNames;
            result.shownView = table.shownView;
            result.headers = table.headers;
            result.summaries = table.summaries;
            result.error = table.error;
            for (const bases::BaseEditorRow &row : table.rows)
                result.rows.append({row.cells, row.notePath});
            deliver(result);
        });
    });
    connect(editor, &MarkdownEditor::baseRowActivated, this, [this](const QString &relativePath) {
        openRelative(relativePath);
        m_active->editor->setFocus();
    });
    connect(&m_index, &Index::changed, editor, &MarkdownEditor::refreshBases);
#endif

    layout->addWidget(leaf->inlineTitle);
    layout->addWidget(editor, 1);
    group->stack->addWidget(leaf->area);
    group->leaves.append(leaf);
    m_leaves.push_back(std::move(owned));
    configureEditor(editor);
    syncTabs(group);
    return leaf;
}

void MainWindow::followLink(const QString &target)
{
    // The web and mail in their own apps; any other scheme a note names
    // (file:, a custom handler) is not something a click should launch.
    if (target.contains(QLatin1String("://")) || target.startsWith(QLatin1String("mailto:"))) {
        const QUrl url(target);
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto"))
            QDesktopServices::openUrl(url);
        else
            statusBar()->showMessage(tr("Links with %1: are not opened").arg(scheme), 3000);
        return;
    }
    // A fragment goes with the file: a note's heading, a book's place, a
    // PDF's page. [[#Heading]] alone is this note's.
    const qsizetype hash = target.indexOf(QLatin1Char('#'));
    const QString name = hash < 0 ? target : target.left(hash);
    const QString fragment = hash < 0 ? QString() : target.mid(hash + 1);
    if (name.isEmpty()) {
        if (!fragment.startsWith(QLatin1Char('^')))
            jumpToHeading(fragment);
        else if (const qsizetype at = subpathOffset(m_active->editor->noteText(), fragment); at >= 0)
            m_active->editor->setCursorOffset(at);
        return;
    }
    const QString resolved = m_index.resolve(name, m_active->notePath);
    if (resolved.isEmpty()) {
        statusBar()->showMessage(tr("%1 is not in this vault yet").arg(name), 3000);
        return;
    }
    if (isViewFile(resolved)) {
        openNote(m_vault.absolute(resolved), NavSource::Other, fragment);
        return;
    }
    if (!openNote(m_vault.absolute(resolved)) || fragment.isEmpty())
        return;
    if (!fragment.startsWith(QLatin1Char('^'))) {
        jumpToHeading(fragment);
    } else if (const qsizetype at = subpathOffset(m_active->editor->noteText(), fragment); at >= 0) {
        m_active->editor->setCursorOffset(at);
    }
}

MarkdownEditor::Mode MainWindow::defaultMode() const
{
    const QString mode = m_settings.text(QStringLiteral("editor.defaultMode"));
    return mode == QLatin1String("source")    ? MarkdownEditor::Mode::Source
           : mode == QLatin1String("reading") ? MarkdownEditor::Mode::Reading
                                               : MarkdownEditor::Mode::LivePreview;
}

void MainWindow::applyEditorSettings(MarkdownEditor *editor)
{
    editor->setReadableLineLength(m_settings.boolean(QStringLiteral("editor.readableLineLength")));
    editor->setContentWidth(m_settings.integer(QStringLiteral("editor.contentWidth")));
    editor->setLineNumbers(m_settings.boolean(QStringLiteral("editor.lineNumbers")));
    editor->setAutoPairs(m_settings.boolean(QStringLiteral("editor.autoPairs")));
    editor->setStrictLineBreaks(m_settings.boolean(QStringLiteral("editor.strictLineBreaks")));
    editor->setIndentGuides(m_settings.boolean(QStringLiteral("editor.showIndentGuides")));
    editor->setOutlineNumbering(m_settings.boolean(QStringLiteral("editor.outlineNumbering")));
    editor->setAlignTables(m_settings.boolean(QStringLiteral("editor.alignTables")));
    editor->setTabWidth(m_settings.integer(QStringLiteral("editor.tabWidth")));
    editor->setUseWikilinks(m_settings.boolean(QStringLiteral("links.useWikilinks")));
    editor->setGPUacceleration(m_settings.boolean(QStringLiteral("editor.gpuAcceleration")));

    // What the editor draws of the optional features: off, a block shows
    // its source and none of that feature's code runs.
    MarkdownEditor::Features features;
    features.queryBlocks = featureOn("global-search"); // embedded search is Search's, in Obsidian
    features.baseBlocks = featureOn("bases");
    features.HTMLblocks = featureOn("html-blocks");
    features.PDFembeds = featureOn("pdf-embeds");
    features.texEmbeds = featureOn("tex-embeds");
    features.mediaEmbeds = featureOn("media-embeds");
    features.notebooks = featureOn("notebooks");
    editor->setFeatures(features);
    // "/" commands: the palette's own list, the window's then the editor's,
    // fetched when the popup opens. Off, "/" is a character like any other.
    if (!featureOn("slash-command")) {
        editor->setSlashCommands(nullptr, nullptr);
    } else {
        editor->setSlashCommands(
            [this] {
                QList<MarkdownEditor::SlashCommand> commands;
                const auto keysOf = [](const QList<QKeySequence> &keys) {
                    return keys.isEmpty() ? QString() : keys.constFirst().toString(QKeySequence::NativeText);
                };
                for (const QAction *action : std::as_const(m_commands)) {
                    if (action->isEnabled() && action->objectName() != QLatin1String("command-palette:open"))
                        commands.append({action->objectName(), action->text(), keysOf(action->shortcuts())});
                }
                for (const MarkdownEditor::Command &command : MarkdownEditor::commands())
                    commands.append({command.id, command.name, keysOf(m_editorBindings.value(command.id, command.keys))});
                return commands;
            },
            [this, editor](const QString &id) {
                if (auto *action = findChild<QAction *>(id, Qt::FindDirectChildrenOnly))
                    action->trigger();
                else
                    editor->runCommand(id);
            });
    }
#ifdef NOTEAPPC_HAVE_CANVAS
    // A canvas embed is the canvas drawn into an image, framed to its cards
    // - the editor cannot hold a widget in its text. Drawn a turn later, so
    // the note paints first with the box in place.
    const MarkdownEditor::FileEmbedRenderer canvasEmbed =
        [this](const QString &path, const QString &, int width, const std::function<void(QImage)> &deliver) {
            QTimer::singleShot(0, this, [this, path, width, deliver] {
                CanvasView canvas;
                canvas.setTheme(m_theme);
                canvas.setFileResolver(m_resolveLink);
                if (!canvas.load(path)) {
                    deliver({});
                    return;
                }
                // Its own shape, within reason, so the cards are not
                // shrunk past reading to fit a letterbox.
                const QRectF bounds = canvas.contentBounds();
                const qreal aspect = bounds.isEmpty() ? 0.5625 : bounds.height() / bounds.width();
                canvas.resize(width, qRound(width * qBound(0.3, aspect, 1.0)));
                canvas.setAlwaysDrawContent(true);
                canvas.fitToContent();
                const qreal ratio = devicePixelRatioF();
                QImage image(canvas.size() * ratio, QImage::Format_ARGB32_Premultiplied);
                image.setDevicePixelRatio(ratio);
                image.fill(Qt::transparent);
                canvas.render(&image);
                deliver(image);
            });
        };
    editor->setFileEmbedRenderer({QStringLiteral("canvas")},
                                 featureOn("canvas") ? canvasEmbed : MarkdownEditor::FileEmbedRenderer());
#endif
}

void MainWindow::configureEditor(MarkdownEditor *editor)
{
    editor->setMode(defaultMode());
    editor->setTheme(m_theme);
    applyEditorSettings(editor);
    editor->setKeyBindings(m_editorBindings);
    if (m_spellHanded)
        editor->setSpelling(m_editorSpelling);
    if (m_index.isOpen())
        editor->setLinkTargets(m_index.linkTargets());

    // Paste and drop: the files go into the vault's attachment folder for the
    // editor's note, by Obsidian's rules; a note outside a vault takes none.
    const auto noteOf = [this, editor]() -> QString {
        for (const auto &leaf : m_leaves) {
            if (leaf->editor == editor)
                return m_vault.isValid() && m_vault.contains(leaf->notePath) ? leaf->notePath : QString();
        }
        return {};
    };
    const auto attachmentFolder = [this](const QString &note) {
        const QString folder =
            m_vault.attachmentFolderFor(m_settings.text(QStringLiteral("files.attachmentFolder")), note);
        QDir().mkpath(folder);
        return folder;
    };
    // ponytail: embeds by file name, Obsidian's "shortest path"; two files
    // of one name in the vault resolve to whichever the index finds first.
    editor->setAttachmentSaver([this, noteOf, attachmentFolder](const QString &name, const QByteArray &bytes) {
        const QString note = noteOf();
        if (note.isEmpty()) {
            statusBar()->showMessage(tr("Images are pasted into notes in a vault"), 4000);
            return QString();
        }
        const QString path = Vault::uniqueAttachmentPath(attachmentFolder(note), name);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            statusBar()->showMessage(tr("Could not save %1: %2").arg(name, file.errorString()), 5000);
            return QString();
        }
        return QStringLiteral("![[") + QFileInfo(path).fileName() + QStringLiteral("]]");
    });
    editor->setFileImporter([this, noteOf, attachmentFolder](const QString &source) {
        const QFileInfo info(source);
        if (!info.isFile())
            return QString();
        // Already in the vault: a link to it, copied nowhere.
        if (m_vault.isValid() && m_vault.contains(info.absoluteFilePath())) {
            return Vault::isNote(source) ? QStringLiteral("[[") + info.completeBaseName() + QStringLiteral("]]")
                                         : QStringLiteral("![[") + info.fileName() + QStringLiteral("]]");
        }
        const QString note = noteOf();
        if (note.isEmpty())
            return source; // no vault to copy into: the path, as text
        const QString target = Vault::uniqueAttachmentPath(attachmentFolder(note), info.fileName());
        if (!QFile::copy(source, target)) {
            statusBar()->showMessage(tr("Could not copy %1 into the vault").arg(info.fileName()), 5000);
            return QString();
        }
        return QStringLiteral("![[") + QFileInfo(target).fileName() + QStringLiteral("]]");
    });
}

void MainWindow::activateLeaf(Leaf *leaf)
{
    // The focused tab can still owe its deferred file: a restored layout
    // reuses the tab that was already focused.
    if (!leaf || (leaf == m_active && leaf->pendingPath.isEmpty()))
        return;
    TabGroup *previousGroup = m_active ? m_active->group : nullptr;
    m_active = leaf;
    if (leaf->group != previousGroup) {
        for (const auto &group : m_groups) {
            group->tabs->setProperty("focusedSplit", group.get() == leaf->group);
            // A dynamic property only restyles on a fresh polish.
            group->tabs->style()->unpolish(group->tabs);
            group->tabs->style()->polish(group->tabs);
        }
    }
    TabGroup *group = leaf->group;
    group->stack->setCurrentWidget(leaf->area);
    const int index = group->leaves.indexOf(leaf);
    if (index < group->tabs->count()) {
        const QSignalBlocker blocker(group->tabs);
        group->tabs->setCurrentIndex(index);
    }
    // The right sidebar is built after the first tab, which is activated
    // before there is one.
    if (!m_rightPanes)
        return;
    if (!leaf->pendingPath.isEmpty() && openNote(std::exchange(leaf->pendingPath, QString())))
        return; // opening it filled the panes
    const QString &path = leaf->notePath;
    showNoteInPanes(path);
    if (!path.isEmpty()) {
        m_settings.noteOpened(m_vault.isValid() ? m_vault.root() : QString(), path);
        revealInTree(path);
    }
    updateTitle();
    updateWordCount();
    updateHistoryButtons();
}

QString MainWindow::tabTitle(const Leaf *leaf) const
{
    if (leaf->notePath.isEmpty() && !leaf->pendingPath.isEmpty())
        return QFileInfo(leaf->pendingPath).completeBaseName();
    if (leaf->notePath.isEmpty())
        return tr("New tab");
    const QString name = QFileInfo(leaf->notePath).completeBaseName();
    return isModified(leaf) ? name + QLatin1String(" *") : name;
}

void MainWindow::syncTabs(TabGroup *group)
{
    const bool soleEmpty = m_leaves.size() == 1 && group->leaves.size() == 1
                        && group->leaves.constFirst()->notePath.isEmpty()
                        && group->leaves.constFirst()->pendingPath.isEmpty();
    const int wanted = soleEmpty ? 0 : int(group->leaves.size());
    m_syncingTabs = true;
    while (group->tabs->count() > wanted) {
        // setTabButton() reparents the close button to the tab bar but
        // removeTab() does not delete it, so it would otherwise linger as an
        // invisible orphan every time a tab is closed. Later, not now: this
        // may run inside that very button's clicked().
        if (QWidget *button = group->tabs->tabButton(group->tabs->count() - 1, QTabBar::RightSide))
            button->deleteLater();
        group->tabs->removeTab(group->tabs->count() - 1);
    }
    while (group->tabs->count() < wanted) {
        const int index = group->tabs->addTab(QString());
        // The style's own close glyph pulls from the desktop icon theme and
        // can come out looking nothing like the rest of the app (a red
        // "document-close" square on this box); a Lucide icon through the
        // same tinting path as every other button keeps it consistent.
        QToolButton *closeButton =
            iconButton(QStringLiteral("tabCloseButton"), QStringLiteral("x"), tr("Close"), 12);
        // applyTheme() only re-icons buttons that already exist when it
        // runs; this one is born between theme applications, so it needs
        // its first icon set directly or it sits there with none at all.
        closeButton->setIcon(
            Icons::icon(QStringLiteral("x"), m_theme.color(QStringLiteral("--text-muted")), 12));
        connect(closeButton, &QToolButton::clicked, this, [this, group, closeButton] {
            // Looked up at click time: tabs move.
            for (int i = 0; i < group->tabs->count(); ++i) {
                if (group->tabs->tabButton(i, QTabBar::RightSide) == closeButton) {
                    closeLeaf(group->leaves.at(i));
                    return;
                }
            }
        });
        group->tabs->setTabButton(index, QTabBar::RightSide, closeButton);
    }
    for (int i = 0; i < wanted; ++i)
        group->tabs->setTabText(i, tabTitle(group->leaves.at(i)));
    const int active = group->leaves.indexOf(m_active);
    if (active >= 0 && active < wanted)
        group->tabs->setCurrentIndex(active);
    m_syncingTabs = false;
    group->strip->setVisible(m_openMode != OpenMode::SingleFile);
}

void MainWindow::dropLeaf(Leaf *leaf)
{
    TabGroup *group = leaf->group;
    const int index = group->leaves.indexOf(leaf);
    group->leaves.removeAt(index);
    // Nothing of this tab may call back in once it is gone: its lambdas hold
    // the Leaf pointer.
    leaf->editor->disconnect(this);
    group->stack->removeWidget(leaf->area);
    leaf->area->hide();
    leaf->area->deleteLater();

    Leaf *next = nullptr;
    if (!group->leaves.isEmpty()) {
        next = group->leaves.at(qMin(index, int(group->leaves.size()) - 1));
    } else {
        // The split goes with its last tab, and a split left holding one
        // child hands it to its own parent.
        auto *parent = qobject_cast<QSplitter *>(group->widget->parentWidget());
        group->widget->hide();
        group->widget->setParent(nullptr);
        group->widget->deleteLater();
        while (parent && parent != m_workspace && parent->count() == 1) {
            auto *grandparent = qobject_cast<QSplitter *>(parent->parentWidget());
            QWidget *only = parent->widget(0);
            grandparent->replaceWidget(grandparent->indexOf(parent), only);
            only->show();
            parent->deleteLater();
            parent = grandparent;
        }
        std::erase_if(m_groups, [group](const auto &owned) { return owned.get() == group; });
        group = nullptr;
    }
    std::erase_if(m_leaves, [leaf](const auto &owned) { return owned.get() == leaf; });
    if (m_active == leaf)
        m_active = nullptr;
    if (!next)
        next = m_leaves.front().get();
    activateLeaf(next);
    if (group)
        syncTabs(group);
    syncTabs(next->group);
}

void MainWindow::closeLeaf(Leaf *leaf)
{
    activateLeaf(leaf);
    if (!maybeLeave())
        return;
    if (m_leaves.size() == 1) {
        clearOpenNote();
        updateTitle();
        updateWordCount();
        return;
    }
    rememberCurrentPosition();
    dropLeaf(leaf);
    m_active->editor->setFocus();
}

void MainWindow::newTab()
{
    activateLeaf(newLeaf(m_active->group));
    syncTabs(m_active->group);
    m_active->editor->setFocus();
}

void MainWindow::cycleTab(int step)
{
    const QList<Leaf *> &leaves = m_active->group->leaves;
    if (leaves.size() < 2)
        return;
    const int index = (leaves.indexOf(m_active) + step + int(leaves.size())) % int(leaves.size());
    activateLeaf(leaves.at(index));
    m_active->editor->setFocus();
}

void MainWindow::splitActive(Qt::Orientation orientation)
{
    TabGroup *from = m_active->group;
    const QString path = m_active->notePath;
    TabGroup *group = newGroup();
    auto *parent = qobject_cast<QSplitter *>(from->widget->parentWidget());
    const int at = parent->indexOf(from->widget);
    if (parent->count() == 1 || parent->orientation() == orientation) {
        parent->setOrientation(orientation);
        parent->insertWidget(at + 1, group->widget);
    } else {
        auto *split = new QSplitter(orientation);
        split->setChildrenCollapsible(false);
        parent->replaceWidget(at, split);
        split->addWidget(from->widget);
        split->addWidget(group->widget);
        from->widget->show();
    }
    // Share the space evenly between the old split and the new one.
    auto *holder = qobject_cast<QSplitter *>(group->widget->parentWidget());
    const int index = holder->indexOf(group->widget);
    QList<int> sizes = holder->sizes();
    const int half = (sizes.value(index - 1) + sizes.value(index)) / 2;
    sizes[index - 1] = half;
    sizes[index] = half;
    holder->setSizes(sizes);

    activateLeaf(newLeaf(group));
    if (!path.isEmpty())
        openNote(path);
    m_active->editor->setFocus();
}

bool MainWindow::maybeSaveAll()
{
    QSet<QString> asked; // a note in two tabs is one document, asked about once
    for (const auto &leaf : m_leaves) {
        if (leaf->notePath.isEmpty() || !isModified(leaf.get()) || asked.contains(leaf->notePath))
            continue;
        asked.insert(leaf->notePath);
        activateLeaf(leaf.get());
        if (!maybeSave())
            return false;
    }
    return true;
}

bool MainWindow::maybeLeave()
{
    // Another tab still holds the same document: this one can go, or move
    // on to another note, without losing anything.
    return !viewsOfSameNote(m_active).isEmpty() || maybeSave();
}

void MainWindow::resetWorkspace()
{
    while (m_leaves.size() > 1) {
        Leaf *last = m_leaves.back().get();
        activateLeaf(last);
        rememberCurrentPosition();
        dropLeaf(last);
    }
    clearOpenNote();
}

QToolButton *MainWindow::rightSidebarTabButton(QWidget *parent, const QString &objectName,
                                               const QString &icon, const QString &label,
                                               const QString &tooltip, QWidget *pane)
{
    auto *button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setText(label);
    button->setToolTip(tooltip);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setIconSize(QSize(14, 14));
    button->setProperty("iconName", icon);
    connect(button, &QToolButton::toggled, this, [this, button](bool) { updateTabButtonIcon(button); });
    button->setProperty("pane", QVariant::fromValue<QObject *>(pane));
    connect(button, &QToolButton::clicked, this, [this, pane] { m_rightPanes->setCurrentWidget(pane); });
    return button;
}

void MainWindow::updateTabButtonIcon(QToolButton *button)
{
    const QString name = button->property("iconName").toString();
    if (name.isEmpty())
        return;
    const QColor color = button->isChecked() ? m_theme.color(QStringLiteral("--interactive-accent"))
                                              : m_theme.color(QStringLiteral("--text-muted"));
    button->setIcon(Icons::icon(name, color, button->iconSize().width()));
}

QWidget *MainWindow::buildRightSidebar()
{
    m_rightSidebar = new QWidget(this);
    m_rightSidebar->setObjectName(QStringLiteral("rightSidebar"));
    m_rightSidebar->setMinimumWidth(200);

    auto *tabRow = new QWidget(m_rightSidebar);
    tabRow->setObjectName(QStringLiteral("rightSidebarTabs"));
    auto *tabLayout = new QHBoxLayout(tabRow);
    tabLayout->setContentsMargins(6, 6, 6, 2);
    tabLayout->setSpacing(2);
    tabLayout->addStretch(1);
    m_rightTabs = new QButtonGroup(tabRow);
    m_rightTabs->setExclusive(true);

    // The panes and their tabs come from applyFeatures(), each only while
    // its feature is on.
    m_rightPanes = new QStackedWidget(m_rightSidebar);
    m_rightPanes->setObjectName(QStringLiteral("rightSidebarPanes"));

    auto *layout = new QVBoxLayout(m_rightSidebar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tabRow);
    layout->addWidget(m_rightPanes, 1);
    return m_rightSidebar;
}

void MainWindow::applyFeatures()
{
    // The side panes, in their tab order. One that is off is not built: no
    // widget, nothing connected to the index, nothing refreshed per note.
    struct Pane {
        const char *feature;
        QWidget *widget;
        std::function<QWidget *()> build;
        const char *button;
        const char *icon;
        QString tooltip;
    };
    QWidget *parent = m_rightPanes;
    const Pane panes[] = {
        {"global-search", m_search,
         [this, parent] {
             m_search = new SearchPane(parent);
             m_search->setTheme(m_theme); // applyTheme runs only when the theme changes
             m_search->setIndex(&m_index);
             connect(m_search, &SearchPane::matchActivated, this,
                     [this](const QString &relativePath, qsizetype offset) {
                         openRelative(relativePath);
                         m_active->editor->setCursorOffset(offset);
                         m_active->editor->setFocus();
                     });
             return m_search.data();
         },
         "searchTabButton", "search", tr("Search")},
        {"backlink", m_backlinks,
         [this, parent] {
             m_backlinks = new BacklinksPane(parent);
             m_backlinks->setTheme(m_theme);
             m_backlinks->setIndex(&m_index);
             connect(m_backlinks, &BacklinksPane::noteActivated, this, &MainWindow::openRelative);
             return m_backlinks.data();
         },
         "backlinksTabButton", "link", tr("Backlinks")},
        {"tag-pane", m_tags,
         [this, parent] {
             m_tags = new TagsPane(parent);
             m_tags->setTheme(m_theme);
             m_tags->setIndex(&m_index);
             // A tag opens a search for it, as in Obsidian; tag: already
             // finds the nested tags beneath it.
             connect(m_tags, &TagsPane::tagActivated, this,
                     [this](const QString &tag) { openSearch(QStringLiteral("tag:#%1").arg(tag)); });
             return m_tags.data();
         },
         "tagsTabButton", "tags", tr("Tags")},
        {"properties", m_properties,
         [this, parent] {
             m_properties = new PropertiesPane(parent);
             m_properties->setTheme(m_theme);
             m_properties->setIndex(&m_index);
             connect(m_properties, &PropertiesPane::propertyActivated, this,
                     [this](const QString &key) { openSearch(QStringLiteral("[%1]").arg(key)); });
             return m_properties.data();
         },
         "propertiesTabButton", "clipboard-list", tr("All properties")},
        {"outgoing-link", m_outgoingLinks,
         [this, parent] {
             m_outgoingLinks = new OutgoingLinksPane(parent);
             m_outgoingLinks->setTheme(m_theme);
             m_outgoingLinks->setIndex(&m_index);
             connect(m_outgoingLinks, &OutgoingLinksPane::noteActivated, this, &MainWindow::openRelative);
             return m_outgoingLinks.data();
         },
         "outgoingLinksTabButton", "arrow-up-right", tr("Outgoing links")},
        {"outline", m_outline,
         [this, parent] {
             m_outline = new OutlinePane(parent);
             connect(m_outline, &OutlinePane::jumpRequested, this, [this](qsizetype offset) {
                 m_active->editor->setCursorOffset(offset);
                 m_active->editor->setFocus();
             });
             return m_outline.data();
         },
         "outlineTabButton", "list", tr("Outline")},
        {"bookmarks", m_bookmarks,
         [this, parent] {
             m_bookmarks = new BookmarksPane(parent);
             m_bookmarks->setVaultRoot(m_vault.isValid() ? m_vault.root() : QString());
             connect(m_bookmarks, &BookmarksPane::noteActivated, this, &MainWindow::openRelative);
             connect(m_bookmarks, &BookmarksPane::headingActivated, this,
                     [this](const QString &relativePath, const QString &subpath) {
                         openRelative(relativePath);
                         jumpToHeading(subpath);
                     });
             connect(m_bookmarks, &BookmarksPane::searchActivated, this,
                     [this](const QString &query) { openSearch(query); });
             return m_bookmarks.data();
         },
         "bookmarksTabButton", "bookmark", tr("Bookmarks")},
#ifdef NOTEAPPC_HAVE_GRAPH
        {"graph", m_localGraph,
         [this, parent] {
             // Cheap while hidden: GraphView reads the index only once shown.
             m_localGraph = new GraphView(parent);
             m_localGraph->setObjectName(QStringLiteral("localGraph"));
             m_localGraph->setLocalGraph(true);
             m_localGraph->setTheme(m_theme);
             m_localGraph->setIndex(&m_index);
             connect(m_localGraph, &GraphView::noteActivated, this, &MainWindow::openRelative);
             return m_localGraph.data();
         },
         "localGraphTabButton", "git-fork", tr("Local graph")},
#endif
    };

    const bool first = m_rightPanes->count() == 0;
    auto *tabLayout = static_cast<QHBoxLayout *>(static_cast<QWidget *>(m_rightTabs->parent())->layout());
    bool built = false;
    int slot = 0;
    for (const Pane &pane : panes) {
        auto *button = m_rightSidebar->findChild<QToolButton *>(QLatin1String(pane.button));
        if (!featureOn(pane.feature)) {
            delete button;
            delete pane.widget; // its QPointer member nulls itself
            continue;
        }
        if (!pane.widget) {
            QWidget *widget = pane.build();
            m_rightPanes->addWidget(widget);
            button = rightSidebarTabButton(tabLayout->parentWidget(), QLatin1String(pane.button),
                                           QLatin1String(pane.icon), pane.tooltip, pane.tooltip, widget);
            // Seven panes do not fit as words; Obsidian's sidebar tabs are icons.
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            updateTabButtonIcon(button);
            m_rightTabs->addButton(button);
            tabLayout->insertWidget(slot, button);
            built = true;
        }
        ++slot;
    }
    if (m_backlinks)
        m_backlinks->setUnlinkedMentions(featureOn("unlinked-mentions"));
    if (first && m_backlinks)
        m_rightPanes->setCurrentWidget(m_backlinks); // Obsidian opens on backlinks
    for (QAbstractButton *tab : m_rightTabs->buttons()) {
        if (tab->property("pane").value<QObject *>() == m_rightPanes->currentWidget())
            tab->setChecked(true);
    }
    if (built && m_active)
        showNoteInPanes(m_active->notePath);

#ifdef NOTEAPPC_HAVE_GRAPH
    if (m_graph && !featureOn("graph")) {
        if (m_mainStack->currentWidget() == m_graph)
            leaveSettings();
        delete m_graph;
    }
#endif
    if (!featureOn("page-preview"))
        delete m_preview;
    m_wordCount->setVisible(featureOn("word-count"));

    // A command is its plugin's while its id starts with that plugin's id,
    // the way Obsidian names them ("graph:open", "daily-notes").
    for (QAction *action : std::as_const(m_commands)) {
        const QString id = action->objectName();
        const QString plugin = id == QLatin1String("insert-template") ? QStringLiteral("templates")
                                                                      : id.section(QLatin1Char(':'), 0, 0);
        for (const Settings::Feature &feature : Settings::features()) {
            if (plugin == QLatin1String(feature.id))
                action->setEnabled(featureOn(feature.id));
        }
    }
    // Switched off mid-recording: its Stop command just went dark, so stop
    // here rather than leave the microphone on.
    if (m_recorder && !featureOn("audio-recorder"))
        m_recorder->stop();
    if (m_painted)
        syncPlugins();
}

void MainWindow::showNoteInPanes(const QString &path)
{
    if (m_backlinks)
        m_backlinks->setNote(path);
    if (m_outgoingLinks)
        m_outgoingLinks->setNote(path);
#ifdef NOTEAPPC_HAVE_GRAPH
    if (m_localGraph)
        m_localGraph->setCurrentNote(m_vault.isValid() && !path.isEmpty() ? m_vault.relative(path) : QString());
#endif
    if (m_outline && path.isEmpty())
        m_outline->clear();
    else if (m_outline && m_active && path == m_active->notePath && !m_active->view) {
        m_outline->setHeadings(m_active->editor->headings());
        m_outline->setCursorOffset(m_active->editor->cursorOffset());
    }
}

void MainWindow::buildActions()
{
    // No menu bar: Obsidian has none. Every command is an action named by its
    // Obsidian command id, so a hotkeys.json rebinds it and the command
    // palette, when it comes, lists the same set.
    const auto add = [this](const QString &id, const QString &name, const QString &keys,
                            auto slot) {
        auto *action = new QAction(name, this);
        action->setObjectName(id);
        action->setData(keys); // the built-in binding, for applyHotkeys()
        action->setShortcut(QKeySequence(keys));
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this, slot);
        addAction(action);
        m_commands << action;
    };
    add(QStringLiteral("app:open-vault"), tr("Open another vault"), QStringLiteral("Ctrl+Shift+O"),
        &MainWindow::chooseVault);
    add(QStringLiteral("editor:save-file"), tr("Save current file"), QStringLiteral("Ctrl+S"),
        [this] { save(); });
    add(QStringLiteral("app:toggle-left-sidebar"), tr("Toggle left sidebar"),
        QStringLiteral("Ctrl+Alt+S"), &MainWindow::toggleSidebar);
    add(QStringLiteral("app:toggle-right-sidebar"), tr("Toggle right sidebar"),
        QStringLiteral("Ctrl+Alt+R"), &MainWindow::toggleRightSidebar);
    add(QStringLiteral("workspace:new-tab"), tr("New tab"), QStringLiteral("Ctrl+T"),
        &MainWindow::newTab);
    add(QStringLiteral("workspace:close"), tr("Close current tab"), QStringLiteral("Ctrl+W"),
        [this] { closeLeaf(m_active); });
    add(QStringLiteral("workspace:split-vertical"), tr("Split right"), QString(),
        [this] { splitActive(Qt::Horizontal); });
    add(QStringLiteral("workspace:split-horizontal"), tr("Split down"), QString(),
        [this] { splitActive(Qt::Vertical); });
    for (int i = 1; i <= 8; ++i) {
        add(QStringLiteral("workspace:goto-tab-%1").arg(i), tr("Go to tab #%1").arg(i),
            QStringLiteral("Ctrl+%1").arg(i), [this, i] { gotoTab(i - 1); });
    }
    add(QStringLiteral("workspace:goto-last-tab"), tr("Go to last tab"), QStringLiteral("Ctrl+9"),
        [this] { gotoTab(-1); });
    add(QStringLiteral("workspace:close-others"), tr("Close all other tabs"), QString(), [this] {
        const QList<Leaf *> others = [this] {
            QList<Leaf *> list;
            for (Leaf *leaf : m_active->group->leaves)
                if (leaf != m_active)
                    list << leaf;
            return list;
        }();
        Leaf *keep = m_active;
        for (Leaf *leaf : others) {
            closeLeaf(leaf);
            if (std::none_of(m_leaves.cbegin(), m_leaves.cend(),
                             [leaf](const auto &owned) { return owned.get() == leaf; }))
                continue;
            break; // cancelled at a save prompt
        }
        activateLeaf(keep);
    });
#ifdef NOTEAPPC_HAVE_HISTORY
    add(QStringLiteral("note-history:open"), tr("Note history: Show this note's history"), QString(),
        &MainWindow::openNoteHistory);
#endif
    add(QStringLiteral("workspaces:open-modal"), tr("Workspaces: Manage workspace layouts"), QString(),
        &MainWindow::manageLayouts);
    add(QStringLiteral("workspaces:load"), tr("Workspaces: Load workspace layout"), QString(), [this] {
        if (!m_vault.isValid())
            return;
        const QStringList names =
            Workspaces::named(m_vault.root()).value(QStringLiteral("workspaces")).toObject().keys();
        bool ok = false;
        const QString name = QInputDialog::getItem(this, tr("Load workspace layout"), tr("Layout:"),
                                                   names, 0, false, &ok);
        if (ok)
            loadNamedLayout(name);
    });
    add(QStringLiteral("workspaces:save-and-load"),
        tr("Workspaces: Save and load another workspace layout"), QString(), [this] {
            if (!m_vault.isValid())
                return;
            QJsonObject all = Workspaces::named(m_vault.root());
            const QString active = all.value(QStringLiteral("active")).toString();
            QJsonObject layouts = all.value(QStringLiteral("workspaces")).toObject();
            if (!active.isEmpty() && layouts.contains(active)) {
                layouts.insert(active, layoutJSON());
                all.insert(QStringLiteral("workspaces"), layouts);
                Workspaces::saveNamed(m_vault.root(), all);
            }
            findChild<QAction *>(QStringLiteral("workspaces:load"))->trigger();
        });
    add(QStringLiteral("workspace:next-tab"), tr("Go to next tab"), QStringLiteral("Ctrl+Tab"),
        [this] { cycleTab(1); });
    add(QStringLiteral("workspace:previous-tab"), tr("Go to previous tab"),
        QStringLiteral("Ctrl+Shift+Tab"), [this] { cycleTab(-1); });
    add(QStringLiteral("switcher:open"), tr("Quick switcher: Open quick switcher"),
        QStringLiteral("Ctrl+O"), &MainWindow::openQuickSwitcher);
    add(QStringLiteral("command-palette:open"), tr("Command palette: Open command palette"),
        QStringLiteral("Ctrl+P"), &MainWindow::openCommandPalette);
    add(QStringLiteral("global-search:open"), tr("Search: Search in all files"),
        QStringLiteral("Ctrl+Shift+F"), [this] { openSearch(); });
#ifdef NOTEAPPC_HAVE_BASES
    // ponytail: Obsidian's own id for this command is unconfirmed; this one
    // follows canvas:new-file's shape. A hotkeys.json naming another will not bind.
    add(QStringLiteral("bases:new-file"), tr("Bases: Create new base"), QString(), [this] {
        if (!m_vault.isValid())
            return;
        QString error;
        const QString path = m_vault.createNote(QString(), tr("Untitled"), &error, QStringLiteral(".base"),
                                                bases::BaseFile::newFileTemplate());
        if (path.isEmpty())
            QMessageBox::warning(this, tr("New base"), tr("Could not create the base:\n%1").arg(error));
        else
            openNote(path);
    });
#endif
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    add(QStringLiteral("notebooks:run-block"), tr("Notebooks: Run the code block at the cursor"),
        QStringLiteral("Ctrl+Shift+Return"), [this] {
            if (!m_active->view)
                m_active->editor->runNotebookBlockAt(m_active->editor->cursorOffset());
        });
    add(QStringLiteral("notebooks:run-all"), tr("Notebooks: Run every code block in this note"), QString(), [this] {
        if (!m_active->view && m_active->editor->runAllNotebookBlocks() == 0)
            statusBar()->showMessage(tr("No code block here can run"), 3000);
    });
    // The kernel of the block at the cursor: one kernel per language.
    add(QStringLiteral("notebooks:interrupt"), tr("Notebooks: Interrupt the kernel"), QString(), [this] {
        if (m_notebook && !m_active->view)
            m_notebook->interruptKernel(m_active->editor->codeLanguageAt(m_active->editor->cursorOffset()));
    });
    add(QStringLiteral("notebooks:restart"), tr("Notebooks: Restart the kernel"), QString(), [this] {
        if (m_notebook && !m_active->view)
            m_notebook->restartKernel(m_active->editor->codeLanguageAt(m_active->editor->cursorOffset()));
    });
#endif
    add(QStringLiteral("audio-recorder:start"), tr("Audio recorder: Start recording audio"), QString(),
        &MainWindow::startRecording);
    add(QStringLiteral("audio-recorder:stop"), tr("Audio recorder: Stop recording audio"), QString(), [this] {
        if (m_recorder)
            m_recorder->stop();
    });
    add(QStringLiteral("note-composer:merge-file"), tr("Note composer: Merge current file with another file"),
        QString(), &MainWindow::mergeCurrentNote);
    add(QStringLiteral("note-composer:split-file"), tr("Note composer: Extract current selection"), QString(),
        &MainWindow::extractSelection);
#ifdef NOTEAPPC_HAVE_CANVAS
    add(QStringLiteral("canvas:new-file"), tr("Canvas: Create new canvas"), QString(), [this] {
        if (!m_vault.isValid())
            return;
        QString error;
        const QString path =
            m_vault.createNote(QString(), tr("Untitled"), &error, QStringLiteral(".canvas"), QStringLiteral("{}\n"));
        if (path.isEmpty())
            QMessageBox::warning(this, tr("New canvas"), tr("Could not create the canvas:\n%1").arg(error));
        else
            openNote(path);
    });
#endif
#ifdef NOTEAPPC_HAVE_GRAPH
    add(QStringLiteral("graph:open"), tr("Graph view: Open graph view"), QStringLiteral("Ctrl+G"),
        &MainWindow::openGraph);
    add(QStringLiteral("graph:open-local"), tr("Graph view: Open local graph"), QString(),
        [this] {
            if (m_localGraph)
                showRightPane(m_localGraph);
        });
#endif
#ifdef NOTEAPPC_HAVE_PDFVIEW
    add(QStringLiteral("pdf-viewer:extract-annotations"), tr("PDF: Extract annotations to a note"), QString(),
        [this] {
            auto *pdf = qobject_cast<PdfView *>(m_active->view);
            if (!pdf) {
                statusBar()->showMessage(tr("Open a PDF first"), 3000);
                return;
            }
            QString error;
            if (!pdf->extractAnnotations(QString(), &error))
                QMessageBox::warning(this, tr("Extract annotations"), error);
            else
                statusBar()->showMessage(tr("Annotations extracted"), 3000);
        });
#endif
#ifdef NOTEAPPC_HAVE_SYNC
    add(QStringLiteral("sync:sign-in"), tr("Sync: Sign in"), QString(), &MainWindow::syncSignIn);
    add(QStringLiteral("sync:sync-now"), tr("Sync: Sync now"), QString(), [this] {
        if (m_sync)
            m_sync->syncNow();
        else
            syncSignIn();
    });
#endif
#ifdef NOTEAPPC_HAVE_PUBLISH
    add(QStringLiteral("publish:export"), tr("Publish: Export the vault as a website"), QString(),
        &MainWindow::publishSite);
#endif
    add(QStringLiteral("markdown:toggle-preview"), tr("Toggle reading view"),
        QStringLiteral("Ctrl+E"), &MainWindow::toggleReadingView);
    add(QStringLiteral("editor:toggle-source"), tr("Toggle Live Preview/Source mode"), QString(),
        &MainWindow::toggleSourceMode);
    add(QStringLiteral("file-explorer:new-file"), tr("Create new note"), QStringLiteral("Ctrl+N"),
        [this] { newNoteIn(QString()); });
    add(QStringLiteral("app:open-settings"), tr("Open settings"), QStringLiteral("Ctrl+,"),
        &MainWindow::openSettings);
    add(QStringLiteral("noteAppC:quit"), tr("Quit"), QStringLiteral("Ctrl+Q"), &MainWindow::close);
    add(QStringLiteral("app:go-back"), tr("Navigate back"), QStringLiteral("Alt+Left"),
        &MainWindow::goBack);
    add(QStringLiteral("app:go-forward"), tr("Navigate forward"), QStringLiteral("Alt+Right"),
        &MainWindow::goForward);
    add(QStringLiteral("file-recovery:open"), tr("File recovery: Open saved snapshots"), QString(),
        &MainWindow::openRecovery);
    add(QStringLiteral("workspace:export-pdf"), tr("Export to PDF"), QString(),
        &MainWindow::exportPdf);
    add(QStringLiteral("daily-notes"), tr("Daily notes: Open today's daily note"), QString(),
        [this] { openDailyNote(0); });
    add(QStringLiteral("daily-notes:goto-prev"), tr("Daily notes: Open previous daily note"),
        QString(), [this] { openDailyNote(-1); });
    add(QStringLiteral("daily-notes:goto-next"), tr("Daily notes: Open next daily note"),
        QString(), [this] { openDailyNote(1); });
    add(QStringLiteral("random-note"), tr("Random note: Open random note"), QString(), [this] {
        QStringList notes = m_index.allPaths();
        if (!m_active->notePath.isEmpty() && m_vault.isValid())
            notes.removeOne(m_vault.relative(m_active->notePath));
        if (!notes.isEmpty())
            openRelative(notes.at(QRandomGenerator::global()->bounded(int(notes.size()))));
    });
    add(QStringLiteral("zk-prefixer"), tr("Unique note creator: Create new unique note"), QString(),
        &MainWindow::createUniqueNote);
    add(QStringLiteral("slides:start"), tr("Slides: Start presentation"), QString(),
        &MainWindow::startPresentation);
    add(QStringLiteral("insert-template"), tr("Templates: Insert template"), QString(),
        &MainWindow::insertTemplate);
    add(QStringLiteral("bookmarks:bookmark-current-view"), tr("Bookmarks: Bookmark this note"),
        QString(), [this] {
            if (m_bookmarks && m_vault.isValid() && !m_active->notePath.isEmpty())
                m_bookmarks->bookmarkNote(m_vault.relative(m_active->notePath),
                                          QFileInfo(m_active->notePath).completeBaseName());
        });

    QList<SettingsPage::HotkeyCommand> rebindable;
    for (const QAction *action : std::as_const(m_commands))
        rebindable << SettingsPage::HotkeyCommand{action->objectName(), action->text(),
                                                  {QKeySequence(action->data().toString())}};
    for (const MarkdownEditor::Command &command : MarkdownEditor::commands())
        rebindable << SettingsPage::HotkeyCommand{command.id, command.name, command.keys};
    m_settingsPage->setCommands(rebindable);
    applyHotkeys();
}

void MainWindow::applyHotkeys()
{
    const Hotkeys::Bindings bindings = Hotkeys::load(m_vault.isValid() ? m_vault.root() : QString());
    for (QAction *action : std::as_const(m_commands)) {
        const auto own = bindings.constFind(action->objectName());
        action->setShortcuts(own != bindings.constEnd()
                                 ? *own
                                 : QList<QKeySequence>{QKeySequence(action->data().toString())});
    }
    m_editorBindings = bindings;
    for (const auto &leaf : m_leaves)
        leaf->editor->setKeyBindings(bindings);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Installed on the whole application (see the constructor), so this sees
    // a side-button click no matter which widget the mouse happens to be
    // over - the editor, the tree, a sidebar pane.
    if (event->type() == QEvent::MouseButtonPress) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const bool isNavButton =
            mouseEvent->button() == Qt::BackButton || mouseEvent->button() == Qt::ForwardButton;
        auto *widget = qobject_cast<QWidget *>(watched);
        if (isNavButton && widget && widget->window() == this) {
            if (mouseEvent->button() == Qt::BackButton)
                goBack();
            else
                goForward();
            return true;
        }
    }

    if (watched == m_tree && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            const QModelIndex current = m_tree->currentIndex();
            if (current.isValid() && m_files->isDir(current)) {
                // Consumed here, before QTreeView's own keyPressEvent can run
                // and emit activated() for the same key press - which would
                // toggle the folder a second time on top of this one.
                leaveSettings();
                m_tree->setExpanded(current, !m_tree->isExpanded(current));
                return true;
            }
        }
    }

    if (watched == m_headerSearch && event->type() == QEvent::FocusIn)
        openQuickSwitcher();
    // The editor changes width when the window resizes, when a sidebar is
    // toggled and in single-file mode, and the title has to keep following
    // its column through all three.
    if (event->type() == QEvent::Resize || event->type() == QEvent::FocusIn) {
        for (const auto &leaf : m_leaves) {
            if (watched != leaf->editor)
                continue;
            if (event->type() == QEvent::Resize)
                updateInlineTitleMargins(leaf.get());
            else
                activateLeaf(leaf.get()); // clicking into a split focuses its tab
            break;
        }
    }
    if (event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton && m_tree
        && watched == m_tree->viewport()) {
        // A middle click opens a file in a new tab, as in Obsidian.
        const QModelIndex index = m_tree->indexAt(static_cast<QMouseEvent *>(event)->position().toPoint());
        if (index.isValid() && !m_files->isDir(index)) {
            const QString path = m_files->filePath(index);
            if (isViewFile(path) || Vault::isNote(path)) {
                newTab();
                openNote(path, NavSource::Tree);
            }
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton) {
        for (const auto &group : m_groups) {
            if (watched != group->tabs)
                continue;
            const int index = group->tabs->tabAt(static_cast<QMouseEvent *>(event)->position().toPoint());
            if (index >= 0 && index < group->leaves.size()) {
                closeLeaf(group->leaves.at(index));
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::updateInlineTitleMargins(Leaf *leaf)
{
    const int column = m_theme.value(QStringLiteral("--file-line-width")).toInt();
    const int side = qMax(24, (leaf->editor->width() - column) / 2);
    leaf->inlineTitle->setContentsMargins(side, 32, side, 0);
}

void MainWindow::toggleSidebar()
{
    m_sidebar->setVisible(!m_sidebar->isVisible());
}

void MainWindow::toggleRightSidebar()
{
    m_rightSidebar->setVisible(!m_rightSidebar->isVisible());
    QSettings().setValue(kRightSidebar, m_rightSidebar->isVisible());
}

void MainWindow::showRightPane(QWidget *pane)
{
    if (!m_rightSidebar->isVisible())
        toggleRightSidebar();
    m_rightPanes->setCurrentWidget(pane);
    for (QAbstractButton *tab : m_rightTabs->buttons()) {
        if (tab->property("pane").value<QObject *>() == pane)
            tab->setChecked(true);
    }
}

void MainWindow::openSearch(const QString &query)
{
    if (!m_search)
        return; // Search is switched off
    showRightPane(m_search);
    if (!query.isEmpty())
        m_search->setQuery(query);
    m_search->setFocus();
}

void MainWindow::openGraph()
{
#ifdef NOTEAPPC_HAVE_GRAPH
    if (!featureOn("graph"))
        return;
    if (m_graph && m_mainStack->currentWidget() == m_graph) {
        leaveSettings();
        return;
    }
    if (!m_graph) {
        m_graph = new GraphView(m_mainStack);
        m_graph->setObjectName(QStringLiteral("graphView"));
        m_graph->setTheme(m_theme);
        if (m_vault.isValid())
            m_graph->setOptions(GraphOptions::readFromVault(m_vault.root()));
        m_graph->setIndex(&m_index);
        connect(m_graph, &GraphView::noteActivated, this, &MainWindow::openRelative);
        m_mainStack->addWidget(m_graph);
    }
    m_graph->setCurrentNote(m_vault.isValid() && !m_active->notePath.isEmpty() ? m_vault.relative(m_active->notePath)
                                                                      : QString());
    m_mainStack->setCurrentWidget(m_graph);
    m_graph->setFocus();
#endif
}

void MainWindow::openQuickSwitcher()
{
    if (!m_vault.isValid())
        return;
    m_switcher->setCandidates(m_index.allPaths());
    m_switcher->popUp();
}

void MainWindow::openCommandPalette()
{
    // Built the first time it is wanted: a session that never opens it pays
    // nothing for it.
    if (!m_palette) {
        m_palette = new QuickSwitcher(this);
        m_palette->setObjectName(QStringLiteral("commandPalette"));
        m_palette->setPlaceholder(tr("Select a command..."));
        connect(m_palette, &QuickSwitcher::accepted, this, [this](const QString &name) {
            const QString id = m_paletteIDs.value(name);
            if (id.isEmpty())
                return;
            m_recentCommands.removeAll(id);
            m_recentCommands.prepend(id);
            if (auto *action = findChild<QAction *>(id, Qt::FindDirectChildrenOnly)) {
                action->trigger();
                return;
            }
            m_active->editor->setFocus();
            m_active->editor->runCommand(id);
        });
    }

    struct Entry {
        QString id;
        QString name;
        QList<QKeySequence> keys;
    };
    QList<Entry> entries;
    for (const QAction *action : std::as_const(m_commands)) {
        if (action->isEnabled() && action->objectName() != QLatin1String("command-palette:open"))
            entries.append({action->objectName(), action->text(), action->shortcuts()});
    }
    // Editing commands only while a note is open for editing, as in Obsidian.
    if (!m_active->notePath.isEmpty() && !m_active->view) {
        for (const MarkdownEditor::Command &command : MarkdownEditor::commands())
            entries.append({command.id, command.name, m_editorBindings.value(command.id, command.keys)});
    }
    std::stable_sort(entries.begin(), entries.end(), [this](const Entry &l, const Entry &r) {
        const auto rank = [this](const Entry &entry) {
            const qsizetype at = m_recentCommands.indexOf(entry.id);
            return at < 0 ? m_recentCommands.size() : at;
        };
        return rank(l) < rank(r);
    });

    QStringList names;
    QStringList keys;
    m_paletteIDs.clear();
    for (const Entry &entry : std::as_const(entries)) {
        names << entry.name;
        keys << (entry.keys.isEmpty() ? QString() : entry.keys.constFirst().toString(QKeySequence::NativeText));
        m_paletteIDs.insert(entry.name, entry.id);
    }
    m_palette->setCandidates(names, keys);
    m_palette->popUp();
}

void MainWindow::toggleReadingView()
{
    MarkdownEditor *editor = m_active->editor;
    if (editor->mode() == MarkdownEditor::Mode::Reading) {
        editor->setMode(m_active->editingMode);
        return;
    }
    m_active->editingMode = editor->mode();
    editor->setMode(MarkdownEditor::Mode::Reading);
}

void MainWindow::toggleSourceMode()
{
    const auto flipped = [](MarkdownEditor::Mode mode) {
        return mode == MarkdownEditor::Mode::Source ? MarkdownEditor::Mode::LivePreview : MarkdownEditor::Mode::Source;
    };
    MarkdownEditor *editor = m_active->editor;
    m_active->editingMode = flipped(editor->mode() == MarkdownEditor::Mode::Reading ? m_active->editingMode
                                                                                     : editor->mode());
    if (editor->mode() != MarkdownEditor::Mode::Reading)
        editor->setMode(m_active->editingMode);
    statusBar()->showMessage(m_active->editingMode == MarkdownEditor::Mode::Source ? tr("Source mode")
                                                                                    : tr("Live preview"),
                             2000);
}

void MainWindow::openSettings()
{
    m_mainStack->setCurrentIndex(1);
}

void MainWindow::closeSettings()
{
    m_settings.save();
    m_mainStack->setCurrentIndex(0);
    if (m_active->editor->isEnabled())
        m_active->editor->setFocus();
}

void MainWindow::leaveSettings()
{
    // Also leaves the graph view, the main area's other page.
    if (m_mainStack->currentWidget() == m_settingsPage)
        closeSettings();
    else
        m_mainStack->setCurrentIndex(0);
}

void MainWindow::openRelative(const QString &relativePath)
{
    if (!relativePath.isEmpty() && m_vault.isValid())
        openNote(m_vault.absolute(relativePath));
}

void MainWindow::onIndexChanged()
{
    const QStringList targets = m_index.linkTargets();
    for (const auto &leaf : m_leaves) {
        leaf->editor->setLinkTargets(targets);
        // Before this, a note opened while the vault was first indexed showed
        // every link as missing until it was next edited.
        leaf->editor->refreshLinkStatus();
    }
    // The backlinks, tags and outgoing-links panes listen to Index::changed
    // themselves; refreshing them here as well did each one twice per save.
    if (m_index.isOpen() && m_active->notePath.isEmpty())
        m_vaultLabel->setText(tr("%1  -  %2 notes").arg(m_vault.name()).arg(m_index.noteCount()));
}

void MainWindow::setThemeMode(Theme::Mode mode)
{
    m_settings.setValue(QStringLiteral("appearance.theme"), Theme::nameOf(mode));
    applySettings();
}

Theme MainWindow::themeFromSettings() const
{
    const QString modeText = m_settings.text(QStringLiteral("appearance.theme"));
    Theme::Mode mode = Theme::modeFromName(modeText);
    if (modeText == QLatin1String("system")) {
        mode = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light
                  ? Theme::Mode::Light
                  : Theme::Mode::Dark;
    }

    const QString schemePath = m_settings.text(QStringLiteral("appearance.scheme"));
    Theme theme = schemePath.isEmpty() ? Theme::builtin(mode) : Scheme::apply(schemePath, mode);

    theme.setAccent(m_settings.integer(QStringLiteral("appearance.accentHue")),
                    m_settings.integer(QStringLiteral("appearance.accentSaturation")),
                    m_settings.integer(QStringLiteral("appearance.accentLightness")));
    theme.setToken(QStringLiteral("--font-text-size"),
                   QString::number(m_settings.integer(QStringLiteral("appearance.baseFontSize"))));
    theme.setFontOverrides(m_settings.text(QStringLiteral("appearance.interfaceFont")),
                           m_settings.text(QStringLiteral("appearance.textFont")),
                           m_settings.text(QStringLiteral("appearance.monospaceFont")));
    return theme;
}

void MainWindow::applySettings()
{
    // Every switch on the settings page comes here, and restyling the window
    // and relaying out every open note was most of it - 17 ms with a 1 MB
    // note open - so that waits for the theme itself to change.
    const Theme theme = themeFromSettings();
    const bool restyle = !m_themeApplied || !(theme == m_theme);
    m_theme = theme;
    applyFeatures(); // first, so a pane it builds is themed below
    if (restyle) {
        applyTheme();
        m_themeApplied = true;
    }

    for (const auto &leaf : m_leaves)
        applyEditorSettings(leaf->editor);
    if (const QString mode = m_settings.text(QStringLiteral("editor.defaultMode")); mode != m_appliedDefaultMode) {
        m_appliedDefaultMode = mode;
        for (const auto &leaf : m_leaves)
            leaf->editor->setMode(defaultMode());
    }
    updateSpelling();
    updateNoteHistory();
    updateSync();
    applyHotkeys(); // the vault may carry its own Obsidian hotkeys.json

    for (const auto &leaf : m_leaves) {
        leaf->inlineTitle->setVisible(!leaf->notePath.isEmpty() && !leaf->view
                                      && m_settings.boolean(QStringLiteral("appearance.showInlineTitle")));
    }
}

DailyNotes MainWindow::dailyNotes() const
{
    DailyNotes::Config config;
    config.folder = m_settings.text(QStringLiteral("dailyNotes.folder"));
    config.format = m_settings.text(QStringLiteral("dailyNotes.format"));
    config.templatePath = m_settings.text(QStringLiteral("dailyNotes.template"));
    if (QFileInfo::exists(QDir(m_vault.root()).filePath(QStringLiteral(".obsidian/daily-notes.json"))))
        config = DailyNotes::readConfig(m_vault.root());
    return DailyNotes(m_vault.root(), config);
}

void MainWindow::startPresentation()
{
    if (m_active->notePath.isEmpty() || m_active->view) {
        statusBar()->showMessage(tr("Open a note to present it"), 3000);
        return;
    }
    if (!m_slides) {
        m_slides = new SlidesView(m_mainStack);
        m_mainStack->addWidget(m_slides);
        connect(m_slides, &SlidesView::finished, this, [this] {
            m_mainStack->setCurrentIndex(0);
            if (m_slidesLeftFullScreen)
                showNormal();
            m_active->editor->setFocus();
        });
    }
    m_slides->setTheme(m_theme);
    m_slides->setLinkResolver(m_resolveLink);
    m_slides->setFeatures(m_active->editor->features());
    // The slide the caret is on, the way Obsidian starts from where you are;
    // a caret just past a rule is on the next one.
    const QString text = m_active->editor->noteText();
    const QString before = text.left(m_active->editor->cursorOffset()) + QLatin1Char('x');
    m_slidesLeftFullScreen = !isFullScreen();
    m_mainStack->setCurrentWidget(m_slides);
    showFullScreen();
    m_slides->present(SlidesView::split(text), int(SlidesView::split(before).size()) - 1);
}

void MainWindow::startRecording()
{
    if (!m_vault.isValid() || m_active->view || !m_vault.contains(m_active->notePath))
        return;
    if (m_recorder && m_recorder->isRecording())
        return;
    const QString suffix = AudioRecorder::fileSuffix();
    const QString folder =
        m_vault.attachmentFolderFor(m_settings.text(QStringLiteral("files.attachmentFolder")), m_active->notePath);
    QDir().mkpath(folder);
    // Obsidian's name for it; a second in the same second gets " 1".
    const QString base = QStringLiteral("Recording ") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss"));
    QString path = QDir(folder).filePath(base + QLatin1Char('.') + suffix);
    for (int n = 1; QFileInfo::exists(path); ++n)
        path = QDir(folder).filePath(QStringLiteral("%1 %2.%3").arg(base).arg(n).arg(suffix));
    if (!m_recorder) {
        m_recorder = new AudioRecorder(this);
        connect(m_recorder, &AudioRecorder::finished, this, [this](const QString &file, const QString &error) {
            m_recordingStatus->hide();
            if (!QFileInfo::exists(file)) {
                statusBar()->showMessage(tr("The recording failed: %1").arg(error), 5000);
                return;
            }
            // Into the note it was started from, where its caret is.
            for (const auto &leaf : m_leaves) {
                if (leaf->notePath == m_recordingNote && !leaf->view) {
                    leaf->editor->insertAtCaret(QStringLiteral("![[") + QFileInfo(file).fileName() + QStringLiteral("]]"));
                    return;
                }
            }
            statusBar()->showMessage(tr("Recording saved as %1").arg(m_vault.relative(file)), 8000);
        });
    }
    QString error;
    if (!m_recorder->start(path, &error)) {
        statusBar()->showMessage(tr("Cannot record: %1").arg(error), 5000);
        return;
    }
    m_recordingNote = m_active->notePath;
    m_recordingStatus->setText(tr("Recording"));
    m_recordingStatus->show();
}

void MainWindow::mergeCurrentNote()
{
    if (!m_vault.isValid() || m_active->view || !m_vault.contains(m_active->notePath) || !save())
        return;
    const QString source = m_active->notePath;
    QStringList notes = m_index.allPaths();
    notes.removeOne(m_vault.relative(source));
    auto *picker = new QuickSwitcher(this);
    picker->setObjectName(QStringLiteral("mergePicker"));
    picker->setPlaceholder(tr("Merge %1 into...").arg(QFileInfo(source).completeBaseName()));
    picker->setCandidates(notes);
    connect(picker, &QuickSwitcher::accepted, this, [this, source, picker](const QString &target) {
        picker->deleteLater();
        mergeNote(source, m_vault.absolute(target));
    });
    connect(picker, &QDialog::rejected, picker, &QObject::deleteLater);
    picker->popUp();
}

bool MainWindow::mergeNote(const QString &source, const QString &target)
{
    // What the target's tabs hold unsaved is its text too.
    for (const auto &leaf : m_leaves) {
        if (leaf->notePath == target && isModified(leaf.get()) && !saveLeaf(leaf.get(), false))
            return false;
    }
    Note from;
    Note into;
    QString error;
    if (!Vault::read(source, from, &error) || !Vault::read(target, into, &error)) {
        QMessageBox::warning(this, tr("Merge"), error);
        return false;
    }
    // The body only: the source's properties would be a second frontmatter
    // in the middle of the target.
    const Range frontmatter = Frontmatter::find(from.text);
    QString body = from.text.mid(frontmatter.isValid() ? frontmatter.end : 0);
    while (body.startsWith(QLatin1Char('\n')))
        body.remove(0, 1);
    if (!into.text.isEmpty() && !into.text.endsWith(QLatin1Char('\n')))
        into.text += QLatin1Char('\n');
    into.text += (into.text.isEmpty() ? QString() : QStringLiteral("\n")) + body;
    if (!into.text.endsWith(QLatin1Char('\n')))
        into.text += QLatin1Char('\n');
    if (!Vault::write(target, into, &error)) {
        QMessageBox::warning(this, tr("Merge"), error);
        return false;
    }
    // Links to the merged note lead to where its text went, as a rename's do.
    if (m_index.isOpen())
        m_index.updateLinksAfterRename(m_vault.relative(source), m_vault.relative(target), m_vault, &error);
    if (!m_vault.moveToTrash(source, &error)) {
        QMessageBox::warning(this, tr("Merge"), tr("Merged, but %1 stays:\n%2").arg(QFileInfo(source).fileName(), error));
        return false;
    }
    openNote(target);
    QList<Leaf *> gone;
    for (const auto &leaf : m_leaves) {
        if (leaf->notePath == source)
            gone << leaf.get();
    }
    for (Leaf *leaf : std::as_const(gone))
        dropLeaf(leaf);
    m_indexer->rebuild();
    return true;
}

void MainWindow::extractSelection()
{
    if (!m_vault.isValid() || m_active->view || !m_vault.contains(m_active->notePath))
        return;
    MarkdownEditor *editor = m_active->editor;
    const QString selection = editor->selectedText();
    if (selection.trimmed().isEmpty()) {
        statusBar()->showMessage(tr("Select the text to extract first"), 3000);
        return;
    }
    // Named after its first heading, else its first line, as Obsidian does.
    QString suggested;
    for (const Heading &heading : Parser().parseAll(selection).metadata.headings) {
        suggested = heading.text;
        break;
    }
    if (suggested.isEmpty())
        suggested = selection.trimmed().section(QLatin1Char('\n'), 0, 0);
    static const QRegularExpression forbidden(QStringLiteral(R"([\[\]#^|\\/:*?"<>])"));
    suggested = suggested.remove(forbidden).simplified().left(60);
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Extract selection"), tr("New note name:"), QLineEdit::Normal,
                                               suggested, &ok)
                             .remove(forbidden)
                             .simplified();
    if (!ok || name.isEmpty())
        return;
    QString error;
    const QString folder = m_vault.relative(QFileInfo(m_active->notePath).absolutePath());
    const QString path =
        m_vault.createNote(folder == QLatin1String(".") ? QString() : folder, name, &error, QStringLiteral(".md"),
                           selection.endsWith(QLatin1Char('\n')) ? selection : selection + QLatin1Char('\n'));
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("Extract selection"), error);
        return;
    }
    // What stays behind: a link to it, or an embed that still shows it.
    const bool embed = m_settings.text(QStringLiteral("noteComposer.replaceWith")) == QLatin1String("embed");
    editor->insertAtCaret((embed ? QStringLiteral("![[") : QStringLiteral("[[")) + QFileInfo(path).completeBaseName()
                          + QStringLiteral("]]"));
    m_indexer->rebuild();
}

void MainWindow::createUniqueNote()
{
    if (!m_vault.isValid())
        return;
    // The vault's own zk-prefixer.json wins, as Obsidian's daily notes do.
    QString folder = m_settings.text(QStringLiteral("uniqueNote.folder"));
    QString format = m_settings.text(QStringLiteral("uniqueNote.format"));
    QFile config(QDir(m_vault.root()).filePath(QStringLiteral(".obsidian/zk-prefixer.json")));
    if (config.open(QIODevice::ReadOnly)) {
        const QJsonObject json = QJsonDocument::fromJson(config.readAll()).object();
        folder = json.value(QLatin1String("folder")).toString(folder);
        if (!json.value(QLatin1String("format")).toString().isEmpty())
            format = json.value(QLatin1String("format")).toString();
    }
    QString error;
    const QString path =
        m_vault.createNote(folder, MomentFormat::format(QDateTime::currentDateTime(), format), &error);
    if (path.isEmpty())
        QMessageBox::warning(this, tr("Unique note creator"), tr("Could not create the note:\n%1").arg(error));
    else
        openNote(path);
}

void MainWindow::openDailyNote(int step)
{
    if (!m_vault.isValid()) {
        statusBar()->showMessage(tr("Daily notes live in a vault"), 3000);
        return;
    }
    const DailyNotes daily = dailyNotes();
    QDate date = QDate::currentDate();
    QString path;
    if (step == 0) {
        QString error;
        path = daily.ensureNote(date, &error);
        if (path.isEmpty()) {
            QMessageBox::warning(this, tr("Daily notes"), error);
            return;
        }
    } else {
        // From the daily note that is open, when one is; from today otherwise.
        // ponytail: finds its date by trying the year either side, since an
        // arbitrary format need not parse back into a date.
        for (int days = 0; days <= 366; ++days) {
            if (daily.pathFor(date.addDays(-days)) == m_active->notePath) {
                date = date.addDays(-days);
                break;
            }
            if (daily.pathFor(date.addDays(days)) == m_active->notePath) {
                date = date.addDays(days);
                break;
            }
        }
        path = step < 0 ? daily.previousNote(date) : daily.nextNote(date);
        if (path.isEmpty()) {
            statusBar()->showMessage(step < 0 ? tr("No earlier daily note") : tr("No later daily note"),
                                     3000);
            return;
        }
    }
    openNote(QFileInfo(path).isAbsolute() ? path : m_vault.absolute(path));
}

void MainWindow::insertTemplate()
{
    if (!m_vault.isValid() || m_active->notePath.isEmpty())
        return;
    Templates::Config config;
    config.folder = m_settings.text(QStringLiteral("templates.folder"));
    config.dateFormat = m_settings.text(QStringLiteral("templates.dateFormat"));
    config.timeFormat = m_settings.text(QStringLiteral("templates.timeFormat"));
    if (QFileInfo::exists(QDir(m_vault.root()).filePath(QStringLiteral(".obsidian/templates.json"))))
        config = Templates::readConfig(m_vault.root());
    m_templatePicker->setTemplates(Templates(m_vault.root(), config));
    m_templatePicker->popUp(QFileInfo(m_active->notePath).completeBaseName());
}

qsizetype MainWindow::subpathOffset(const QString &text, const QString &subpath)
{
    const QString wanted = (subpath.startsWith(QLatin1Char('#')) ? subpath.mid(1) : subpath).trimmed();
    if (wanted.isEmpty())
        return -1;
    const Metadata metadata = Parser().parseAll(text).metadata;
    if (wanted.startsWith(QLatin1Char('^'))) {
        // The line the marker ends, where Obsidian's block starts to show.
        for (const BlockId &block : metadata.blockIds) {
            if (block.id.compare(wanted.mid(1), Qt::CaseInsensitive) == 0)
                return text.lastIndexOf(QLatin1Char('\n'), qMax<qsizetype>(0, block.range.start - 1)) + 1;
        }
        return -1;
    }
    for (const Heading &heading : metadata.headings) {
        if (heading.text.compare(wanted, Qt::CaseInsensitive) == 0)
            return heading.range.start;
    }
    return -1;
}

void MainWindow::jumpToHeading(const QString &subpath)
{
    const QString wanted = (subpath.startsWith(QLatin1Char('#')) ? subpath.mid(1) : subpath).trimmed();
    if (wanted.isEmpty())
        return;
    const auto jump = [this, wanted] {
        for (const Heading &heading : m_active->editor->headings()) {
            if (heading.text.compare(wanted, Qt::CaseInsensitive) == 0) {
                m_active->editor->setCursorOffset(heading.range.start);
                return true;
            }
        }
        return false;
    };
    if (jump())
        return;
    // A big note's headings come with its parse, off the UI thread.
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(m_active->editor, &MarkdownEditor::headingsChanged, this, [this, jump, connection] {
        jump();
        disconnect(*connection);
    });
}

void MainWindow::exportPdf()
{
    if (m_active->notePath.isEmpty())
        return;
    ExportPdfDialog options(this);
    if (options.exec() != QDialog::Accepted)
        return;
    const QFileInfo note(m_active->notePath);
    const QString target = QFileDialog::getSaveFileName(
        this, tr("Export to PDF"), note.dir().filePath(note.completeBaseName() + QStringLiteral(".pdf")),
        tr("PDF (*.pdf)"));
    if (target.isEmpty())
        return;
    QString error;
    if (MarkdownEditor::exportPdf(target, m_active->editor->noteText(), note.completeBaseName(),
                                  m_resolveLink, options.options(), &error))
        statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(target).fileName()), 3000);
    else
        QMessageBox::warning(this, tr("Export to PDF"), error);
}

void MainWindow::openRecovery()
{
    auto *dialog = new RecoveryDialog(m_vault, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &RecoveryDialog::restoreRequested, this, &MainWindow::restoreSnapshot);
    dialog->open();
}

void MainWindow::restoreSnapshot(const QString &notePath, const QString &text)
{
    // The open note first saves what is in the editor, so that is what the
    // snapshot below keeps rather than what was last written.
    if (notePath == m_active->notePath && !save())
        return;
    Note note;
    QString error;
    if (!Vault::read(notePath, note, &error)) {
        QMessageBox::warning(this, tr("Restore"), tr("Could not read %1:\n%2").arg(notePath, error));
        return;
    }
    Snapshots::record(notePath, note.text, 0);
    note.text = text; // its line endings and BOM stay as the file has them
    if (!Vault::write(notePath, note, &error)) {
        QMessageBox::warning(this, tr("Restore"), tr("Could not write %1:\n%2").arg(notePath, error));
        return;
    }
    recordHistory(notePath, text);
    if (m_vault.isValid())
        m_indexer->reindex(notePath);
    if (notePath == m_active->notePath) {
        m_active->note = note;
        m_active->editor->setNoteText(text);
    }
    statusBar()->showMessage(tr("Restored %1").arg(QFileInfo(notePath).fileName()), 3000);
}

void MainWindow::showPagePreview()
{
    // "Note#Heading", "Note#^block", "#Heading" in this note, or "Note".
    const qsizetype hash = m_previewTarget.indexOf(QLatin1Char('#'));
    const QString name = hash >= 0 ? m_previewTarget.left(hash) : m_previewTarget;
    const QString subpath = hash >= 0 ? m_previewTarget.mid(hash + 1).trimmed() : QString();
    QString path = m_active->notePath;
    if (!name.isEmpty()) {
        path = m_resolveLink(name); // attachments too, which the index may not hold
        if (path.isEmpty())
            return; // nothing there yet to preview
    }
    // Images and PDFs preview as a picture, as in Obsidian. The reader looks
    // at the header first, so a note is never decoded as an image.
    if (QImageReader reader(path); reader.canRead()) {
        QSize size = reader.size();
        if (size.width() > 1040)
            reader.setScaledSize(size.scaled(1040, size.height(), Qt::KeepAspectRatio));
        const QImage image = reader.read();
        if (!image.isNull())
            pagePreview()->showImage(image, m_previewPos);
        return;
    }
    if (path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) {
        const int page = subpath.startsWith(QLatin1String("page=")) ? subpath.mid(5).toInt() - 1 : 0;
        const QImage image = PdfPage::render(path, qMax(0, page), 520);
        if (!image.isNull())
            pagePreview()->showImage(image, m_previewPos);
        return;
    }
    Note note;
    if (!Vault::isNote(path) || !Vault::read(path, note))
        return;

    pagePreview()->showNote(note.text, qMax<qsizetype>(0, subpathOffset(note.text, subpath)), m_previewPos);
}

PagePreview *MainWindow::pagePreview()
{
    if (!m_preview) {
        m_preview = new PagePreview(this);
        m_preview->setLinkResolver(m_resolveLink);
        m_preview->applyTheme(m_theme);
        connect(m_preview, &PagePreview::linkActivated, this, &MainWindow::followLink);
    }
    return m_preview;
}

void MainWindow::updateSpelling()
{
#ifdef NOTEAPPC_HAVE_SPELLCHECK
    if (!m_settings.boolean(QStringLiteral("editor.spellcheck"))) {
        if (m_spellHanded) {
            for (const auto &leaf : m_leaves)
                leaf->editor->setSpelling({});
        }
        m_spellHanded = false;
        return;
    }
    if (m_spell) {
        // Handed over once; every later settings change would otherwise lay
        // the note out again for nothing. No dictionary, nothing to hand.
        if (m_spellHanded || m_spell->languages().isEmpty())
            return;
        const std::shared_ptr<SpellChecker> spell = m_spell;
        m_editorSpelling = {[spell](const QString &word) { return spell->isCorrect(word); },
                            [spell](const QString &word) { return spell->suggestions(word); },
                            [spell](const QString &word) { spell->addWord(word); }};
        for (const auto &leaf : m_leaves)
            leaf->editor->setSpelling(m_editorSpelling);
        m_spellHanded = true;
        return;
    }
    if (m_spellLoading || !m_painted)
        return;
    m_spellLoading = true;
    auto *watcher = new QFutureWatcher<std::shared_ptr<SpellChecker>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        m_spell = watcher->result();
        m_spellLoading = false;
        watcher->deleteLater();
        if (m_spell->languages().isEmpty()) {
            statusBar()->showMessage(
                tr("Spellcheck is on, but there is no hunspell dictionary for %1")
                    .arg(SpellChecker::systemLanguages().join(QStringLiteral(", "))),
                8000);
        }
        updateSpelling();
    });
    watcher->setFuture(QtConcurrent::run(
        [] { return std::make_shared<SpellChecker>(SpellChecker::systemLanguages()); }));
#endif
}

void MainWindow::notifyPlugins(const char *event, const QString &path)
{
#ifdef NOTEAPPC_HAVE_PLUGINS
    if (m_plugins && m_vault.isValid() && m_vault.contains(path))
        m_plugins->notifyVaultEvent(QString::fromLatin1(event), m_vault.relative(path));
#else
    Q_UNUSED(event);
    Q_UNUSED(path);
#endif
}

void MainWindow::syncPlugins(bool rescan)
{
#ifdef NOTEAPPC_HAVE_PLUGINS
    const bool on = featureOn("community-plugins");
    if (on == bool(m_plugins) && !rescan)
        return;
    if (on && !m_plugins) {
        m_plugins = new PluginRegistry(this);
        // Every path a plugin names goes through confinedNotePath: notes in
        // this vault only, never "..", a symlink out, or its config.
        PluginAppCallbacks callbacks;
        callbacks.activeNotePath = [this] {
            return m_active && m_vault.isValid() && m_vault.contains(m_active->notePath)
                       ? m_vault.relative(m_active->notePath)
                       : QString();
        };
        callbacks.activeNoteText = [this] {
            return m_active && !m_active->view ? m_active->editor->noteText() : QString();
        };
        callbacks.activeSelectionText = [this] {
            return m_active && !m_active->view ? m_active->editor->selectedText() : QString();
        };
        callbacks.readNote = [this](const QString &relative) {
            const QString path = m_vault.confinedNotePath(relative);
            Note note;
            return !path.isEmpty() && Vault::read(path, note) ? note.text : QString();
        };
        callbacks.listNotes = [this] { return m_index.allPaths(); };
        callbacks.replaceSelection = [this](const QString &text) {
            if (!m_active || m_active->view || !m_active->editor->isEnabled())
                return false;
            m_active->editor->insertAtCaret(text);
            return true;
        };
        callbacks.writeNote = [this](const QString &relative, const QString &text) {
            const QString path = m_vault.confinedNotePath(relative);
            if (path.isEmpty())
                return false;
            Note note;
            if (QFileInfo::exists(path) && !Vault::read(path, note))
                return false;
            note.text = text; // its line endings and BOM stay as they were
            return Vault::write(path, note);
        };
        callbacks.clipboardRead = [] { return QGuiApplication::clipboard()->text(); };
        callbacks.clipboardWrite = [](const QString &text) {
            QGuiApplication::clipboard()->setText(text);
            return true;
        };
        callbacks.showNotice = [this](const QString &message) { statusBar()->showMessage(message, 5000); };
        callbacks.log = [](const QString &pluginID, const QString &message) {
            qInfo("plugin %s: %s", qPrintable(pluginID), qPrintable(message));
        };
        m_plugins->setCallbacks(std::move(callbacks));
        m_plugins->scan(true);
    } else if (!on && m_plugins) {
        m_plugins->shutdownAll();
        delete m_plugins;
        m_plugins = nullptr;
    }
    m_settingsPage->setPluginRegistry(m_plugins);

    for (QAction *action : std::as_const(m_pluginCommands)) {
        m_commands.removeOne(action);
        delete action;
    }
    m_pluginCommands.clear();
    QStringList languages;
    if (m_plugins) {
        for (const PluginCommand &command : m_plugins->commands()) {
            auto *action = new QAction(command.title.isEmpty()
                                           ? command.pluginName + QStringLiteral(": ") + command.commandID
                                           : command.title,
                                       this);
            action->setObjectName(command.qualifiedID); // "pluginID:commandID", as hotkeys.json names it
            action->setShortcutContext(Qt::WindowShortcut);
            connect(action, &QAction::triggered, this, [this, id = command.qualifiedID] {
                if (!m_plugins)
                    return;
                m_plugins->runCommand(id, [this](bool ok, const QJsonObject &, const QString &error) {
                    if (!ok)
                        statusBar()->showMessage(tr("The plugin command failed: %1").arg(error), 5000);
                });
            });
            addAction(action);
            m_commands << action;
            m_pluginCommands << action;
        }
        languages = m_plugins->codeBlockOwners().keys();
    }
    applyHotkeys();

    MarkdownEditor::CodeBlockRenderer renderer;
    if (!languages.isEmpty()) {
        const QPointer<PluginRegistry> registry = m_plugins;
        const qreal ratio = devicePixelRatioF();
        renderer = [registry, ratio](const QString &language, const QString &source, int width, const QString &theme,
                                     const std::function<void(MarkdownEditor::CodeBlockResult)> &deliver) {
            if (!registry) {
                deliver({});
                return;
            }
            registry->renderCodeBlock(
                language, source, width, ratio, theme,
                [deliver, width](bool ok, const QJsonObject &result, const QString &error) {
                    if (!ok) {
                        deliver({true, QStringLiteral("> [!failure] %1").arg(error), {}});
                        return;
                    }
                    if (result.value(QStringLiteral("isMarkdown")).toBool()) {
                        deliver({true, result.value(QStringLiteral("text")).toString(), {}});
                        return;
                    }
                    // A plugin's bytes: the size they claim is checked, not trusted.
                    const int w = result.value(QStringLiteral("width")).toInt();
                    const int h = result.value(QStringLiteral("height")).toInt();
                    const QByteArray bytes =
                        QByteArray::fromBase64(result.value(QStringLiteral("rgbaBase64")).toString().toLatin1());
                    if (w <= 0 || h <= 0 || bytes.size() != qsizetype(w) * h * 4) {
                        deliver({});
                        return;
                    }
                    QImage image = QImage(reinterpret_cast<const uchar *>(bytes.constData()), w, h, w * 4,
                                          QImage::Format_RGBA8888)
                                       .copy();
                    if (w > width)
                        image.setDevicePixelRatio(qreal(w) / width);
                    deliver({false, QString(), std::move(image)});
                });
        };
    }
    for (const auto &leaf : m_leaves) {
        if (!m_pluginLanguages.isEmpty())
            leaf->editor->setCodeBlockRenderer(m_pluginLanguages, nullptr);
        if (renderer)
            leaf->editor->setCodeBlockRenderer(languages, renderer);
    }
    m_pluginLanguages = languages;
    m_applyPluginRenderer = nullptr;
    if (renderer)
        m_applyPluginRenderer = [languages, renderer](MarkdownEditor *editor) {
            editor->setCodeBlockRenderer(languages, renderer);
        };
#else
    Q_UNUSED(rescan);
#endif
}

void MainWindow::publishSite()
{
#ifdef NOTEAPPC_HAVE_PUBLISH
    if (!m_vault.isValid())
        return;
    if (m_publishing) {
        statusBar()->showMessage(tr("Already publishing"), 3000);
        return;
    }
    // The last choices for this vault, kept with the window's own state, not
    // in the vault. Obsidian Publish shares nothing until it is picked, so
    // neither does a first publish here: marked notes only.
    QSettings state;
    const QString group = QStringLiteral("publish/")
                          + QString::fromLatin1(QCryptographicHash::hash(m_vault.root().toUtf8(),
                                                                         QCryptographicHash::Sha1).toHex());
    PublishDialog::Choices remembered;
    remembered.folder = state.value(group + QStringLiteral("/folder")).toString();
    remembered.siteName = state.value(group + QStringLiteral("/siteName"), m_vault.name()).toString();
    remembered.homeNote = state.value(group + QStringLiteral("/homeNote")).toString();
    remembered.onlyMarked = state.value(group + QStringLiteral("/onlyMarked"), true).toBool();
    remembered.theme = state.value(group + QStringLiteral("/theme"), 2).toInt();
    QStringList notes;
    for (const QString &path : m_index.allPaths()) {
        if (path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
            notes << path;
    }
    PublishDialog dialog(remembered, notes, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const PublishDialog::Choices choices = dialog.choices();
    state.setValue(group + QStringLiteral("/folder"), choices.folder);
    state.setValue(group + QStringLiteral("/siteName"), choices.siteName);
    state.setValue(group + QStringLiteral("/homeNote"), choices.homeNote);
    state.setValue(group + QStringLiteral("/onlyMarked"), choices.onlyMarked);
    state.setValue(group + QStringLiteral("/theme"), choices.theme);

    struct Result {
        bool ok = false;
        QString error;
        publish::Counts counts;
    };
    m_publishing = true;
    statusBar()->showMessage(tr("Publishing..."));
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        const Result result = watcher->result();
        watcher->deleteLater();
        m_publishing = false;
        if (!result.ok) {
            statusBar()->clearMessage();
            QMessageBox::warning(this, tr("Publish"), tr("Could not publish the vault:\n%1").arg(result.error));
            return;
        }
        statusBar()->showMessage(tr("Published %n page(s)", nullptr, result.counts.pages), 5000);
    });
    // Publisher queries the index on the thread that calls it, and an Index
    // is one connection for one thread: this run opens its own on the same
    // cache (WAL, so the indexer is not blocked) instead of sharing m_index.
    watcher->setFuture(QtConcurrent::run([vault = m_vault, choices] {
        Result result;
        Index index;
        if (!index.open(vault.root())) {
            result.error = index.lastError();
            return result;
        }
        publish::Publisher publisher(vault, index);
        publish::Options options;
        options.outputDir = choices.folder;
        options.onlyMarked = choices.onlyMarked;
        options.homeNote = choices.homeNote;
        options.siteName = choices.siteName.isEmpty() ? vault.name() : choices.siteName;
        options.theme = publish::Options::ThemeMode(choices.theme);
        result.ok = publisher.publish(options, {}, &result.error);
        result.counts = publisher.counts();
        return result;
    }));
#endif
}

void MainWindow::applyTheme()
{
    // Keeps the watch on the right files as settings change - but only once
    // watching has begun, which openTarget does after the first paint. Live
    // reload is optional, and nothing optional runs before the window is up.
    if (m_themeWatcher)
        watchThemeFiles();
    Icons::clearCache();

    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        app->setPalette(m_theme.palette());
        app->setStyleSheet(m_theme.styleSheet());
    } else {
        setPalette(m_theme.palette());
        setStyleSheet(m_theme.styleSheet());
    }

    const QColor iconColor = m_theme.color(QStringLiteral("--text-muted"));
    const QColor accentColor = m_theme.color(QStringLiteral("--interactive-accent"));
    for (QToolButton *button : findChildren<QToolButton *>()) {
        const QString name = button->property("iconName").toString();
        if (name.isEmpty())
            continue;
        // A checked tab button is the active one: accented, like Obsidian's
        // own tab headers.
        const QColor color = button->isChecked() ? accentColor : iconColor;
        button->setIcon(Icons::icon(name, color, button->iconSize().width()));
    }

    setFont(m_theme.uiFont());
    m_tree->setFont(m_theme.uiFont());
    for (const auto &leaf : m_leaves) {
        leaf->editor->setTheme(m_theme);
#ifdef NOTEAPPC_HAVE_CANVAS
        if (auto *canvas = qobject_cast<CanvasView *>(leaf->view))
            canvas->setTheme(m_theme);
#endif
#ifdef NOTEAPPC_HAVE_PDFVIEW
        if (auto *pdf = qobject_cast<PdfView *>(leaf->view))
            pdf->setTheme(m_theme);
#endif
        if (auto *media = qobject_cast<MarkdownEditor *>(leaf->view))
            media->setTheme(m_theme);
    }
    if (m_tags)
        m_tags->setTheme(m_theme);
    if (m_properties)
        m_properties->setTheme(m_theme);
    if (m_backlinks)
        m_backlinks->setTheme(m_theme);
    if (m_search)
        m_search->setTheme(m_theme);
#ifdef NOTEAPPC_HAVE_GRAPH
    if (m_localGraph)
        m_localGraph->setTheme(m_theme);
    if (m_graph)
        m_graph->setTheme(m_theme);
#endif
    if (m_preview)
        m_preview->applyTheme(m_theme);
    if (m_outgoingLinks)
        m_outgoingLinks->setTheme(m_theme);
    if (m_settingsPage)
        m_settingsPage->applyTheme(m_theme);

    // The inline title shares the editor's centred column.
    for (const auto &leaf : m_leaves)
        updateInlineTitleMargins(leaf.get());

    // Local overrides for what the app-wide stylesheet (Theme::styleSheet(),
    // still shaped around Obsidian's ribbon-and-flat-tab chrome) does not
    // reach: this shell's own header bar, flat underline tabs, icon+label
    // right-sidebar tabs, and the sidebar's accent-tinted selection. The
    // note area itself is untouched - only its surroundings are noteAppC's
    // own.
    m_headerBar->setStyleSheet(
        QStringLiteral(
            "QWidget#headerBar { background: %1; border-bottom: 1px solid %2; }"
            "QToolButton#vaultMenuButton { font-weight: 600; color: %3; border: none; "
            "border-radius: 4px; padding: 4px 10px; }"
            "QToolButton#vaultMenuButton:hover { background: %4; }"
            "QLineEdit#headerSearch { background: %5; border: 1px solid %2; border-radius: 8px; "
            "padding: 5px 10px; color: %3; }"
            "QLineEdit#headerSearch:focus { border-color: %6; }")
            .arg(m_theme.value(QStringLiteral("--background-secondary")),
                m_theme.value(QStringLiteral("--divider-color")),
                m_theme.value(QStringLiteral("--text-normal")),
                m_theme.value(QStringLiteral("--background-modifier-hover")))
            .arg(m_theme.value(QStringLiteral("--background-modifier-form-field")),
                m_theme.value(QStringLiteral("--interactive-accent"))));

    for (const auto &group : m_groups)
        group->tabs->setStyleSheet(tabBarStyle());

    if (auto *tabRow = findChild<QWidget *>(QStringLiteral("rightSidebarTabs"))) {
        tabRow->setStyleSheet(
            QStringLiteral(
                "QWidget#rightSidebarTabs QToolButton { background: transparent; color: %1; "
                "border: none; border-radius: 4px; padding: 4px 8px; font-size: %2px; }"
                "QWidget#rightSidebarTabs QToolButton:hover { background: %3; }"
                "QWidget#rightSidebarTabs QToolButton:checked { color: %4; border-bottom: 2px "
                "solid %5; }")
                .arg(m_theme.value(QStringLiteral("--text-muted")),
                    m_theme.value(QStringLiteral("--font-ui-smaller")),
                    m_theme.value(QStringLiteral("--background-modifier-hover")))
                .arg(m_theme.value(QStringLiteral("--text-normal")), accentColor.name()));
    }

    m_sidebar->setStyleSheet(
        QStringLiteral(
            "QLabel#filesLabel { color: %1; font-size: 11px; font-weight: 700; padding-left: 4px; }"
            "QTreeView#fileExplorer::item:selected, QTreeView#fileExplorer::item:selected:active { "
            "background: %2; border-radius: 6px; color: %3; }")
            .arg(m_theme.value(QStringLiteral("--text-faint")), rgbaString(accentColor, 0.16),
                m_theme.value(QStringLiteral("--text-normal"))));
}

void MainWindow::chooseVault()
{
    const QString start = m_vault.isValid() ? m_vault.root() : QDir::homePath();
    const QString path = QFileDialog::getExistingDirectory(this, tr("Open vault"), start);
    if (!path.isEmpty())
        openVault(path);
}

void MainWindow::reloadSettings(const QString &vaultRoot)
{
    QVariantMap unsaved;
    for (const QString &key : Settings::keys()) {
        if (m_settings.isCustomised(key))
            unsaved.insert(key, m_settings.value(key));
    }
    m_settings = Settings::load(vaultRoot);
    for (auto it = unsaved.constBegin(); it != unsaved.constEnd(); ++it)
        m_settings.setValue(it.key(), it.value());
    m_settingsPage->setContext(&m_settings, vaultRoot);
}

void MainWindow::watchThemeFiles()
{
    const QString schemePath = m_settings.text(QStringLiteral("appearance.scheme"));
    const QString vaultRoot = m_vault.root();
    if (m_themeWatcher && schemePath == m_watchedSchemePath && vaultRoot == m_watchedThemeVaultRoot)
        return;

    if (!m_themeWatcher) {
        m_themeWatcher = new QFileSystemWatcher(this);
        connect(m_themeWatcher, &QFileSystemWatcher::fileChanged, this,
                &MainWindow::reloadThemeFiles);
        connect(m_themeWatcher, &QFileSystemWatcher::directoryChanged, this,
                &MainWindow::reloadThemeFiles);
    }
    const QStringList current = m_themeWatcher->directories() + m_themeWatcher->files();
    if (!current.isEmpty())
        m_themeWatcher->removePaths(current);

    // Nothing is created here. ~/.config/noteAppC appears only when something
    // is saved - opening a stray note must leave nothing behind - so a themes
    // directory that does not exist yet is simply not watched; the settings
    // page lists what is there whenever it opens.
    // Every directory Scheme::available() would look in, so adding or
    // removing a theme file is noticed even when no scheme is selected yet.
    for (const QString &dir : Scheme::searchPaths(vaultRoot)) {
        if (QFileInfo(dir).isDir())
            m_themeWatcher->addPath(dir);
    }
    // The selected scheme's own file, and (the same reason
    // MarkdownEditor::watchStyleFiles watches a directory too) the directory
    // holding it, in case an editor saves by renaming a new file over the
    // old one and drops inotify's watch on the file itself.
    if (!schemePath.isEmpty()) {
        const QString directory = QFileInfo(schemePath).absolutePath();
        if (QFileInfo(directory).isDir() && !m_themeWatcher->directories().contains(directory))
            m_themeWatcher->addPath(directory);
        if (QFile::exists(schemePath))
            m_themeWatcher->addPath(schemePath);
    }

    m_watchedSchemePath = schemePath;
    m_watchedThemeVaultRoot = vaultRoot;
}

void MainWindow::reloadThemeFiles()
{
    const QString schemePath = m_settings.text(QStringLiteral("appearance.scheme"));
    if (!schemePath.isEmpty() && QFile::exists(schemePath) && m_themeWatcher
        && !m_themeWatcher->files().contains(schemePath)) {
        m_themeWatcher->addPath(schemePath);
    }
    m_theme = themeFromSettings();
    applyTheme();
    // A theme added, removed or renamed changes what the combo box should
    // list; setContext() only rebuilds immediately if the page happens to be
    // visible right now, and otherwise just marks it stale for next time.
    m_settingsPage->setContext(&m_settings, m_vault.root());
}

void MainWindow::clearOpenNote()
{
    rememberCurrentPosition();
    m_active->notePath.clear();
    m_active->note = Note{};
    m_active->editor->setNoteText(QString());
    m_active->editor->setEnabled(false);
    m_active->inlineTitle->setVisible(false);
    syncTabs(m_active->group);
    showNoteInPanes(QString());
}

void MainWindow::openVault(const QString &path)
{
    if (!maybeSaveAll())
        return;

    Vault vault(path);
    if (!vault.isValid()) {
        QMessageBox::warning(this, tr("Open vault"), tr("%1 is not a directory.").arg(path));
        return;
    }

    saveLayout(); // the vault being left
    m_openMode = OpenMode::Vault;
    m_vault = vault;
    resetWorkspace();
    resetHistory();

    m_sidebar->setVisible(true);
    m_rightSidebar->setVisible(true);
    syncTabs(m_active->group);

    m_tree->setRootIndex(m_files->setRootPath(m_vault.root()));
    m_indexer->setVault(m_vault);

    reloadSettings(m_vault.root());
    m_settings.noteOpened(m_vault.root(), QString());
    applySettings();
    if (m_bookmarks)
        m_bookmarks->setVaultRoot(m_vault.root());
    updateNoteHistory();
    restoreLayout(Workspaces::current(m_vault.root()));
#ifdef NOTEAPPC_HAVE_GRAPH
    if (m_graph)
        m_graph->setOptions(GraphOptions::readFromVault(m_vault.root()));
#endif

    statusBar()->showMessage(m_vault.hasObsidianConfig()
                                 ? tr("Vault opened - Obsidian settings found")
                                 : tr("Vault opened"),
                             4000);
    updateTitle();
    updateWordCount();
}

void MainWindow::closeVault()
{
    if (!maybeSaveAll())
        return;

    saveLayout();
    m_openMode = OpenMode::Empty;
    m_vault = Vault();
    m_indexer->setVault(Vault()); // its scan stopped first: it writes to the cache
    m_index.close();
    updateSync(); // stops syncing the vault just closed
    resetWorkspace();
    resetHistory();

    m_tree->setRootIndex(QModelIndex());
    m_files->setRootPath(QString());
    if (m_bookmarks)
        m_bookmarks->setVaultRoot(QString());

    reloadSettings(QString());
    applySettings();

    updateTitle();
    updateWordCount();
    statusBar()->showMessage(tr("Vault closed"), 3000);
}

void MainWindow::openSingleFile(const QString &path)
{
    if (!maybeSaveAll())
        return;

    saveLayout();
    m_openMode = OpenMode::SingleFile;
    m_vault = Vault();
    m_indexer->setVault(Vault()); // its scan stopped first: it writes to the cache
    m_index.close();
    resetWorkspace();
    resetHistory();

    m_sidebar->setVisible(false);
    m_rightSidebar->setVisible(false);
    syncTabs(m_active->group);

    reloadSettings(QString());
    applySettings();

    openNote(path);
}

void MainWindow::openTarget(const LaunchTarget &target)
{
    switch (target.mode) {
    case OpenMode::Vault:
        openVault(target.vaultRoot);
        if (!target.filePath.isEmpty())
            openNote(target.filePath);
        break;
    case OpenMode::SingleFile:
        openSingleFile(target.filePath);
        break;
    case OpenMode::Empty: {
        // Nothing named on the command line: pick up where the last session
        // left off, or stay an empty window.
        const QString last = m_settings.recentVaults().value(0);
        if (!last.isEmpty() && QFileInfo(last).isDir()) {
            openVault(last);
            // And within that vault, whichever note was open when it was
            // last used - the most recently remembered position that still
            // lives under this vault's root.
            for (const QString &candidate : m_settings.rememberedNotes()) {
                if (m_vault.contains(candidate) && QFileInfo(candidate).exists()) {
                    openNote(candidate);
                    break;
                }
            }
        } else {
            updateTitle();
        }
        break;
    }
    }
    // After the first paint: openWhenShown queued this call.
    if (!m_painted && featureOn("file-recovery")) {
        // Old snapshots go once a session, with the window already up.
        const qint64 keep = qint64(m_settings.integer(QStringLiteral("fileRecovery.keepDays"))) * 24 * 3600 * 1000;
        Snapshots::prune(keep);
#ifdef NOTEAPPC_HAVE_PDFVIEW
        PdfRecovery::prune(keep);
#endif
    }
    m_painted = true;
    syncPlugins();
    updateSync();
    watchThemeFiles();
    updateSpelling();
}

void MainWindow::openWhenShown(const LaunchTarget &target)
{
    m_pendingOpen = target;
    update(); // make sure a paint is coming even if nothing else asks for one
}

void MainWindow::paintEvent(QPaintEvent *event)
{
    QMainWindow::paintEvent(event);
    if (!m_pendingOpen)
        return;

    // The shell is on screen now, so the wait for the vault happens with
    // something to look at. Queued rather than run here: a paint handler is no
    // place to open a database and read a note.
    const LaunchTarget target = *m_pendingOpen;
    m_pendingOpen.reset();
    QTimer::singleShot(0, this, [this, target] { openTarget(target); });
}

void MainWindow::onTreeActivated(const QModelIndex &index)
{
    // A folder toggles from onTreeClicked() (mouse) or the Enter-key handling
    // in eventFilter() (keyboard), never from here: activated() also fires on
    // top of a double-click's own two clicks and on top of Enter's own key
    // event, and toggling in both places would toggle it twice.
    if (!index.isValid() || m_files->isDir(index))
        return;
    openNote(m_files->filePath(index), NavSource::Tree);
}

void MainWindow::onTreeClicked(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    if (m_files->isDir(index)) {
        leaveSettings();
        m_tree->setExpanded(index, !m_tree->isExpanded(index));
        return;
    }
    onTreeActivated(index);
}

bool MainWindow::openNote(const QString &rawPath, NavSource source, const QString &fragment)
{
    // Any navigation - the tree, the switcher, a link, history - leaves the
    // settings page if it was open, even below, where a same-path or
    // not-a-note early return means nothing else here runs: a click behind an
    // overlay must not load invisibly behind it.
    leaveSettings();

    // A caller outside the tree view - the screenshot tool, a future
    // command-line argument - may hand this a path relative to the working
    // directory. Index::backlinks/outgoingLinks resolve against the vault
    // root with QDir::relativeFilePath, which silently misreads a relative
    // input as relative to the vault instead of the cwd, so every pane that
    // keys off m_active->notePath comes up empty. Normalise once, here, so nothing
    // downstream has to guess.
    const QString path = QFileInfo(rawPath).absoluteFilePath();
    if (path == m_active->notePath)
        return true;
    if (isViewFile(path)) {
        if (!maybeLeave())
            return false;
        return openView(path, source, fragment);
    }
    if (!Vault::isNote(path)) {
        if (source != NavSource::History) {
            statusBar()->showMessage(tr("%1 is not a note - editors for other file types come later")
                                         .arg(QFileInfo(path).fileName()),
                                     4000);
        }
        return false;
    }
    if (!maybeLeave())
        return false;

    Note note;
    QString error;
    if (!Vault::read(path, note, &error)) {
        // Replaying history over a note that has since been deleted or
        // renamed is not an error, just a dead end to skip past.
        if (source != NavSource::History)
            QMessageBox::warning(this, tr("Open note"), tr("Could not read %1:\n%2").arg(path, error));
        return false;
    }

    // The note being left keeps its place - in memory for the rest of this
    // session, and on disk from here - before anything about it changes.
    rememberCurrentPosition();
    pushHistory(path, source);
    closeView(m_active);

    m_active->note = note;
    m_active->notePath = path;
    m_active->editor->setEnabled(true);
    // Open in another tab already: that document, unsaved edits and all,
    // not the older bytes on disk.
    const QList<Leaf *> others = viewsOfSameNote(m_active);
    m_active->editor->setNoteText(others.isEmpty() ? m_active->note.text : others.constFirst()->editor->noteText());
    if (!others.isEmpty())
        m_active->editor->setModified(others.constFirst()->editor->isModified());
    if (const auto position = m_settings.notePosition(path)) {
        m_active->editor->setCursorOffset(qMin(position->cursor, qsizetype(m_active->note.text.size())));
        m_active->editor->verticalScrollBar()->setValue(position->scroll);
    }
    m_active->editor->setFocus();

    const QString title = QFileInfo(path).completeBaseName();
    m_active->inlineTitle->setText(title);
    m_active->inlineTitle->setVisible(m_settings.boolean(QStringLiteral("appearance.showInlineTitle")));
    syncTabs(m_active->group);

    showNoteInPanes(path);
    m_settings.noteOpened(m_vault.isValid() ? m_vault.root() : QString(), path);
    updateTitle();
    updateWordCount();

    // The tree already shows its own row selected; every other route in -
    // switcher, link, history - reveals the note there instead of leaving the
    // sidebar pointed at whatever was open before.
    if (source != NavSource::Tree)
        revealInTree(path);
    if (source != NavSource::History)
        updateHistoryButtons();
    notifyPlugins("note-opened", path);
    return true;
}

QJsonObject MainWindow::layoutJSON() const
{
    // Obsidian's ids are 16 hex digits; any unique string reads back.
    int counter = 0;
    const auto nextID = [&counter] { return QString::number(++counter, 16).rightJustified(16, u'0'); };
    QString activeID;
    const std::function<QJsonObject(QWidget *)> node = [&](QWidget *widget) -> QJsonObject {
        if (auto *split = qobject_cast<QSplitter *>(widget)) {
            const QList<int> sizes = split->sizes();
            const double total = std::accumulate(sizes.cbegin(), sizes.cend(), 0.0);
            QJsonArray children;
            for (int i = 0; i < split->count(); ++i) {
                QJsonObject child = node(split->widget(i));
                if (total > 0)
                    child.insert(QStringLiteral("dimension"), 100.0 * sizes.value(i) / total);
                children.append(child);
            }
            // Obsidian names a split by its divider: "vertical" sits side by side.
            return {{QStringLiteral("id"), nextID()},
                    {QStringLiteral("type"), QStringLiteral("split")},
                    {QStringLiteral("children"), children},
                    {QStringLiteral("direction"), split->orientation() == Qt::Horizontal
                                                      ? QStringLiteral("vertical")
                                                      : QStringLiteral("horizontal")}};
        }
        const auto owner = std::find_if(m_groups.cbegin(), m_groups.cend(),
                                        [widget](const auto &group) { return group->widget == widget; });
        if (owner == m_groups.cend())
            return {};
        const TabGroup *group = owner->get();
        QJsonArray tabs;
        int currentTab = 0;
        for (int i = 0; i < group->leaves.size(); ++i) {
            const Leaf *leaf = group->leaves.at(i);
            const QString path = leaf->notePath.isEmpty() ? leaf->pendingPath : leaf->notePath;
            QJsonObject view{{QStringLiteral("type"), QStringLiteral("empty")},
                             {QStringLiteral("state"), QJsonObject()}};
            if (!path.isEmpty() && m_vault.isValid()) {
                const QString type = path.endsWith(QLatin1String(".canvas")) ? QStringLiteral("canvas")
                                   : path.endsWith(QLatin1String(".base"))   ? QStringLiteral("bases")
                                   : path.endsWith(QLatin1String(".epub"))   ? QStringLiteral("epub")
                                   : path.endsWith(QLatin1String(".pdf"))    ? QStringLiteral("pdf")
                                   : !mediaKindOf(path).isEmpty()            ? mediaKindOf(path)
                                                                            : QStringLiteral("markdown");
                view = {{QStringLiteral("type"), type},
                        {QStringLiteral("state"), QJsonObject{{QStringLiteral("file"), m_vault.relative(path)}}}};
            }
            const QString id = nextID();
            if (leaf == m_active)
                activeID = id;
            if (leaf->area == group->stack->currentWidget())
                currentTab = i;
            tabs.append(QJsonObject{{QStringLiteral("id"), id},
                                    {QStringLiteral("type"), QStringLiteral("leaf")},
                                    {QStringLiteral("state"), view}});
        }
        return {{QStringLiteral("id"), nextID()},
                {QStringLiteral("type"), QStringLiteral("tabs")},
                {QStringLiteral("children"), tabs},
                {QStringLiteral("currentTab"), currentTab}};
    };
    QJsonObject layout{{QStringLiteral("main"), node(m_workspace)}};
    layout.insert(QStringLiteral("left"),
                  QJsonObject{{QStringLiteral("collapsed"), !m_sidebar->isVisible()}});
    layout.insert(QStringLiteral("right"),
                  QJsonObject{{QStringLiteral("collapsed"), !m_rightSidebar->isVisible()}});
    layout.insert(QStringLiteral("active"), activeID);
    return layout;
}

bool MainWindow::restoreLayout(const QJsonObject &layout)
{
    const QJsonObject main = layout.value(QStringLiteral("main")).toObject();
    if (main.value(QStringLiteral("type")).toString() != QLatin1String("split") || !m_vault.isValid())
        return false;
    resetWorkspace(); // one empty tab in one split, straight in m_workspace
    TabGroup *spareGroup = m_active->group;
    Leaf *spareLeaf = m_active;
    QHash<QString, Leaf *> byID;
    QList<std::pair<TabGroup *, int>> shown;

    const std::function<void(const QJsonObject &, QSplitter *)> build =
        [&](const QJsonObject &node, QSplitter *parent) {
            const QString type = node.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("split")) {
                QSplitter *split = parent ? new QSplitter : m_workspace;
                split->setChildrenCollapsible(false);
                split->setOrientation(node.value(QStringLiteral("direction")).toString()
                                              == QLatin1String("horizontal")
                                          ? Qt::Vertical
                                          : Qt::Horizontal);
                if (parent)
                    parent->addWidget(split);
                QList<int> sizes;
                for (const QJsonValue &child : node.value(QStringLiteral("children")).toArray()) {
                    build(child.toObject(), split);
                    sizes << int(child.toObject().value(QStringLiteral("dimension")).toDouble(0) * 100);
                }
                if (!sizes.contains(0) && sizes.size() == split->count())
                    split->setSizes(sizes);
                return;
            }
            if (type != QLatin1String("tabs") || !parent)
                return;
            TabGroup *group = spareGroup ? std::exchange(spareGroup, nullptr) : newGroup();
            parent->addWidget(group->widget);
            for (const QJsonValue &value : node.value(QStringLiteral("children")).toArray()) {
                const QJsonObject tab = value.toObject();
                if (tab.value(QStringLiteral("type")).toString() != QLatin1String("leaf"))
                    continue;
                Leaf *leaf = spareLeaf ? std::exchange(spareLeaf, nullptr) : newLeaf(group);
                const QJsonObject view = tab.value(QStringLiteral("state")).toObject();
                const QString file =
                    view.value(QStringLiteral("state")).toObject().value(QStringLiteral("file")).toString();
                const QString kind = view.value(QStringLiteral("type")).toString();
                const bool opens = kind == QLatin1String("markdown") || kind == QLatin1String("canvas")
                                || kind == QLatin1String("bases") || kind == QLatin1String("epub")
                                || kind == QLatin1String("pdf") || kind == QLatin1String("image")
                                || kind == QLatin1String("audio") || kind == QLatin1String("video");
                if (opens && !file.isEmpty() && QFileInfo::exists(m_vault.absolute(file)))
                    leaf->pendingPath = m_vault.absolute(file);
                byID.insert(tab.value(QStringLiteral("id")).toString(), leaf);
            }
            shown << std::pair{group, node.value(QStringLiteral("currentTab")).toInt()};
            syncTabs(group);
        };
    build(main, nullptr);

    // Each split's own tab first, the focused one last so it ends up active.
    for (const auto &[group, index] : std::as_const(shown)) {
        if (!group->leaves.isEmpty())
            activateLeaf(group->leaves.at(qBound(0, index, int(group->leaves.size()) - 1)));
    }
    if (Leaf *active = byID.value(layout.value(QStringLiteral("active")).toString()))
        activateLeaf(active);
    for (const auto &group : m_groups)
        syncTabs(group.get());

    m_sidebar->setVisible(!layout.value(QStringLiteral("left")).toObject().value(QStringLiteral("collapsed")).toBool());
    m_rightSidebar->setVisible(
        !layout.value(QStringLiteral("right")).toObject().value(QStringLiteral("collapsed")).toBool());
    return true;
}

void MainWindow::saveLayout()
{
    if (m_openMode == OpenMode::Vault && m_vault.isValid())
        Workspaces::saveCurrent(m_vault.root(), layoutJSON());
}

void MainWindow::loadNamedLayout(const QString &name)
{
    QJsonObject all = Workspaces::named(m_vault.root());
    const QJsonObject layout =
        all.value(QStringLiteral("workspaces")).toObject().value(name).toObject();
    if (layout.isEmpty() || !maybeSaveAll())
        return;
    restoreLayout(layout);
    all.insert(QStringLiteral("active"), name);
    Workspaces::saveNamed(m_vault.root(), all);
    if (m_active->editor->isVisible())
        m_active->editor->setFocus();
}

void MainWindow::manageLayouts()
{
    if (!m_vault.isValid())
        return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("workspacesDialog"));
    dialog.setWindowTitle(tr("Manage workspace layouts"));
    auto *name = new QLineEdit(&dialog);
    name->setObjectName(QStringLiteral("workspaceName"));
    name->setPlaceholderText(tr("Name of workspace layout"));
    auto *save = new QPushButton(tr("Save"), &dialog);
    save->setObjectName(QStringLiteral("workspaceSave"));
    auto *list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("workspaceList"));
    auto *load = new QPushButton(tr("Load"), &dialog);
    load->setObjectName(QStringLiteral("workspaceLoad"));
    auto *remove = new QPushButton(tr("Delete layout"), &dialog);
    remove->setObjectName(QStringLiteral("workspaceDelete"));

    const auto refill = [this, list] {
        list->clear();
        list->addItems(Workspaces::named(m_vault.root()).value(QStringLiteral("workspaces")).toObject().keys());
    };
    refill();
    connect(save, &QPushButton::clicked, &dialog, [this, name, refill] {
        const QString title = name->text().trimmed();
        if (title.isEmpty())
            return;
        QJsonObject all = Workspaces::named(m_vault.root());
        QJsonObject layouts = all.value(QStringLiteral("workspaces")).toObject();
        layouts.insert(title, layoutJSON()); // the same name updates it, as in Obsidian
        all.insert(QStringLiteral("workspaces"), layouts);
        all.insert(QStringLiteral("active"), title);
        Workspaces::saveNamed(m_vault.root(), all);
        refill();
    });
    connect(load, &QPushButton::clicked, &dialog, [this, list, &dialog] {
        if (QListWidgetItem *item = list->currentItem()) {
            const QString title = item->text();
            dialog.accept();
            loadNamedLayout(title);
        }
    });
    connect(list, &QListWidget::itemDoubleClicked, load, &QPushButton::click);
    connect(remove, &QPushButton::clicked, &dialog, [this, list, refill] {
        QListWidgetItem *item = list->currentItem();
        if (!item)
            return;
        QJsonObject all = Workspaces::named(m_vault.root());
        QJsonObject layouts = all.value(QStringLiteral("workspaces")).toObject();
        layouts.remove(item->text());
        all.insert(QStringLiteral("workspaces"), layouts);
        Workspaces::saveNamed(m_vault.root(), all);
        refill();
    });

    auto *saveRow = new QHBoxLayout;
    saveRow->addWidget(name, 1);
    saveRow->addWidget(save);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(remove);
    buttons->addWidget(load);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addLayout(saveRow);
    layout->addWidget(list, 1);
    layout->addLayout(buttons);
    dialog.resize(420, 360);
    dialog.exec();
}

bool MainWindow::featureOn(const char *id) const
{
    return m_settings.boolean(Settings::featureKey(id));
}

void MainWindow::updateNoteHistory()
{
#ifdef NOTEAPPC_HAVE_HISTORY
    const bool wanted = m_openMode == OpenMode::Vault && m_vault.isValid() && featureOn("note-history");
    const QString root = wanted ? m_vault.root() : QString();
    if (root == m_noteHistoryRoot && (m_noteHistory != nullptr) == wanted)
        return;
    m_historyPool.waitForDone(); // the old vault's last saves land first
    m_noteHistory.reset();
    m_noteHistoryRoot = root;
    if (!wanted)
        return;
    m_noteHistory = std::make_shared<NoteHistory>(root);
    if (!m_noteHistory->isOpen()) {
        statusBar()->showMessage(tr("Note history is off: %1").arg(m_noteHistory->error()), 6000);
        m_noteHistory.reset();
    }
#endif
}

void MainWindow::updateSync()
{
#ifdef NOTEAPPC_HAVE_SYNC
    const QString server = m_settings.text(QStringLiteral("sync.serverURL")).trimmed();
    const QString email = m_settings.text(QStringLiteral("sync.email")).trimmed();
    const bool configured = m_painted && m_openMode == OpenMode::Vault && m_vault.isValid() && featureOn("sync")
                            && !server.isEmpty() && !email.isEmpty();
    const QString wanted = configured ? m_vault.root() + u'\n' + server + u'\n' + email : QString();
    if (wanted == m_syncFor && (m_sync || m_syncPassword.isEmpty()))
        return;
    m_sync.reset();
    m_syncFor.clear();
    m_syncStatus->setVisible(configured);
    if (!configured)
        return;
    if (m_syncPassword.isEmpty()) {
        m_syncStatus->setText(tr("Sync: not signed in"));
        m_syncFor = wanted;
        return;
    }
    // The password goes to the engine's thread once and is gone from here:
    // it keeps the keys it derives, not the password.
    auto password = std::make_shared<QString>(std::exchange(m_syncPassword, QString()));
    m_sync = std::make_shared<SyncEngine>();
    m_syncFor = wanted;
    connect(m_sync.get(), &SyncEngine::statusChanged, this,
            [this](SyncEngine::Status status, const QString &message) {
                const QString state = status == SyncEngine::Status::Idle          ? tr("synced")
                                      : status == SyncEngine::Status::Error      ? tr("error")
                                      : status == SyncEngine::Status::WaitingForApproval ? tr("waiting for approval")
                                                                                          : tr("syncing");
                m_syncStatus->setText(tr("Sync: %1").arg(state));
                m_syncStatus->setToolTip(message);
            });
    // A device enrolling without the password gets this vault's keys only
    // when a person here says so, having compared the code on both screens.
    connect(m_sync.get(), &SyncEngine::deviceAwaitingApproval, this,
            [this](const QByteArray &deviceID, const QString &fingerprint) {
                QMessageBox ask(QMessageBox::Question, tr("Sync"),
                                tr("A new device asks for this vault's keys. Its code is:\n\n%1\n\n"
                                   "Approve it only if the new device shows exactly this code. A code you "
                                   "cannot find on any of your devices is not yours.")
                                    .arg(fingerprint),
                                QMessageBox::NoButton, this);
                QPushButton *approve = ask.addButton(tr("Approve"), QMessageBox::AcceptRole);
                ask.setDefaultButton(ask.addButton(tr("Not now"), QMessageBox::RejectRole));
                ask.exec();
                if (ask.clickedButton() == approve && m_sync)
                    m_sync->approveDevice(deviceID);
            });
    // A file changed on disk (another app, the tree) is a change to sync too.
    connect(&m_index, &Index::changed, m_sync.get(), &SyncEngine::noteChanged);
    m_syncStatus->setText(tr("Sync: signing in"));
    m_sync->start(m_vault.root(), server, email, [password](const QString &) {
        return std::exchange(*password, QString());
    });
#endif
}

void MainWindow::syncSignIn()
{
#ifdef NOTEAPPC_HAVE_SYNC
    const QString email = m_settings.text(QStringLiteral("sync.email")).trimmed();
    if (m_settings.text(QStringLiteral("sync.serverURL")).trimmed().isEmpty() || email.isEmpty()) {
        statusBar()->showMessage(tr("Set a sync server and account in Settings, Core plugins"), 5000);
        return;
    }
    bool ok = false;
    const QString password = QInputDialog::getText(this, tr("Sync"), tr("Password for %1:").arg(email),
                                                   QLineEdit::Password, QString(), &ok);
    if (!ok || password.isEmpty())
        return;
    m_syncPassword = password;
    m_syncFor.clear(); // a fresh engine, even for the same account
    updateSync();
#endif
}

void MainWindow::recordHistory(const QString &absolutePath, const QString &text)
{
#ifdef NOTEAPPC_HAVE_HISTORY
    if (!m_noteHistory || !m_vault.contains(absolutePath))
        return;
    const std::shared_ptr<NoteHistory> history = m_noteHistory;
    const QString relative = m_vault.relative(absolutePath);
    // Now and then, fold the loose objects into a pack, where git keeps each
    // version as a delta: never on a save's own path, and only once there
    // are enough to be worth it.
    const bool maybePack = ++m_historyRecords % 256 == 0;
    m_historyPool.start([history, relative, text, maybePack] {
        history->record(relative, text);
        if (maybePack && history->looseObjects() > 4000)
            history->pack();
    });
#else
    Q_UNUSED(absolutePath);
    Q_UNUSED(text);
#endif
}

void MainWindow::openNoteHistory()
{
#ifdef NOTEAPPC_HAVE_HISTORY
    if (!m_noteHistory || m_active->notePath.isEmpty() || m_active->view)
        return;
    const std::shared_ptr<NoteHistory> history = m_noteHistory;
    const QString relative = m_vault.relative(m_active->notePath);
    // Every queued save lands first, then the history is read here: with
    // the pool drained nothing else touches the repository, and the dialog
    // is modal, so nothing can queue behind it. (Not a task on the pool:
    // QFuture::waitForFinished() may run an unstarted task on the calling
    // thread, ahead of the saves and beside the pool's own thread.)
    m_historyPool.waitForDone();
    QList<NoteHistoryDialog::Version> versions;
    for (const NoteHistory::Version &version : history->versions(relative))
        versions.append({version.id, version.time});
    QThreadPool *pool = &m_historyPool;
    const auto read = [history, relative, pool](const QString &id) {
        pool->waitForDone();
        return history->read(relative, id);
    };
    NoteHistoryDialog dialog(QFileInfo(m_active->notePath).completeBaseName(), versions, read,
                             m_active->editor->noteText(), this);
    const QString path = m_active->notePath;
    connect(&dialog, &NoteHistoryDialog::restoreRequested, this, [this, path, &dialog](const QString &text) {
        dialog.accept();
        restoreSnapshot(path, text);
    });
    dialog.exec();
#endif
}

void MainWindow::gotoTab(int index)
{
    const QList<Leaf *> &leaves = m_active->group->leaves;
    if (leaves.isEmpty())
        return;
    activateLeaf(leaves.at(index < 0 ? leaves.size() - 1 : qMin(index, int(leaves.size()) - 1)));
    m_active->editor->setFocus();
}

void MainWindow::pushHistory(const QString &path, NavSource source)
{
    if (source == NavSource::History)
        return;
    // A fresh navigation discards whatever "forward" existed past here, the
    // way a browser does when you follow a new link after going back.
    while (m_active->history.size() > m_active->historyIndex + 1)
        m_active->history.removeLast();
    m_active->history.append(path);
    m_active->historyIndex = m_active->history.size() - 1;
}

QList<MainWindow::Leaf *> MainWindow::viewsOfSameNote(const Leaf *leaf) const
{
    QList<Leaf *> views;
    if (!leaf || leaf->notePath.isEmpty() || leaf->view)
        return views;
    for (const auto &other : m_leaves) {
        if (other.get() != leaf && !other->view && other->notePath == leaf->notePath)
            views << other.get();
    }
    return views;
}

bool MainWindow::isViewFile(const QString &path) const
{
#ifdef NOTEAPPC_HAVE_EPUB
    if (path.endsWith(QLatin1String(".epub"), Qt::CaseInsensitive))
        return featureOn("epub-reader");
#endif
#ifdef NOTEAPPC_HAVE_CANVAS
    if (path.endsWith(QLatin1String(".canvas"), Qt::CaseInsensitive))
        return featureOn("canvas");
#endif
#ifdef NOTEAPPC_HAVE_BASES
    if (path.endsWith(QLatin1String(".base"), Qt::CaseInsensitive))
        return featureOn("bases");
#endif
#ifdef NOTEAPPC_HAVE_PDFVIEW
    if (path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
        return featureOn("pdf-viewer");
#endif
    const QString media = mediaKindOf(path);
    return media == QLatin1String("image") || (!media.isEmpty() && featureOn("media-embeds"));
}

bool MainWindow::openView(const QString &path, NavSource source, const QString &fragment)
{
    QWidget *view = nullptr;
    QString error;
#ifdef NOTEAPPC_HAVE_EPUB
    if (path.endsWith(QLatin1String(".epub"), Qt::CaseInsensitive)) {
        auto *epub = new EpubView(m_active->area);
        epub->setTheme(m_theme);
        if (!epub->load(path, &error)) {
            delete epub;
        } else {
            if (!fragment.isEmpty())
                epub->navigateToFragment(fragment);
            connect(epub, &EpubView::noteActivated, this, &MainWindow::openRelative);
            connect(epub, &EpubView::statusMessage, this,
                    [this](const QString &text, int timeout) { statusBar()->showMessage(text, timeout); });
            view = epub;
        }
    }
#endif
#ifdef NOTEAPPC_HAVE_PDFVIEW
    if (path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) {
        auto *pdf = new PdfView(m_active->area);
        pdf->setTheme(m_theme);
        if (!pdf->load(path, &error)) {
            delete pdf;
        } else {
            if (!fragment.isEmpty())
                pdf->openFragment(fragment);
            Leaf *leaf = m_active;
            connect(pdf, &PdfView::modificationChanged, this, [this, leaf](bool modified) {
                if (leaf == m_active)
                    updateTitle();
                else
                    syncTabs(leaf->group);
                if (modified && m_settings.boolean(QStringLiteral("files.autosave")))
                    m_autosave.start(); // annotations are kept like a note's text
            });
            connect(pdf, &PdfView::noteActivated, this, &MainWindow::openRelative);
            view = pdf;
        }
    }
#endif
    if (!view && !mediaKindOf(path).isEmpty()) {
        // An image, a recording or a film is its own embed, alone, in
        // reading view: the same drawing and player a note gives it.
        auto *media = new MarkdownEditor(m_active->area);
        media->setObjectName(QStringLiteral("mediaView"));
        media->setTheme(m_theme);
        media->setFeatures(m_active->editor->features());
        media->setLinkResolver(m_resolveLink);
        media->setMode(MarkdownEditor::Mode::Reading);
        media->setNoteText(QStringLiteral("![[") + (m_vault.contains(path) ? m_vault.relative(path) : path)
                           + QStringLiteral("]]\n"));
        view = media;
    }
    Q_UNUSED(fragment);
#ifdef NOTEAPPC_HAVE_CANVAS
    if (path.endsWith(QLatin1String(".canvas"), Qt::CaseInsensitive)) {
        auto *canvas = new CanvasView(m_active->area);
        canvas->setTheme(m_theme);
        canvas->setFileResolver(m_resolveLink);
        if (!canvas->load(path, &error)) {
            delete canvas;
        } else {
            Leaf *leaf = m_active;
            connect(canvas, &CanvasView::modificationChanged, this, [this, leaf, canvas] {
                if (leaf == m_active)
                    updateTitle();
                else
                    syncTabs(leaf->group);
                if (canvas->isModified() && m_settings.boolean(QStringLiteral("files.autosave")))
                    m_autosave.start(); // a canvas is kept like a note
            });
            connect(canvas, &CanvasView::noteActivated, this, &MainWindow::openRelative);
            view = canvas;
        }
    }
#endif
#ifdef NOTEAPPC_HAVE_BASES
    if (path.endsWith(QLatin1String(".base"), Qt::CaseInsensitive)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            error = file.errorString();
        } else {
            auto *base = new bases::BaseView(m_active->area);
            base->setIndex(&m_index);
            base->setVault(&m_vault);
            if (!base->open(path, QString::fromUtf8(file.readAll()))) {
                error = base->lastError();
                delete base;
            } else {
                connect(base, &bases::BaseView::noteActivated, this, &MainWindow::openRelative);
                view = base;
            }
        }
    }
#endif
    if (!view) {
        if (source != NavSource::History)
            QMessageBox::warning(this, tr("Open"), tr("Could not open %1:\n%2").arg(path, error));
        return false;
    }

    rememberCurrentPosition();
    pushHistory(path, source);
    closeView(m_active);
    m_active->note = Note{};
    m_active->editor->setNoteText(QString());
    m_active->editor->setEnabled(false);
    m_active->editor->hide();
    m_active->inlineTitle->hide();
    m_active->view = view;
    static_cast<QVBoxLayout *>(m_active->area->layout())->addWidget(view, 1);
    m_active->notePath = path;
    view->setFocus();

    syncTabs(m_active->group);
    showNoteInPanes(path);
    if (m_outline)
        m_outline->clear();
    m_settings.noteOpened(m_vault.isValid() ? m_vault.root() : QString(), path);
    updateTitle();
    updateWordCount();
    if (source != NavSource::Tree)
        revealInTree(path);
    if (source != NavSource::History)
        updateHistoryButtons();
    return true;
}

void MainWindow::closeView(Leaf *leaf)
{
    if (!leaf->view)
        return;
    leaf->view->disconnect(this);
    leaf->view->hide();
    leaf->view->deleteLater();
    leaf->view = nullptr;
    leaf->editor->show();
    leaf->inlineTitle->setVisible(m_settings.boolean(QStringLiteral("appearance.showInlineTitle")));
}

bool MainWindow::isModified(const Leaf *leaf) const
{
#ifdef NOTEAPPC_HAVE_CANVAS
    if (auto *canvas = qobject_cast<CanvasView *>(leaf->view))
        return canvas->isModified();
#endif
#ifdef NOTEAPPC_HAVE_PDFVIEW
    if (auto *pdf = qobject_cast<PdfView *>(leaf->view))
        return pdf->isModified();
#endif
    return leaf->editor->isModified();
}

void MainWindow::rememberCurrentPosition()
{
    if (m_active->notePath.isEmpty() || m_active->view)
        return;
    m_settings.setNotePosition(m_active->notePath, m_active->editor->cursorOffset(),
                               m_active->editor->verticalScrollBar()->value());
    m_settings.save();
}

void MainWindow::revealInTree(const QString &path)
{
    if (!m_tree || !m_vault.isValid())
        return;
    const QModelIndex index = m_files->index(path);
    if (!index.isValid()) {
        // QFileSystemModel fills each directory lazily on a worker thread, so
        // a note revealed right after a vault switch may have no index yet.
        // Wait for its own directory to finish loading and try once more; the
        // connection disconnects itself once that happens, so it does not
        // linger listening for a directory nobody cares about any more.
        const QString dirPath = QFileInfo(path).absolutePath();
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = connect(m_files, &QFileSystemModel::directoryLoaded, this,
                              [this, path, dirPath, connection](const QString &loaded) {
            if (loaded != dirPath)
                return;
            QObject::disconnect(*connection);
            // The user may have navigated elsewhere by the time this fires.
            if (path == m_active->notePath)
                revealInTree(path);
        });
        return;
    }
    for (QModelIndex parent = m_files->parent(index); parent.isValid();
        parent = m_files->parent(parent))
        m_tree->expand(parent);
    m_tree->setCurrentIndex(index);
    if (auto *selection = m_tree->selectionModel())
        selection->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_tree->scrollTo(index);
}

void MainWindow::updateHistoryButtons()
{
    if (m_backButton)
        m_backButton->setEnabled(m_active->historyIndex > 0);
    if (m_forwardButton)
        m_forwardButton->setEnabled(m_active->historyIndex >= 0 && m_active->historyIndex < m_active->history.size() - 1);
}

void MainWindow::resetHistory()
{
    m_active->history.clear();
    m_active->historyIndex = -1;
    updateHistoryButtons();
}

void MainWindow::goBack()
{
    for (int i = m_active->historyIndex - 1; i >= 0; --i) {
        if (openNote(m_active->history.at(i), NavSource::History)) {
            m_active->historyIndex = i;
            updateHistoryButtons();
            return;
        }
    }
}

void MainWindow::goForward()
{
    for (int i = m_active->historyIndex + 1; i < m_active->history.size(); ++i) {
        if (openNote(m_active->history.at(i), NavSource::History)) {
            m_active->historyIndex = i;
            updateHistoryButtons();
            return;
        }
    }
}

bool MainWindow::save()
{
    return saveLeaf(m_active, false);
}

bool MainWindow::saveLeaf(Leaf *target, bool quiet)
{
    if (target->notePath.isEmpty())
        return true;
#ifdef NOTEAPPC_HAVE_CANVAS
    if (auto *canvas = qobject_cast<CanvasView *>(target->view)) {
        QString error;
        if (!canvas->save(QString(), &error)) {
            reportSaveError(target->notePath, error, quiet);
            return false;
        }
        if (!quiet)
            statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(target->notePath).fileName()), 2000);
        return true;
    }
#endif
#ifdef NOTEAPPC_HAVE_PDFVIEW
    if (auto *pdf = qobject_cast<PdfView *>(target->view)) {
        QString error;
        if (!pdf->save(&error)) {
            reportSaveError(target->notePath, error, quiet);
            return false;
        }
        if (!quiet)
            statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(target->notePath).fileName()), 2000);
        return true;
    }
#endif
    if (target->view)
        return true; // a base writes each cell edit to its note as it happens

    Note note = target->note;
    note.text = target->editor->noteText();

    // File recovery, for notes in a vault only: a stray note leaves nothing
    // behind. The first save of a note keeps what it held before, too.
    const bool snapshots =
        featureOn("file-recovery") && m_vault.isValid() && m_vault.contains(target->notePath);
    if (snapshots && Snapshots::versions(target->notePath).isEmpty() && note.text != target->note.text)
        Snapshots::record(target->notePath, target->note.text, 0);

    QString error;
    if (!Vault::write(target->notePath, note, &error)) {
        reportSaveError(target->notePath, error, quiet);
        return false;
    }

    if (snapshots)
        Snapshots::record(target->notePath, note.text,
                          qint64(m_settings.integer(QStringLiteral("fileRecovery.interval"))) * 60'000);
    recordHistory(target->notePath, note.text);
    notifyPlugins("note-saved", target->notePath);
#ifdef NOTEAPPC_HAVE_SYNC
    if (m_sync)
        m_sync->noteChanged();
#endif
    target->note = note;
    target->editor->setModified(false);
    // Every other tab on this note shows this same document, so this saved
    // it too.
    for (Leaf *leaf : viewsOfSameNote(target)) {
        leaf->note = note;
        if (leaf->editor->noteText() == note.text) {
            leaf->editor->setModified(false);
            continue;
        }
        const qsizetype caret = leaf->editor->cursorOffset();
        leaf->editor->setNoteText(note.text);
        leaf->editor->setCursorOffset(qMin(caret, qsizetype(note.text.size())));
    }
    if (!quiet) {
        statusBar()->showMessage(tr("Saved %1").arg(m_vault.isValid() ? m_vault.relative(target->notePath)
                                                                       : QFileInfo(target->notePath).fileName()),
                                 2000);
    }
    if (m_vault.isValid())
        m_indexer->reindex(target->notePath);
    return true;
}

void MainWindow::reportSaveError(const QString &path, const QString &error, bool quiet)
{
    // An autosave runs while someone types: a box in their face then is
    // worse than the failure, which the next pause tries again anyway. The
    // bar keeps saying so until a save goes through.
    if (quiet)
        statusBar()->showMessage(tr("Could not save %1: %2 - will try again").arg(QFileInfo(path).fileName(), error));
    else
        QMessageBox::warning(this, tr("Save"), tr("Could not save %1:\n%2").arg(path, error));
}

void MainWindow::autosave()
{
    QSet<QString> saved; // a note in two tabs is one document, saved once
    for (const auto &leaf : m_leaves) {
        if (leaf->notePath.isEmpty() || !isModified(leaf.get()) || saved.contains(leaf->notePath))
            continue;
        saved.insert(leaf->notePath);
        saveLeaf(leaf.get(), true);
    }
}

bool MainWindow::maybeSave()
{
    if (m_active->notePath.isEmpty() || !isModified(m_active))
        return true;
    // Obsidian's way: what is typed is kept, never asked about.
    if (m_settings.boolean(QStringLiteral("files.autosave")))
        return saveLeaf(m_active, true);

    const QString shown =
        m_vault.isValid() ? m_vault.relative(m_active->notePath) : QFileInfo(m_active->notePath).fileName();
    const auto answer = QMessageBox::question(
        this, tr("Unsaved changes"), tr("Save changes to %1?").arg(shown),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Discard)
        return true;
    return save();
}

void MainWindow::onModificationChanged(bool)
{
    updateTitle();
}

void MainWindow::updateTitle()
{
    const bool modified = !m_active->notePath.isEmpty() && isModified(m_active);
    QString title;
    if (!m_active->notePath.isEmpty()) {
        title = (m_vault.isValid() ? m_vault.relative(m_active->notePath) : QFileInfo(m_active->notePath).fileName())
              + QLatin1String(" - ");
    }
    if (m_vault.isValid())
        title += m_vault.name() + QLatin1String(" - ");
    title += QStringLiteral("noteAppC");
    setWindowTitle(modified ? QLatin1Char('*') + title : title);

    syncTabs(m_active->group);
    m_vaultLabel->setText(m_vault.isValid() ? m_vault.name() : tr("No vault open"));

    if (m_vaultMenuButton) {
        if (m_openMode == OpenMode::SingleFile && !m_active->notePath.isEmpty())
            m_vaultMenuButton->setText(QFileInfo(m_active->notePath).fileName());
        else
            m_vaultMenuButton->setText(m_vault.isValid() ? m_vault.name() : tr("No vault"));
    }
}

void MainWindow::updateWordCount()
{
    // A canvas or a base has no words of its own to count.
    if (m_active->notePath.isEmpty() || m_active->view || !featureOn("word-count")) {
        m_wordCount->clear();
        return;
    }
    const int words = m_active->editor->wordCount();
    if (words < 0)
        m_wordCount->clear(); // a large note, still being counted
    else
        m_wordCount->setText(tr("%n word(s)", nullptr, words));
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    const bool narrow = width() < kNarrowWidth;
    if (narrow == m_narrow || !m_sidebar || !m_rightSidebar)
        return;
    m_narrow = narrow;
    if (narrow) {
        m_leftBeforeNarrow = m_sidebar->isVisible();
        m_rightBeforeNarrow = m_rightSidebar->isVisible();
        m_sidebar->hide();
        m_rightSidebar->hide();
    } else if (m_openMode != OpenMode::SingleFile) {
        m_sidebar->setVisible(m_leftBeforeNarrow);
        m_rightSidebar->setVisible(m_rightBeforeNarrow);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!maybeSaveAll()) {
        event->ignore();
        return;
    }
    for (const auto &leaf : m_leaves) {
        if (!leaf->notePath.isEmpty() && !leaf->view)
            m_settings.setNotePosition(leaf->notePath, leaf->editor->cursorOffset(),
                                       leaf->editor->verticalScrollBar()->value());
    }
    saveLayout();
    m_settings.save();
    QSettings settings;
    settings.setValue(kGeometry, saveGeometry());
    if (auto *splitter = findChild<QSplitter *>(QStringLiteral("mainSplitter")))
        settings.setValue(kSplitter, splitter->saveState());
    event->accept();
}

// --- file operations: New note/folder, Rename, Delete ---------------------

QString MainWindow::folderRelativeFor(const QModelIndex &index) const
{
    if (!index.isValid())
        return QString();
    const QString path = m_files->filePath(index);
    const QString dirPath = m_files->isDir(index) ? path : QFileInfo(path).absolutePath();
    return m_vault.relative(dirPath);
}

void MainWindow::showTreeContextMenu(const QPoint &pos)
{
    if (!m_vault.isValid())
        return;

    const QModelIndex index = m_tree->indexAt(pos);
    QMenu menu(this);
    QAction *newNoteAction = menu.addAction(tr("New note"));
    QAction *newFolderAction = menu.addAction(tr("New folder"));
    QAction *renameAction = nullptr;
    QAction *deleteAction = nullptr;
    if (index.isValid()) {
        menu.addSeparator();
        renameAction = menu.addAction(tr("Rename"));
        deleteAction = menu.addAction(tr("Delete"));
    }

    QAction *chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == newNoteAction)
        newNoteIn(folderRelativeFor(index));
    else if (chosen == newFolderAction)
        newFolderIn(folderRelativeFor(index));
    else if (chosen == renameAction)
        renamePathAt(index);
    else if (chosen == deleteAction)
        deletePathAt(index);
}

void MainWindow::newNoteIn(const QString &folderRelative)
{
    if (!m_vault.isValid())
        return;
    QString error;
    const QString path = m_vault.createNote(folderRelative, tr("Untitled"), &error);
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("New note"), tr("Could not create the note:\n%1").arg(error));
        return;
    }
    m_indexer->reindex(path);
    openNote(path);
}

void MainWindow::newFolderIn(const QString &folderRelative)
{
    if (!m_vault.isValid())
        return;
    QString error;
    const QString path = m_vault.createFolder(folderRelative, tr("New folder"), &error);
    if (path.isEmpty())
        QMessageBox::warning(this, tr("New folder"),
                             tr("Could not create the folder:\n%1").arg(error));
}

void MainWindow::renamePathAt(const QModelIndex &index)
{
    if (!index.isValid() || !m_vault.isValid())
        return;

    const QString oldPath = m_files->filePath(index);
    const bool isDir = m_files->isDir(index);
    const QString currentName =
        isDir ? QFileInfo(oldPath).fileName() : QFileInfo(oldPath).completeBaseName();

    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                                   QLineEdit::Normal, currentName, &ok);
    if (!ok || newName.isEmpty() || newName == currentName)
        return;

    const QString oldRelative = m_vault.relative(oldPath);
    QString newPath;
    QString error;
    if (!m_vault.renamePath(oldPath, newName, &newPath, &error)) {
        QMessageBox::warning(this, tr("Rename"), tr("Could not rename %1:\n%2").arg(currentName, error));
        return;
    }
    const QString newRelative = m_vault.relative(newPath);
    notifyPlugins("note-renamed", newPath);
#ifdef NOTEAPPC_HAVE_HISTORY
    // The history follows the note: every note under a renamed folder too.
    if (m_noteHistory) {
        QList<std::pair<QString, QString>> moves;
        if (isDir) {
            QDirIterator notes(newPath, {QStringLiteral("*.md")}, QDir::Files, QDirIterator::Subdirectories);
            while (notes.hasNext()) {
                const QString now = m_vault.relative(notes.next());
                moves.append({oldRelative + now.mid(newRelative.size()), now});
            }
        } else {
            moves.append({oldRelative, newRelative});
        }
        const std::shared_ptr<NoteHistory> history = m_noteHistory;
        m_historyPool.start([history, moves] {
            for (const auto &[from, to] : moves)
                history->rename(from, to);
        });
    }
#endif

    if (m_settings.boolean(QStringLiteral("links.autoUpdate")) && m_index.isOpen()) {
        QString linkError;
        if (m_index.updateLinksAfterRename(oldRelative, newRelative, m_vault, &linkError) < 0) {
            statusBar()->showMessage(tr("Renamed, but links could not be updated: %1").arg(linkError),
                                     4000);
        }
    }

    // A tab's note may be the renamed item itself, or nested inside a
    // renamed folder; either way its path just moved under it.
    for (const auto &leaf : m_leaves) {
        if (leaf->notePath.isEmpty())
            continue;
        if (leaf->notePath == oldPath) {
            leaf->notePath = newPath;
            leaf->inlineTitle->setText(QFileInfo(newPath).completeBaseName());
        } else if (isDir && leaf->notePath.startsWith(oldPath + QLatin1Char('/'))) {
            leaf->notePath.replace(0, oldPath.size(), newPath);
        }
    }
    for (const auto &group : m_groups)
        syncTabs(group.get());

    // Picks up the moved path(s) under their new name and drops the old
    // ones, notes and attachments alike - simpler and just as correct as
    // patching the renamed rows by hand, folder rename included.
    m_indexer->rebuild();
    updateTitle();
}

void MainWindow::deletePathAt(const QModelIndex &index)
{
    if (!index.isValid() || !m_vault.isValid())
        return;

    const QString path = m_files->filePath(index);
    const QString name = QFileInfo(path).fileName();

    if (m_settings.boolean(QStringLiteral("files.confirmDelete"))) {
        const auto answer =
            QMessageBox::question(this, tr("Delete"), tr("Move %1 to the vault's trash?").arg(name),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    QString error;
    if (!m_vault.moveToTrash(path, &error)) {
        QMessageBox::warning(this, tr("Delete"), tr("Could not delete %1:\n%2").arg(name, error));
        return;
    }

    // Obsidian closes the tabs of what was deleted.
    QList<Leaf *> gone;
    for (const auto &leaf : m_leaves) {
        if (!leaf->notePath.isEmpty()
            && (leaf->notePath == path || leaf->notePath.startsWith(path + QLatin1Char('/'))))
            gone << leaf.get();
    }
    for (Leaf *leaf : std::as_const(gone)) {
        if (m_leaves.size() > 1) {
            dropLeaf(leaf);
        } else {
            activateLeaf(leaf);
            clearOpenNote();
        }
    }
    m_indexer->rebuild();
    updateTitle();
    updateWordCount();
}

} // namespace noteAppC
