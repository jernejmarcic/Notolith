#include "MainWindow.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <memory>

#include "BacklinksPane.h"
#include "Icons.h"
#include "MarkdownEditor.h"
#include "OutgoingLinksPane.h"
#include "QuickSwitcher.h"
#include "Scheme.h"
#include "SettingsPage.h"
#include "TagsPane.h"

namespace noteAppC {

namespace {
constexpr auto kGeometry = "window/geometry";
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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
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
        onIndexChanged();
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
               [this] { openVault(QFileInfo(m_notePath).absolutePath()); });
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

    m_tabStrip = new QWidget(notesPage);
    m_tabStrip->setObjectName(QStringLiteral("tabStrip"));
    auto *stripLayout = new QHBoxLayout(m_tabStrip);
    stripLayout->setContentsMargins(6, 4, 6, 0);
    stripLayout->setSpacing(0);

    m_tabs = new QTabBar(m_tabStrip);
    m_tabs->setObjectName(QStringLiteral("noteTabs"));
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    m_tabs->setMovable(true);
    m_tabs->setElideMode(Qt::ElideRight);
    stripLayout->addWidget(m_tabs);
    stripLayout->addStretch(1);

    auto *editorArea = new QWidget(notesPage);
    editorArea->setObjectName(QStringLiteral("editorArea"));
    auto *editorLayout = new QVBoxLayout(editorArea);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);

    // ponytail: the inline title is a fixed label above the editor, so it does
    // not scroll away like Obsidian's. It becomes the first block once the
    // Phase 2 engine owns the layout.
    m_inlineTitle = new QLabel(editorArea);
    m_inlineTitle->setObjectName(QStringLiteral("inlineTitle"));
    m_inlineTitle->setVisible(false);

    m_editor = new MarkdownEditor(editorArea);
    // Its Resize events reach eventFilter() (which keeps the inline title on
    // its column) through the application-wide filter installed in the
    // constructor; a second filter installed here on the widget itself would
    // just run that same check twice per resize.
    m_editor->setEnabled(false);
    connect(m_editor, &MarkdownEditor::modificationChanged,
            this, &MainWindow::onModificationChanged);
    // The count comes with each parse. Taking it from textChanged recounted
    // the whole note on the UI thread for every keystroke.
    connect(m_editor, &MarkdownEditor::wordCountChanged, this, &MainWindow::updateWordCount);
    m_editor->setLinkResolver([this](const QString &target) -> QString {
        if (!m_vault.isValid())
            return {};
        // An attachment may not be in the index (it only holds notes), so fall
        // back on the path the note actually wrote.
        const QString resolved = m_index.resolve(target, m_notePath);
        if (!resolved.isEmpty())
            return m_vault.absolute(resolved);
        const QString beside =
            QFileInfo(m_notePath).absolutePath() + QLatin1Char('/') + target;
        if (QFileInfo::exists(beside))
            return beside;
        const QString fromRoot = m_vault.absolute(target);
        return QFileInfo::exists(fromRoot) ? fromRoot : QString();
    });
    connect(m_editor, &MarkdownEditor::linkActivated, this, [this](const QString &target) {
        const QString resolved = m_index.resolve(target, m_notePath);
        if (!resolved.isEmpty())
            openRelative(resolved);
        else
            statusBar()->showMessage(tr("%1 is not in this vault yet").arg(target), 3000);
    });

    editorLayout->addWidget(m_inlineTitle);
    editorLayout->addWidget(m_editor, 1);

    auto *notesLayout = new QVBoxLayout(notesPage);
    notesLayout->setContentsMargins(0, 0, 0, 0);
    notesLayout->setSpacing(0);
    notesLayout->addWidget(m_tabStrip);
    notesLayout->addWidget(editorArea, 1);

    m_settingsPage = new SettingsPage(this);
    connect(m_settingsPage, &SettingsPage::settingChanged, this, &MainWindow::applySettings);
    connect(m_settingsPage, &SettingsPage::closeRequested, this, &MainWindow::closeSettings);

    m_mainStack = new QStackedWidget(this);
    m_mainStack->setObjectName(QStringLiteral("mainStack"));
    m_mainStack->addWidget(notesPage);
    m_mainStack->addWidget(m_settingsPage);
    return m_mainStack;
}

