#include "MainWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "BacklinksPane.h"
#include "Icons.h"
#include "NoteEdit.h"
#include "QuickSwitcher.h"

namespace noteAppC {

namespace {
constexpr auto kLastVault = "vault/last";
constexpr auto kGeometry = "window/geometry";
constexpr auto kSplitter = "window/splitter";
constexpr auto kThemeMode = "appearance/theme";
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
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    QSettings settings;
    // Dark by default. The stored value uses Obsidian's own names so that
    // reading `appearance.json` in Phase 4 needs no translation.
    m_theme = Theme::builtin(settings.value(kThemeMode, QStringLiteral("obsidian")).toString()
                                     == QLatin1String("moonstone")
                                 ? Theme::Mode::Light
                                 : Theme::Mode::Dark);

    auto *central = new QWidget(this);
    auto *columns = new QHBoxLayout(central);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(0);

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

    columns->addWidget(buildRibbon());
    columns->addWidget(splitter, 1);
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
                                     ? tr("Indexed %1 notes in %2 ms").arg(indexed).arg(milliseconds)
                                     : tr("Index up to date"),
                                 4000);
        onIndexChanged();
    });
    connect(&m_index, &Index::changed, this, &MainWindow::onIndexChanged);

    m_switcher = new QuickSwitcher(this);
    connect(m_switcher, &QuickSwitcher::accepted, this, &MainWindow::openRelative);

    buildActions();
    applyTheme();
    resize(1180, 760);

    restoreGeometry(settings.value(kGeometry).toByteArray());
    splitter->restoreState(settings.value(kSplitter).toByteArray());
    if (splitter->sizes().value(0) <= 0)
        splitter->setSizes({260, 640, 260});
    m_backlinks->setVisible(settings.value(kRightSidebar, true).toBool());

    const QString last = settings.value(kLastVault).toString();
    if (!last.isEmpty() && QFileInfo(last).isDir())
        openVault(last);
    else
        updateTitle();
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

QWidget *MainWindow::buildRibbon()
{
    auto *ribbon = new QWidget(this);
    ribbon->setObjectName(QStringLiteral("ribbon"));
    ribbon->setFixedWidth(m_theme.value(QStringLiteral("--ribbon-width")).toInt());

    auto *layout = new QVBoxLayout(ribbon);
    layout->setContentsMargins(4, 8, 4, 8);
    layout->setSpacing(2);

    QToolButton *openVault =
        iconButton(QStringLiteral("ribbonButton"), QStringLiteral("folder-open"),
                   tr("Open vault (Ctrl+Shift+O)"), 18);
    connect(openVault, &QToolButton::clicked, this, &MainWindow::chooseVault);

    QToolButton *sidebar = iconButton(QStringLiteral("ribbonButton"),
                                      QStringLiteral("panel-left"),
                                      tr("Toggle sidebar (Ctrl+Alt+S)"), 18);
    connect(sidebar, &QToolButton::clicked, this, &MainWindow::toggleSidebar);

    m_themeButton = iconButton(QStringLiteral("ribbonButton"), QStringLiteral("sun"),
                               tr("Switch between dark and light"), 18);
    connect(m_themeButton, &QToolButton::clicked, this, &MainWindow::toggleThemeMode);

    QToolButton *settings = iconButton(QStringLiteral("ribbonButton"),
                                       QStringLiteral("settings"), tr("Settings (Phase 4)"), 18);
    settings->setEnabled(false);

    layout->addWidget(openVault);
    layout->addWidget(sidebar);
    layout->addWidget(m_themeButton);
    layout->addStretch(1);
    layout->addWidget(settings);
    return ribbon;
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

    m_vaultName = new QLabel(tr("No vault"), header);
    m_vaultName->setObjectName(QStringLiteral("vaultName"));

    QToolButton *newNote = iconButton(QStringLiteral("headerButton"),
                                      QStringLiteral("file-plus"), tr("New note (Phase 1)"), 16);
    newNote->setEnabled(false);
    QToolButton *search = iconButton(QStringLiteral("headerButton"), QStringLiteral("search"),
                                     tr("Search (Phase 5)"), 16);
    search->setEnabled(false);

    headerLayout->addWidget(m_vaultName, 1);
    headerLayout->addWidget(newNote);
    headerLayout->addWidget(search);

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
    for (int column = 1; column < m_files->columnCount(); ++column)
        m_tree->hideColumn(column);
    connect(m_tree, &QTreeView::activated, this, &MainWindow::onTreeActivated);
    connect(m_tree, &QTreeView::clicked, this, &MainWindow::onTreeActivated);
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
    auto *main = new QWidget(this);
    main->setObjectName(QStringLiteral("mainArea"));

    auto *strip = new QWidget(main);
    strip->setObjectName(QStringLiteral("tabStrip"));
    auto *stripLayout = new QHBoxLayout(strip);
    stripLayout->setContentsMargins(6, 4, 6, 0);
    stripLayout->setSpacing(0);

    m_tabs = new QTabBar(strip);
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    m_tabs->setMovable(true);
    m_tabs->setElideMode(Qt::ElideRight);
    stripLayout->addWidget(m_tabs);
    stripLayout->addStretch(1);

    auto *editorArea = new QWidget(main);
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

    m_editor = new NoteEdit(editorArea);
    m_editor->setEnabled(false);
    connect(m_editor->document(), &QTextDocument::modificationChanged,
            this, &MainWindow::onModificationChanged);
    connect(m_editor, &NoteEdit::textChanged, this, &MainWindow::onTextChanged);
    // Keep the inline title in the editor's text column.
    connect(m_editor, &NoteEdit::sideMarginChanged, this, [this](int side) {
        m_inlineTitle->setContentsMargins(side, 32, side, 0);
    });
    m_editor->setTopPadding(10);

    editorLayout->addWidget(m_inlineTitle);
    editorLayout->addWidget(m_editor, 1);

    auto *layout = new QVBoxLayout(main);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(strip);
    layout->addWidget(editorArea, 1);
    return main;
}

QWidget *MainWindow::buildRightSidebar()
{
    m_backlinks = new BacklinksPane(this);
    m_backlinks->setMinimumWidth(200);
    m_backlinks->setIndex(&m_index);
    connect(m_backlinks, &BacklinksPane::noteActivated, this, &MainWindow::openRelative);
    return m_backlinks;
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
    add(QStringLiteral("Ctrl+Q"), &MainWindow::close);
}

void MainWindow::toggleSidebar()
{
    m_sidebar->setVisible(!m_sidebar->isVisible());
}

void MainWindow::toggleRightSidebar()
{
    m_backlinks->setVisible(!m_backlinks->isVisible());
    QSettings().setValue(kRightSidebar, m_backlinks->isVisible());
}

void MainWindow::openQuickSwitcher()
{
    if (!m_vault.isValid())
        return;
    m_switcher->setCandidates(m_index.allPaths());
    m_switcher->popUp();
}

void MainWindow::openRelative(const QString &relativePath)
{
    if (!relativePath.isEmpty() && m_vault.isValid())
        openNote(m_vault.absolute(relativePath));
}

void MainWindow::onIndexChanged()
{
    m_editor->setLinkTargets(m_index.linkTargets());
    m_backlinks->refresh();
    if (m_index.isOpen() && m_notePath.isEmpty())
        m_vaultLabel->setText(tr("%1  -  %2 notes").arg(m_vault.name()).arg(m_index.noteCount()));
}

void MainWindow::toggleThemeMode()
{
    setThemeMode(m_theme.isDark() ? Theme::Mode::Light : Theme::Mode::Dark);
}

void MainWindow::setThemeMode(Theme::Mode mode)
{
    if (m_theme.mode() == mode)
        return;
    m_theme = Theme::builtin(mode);
    applyTheme();
    QSettings().setValue(kThemeMode, m_theme.isDark() ? QStringLiteral("obsidian")
                                                      : QStringLiteral("moonstone"));
}

void MainWindow::applyTheme()
{
    Icons::clearCache();

    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        app->setPalette(m_theme.palette());
        app->setStyleSheet(m_theme.styleSheet());
    } else {
        setPalette(m_theme.palette());
        setStyleSheet(m_theme.styleSheet());
    }

    const QColor iconColor = m_theme.color(QStringLiteral("--text-muted"));
    if (m_themeButton)
        m_themeButton->setProperty("iconName", m_theme.isDark() ? QStringLiteral("sun")
                                                                : QStringLiteral("moon"));
    for (QToolButton *button : findChildren<QToolButton *>()) {
        const QString name = button->property("iconName").toString();
        if (!name.isEmpty())
            button->setIcon(Icons::icon(name, iconColor, button->iconSize().width()));
    }

    setFont(m_theme.uiFont());
    m_tree->setFont(m_theme.uiFont());
    m_editor->setFont(m_theme.textFont());
    m_editor->setContentWidth(m_theme.value(QStringLiteral("--file-line-width")).toInt());
    m_inlineTitle->setContentsMargins(m_editor->sideMargin(), 32, m_editor->sideMargin(), 0);
}