QToolButton *MainWindow::rightSidebarTabButton(QWidget *parent, const QString &objectName,
                                               const QString &icon, const QString &label,
                                               const QString &tooltip, int index)
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
    connect(button, &QToolButton::clicked, this, [this, index] { m_rightPanes->setCurrentIndex(index); });
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

    m_backlinks = new BacklinksPane(m_rightSidebar);
    m_backlinks->setIndex(&m_index);
    connect(m_backlinks, &BacklinksPane::noteActivated, this, &MainWindow::openRelative);

    m_tags = new TagsPane(m_rightSidebar);
    m_tags->setIndex(&m_index);
    connect(m_tags, &TagsPane::tagActivated, this, [this](const QString &tag) {
        // TODO(Phase 5): hand the tag to the search pane instead of jumping
        // straight to the first note that carries it.
        const QStringList notes = m_index.notesWithTag(tag);
        if (!notes.isEmpty())
            openRelative(notes.first());
    });

    m_outgoingLinks = new OutgoingLinksPane(m_rightSidebar);
    m_outgoingLinks->setIndex(&m_index);
    connect(m_outgoingLinks, &OutgoingLinksPane::noteActivated, this, &MainWindow::openRelative);

    m_rightPanes = new QStackedWidget(m_rightSidebar);
    m_rightPanes->setObjectName(QStringLiteral("rightSidebarPanes"));
    m_rightPanes->addWidget(m_backlinks);
    m_rightPanes->addWidget(m_tags);
    m_rightPanes->addWidget(m_outgoingLinks);

    auto *group = new QButtonGroup(tabRow);
    group->setExclusive(true);
    QToolButton *backlinksTab =
        rightSidebarTabButton(tabRow, QStringLiteral("backlinksTabButton"), QStringLiteral("link"),
                              tr("Backlinks"), tr("Backlinks"), 0);
    QToolButton *tagsTab = rightSidebarTabButton(tabRow, QStringLiteral("tagsTabButton"),
                                                 QStringLiteral("tags"), tr("Tags"), tr("Tags"), 1);
    QToolButton *outgoingTab =
        rightSidebarTabButton(tabRow, QStringLiteral("outgoingLinksTabButton"),
                              QStringLiteral("arrow-up-right"), tr("Outgoing"),
                              tr("Outgoing links"), 2);
    group->addButton(backlinksTab);
    group->addButton(tagsTab);
    group->addButton(outgoingTab);
    tabLayout->addWidget(backlinksTab);
    tabLayout->addWidget(tagsTab);
    tabLayout->addWidget(outgoingTab);
    tabLayout->addStretch(1);
    backlinksTab->setChecked(true);

    auto *layout = new QVBoxLayout(m_rightSidebar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tabRow);
    layout->addWidget(m_rightPanes, 1);
    return m_rightSidebar;
}