void MainWindow::chooseVault()
{
    const QString start = m_vault.isValid() ? m_vault.root() : QDir::homePath();
    const QString path = QFileDialog::getExistingDirectory(this, tr("Open vault"), start);
    if (!path.isEmpty())
        openVault(path);
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

    m_vault = vault;
    m_notePath.clear();
    m_note = Note{};
    m_editor->clear();
    m_editor->setEnabled(false);
    m_inlineTitle->setVisible(false);
    while (m_tabs->count() > 0)
        m_tabs->removeTab(0);

    m_tree->setRootIndex(m_files->setRootPath(m_vault.root()));
    m_vaultName->setText(m_vault.name());
    m_backlinks->setNote(QString());
    m_indexer->setVault(m_vault);
    QSettings().setValue(kLastVault, m_vault.root());

    statusBar()->showMessage(m_vault.hasObsidianConfig()
                                 ? tr("Vault opened - Obsidian settings found")
                                 : tr("Vault opened"),
                             4000);
    updateTitle();
    updateWordCount();
}

void MainWindow::onTreeActivated(const QModelIndex &index)
{
    if (m_files->isDir(index))
        return;
    openNote(m_files->filePath(index));
}

void MainWindow::openNote(const QString &path)
{
    if (path == m_notePath)
        return;
    if (!Vault::isNote(path)) {
        statusBar()->showMessage(tr("%1 is not a note - editors for other file types come later")
                                     .arg(QFileInfo(path).fileName()),
                                 4000);
        return;
    }
    if (!maybeSave())
        return;

    Note note;
    QString error;
    if (!Vault::read(path, note, &error)) {
        QMessageBox::warning(this, tr("Open note"), tr("Could not read %1:\n%2").arg(path, error));
        return;
    }

    m_note = note;
    m_notePath = path;
    m_editor->setEnabled(true);
    m_editor->setNoteText(m_note.text);
    m_editor->setFocus();

    const QString title = QFileInfo(path).completeBaseName();
    m_inlineTitle->setText(title);
    m_inlineTitle->setVisible(true);
    // ponytail: one tab until Phase 4 brings the workspace tree.
    if (m_tabs->count() == 0)
        m_tabs->addTab(title);
    else
        m_tabs->setTabText(0, title);

    m_backlinks->setNote(path);
    updateTitle();
    updateWordCount();
}

bool MainWindow::save()
{
    if (m_notePath.isEmpty())
        return true;

    Note note = m_note;
    note.text = m_editor->toPlainText();

    QString error;
    if (!Vault::write(m_notePath, note, &error)) {
        QMessageBox::warning(this, tr("Save"), tr("Could not save %1:\n%2").arg(m_notePath, error));
        return false;
    }

    m_note = note;
    m_editor->document()->setModified(false);
    statusBar()->showMessage(tr("Saved %1").arg(m_vault.relative(m_notePath)), 2000);
    m_indexer->reindex(m_notePath);
    return true;
}

bool MainWindow::maybeSave()
{
    if (m_notePath.isEmpty() || !m_editor->document()->isModified())
        return true;

    const auto answer = QMessageBox::question(
        this, tr("Unsaved changes"),
        tr("Save changes to %1?").arg(m_vault.relative(m_notePath)),
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

void MainWindow::onTextChanged()
{
    updateWordCount();
}

void MainWindow::updateTitle()
{
    const bool modified = !m_notePath.isEmpty() && m_editor->document()->isModified();
    QString title;
    if (!m_notePath.isEmpty())
        title = m_vault.relative(m_notePath) + QLatin1String(" - ");
    if (m_vault.isValid())
        title += m_vault.name() + QLatin1String(" - ");
    title += QStringLiteral("noteAppC");
    setWindowTitle(modified ? QLatin1Char('*') + title : title);

    if (m_tabs->count() > 0) {
        const QString tab = QFileInfo(m_notePath).completeBaseName();
        m_tabs->setTabText(0, modified ? tab + QLatin1String(" *") : tab);
    }
    m_vaultLabel->setText(m_vault.isValid() ? m_vault.name() : tr("No vault open"));
}

void MainWindow::updateWordCount()
{
    if (m_notePath.isEmpty()) {
        m_wordCount->clear();
        return;
    }
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    const qsizetype words = m_editor->toPlainText().split(whitespace, Qt::SkipEmptyParts).size();
    m_wordCount->setText(tr("%n word(s)", nullptr, int(words)));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    QSettings settings;
    settings.setValue(kGeometry, saveGeometry());
    if (auto *splitter = findChild<QSplitter *>(QStringLiteral("mainSplitter")))
        settings.setValue(kSplitter, splitter->saveState());
    event->accept();
}

} // namespace noteAppC