void MainWindow::buildActions()
{
    // No menu bar: Obsidian has none, and Phase 4 replaces these with the
    // command palette and its hotkeys.
    const auto add = [this](const QString &shortcut, auto slot) {
        auto *action = new QAction(this);
        action->setShortcut(QKeySequence(shortcut));
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this, slot);
        addAction(action);
    };
    add(QStringLiteral("Ctrl+Shift+O"), &MainWindow::chooseVault);
    add(QStringLiteral("Ctrl+S"), [this] { save(); });
    add(QStringLiteral("Ctrl+Alt+S"), &MainWindow::toggleSidebar);
    add(QStringLiteral("Ctrl+Alt+R"), &MainWindow::toggleRightSidebar);
    add(QStringLiteral("Ctrl+O"), &MainWindow::openQuickSwitcher);
    add(QStringLiteral("Ctrl+E"), &MainWindow::toggleEditingMode);
    add(QStringLiteral("Ctrl+N"), [this] { newNoteIn(QString()); });
    add(QStringLiteral("Ctrl+,"), &MainWindow::openSettings);
    add(QStringLiteral("Ctrl+Q"), &MainWindow::close);
    add(QStringLiteral("Alt+Left"), &MainWindow::goBack);
    add(QStringLiteral("Alt+Right"), &MainWindow::goForward);
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
    if (watched == m_editor && event->type() == QEvent::Resize)
        updateInlineTitleMargins();
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::updateInlineTitleMargins()
{
    if (!m_editor || !m_inlineTitle)
        return;
    const int column = m_theme.value(QStringLiteral("--file-line-width")).toInt();
    const int side = qMax(24, (m_editor->width() - column) / 2);
    m_inlineTitle->setContentsMargins(side, 32, side, 0);
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

void MainWindow::openQuickSwitcher()
{
    if (!m_vault.isValid())
        return;
    m_switcher->setCandidates(m_index.allPaths());
    m_switcher->popUp();
}

void MainWindow::toggleEditingMode()
{
    const bool source = m_editor->mode() == MarkdownEditor::Mode::Source;
    m_editor->setMode(source ? MarkdownEditor::Mode::LivePreview
                             : MarkdownEditor::Mode::Source);
    statusBar()->showMessage(source ? tr("Live preview") : tr("Source mode"), 2000);
}

void MainWindow::openSettings()
{
    m_mainStack->setCurrentIndex(1);
}

void MainWindow::closeSettings()
{
    m_settings.save();
    m_mainStack->setCurrentIndex(0);
    if (m_editor->isEnabled())
        m_editor->setFocus();
}

void MainWindow::leaveSettings()
{
    if (m_mainStack->currentIndex() != 0)
        closeSettings();
}

void MainWindow::openRelative(const QString &relativePath)
{
    if (!relativePath.isEmpty() && m_vault.isValid())
        openNote(m_vault.absolute(relativePath));
}

void MainWindow::onIndexChanged()
{
    m_editor->setLinkTargets(m_index.linkTargets());
    // Before this, a note opened while the vault was first indexed showed
    // every link as missing until it was next edited.
    m_editor->refreshLinkStatus();
    m_backlinks->refresh();
    m_tags->refresh();
    m_outgoingLinks->refresh();
    if (m_index.isOpen() && m_notePath.isEmpty())
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
    m_theme = themeFromSettings();
    applyTheme();

    const QString mode = m_settings.text(QStringLiteral("editor.defaultMode"));
    m_editor->setMode(mode == QLatin1String("source")    ? MarkdownEditor::Mode::Source
                      : mode == QLatin1String("reading") ? MarkdownEditor::Mode::Reading
                                                          : MarkdownEditor::Mode::LivePreview);
    m_editor->setReadableLineLength(m_settings.boolean(QStringLiteral("editor.readableLineLength")));
    m_editor->setContentWidth(m_settings.integer(QStringLiteral("editor.contentWidth")));
    m_editor->setLineNumbers(m_settings.boolean(QStringLiteral("editor.lineNumbers")));
    m_editor->setAutoPairs(m_settings.boolean(QStringLiteral("editor.autoPairs")));
    m_editor->setStrictLineBreaks(m_settings.boolean(QStringLiteral("editor.strictLineBreaks")));
    m_editor->setIndentGuides(m_settings.boolean(QStringLiteral("editor.showIndentGuides")));
    m_editor->setTabWidth(m_settings.integer(QStringLiteral("editor.tabWidth")));
    m_editor->setUseWikilinks(m_settings.boolean(QStringLiteral("links.useWikilinks")));
    m_editor->setGPUacceleration(m_settings.boolean(QStringLiteral("editor.gpuAcceleration")));

    m_inlineTitle->setVisible(!m_notePath.isEmpty()
                              && m_settings.boolean(QStringLiteral("appearance.showInlineTitle")));
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
    m_editor->setTheme(m_theme);
    m_tags->setTheme(m_theme);
    m_outgoingLinks->setTheme(m_theme);
    if (m_settingsPage)
        m_settingsPage->applyTheme(m_theme);

    // The inline title shares the editor's centred column.
    updateInlineTitleMargins();

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

    m_tabs->setStyleSheet(
        QStringLiteral("QTabBar::tab { background: transparent; color: %1; "
                      "padding: 8px 16px 10px 16px; margin: 0; min-width: 90px; max-width: 260px; "
                      "border: none; border-bottom: 2px solid transparent; }"
                      "QTabBar::tab:hover { color: %2; }"
                      "QTabBar::tab:selected { color: %2; border-bottom: 2px solid %3; }")
            .arg(m_theme.value(QStringLiteral("--text-muted")),
                m_theme.value(QStringLiteral("--text-normal")),
                m_theme.value(QStringLiteral("--interactive-accent"))));

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
    m_notePath.clear();
    m_note = Note{};
    m_editor->setNoteText(QString());
    m_editor->setEnabled(false);
    m_inlineTitle->setVisible(false);
    while (m_tabs->count() > 0) {
        // setTabButton() reparents the close button to the tab bar but
        // removeTab() does not delete it, so it would otherwise linger as an
        // invisible orphan every time a note is closed.
        if (QWidget *button = m_tabs->tabButton(0, QTabBar::RightSide))
            button->deleteLater();
        m_tabs->removeTab(0);
    }
    m_backlinks->setNote(QString());
    m_outgoingLinks->setNote(QString());
}

void MainWindow::openVault(const QString &path)
{
    if (!maybeSave())
        return;

    Vault vault(path);
    if (!vault.isValid()) {
        QMessageBox::warning(this, tr("Open vault"), tr("%1 is not a directory.").arg(path));
        return;
    }

    m_openMode = OpenMode::Vault;
    m_vault = vault;
    clearOpenNote();
    resetHistory();

    m_sidebar->setVisible(true);
    m_rightSidebar->setVisible(true);
    m_tabStrip->setVisible(true);

    m_tree->setRootIndex(m_files->setRootPath(m_vault.root()));
    m_indexer->setVault(m_vault);

    reloadSettings(m_vault.root());
    m_settings.noteOpened(m_vault.root(), QString());
    applySettings();

    statusBar()->showMessage(m_vault.hasObsidianConfig()
                                 ? tr("Vault opened - Obsidian settings found")
                                 : tr("Vault opened"),
                             4000);
    updateTitle();
    updateWordCount();
}

void MainWindow::closeVault()
{
    if (!maybeSave())
        return;

    m_openMode = OpenMode::Empty;
    m_vault = Vault();
    m_index.close();
    clearOpenNote();
    resetHistory();

    m_tree->setRootIndex(QModelIndex());
    m_files->setRootPath(QString());

    reloadSettings(QString());
    applySettings();

    updateTitle();
    updateWordCount();
    statusBar()->showMessage(tr("Vault closed"), 3000);
}

void MainWindow::openSingleFile(const QString &path)
{
    if (!maybeSave())
        return;

    m_openMode = OpenMode::SingleFile;
    m_vault = Vault();
    m_index.close();
    clearOpenNote();
    resetHistory();

    m_sidebar->setVisible(false);
    m_rightSidebar->setVisible(false);
    m_tabStrip->setVisible(false);

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
    watchThemeFiles(); // after the first paint: openWhenShown queued this call
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

bool MainWindow::openNote(const QString &rawPath, NavSource source)
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
    // keys off m_notePath comes up empty. Normalise once, here, so nothing
    // downstream has to guess.
    const QString path = QFileInfo(rawPath).absoluteFilePath();
    if (path == m_notePath)
        return true;
    if (!Vault::isNote(path)) {
        if (source != NavSource::History) {
            statusBar()->showMessage(tr("%1 is not a note - editors for other file types come later")
                                         .arg(QFileInfo(path).fileName()),
                                     4000);
        }
        return false;
    }
    if (!maybeSave())
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

    if (source != NavSource::History) {
        // A fresh navigation discards whatever "forward" existed past here,
        // the way a browser does when you follow a new link after going back.
        while (m_history.size() > m_historyIndex + 1)
            m_history.removeLast();
        m_history.append(path);
        m_historyIndex = m_history.size() - 1;
    }

    m_note = note;
    m_notePath = path;
    m_editor->setEnabled(true);
    m_editor->setNoteText(m_note.text);
    if (const auto position = m_settings.notePosition(path)) {
        m_editor->setCursorOffset(qMin(position->cursor, qsizetype(m_note.text.size())));
        m_editor->verticalScrollBar()->setValue(position->scroll);
    }
    m_editor->setFocus();

    const QString title = QFileInfo(path).completeBaseName();
    m_inlineTitle->setText(title);
    m_inlineTitle->setVisible(m_settings.boolean(QStringLiteral("appearance.showInlineTitle")));
    // ponytail: one tab until Phase 4 brings the workspace tree.
    if (m_tabs->count() == 0) {
        m_tabs->addTab(title);
        // The style's own close glyph pulls from the desktop icon theme and
        // can come out looking nothing like the rest of the app (a red
        // "document-close" square on this box); a Lucide icon through the
        // same tinting path as every other button keeps it consistent.
        QToolButton *closeButton = iconButton(QStringLiteral("tabCloseButton"),
                                              QStringLiteral("x"), tr("Close"), 12);
        // applyTheme() only re-icons buttons that already exist when it
        // runs; this one is born between theme applications, so it needs
        // its first icon set directly or it sits there with none at all.
        closeButton->setIcon(
            Icons::icon(QStringLiteral("x"), m_theme.color(QStringLiteral("--text-muted")), 12));
        connect(closeButton, &QToolButton::clicked, this, [this] {
            if (!maybeSave())
                return;
            clearOpenNote();
            updateTitle();
            updateWordCount();
        });
        m_tabs->setTabButton(0, QTabBar::RightSide, closeButton);
    } else {
        m_tabs->setTabText(0, title);
    }

    m_backlinks->setNote(path);
    m_outgoingLinks->setNote(path);
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
    return true;
}

void MainWindow::rememberCurrentPosition()
{
    if (m_notePath.isEmpty())
        return;
    m_settings.setNotePosition(m_notePath, m_editor->cursorOffset(),
                               m_editor->verticalScrollBar()->value());
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
            if (path == m_notePath)
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
        m_backButton->setEnabled(m_historyIndex > 0);
    if (m_forwardButton)
        m_forwardButton->setEnabled(m_historyIndex >= 0 && m_historyIndex < m_history.size() - 1);
}

void MainWindow::resetHistory()
{
    m_history.clear();
    m_historyIndex = -1;
    updateHistoryButtons();
}

void MainWindow::goBack()
{
    for (int i = m_historyIndex - 1; i >= 0; --i) {
        if (openNote(m_history.at(i), NavSource::History)) {
            m_historyIndex = i;
            updateHistoryButtons();
            return;
        }
    }
}

void MainWindow::goForward()
{
    for (int i = m_historyIndex + 1; i < m_history.size(); ++i) {
        if (openNote(m_history.at(i), NavSource::History)) {
            m_historyIndex = i;
            updateHistoryButtons();
            return;
        }
    }
}

bool MainWindow::save()
{
    if (m_notePath.isEmpty())
        return true;

    Note note = m_note;
    note.text = m_editor->noteText();

    QString error;
    if (!Vault::write(m_notePath, note, &error)) {
        QMessageBox::warning(this, tr("Save"), tr("Could not save %1:\n%2").arg(m_notePath, error));
        return false;
    }

    m_note = note;
    m_editor->setModified(false);
    statusBar()->showMessage(tr("Saved %1").arg(m_vault.isValid() ? m_vault.relative(m_notePath)
                                                                   : QFileInfo(m_notePath).fileName()),
                             2000);
    if (m_vault.isValid())
        m_indexer->reindex(m_notePath);
    return true;
}

bool MainWindow::maybeSave()
{
    if (m_notePath.isEmpty() || !m_editor->isModified())
        return true;

    const QString shown =
        m_vault.isValid() ? m_vault.relative(m_notePath) : QFileInfo(m_notePath).fileName();
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
    const bool modified = !m_notePath.isEmpty() && m_editor->isModified();
    QString title;
    if (!m_notePath.isEmpty()) {
        title = (m_vault.isValid() ? m_vault.relative(m_notePath) : QFileInfo(m_notePath).fileName())
              + QLatin1String(" - ");
    }
    if (m_vault.isValid())
        title += m_vault.name() + QLatin1String(" - ");
    title += QStringLiteral("noteAppC");
    setWindowTitle(modified ? QLatin1Char('*') + title : title);

    if (m_tabs->count() > 0) {
        const QString tab = QFileInfo(m_notePath).completeBaseName();
        m_tabs->setTabText(0, modified ? tab + QLatin1String(" *") : tab);
    }
    m_vaultLabel->setText(m_vault.isValid() ? m_vault.name() : tr("No vault open"));

    if (m_vaultMenuButton) {
        if (m_openMode == OpenMode::SingleFile && !m_notePath.isEmpty())
            m_vaultMenuButton->setText(QFileInfo(m_notePath).fileName());
        else
            m_vaultMenuButton->setText(m_vault.isValid() ? m_vault.name() : tr("No vault"));
    }
}

void MainWindow::updateWordCount()
{
    if (m_notePath.isEmpty()) {
        m_wordCount->clear();
        return;
    }
    const int words = m_editor->wordCount();
    if (words < 0)
        m_wordCount->clear(); // a large note, still being counted
    else
        m_wordCount->setText(tr("%n word(s)", nullptr, words));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    rememberCurrentPosition();
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

    if (m_settings.boolean(QStringLiteral("links.autoUpdate")) && m_index.isOpen()) {
        QString linkError;
        if (m_index.updateLinksAfterRename(oldRelative, newRelative, m_vault, &linkError) < 0) {
            statusBar()->showMessage(tr("Renamed, but links could not be updated: %1").arg(linkError),
                                     4000);
        }
    }

    // The open note may be the renamed item itself, or nested inside a
    // renamed folder; either way its path just moved under it.
    if (!m_notePath.isEmpty()) {
        if (m_notePath == oldPath) {
            m_notePath = newPath;
            const QString title = QFileInfo(m_notePath).completeBaseName();
            m_inlineTitle->setText(title);
            if (m_tabs->count() > 0)
                m_tabs->setTabText(0, title);
        } else if (isDir && m_notePath.startsWith(oldPath + QLatin1Char('/'))) {
            m_notePath.replace(0, oldPath.size(), newPath);
        }
    }

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

    if (!m_notePath.isEmpty()
        && (m_notePath == path || m_notePath.startsWith(path + QLatin1Char('/')))) {
        clearOpenNote();
    }
    m_indexer->rebuild();
    updateTitle();
    updateWordCount();
}

} // namespace noteAppC
