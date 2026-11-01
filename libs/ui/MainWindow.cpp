#include "MainWindow.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QScopeGuard>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDrag>
#include <QDropEvent>
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
#include <QScreen>
#include <QWindow>
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
#include <QActionGroup>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStackedLayout>
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
#include "ConfigFile.h"
#include "Hotkeys.h"
#include "FileTree.h"
#include "Icons.h"
#include "MarkdownEditor.h"
#include "SingleInstance.h"
#include "Markmap.h"
#include "MarkmapBlock.h"
#include "MermaidBlock.h"
#include "BookmarksPane.h"
#include "DailyNotes.h"
#include "DimMenu.h"
#include "AudioRecorder.h"
#ifdef NOTEAPPC_HAVE_NOTEBOOK
#include "DirectRunner.h"
#include "Ipynb.h"
#include "KernelSpec.h"
#include "NotebookController.h"
#endif
#include "Frontmatter.h"
#include "InkTextBoxes.h"
#include "MomentFormat.h"
#include "OutgoingLinksPane.h"
#include "MetaNotesPane.h"
#include "OutlinePane.h"
#include "TemplatePicker.h"
#include "Templates.h"
#include "PublishDialog.h"
#include "PasswordDialog.h"
#ifdef NOTEAPPC_HAVE_ENCRYPTION
#include "EncryptionSession.h"
#endif
#include "ExportPdfDialog.h"
#ifdef NOTEAPPC_HAVE_PANDOC
#include "Pandoc.h"
#include "PandocRunner.h"
#endif
#ifdef NOTEAPPC_HAVE_NETWORK
#include "DictClient.h"
#endif
#include "PagePreview.h"
#include "PdfPage.h"
#include "RecoveryDialog.h"
#include "Snapshots.h"
#include "ObsidianSyntax.h"
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
#ifdef NOTEAPPC_HAVE_SHARE
#include "ShareDialog.h"
#include "ShareSession.h"
#endif
#ifdef NOTEAPPC_HAVE_IMAGEVIEW
#include "ImageView.h"
#endif
#include "PdfRecovery.h"
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
#include "FormatColours.h"
#include "Scheme.h"
#include "SettingsPage.h"
#include "FootnotesPane.h"
#include "InlineTitleEdit.h"
#include "PopoutWindow.h"
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
QString mediaKindOf(const QString &path, bool askPlugins = true)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QStringList audio = {QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("m4a"),
                                      QStringLiteral("ogg"), QStringLiteral("3gp"), QStringLiteral("flac"),
                                      QStringLiteral("opus"), QStringLiteral("oga")};
    if (audio.contains(suffix))
        return QStringLiteral("audio");
    static const QStringList video = {QStringLiteral("mp4"), QStringLiteral("webm"), QStringLiteral("ogv"),
                                      QStringLiteral("mov"), QStringLiteral("mkv")};
    if (video.contains(suffix))
        return QStringLiteral("video");
    // Asking Qt for its image formats dlopens every image plugin (libheif, libjxl
    // and what they pull in) on the UI thread, and the first note opened would
    // pay for it: the common suffixes are answered from a list, the notes and
    // views the app opens itself never reach the plugins, and only a suffix
    // nothing else claims is looked up there.
    static const QStringList images = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                       QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("svg"),
                                       QStringLiteral("webp"), QStringLiteral("avif")};
    if (images.contains(suffix))
        return QStringLiteral("image");
    static const QStringList notImages = {QString(), QStringLiteral("md"), QStringLiteral("mdlock"),
                                          QStringLiteral("pdf"), QStringLiteral("canvas"), QStringLiteral("base"),
                                          QStringLiteral("ipynb"), QStringLiteral("epub")};
    if (notImages.contains(suffix))
        return {};
    if (!askPlugins)
        return {};
    static const QByteArrayList plugged = QImageReader::supportedImageFormats();
    return plugged.contains(suffix.toUtf8()) ? QStringLiteral("image") : QString();
}
/// A suffix the app opens itself: a module may not claim it (the registry gives
/// such a module a problem notice and never loads it). Per kind, under the same
/// compile-time option as isViewFile, so a build without EPUB does not
/// forbid a module that suffix. A PDF is a module's (notolith-pdfview).
bool isBuiltInSuffix(const QString &suffix)
{
    if (suffix == QLatin1String("md") || suffix == QLatin1String("mdlock"))
        return true;
#ifdef NOTEAPPC_HAVE_EPUB
    if (suffix == QLatin1String("epub"))
        return true;
#endif
#ifdef NOTEAPPC_HAVE_CANVAS
    if (suffix == QLatin1String("canvas"))
        return true;
#endif
#ifdef NOTEAPPC_HAVE_BASES
    if (suffix == QLatin1String("base"))
        return true;
#endif
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    if (suffix == QLatin1String("ipynb"))
        return true;
#endif
    // The module scan runs this on a worker for every suffix a module claims. The image plugins are
    // asked only about a suffix one of them is known to read (a module claiming "ink" does not load
    // them all); a module for such a suffix is refused, as the app opens the file as an image itself.
    static const QStringList pluginImages = {
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("heic"), QStringLiteral("heif"),
        QStringLiteral("hif"), QStringLiteral("jxl"), QStringLiteral("jp2"), QStringLiteral("j2k"),
        QStringLiteral("ico"), QStringLiteral("cur"), QStringLiteral("icns"), QStringLiteral("tga"),
        QStringLiteral("ppm"), QStringLiteral("pgm"), QStringLiteral("pbm"), QStringLiteral("xbm"),
        QStringLiteral("xpm"), QStringLiteral("wbmp"), QStringLiteral("dds"), QStringLiteral("ani"),
        QStringLiteral("exr"), QStringLiteral("qoi"), QStringLiteral("pcx"), QStringLiteral("psd"),
        QStringLiteral("xcf"), QStringLiteral("hdr")};
    return !mediaKindOf(QStringLiteral("x.") + suffix, pluginImages.contains(suffix)).isEmpty();
}
constexpr auto kGeometry = "window/geometry";
/// Narrower than this there is no room for a sidebar beside a readable line:
/// a phone held upright, or a window tiled to a sliver.
constexpr int kNarrowWidth = 720;
constexpr auto kSplitter = "window/splitter";
constexpr auto kRightSidebar = "window/rightSidebar";

/// What the window remembers about itself - geometry, splitter, the publish dialog's choices - an INI file
/// in the state directory, shared by every window.
QSettings windowSettings()
{
    ensurePrivateDir(appDir(XDGroot::State));
    return QSettings(appDir(XDGroot::State, QStringLiteral("window.ini")), QSettings::IniFormat);
}

/// Obsidian's file explorer is text only; QFileSystemModel wants to draw
/// desktop icons, so hand it a provider that draws nothing.
class NoIcons : public QFileIconProvider {
public:
    QIcon icon(IconType) const override { return {}; }
    QIcon icon(const QFileInfo &) const override { return {}; }
    /// The Type column is hidden and never sorted by; the base class asks the
    /// MIME database about every file in a listed directory. Not an empty
    /// string: QFileSystemModel then lists no rows (WindowTest caught it).
    QString type(const QFileInfo &) const override { return QStringLiteral("File"); }
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

/// A tab's view that is one file it saves to: a module's view (PDF, ink, ...), a canvas, an image. The window watches
/// that file, follows it when it moves outside the app and refuses to save it once it is gone.
bool isFileView(const QWidget *view)
{
    if (!view)
        return false;
    if (qobject_cast<const ModuleView *>(view))
        return true;
#ifdef NOTEAPPC_HAVE_CANVAS
    if (qobject_cast<const CanvasView *>(view))
        return true;
#endif
#ifdef NOTEAPPC_HAVE_IMAGEVIEW
    if (qobject_cast<const ImageView *>(view))
        return true;
#endif
    return false;
}

/// The view keeps the path it saves to: tell it the file moved.
void followViewPath(QWidget *view, const QString &path)
{
    if (auto *module = qobject_cast<ModuleView *>(view))
        module->setFilePath(path);
#ifdef NOTEAPPC_HAVE_CANVAS
    else if (auto *canvas = qobject_cast<CanvasView *>(view))
        canvas->setPath(path);
#endif
#ifdef NOTEAPPC_HAVE_IMAGEVIEW
    else if (auto *image = qobject_cast<ImageView *>(view))
        image->setPath(path);
#endif
}

QString stampOf(const QFileInfo &info)
{
    return QStringLiteral("%1|%2").arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch());
}
} // namespace

MainWindow::~MainWindow()
{
    if (m_graphSave.isActive()) {
        m_graphSave.stop();
        saveGraphOptions(); // a change made in the last half second still counts
    }
    flushSaves(); // an autosave in flight reaches the disk and the index first
    // The indexer's scan, which posts to the index, stops first. Searches
    // for ```query blocks run on the pool against m_index, and the history's
    // last saves may still be on their way to disk.
    m_indexer->stop();
    // A module view keeps something for the disk (a learned pen): its own destructor runs when the child widgets go, after
    // m_settings, which it writes through. So they write now.
    for (auto *view : findChildren<ModuleView *>())
        view->flushSettings();
    for (const auto &popout : m_popouts) // top-levels of their own, destroyed after m_settings too
        for (auto *view : popout->findChildren<ModuleView *>())
            view->flushSettings();
    m_sync.reset(); // its thread stops and joins here
#ifdef NOTEAPPC_HAVE_SHARE
    qDeleteAll(m_sessions); // they point at the engine, which saves every share as it goes
    m_sessions.clear();
    m_share.reset();
#endif
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

#ifdef NOTEAPPC_HAVE_NOTEBOOK
namespace {

/// Which kernel (if any) a block with `options` runs in: the note's shared
/// one, a named kernelspec, or none. Variables that live on between blocks:
/// one kernel per note, when the block (`#| kernel: on`, or a kernelspec's
/// name) or the setting asks for it. Otherwise each block is its own short
/// process.
void chooseKernel(notebook::NotebookController::RunRequest &request,
                  const MarkdownEditor::NotebookRunOptions &options, const QString &notePath,
                  bool sharedByDefault)
{
    const QString kernel = options.kernel;
    if (kernel == QLatin1String("on") || (kernel.isEmpty() && sharedByDefault)) {
        request.useKernel = true;
    } else if (!kernel.isEmpty() && kernel != QLatin1String("off")) {
        request.kernelName = kernel;
    }
    if (request.useKernel || !request.kernelName.isEmpty())
        request.scope = options.scope.isEmpty() ? notePath : options.scope;
}

} // namespace
#endif

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_autosave.setSingleShot(true);
    m_autosave.setInterval(2000); // Obsidian's pause, near enough
    m_autosaveMax.setSingleShot(true);
    m_autosaveMax.setInterval(30000); // typing without a pause still saves
    connect(&m_autosaveMax, &QTimer::timeout, this, &MainWindow::autosaveInBackground);
    m_layoutSave.setSingleShot(true);
    m_layoutSave.setInterval(1000);
    connect(&m_layoutSave, &QTimer::timeout, this, &MainWindow::saveLayout);
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    // Encrypted notes: nothing happens until one is opened or made.
    m_encryption = new crypt::EncryptionSession(this);
    m_encryption->setPrompt([this](bool creating, bool retry) { return PasswordDialog::ask(this, creating, retry); });
    connect(m_encryption, &crypt::EncryptionSession::aboutToLock, this, &MainWindow::saveEncryptedLeaves);
    connect(m_encryption, &crypt::EncryptionSession::locked, this, [this] {
        closeEncryptedLeaves();
        statusBar()->showMessage(tr("Encrypted notes locked"), 4000);
    });
#endif
    connect(&m_autosave, &QTimer::timeout, this, &MainWindow::autosaveInBackground);
    m_noteWatcher = new QFileSystemWatcher(this);
    m_noteChanges.setSingleShot(true);
    m_noteChanges.setInterval(100); // a write in place can come in more than one go
    connect(m_noteWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
        m_changedNotes.insert(path);
        m_noteChanges.start();
    });
    // A folder changing is how a deleted-and-recreated note, or a moved one,
    // shows: the file's own watch dies with the file.
    connect(m_noteWatcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_noteChanges.start(); });
    connect(&m_noteChanges, &QTimer::timeout, this, &MainWindow::reloadChangedNotes);
    // The vault's own watcher (the Indexer's, so no second set of inotify watches): a file that is no
    // note appearing or going changes no index row, yet an open note's link to it must change look.
    // The refresh waits for the Indexer's scan of the same event (a bare-name link resolves through
    // the index it updates), and Index::changed then does it, so one save is one refresh.
    m_vaultFiles.setSingleShot(true);
    m_vaultFiles.setInterval(300); // a sync or a trash emptying is a storm of events
    connect(&m_vaultFiles, &QTimer::timeout, this, [this] {
        m_vaultPendingSince.invalidate();
        if (!m_indexer->isRunning())
            refreshOpenVaultFiles();
    });
    m_lostRetry.setSingleShot(true);
    m_lostRetry.setInterval(1000);
    connect(&m_lostRetry, &QTimer::timeout, this, [this] { followLostNotes(true); });
    m_savePool.setMaxThreadCount(1);
    m_savePool.setExpiryTimeout(-1);
    m_historyPool.setMaxThreadCount(1);
    m_historyPool.setExpiryTimeout(-1);
    m_theme = themeFromSettings();
    m_modules = new ModuleRegistry(this);
    m_modules->setEnabledCheck([this](const QString &featureID) { return featureOn(featureID.toUtf8().constData()); });
    m_moduleHost.settings = &m_settings;
    m_moduleHost.theme = &m_theme;
    m_moduleHost.vault = &m_vault;
    m_moduleHost.registerView = [this](const QStringList &suffixes, ModuleHost::ViewFactory factory) {
        for (const QString &suffix : suffixes) {
            if (!m_moduleViews.contains(suffix.toLower())) // first claim wins, as in ModuleRegistry::scan
                m_moduleViews.insert(suffix.toLower(), factory);
        }
    };
    m_moduleHost.registerCommand = [this](const QString &id, const QString &, std::function<void()> run) {
        m_moduleCommands.insert(id, std::move(run)); // the QAction exists since the scan; this is what it runs
    };
    m_moduleHost.registerSetting = [this](const ModuleHost::SettingRow &row) {
        Settings::registerDefaults({{row.key, row.defaultValue}});
        m_moduleSettingRows << row; // loadModule() adopts the stored values and refreshes the page once, after attach()
    };
    m_moduleHost.registerIndexReader = nullptr;   // seam 6
    m_moduleHost.registerTabletHandler = nullptr; // seam 2
    m_moduleHost.showTab = [this](ModuleView *view, const QString &title) { showModuleTab(view, title); };
    m_moduleHost.showStatus = [this](const QString &text, int timeoutMs) { statusBar()->showMessage(text, timeoutMs); };
    m_moduleHost.openFile = [this](const QString &absolutePath) { return openNote(absolutePath); };
    m_moduleHost.currentView = [this]() -> ModuleView * {
        return m_active ? qobject_cast<ModuleView *>(m_active->view) : nullptr;
    };
    m_moduleHost.flushSaves = [this] { flushSaves(); };
#ifdef NOTEAPPC_HAVE_SHARE
    m_moduleHost.registerShareEngine = [this](ModuleHost::ShareEngineFactory factory) { m_makeShare = std::move(factory); };
#endif
    m_moduleHost.currentFolder = [this]() -> QString {
        if (!m_vault.isValid())
            return QString();
        if (m_active && !m_active->notePath.isEmpty() && m_vault.contains(m_active->notePath))
            return QFileInfo(m_active->notePath).absolutePath();
        return m_vault.root();
    };
    // The style sheet before the widgets exist, so each is styled once as it
    // is made: set after them (applyTheme), it re-polished every one, a third
    // of a cold start. applyTheme then finds it already in place.
    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        app->setPalette(m_theme.palette());
        app->setStyleSheet(m_theme.styleSheet());
    }

    auto *central = new QWidget(this);
    auto *rows = new QVBoxLayout(central);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(0);

    auto *splitter = new QSplitter(Qt::Horizontal, central);
    connect(splitter, &QSplitter::splitterMoved, this, [this] { m_layoutSave.start(); });
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

    // Obsidian's status bar: backlinks, properties, the mode, words, characters.
    const auto statusLink = [this](const char *name) {
        auto *button = new QToolButton(this);
        button->setObjectName(QLatin1String(name));
        button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        return button;
    };
    m_backlinkStatus = statusLink("backlinkStatus");
    connect(m_backlinkStatus, &QToolButton::clicked, this, [this] {
        if (m_backlinks)
            showRightPane(m_backlinks);
    });
    m_propertyStatus = statusLink("propertyStatus");
    connect(m_propertyStatus, &QToolButton::clicked, this, [this] {
        // The properties are the first block of the note: bring them into view.
        m_active->editor->verticalScrollBar()->setValue(0);
        m_active->editor->setFocus();
    });
    m_metaStatus = statusLink("metaNoteStatus");
    m_metaStatus->hide();
    connect(m_metaStatus, &QToolButton::clicked, this, [this] {
        if (m_metaPane)
            showRightPane(m_metaPane);
    });
    m_modeButton = statusLink("modeButton");
    m_modeButton->setPopupMode(QToolButton::InstantPopup);
    m_modeButton->setIconSize(QSize(14, 14));
    {
        auto *menu = new QMenu(m_modeButton);
        menu->setObjectName(QStringLiteral("modeMenu"));
        auto *group = new QActionGroup(menu);
        const struct {
            const char *id;
            const char *icon;
            QString label;
            MarkdownEditor::Mode mode;
        } entries[] = {
            {"mode-reading", "book-open", tr("Reading"), MarkdownEditor::Mode::Reading},
            {"mode-source", "code", tr("Source mode"), MarkdownEditor::Mode::Source},
            {"mode-live", "pencil", tr("Live Preview"), MarkdownEditor::Mode::LivePreview},
        };
        for (const auto &entry : entries) {
            QAction *action = menu->addAction(entry.label);
            action->setObjectName(QLatin1String(entry.id));
            action->setCheckable(true);
            action->setData(entry.icon);
            group->addAction(action);
            const auto mode = entry.mode;
            connect(action, &QAction::triggered, this, [this, mode] { setViewMode(mode); });
        }
        m_modeButton->setMenu(menu);
    }
    m_modeButton->hide();
    m_selectionCountTimer.setSingleShot(true);
    m_selectionCountTimer.setInterval(200);
    connect(&m_selectionCountTimer, &QTimer::timeout, this, [this] {
        if (m_active)
            updateTextCount();
    });
    m_backlinkCountTimer.setSingleShot(true);
    m_backlinkCountTimer.setInterval(200);
    connect(&m_backlinkCountTimer, &QTimer::timeout, this, &MainWindow::updateBacklinkCount);
    m_wordCount = new QLabel(this);
    m_charCount = new QLabel(this);
    m_vimStatus = new QLabel(this);
    m_vimStatus->setObjectName(QStringLiteral("vimStatus"));
    m_vimStatus->hide();
    m_vaultLabel = new QLabel(this);
    m_syncStatus = new QLabel(this);
    m_syncStatus->setObjectName(QStringLiteral("syncStatus"));
    m_syncStatus->hide();
    statusBar()->addPermanentWidget(m_syncStatus);
    m_shareStatus = new QLabel(this);
    m_shareStatus->setObjectName(QStringLiteral("shareStatus"));
    m_shareStatus->hide();
    statusBar()->addPermanentWidget(m_shareStatus);
    // Never recording unseen: the status bar says so while it does.
    m_recordingStatus = new QLabel(this);
    m_recordingStatus->setObjectName(QStringLiteral("recordingStatus"));
    m_recordingStatus->hide();
    statusBar()->addPermanentWidget(m_recordingStatus);
    statusBar()->addPermanentWidget(m_vimStatus);
    statusBar()->addPermanentWidget(m_backlinkStatus);
    statusBar()->addPermanentWidget(m_propertyStatus);
    statusBar()->addPermanentWidget(m_metaStatus);
    statusBar()->addPermanentWidget(m_modeButton);
    statusBar()->addPermanentWidget(m_wordCount);
    statusBar()->addPermanentWidget(m_charCount);
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
    connect(m_indexer, &Indexer::vaultFilesChanged, this, [this] {
        m_vaultDirty = true;
        // A debounce, but a sync that raises an event every 100 ms must still show: past 2 s the running timer is left to fire.
        if (!m_vaultPendingSince.isValid())
            m_vaultPendingSince.start();
        if (!m_vaultFiles.isActive() || m_vaultPendingSince.elapsed() < 2000)
            m_vaultFiles.start();
    });
    connect(m_indexer, &Indexer::finished, this, [this] { refreshOpenVaultFiles(); }); // a scan that changed nothing says nothing itself
    connect(m_indexer, &Indexer::watchesFailed, this, [this](int folders) {
        statusBar()->showMessage(tr("%n folder(s) cannot be watched (fs.inotify.max_user_watches?): changes made "
                                    "outside Notolith there are found every 30 seconds",
                                    nullptr, folders),
                                 10'000);
    });
    connect(&m_index, &Index::changed, this, &MainWindow::onIndexChanged);

    m_switcher = new QuickSwitcher(this);
    connect(m_switcher, &QuickSwitcher::accepted, this, &MainWindow::openRelative);
    m_switcher->setCreateAllowed(true);
    connect(m_switcher, &QuickSwitcher::createRequested, this, &MainWindow::createNoteNamed);

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

    QSettings settings = windowSettings();
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
    connect(newNote, &QToolButton::clicked, this, [this] { newNoteIn(newNoteFolder()); });

#ifdef NOTEAPPC_HAVE_GRAPH
    // Obsidian's ribbon has a graph icon; Ctrl+G alone left nothing on screen
    // that opens it. Hidden with the feature (applyFeatures()).
    m_headerGraphButton = iconButton(QStringLiteral("headerGraphButton"), QStringLiteral("git-fork"),
                                     tr("Open graph view (Ctrl+G)"), 18);
    connect(m_headerGraphButton, &QToolButton::clicked, this, &MainWindow::openGraph);
#endif

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
#ifdef NOTEAPPC_HAVE_GRAPH
    layout->addWidget(m_headerGraphButton);
#endif
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
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QWidget::customContextMenuRequested, this,
            [this, header](const QPoint &pos) { showDetachMenu(m_sidebar, header, pos); });
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 10, 8, 6);
    headerLayout->setSpacing(2);

    auto *filesLabel = new QLabel(tr("FILES"), header);
    filesLabel->setObjectName(QStringLiteral("filesLabel"));

    QToolButton *newNote = iconButton(QStringLiteral("sidebarNewNoteButton"),
                                      QStringLiteral("file-plus"), tr("New note"), 14);
    connect(newNote, &QToolButton::clicked, this, [this] { newNoteIn(newNoteFolder()); });
    QToolButton *newFolder = iconButton(QStringLiteral("sidebarNewFolderButton"),
                                        QStringLiteral("folder-plus"), tr("New folder"), 14);
    connect(newFolder, &QToolButton::clicked, this, [this] { newFolderIn(QString()); });

    // Sorting. QFileSystemModel has no created-time column and does not keep
    // folders first on a modified-time sort, so those two are not offered.
    QToolButton *sortButton = iconButton(QStringLiteral("sidebarSortButton"),
                                         QStringLiteral("arrow-up-down"), tr("Change sort order"), 14);
    sortButton->setPopupMode(QToolButton::InstantPopup);
    auto *sortMenu = new QMenu(sortButton);
    sortMenu->setObjectName(QStringLiteral("sidebarSortMenu"));
    auto *sortGroup = new QActionGroup(sortMenu);
    static const struct {
        const char *key;
        const char *label;
        int column;
        Qt::SortOrder order;
    } sortOrders[] = {
        {"nameAsc", QT_TR_NOOP("File name (A to Z)"), 0, Qt::AscendingOrder},
        {"nameDesc", QT_TR_NOOP("File name (Z to A)"), 0, Qt::DescendingOrder},
        {"modifiedDesc", QT_TR_NOOP("Modified time (new to old)"), 3, Qt::DescendingOrder},
        {"modifiedAsc", QT_TR_NOOP("Modified time (old to new)"), 3, Qt::AscendingOrder},
    };
    for (const auto &entry : sortOrders) {
        QAction *action = sortMenu->addAction(tr(entry.label));
        action->setCheckable(true);
        action->setData(QLatin1String(entry.key));
        action->setProperty("column", entry.column);
        action->setProperty("order", int(entry.order));
        sortGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, entry] {
            m_tree->sortByColumn(entry.column, entry.order);
            m_settings.setValue(QStringLiteral("files.sortOrder"), QLatin1String(entry.key));
            saveSettings();
        });
    }
    sortButton->setMenu(sortMenu);

    QToolButton *collapseAll = iconButton(QStringLiteral("sidebarCollapseButton"),
                                          QStringLiteral("chevrons-down-up"), tr("Collapse all"), 14);
    connect(collapseAll, &QToolButton::clicked, this, [this] { m_tree->collapseAll(); });

    headerLayout->addWidget(filesLabel, 1);
    headerLayout->addWidget(newNote);
    headerLayout->addWidget(newFolder);
    headerLayout->addWidget(sortButton);
    headerLayout->addWidget(collapseAll);

    m_files = new QFileSystemModel(this);
    m_files->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    m_files->setReadOnly(false);
    m_files->setIconProvider(noIcons());

    auto *fileTree = new FileTree(m_sidebar);
    m_tree = fileTree;
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
    // The inline editor (F2, context menu) hands the new name here instead of
    // writing to QFileSystemModel, so links, history and plugins follow. Queued:
    // the delegate calls this from inside the view closing the editor, and a
    // synchronous rename would change rows under it.
    connect(fileTree->rowDelegate(), &FileRowDelegate::renameRequested, this,
            [this](const QPersistentModelIndex &index, const QString &name) {
                if (!index.isValid() || name.isEmpty())
                    return;
                const QString oldPath = m_files->filePath(index);
                const bool isDir = m_files->isDir(index);
                if (name == FileRowDelegate::displayName(QFileInfo(oldPath)))
                    return;
                QString error;
                if (!renameVaultPath(oldPath, isDir, name, &error))
                    statusBar()->showMessage(tr("Could not rename: %1").arg(error), 4000);
            },
            Qt::QueuedConnection);
    // A drag in the tree. Queued for the same reason as the rename above: the
    // move changes rows while the view is still finishing the drop.
    connect(fileTree, &FileTree::moveRequested, this,
            [this](const QString &path, const QString &folder) { moveToFolder(path, folder); },
            Qt::QueuedConnection);
#ifdef NOTEAPPC_HAVE_PANDOC
    // A .docx, .odt, .epub, .html, .rtf or .tex dragged in from a file manager: Import with pandoc.
    connect(fileTree, &FileTree::filesDropped, this, [this](const QStringList &files, const QString &folder) {
        if (!m_vault.isValid() || !featureOn("pandoc") || !m_vault.contains(folder))
            return;
        for (const QString &file : files) {
            if (!pandoc::readerForSuffix(QFileInfo(file).suffix()).isEmpty())
                importWithPandoc(file, m_vault.relative(folder));
        }
    });
#endif
    fileTree->rowDelegate()->setIconHook([this](const QString &absolutePath) {
        return fileIconsOn() && m_vault.contains(absolutePath) ? m_fileIcons.iconFor(m_vault.relative(absolutePath))
                                                              : QString();
    });
    // QFileSystemModel fills a directory on a worker thread, so the root
    // index is not usable until it says so.
    connect(m_files, &QFileSystemModel::directoryLoaded, this, [this](const QString &path) {
        if (m_vault.isValid() && path == m_vault.root())
            m_tree->setRootIndex(m_files->index(m_vault.root()));
    });

    // Hidden file types: QFileSystemModel loads a folder in batches on a
    // worker thread, so a row is hidden as it arrives. Nothing to do (and
    // nothing walked) while the setting is empty.
    connect(m_files, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &parent, int first, int last) {
                if (!m_hiddenTypes.isEmpty())
                    hideTypedRows(parent, first, last);
            });

    auto *layout = new QVBoxLayout(m_sidebar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(m_tree, 1);
    return m_sidebar;
}

QString MainWindow::resolveLinkFrom(const QString &target, const QString &notePath) const
{
    if (!m_vault.isValid())
        return {};
    // An attachment may not be in the index (it only holds notes), so fall
    // back on the path the note actually wrote.
    const QString resolved = m_index.resolve(target, notePath);
    if (!resolved.isEmpty())
        return m_vault.absolute(resolved);
    // The vault's own text names these, so ".." or an absolute target (or a link)
    // must not lead out of the vault.
    for (const QString &candidate : {QFileInfo(notePath).absolutePath() + QLatin1Char('/') + target,
                                     m_vault.absolute(target)}) {
        const QString path = QDir::cleanPath(candidate);
        if (QFileInfo::exists(path) && m_vault.resolvesInside(path))
            return path;
    }
    return {};
}

QWidget *MainWindow::buildMainArea()
{
    auto *notesPage = new QWidget(this);
    notesPage->setObjectName(QStringLiteral("mainArea"));

    const auto resolver = [this](const QString &target) { return resolveLinkFrom(target, m_active->notePath); };
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

    // The workspace with the first-run hint stacked over it: shown only while no vault is open
    // (showEmptyState), so a window that is about to open one never flashes it.
    auto *notesLayout = new QStackedLayout(notesPage);
    notesLayout->setStackingMode(QStackedLayout::StackAll);
    notesLayout->setContentsMargins(0, 0, 0, 0);
    notesLayout->addWidget(m_workspace);
    m_emptyState = new QWidget(notesPage);
    m_emptyState->setObjectName(QStringLiteral("emptyState"));
    m_emptyState->setAutoFillBackground(true);
    // Stacked over the workspace, so it must not raise the area's minimum width: a narrow window folds the sidebars.
    m_emptyState->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto *emptyLayout = new QVBoxLayout(m_emptyState);
    emptyLayout->setAlignment(Qt::AlignCenter);
    auto *emptyLabel = new QLabel(tr("No vault open. Open a folder of Markdown notes to start."), m_emptyState);
    emptyLabel->setWordWrap(true);
    emptyLabel->setAlignment(Qt::AlignCenter);
    auto *emptyButton = new QPushButton(tr("Open folder..."), m_emptyState);
    connect(emptyButton, &QPushButton::clicked, this, &MainWindow::chooseVault);
    emptyLayout->addWidget(emptyLabel, 0, Qt::AlignCenter);
    emptyLayout->addWidget(emptyButton, 0, Qt::AlignCenter);
    notesLayout->addWidget(m_emptyState);
    notesLayout->setCurrentWidget(m_emptyState); // raises it; show() comes with it, so hide after
    m_emptyState->hide();

    // A window of its own, sized like a dialog: a page as wide as the main
    // area stretches every row across an ultrawide screen.
    m_settingsWindow = new QDialog(this);
    m_settingsWindow->setObjectName(QStringLiteral("settingsWindow"));
    m_settingsWindow->setWindowTitle(tr("Settings"));
    auto *settingsLayout = new QVBoxLayout(m_settingsWindow);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    m_settingsPage = new SettingsPage(m_settingsWindow);
    settingsLayout->addWidget(m_settingsPage);
    connect(m_settingsWindow, &QDialog::finished, this, [this] {
        saveSettings();
        if (isVisible())
            activateWindow(); // back to the notes: their shortcuts are this window's
        if (m_active && m_active->editor->isEnabled() && m_active->editor->isVisible())
            m_active->editor->setFocus();
    });
    connect(m_settingsPage, &SettingsPage::settingChanged, this, &MainWindow::applySettings);
    connect(m_settingsPage, &SettingsPage::hotkeysChanged, this, &MainWindow::applyHotkeys);
    connect(m_settingsPage, &SettingsPage::recoveryRequested, this, &MainWindow::openRecovery);
    connect(m_settingsPage, &SettingsPage::closeRequested, this, &MainWindow::closeSettings);
    connect(m_settingsPage, &SettingsPage::pluginsChanged, this, [this] { syncPlugins(true); });
#ifdef NOTEAPPC_HAVE_PANDOC
    connect(m_settingsPage, &SettingsPage::pandocNeeded, this, &MainWindow::showPandocMissing);
#endif

    m_mainStack = new QStackedWidget(this);
    m_mainStack->setObjectName(QStringLiteral("mainStack"));
    m_mainStack->addWidget(notesPage);
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
        Leaf *picked = group->leaves.at(index);
        if (picked->notePath.isEmpty() && !picked->view && !picked->keptPath.isEmpty())
            picked->pendingPath = std::exchange(picked->keptPath, QString()); // the user picked it: one more try
        activateLeaf(picked);
        m_active->editor->setFocus();
    });
    connect(group->tabs, &QTabBar::tabMoved, this,
            [group](int from, int to) { group->leaves.move(from, to); });
    group->tabs->installEventFilter(this); // a middle click closes a tab
    group->tabs->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(group->tabs, &QWidget::customContextMenuRequested, this, [this, group](const QPoint &pos) {
        showTabMenu(group, group->tabs->mapToGlobal(pos));
    });
    group->widget->setAcceptDrops(m_popoutsOn);

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

    // The inline title is a fixed label above the editor, so it does not
    // scroll away like Obsidian's.
    leaf->inlineTitle = new InlineTitleEdit(leaf->area);
    leaf->inlineTitle->setObjectName(QStringLiteral("inlineTitle"));
    leaf->inlineTitle->setVisible(false);
    // Enter and losing the focus rename the note; Esc and Down leave the
    // field for the text.
    connect(leaf->inlineTitle, &QLineEdit::editingFinished, this, [this, leaf] { commitInlineTitle(leaf); });

    // Its Resize and FocusIn events reach eventFilter() through the
    // application-wide filter installed in the constructor.
    MarkdownEditor *editor = new MarkdownEditor(leaf->area);
    leaf->editor = editor;
    connect(leaf->inlineTitle, &QLineEdit::returnPressed, editor, qOverload<>(&QWidget::setFocus));
    connect(leaf->inlineTitle, &InlineTitleEdit::moveDown, editor, qOverload<>(&QWidget::setFocus));
    connect(leaf->inlineTitle, &InlineTitleEdit::cancelled, this, [this, leaf, editor] {
        leaf->inlineTitle->setText(QFileInfo(leaf->notePath).completeBaseName());
        editor->setFocus();
    });
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
            armAutosave();
    });
    // The count comes with each parse. Taking it from textChanged recounted
    // the whole note on the UI thread for every keystroke.
    connect(editor, &MarkdownEditor::wordCountChanged, this, [this, leaf] {
        if (leaf == m_active)
            updateWordCount();
    });
    // Each parse also settles the character and property counts, which
    // wordCountChanged alone would miss when only those moved.
    connect(editor, &MarkdownEditor::headingsChanged, this, [this, leaf] {
        if (leaf == m_active)
            updateWordCount();
    });
    // A selection is counted after a pause, off the keystroke and drag path.
    connect(editor, &MarkdownEditor::cursorMoved, this, [this, leaf] {
        if (leaf == m_active)
            m_selectionCountTimer.start();
    });
    connect(editor, &MarkdownEditor::modeChanged, this, [this, leaf] {
        if (leaf == m_active)
            updateModeButton();
    });
    // Looked up by editor, never by the Leaf pointer: dropLeaf frees the Leaf at once and the editor (and the
    // nested editors that copy this resolver) a moment later, and a posted completion may ask in between.
    editor->setLinkResolver([this, editor = QPointer<MarkdownEditor>(editor)](const QString &target) -> QString {
        for (const auto &owned : m_leaves) {
            if (owned->editor == editor)
                return resolveLinkFrom(target, owned->notePath);
        }
        return {};
    });
    editor->setVaultRootProvider([this] { return m_vault.isValid() ? m_vault.root() : QString(); });
    connect(editor, &MarkdownEditor::linkActivated, this, &MainWindow::followLink);
    connect(editor, &MarkdownEditor::noticeRaised, this, [this](const QString &message) { statusBar()->showMessage(message, 6000); });
    connect(editor, &MarkdownEditor::linkHovered, this,
            [this](const QString &target, const QPoint &globalPos) {
                if (!featureOn("page-preview"))
                    return;
                m_previewTarget = target;
                m_previewPos = globalPos;
                m_previewDelay.start();
            });
    connect(editor, &MarkdownEditor::searchRequested, this, &MainWindow::openSearch);
#ifdef NOTEAPPC_HAVE_NETWORK
    connect(editor, &MarkdownEditor::lookupRequested, this, &MainWindow::lookUp);
#endif
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
        // The meta notes ride the same parse: the pane and the count follow it.
        if (m_metaPane && leaf == m_active) {
            m_metaPane->setNote(leaf->editor->noteText(), leaf->editor->metaNoteRanges(),
                                leaf->editor->headings());
            updateMetaCount();
        }
    });
    connect(editor, &MarkdownEditor::cursorMoved, this, [this, leaf](qsizetype offset) {
        if (m_outline && leaf == m_active)
            m_outline->setCursorOffset(offset);
        if (m_metaPane && leaf == m_active)
            m_metaPane->setCursorOffset(offset);
    });
    // Vim mode: its mode line sits in the status bar, and :w / :q are the
    // window's save and close for this tab.
    connect(editor, &MarkdownEditor::vimStatusChanged, this, [this, leaf] {
        if (leaf == m_active)
            updateVimStatus();
    });
    connect(editor, &MarkdownEditor::vimSaveRequested, this, [this, leaf] { saveLeaf(leaf, false); });
    connect(editor, &MarkdownEditor::vimCloseRequested, this, [this, leaf] {
        // Queued: the editor is still inside the key event that asked, and
        // closing the tab deletes it.
        QMetaObject::invokeMethod(this, [this, leaf] {
            if (std::any_of(m_leaves.cbegin(), m_leaves.cend(),
                            [leaf](const auto &open) { return open.get() == leaf; }))
                closeLeaf(leaf);
        }, Qt::QueuedConnection);
    });
    connect(editor, &MarkdownEditor::footnotesChanged, this, [this, leaf] {
        if (m_footnotes && leaf == m_active)
            m_footnotes->setFootnotes(leaf->editor->footnotes());
    });
    // ```query blocks: the search runs on the thread pool through its own
    // connection, given a copy of the cache's path rather than the Index (a
    // vault switch reopens it), and the editor brings the results back to its
    // own thread.
    Index *index = &m_index;
    editor->setQueryRunner([index](const QString &query, const MarkdownEditor::QueryDeliver &deliver) {
        (void)QtConcurrent::run([databasePath = index->databasePath(), query, deliver] {
            QueryResults results;
            for (const SearchResult &found : Index::searchIn(databasePath, query)) {
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
    // A hooked embed's own card (a `.canvas`'s file node) or its own file
    // (double-clicking empty space in one) opened the same way every other
    // pane's noteActivated already is.
    connect(editor, &MarkdownEditor::fileEmbedActivated, this, [this](const QString &relativePath) {
        openRelative(relativePath);
        m_active->editor->setFocus();
    });
    if (m_applyPluginRenderer)
        m_applyPluginRenderer(editor); // plugin fences in a new tab too
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    // A block gets a run control only for a language something can run: a
    // recipe with its tool installed, or a Jupyter kernel - the two paths
    // NotebookController::run takes. Remembered per language for the session:
    // this is asked while laying out a block, and findExecutable walks PATH and
    // the kernel list is read from disk on every call.
    editor->setNotebookRunnable([this](const QString &language) {
        auto known = m_runnableLanguages.constFind(language);
        if (known == m_runnableLanguages.constEnd()) {
            known = m_runnableLanguages.insert(
                language, notebook::DirectRunner::canRun(language)
                              || notebook::kernelSpecsByLanguage().contains(language.trimmed().toLower()));
        }
        return known.value();
    });
    // Code blocks through a Jupyter kernel. A lookup (forceRun false) only
    // reads the output cache; the controller, and any kernel, exist only
    // once something is run.
    editor->setNotebookRunner([this, editor](const QString &language, const QString &source, bool forceRun,
                                     const MarkdownEditor::NotebookRunOptions &options,
                                     const MarkdownEditor::NotebookDeliver &deliver) {
        // Its output would be cached on disk, and a kernel is another process.
        if (showsEncryptedNote(editor)) {
            MarkdownEditor::NotebookRunResult refused;
            refused.hasRun = forceRun;
            refused.kernelError = forceRun ? tr("Code does not run in an encrypted note: its output would be kept on disk")
                                           : QString();
            deliver(refused);
            return;
        }
        if (!m_notebook) {
            m_notebook = new notebook::NotebookController(this);
            connect(m_notebook, &notebook::NotebookController::kernelStateChanged, this,
                    [this](const QString &kernelLanguage, bool running) {
                        statusBar()->showMessage(running ? tr("Started the %1 kernel").arg(kernelLanguage)
                                                         : tr("Stopped the %1 kernel").arg(kernelLanguage),
                                                 3000);
                    });
            m_notebook->setRunGate([this](const notebook::NotebookController::RunRequest &request) -> QString {
                const auto *editor = qobject_cast<const MarkdownEditor *>(request.origin.data());
                const bool shared = editor && isSharedNote(editor);
#ifdef NOTEAPPC_HAVE_SHARE
                if (shared && !allowSharedRun(editor, request.language, request.source))
                    return tr("Not run: a block of a shared note runs only once you allow it");
#endif
                // Nothing a note holds runs by itself, and in a vault the person has not trusted each block asks first.
                if (!shared && !allowRun(request.language, request.source))
                    return tr("Not run: the vault is not trusted, and this block was not allowed");
                return {};
            });
        }
        // Every block runs in the sandbox, whoever wrote it; `#| sandbox: wasm` is a stricter one.
        const notebook::SandboxTier tier = options.sandboxWasm ? notebook::SandboxTier::Wasm : notebook::SandboxTier::Bubblewrap;
        notebook::NotebookController::RunRequest request;
        request.language = language;
        request.source = source;
        request.prelude = options.implicit;
        request.wrapper = options.implicitMain;
        request.duplicateOrdinal = options.duplicateOrdinal; // the key's only: the code is what was written
        request.tier = tier;
        const QString cacheKey = notebook::NotebookController::cacheKeyOf(request);
        // Forgetting a block's output: the sidecar entry goes, nothing runs.
        if (options.clear) {
            // A notebook file says so on its next save, if there was anything to forget.
            const bool had = notebook::hasNotebookCache(cacheKey);
            notebook::removeNotebookCache(cacheKey);
            if (had)
                notebookOutputsChanged(notePathOf(editor), cacheKey, notebook::NotebookRunResult());
            deliver(MarkdownEditor::NotebookRunResult());
            return;
        }
        // The size limit, the shared note's and the vault's trust questions are asked in NotebookController::run
        // (its run gate, set above), which every run passes.
        request.maxSourceBytes = m_settings.integer(QStringLiteral("notebooks.maxRunnableBytes"));
        request.origin = editor;
        request.wasmModules = m_settings.text(QStringLiteral("notebooks.wasmModules"));
        // A runnable embed's own folder is bound read-only, when it is inside the vault (and the vault is not the
        // whole home directory): a symlink or a vault opened at $HOME must not hand a cell the rest of the disk.
        if (!options.workingDir.isEmpty()) {
            if (const QString bind = m_vault.isValid() ? notebook::confinedBind(options.workingDir, m_vault.root()) : QString();
                !bind.isEmpty())
                request.extraReadOnlyBinds = {bind};
        }
        request.cache = options.cache;
        request.timeoutMs = options.timeoutSeconds * 1000;
        chooseKernel(request, options, notePathOf(editor),
                     m_settings.boolean(QStringLiteral("notebooks.sharedKernel")) || notebook::isIpynbPath(notePathOf(editor)));
        if (options.onInput) {
            // The editor's inline prompt; the controller calls this from the kernel's thread of events.
            request.onInput = [handler = options.onInput](const notebook::NotebookController::InputPrompt &prompt,
                                                         const notebook::NotebookController::InputReply &reply) {
                handler(MarkdownEditor::NotebookInputPrompt{prompt.prompt, prompt.password}, reply);
            };
        }
        m_notebook->run(request, forceRun, [this, deliver, forceRun, cacheKey, notePath = notePathOf(editor),
                                            guard = QPointer<MarkdownEditor>(editor),
                                            keep = request.cache](const notebook::NotebookRunResult &result) {
            // A run of a notebook's cell changes what the file would say: the tab is modified, and the save
            // writes that cell's outputs back (a lookup, or a run that never reached a kernel, changes nothing).
            // The path is the tab's when the result lands: the notebook may have been renamed while it ran.
            if (forceRun && keep && result.hasRun && result.kernelError.isEmpty()) {
                QMetaObject::invokeMethod(this, [this, notePath, guard, cacheKey, result] {
                    const QString now = guard ? notePathOf(guard) : QString();
                    notebookOutputsChanged(now.isEmpty() ? notePath : now, cacheKey, result);
                });
            }
            MarkdownEditor::NotebookRunResult shown;
            shown.hasRun = result.hasRun;
            shown.ok = result.ok;
            shown.wallMs = result.wallMs;
            shown.kernelError = result.kernelError;
            shown.executionCount = result.executionCount;
            for (const notebook::NotebookOutput &output : result.outputs) {
                MarkdownEditor::NotebookOutput cell;
                cell.kind = MarkdownEditor::NotebookOutput::Kind(int(output.kind));
                cell.text = output.kind == notebook::NotebookOutput::Kind::Error ? output.evalue : output.text;
                cell.imageBytes = output.imageBytes;
                cell.ename = output.ename;
                cell.traceback = output.traceback;
                cell.isStderr = output.isStderr;
                cell.alt = output.alt;
                shown.outputs.append(cell);
            }
            deliver(shown);
        });
    });
    // Ctrl+Space / Shift+Tab: the kernel a block would run in, if it is already
    // running - never started for this.
    editor->setNotebookIntrospector([this, editor](const QString &language, const QString &code, qsizetype cursor,
                                                   bool inspect, const MarkdownEditor::NotebookRunOptions &options,
                                                   const MarkdownEditor::NotebookIntrospectionDeliver &deliver) {
        if (!m_notebook || showsEncryptedNote(editor) || options.sandboxWasm)
            return false; // a wasm block never has a kernel
        notebook::NotebookController::RunRequest request;
        request.language = language;
        request.tier = notebook::SandboxTier::Bubblewrap;
        chooseKernel(request, options, notePathOf(editor),
                     m_settings.boolean(QStringLiteral("notebooks.sharedKernel")) || notebook::isIpynbPath(notePathOf(editor)));
        if (!request.useKernel && request.kernelName.isEmpty())
            return false;
        return m_notebook->introspect(request, code, cursor, inspect,
                                      [deliver](const notebook::NotebookController::Introspection &answer) {
                                          MarkdownEditor::NotebookIntrospection shown;
                                          shown.matches = answer.matches;
                                          shown.from = answer.from;
                                          shown.to = answer.to;
                                          shown.text = answer.text;
                                          deliver(shown);
                                      });
    });
    // A click on a running block's stop control.
    connect(editor, &MarkdownEditor::openFileRequested, this, [this](const QString &path) { openFileExternally(path); });
    connect(editor, &MarkdownEditor::notebookInterruptRequested, this, [this, editor](const QString &language) {
        if (m_notebook)
            m_notebook->interruptKernel(language, notePathOf(editor));
    });
#endif
    // A reference block's header: a note opens in a tab and a URL in the browser; any other file of the
    // vault is left alone (the file tree has "Open in default app").
    connect(editor, &MarkdownEditor::referenceActivated, this, [this](const QString &target) {
        if (target.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
            || target.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
            QDesktopServices::openUrl(QUrl(target));
        else if (!m_vault.isValid()
                 || !QFileInfo(target).canonicalFilePath().startsWith(QDir(m_vault.root()).canonicalPath() + QLatin1Char('/')))
            return; // the editor only offers vault files; this is the second lock
        else if (Vault::isNote(target))
            openNote(target);
        else // a click on what looks like a title never hands a vault file to xdg-open: that is an explicit command
            statusBar()->showMessage(tr("%1 is not a note: open it from the file tree").arg(QFileInfo(target).fileName()), 5000);
    });
#ifdef NOTEAPPC_HAVE_BASES
    // ```base blocks: the notes and the evaluation both on the pool (the
    // notes are 54 ms on 20 000); the vault is copied here, on the UI
    // thread, where the editor asks.
    editor->setBaseRunner([this, index](const QString &YAMLtext, const QString &viewName,
                                        const MarkdownEditor::BaseDeliver &deliver) {
        (void)QtConcurrent::run([databasePath = index->databasePath(), vault = m_vault, YAMLtext, viewName, deliver] {
            const bases::BaseEditorTable table = bases::evaluateBaseForEditor(
                YAMLtext, Index::notesForBases(databasePath), &vault, -1, viewName);
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
        // Obsidian's way of making a note: click a link to one that is not there.
        // A missing attachment is only reported ("Node.js" is still a note's
        // name), and so is any link while the vault is still being read: the
        // note may be there, in another folder.
        static const QSet<QString> attachments{
            QStringLiteral("avif"), QStringLiteral("bmp"),  QStringLiteral("gif"),  QStringLiteral("jpeg"),
            QStringLiteral("jpg"),  QStringLiteral("png"),  QStringLiteral("svg"),  QStringLiteral("webp"),
            QStringLiteral("flac"), QStringLiteral("m4a"),  QStringLiteral("mp3"),  QStringLiteral("ogg"),
            QStringLiteral("wav"),  QStringLiteral("webm"), QStringLiteral("3gp"),  QStringLiteral("mkv"),
            QStringLiteral("mov"),  QStringLiteral("mp4"),  QStringLiteral("ogv"),  QStringLiteral("pdf"),
            QStringLiteral("canvas"), QStringLiteral("base"), QStringLiteral("epub"), QStringLiteral("ipynb")};
        if (attachments.contains(QFileInfo(name).suffix().toLower()))
            statusBar()->showMessage(tr("%1 is not in this vault").arg(name), 3000);
        else if (m_indexer->isScanningWholeVault())
            statusBar()->showMessage(tr("%1 is not found yet: the vault is still being read").arg(name), 3000);
        else
            createNoteNamed(name);
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

#ifdef NOTEAPPC_HAVE_CANVAS
namespace {
QString canvasEmbedKey(MarkdownEditor *editor, const QString &path, const QString &subpath)
{
    return QString::number(quintptr(editor)) + QLatin1Char('|') + path + QLatin1Char('|') + subpath;
}
}

CanvasView &MainWindow::canvasEmbedView(MarkdownEditor *editor, const QString &path, const QString &subpath)
{
    const QString key = canvasEmbedKey(editor, path, subpath);
    auto it = m_canvasEmbedViews.find(key);
    if (it == m_canvasEmbedViews.end()) {
        auto canvas = std::make_shared<CanvasView>();
        canvas->load(path);
        it = m_canvasEmbedViews.insert(key, std::move(canvas));
        // The note that asked for this view is what bounds its lifetime -
        // "until the note is closed" - not the file it happens to embed,
        // which may still be open elsewhere. dropLeaf() erases this entry
        // when that happens; a whole-window close takes m_canvasEmbedViews
        // (an ordinary member) with it regardless, so nothing leaks past the
        // window either way. Cleanup is deliberately not hooked to editor's
        // own destroyed(): that fires while a widget tree is unwound, which
        // can be well after MainWindow's own members - this one included -
        // are already gone.
    }
    return *it.value();
}

CanvasView *MainWindow::canvasEmbedViewIfExists(MarkdownEditor *editor, const QString &path,
                                                const QString &subpath) const
{
    const auto it = m_canvasEmbedViews.constFind(canvasEmbedKey(editor, path, subpath));
    return it == m_canvasEmbedViews.constEnd() ? nullptr : it.value().get();
}

QImage MainWindow::renderCanvasEmbed(CanvasView &canvas) const
{
    const qreal ratio = devicePixelRatioF();
    QImage image(canvas.size() * ratio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    canvas.render(&image);
    return image;
}
#endif

ParseOptions MainWindow::parseOptions() const
{
    const auto text = [this](const char *key) { return m_settings.text(QLatin1String(key)); };
    ParseOptions options;
    options.brackets = true;
    options.metaNotes = m_settings.boolean(QStringLiteral("editor.metaNotes"));
    options.open = text("editor.metaNoteOpen");
    options.close = text("editor.metaNoteClose");
    // A hand-edited pair the parser refuses falls back to the brackets.
    if (!metaNoteDelimitersError(options.open, options.close).isEmpty()) {
        options.open = QStringLiteral("[");
        options.close = QStringLiteral("]");
    }
    options.excludeBetween = m_settings.boolean(QStringLiteral("editor.countExcludeBetween"));
    options.excludeOpen = text("editor.countExcludeOpen");
    options.excludeClose = text("editor.countExcludeClose");
    if (options.excludeOpen.isEmpty() || options.excludeClose.isEmpty()) {
        options.excludeOpen = options.open;
        options.excludeClose = options.close;
    }
    options.ignoreFormatting = m_settings.boolean(QStringLiteral("editor.countIgnoreFormatting"));
    options.taskStates = featureOn("task-states");
    options.codeStyler = featureOn("advanced-code-blocks");
    if (options.codeStyler) {
        for (const auto &kind : parseHighlightKinds(m_settings.text(QStringLiteral("editor.codeHighlights"))))
            options.highlightKinds.append(kind.first);
    }
    return options;
}

void MainWindow::applyEditorSettings(MarkdownEditor *editor)
{
    editor->setReadableLineLength(m_settings.boolean(QStringLiteral("editor.readableLineLength")));
    editor->setContentWidth(m_settings.integer(QStringLiteral("editor.contentWidth")));
    editor->setLineNumbers(m_settings.boolean(QStringLiteral("editor.lineNumbers")));
    editor->setAutoPairs(m_settings.boolean(QStringLiteral("editor.autoPairs")));
    editor->setVimMode(m_settings.boolean(QStringLiteral("editor.vimMode")));
    editor->setStrictLineBreaks(m_settings.boolean(QStringLiteral("editor.strictLineBreaks")));
    editor->setIndentGuides(m_settings.boolean(QStringLiteral("editor.showIndentGuides")));
    editor->setFolding(m_settings.boolean(QStringLiteral("editor.foldHeading")),
                       m_settings.boolean(QStringLiteral("editor.foldIndent")));
    editor->setOutlineNumbering(m_settings.text(QStringLiteral("editor.listNumberStyle")) == QLatin1String("hierarchical"));
    editor->setListNumberSuffix(m_settings.text(QStringLiteral("editor.listNumberSuffix")));
    editor->setBulletGlyphs(m_settings.text(QStringLiteral("editor.bulletGlyphs")));
    editor->setNestedBulletGlyphs(m_settings.boolean(QStringLiteral("editor.bulletGlyphsNested")));
    editor->setAlignTables(m_settings.boolean(QStringLiteral("editor.alignTables")));
    editor->setHoverEditButton(m_settings.boolean(QStringLiteral("editor.hoverEditButton")));
    editor->setTableHandles(m_settings.boolean(QStringLiteral("editor.tableHandles")));
    editor->setComplexCodeBlocks(m_settings.boolean(QStringLiteral("editor.complexCodeBlocks")));
    editor->setCodeHighlights(m_settings.text(QStringLiteral("editor.codeHighlights")));
#ifdef NOTEAPPC_HAVE_NETWORK
    editor->setRemoteReferences(m_settings.boolean(QStringLiteral("editor.remoteReferences")));
    Icons::setRemoteAllowed(m_settings.boolean(QStringLiteral("editor.remoteReferences")));
#endif
    editor->setTabWidth(m_settings.integer(QStringLiteral("editor.tabWidth")));
    editor->setParseOptions(parseOptions());
    editor->setUseWikilinks(m_settings.boolean(QStringLiteral("links.useWikilinks")));
    // Limit: decided when settings are applied; a window dragged to a
    // screen of another scale keeps its viewport until the next change.
    const QScreen *screen = editor->screen();
    editor->setGPUacceleration(MarkdownEditor::GPUaccelerationWanted(
        m_settings.text(QStringLiteral("editor.gpuAcceleration")),
        screen ? screen->devicePixelRatio() : qApp->devicePixelRatio()));
    editor->setContextActions(contextActions());

    // What the editor draws of the optional features: off, a block shows
    // its source and none of that feature's code runs.
    MarkdownEditor::Features features;
    features.queryBlocks = featureOn("global-search"); // embedded search is Search's, in Obsidian
    features.baseBlocks = featureOn("bases");
    features.HTMLblocks = featureOn("html-blocks");
    features.PDFembeds = featureOn("pdf-embeds");
    features.texEmbeds = featureOn("tex-embeds");
    features.CSVembeds = featureOn("csv-embeds");
    features.mediaEmbeds = featureOn("media-embeds");
    features.notebooks = featureOn("notebooks");
    editor->setFeatures(features);
    // Diagrams (Mermaid, PlantUML, TikZ, Graphviz): the renderer is the switch. Off, nothing is registered; on,
    // nothing is looked up or started until a fence is laid out. Handing it over resets an editor's code
    // blocks, so only when the setting or the tool changed since this editor last had it.
    static const struct {
        DiagramKind kind;
        const char *on;
        const char *tool;
    } diagrams[] = {{DiagramKind::Mermaid, "render.mermaid", "render.mermaidTool"},
                    {DiagramKind::PlantUML, "render.plantuml", "render.plantumlTool"},
                    {DiagramKind::TikZ, "render.tikz", "render.latexTool"},
                    {DiagramKind::Graphviz, "render.graphviz", "render.dotTool"}};
    for (const auto &diagram : diagrams) {
        const int index = int(diagram.kind);
        const QString key = m_settings.boolean(QLatin1String(diagram.on))
                                ? QLatin1Char('1') + m_settings.text(QLatin1String(diagram.tool))
                                : QString();
        if (key != m_diagramKey[index]) {
            m_diagramKey[index] = key;
            m_diagramRenderer[index] = key.isEmpty() ? MarkdownEditor::CodeBlockRenderer()
                                                     : diagramRenderer(diagram.kind, key.mid(1), [this] { return m_theme; });
        }
        const QString property = QStringLiteral("diagramKey%1").arg(index);
        if (editor->property(property.toUtf8().constData()).toString() == key)
            continue;
        editor->setProperty(property.toUtf8().constData(), key);
        if (!m_diagramRenderer[index]) {
            editor->setCodeBlockRenderer(diagramLanguages(diagram.kind), nullptr);
        } else {
            // The source goes to a tool that writes it to a temporary file and a cache: not for an encrypted note.
            editor->setCodeBlockRenderer(
                diagramLanguages(diagram.kind),
                [this, editor, renderer = m_diagramRenderer[index]](
                    const QString &language, const QString &source, int width, const QString &theme,
                    const std::function<void(MarkdownEditor::CodeBlockResult)> &deliver) {
                    if (showsEncryptedNote(editor)) {
                        deliver({true, tr("*Diagrams are not drawn for encrypted notes.*"), QImage()});
                        return;
                    }
                    renderer(language, source, width, theme, deliver);
                });
        }
    }
    // Mind maps (```markmap): the renderer is the switch here too. Nothing is made until a fence is laid out.
    {
        const QString key = featureOn("markmap") ? QStringLiteral("1") : QString();
        if (editor->property("markmapKey").toString() != key) {
            editor->setProperty("markmapKey", key);
            if (key.isEmpty()) {
                editor->setCodeBlockRenderer({QStringLiteral("markmap")}, nullptr);
            } else {
                if (!m_markmapRenderer)
                    m_markmapRenderer = markmapRenderer([this] { return m_theme; });
                // The map is drawn from the fence's text here, in the window; publish and PDF export draw their own.
                editor->setCodeBlockRenderer(
                    {QStringLiteral("markmap")},
                    [this, editor](const QString &language, const QString &source, int width, const QString &theme,
                                   const std::function<void(MarkdownEditor::CodeBlockResult)> &deliver) {
                        if (showsEncryptedNote(editor)) {
                            deliver({true, tr("*Mind maps are not drawn for encrypted notes.*"), QImage()});
                            return;
                        }
                        m_markmapRenderer(language, source, width, theme, deliver);
                    });
            }
        }
    }
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
    // - the editor cannot hold a widget in its text - but it keeps its own
    // CanvasView underneath, hidden and never shown, for as long as this
    // editor stays open: wheel/drag interaction (below) reuses its pan, zoom
    // and already-loaded document instead of reloading the file and
    // snapping back to fitToContent on every tick. Drawn a turn later, so
    // the note paints first with the box in place.
    const MarkdownEditor::FileEmbedRenderer canvasEmbed =
        [this, editor](const QString &path, const QString &subpath, int width,
                       const std::function<void(QImage)> &deliver) {
            // The tab can close before this turn: a view made then would sit
            // under a key nobody erases (dropLeaf() already ran).
            QTimer::singleShot(0, this, [this, guard = QPointer<MarkdownEditor>(editor), path, subpath, width, deliver] {
                if (!guard)
                    return;
                CanvasView &canvas = canvasEmbedView(guard, path, subpath);
                if (canvas.path().isEmpty()) {
                    deliver({});
                    return;
                }
                canvas.setTheme(m_theme);
                canvas.setFileResolver(m_resolveLink);
                // Its own shape, within reason, so the cards are not shrunk
                // past reading to fit a letterbox - resizing does not touch
                // whatever pan/zoom a previous interaction left it at
                // (CanvasView's own resizeEvent leaves them alone), and
                // load() already arranged for the very first render to
                // fitToContent on its own (m_fitPending, spent by the first
                // paintEvent a render() triggers).
                const QRectF bounds = canvas.contentBounds();
                const qreal aspect = bounds.isEmpty() ? 0.5625 : bounds.height() / bounds.width();
                canvas.resize(width, qRound(width * qBound(0.3, aspect, 1.0)));
                canvas.setAlwaysDrawContent(true);
                deliver(renderCanvasEmbed(canvas));
            });
        };
    editor->setFileEmbedRenderer({QStringLiteral("canvas")},
                                 featureOn("canvas") ? canvasEmbed : MarkdownEditor::FileEmbedRenderer());

    // Wheel (Ctrl zooms about the pointer; a plain wheel is declined so the
    // note scrolls instead - CanvasView's own wheelEvent draws that same
    // line for the real, editable canvas), drag (pans) and click (opens a
    // card, or - on empty space - the canvas file itself) over the embed's
    // own image, all against the same live CanvasView the renderer above
    // keeps for it.
    const MarkdownEditor::FileEmbedInputHandler canvasInput =
        [this, editor](const QString &path, const QString &subpath,
                       const MarkdownEditor::FileEmbedInputEvent &event) {
            MarkdownEditor::FileEmbedInputResult result;
            CanvasView *canvas = canvasEmbedViewIfExists(editor, path, subpath);
            if (!canvas)
                return result; // an interaction cannot outrun its own first render
            switch (event.kind) {
            case MarkdownEditor::FileEmbedInputKind::Wheel:
                if (!event.modifiers.testFlag(Qt::ControlModifier))
                    return result; // declined: the note scrolls instead
                canvas->zoomAbout(event.localPoint, event.delta.y());
                result.image = renderCanvasEmbed(*canvas);
                break;
            case MarkdownEditor::FileEmbedInputKind::Drag:
                canvas->panBy(QPointF(event.delta));
                result.image = renderCanvasEmbed(*canvas);
                break;
            case MarkdownEditor::FileEmbedInputKind::Click:
                if (const QString hitID = canvas->nodeAt(event.localPoint); !hitID.isEmpty()) {
                    if (const CanvasNode *node = canvas->document().node(hitID);
                        node && node->type() == CanvasNodeType::File && !node->file().isEmpty())
                        result.openPath = node->file();
                }
                break;
            case MarkdownEditor::FileEmbedInputKind::DoubleClick:
                if (canvas->nodeAt(event.localPoint).isEmpty())
                    result.openPath = m_vault.relative(path); // empty space: open the canvas itself
                break;
            }
            return result;
        };
    editor->setFileEmbedInputHandler({QStringLiteral("canvas")},
                                     featureOn("canvas") ? canvasInput : MarkdownEditor::FileEmbedInputHandler());
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
    if (m_index.isOpen()) {
        editor->setLinkTargets(m_index.linkTargets());
        editor->setLinkAliases(m_index.aliases());
    }

    // Paste and drop: the files go into the vault's attachment folder for the
    // editor's note, by Obsidian's rules; a note outside a vault takes none.
    const auto noteOf = [this, editor]() -> QString {
        for (const auto &leaf : m_leaves) {
            if (leaf->editor == editor)
                return m_vault.isValid() && m_vault.contains(leaf->notePath) ? leaf->notePath : QString();
        }
        return {};
    };
    editor->setLinkFragmentProvider([this, noteOf](const QString &target, bool blocks) {
        // `Note#H1#H2` asks for what lies under H2 in Note.
        const QStringList path = target.split(QLatin1Char('#'));
        return m_index.fragmentsOf(m_index.resolve(path.first(), noteOf()), blocks, path.mid(1));
    });
    const auto attachmentFolder = [this](const QString &note) {
        const QString folder =
            m_vault.attachmentFolderFor(m_settings.text(QStringLiteral("files.attachmentFolder")), note);
        QDir().mkpath(folder);
        return folder;
    };
    // Limit: embeds by file name, Obsidian's "shortest path"; two files
    // of one name in the vault resolve to whichever the index finds first.
    editor->setAttachmentSaver([this, noteOf, attachmentFolder](const QString &name, const QByteArray &bytes) {
        const QString note = noteOf();
        if (note.isEmpty()) {
            statusBar()->showMessage(tr("Images are pasted into notes in a vault"), 4000);
            return QString();
        }
        const QString path = Vault::uniqueAttachmentPath(attachmentFolder(note), name);
        QString writeError;
        if (!Vault::writeBytes(path, bytes, &writeError)) { // refuses a link at the path
            statusBar()->showMessage(tr("Could not save %1: %2").arg(name, writeError), 5000);
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
            return Vault::isNote(source) || Vault::isEncryptedNote(source)
                       ? QStringLiteral("[[") + info.completeBaseName() + QStringLiteral("]]")
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
    m_layoutSave.start(); // a crash keeps the tabs too
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
    retitlePopout(group);
    // The right sidebar is built after the first tab, which is activated
    // before there is one.
    if (!m_rightPanes)
        return;
    if (!leaf->pendingPath.isEmpty()) {
        const QString pending = std::exchange(leaf->pendingPath, QString());
        if (openNote(pending))
            return; // opening it filled the panes
        // A file only a module opens, and none is installed (or it will not load): the tab stays in the saved
        // workspace for the day it does, rather than being dropped from it.
        if (leaf->notePath.isEmpty() && !leaf->view && !Vault::isNote(pending) && !isViewFile(pending))
            leaf->keptPath = pending; // shows nothing meanwhile; focus never retries it
    }
    const QString &path = leaf->notePath;
    showNoteInPanes(path);
    if (!path.isEmpty()) {
        m_settings.noteOpened(m_vault.isValid() ? m_vault.root() : QString(), path);
        revealInTree(path);
    }
    updateTitle();
    updateWordCount();
    updateHistoryButtons();
    updateShareStatus();
}

QString MainWindow::tabTitle(const Leaf *leaf) const
{
    if (leaf->notePath.isEmpty() && !leaf->pendingPath.isEmpty())
        return QFileInfo(leaf->pendingPath).completeBaseName();
    if (leaf->notePath.isEmpty() && !leaf->view && !leaf->keptPath.isEmpty())
        return QFileInfo(leaf->keptPath).completeBaseName();
    if (leaf->notePath.isEmpty() && !leaf->viewTitle.isEmpty())
        return isModified(leaf) ? leaf->viewTitle + QLatin1String(" *") : leaf->viewTitle;
    if (leaf->notePath.isEmpty())
        return tr("New tab");
    QString name = QFileInfo(leaf->notePath).completeBaseName();
    if (leaf->view && m_lostNotes.value(leaf->notePath, false))
        name += tr(" (missing)"); // a view's file is gone: its save is refused
#ifdef NOTEAPPC_HAVE_SHARE
    if (sessionFor(leaf->notePath))
        name += QStringLiteral(" \u21C4"); // shared: others may be editing it
#endif
    return isModified(leaf) ? name + QLatin1String(" *") : name;
}

void MainWindow::updateShareStatus()
{
#ifdef NOTEAPPC_HAVE_SHARE
    if (!m_shareStatus)
        return; // the window is still being built
    for (const auto &group : m_groups)
        for (int i = 0; i < group->tabs->count() && i < group->leaves.size(); ++i)
            if (const QString title = tabTitle(group->leaves.at(i)); group->tabs->tabText(i) != title)
                group->tabs->setTabText(i, title); // QTabBar relays out on every call
    const ShareSession *session = m_active && m_share ? sessionFor(m_active->notePath) : nullptr;
    m_shareStatus->setVisible(session != nullptr);
    if (!session)
        return;
    m_shareStatus->setText(tr("Shared - %1 here").arg(m_share->membersHere(session->shareID())));
    const ShareEngineBase::Counters counters = m_share->counters(session->shareID());
    m_shareStatus->setToolTip(counters.dropped ? tr("%1 messages refused, the last: %2").arg(counters.dropped).arg(counters.lastReason)
                                               : tr("Everyone connected sees your edits as you type"));
#endif
}

QIcon MainWindow::tabIcon(const Leaf *leaf) const
{
    if (!fileIconsOn() || leaf->notePath.isEmpty() || !m_vault.contains(leaf->notePath))
        return {};
    return fileIcon(m_fileIcons.iconFor(m_vault.relative(leaf->notePath)),
                    m_theme.color(QStringLiteral("--text-muted")), 14);
}

bool MainWindow::fileIconsOn() const
{
    return m_settings.boolean(QStringLiteral("appearance.fileIcons"));
}

void MainWindow::setFileIcon(const QString &absolutePath, const QString &icon)
{
    if (!m_vault.isValid() || !m_vault.contains(absolutePath))
        return;
    m_fileIcons.set(m_vault.relative(absolutePath), icon);
    QString error;
    if (!m_fileIcons.save(&error))
        statusBar()->showMessage(tr("Could not save the icon: %1").arg(error), 4000);
    refreshFileIcons();
}

void MainWindow::chooseFileIcon(const QString &absolutePath)
{
    static const QSet<QString> notForFiles = {QStringLiteral("noteAppC"), QStringLiteral("branch-collapsed"),
                                              QStringLiteral("branch-expanded"), QStringLiteral("spin-up")};
    QStringList names;
    QList<QIcon> icons;
    const QColor colour = m_theme.color(QStringLiteral("--text-normal"));
    for (const QString &file : QDir(QStringLiteral(":/icons")).entryList({QStringLiteral("*.svg")})) {
        const QString name = file.chopped(4);
        if (notForFiles.contains(name))
            continue;
        names << name;
        icons << fileIcon(name, colour, 16);
    }
    auto *picker = new QuickSwitcher(this);
    picker->setObjectName(QStringLiteral("fileIconPicker"));
    picker->setPlaceholder(tr("Icon name, or type one character"));
    picker->setCandidates(names, {}, icons);
    picker->setCreateAllowed(true);
    connect(picker, &QuickSwitcher::accepted, this, [this, absolutePath, picker](const QString &name) {
        picker->deleteLater();
        setFileIcon(absolutePath, name);
    });
    connect(picker, &QuickSwitcher::createRequested, this, [this, absolutePath, picker](const QString &typed) {
        picker->deleteLater();
        const QString text = typed.trimmed();
        if (FileIcons::isGlyph(text))
            setFileIcon(absolutePath, text);
        else
            statusBar()->showMessage(tr("Not an icon name: type one character, or pick from the list"), 4000);
    });
    connect(picker, &QDialog::rejected, picker, &QObject::deleteLater);
    picker->popUp();
}

void MainWindow::refreshFileIcons()
{
    m_tree->viewport()->update();
    for (const auto &group : m_groups) {
        for (int i = 0; i < group->tabs->count() && i < group->leaves.size(); ++i)
            group->tabs->setTabIcon(i, tabIcon(group->leaves.at(i)));
    }
}

void MainWindow::syncTabs(TabGroup *group)
{
    const bool soleEmpty = m_leaves.size() == 1 && group->leaves.size() == 1
                        && group->leaves.constFirst()->notePath.isEmpty()
                        && group->leaves.constFirst()->pendingPath.isEmpty()
                        && group->leaves.constFirst()->keptPath.isEmpty();
    const int wanted = soleEmpty ? 0 : int(group->leaves.size());
    m_layoutSave.start();
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
                    Leaf *leaf = group->leaves.at(i);
                    if (leaf->pinned) { // Obsidian: the pin unpins
                        leaf->pinned = false;
                        syncTabs(group);
                    } else
                        closeLeaf(leaf);
                    return;
                }
            }
        });
        group->tabs->setTabButton(index, QTabBar::RightSide, closeButton);
    }
    for (int i = 0; i < wanted; ++i) {
        const Leaf *leaf = group->leaves.at(i);
        group->tabs->setTabText(i, tabTitle(leaf));
        group->tabs->setTabIcon(i, tabIcon(leaf));
        if (auto *button = qobject_cast<QToolButton *>(group->tabs->tabButton(i, QTabBar::RightSide));
            button && button->property("iconName").toString() != (leaf->pinned ? QLatin1String("pin") : QLatin1String("x"))) {
            const QString icon = leaf->pinned ? QStringLiteral("pin") : QStringLiteral("x");
            button->setProperty("iconName", icon);
            button->setIcon(Icons::icon(icon, m_theme.color(QStringLiteral("--text-muted")), 12));
            button->setToolTip(leaf->pinned ? tr("Unpin") : tr("Close"));
        }
    }
    const int active = group->leaves.indexOf(m_active);
    if (active >= 0 && active < wanted)
        group->tabs->setCurrentIndex(active);
    m_syncingTabs = false;
    group->strip->setVisible(m_openMode != OpenMode::SingleFile);
    retitlePopout(group);
}

void MainWindow::dropLeaf(Leaf *leaf)
{
    TabGroup *group = leaf->group;
    const int index = group->leaves.indexOf(leaf);
    group->leaves.removeAt(index);
    // Nothing of this tab may call back in once it is gone: its lambdas hold
    // the Leaf pointer.
    leaf->editor->disconnect(this);
    leaf->inlineTitle->disconnect(this);
    if (leaf->view)
        leaf->view->disconnect(this); // a module view's modificationChanged lambda holds the Leaf pointer
#ifdef NOTEAPPC_HAVE_CANVAS
    // Its own canvas embeds' view state goes with it too - "until the note
    // is closed" means this, not whenever Qt gets around to deleting the
    // now-detached editor.
    {
        const QString prefix = QString::number(quintptr(leaf->editor)) + QLatin1Char('|');
        for (auto it = m_canvasEmbedViews.begin(); it != m_canvasEmbedViews.end();) {
            it = it.key().startsWith(prefix) ? m_canvasEmbedViews.erase(it) : std::next(it);
        }
    }
#endif
    group->stack->removeWidget(leaf->area);
    leaf->area->hide();
    leaf->area->deleteLater();

    Leaf *next = nullptr;
    if (!group->leaves.isEmpty()) {
        next = group->leaves.at(qMin(index, int(group->leaves.size()) - 1));
    } else if (collapseIfEmpty(group)) {
        group = nullptr;
    }
    std::erase_if(m_leaves, [leaf](const auto &owned) { return owned.get() == leaf; });
    pruneNotebookImages(); // a notebook no tab shows keeps no pictures
#ifdef NOTEAPPC_HAVE_SHARE
    syncShareViews(); // the closed tab's editor no longer shows a shared note
#endif
    if (m_active == leaf)
        m_active = nullptr;
    if (!next) {
        // The main window's tab, not another pop-out's, when the split went.
        next = m_leaves.front().get();
        for (const auto &other : m_leaves) {
            if (other->group->widget->window() == this) {
                next = other.get();
                break;
            }
        }
    }
    activateLeaf(next);
    if (group)
        syncTabs(group);
    syncTabs(next->group);
}

void MainWindow::closeLeaf(Leaf *leaf)
{
    // As in Obsidian, a pinned tab does not close by accident: Ctrl+W, a middle
    // click, :q and "Close all other tabs" all leave it.
    if (leaf->pinned) {
        statusBar()->showMessage(tr("This tab is pinned: unpin it to close it"), 3000);
        return;
    }
    activateLeaf(leaf);
    if (!maybeLeave())
        return;
    if (!leaf->notePath.isEmpty()) {
        m_closedTabs.append({leaf->notePath, leaf->group, leaf->editor->cursorOffset(),
                             leaf->editor->verticalScrollBar()->value(), leaf->editor->topOffset()});
        if (m_closedTabs.size() > 20)
            m_closedTabs.removeFirst();
    }
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

void MainWindow::undoCloseTab()
{
    // Newest first; a note gone from disk, or encrypted while locked (that
    // asks for a password out of nowhere), is passed over.
    for (qsizetype i = m_closedTabs.size() - 1; i >= 0; --i) {
        const ClosedTab tab = m_closedTabs.at(i);
        if (!QFileInfo::exists(tab.path)) {
            m_closedTabs.removeAt(i);
            continue;
        }
#ifdef NOTEAPPC_HAVE_ENCRYPTION
        if (Vault::isEncryptedNote(tab.path) && !m_encryption->isUnlocked())
            continue;
#endif
        m_closedTabs.removeAt(i);
        // The split it was in may be gone: the focused one then.
        const bool groupLives = std::any_of(m_groups.cbegin(), m_groups.cend(),
                                            [&tab](const auto &owned) { return owned.get() == tab.group; });
        TabGroup *group = groupLives ? tab.group : m_active->group;
        // The emptied last tab takes it back rather than leaving a blank beside it.
        if (!(m_active->group == group && m_active->notePath.isEmpty() && !m_active->view)) {
            activateLeaf(newLeaf(group));
            syncTabs(group);
        }
        if (openNote(tab.path) && !m_active->view) {
            m_active->editor->setCursorOffset(qMin(tab.caret, qsizetype(m_active->editor->noteText().size())));
            m_active->editor->restoreScroll(tab.top, tab.scroll);
        }
        m_active->editor->setFocus();
        return;
    }
}

void MainWindow::newTab()
{
    activateLeaf(newLeaf(m_active->group));
    syncTabs(m_active->group);
    m_active->editor->setFocus();
}

void MainWindow::togglePin()
{
    m_active->pinned = !m_active->pinned;
    syncTabs(m_active->group);
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
    const QString path = m_active->notePath;
    TabGroup *group = splitBeside(m_active->group, orientation, true);
    activateLeaf(newLeaf(group));
    if (!path.isEmpty())
        openNote(path);
    m_active->editor->setFocus();
}

MainWindow::TabGroup *MainWindow::splitBeside(TabGroup *beside, Qt::Orientation orientation, bool after)
{
    TabGroup *group = newGroup();
    auto *parent = qobject_cast<QSplitter *>(beside->widget->parentWidget());
    const int at = parent->indexOf(beside->widget);
    if (parent->count() == 1 || parent->orientation() == orientation) {
        parent->setOrientation(orientation);
        parent->insertWidget(after ? at + 1 : at, group->widget);
    } else {
        auto *split = new QSplitter(orientation);
        split->setChildrenCollapsible(false);
        parent->replaceWidget(at, split);
        split->addWidget(after ? beside->widget : group->widget);
        split->addWidget(after ? group->widget : beside->widget);
        beside->widget->show();
    }
    // Share the space evenly between the old split and the new one.
    auto *holder = qobject_cast<QSplitter *>(group->widget->parentWidget());
    const int index = holder->indexOf(group->widget);
    const int other = holder->indexOf(beside->widget);
    QList<int> sizes = holder->sizes();
    const int half = (sizes.value(other) + sizes.value(index)) / 2;
    sizes[other] = half;
    sizes[index] = half;
    holder->setSizes(sizes);
    return group;
}

// --- pop-out windows -----------------------------------------------------

bool MainWindow::isRootSplit(const QSplitter *split) const
{
    return split == m_workspace || rootSplits().contains(const_cast<QSplitter *>(split));
}

QList<QSplitter *> MainWindow::rootSplits() const
{
    QList<QSplitter *> roots{m_workspace};
    for (const auto &popout : m_popouts) {
        if (auto *root = qobject_cast<QSplitter *>(popout->content()); root && popout->kind() == PopoutWindow::Kind::Leaves)
            roots << root;
    }
    return roots;
}

bool MainWindow::isWorkspaceWindow(const QWidget *window) const
{
    return window == this
        || std::any_of(m_popouts.cbegin(), m_popouts.cend(), [window](const auto &popout) { return popout.get() == window; });
}

bool MainWindow::collapseIfEmpty(TabGroup *group)
{
    if (!group->leaves.isEmpty())
        return false;
    // The split goes with its last tab, and a split left holding one child
    // hands it to its own parent.
    auto *parent = qobject_cast<QSplitter *>(group->widget->parentWidget());
    group->widget->hide();
    group->widget->setParent(nullptr);
    group->widget->deleteLater();
    while (parent && !isRootSplit(parent) && parent->count() == 1) {
        auto *grandparent = qobject_cast<QSplitter *>(parent->parentWidget());
        QWidget *only = parent->widget(0);
        grandparent->replaceWidget(grandparent->indexOf(parent), only);
        only->show();
        parent->deleteLater();
        parent = grandparent;
    }
    std::erase_if(m_groups, [group](const auto &owned) { return owned.get() == group; });
    if (parent && parent->count() == 0) {
        if (parent == m_workspace) {
            TabGroup *fresh = newGroup();
            m_workspace->addWidget(fresh->widget);
            newLeaf(fresh);
            syncTabs(fresh); // another window holds the tabs, so this empty one is drawn
        } else {
            deletePopout(qobject_cast<PopoutWindow *>(parent->window()));
        }
    }
    return true;
}

void MainWindow::moveLeaf(Leaf *leaf, TabGroup *to, int index)
{
    TabGroup *from = leaf->group;
    if (from == to)
        return;
    const int at = from->leaves.indexOf(leaf);
    from->leaves.removeAt(at);
    from->stack->removeWidget(leaf->area);
    to->stack->addWidget(leaf->area);
    leaf->group = to;
    to->leaves.insert(qBound(0, index, int(to->leaves.size())), leaf);
    syncTabs(to);
    if (!collapseIfEmpty(from)) {
        syncTabs(from);
        activateLeaf(from->leaves.at(qMin(at, int(from->leaves.size()) - 1))); // what `from` shows now
    }
    if (m_active == leaf)
        m_active = nullptr; // or activateLeaf() would return early, leaving `to` unaware
    activateLeaf(leaf);
}

MainWindow::TabGroup *MainWindow::newPopout(Leaf *leaf)
{
    auto *root = new QSplitter(Qt::Horizontal);
    root->setObjectName(QStringLiteral("popoutWorkspace"));
    root->setChildrenCollapsible(false);
    TabGroup *group = newGroup();
    root->addWidget(group->widget);
    auto *window = new PopoutWindow(PopoutWindow::Kind::Leaves, root);
    registerPopout(window);
    window->resize(size() * 0.6);
    if (leaf)
        moveLeaf(leaf, group, 0);
    else
        activateLeaf(newLeaf(group));
    syncTabs(group);
    window->show();
    m_active->editor->setFocus();
    return group;
}

void MainWindow::closePopout(PopoutWindow *window, bool *veto)
{
    if (window->kind() != PopoutWindow::Kind::Leaves) {
        deletePopout(window); // docks what it holds
        return;
    }
    QList<Leaf *> leaves;
    for (const auto &leaf : m_leaves) {
        if (window->isAncestorOf(leaf->group->widget))
            leaves << leaf.get();
    }
    // Everything is asked before anything closes: a cancel keeps the window whole.
    for (Leaf *leaf : std::as_const(leaves)) {
        activateLeaf(leaf);
        if (!maybeLeave()) {
            *veto = true;
            return;
        }
    }
    for (Leaf *leaf : std::as_const(leaves)) {
        activateLeaf(leaf);
        rememberCurrentPosition();
        if (!leaf->notePath.isEmpty()) {
            m_closedTabs.append({leaf->notePath, leaf->group, leaf->editor->cursorOffset(),
                                 leaf->editor->verticalScrollBar()->value(), leaf->editor->topOffset()});
            if (m_closedTabs.size() > 20)
                m_closedTabs.removeFirst();
        }
        dropLeaf(leaf); // the last one takes the window with it
    }
    m_active->editor->setFocus();
}

void MainWindow::deletePopout(PopoutWindow *window)
{
    // A sidebar or pane goes home first; only the window is deleted. There is
    // no content left to dock when the window is closing because applyFeatures()
    // deleted it.
    if (QWidget *content = window->content()) {
        if (window->kind() == PopoutWindow::Kind::Sidebar)
            dockSidebar(content);
        else if (window->kind() == PopoutWindow::Kind::Pane)
            dockPane(content);
    }
    // Tabs still in it (shutdown) leave the model first; nothing of them may call back in.
    for (auto it = m_groups.begin(); it != m_groups.end();) {
        if (!window->isAncestorOf((*it)->widget)) {
            ++it;
            continue;
        }
        for (Leaf *leaf : (*it)->leaves) {
            leaf->editor->disconnect(this);
            leaf->inlineTitle->disconnect(this);
            std::erase_if(m_leaves, [leaf](const auto &owned) { return owned.get() == leaf; });
        }
        it = m_groups.erase(it);
    }
    if (m_active && std::none_of(m_leaves.cbegin(), m_leaves.cend(), [this](const auto &owned) { return owned.get() == m_active; }))
        m_active = m_leaves.front().get();
    // Not deleted on the spot: this may run inside the window's own closeEvent.
    window->hide();
    const auto it = std::find_if(m_popouts.begin(), m_popouts.end(), [window](const auto &owned) { return owned.get() == window; });
    if (it != m_popouts.end()) {
        it->release();
        m_popouts.erase(it);
    }
    window->deleteLater();
}

void MainWindow::dockAll()
{
    TabGroup *home = nullptr;
    for (const auto &group : m_groups) {
        if (m_workspace->isAncestorOf(group->widget)) {
            home = group.get();
            break;
        }
    }
    // The last tab out takes its window with it (collapseIfEmpty).
    for (size_t i = m_popouts.size(); i-- > 0;) { // sidebars and panes dock as their window goes
        if (m_popouts[i]->kind() != PopoutWindow::Kind::Leaves)
            deletePopout(m_popouts[i].get());
    }
    QList<Leaf *> inPopouts;
    for (const auto &leaf : m_leaves) {
        if (leaf->group->widget->window() != this)
            inPopouts << leaf.get();
    }
    for (Leaf *leaf : std::as_const(inPopouts))
        moveLeaf(leaf, home, int(home->leaves.size()));
}

void MainWindow::registerPopout(PopoutWindow *window)
{
    m_popouts.emplace_back(window);
    connect(window, &PopoutWindow::closing, this, &MainWindow::closePopout);
    window->addActions(m_commands); // a hotkey works in the window it is pressed in
    window->setAcceptDrops(true); // so a tab dragged over it is still seen as inside a window
}

PopoutWindow *MainWindow::popoutHolding(QWidget *widget) const
{
    return qobject_cast<PopoutWindow *>(widget->window());
}

void MainWindow::detachSidebar(QWidget *side)
{
    if (!m_popoutsOn || popoutHolding(side) || m_openMode == OpenMode::SingleFile)
        return; // a single file has no sidebars to show
    closeDrawer(); // a drawer is the sidebar lifted out of the splitter already
    auto *window = new PopoutWindow(PopoutWindow::Kind::Sidebar, side);
    window->setWindowTitle(side == m_sidebar ? tr("Files - Notolith") : tr("Sidebar - Notolith"));
    registerPopout(window);
    window->resize(qMax(side->width(), 280), height());
    side->show(); // a narrow window had it folded
    window->show();
}

void MainWindow::dockSidebar(QWidget *side)
{
    // Positional, as built: the left sidebar is the splitter's first child, the right its last.
    if (auto *splitter = findChild<QSplitter *>(QStringLiteral("mainSplitter")))
        splitter->insertWidget(side == m_sidebar ? 0 : splitter->count(), side);
    side->setVisible(!m_narrow);
}

QAbstractButton *MainWindow::tabButtonOf(QWidget *pane) const
{
    for (QAbstractButton *tab : m_rightTabs->buttons()) {
        if (tab->property("pane").value<QObject *>() == pane)
            return tab;
    }
    return nullptr;
}

void MainWindow::detachPane(QWidget *pane)
{
    QAbstractButton *tab = tabButtonOf(pane);
    if (!m_popoutsOn || !tab || m_rightPanes->indexOf(pane) < 0)
        return;
    m_rightPanes->removeWidget(pane); // the pane itself lives on: showNoteInPanes() still feeds it
    tab->hide();
    if (QAbstractButton *next = m_rightPanes->count() ? tabButtonOf(m_rightPanes->currentWidget()) : nullptr)
        next->setChecked(true);
    auto *window = new PopoutWindow(PopoutWindow::Kind::Pane, pane);
    window->setWindowTitle(tr("%1 - Notolith").arg(tab->toolTip()));
    registerPopout(window);
    window->resize(width() / 3, height());
    pane->show(); // a pane the stack was not showing is hidden
    window->show();
}

void MainWindow::dockPane(QWidget *pane)
{
    m_rightPanes->addWidget(pane);
    if (QAbstractButton *tab = tabButtonOf(pane))
        tab->show();
}

void MainWindow::showDetachMenu(QWidget *side, QWidget *anchor, const QPoint &pos)
{
    if (!m_popoutsOn)
        return;
    QMenu menu(anchor);
    if (side == m_rightSidebar) { // over a pane's tab: that pane alone can go
        const QWidget *tab = anchor->childAt(pos);
        const QPointer<QWidget> pane = tab ? qobject_cast<QWidget *>(tab->property("pane").value<QObject *>()) : nullptr;
        if (pane)
            connect(menu.addAction(tr("Detach pane")), &QAction::triggered, this, [this, pane] { detachPane(pane); });
    }
    if (!popoutHolding(side))
        connect(menu.addAction(tr("Detach sidebar")), &QAction::triggered, this, [this, side] { detachSidebar(side); });
    if (!menu.isEmpty())
        menu.exec(anchor->mapToGlobal(pos));
}

void MainWindow::buildSidebarPopoutActions()
{
    const auto add = [this](const QString &id, const QString &name, bool left) {
        auto *action = new QAction(name, this);
        action->setObjectName(id);
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this, [this, left] { detachSidebar(left ? m_sidebar : m_rightSidebar); });
        addAction(action);
        m_commands << action;
    };
    add(QStringLiteral("app:detach-left-sidebar"), tr("Detach left sidebar"), true);
    add(QStringLiteral("app:detach-right-sidebar"), tr("Detach right sidebar"), false);
}

void MainWindow::retitlePopout(TabGroup *group)
{
    auto *popout = qobject_cast<PopoutWindow *>(group->widget->window());
    if (!popout)
        return;
    const Leaf *current = group->leaves.value(group->tabs->currentIndex(), group->leaves.value(0));
    if (current)
        popout->setWindowTitle(tabTitle(current) + QStringLiteral(" - Notolith"));
}

void MainWindow::buildPopoutActions()
{
    const auto add = [this](const QString &id, const QString &name, auto slot) {
        auto *action = new QAction(name, this);
        action->setObjectName(id);
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this, slot);
        addAction(action);
        m_commands << action;
    };
    add(QStringLiteral("workspace:open-in-new-window"), tr("Open current tab in new window"), [this] {
        const QString path = m_active->notePath;
        newPopout();
        if (!path.isEmpty())
            openNote(path);
        m_active->editor->setFocus();
    });
    add(QStringLiteral("workspace:move-to-new-window"), tr("Move current tab to new window"), [this] {
        newPopout(m_active);
    });
}

void MainWindow::showTabMenu(TabGroup *group, const QPoint &globalPos)
{
    const int index = group->tabs->tabAt(group->tabs->mapFromGlobal(globalPos));
    if (index < 0 || index >= group->leaves.size())
        return;
    activateLeaf(group->leaves.at(index));
    QMenu menu(group->tabs); // the popup belongs to the window the tab is in
    const auto item = [this, &menu](const char *id, const QString &text) {
        QAction *command = findChild<QAction *>(QLatin1String(id));
        if (command && command->isEnabled())
            connect(menu.addAction(text), &QAction::triggered, command, &QAction::trigger);
    };
    item("workspace:close", tr("Close"));
    item("workspace:close-others", tr("Close others"));
    item("workspace:toggle-pin", group->leaves.at(index)->pinned ? tr("Unpin") : tr("Pin"));
    menu.addSeparator();
    item("workspace:split-vertical", tr("Split right"));
    item("workspace:split-horizontal", tr("Split down"));
    menu.addSeparator();
    item("workspace:open-in-new-window", tr("Open in new window"));
    item("workspace:move-to-new-window", tr("Move to new window"));
    menu.exec(globalPos);
}

void MainWindow::applyPopoutFeature()
{
    m_popoutsOn = featureOn("popout-windows");
    for (const char *id : {"workspace:open-in-new-window", "workspace:move-to-new-window",
                           "app:detach-left-sidebar", "app:detach-right-sidebar"}) {
        if (QAction *action = findChild<QAction *>(QLatin1String(id)))
            action->setEnabled(m_popoutsOn);
    }
    for (const auto &group : m_groups)
        group->widget->setAcceptDrops(m_popoutsOn);
    setAcceptDrops(m_popoutsOn);
    if (!m_popoutsOn && !m_popouts.empty())
        dockAll();
}

// --- drag-to-tile --------------------------------------------------------

namespace {
constexpr const char *kLeafMime = "application/x-notolith-leaf";
constexpr const char *kPaneMime = "application/x-notolith-pane";
}

MainWindow::DropZone MainWindow::dropZoneAt(const QRect &rect, const QPoint &pos)
{
    // Each edge's distance as a share of the rect, so a corner goes to the nearer edge.
    const double dx = double(pos.x() - rect.left()) / qMax(1, rect.width());
    const double dy = double(pos.y() - rect.top()) / qMax(1, rect.height());
    const std::pair<double, DropZone> edges[] = {{dx, DropZone::Left}, {1 - dx, DropZone::Right},
                                                 {dy, DropZone::Top}, {1 - dy, DropZone::Bottom}};
    const auto nearest = std::min_element(std::begin(edges), std::end(edges),
                                          [](const auto &a, const auto &b) { return a.first < b.first; });
    return nearest->first < 0.25 ? nearest->second : DropZone::Center;
}

// The leaf travels as its address, like FileTree's drag marker: a drag never
// leaves the process, and leafFromMime() only ever compares it, never follows it.
QMimeData *MainWindow::leafMime(const Leaf *leaf) const
{
    auto *mime = new QMimeData;
    mime->setData(QLatin1String(kLeafMime), QByteArray::number(quintptr(leaf)));
    return mime;
}

MainWindow::Leaf *MainWindow::leafFromMime(const QMimeData *mime) const
{
    if (!mime || !mime->hasFormat(QLatin1String(kLeafMime)))
        return nullptr;
    const quintptr address = mime->data(QLatin1String(kLeafMime)).toULongLong();
    const auto it = std::find_if(m_leaves.cbegin(), m_leaves.cend(),
                                 [address](const auto &leaf) { return quintptr(leaf.get()) == address; });
    return it == m_leaves.cend() ? nullptr : it->get();
}

// A right-sidebar pane travels the same way, as its address.
QMimeData *MainWindow::paneMime(const QWidget *pane) const
{
    auto *mime = new QMimeData;
    mime->setData(QLatin1String(kPaneMime), QByteArray::number(quintptr(pane)));
    return mime;
}

QWidget *MainWindow::paneFromMime(const QMimeData *mime) const
{
    if (!mime || !mime->hasFormat(QLatin1String(kPaneMime)))
        return nullptr;
    const quintptr address = mime->data(QLatin1String(kPaneMime)).toULongLong();
    for (QAbstractButton *tab : m_rightTabs->buttons()) { // only the panes in the sidebar can be dragged out of it
        auto *pane = qobject_cast<QWidget *>(tab->property("pane").value<QObject *>());
        if (pane && quintptr(pane) == address && m_rightPanes->indexOf(pane) >= 0)
            return pane;
    }
    return nullptr;
}

QMimeData *MainWindow::paneDragMimeForTests(const QWidget *pane) const
{
    return paneMime(pane);
}

QMimeData *MainWindow::tabDragMimeForTests(const QTabBar *tabs, int index) const
{
    for (const auto &group : m_groups) {
        if (group->tabs == tabs && index >= 0 && index < group->leaves.size())
            return leafMime(group->leaves.at(index));
    }
    return nullptr;
}

bool MainWindow::popoutEventFilter(QObject *watched, QEvent *event)
{
    if (!m_popoutsOn)
        return false;
    // The group a drag event over `widget` belongs to. A child that takes drops
    // itself (the editor does, for files) gets the event first and Qt hands it
    // on only if that child refuses; this takes the tab drag from it either way.
    const auto groupOf = [this](const QWidget *widget) -> TabGroup * {
        for (const auto &group : m_groups) {
            if (group->widget == widget || group->widget->isAncestorOf(widget))
                return group.get();
        }
        return nullptr;
    };
    switch (event->type()) {
    case QEvent::KeyPress:
        if (m_dragRunning && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape)
            m_dragEscaped = true; // the drag is cancelled, not dropped: no window
        return false;
    case QEvent::MouseButtonPress: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton)
            return false;
        m_pressedLeaf = nullptr;
        m_pressedPane = nullptr;
        for (const auto &group : m_groups) {
            if (watched == group->tabs) {
                m_pressPos = mouse->position().toPoint();
                m_pressedLeaf = group->leaves.value(group->tabs->tabAt(m_pressPos), nullptr);
            }
        }
        for (QAbstractButton *tab : m_rightTabs->buttons()) {
            if (watched == tab) {
                m_pressPos = mouse->position().toPoint();
                m_pressedPane = qobject_cast<QWidget *>(tab->property("pane").value<QObject *>());
            }
        }
        return false;
    }
    case QEvent::MouseButtonRelease:
        m_pressedLeaf = nullptr;
        m_pressedPane = nullptr;
        return false;
    case QEvent::MouseMove: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        // A right-sidebar tab pulled off its row: the pane goes to a window unless it lands somewhere.
        if (m_pressedPane && (mouse->buttons() & Qt::LeftButton)) {
            QAbstractButton *tab = tabButtonOf(m_pressedPane);
            if (tab && watched == tab
                && (mouse->position().toPoint() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
                QWidget *pane = std::exchange(m_pressedPane, nullptr);
                startPaneDrag(tab, pane);
                return true;
            }
            return false;
        }
        if (!m_pressedLeaf || !(mouse->buttons() & Qt::LeftButton))
            return false;
        for (const auto &group : m_groups) {
            if (watched != group->tabs || !group->leaves.contains(m_pressedLeaf))
                continue;
            // Inside the strip the bar's own drag reorders the tabs; a drag
            // that leaves it is ours.
            const QPoint pos = mouse->position().toPoint();
            const QRect strip(group->tabs->mapFromGlobal(group->strip->mapToGlobal(QPoint())), group->strip->size());
            if (strip.contains(pos) || (pos - m_pressPos).manhattanLength() < QApplication::startDragDistance())
                return false;
            startTabDrag(group.get(), std::exchange(m_pressedLeaf, nullptr), pos);
            return true;
        }
        return false;
    }
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        auto *widget = qobject_cast<QWidget *>(watched);
        const bool pane = widget && !leafFromMime(drag->mimeData()) && paneFromMime(drag->mimeData());
        if (!widget || (!pane && !leafFromMime(drag->mimeData())))
            return false;
        TabGroup *group = groupOf(widget);
        // A window is a target of its own only so that it hears the drag leave:
        // over a sidebar or header the drag is inside, and a drop there cancels.
        if (!group && !(widget->isWindow() && isWorkspaceWindow(widget)))
            return false;
        m_dragInside = true;
        if (pane) { // nothing in a window takes a pane: inside is where it is cancelled
            hideDropOverlay();
            drag->setDropAction(Qt::IgnoreAction);
            drag->accept();
            return true;
        }
        const QPoint pos = group ? group->widget->mapFromGlobal(widget->mapToGlobal(drag->position().toPoint())) : QPoint();
        if (!group || group->strip->geometry().contains(pos))
            hideDropOverlay();
        else
            showDropOverlay(group, dropZoneAt(group->stack->geometry(), pos));
        drag->setDropAction(Qt::MoveAction);
        drag->accept();
        return true;
    }
    case QEvent::DragLeave:
        m_dragInside = false;
        hideDropOverlay();
        return false;
    case QEvent::Drop: {
        auto *widget = qobject_cast<QWidget *>(watched);
        auto *drop = static_cast<QDropEvent *>(event);
        if (widget && !leafFromMime(drop->mimeData()) && paneFromMime(drop->mimeData())) {
            drop->setDropAction(Qt::IgnoreAction); // let go over a window: cancelled
            drop->accept();
            return true;
        }
        TabGroup *group = widget && leafFromMime(drop->mimeData()) ? groupOf(widget) : nullptr;
        if (!group)
            return false;
        hideDropOverlay();
        return handleGroupDrop(group, drop, group->widget->mapFromGlobal(widget->mapToGlobal(drop->position().toPoint())));
    }
    default:
        return false;
    }
}

void MainWindow::startTabDrag(TabGroup *group, Leaf *leaf, const QPoint &pos)
{
    // The bar is mid-way through its own tab move, and the platform drag
    // swallows the release that would end it.
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos), QPointF(group->tabs->mapToGlobal(pos)),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(group->tabs, &release);

    auto *drag = new QDrag(this);
    drag->setMimeData(leafMime(leaf));
    const int index = group->leaves.indexOf(leaf);
    if (index >= 0)
        drag->setPixmap(group->tabs->grab(group->tabs->tabRect(index)));
    // Taken by no group and let go outside every window: a window of its own.
    if (dragEndedOutside(drag) && m_popoutsOn
        && std::any_of(m_leaves.cbegin(), m_leaves.cend(), [leaf](const auto &owned) { return owned.get() == leaf; }))
        newPopout(leaf);
}

void MainWindow::startPaneDrag(QAbstractButton *tab, QWidget *pane)
{
    // The press that began the drag is swallowed by the platform; a release
    // off the button ends it without the click.
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(-1, -1), QPointF(tab->mapToGlobal(QPoint(-1, -1))),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(tab, &release);

    auto *drag = new QDrag(this);
    drag->setMimeData(paneMime(pane));
    drag->setPixmap(tab->grab());
    const QPointer<QWidget> guard(pane);
    if (dragEndedOutside(drag) && guard && m_popoutsOn)
        detachPane(pane);
}

bool MainWindow::dropOpensWindow(bool handled, bool inside, bool escaped)
{
    return !handled && !inside && !escaped;
}

bool MainWindow::dragEndedOutside(QDrag *drag)
{
    m_dragInside = m_dragHandled = m_dragEscaped = false;
    m_dragRunning = true;
    // Qt reports a drag cancelled with Escape like one let go outside every
    // window, and its own drag (QBasicDrag) eats every key from a filter it
    // installs inside exec(), ahead of ours. From the loop exec() starts,
    // install ours again: that puts it in front.
    QTimer::singleShot(0, this, [this] {
        if (m_dragRunning)
            watchDragKeys();
    });
    drag->exec(Qt::MoveAction);
    m_dragRunning = false;
    hideDropOverlay();
    return dropOpensWindow(m_dragHandled, m_dragInside, m_dragEscaped);
}

void MainWindow::watchDragKeys()
{
    qApp->installEventFilter(this); // already installed: moved to the front of the list
}

void MainWindow::watchDragForTests(bool on)
{
    m_dragRunning = on;
    m_dragEscaped = false;
    if (on)
        watchDragKeys();
}

bool MainWindow::dragEscapedForTests() const
{
    return m_dragEscaped;
}

void MainWindow::showDropOverlay(TabGroup *group, DropZone zone)
{
    if (!m_dropOverlay) {
        m_dropOverlay = new QWidget(group->widget);
        m_dropOverlay->setObjectName(QStringLiteral("dropOverlay"));
        m_dropOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_dropOverlay->setAttribute(Qt::WA_StyledBackground);
    }
    if (m_dropOverlay->parentWidget() != group->widget)
        m_dropOverlay->setParent(group->widget);
    if (!m_dropOverlay->isVisible()) {
        QColor accent = m_theme.color(QStringLiteral("--interactive-accent"));
        accent.setAlpha(80);
        m_dropOverlay->setStyleSheet(
            QStringLiteral("#dropOverlay { background: %1; }").arg(accent.name(QColor::HexArgb)));
    }
    QRect area = group->stack->geometry();
    switch (zone) {
    case DropZone::Left: area.setWidth(area.width() / 2); break;
    case DropZone::Right: area.setLeft(area.left() + area.width() / 2); break;
    case DropZone::Top: area.setHeight(area.height() / 2); break;
    case DropZone::Bottom: area.setTop(area.top() + area.height() / 2); break;
    case DropZone::Center: break;
    }
    m_dropOverlay->setGeometry(area);
    m_dropOverlay->show();
    m_dropOverlay->raise();
}

void MainWindow::hideDropOverlay()
{
    if (m_dropOverlay)
        m_dropOverlay->hide();
}

bool MainWindow::handleGroupDrop(TabGroup *group, QDropEvent *event, const QPoint &pos)
{
    Leaf *leaf = leafFromMime(event->mimeData());
    if (!leaf)
        return false;
    if (group->strip->geometry().contains(pos)) {
        // On the bar: at the tab under the pointer, else last.
        int index = group->tabs->tabAt(group->tabs->mapFrom(group->widget, pos));
        if (index < 0)
            index = int(group->leaves.size());
        if (leaf->group == group)
            group->tabs->moveTab(group->leaves.indexOf(leaf), qMin(index, int(group->leaves.size()) - 1));
        else
            moveLeaf(leaf, group, index);
    } else if (const DropZone zone = dropZoneAt(group->stack->geometry(), pos); zone == DropZone::Center) {
        moveLeaf(leaf, group, int(group->leaves.size()));
    } else if (leaf->group != group || group->leaves.size() > 1) { // a group's only tab has nothing to split from
        const bool sideBySide = zone == DropZone::Left || zone == DropZone::Right;
        TabGroup *beside = splitBeside(group, sideBySide ? Qt::Horizontal : Qt::Vertical,
                                       zone == DropZone::Right || zone == DropZone::Bottom);
        moveLeaf(leaf, beside, 0);
    }
    m_dragHandled = true;
    event->setDropAction(Qt::MoveAction);
    event->accept();
    return true;
}

bool MainWindow::maybeSaveAll()
{
    QSet<QString> asked; // a note in two tabs is one document, asked about once
    std::vector<Leaf *> leaves; // a copy: the wait for a reloading view runs events, and the list must not change under the loop
    for (const auto &candidate : m_leaves)
        leaves.push_back(candidate.get());
    for (Leaf *leaf : leaves) {
        if (std::none_of(m_leaves.begin(), m_leaves.end(), [leaf](const auto &open) { return open.get() == leaf; }))
            continue; // closed while an earlier tab was being asked about
        const bool fileLess = leaf->notePath.isEmpty() && qobject_cast<ModuleView *>(leaf->view);
        const QString key = fileLess ? QStringLiteral("view:%1").arg(quintptr(leaf)) : leaf->notePath; // file-less tabs are not one document
        if ((leaf->notePath.isEmpty() && !fileLess) || !isModified(leaf) || asked.contains(key))
            continue;
        asked.insert(key);
        activateLeaf(leaf);
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
#ifdef NOTEAPPC_HAVE_PANDOC
    m_pandocQueue.clear(); // dropped files waiting for pandoc were meant for the vault left
#endif
    // Pop-outs' tabs first: the main window's last one must not be the one left over.
    while (m_leaves.size() > 1) {
        Leaf *last = m_leaves.back().get();
        for (const auto &leaf : m_leaves) {
            if (leaf->group->widget->window() != this)
                last = leaf.get();
        }
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
    tabRow->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabRow, &QWidget::customContextMenuRequested, this,
            [this, tabRow](const QPoint &pos) { showDetachMenu(m_rightSidebar, tabRow, pos); });
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
        const char *viewType; // Obsidian's name for the view, written to workspace.json when detached
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
         "searchTabButton", "search", tr("Search"), "search"},
        {"backlink", m_backlinks,
         [this, parent] {
             m_backlinks = new BacklinksPane(parent);
             m_backlinks->setTheme(m_theme);
             m_backlinks->setIndex(&m_index);
             connect(m_backlinks, &BacklinksPane::noteActivated, this, &MainWindow::openRelative);
             return m_backlinks.data();
         },
         "backlinksTabButton", "link", tr("Backlinks"), "backlink"},
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
         "tagsTabButton", "tags", tr("Tags"), "tag"},
        {"properties", m_properties,
         [this, parent] {
             m_properties = new PropertiesPane(parent);
             m_properties->setTheme(m_theme);
             m_properties->setIndex(&m_index);
             connect(m_properties, &PropertiesPane::propertyActivated, this,
                     [this](const QString &key) { openSearch(QStringLiteral("[%1]").arg(key)); });
             return m_properties.data();
         },
         "propertiesTabButton", "clipboard-list", tr("All properties"), "file-properties"},
        {"outgoing-link", m_outgoingLinks,
         [this, parent] {
             m_outgoingLinks = new OutgoingLinksPane(parent);
             m_outgoingLinks->setTheme(m_theme);
             m_outgoingLinks->setIndex(&m_index);
             connect(m_outgoingLinks, &OutgoingLinksPane::noteActivated, this, &MainWindow::openRelative);
             return m_outgoingLinks.data();
         },
         "outgoingLinksTabButton", "arrow-up-right", tr("Outgoing links"), "outgoing-link"},
        {"outline", m_outline,
         [this, parent] {
             m_outline = new OutlinePane(parent);
             connect(m_outline, &OutlinePane::jumpRequested, this, [this](qsizetype offset) {
                 m_active->editor->setCursorOffset(offset);
                 m_active->editor->setFocus();
             });
             return m_outline.data();
         },
         "outlineTabButton", "list", tr("Outline"), "outline"},
        // No plugin of its own: the editor.metaNotes setting is the switch.
        {nullptr, m_metaPane,
         [this, parent] {
             m_metaPane = new MetaNotesPane(parent);
             m_metaPane->setTheme(m_theme);
             m_metaPane->setIndex(&m_index);
             connect(m_metaPane, &MetaNotesPane::noteActivated, this,
                     [this](const QString &relativePath, qsizetype offset) {
                         // Vault -> note -> run: the note opens and the pane goes
                         // back to listing its own runs.
                         m_metaPane->setVaultMode(false);
                         openRelative(relativePath);
                         m_active->editor->setCursorOffset(offset);
                         m_active->editor->setFocus();
                     });
             const ParseOptions options = parseOptions();
             m_metaPane->setDelimiters(options.open, options.close);
             connect(m_metaPane, &MetaNotesPane::jumpRequested, this, [this](const Range &run) {
                 m_active->editor->selectRange(run);
                 m_active->editor->setFocus();
             });
             return m_metaPane.data();
         },
         "metaNotesTabButton", "brackets", tr("Meta notes"), "meta-notes"},
        {"footnotes", m_footnotes,
         [this, parent] {
             m_footnotes = new FootnotesPane(parent);
             connect(m_footnotes, &FootnotesPane::jumpRequested, this, [this](qsizetype offset) {
                 m_active->editor->setCursorOffset(offset);
                 m_active->editor->setFocus();
             });
             return m_footnotes.data();
         },
         "footnotesTabButton", "quote", tr("Footnotes"), "footnotes"},
        {"bookmarks", m_bookmarks,
         [this, parent] {
             m_bookmarks = new BookmarksPane(parent);
             m_bookmarks->setVaultRoot(m_vault.isValid() ? m_vault.root() : QString());
             // Bookmarks are this app's own (the vault's file is filtered when read), so a note
             // outside the vault that was bookmarked stays openable.
             const auto openBookmark = [this](const QString &relativePath) {
                 if (m_vault.isValid() && !relativePath.isEmpty())
                     openNote(m_vault.absolute(relativePath));
             };
             connect(m_bookmarks, &BookmarksPane::noteActivated, this, openBookmark);
             connect(m_bookmarks, &BookmarksPane::headingActivated, this,
                     [this, openBookmark](const QString &relativePath, const QString &subpath) {
                         openBookmark(relativePath);
                         jumpToHeading(subpath);
                     });
             connect(m_bookmarks, &BookmarksPane::searchActivated, this,
                     [this](const QString &query) { openSearch(query); });
             return m_bookmarks.data();
         },
         "bookmarksTabButton", "bookmark", tr("Bookmarks"), "bookmarks"},
#ifdef NOTEAPPC_HAVE_GRAPH
        {"graph", m_localGraph,
         [this, parent] {
             // The factory runs at start, shown or not; setIndex() below defers
             // the whole-vault read to the pane's first show.
             m_localGraph = new GraphView(parent);
             m_localGraph->setObjectName(QStringLiteral("localGraph"));
             wireGraphOptions(m_localGraph);
             m_localGraph->setLocalGraph(true);
             m_localGraph->setTheme(m_theme);
             m_localGraph->setIndex(&m_index, true); // read when the pane is first shown
             connect(m_localGraph, &GraphView::noteActivated, this, &MainWindow::openRelative);
             connect(m_localGraph, &GraphView::noteActivatedInNewTab, this, &MainWindow::openRelativeInNewTab);
             return m_localGraph.data();
         },
         "localGraphTabButton", "git-fork", tr("Local graph"), "localgraph"},
#endif
    };

    const bool first = m_rightPanes->count() == 0;
    auto *tabLayout = static_cast<QHBoxLayout *>(static_cast<QWidget *>(m_rightTabs->parent())->layout());
    bool built = false;
    int slot = 0;
    for (const Pane &pane : panes) {
        auto *button = m_rightSidebar->findChild<QToolButton *>(QLatin1String(pane.button));
        const bool on = pane.feature ? featureOn(pane.feature) : m_settings.boolean(QStringLiteral("editor.metaNotes"));
        if (!on) {
            delete button;
            delete pane.widget; // its QPointer member nulls itself
            continue;
        }
        if (!pane.widget) {
            QWidget *widget = pane.build();
            widget->setProperty("viewType", pane.viewType);
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
    if (m_headerGraphButton)
        m_headerGraphButton->setVisible(featureOn("graph"));
    if (m_graph && !featureOn("graph")) {
        if (m_mainStack->currentWidget() == m_graph)
            leaveSettings();
        delete m_graph;
    }
#endif
#ifdef NOTEAPPC_HAVE_PANDOC
    if (auto *tree = qobject_cast<FileTree *>(m_tree))
        tree->setAcceptFileDrops(featureOn("pandoc") ? [](const QString &path) {
            return !pandoc::readerForSuffix(QFileInfo(path).suffix()).isEmpty();
        } : std::function<bool(const QString &)>());
#endif
    if (!featureOn("page-preview"))
        delete m_preview;
#ifdef NOTEAPPC_HAVE_NETWORK
    if (!featureOn("word-lookup")) { // nothing built, nothing listening
        delete m_lookupPopover;
        delete std::exchange(m_dictClient, nullptr);
    }
#endif
    for (const auto &leaf : m_leaves)
        leaf->editor->setContextActions(contextActions());
    if (m_active)
        updateWordCount(); // the word and character counts are one feature

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
    for (const char *id : {"noteAppC:next-meta-note", "noteAppC:previous-meta-note"}) {
        for (QAction *action : std::as_const(m_commands)) {
            if (action->objectName() == QLatin1String(id))
                action->setEnabled(m_settings.boolean(QStringLiteral("editor.metaNotes")));
        }
    }
    applyPopoutFeature();
    // Switched off mid-recording: its Stop command just went dark, so stop
    // here rather than leave the microphone on.
    if (m_recorder && !featureOn("audio-recorder"))
        m_recorder->stop();
    m_indexer->setMetaNotes(parseOptions()); // index the runs only while the setting is on
    updateNotebookIndex();
    if (m_painted)
        syncPlugins();
}

void MainWindow::updateNotebookIndex()
{
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    // With notebooks on, the index reads a .ipynb as the text it opens as; off, it is a plain file.
    m_indexer->setNotebookReader(featureOn("notebooks")
                                     ? Indexer::NotebookReader([](const QByteArray &bytes) -> std::optional<QString> {
                                           notebook::IpynbDocument document;
                                           if (!notebook::parseIpynb(bytes, &document))
                                               return std::nullopt;
                                           return notebook::ipynbMarkdown(document);
                                       })
                                     : Indexer::NotebookReader());
#endif
    // A .ink file is a note whose text is its text boxes, indexed whether or not the ink module is installed, on
    // or off: the host reads the JSON itself (QJsonDocument), so indexing never loads the module.
    m_indexer->setForeignReader({QStringLiteral("ink")}, noteAppC::inkTextBoxes, noteAppC::kInkIndexMaxBytes);
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
    if (m_footnotes) {
        if (m_active && path == m_active->notePath && !m_active->view)
            m_footnotes->setFootnotes(m_active->editor->footnotes());
        else
            m_footnotes->clear();
    }
    if (m_metaPane) {
        if (m_active && !path.isEmpty() && path == m_active->notePath && !m_active->view) {
            m_metaPane->setNote(m_active->editor->noteText(), m_active->editor->metaNoteRanges(),
                                m_active->editor->headings());
            m_metaPane->setCursorOffset(m_active->editor->cursorOffset());
        } else {
            m_metaPane->clear();
        }
    }
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
    add(QStringLiteral("workspace:toggle-pin"), tr("Toggle pin"), QString(), &MainWindow::togglePin);
    add(QStringLiteral("workspace:close"), tr("Close current tab"), QStringLiteral("Ctrl+W"),
        [this] { closeLeaf(m_active); });
    add(QStringLiteral("workspace:undo-close-pane"), tr("Undo close tab"), QStringLiteral("Ctrl+Shift+T"),
        &MainWindow::undoCloseTab);
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
                if (leaf != m_active && !leaf->pinned)
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
    // Limit: Obsidian's own id for this command is unconfirmed; this one
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
    // Where New note puts a note (newNoteFolder), the next free "Untitled", opened at once. A python kernel is the
    // notebook's kernelspec when one is installed; without, only the language says python.
    add(QStringLiteral("notebooks:new"), tr("Notebooks: Create new notebook"), QString(), [this] {
        if (!m_vault.isValid())
            return;
        const notebook::KernelSpec kernel = notebook::kernelSpecsByLanguage().value(QStringLiteral("python"));
        QString error;
        const QString path = m_vault.createNote(newNoteFolder(), tr("Untitled"), &error, QStringLiteral(".ipynb"),
                                                QString::fromUtf8(notebook::newIpynb(kernel.name.isEmpty() ? nullptr : &kernel)));
        if (path.isEmpty()) {
            QMessageBox::warning(this, tr("New notebook"), tr("Could not create the notebook:\n%1").arg(error));
            return;
        }
        m_indexer->reindex(path);
        openNote(path);
    });
    add(QStringLiteral("notebooks:run-block"), tr("Notebooks: Run the code block at the cursor"),
        QStringLiteral("Ctrl+Shift+Return"), [this] {
            if (!m_active->view)
                m_active->editor->runNotebookBlockAt(m_active->editor->cursorOffset());
        });
    add(QStringLiteral("notebooks:run-all"), tr("Notebooks: Run every code block in this note"), QString(), [this] {
        if (!m_active->view && m_active->editor->runAllNotebookBlocks() == 0)
            statusBar()->showMessage(tr("No code block here can run"), 3000);
    });
    add(QStringLiteral("notebooks:run-above"), tr("Notebooks: Run every code block down to the cursor"), QString(),
        [this] {
            if (!m_active->view)
                m_active->editor->runNotebookBlocksUpTo(m_active->editor->cursorOffset());
        });
    add(QStringLiteral("notebooks:run-below"), tr("Notebooks: Run every code block from the cursor down"), QString(),
        [this] {
            if (!m_active->view)
                m_active->editor->runNotebookBlocksFrom(m_active->editor->cursorOffset());
        });
    // The kernel of the block at the cursor, in this note: a note has one
    // kernel per language, so what one block set the next one sees.
    add(QStringLiteral("notebooks:interrupt"), tr("Notebooks: Interrupt the kernel"), QString(), [this] {
        if (m_notebook && !m_active->view)
            m_notebook->interruptKernel(m_active->editor->codeLanguageAt(m_active->editor->cursorOffset()),
                                        m_active->notePath);
    });
    add(QStringLiteral("notebooks:restart"), tr("Notebooks: Restart the kernel"), QString(), [this] {
        if (m_notebook && !m_active->view)
            m_notebook->restartKernel(m_active->editor->codeLanguageAt(m_active->editor->cursorOffset()),
                                      m_active->notePath);
    });
    // Jupyter's "restart and run all": clean kernels, then every block in order.
    add(QStringLiteral("notebooks:restart-run-all"), tr("Notebooks: Restart the kernel and run every code block"),
        QString(), [this] {
            if (m_active->view)
                return;
            if (m_notebook)
                m_notebook->restartKernel(QString(), m_active->notePath); // every language's, in this note
            if (m_active->editor->runAllNotebookBlocks() == 0)
                statusBar()->showMessage(tr("No code block here can run"), 3000);
        });
    add(QStringLiteral("notebooks:clear-outputs"), tr("Notebooks: Clear the output of every code block"), QString(),
        [this] {
            if (!m_active->view)
                statusBar()->showMessage(tr("Cleared %1 outputs").arg(m_active->editor->clearNotebookOutputs()), 3000);
        });
#endif
    add(QStringLiteral("audio-recorder:start"), tr("Audio recorder: Start recording audio"), QString(),
        &MainWindow::startRecording);
    add(QStringLiteral("audio-recorder:stop"), tr("Audio recorder: Stop recording audio"), QString(), [this] {
        if (m_recorder)
            m_recorder->stop();
    });
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    add(QStringLiteral("encrypted-notes:encrypt"), tr("Encrypted notes: Encrypt this note"), QString(),
        &MainWindow::encryptCurrentNote);
    add(QStringLiteral("encrypted-notes:decrypt"), tr("Encrypted notes: Decrypt this note"), QString(),
        &MainWindow::decryptCurrentNote);
    add(QStringLiteral("encrypted-notes:new"), tr("Encrypted notes: New encrypted note"), QString(),
        &MainWindow::newEncryptedNote);
    add(QStringLiteral("encrypted-notes:lock"), tr("Encrypted notes: Lock encrypted notes"), QString(),
        [this] { m_encryption->lock(); });
    add(QStringLiteral("encrypted-notes:change-password"), tr("Encrypted notes: Change password"), QString(),
        &MainWindow::changeEncryptionPassword);
#endif
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
#ifdef NOTEAPPC_HAVE_IMAGEVIEW
    add(QStringLiteral("image-annotation:extract-annotations"), tr("Image: Extract annotations to a note"), QString(),
        [this] {
            auto *image = qobject_cast<ImageView *>(m_active->view);
            if (!image) {
                statusBar()->showMessage(tr("Open an image with annotation on first"), 3000);
                return;
            }
            flushSaves(); // the note it extracts into may be open, with a commit in flight
            QString error;
            if (!image->extractAnnotations(QString(), &error))
                QMessageBox::warning(this, tr("Extract annotations"), error);
            else
                statusBar()->showMessage(tr("Annotations extracted"), 3000);
        });
#endif
#ifdef NOTEAPPC_HAVE_SYNC
    add(QStringLiteral("sync:sign-in"), tr("Sync: Sign in"), QString(), &MainWindow::syncSignIn);
    add(QStringLiteral("sync:forget-keys"), tr("Sync: Forget this device's keys"), QString(), [this] {
        if (m_sync) {
            m_sync->forgetStoredKeys();
            statusBar()->showMessage(tr("Sync keys removed from the keyring; the password is asked next time"), 5000);
        }
    });
    add(QStringLiteral("sync:sync-now"), tr("Sync: Sync now"), QString(), [this] {
        if (m_sync && !m_syncNeedsPassword)
            m_sync->syncNow();
        else
            syncSignIn();
    });
#endif
#ifdef NOTEAPPC_HAVE_SHARE
    // A note shared already opens its dialog; one that is not is shared first.
    add(QStringLiteral("share:share-note"), tr("Share: Share this note"), QString(), [this] {
        if (const ShareSession *session = m_active ? sessionFor(m_active->notePath) : nullptr) {
            showShareDialog(session->shareID(), QString());
            return;
        }
        QString error;
        const QString code = shareNote(&error);
        const ShareSession *session = m_active ? sessionFor(m_active->notePath) : nullptr;
        if (code.isEmpty() || !session) {
            statusBar()->showMessage(error, 8000);
            return;
        }
        showShareDialog(session->shareID(), code);
    });
    add(QStringLiteral("share:who"), tr("Share: Who has access to this note"), QString(), [this] {
        if (const ShareSession *session = m_active ? sessionFor(m_active->notePath) : nullptr)
            showShareDialog(session->shareID(), QString());
        else
            statusBar()->showMessage(tr("This note is not shared"), 5000);
    });
    add(QStringLiteral("share:join"), tr("Share: Join a shared note"), QString(), [this] {
        QString error;
        if (!ensureShare(&error)) { // the dialog checks the code with the engine's own parser
            statusBar()->showMessage(error, 8000);
            return;
        }
        auto *dialog = new JoinShareDialog(m_share.get(), [this](const QString &code, QString *error) { return joinShare(code, error); },
                                           this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->open();
    });
    add(QStringLiteral("share:stop"), tr("Share: Stop sharing this note on this device"), QString(),
        &MainWindow::stopSharingNote);
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
        [this] { newNoteIn(newNoteFolder()); });
    add(QStringLiteral("file-explorer:move-file"), tr("Move file to another folder"), QString(),
        [this] { chooseMoveFolder(m_active->notePath); });
    add(QStringLiteral("app:delete-file"), tr("Delete current file"), QString(),
        [this] {
            if (!m_vault.isValid())
                statusBar()->showMessage(tr("Delete works on notes in a vault"), 3000);
            deletePath(m_active->notePath);
        });
    add(QStringLiteral("app:open-settings"), tr("Open settings"), QStringLiteral("Ctrl+,"),
        &MainWindow::openSettings);
    // The one switch for the trust gate: kept in the person's own settings, never in the vault.
    add(QStringLiteral("vault:toggle-trust"), tr("Vault: Trust or stop trusting this vault"), QString(), [this] {
        if (m_vault.isValid())
            setVaultTrusted(!vaultTrusted());
        else
            statusBar()->showMessage(tr("Only a vault can be trusted"), 3000);
    });
    add(QStringLiteral("noteAppC:quit"), tr("Quit"), QStringLiteral("Ctrl+Q"), &MainWindow::close);
    add(QStringLiteral("noteAppC:next-meta-note"), tr("Meta notes: Next meta note"), QStringLiteral("Alt+]"),
        [this] { jumpMetaNote(+1); });
    add(QStringLiteral("noteAppC:previous-meta-note"), tr("Meta notes: Previous meta note"),
        QStringLiteral("Alt+["), [this] { jumpMetaNote(-1); });
    add(QStringLiteral("app:go-back"), tr("Navigate back"), QStringLiteral("Alt+Left"),
        &MainWindow::goBack);
    add(QStringLiteral("app:go-forward"), tr("Navigate forward"), QStringLiteral("Alt+Right"),
        &MainWindow::goForward);
    add(QStringLiteral("file-recovery:open"), tr("File recovery: Open saved snapshots"), QString(),
        &MainWindow::openRecovery);
    add(QStringLiteral("workspace:export-pdf"), tr("Export to PDF"), QString(),
        &MainWindow::exportPdf);
    add(QStringLiteral("markmap:show-note"), tr("Mind maps: Show as mind map"), QString(), &MainWindow::showAsMindMap);
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

#ifdef NOTEAPPC_HAVE_PANDOC
    buildPandocActions();
#endif
    buildPopoutActions();
    buildSidebarPopoutActions();
    rebuildHotkeyCommands();
}

void MainWindow::rebuildHotkeyCommands()
{
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
    const Hotkeys::Bindings bindings = Hotkeys::load(m_vault.isValid() ? m_vault.root() : QString(), vaultTrusted());
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
    if (popoutEventFilter(watched, event))
        return true;
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    // A person at the keys keeps unlocked notes from timing out.
    if (m_encryption && (event->type() == QEvent::KeyPress || event->type() == QEvent::MouseButtonPress))
        m_encryption->noteActivity();
#endif
    // Installed on the whole application (see the constructor), so this sees
    // a side-button click no matter which widget the mouse happens to be
    // over - the editor, the tree, a sidebar pane.
    if (event->type() == QEvent::MouseButtonPress && watched == m_scrim && m_drawer) {
        closeDrawer(); // a tap beside the drawer puts it away
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const bool isNavButton =
            mouseEvent->button() == Qt::BackButton || mouseEvent->button() == Qt::ForwardButton;
        auto *widget = qobject_cast<QWidget *>(watched);
        if (isNavButton && widget && isWorkspaceWindow(widget->window())) {
            if (mouseEvent->button() == Qt::BackButton)
                goBack();
            else
                goForward();
            return true;
        }
    }

#ifdef NOTEAPPC_HAVE_NETWORK
    // The definition popover goes with a click outside it or Escape; it has
    // no link to leave, so PagePreview's own timer never starts for it.
    if (m_lookupPopover && m_lookupPopover->isVisible()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *widget = qobject_cast<QWidget *>(watched);
            if (widget && widget != m_lookupPopover && !m_lookupPopover->isAncestorOf(widget))
                m_lookupPopover->hide();
        } else if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            m_lookupPopover->hide();
        }
    }
#endif

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
        if (keyEvent->key() == Qt::Key_Delete && keyEvent->modifiers() == Qt::NoModifier && !keyEvent->isAutoRepeat()
            && m_tree->currentIndex().isValid()) { // while renaming, the key goes to the line edit
            deletePathAt(m_tree->currentIndex());
            return true;
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
    if (event->type() == QEvent::FocusIn) {
        for (const auto &leaf : m_leaves) {
            if (watched == leaf->inlineTitle) {
                activateLeaf(leaf.get());
                break;
            }
        }
    }
    if (event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton && m_tree
        && watched == m_tree->viewport()) {
        // A middle click opens a file in a new tab, as in Obsidian.
        const QModelIndex index = m_tree->indexAt(static_cast<QMouseEvent *>(event)->position().toPoint());
        if (index.isValid() && !m_files->isDir(index)) {
            const QString path = m_files->filePath(index);
            if (isViewFile(path) || Vault::isNote(path) || Vault::isEncryptedNote(path) || isNotebookFile(path)) {
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
    leaf->inlineTitle->setTextMargins(side, 32, side, 0);
}

void MainWindow::toggleSidebar()
{
    if (popoutHolding(m_sidebar))
        return; // in a window of its own: closing that window is how it goes
    if (m_narrow) {
        m_drawer == m_sidebar ? closeDrawer() : openDrawer(m_sidebar);
        return;
    }
    m_sidebar->setVisible(!m_sidebar->isVisible());
    m_layoutSave.start();
}

void MainWindow::toggleRightSidebar()
{
    if (popoutHolding(m_rightSidebar))
        return;
    if (m_narrow) {
        m_drawer == m_rightSidebar ? closeDrawer() : openDrawer(m_rightSidebar);
        return;
    }
    m_rightSidebar->setVisible(!m_rightSidebar->isVisible());
    windowSettings().setValue(kRightSidebar, m_rightSidebar->isVisible());
    m_layoutSave.start();
}

void MainWindow::openDrawer(QWidget *side)
{
    auto *splitter = findChild<QSplitter *>(QStringLiteral("mainSplitter"));
    closeDrawer();
    if (!splitter || splitter->indexOf(side) < 0)
        return;
    m_drawerIndex = splitter->indexOf(side);
    if (!m_scrim) {
        m_scrim = new QWidget(centralWidget());
        m_scrim->setObjectName(QStringLiteral("drawerScrim"));
        m_scrim->setAttribute(Qt::WA_StyledBackground);
        m_scrim->setStyleSheet(QStringLiteral("#drawerScrim { background: rgba(0, 0, 0, 0.45); }"));
    }
    // Over the note area only; the header bar, with the buttons that open
    // and close it, stays reachable.
    const QRect area = splitter->geometry();
    m_scrim->setGeometry(area);
    m_scrim->show();
    m_scrim->raise();
    side->setParent(centralWidget());
    const int width = qMin(320, int(area.width() * 0.85));
    side->setGeometry(side == m_sidebar ? area.x() : area.right() - width + 1, area.y(), width, area.height());
    side->show();
    side->raise();
    m_drawer = side;
}

void MainWindow::closeDrawer()
{
    if (!m_drawer)
        return;
    QWidget *side = std::exchange(m_drawer, nullptr);
    side->hide();
    if (auto *splitter = findChild<QSplitter *>(QStringLiteral("mainSplitter")))
        splitter->insertWidget(m_drawerIndex, side); // back where it came from, folded
    if (m_scrim)
        m_scrim->hide();
}

void MainWindow::showRightPane(QWidget *pane)
{
    PopoutWindow *own = popoutHolding(pane);
    if (own && own->kind() == PopoutWindow::Kind::Pane) { // a window of its own: bring that
        own->raise();
        own->activateWindow();
        return;
    }
    if (!m_rightSidebar->isVisible())
        toggleRightSidebar();
    m_rightPanes->setCurrentWidget(pane);
    for (QAbstractButton *tab : m_rightTabs->buttons()) {
        if (tab->property("pane").value<QObject *>() == pane)
            tab->setChecked(true);
    }
    if (own) { // the whole sidebar is in a window
        own->raise();
        own->activateWindow();
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
        wireGraphOptions(m_graph);
        m_graph->setIndex(&m_index);
        connect(m_graph, &GraphView::noteActivated, this, &MainWindow::openRelative);
        connect(m_graph, &GraphView::noteActivatedInNewTab, this, &MainWindow::openRelativeInNewTab);
        connect(m_graph, &GraphView::closeRequested, this, &MainWindow::leaveSettings);
        m_mainStack->addWidget(m_graph);
    }
    m_graph->setCurrentNote(m_vault.isValid() && !m_active->notePath.isEmpty() ? m_vault.relative(m_active->notePath)
                                                                      : QString());
    m_mainStack->setCurrentWidget(m_graph);
    m_graph->setFocus();
#endif
}

QString MainWindow::graphOptionsPath() const
{
    return appDir(XDGroot::State, QStringLiteral("vaults/%1/graph.json").arg(vaultDigest(m_vault.root())));
}

void MainWindow::wireGraphOptions(GraphView *view)
{
#ifdef NOTEAPPC_HAVE_GRAPH
    if (m_vault.isValid()) {
        QFile own(graphOptionsPath());
        view->setOptions(own.open(QIODevice::ReadOnly)
                             ? GraphOptions::fromJson(QJsonDocument::fromJson(own.readAll()).object())
                             : GraphOptions::readFromVault(m_vault.root()));
    }
    if (!m_graphSave.isSingleShot()) {
        m_graphSave.setSingleShot(true);
        m_graphSave.setInterval(500);
        connect(&m_graphSave, &QTimer::timeout, this, &MainWindow::saveGraphOptions);
    }
    // Connected once per view: wireGraphOptions() runs again on a vault switch.
    if (view->property("optionsWired").toBool())
        return;
    view->setProperty("optionsWired", true);
    connect(view, &GraphView::optionsChanged, this, [this, view] {
        m_graphChanged = view;
        m_graphSave.start();
    });
#else
    Q_UNUSED(view);
#endif
}

void MainWindow::saveGraphOptions()
{
#ifdef NOTEAPPC_HAVE_GRAPH
    if (!m_graphChanged || !m_vault.isValid())
        return;
    // One file serves both graphs; the local depth and link switches are the local pane's alone.
    GraphOptions options = m_graphChanged->options();
    if (m_localGraph) {
        options.localDepth = m_localGraph->options().localDepth;
        options.localIncoming = m_localGraph->options().localIncoming;
        options.localOutgoing = m_localGraph->options().localOutgoing;
    }
    const QString path = graphOptionsPath();
    ensurePrivateDir(QFileInfo(path).path());
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(options.toJson()).toJson());
        file.commit();
    }
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

#ifdef NOTEAPPC_HAVE_PANDOC
    refreshPandocCommands();
#endif
    struct Entry {
        QString id;
        QString name;
        QList<QKeySequence> keys;
        bool dimmed = false;
    };
    QList<Entry> entries;
    for (const QAction *action : std::as_const(m_commands)) {
        if (action->isEnabled() && action->objectName() != QLatin1String("command-palette:open"))
            entries.append({action->objectName(), action->text(), action->shortcuts(), action->property("dimmed").toBool()});
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
    QList<bool> dimmed;
    m_paletteIDs.clear();
    for (const Entry &entry : std::as_const(entries)) {
        names << entry.name;
        dimmed << entry.dimmed;
        keys << (entry.keys.isEmpty() ? QString() : entry.keys.constFirst().toString(QKeySequence::NativeText));
        m_paletteIDs.insert(entry.name, entry.id);
    }
    m_palette->setCandidates(names, keys, {}, dimmed);
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
    if (!m_settingsWindow->isVisible()) {
        // A third of an ultrawide, most of a laptop; centred on this window.
        const QRect screen = (windowHandle() && windowHandle()->screen() ? windowHandle()->screen()
                                                                        : QGuiApplication::primaryScreen())
                                 ->availableGeometry();
        const QSize size(std::min(960, screen.width() * 9 / 10), std::min(760, screen.height() * 85 / 100));
        m_settingsWindow->resize(size);
        QRect frame(QPoint(), size);
        frame.moveCenter(isVisible() ? geometry().center() : screen.center());
        m_settingsWindow->move(frame.topLeft());
    }
    m_settingsWindow->show();
    m_settingsWindow->raise();
    m_settingsWindow->activateWindow();
}

void MainWindow::closeSettings()
{
    m_settingsWindow->done(QDialog::Accepted); // finished() saves
}

bool MainWindow::settingsOpen() const
{
    return m_settingsWindow->isVisible();
}

void MainWindow::leaveSettings()
{
    // The main area's other page is the graph; settings is a window of its own
    // and stays open beside the note.
    m_mainStack->setCurrentIndex(0);
}

// The path of a canvas card, an embed or a base row comes from the vault's own text:
// ".." or an absolute one does not open anything outside it.
QString MainWindow::insideVault(const QString &relativePath) const
{
    if (relativePath.isEmpty() || !m_vault.isValid())
        return {};
    const QString path = QDir::cleanPath(m_vault.absolute(relativePath));
    return m_vault.contains(path) ? path : QString();
}

void MainWindow::openRelative(const QString &relativePath)
{
    if (const QString path = insideVault(relativePath); !path.isEmpty())
        openNote(path);
}

void MainWindow::openRelativeInNewTab(const QString &relativePath)
{
    const QString path = insideVault(relativePath);
    if (path.isEmpty())
        return;
    newTab();
    openNote(path);
}

void MainWindow::refreshOpenVaultFiles()
{
    if (!std::exchange(m_vaultDirty, false))
        return;
    for (const auto &leaf : m_leaves)
        leaf->editor->refreshVaultFiles();
}

void MainWindow::onIndexChanged()
{
    const bool files = std::exchange(m_vaultDirty, false); // this refresh is the vault timer's as well
    const QStringList targets = m_index.linkTargets();
    const auto aliases = m_index.aliases();
    for (const auto &leaf : m_leaves) {
        leaf->editor->setLinkTargets(targets);
        leaf->editor->setLinkAliases(aliases);
        // Before this, a note opened while the vault was first indexed showed
        // every link as missing until it was next edited.
        if (files)
            leaf->editor->refreshVaultFiles();
        else
            leaf->editor->refreshLinkStatus();
    }
    // The backlinks, tags and outgoing-links panes listen to Index::changed
    // themselves; refreshing them here as well did each one twice per save.
    if (m_backlinkStatus->isVisible())
        m_backlinkCountTimer.start();
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
    applyUserColours(theme, [this](const QString &key) { return m_settings.text(key); });
    return theme;
}

void MainWindow::applySettings()
{
    // Every switch on the settings page comes here, and restyling the window
    // and relaying out every open note was most of it - 17 ms with a 1 MB
    // note open - so that waits for the theme itself to change.
    // The sidebar's sort order, as the settings file has it (per vault).
    if (auto *sortMenu = m_sidebar->findChild<QMenu *>(QStringLiteral("sidebarSortMenu"))) {
        const QString key = m_settings.text(QStringLiteral("files.sortOrder"));
        for (QAction *action : sortMenu->actions()) {
            if (action->data().toString() != key)
                continue;
            action->setChecked(true);
            const int column = action->property("column").toInt();
            const auto order = Qt::SortOrder(action->property("order").toInt());
            if (m_tree->header()->sortIndicatorSection() != column
                || m_tree->header()->sortIndicatorOrder() != order)
                m_tree->sortByColumn(column, order);
        }
    }
    applyHiddenTypes();
    const Theme theme = themeFromSettings();
    const bool restyle = !m_themeApplied || !(theme == m_theme);
    m_theme = theme;
    refreshFileIcons(); // appearance.fileIcons, and the tab icons' colour
    applyFeatures(); // first, so a pane it builds is themed below
    if (m_metaPane) {
        const ParseOptions options = parseOptions();
        m_metaPane->setDelimiters(options.open, options.close);
    }
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    m_encryption->setAutoLockMinutes(m_settings.integer(QStringLiteral("encryption.autoLockMinutes")));
    if (!featureOn("encrypted-notes"))
        m_encryption->lock();
#endif
    if (restyle) {
        applyTheme();
        m_themeApplied = true;
    }

    for (const auto &leaf : m_leaves)
        applyEditorSettings(leaf->editor);
#ifdef NOTEAPPC_HAVE_CANVAS
    for (const auto &leaf : m_leaves) {
        if (auto *canvas = qobject_cast<CanvasView *>(leaf->view)) {
            canvas->setSnapToObjects(m_settings.boolean(QStringLiteral("canvas.snapToObjects")));
            canvas->setSnapToGrid(m_settings.boolean(QStringLiteral("canvas.snapToGrid")));
        }
    }
#endif
    for (const auto &leaf : m_leaves) {
        if (auto *module = qobject_cast<ModuleView *>(leaf->view))
            module->applySettings(m_settings);
    }
    if (const QString mode = m_settings.text(QStringLiteral("editor.defaultMode")); mode != m_appliedDefaultMode) {
        m_appliedDefaultMode = mode;
        for (const auto &leaf : m_leaves)
            leaf->editor->setMode(defaultMode());
    }
    updateSpelling();
    updateNoteHistory();
    updateSync();
    updateShare();
    applyHotkeys(); // the vault may carry its own Obsidian hotkeys.json

    for (const auto &leaf : m_leaves) {
        leaf->inlineTitle->setVisible(!leaf->notePath.isEmpty() && !leaf->view
                                      && m_settings.boolean(QStringLiteral("appearance.showInlineTitle")));
    }
}

void MainWindow::hideTypedRows(const QModelIndex &parent, int first, int last)
{
    for (int row = first; row <= last; ++row) {
        const QModelIndex index = m_files->index(row, 0, parent);
        if (m_files->isDir(index)) {
            // Only folders already loaded have rows; the rest cost nothing.
            if (m_files->rowCount(index) > 0)
                hideTypedRows(index, 0, m_files->rowCount(index) - 1);
            continue;
        }
        const QString name = m_files->fileName(index).toLower();
        bool hide = false;
        for (const QString &type : m_hiddenTypes)
            hide = hide || name.endsWith(QLatin1Char('.') + type);
        if (hide != m_tree->isRowHidden(row, parent))
            m_tree->setRowHidden(row, parent, hide);
    }
}

void MainWindow::applyHiddenTypes()
{
    const QStringList types = Settings::fileTypes(m_settings.value(QStringLiteral("files.hiddenExtensions")).toStringList());
    if (types == m_hiddenTypes)
        return;
    m_hiddenTypes = types;
    if (!m_vault.isValid())
        return;
    const QModelIndex root = m_files->index(m_vault.root());
    if (const int rows = m_files->rowCount(root); rows > 0)
        hideTypedRows(root, 0, rows - 1);
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
    const QString path = Vault::uniqueAttachmentPath(folder, base + QLatin1Char('.') + suffix);
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
    if (!m_vault.isValid() || m_active->view || !m_vault.contains(m_active->notePath)
        || !Vault::isNote(m_active->notePath) || !save()) // never from a notebook: its JSON is not a note's text
        return;
    const QString source = m_active->notePath;
    QStringList notes;
    for (const QString &path : m_index.allPaths()) { // the index lists a notebook too, while the reader is on
        if (Vault::isNote(path))
            notes << path;
    }
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
    // Both ends are Markdown notes: appending text to a notebook's JSON, or its JSON to a note, corrupts it.
    if (!Vault::isNote(source) || !Vault::isNote(target)) {
        QMessageBox::warning(this, tr("Merge"), tr("Only Markdown notes can be merged: %1")
                                                    .arg(QFileInfo(Vault::isNote(source) ? target : source).fileName()));
        return false;
    }
    // What the target's tabs hold unsaved is its text too.
    for (const auto &leaf : m_leaves) {
        if (leaf->notePath == target && isModified(leaf.get()) && !saveLeaf(leaf.get(), false))
            return false;
    }
    // As for a rename: typed [[links]] to the source go to disk first and
    // are read from there, or the next autosave writes them back unrewritten.
    const QStringList unindexed = unindexedNotes(autosave());
    Note from;
    Note into;
    QString error;
    if (!Vault::read(source, from, &error) || !Vault::read(target, into, &error)) {
        QMessageBox::warning(this, tr("Merge"), error);
        return false;
    }
    if (from.lossy) {
        QMessageBox::warning(this, tr("Merge"), tr("%1 is not valid UTF-8; merging would change its bytes").arg(QFileInfo(source).fileName()));
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
        m_index.updateLinksAfterRename(m_vault.relative(source), m_vault.relative(target), m_vault, &error, unindexed);
    if (!deleteVaultPath(source, &error)) {
        QMessageBox::warning(this, tr("Merge"), tr("Merged, but %1 stays:\n%2").arg(QFileInfo(source).fileName(), error));
        return false;
    }
    if (m_fileIcons.removeUnder(m_vault.relative(source)))
        m_fileIcons.save();
    openNote(target);
    QList<Leaf *> gone;
    for (const auto &leaf : m_leaves) {
        if (leaf->notePath == source)
            gone << leaf.get();
    }
    for (Leaf *leaf : std::as_const(gone))
        dropLeaf(leaf);
    m_settings.forgetNotePosition(source);
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
        // Limit: finds its date by trying the year either side, since an
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
    const qsizetype found = ObsidianSyntax::findHeading(metadata.headings, wanted);
    return found >= 0 ? metadata.headings.at(found).range.start : -1;
}

void MainWindow::jumpToHeading(const QString &subpath)
{
    const QString wanted = (subpath.startsWith(QLatin1Char('#')) ? subpath.mid(1) : subpath).trimmed();
    if (wanted.isEmpty())
        return;
    const auto jump = [this, wanted] {
        const QList<Heading> &headings = m_active->editor->headings();
        const qsizetype found = ObsidianSyntax::findHeading(headings, wanted);
        if (found < 0)
            return false;
        m_active->editor->setCursorOffset(headings.at(found).range.start);
        return true;
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

void MainWindow::showAsMindMap()
{
    if (!m_active || !m_active->editor || m_active->view)
        return;
    if (showsEncryptedNote(m_active->editor)) { // the dialog's editor is not the note's, and has no key
        QMessageBox::information(this, tr("Mind map"), tr("Mind maps are not drawn for encrypted notes."));
        return;
    }
    const QString title = m_active->notePath.isEmpty() ? tr("Untitled") : QFileInfo(m_active->notePath).completeBaseName();
    const QString outline = markmapSourceOfNote(m_active->editor->noteText());
    if (outline.isEmpty()) {
        QMessageBox::information(this, tr("Mind map"), tr("This note has nothing to draw."));
        return;
    }
    // A fence longer than any run of backticks in the text, so nothing in it can close it early.
    qsizetype longest = 0, run = 0;
    for (const QChar c : outline)
        longest = qMax(longest, run = (c == QLatin1Char('`') ? run + 1 : 0));
    const QString fence(qMax<qsizetype>(3, longest + 1), QLatin1Char('`'));
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("mindMapDialog"));
    dialog.setWindowTitle(tr("Mind map: %1").arg(title));
    dialog.resize(1000, 700);
    auto *editor = new MarkdownEditor(&dialog);
    editor->setTheme(m_theme);
    applyEditorSettings(editor); // registers the markmap renderer, as for any note
    editor->setReadableLineLength(false);
    editor->setMode(MarkdownEditor::Mode::Reading);
    editor->setNoteText(fence + QStringLiteral("markmap\n") + outline + QLatin1Char('\n') + fence + QLatin1Char('\n'));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(editor);
    dialog.exec();
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
    MarkdownEditor::PdfOptions pdf = options.options();
    pdf.taskStates = featureOn("task-states");
    pdf.mindMaps = featureOn("markmap");
    pdf.codeStyler = featureOn("advanced-code-blocks");
    pdf.codeHighlights = m_settings.text(QStringLiteral("editor.codeHighlights"));
#ifdef NOTEAPPC_HAVE_NETWORK
    pdf.remoteReferences = m_settings.boolean(QStringLiteral("editor.remoteReferences"));
#endif
    pdf.vaultRoot = m_vault.isValid() ? m_vault.root() : QString();
    pdf.theme = Theme::builtin(Theme::Mode::Light); // paper is white; the person's colours still apply
    applyUserColours(*pdf.theme, [this](const QString &key) { return m_settings.text(key); });
    if (MarkdownEditor::exportPdf(target, m_active->editor->noteText(), note.completeBaseName(),
                                  m_resolveLink, pdf, &error))
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
    // Any tab on the note first saves what is in its editor, so that is
    // what the snapshot below keeps rather than what was last written.
    for (const auto &leaf : m_leaves) {
        if (!leaf->view && leaf->notePath == notePath && isModified(leaf.get()) && !saveLeaf(leaf.get(), false))
            return;
    }
    Note note;
    QString error;
    if (!readNote(notePath, note, &error)) {
        QMessageBox::warning(this, tr("Restore"), tr("Could not read %1:\n%2").arg(notePath, error));
        return;
    }
    Snapshots::record(notePath, note.text, 0);
    note.text = text; // its line endings and BOM stay as the file has them
    if (!writeNote(notePath, note, &error)) {
        QMessageBox::warning(this, tr("Restore"), tr("Could not write %1:\n%2").arg(notePath, error));
        return;
    }
    note = Vault::asWritten(note);
    recordHistory(notePath, text);
    if (m_vault.isValid())
        m_indexer->reindex(notePath);
    showDiskNote(notePath, note); // every tab on it, not only the focused one
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

#ifdef NOTEAPPC_HAVE_NETWORK
PagePreview *MainWindow::lookupPopover()
{
    if (!m_lookupPopover) {
        m_lookupPopover = new PagePreview(this);
        m_lookupPopover->setObjectName(QStringLiteral("lookupPopover"));
        m_lookupPopover->applyTheme(m_theme);
    }
    return m_lookupPopover;
}
#endif

MarkdownEditor::ContextActions MainWindow::contextActions() const
{
#ifdef NOTEAPPC_HAVE_NETWORK
    const bool lookup = featureOn("word-lookup");
#else
    const bool lookup = false;
#endif
    return {featureOn("global-search"), lookup, m_settings.text(QStringLiteral("lookup.webSearchURL"))};
}

#ifdef NOTEAPPC_HAVE_NETWORK
void MainWindow::lookUp(const QString &word, const QPoint &globalPos)
{
    if (!featureOn("word-lookup"))
        return;
    m_lookupPos = globalPos;
    lookupPopover()->showNote(tr("Looking up \"%1\"...").arg(word), 0, globalPos);
    const quint64 token = ++m_lookupToken;
    const auto show = [this, token](const QString &markdown) {
        // Dismissed, or a newer lookup started, while this one was away.
        if (token == m_lookupToken && m_lookupPopover && m_lookupPopover->isVisible())
            m_lookupPopover->showNote(markdown, 0, m_lookupPos);
    };
    if (m_lookupFunction) {
        m_lookupFunction(word, show);
        return;
    }
    if (!m_dictClient)
        m_dictClient = new DictClient(this);
    const QString server = m_settings.text(QStringLiteral("lookup.server")).trimmed();
    m_dictClient->setServers(server.isEmpty() ? DictClient::serversFromSystem()
                                              : QList<DictClient::Server>{DictClient::parseServerSpec(server)});
    m_dictClient->define(word, [show](DictClient::Result result) { show(DictClient::toMarkdown(result)); });
}
#endif

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
                            [this, spell](const QString &word) {
                                spell->addWord(word);
                                // Every open note stops underlining it, not just this one.
                                for (const auto &leaf : m_leaves)
                                    leaf->editor->refreshSpelling();
                            },
                            [spell](const QString &word) { return spell->isPersonal(word); },
                            [this, spell](const QString &word) {
                                spell->removeWord(word);
                                for (const auto &leaf : m_leaves)
                                    leaf->editor->refreshSpelling();
                            }};
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
            return m_active && !m_active->view && !showsEncryptedNote(m_active->editor) ? m_active->editor->noteText()
                                                                                        : QString();
        };
        callbacks.activeSelectionText = [this] {
            // A plugin never reads an encrypted note: the AI one would send it on.
            return m_active && !m_active->view && !showsEncryptedNote(m_active->editor) ? m_active->editor->selectedText()
                                                                                        : QString();
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
            flushSaves(); // an autosave in flight would write over this
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
        // A plugin's typed question: a small window-modal prompt that does not block the event loop,
        // cancelled by Esc or by its timeout. `done` is called once, whichever comes first.
        callbacks.askUser = [this](const QString &pluginName, const QString &label, bool password, int timeoutMs,
                                   const PluginAppCallbacks::PromptDone &done) {
            auto *dialog = new QInputDialog(this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle(pluginName);
            dialog->setLabelText(label);
            dialog->setTextEchoMode(password ? QLineEdit::Password : QLineEdit::Normal);
            connect(dialog, &QDialog::finished, this, [dialog, done](int result) {
                done(result == QDialog::Accepted, result == QDialog::Accepted ? dialog->textValue() : QString());
            });
            if (timeoutMs > 0)
                QTimer::singleShot(timeoutMs, dialog, &QDialog::reject);
            dialog->open();
        };
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
            for (const auto &popout : m_popouts)
                popout->addAction(action);
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
    // A plugin's fence in an encrypted note is never handed to the plugin.
    const auto guarded = [this, renderer](MarkdownEditor *editor) -> MarkdownEditor::CodeBlockRenderer {
        return [this, renderer, editor](const QString &language, const QString &source, int width,
                                        const QString &theme,
                                        const std::function<void(MarkdownEditor::CodeBlockResult)> &deliver) {
            if (showsEncryptedNote(editor)) {
                deliver({true, tr("*Plugins do not see encrypted notes.*"), QImage()});
                return;
            }
            renderer(language, source, width, theme, deliver);
        };
    };
    for (const auto &leaf : m_leaves) {
        if (!m_pluginLanguages.isEmpty())
            leaf->editor->setCodeBlockRenderer(m_pluginLanguages, nullptr);
        if (renderer)
            leaf->editor->setCodeBlockRenderer(languages, guarded(leaf->editor));
    }
    m_pluginLanguages = languages;
    m_applyPluginRenderer = nullptr;
    if (renderer)
        m_applyPluginRenderer = [languages, guarded](MarkdownEditor *editor) {
            editor->setCodeBlockRenderer(languages, guarded(editor));
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
    QSettings state = windowSettings();
    const QString group = QStringLiteral("publish/") + vaultDigest(m_vault.root());
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
    // The code colours come from the vault's scheme, one reading per mode
    // (a built-in scheme has none), on the worker with the rest.
    const QString schemePath = m_settings.text(QStringLiteral("appearance.scheme"));
    const bool taskStates = featureOn("task-states");
    const bool mindMaps = featureOn("markmap");
    const ParseOptions codeStylerOptions = parseOptions(); // codeStyler and the highlight names, which are all of it
    // The formatting colours too, read here: Settings is not for the worker.
    QHash<QString, QString> colours;
    for (const FormatRow &row : formatRows())
        for (const FormatColour &colour : row.colours)
            for (const bool dark : {true, false})
                colours.insert(formatColourKey(colour, dark), m_settings.text(formatColourKey(colour, dark)));
    watcher->setFuture(QtConcurrent::run([vault = m_vault, choices, schemePath, colours, taskStates, mindMaps,
                                         codeStylerOptions] {
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
        options.parse.taskStates = taskStates;
        options.mindMaps = mindMaps;
        options.parse.codeStyler = codeStylerOptions.codeStyler;
        options.parse.highlightKinds = codeStylerOptions.highlightKinds;
        for (const Theme::Mode mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
            Theme theme = schemePath.isEmpty() ? Theme::builtin(mode) : Scheme::apply(schemePath, mode);
            applyUserColours(theme, [&colours](const QString &key) { return colours.value(key); });
            (mode == Theme::Mode::Dark ? options.codeThemeDark : options.codeThemeLight) = theme;
        }
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
    Icons::setRemoteAllowed(m_settings.boolean(QStringLiteral("editor.remoteReferences")));
    Icons::clearCache();

    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        // Either one, set again unchanged, repolishes every widget in the app.
        if (!(app->palette() == m_theme.palette()))
            app->setPalette(m_theme.palette());
        if (const QString sheet = m_theme.styleSheet(); app->styleSheet() != sheet)
            app->setStyleSheet(sheet);
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
    if (m_active && m_modeButton)
        updateModeButton();
    for (const auto &leaf : m_leaves) {
        leaf->editor->setTheme(m_theme);
#ifdef NOTEAPPC_HAVE_CANVAS
        if (auto *canvas = qobject_cast<CanvasView *>(leaf->view))
            canvas->setTheme(m_theme);
#endif
        if (auto *module = qobject_cast<ModuleView *>(leaf->view))
            module->setTheme(m_theme);
#ifdef NOTEAPPC_HAVE_IMAGEVIEW
        if (auto *image = qobject_cast<ImageView *>(leaf->view))
            image->setTheme(m_theme);
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
#ifdef NOTEAPPC_HAVE_NETWORK
    if (m_lookupPopover)
        m_lookupPopover->applyTheme(m_theme);
#endif
    if (m_outgoingLinks)
        m_outgoingLinks->setTheme(m_theme);
    if (m_metaPane)
        m_metaPane->setTheme(m_theme);
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
        QStringLiteral("QLabel#filesLabel { color: %1; font-size: 11px; font-weight: 700; "
                       "padding-left: 4px; }")
            .arg(m_theme.value(QStringLiteral("--nav-heading-color"))));
    static_cast<FileTree *>(m_tree)->setColours(m_theme);
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

    // Nothing is created here. ~/.config/notolith appears only when something
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
    pruneNotebookImages();
#ifdef NOTEAPPC_HAVE_SHARE
    syncShareViews();
#endif
    syncTabs(m_active->group);
    showNoteInPanes(QString());
}

void MainWindow::openVault(const QString &path)
{
    // A vault another Notolith process has open is that process's: asked before anything here is
    // saved or closed, since a handed-over vault leaves this window as it is.
    const QString served = SingleInstance::nameForVault(path);
    if (m_instance && m_instance->name() != served && QFileInfo(path).isDir()
        && m_instance->deliverTo(served, QFileInfo(path).absoluteFilePath()))
        return;

    if (!maybeSaveAll())
        return;

    Vault vault(path);
    if (!vault.isValid()) {
        QMessageBox::warning(this, tr("Open vault"), tr("%1 is not a directory.").arg(path));
        return;
    }
    // Lost the race for the name (the other process was still starting): it has the vault now.
    if (m_instance && !m_instance->moveTo(served, vault.root()))
        return;

    saveLayout(); // the vault being left
    if (m_graphSave.isActive()) {
        m_graphSave.stop();
        saveGraphOptions(); // still the old vault's path: a change made in the last half second counts
    }
    m_openMode = OpenMode::Vault;
    m_vault = vault;
    m_fileIcons = FileIcons(m_vault.root());
    m_fileIcons.load();
    resetWorkspace();
    resetHistory();

    m_sidebar->setVisible(true);
    m_rightSidebar->setVisible(true);
    syncTabs(m_active->group);

    m_tree->setRootIndex(m_files->setRootPath(m_vault.root()));
    reloadSettings(m_vault.root());
    m_settings.noteOpened(m_vault.root(), QString());
    m_settings.save(); // at once: another process's bare launch reopens the vault most recently opened
    m_settings.takeAdoptedKeys(); // applySettings() follows: nothing left to report
    // Before the scan, by this vault's settings: its notebooks and meta notes
    // are indexed by the first one, and a warm cache is checked against them.
    updateNotebookIndex();
    m_indexer->setMetaNotes(parseOptions());
    m_indexer->setVault(m_vault);
    applySettings();
    if (m_bookmarks)
        m_bookmarks->setVaultRoot(m_vault.root());
    updateNoteHistory();
    restoreLayout(Workspaces::current(m_vault.root()));
#ifdef NOTEAPPC_HAVE_GRAPH
    if (m_graph)
        wireGraphOptions(m_graph);
    if (m_localGraph)
        wireGraphOptions(m_localGraph);
#endif

    statusBar()->showMessage(m_vault.hasObsidianConfig()
                                 ? tr("Vault opened - Obsidian settings found")
                                 : tr("Vault opened"),
                             4000);
    updateTitle();
    updateWordCount();
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    // "On startup" rather than on first open: asked once the window is up.
    if (featureOn("encrypted-notes") && m_settings.boolean(QStringLiteral("encryption.unlockAtStartup"))) {
        QTimer::singleShot(0, this, [this] {
            const QByteArray sample = encryptionSample();
            if (!sample.isEmpty() && !m_encryption->isUnlocked())
                m_encryption->unlock(sample);
        });
    }
#endif
}

void MainWindow::closeVault()
{
    if (!maybeSaveAll())
        return;
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    m_encryption->lock(); // another vault, another password
#endif

    saveLayout();
    if (m_instance)
        m_instance->moveTo(SingleInstance::nameForVault(QString()));
    m_openMode = OpenMode::Empty;
    m_vault = Vault();
    m_indexer->setVault(Vault()); // its scan stopped first: it writes to the cache
    m_index.close();
    updateSync(); // stops syncing the vault just closed
    updateShare();
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
    m_emptyState->show();
}

void MainWindow::openSingleFile(const QString &path)
{
    if (!maybeSaveAll())
        return;

    saveLayout();
    if (m_instance)
        m_instance->moveTo(SingleInstance::nameForVault(QString()));
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
    // A file that is no note (a PDF, an .ink page) is a module's to open: the scan that tells which module
    // runs after the first paint, so the open that paint queued - a path on the command line, the last
    // session's note - would find no owner for it. One directory listing, and only for such a file.
    const auto scanFor = [this](const QString &path) {
        if (!m_modulesScanned && !path.isEmpty() && !Vault::isNote(path))
            scanModulesNow();
    };
    switch (target.mode) {
    case OpenMode::Vault:
        openVault(target.vaultRoot);
        if (!target.filePath.isEmpty()) {
            scanFor(target.filePath);
            openNote(target.filePath);
        }
        break;
    case OpenMode::SingleFile:
        scanFor(target.filePath);
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
                    scanFor(candidate);
                    openNote(candidate);
                    break;
                }
            }
        } else {
            updateTitle();
            m_emptyState->show();
        }
        break;
    }
    }
    // After the first paint: openWhenShown queued this call.
    if (!m_painted && featureOn("file-recovery")) {
        // Old snapshots go once a session, with the window already up.
        const qint64 keep = qint64(m_settings.integer(QStringLiteral("fileRecovery.keepDays"))) * 24 * 3600 * 1000;
        Snapshots::prune(keep);
        PdfRecovery::prune(keep);
    }
    m_painted = true;
    startModuleScan(); // PERFORMANCE.md rule 5: a directory listing, off the UI thread, after the first frame
    syncPlugins();
    updateSync();
    updateShare();
    watchThemeFiles();
    updateSpelling();
}

// The scan runs once per process, after the first paint. Opening another vault does not rescan:
// modules are installed with the app, not with a vault (spec section 4, "When"), so the set found
// here serves every vault this window opens.
void MainWindow::startModuleScan()
{
    if (m_modulesScanned || m_modulesScanStarted)
        return;
    m_modulesScanStarted = true;
    const QStringList directories = ModuleRegistry::defaultDirectories();
    auto *watcher = new QFutureWatcher<QList<ModuleInfo>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        if (!m_modulesScanned) { // scanModulesNow() may have run meanwhile
            m_modules->setModules(watcher->result());
            onModulesScanned();
        }
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([directories] { return ModuleRegistry::scan(directories, isBuiltInSuffix); }));
}

void MainWindow::scanModulesNow()
{
    m_modules->setModules(ModuleRegistry::scan(ModuleRegistry::defaultDirectories(), isBuiltInSuffix));
    onModulesScanned();
}

void MainWindow::onModulesScanned()
{
    m_modulesScanned = true;
    // A vault that holds shares waits for this: the share module is found by this scan.
    const auto shareNow = qScopeGuard([this] { updateShare(); });
    if (m_modules->modules().isEmpty())
        return; // nothing installed: no feature row, no command, no rebuild - the base app pays nothing
    // The module set is known now: a tab that kept a file for want of its module tries it once more.
    if (m_active && m_active->notePath.isEmpty() && !m_active->view && !m_active->keptPath.isEmpty()) {
        m_active->pendingPath = std::exchange(m_active->keptPath, QString());
        activateLeaf(m_active);
    }
    bool featureRegistered = false;
    bool commandAdded = false;
    for (const ModuleInfo &module : m_modules->modules()) {
        if (!module.problem.isEmpty()) {
            qWarning("%s", qPrintable(module.problem));
            continue;
        }
        Settings::registerFeature(module.featureID, module.name, module.description);
        featureRegistered = true;
        for (const ModuleInfo::Command &moduleCommand : module.commands) {
            if (!command(moduleCommand.id)) { // a rescan does not duplicate
                addModuleCommand(moduleCommand.id, moduleCommand.title);
                commandAdded = true;
            }
        }
    }
    m_settings.adoptStored(); // a saved feature switch for a key registered just now (load() ran before the scan)
    m_settingsPage->setModules(m_moduleSettingRows, moduleProblems(), moduleFeatures());
    if (commandAdded)
        rebuildHotkeyCommands();
    if (featureRegistered)
        applyFeatures(); // greys a disabled module's commands by their id prefix, as for every feature
}

bool MainWindow::loadModule(const ModuleInfo &module, QString *error)
{
    if (m_modules->isLoaded(module.id))
        return true; // attached already: nothing new to pick up
    const bool ok = m_modules->ensureLoaded(module, m_moduleHost, error);
    if (ok)
        m_settings.adoptStored(); // the module's keys exist now; load() ran before them (once, not per row)
    m_settingsPage->setModules(m_moduleSettingRows, moduleProblems(), moduleFeatures()); // its rows, or its load failure
    return ok;
}

QAction *MainWindow::addModuleCommand(const QString &id, const QString &title)
{
    auto *action = new QAction(title, this);
    action->setObjectName(id);
    action->setData(QString());
    action->setShortcutContext(Qt::WindowShortcut);
    connect(action, &QAction::triggered, this, [this, id] {
        const std::optional<ModuleInfo> module = m_modules->moduleForCommand(id);
        if (!module)
            return;
        QString error;
        if (!loadModule(*module, &error)) {
            statusBar()->showMessage(error, 5000);
            return;
        }
        if (const auto run = m_moduleCommands.value(id))
            run();
    });
    addAction(action);
    for (const auto &popout : m_popouts)
        popout->addAction(action); // a window opened before the scan gets the command too, as for plugin commands
    m_commands << action;
    return action;
}

QList<QPair<QString, QString>> MainWindow::moduleProblems() const
{
    QList<QPair<QString, QString>> problems;
    for (const ModuleInfo &module : m_modules->modules()) {
        const QString notice = module.problem.isEmpty() ? m_modules->loadFailure(module.id) : module.problem;
        if (!notice.isEmpty())
            problems.append({module.id, notice});
    }
    return problems;
}

/// The feature ids of the installed modules that can load: what the Core plugins page offers a switch for when the
/// feature is a module's alone (the PDF viewer).
QStringList MainWindow::moduleFeatures() const
{
    QStringList features;
    for (const ModuleInfo &module : m_modules->modules())
        if (module.problem.isEmpty())
            features << module.featureID;
    return features;
}

QAction *MainWindow::command(const QString &id) const
{
    for (QAction *action : std::as_const(m_commands))
        if (action->objectName() == id)
            return action;
    return nullptr;
}

void MainWindow::openWhenShown(const LaunchTarget &target)
{
    m_pendingOpen = target;
    update(); // make sure a paint is coming even if nothing else asks for one
}

void MainWindow::openFromOtherInstance(const QString &argument)
{
    const LaunchTarget target = resolveLaunchTarget(argument);
    const bool sameVault = m_openMode == OpenMode::Vault && m_vault.root() == target.vaultRoot;
    if (!m_painted) {
        // Still starting up: that open would be overwritten by the one
        // queued for the first paint, so replace it instead.
        if (target.mode != OpenMode::Empty)
            openWhenShown(target);
    } else if ((target.mode == OpenMode::Vault && !sameVault)
               || (target.mode == OpenMode::SingleFile && !m_vault.isValid())) {
        openTarget(target);
    } else if (!target.filePath.isEmpty()) {
        openNote(target.filePath); // a tab in the open vault, loose files included
    }
    setWindowState(windowState() & ~Qt::WindowMinimized); // not showNormal(): that would un-maximize
    raise();
    activateWindow(); // on Wayland this needs the launcher's activation token, see SingleInstance
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
    // A tab that kept a file nothing could open keeps it only while it shows nothing: any open into it ends that.
    Leaf *const target = m_active;
    const QString kept = target ? std::exchange(target->keptPath, QString()) : QString();
    const auto keepIfStillEmpty = qScopeGuard([&] {
        if (!kept.isEmpty()
            && std::any_of(m_leaves.cbegin(), m_leaves.cend(), [target](const auto &open) { return open.get() == target; })
            && target->notePath.isEmpty() && !target->view)
            target->keptPath = kept;
    });
    // Any navigation - the tree, the switcher, a link, history - leaves the
    // settings page if it was open, even below, where a same-path or
    // not-a-note early return means nothing else here runs: a click behind an
    // overlay must not load invisibly behind it. A drawer picked from goes
    // away too, as on a phone.
    leaveSettings();
    closeDrawer();

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
    // A pinned tab keeps its note: what would replace it opens beside it.
    // Back and forward stay in the tab.
    if (m_active->pinned && !m_active->notePath.isEmpty() && source != NavSource::History
        && (isViewFile(path) || Vault::isNote(path) || Vault::isEncryptedNote(path) || isNotebookFile(path))) {
        Leaf *pinned = m_active;
        newTab();
        Leaf *fresh = m_active;
        if (openNote(path, source, fragment))
            return true;
        // Not opened (a cancelled password, an unreadable file): no blank tab left behind.
        if (fresh->notePath.isEmpty() && fresh->pendingPath.isEmpty() && fresh->keptPath.isEmpty()
            && std::any_of(m_leaves.cbegin(), m_leaves.cend(), [fresh](const auto &open) { return open.get() == fresh; })) {
            dropLeaf(fresh);
            activateLeaf(pinned);
        }
        return false;
    }
    if (isViewFile(path)) {
        if (!maybeLeave())
            return false;
        return openView(path, source, fragment);
    }
    if (!Vault::isNote(path) && !Vault::isEncryptedNote(path) && !isNotebookFile(path)) {
        if (source != NavSource::History) {
            statusBar()->showMessage(path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)
                                         ? pdfViewerProblem()
                                         : tr("%1 is not a note - editors for other file types come later")
                                               .arg(QFileInfo(path).fileName()),
                                     4000);
        }
        return false;
    }
    if (!maybeLeave())
        return false;

    // Another tab showing it already holds the document, runs not yet saved included: the file is not the truth
    // for it, so its outputs are not read in again over them.
    const bool shownElsewhere = std::any_of(m_leaves.cbegin(), m_leaves.cend(), [&](const auto &other) {
        return other.get() != m_active && !other->view && other->notePath == path;
    });
    Note note;
    QString error;
    if (!readNote(path, note, &error, !shownElsewhere)) {
        // Replaying history over a note that has since been deleted or
        // renamed is not an error, just a dead end to skip past; a password
        // prompt cancelled is not one either.
        if (source != NavSource::History && !error.isEmpty())
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
    showNotebookPictures(m_active->editor, path); // a notebook's attachments, else none
    // Open in another tab already: that document, unsaved edits and all,
    // not the older bytes on disk.
    const QList<Leaf *> others = viewsOfSameNote(m_active);
    m_active->editor->setNoteText(others.isEmpty() ? m_active->note.text : others.constFirst()->editor->noteText());
    if (!others.isEmpty())
        m_active->editor->setModified(others.constFirst()->editor->isModified());
    // Caret first, folds after: setCursorOffset opens the folds around the
    // caret, which would undo a remembered fold the caret sits under.
    const auto position = m_settings.notePosition(path);
    if (position)
        m_active->editor->setCursorOffset(qMin(position->cursor, qsizetype(m_active->note.text.size())));
    if (!others.isEmpty())
        m_active->editor->setFoldState(others.constFirst()->editor->foldState());
    else if (position)
        m_active->editor->setFoldState(position->folds);
    if (position)
        m_active->editor->restoreScroll(position->top, position->scroll);
#ifdef NOTEAPPC_HAVE_SHARE
    syncShareViews(); // a shared note takes its document's text here, caret and folds already in place
#endif
    m_active->editor->setFocus();

    const QString title = QFileInfo(path).completeBaseName();
    m_active->inlineTitle->setText(title);
    m_active->inlineTitle->setVisible(m_settings.boolean(QStringLiteral("appearance.showInlineTitle")));
    syncTabs(m_active->group);

    showNoteInPanes(path);
    watchOpenNotes();
    m_settings.noteOpened(m_vault.isValid() ? m_vault.root() : QString(), path);
    updateTitle();
    updateWordCount();
    if (note.lossy)
        statusBar()->showMessage(tr("%1 is not valid UTF-8: invalid bytes are shown as U+FFFD and the note is not "
                                    "saved unless you save it by hand")
                                     .arg(QFileInfo(path).fileName()),
                                 10000);

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
            const QString path = !leaf->notePath.isEmpty() ? leaf->notePath
                                : !leaf->pendingPath.isEmpty() ? leaf->pendingPath
                                : leaf->view ? QString() : leaf->keptPath;
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
            QJsonObject tab{{QStringLiteral("id"), id},
                            {QStringLiteral("type"), QStringLiteral("leaf")},
                            {QStringLiteral("state"), view}};
            if (leaf->pinned)
                tab.insert(QStringLiteral("pinned"), true); // Obsidian's own key
            tabs.append(tab);
        }
        return {{QStringLiteral("id"), nextID()},
                {QStringLiteral("type"), QStringLiteral("tabs")},
                {QStringLiteral("children"), tabs},
                {QStringLiteral("currentTab"), currentTab}};
    };
    QJsonObject layout{{QStringLiteral("main"), node(m_workspace)}};
    // Obsidian's pop-outs: floating > window > the split (or tabs) it holds.
    // Only when there are some, so a layout without them is as it always was.
    // A detached sidebar or pane is a window holding one leaf of its view type.
    QJsonArray windows;
    for (const auto &popout : m_popouts) {
        const QWidget *held = popout->content();
        if (!held)
            continue;
        QJsonObject content;
        if (popout->kind() == PopoutWindow::Kind::Leaves) {
            content = node(popout->content());
        } else {
            const QString viewType = held == m_sidebar        ? QStringLiteral("file-explorer")
                                   : held == m_rightSidebar   ? QStringLiteral("right-sidebar")
                                                              : held->property("viewType").toString();
            if (viewType.isEmpty())
                continue; // a pane with no view type is not remembered
            const QJsonObject leaf{{QStringLiteral("id"), nextID()},
                                   {QStringLiteral("type"), QStringLiteral("leaf")},
                                   {QStringLiteral("state"), QJsonObject{{QStringLiteral("type"), viewType},
                                                                         {QStringLiteral("state"), QJsonObject()}}}};
            content = {{QStringLiteral("id"), nextID()},
                       {QStringLiteral("type"), QStringLiteral("tabs")},
                       {QStringLiteral("children"), QJsonArray{leaf}},
                       {QStringLiteral("currentTab"), 0}};
        }
        windows.append(QJsonObject{{QStringLiteral("id"), nextID()},
                                   {QStringLiteral("type"), QStringLiteral("window")},
                                   {QStringLiteral("children"), QJsonArray{content}},
                                   {QStringLiteral("direction"), QStringLiteral("vertical")},
                                   {QStringLiteral("x"), popout->x()},
                                   {QStringLiteral("y"), popout->y()},
                                   {QStringLiteral("width"), popout->width()},
                                   {QStringLiteral("height"), popout->height()},
                                   {QStringLiteral("maximized"), popout->isMaximized()},
                                   {QStringLiteral("zoom"), 0}});
    }
    if (!windows.isEmpty())
        layout.insert(QStringLiteral("floating"), QJsonObject{{QStringLiteral("id"), nextID()},
                                                              {QStringLiteral("type"), QStringLiteral("floating")},
                                                              {QStringLiteral("children"), windows}});
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
    dockAll();        // a sidebar or pane in a window stays only if this layout says so
    TabGroup *spareGroup = m_active->group;
    Leaf *spareLeaf = m_active;
    QHash<QString, Leaf *> byID;
    QList<std::pair<TabGroup *, int>> shown;
    // What a tab opens, as an absolute path; empty when it opens nothing we can show.
    const auto tabFile = [this](const QJsonObject &tab) {
        const QJsonObject view = tab.value(QStringLiteral("state")).toObject();
        const QString file =
            view.value(QStringLiteral("state")).toObject().value(QStringLiteral("file")).toString();
        const QString kind = view.value(QStringLiteral("type")).toString();
        const bool opens = kind == QLatin1String("markdown") || kind == QLatin1String("canvas")
                        || kind == QLatin1String("bases") || kind == QLatin1String("epub")
                        || kind == QLatin1String("pdf") || kind == QLatin1String("image")
                        || kind == QLatin1String("audio") || kind == QLatin1String("video");
        return opens && !file.isEmpty() && QFileInfo::exists(m_vault.absolute(file)) ? m_vault.absolute(file)
                                                                                      : QString();
    };
    QSplitter *rootSplit = m_workspace; // the split a node with no parent becomes
    bool floating = false;              // a pop-out's tabs: only those that open something

    const std::function<void(const QJsonObject &, QSplitter *)> build =
        [&](const QJsonObject &node, QSplitter *parent) {
            const QString type = node.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("split")) {
                QSplitter *split = parent ? new QSplitter : rootSplit;
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
                if (floating && split != rootSplit && split->count() == 0) {
                    delete split; // nothing in it could be restored
                    return;
                }
                if (!sizes.contains(0) && sizes.size() == split->count())
                    split->setSizes(sizes);
                return;
            }
            if (type != QLatin1String("tabs") || !parent)
                return;
            QList<QJsonObject> tabs;
            for (const QJsonValue &value : node.value(QStringLiteral("children")).toArray()) {
                const QJsonObject tab = value.toObject();
                if (tab.value(QStringLiteral("type")).toString() == QLatin1String("leaf")
                    && (!floating || !tabFile(tab).isEmpty()))
                    tabs << tab;
            }
            if (tabs.isEmpty() && floating)
                return;
            TabGroup *group = spareGroup ? std::exchange(spareGroup, nullptr) : newGroup();
            parent->addWidget(group->widget);
            for (const QJsonObject &tab : std::as_const(tabs)) {
                Leaf *leaf = spareLeaf ? std::exchange(spareLeaf, nullptr) : newLeaf(group);
                leaf->pendingPath = tabFile(tab);
                leaf->pinned = tab.value(QStringLiteral("pinned")).toBool() && !leaf->pendingPath.isEmpty();
                byID.insert(tab.value(QStringLiteral("id")).toString(), leaf);
            }
            shown << std::pair{group, node.value(QStringLiteral("currentTab")).toInt()};
            syncTabs(group);
        };
    build(main, nullptr);

    // Obsidian's pop-outs. Sized, never moved: where a window goes is the
    // compositor's to say. A window nothing could be restored into is not made.
    floating = true;
    spareGroup = nullptr;
    spareLeaf = nullptr;
    const QJsonArray windows = layout.value(QStringLiteral("floating")).toObject().value(QStringLiteral("children")).toArray();
    // The sidebar or pane a view type names, in a window of `size` (the default when empty).
    // An unknown type, or a pane its feature has switched off, names nothing.
    const auto detachView = [this](const QString &viewType, const QSize &size) {
        QWidget *view = viewType == QLatin1String("file-explorer")  ? m_sidebar
                      : viewType == QLatin1String("right-sidebar")  ? m_rightSidebar
                                                                    : nullptr;
        if (!view && !viewType.isEmpty()) {
            for (QAbstractButton *tab : m_rightTabs->buttons()) {
                auto *pane = qobject_cast<QWidget *>(tab->property("pane").value<QObject *>());
                if (pane && pane->property("viewType").toString() == viewType)
                    view = pane;
            }
        }
        if (!view)
            return;
        if (view == m_sidebar || view == m_rightSidebar)
            detachSidebar(view);
        else
            detachPane(view);
        if (PopoutWindow *popout = popoutHolding(view); popout && !size.isEmpty())
            popout->resize(size);
    };
    for (const QJsonValue &value : m_popoutsOn ? windows : QJsonArray()) {
        const QJsonObject window = value.toObject();
        const QJsonArray inside = window.value(QStringLiteral("children")).toArray();
        const QJsonObject content = inside.isEmpty() ? QJsonObject() : inside.first().toObject();
        const QString contentType = content.value(QStringLiteral("type")).toString();
        if (window.value(QStringLiteral("type")).toString() != QLatin1String("window")
            || (contentType != QLatin1String("split") && contentType != QLatin1String("tabs")))
            continue;
        // A sidebar or pane view: what it names leaves the main window again.
        if (contentType == QLatin1String("tabs")) {
            const QSize size(window.value(QStringLiteral("width")).toInt(), window.value(QStringLiteral("height")).toInt());
            for (const QJsonValue &leaf : content.value(QStringLiteral("children")).toArray())
                detachView(leaf.toObject().value(QStringLiteral("state")).toObject().value(QStringLiteral("type")).toString(), size);
        }
        auto *root = new QSplitter(Qt::Horizontal);
        root->setObjectName(QStringLiteral("popoutWorkspace"));
        root->setChildrenCollapsible(false);
        rootSplit = root;
        build(content, contentType == QLatin1String("split") ? nullptr : root);
        if (root->count() == 0) {
            delete root;
            continue;
        }
        auto *popout = new PopoutWindow(PopoutWindow::Kind::Leaves, root);
        registerPopout(popout);
        const QSize wanted(window.value(QStringLiteral("width")).toInt(), window.value(QStringLiteral("height")).toInt());
        popout->resize(wanted.isEmpty() ? size() * 0.6 : wanted);
        popout->show();
    }

    // A tab whose file only a module can open (a PDF, an .ink page): the scan that finds the module runs after the
    // first paint, later than this, so the open below would find no owner. One directory listing, and only then.
    if (!m_modulesScanned && std::ranges::any_of(m_leaves, [](const auto &leaf) {
            const QString &path = leaf->pendingPath;
            return !path.isEmpty() && !Vault::isNote(path) && !isBuiltInSuffix(QFileInfo(path).suffix().toLower());
        }))
        scanModulesNow();
    // Each split's own tab first, the focused one last so it ends up active.
    for (const auto &[group, index] : std::as_const(shown)) {
        if (!group->leaves.isEmpty())
            activateLeaf(group->leaves.at(qBound(0, index, int(group->leaves.size()) - 1)));
    }
    if (Leaf *active = byID.value(layout.value(QStringLiteral("active")).toString()))
        activateLeaf(active);
    for (const auto &group : m_groups)
        syncTabs(group.get());

    // A sidebar in a window of its own is shown there, whatever "collapsed" says.
    if (!popoutHolding(m_sidebar))
        m_sidebar->setVisible(!layout.value(QStringLiteral("left")).toObject().value(QStringLiteral("collapsed")).toBool());
    if (!popoutHolding(m_rightSidebar))
        m_rightSidebar->setVisible(
            !layout.value(QStringLiteral("right")).toObject().value(QStringLiteral("collapsed")).toBool());
    return true;
}

void MainWindow::saveLayout()
{
    m_layoutSave.stop();
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
    // Nothing is opened here: libgit2 (OpenSSL, +6 MB) is set up by the first save, on the pool.
    m_noteHistory = std::make_shared<NoteHistory>(root);
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
    if (wanted == m_syncFor && m_sync)
        return;
    m_sync.reset();
    m_syncFor.clear();
    m_syncStatus->setVisible(configured);
    if (!configured)
        return;
    // The engine starts even with no password: it looks for this device's
    // keys in the keyring first and only reports NeedsPassword when there
    // are none. A password goes to its thread once and is gone from here:
    // it keeps the keys it derives, not the password.
    auto password = std::make_shared<QString>(std::exchange(m_syncPassword, QString()));
    m_sync = std::make_shared<SyncEngine>();
    m_syncFor = wanted;
    connect(m_sync.get(), &SyncEngine::statusChanged, this,
            [this](SyncEngine::Status status, const QString &message) {
                const QString state = status == SyncEngine::Status::Idle          ? tr("synced")
                                      : status == SyncEngine::Status::Error      ? tr("error")
                                      : status == SyncEngine::Status::NeedsPassword ? tr("not signed in")
                                      : status == SyncEngine::Status::WaitingForApproval ? tr("waiting for approval")
                                                                                          : tr("syncing");
                m_syncNeedsPassword = status == SyncEngine::Status::NeedsPassword;
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
    m_syncNeedsPassword = false;
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

bool MainWindow::vaultTrusted() const
{
    return m_vault.isValid() && m_settings.isVaultTrusted(m_vault.root());
}

void MainWindow::setVaultTrusted(bool trusted)
{
    if (!m_vault.isValid())
        return;
    m_settings.setVaultTrusted(m_vault.root(), trusted);
    m_settings.save();
    applyHotkeys(); // its hotkeys count (or stop counting) from now on; its feature switches at the next open
    statusBar()->showMessage(trusted ? tr("This vault is trusted: its hotkeys apply, and code still runs only in the sandbox")
                                     : tr("This vault is not trusted"),
                             5000);
}

void MainWindow::setOpenApprovalForTests(std::function<bool(const QString &, const QString &)> approve)
{
    m_openApproval = std::move(approve);
}

namespace {

/// Whether the desktop would run `info` rather than show it: an executable bit, a launcher, a script or a
/// program by its suffix, its first bytes (an ELF, a Windows program, a shebang) or its type.
bool desktopWouldRun(const QFileInfo &info)
{
    static const QSet<QString> suffixes = {
        QStringLiteral("desktop"), QStringLiteral("exe"),   QStringLiteral("msi"),   QStringLiteral("jar"),
        QStringLiteral("appimage"), QStringLiteral("sh"),   QStringLiteral("bash"),  QStringLiteral("zsh"),
        QStringLiteral("fish"),    QStringLiteral("bat"),   QStringLiteral("cmd"),   QStringLiteral("com"),
        QStringLiteral("scr"),     QStringLiteral("lnk"),   QStringLiteral("url"),   QStringLiteral("jnlp"),
        QStringLiteral("py"),      QStringLiteral("pl"),    QStringLiteral("rb"),    QStringLiteral("js"),
        QStringLiteral("mjs"),     QStringLiteral("vbs"),   QStringLiteral("vbe"),   QStringLiteral("wsf"),
        QStringLiteral("ps1"),     QStringLiteral("run"),   QStringLiteral("bin"),   QStringLiteral("so"),
        QStringLiteral("dll"),     QStringLiteral("hta"),   QStringLiteral("reg"),   QStringLiteral("deb"),
        QStringLiteral("rpm"),     QStringLiteral("apk"),   QStringLiteral("pif"),   QStringLiteral("cpl"),
        QStringLiteral("application"), QStringLiteral("service"), QStringLiteral("flatpakref"),
        QStringLiteral("flatpak"), QStringLiteral("snap"),  QStringLiteral("workflow"),
        QStringLiteral("pyw"),     QStringLiteral("pyz"),   QStringLiteral("phar"),  QStringLiteral("lua"),
        QStringLiteral("tcl"),     QStringLiteral("command"), QStringLiteral("tool"),
        // Office files with macros.
        QStringLiteral("docm"),    QStringLiteral("xlsm"),  QStringLiteral("pptm"),
    };
    if (info.isExecutable() || suffixes.contains(info.suffix().toLower()))
        return true;
    QFile file(info.canonicalFilePath());
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray head = file.read(4);
        if (head.startsWith("\x7f" "ELF") || head.startsWith("MZ") || head.startsWith("#!"))
            return true;
    }
    const QMimeType type = QMimeDatabase().mimeTypeForFile(info);
    for (const char *runnable : {"application/x-executable", "application/x-sharedlib", "application/x-desktop",
                                 "application/x-shellscript", "application/x-ms-dos-executable",
                                 "application/vnd.microsoft.portable-executable", "application/x-java-archive",
                                 "application/x-executable-script"}) {
        if (type.inherits(QLatin1String(runnable)))
            return true;
    }
    return false;
}

} // namespace

/// A file of the vault to the desktop's default app. Never what the desktop would run (an executable, a
/// launcher, a script); and unless the vault is trusted (or `ask` is false: the person chose this file
/// themselves) only after a yes that names the file and its type.
bool MainWindow::openFileExternally(const QString &path, bool ask)
{
    const QFileInfo info(path);
    if (!info.isFile())
        return false;
    if (desktopWouldRun(info)) {
        statusBar()->showMessage(tr("%1 is a program or a launcher: it is not opened from here").arg(info.fileName()), 6000);
        return false;
    }
    if (ask && !vaultTrusted()) {
        const QString type = QMimeDatabase().mimeTypeForFile(info).comment();
        bool yes = false;
        if (m_openApproval) {
            yes = m_openApproval(info.fileName(), type);
        } else {
            QMessageBox box(QMessageBox::Question, tr("Open file"),
                            tr("Open \"%1\" (%2) with the default app?\n\nThis vault is not trusted: the file may "
                               "have been made to harm the app that opens it.")
                                .arg(info.fileName(), type),
                            QMessageBox::NoButton, this);
            box.setTextFormat(Qt::PlainText);
            QPushButton *open = box.addButton(tr("Open"), QMessageBox::AcceptRole);
            box.setDefaultButton(box.addButton(tr("Cancel"), QMessageBox::RejectRole));
            box.exec();
            yes = box.clickedButton() == open;
        }
        if (!yes)
            return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(info.canonicalFilePath()));
}

void MainWindow::setRunApprovalForTests(std::function<bool(const QString &)> approve)
{
    m_runApproval = std::move(approve);
}

/// A block of a vault that is not trusted asks before it first runs: what runs is the person's decision, not the
/// vault's. "Trust this vault" ends the asking for this vault; either way the block runs in the sandbox.
bool MainWindow::allowRun(const QString &language, const QString &source)
{
    if (vaultTrusted())
        return true;
    const QByteArray key = QCryptographicHash::hash((language + QChar(0) + source).toUtf8(), QCryptographicHash::Sha256);
    if (m_runsAllowed.contains(key))
        return true;
    QString shown = source.left(600);
    if (source.size() > shown.size())
        shown += QStringLiteral("\n...");
    bool allowed = false;
    if (m_runApproval) {
        allowed = m_runApproval(shown);
    } else {
        QMessageBox ask(QMessageBox::Warning, tr("Run code"),
                        tr("This vault is not trusted, so this %1 block may have been written by anyone.\n\n%2\n\n"
                           "It runs in the sandbox: no network, none of your files. Run it?")
                            .arg(language, shown),
                        QMessageBox::NoButton, this);
        ask.setTextFormat(Qt::PlainText); // the code is the vault's
        QPushButton *run = ask.addButton(tr("Run"), QMessageBox::AcceptRole);
        QPushButton *trust = m_vault.isValid() ? ask.addButton(tr("Trust this vault"), QMessageBox::AcceptRole) : nullptr;
        ask.setDefaultButton(ask.addButton(tr("Cancel"), QMessageBox::RejectRole));
        ask.exec();
        allowed = ask.clickedButton() == run || (trust && ask.clickedButton() == trust);
        if (allowed && trust && ask.clickedButton() == trust)
            setVaultTrusted(true);
    }
    if (allowed)
        m_runsAllowed.insert(key);
    return allowed;
}

#ifdef NOTEAPPC_HAVE_SHARE
void MainWindow::setShareConfigForTests(ShareEngineBase::Config config)
{
    m_shareConfig = std::move(config);
}

void MainWindow::setShareApprovalForTests(std::function<bool(const QString &, const QString &)> approve)
{
    m_shareApproval = std::move(approve);
}

void MainWindow::setSharedRunApprovalForTests(std::function<bool(const QString &)> approve)
{
    m_sharedRunApproval = std::move(approve);
}

/// A shared note's code may be anyone's in the share: each block asks once
/// before it first runs here, naming who could have written it.
bool MainWindow::allowSharedRun(const MarkdownEditor *editor, const QString &language, const QString &source)
{
    const ShareSession *session = sessionFor(notePathOf(editor));
    const QByteArray key = QCryptographicHash::hash((language + QChar(0) + source).toUtf8(), QCryptographicHash::Sha256);
    if (m_sharedRunsAllowed.contains(key))
        return true;
    QStringList names;
    if (session && m_share) {
        for (const auto &member : m_share->members(session->shareID()))
            if (member.first != m_share->publicKey())
                names.append(member.second);
    }
    if (names.isEmpty())
        names.append(tr("(the share's members; sharing is not running)"));
    const QString members = names.join(QLatin1Char('\n'));
    bool allowed = false;
    if (m_sharedRunApproval) {
        allowed = m_sharedRunApproval(members);
    } else {
        QMessageBox ask(QMessageBox::Warning, tr("Run shared code"),
                        tr("This note is shared, so this block may have been written by:\n\n%1\n\n"
                           "It runs in the sandbox. Run it?")
                            .arg(members),
                        QMessageBox::NoButton, this);
        ask.setTextFormat(Qt::PlainText); // the names come from other devices
        QPushButton *run = ask.addButton(tr("Run"), QMessageBox::AcceptRole);
        ask.setDefaultButton(ask.addButton(tr("Cancel"), QMessageBox::RejectRole));
        ask.exec();
        allowed = ask.clickedButton() == run;
    }
    if (allowed)
        m_sharedRunsAllowed.insert(key);
    return allowed;
}
#endif

#ifdef NOTEAPPC_HAVE_SHARE
QList<ShareRecord> MainWindow::storedShares() const
{
    if (!m_vault.isValid())
        return {};
    return ShareStore(m_shareConfig.storeDirectory.isEmpty() ? ShareStore::directoryFor(m_vault.root())
                                                             : m_shareConfig.storeDirectory)
        .all();
}
#endif

void MainWindow::updateShare()
{
#ifdef NOTEAPPC_HAVE_SHARE
    const bool wanted = m_painted && m_openMode == OpenMode::Vault && m_vault.isValid() && featureOn("share");
    const QString root = wanted ? m_vault.root() : QString();
    if (!wanted)
        m_shareDemanded = false;
    if (root == m_shareFor && (m_share != nullptr || !m_shareDemanded))
        return; // settled for this vault: an engine, or nothing to build one for
    qDeleteAll(m_sessions);
    m_sessions.clear();
    m_share.reset(); // nothing built, nothing listening, with the feature off
    m_shareFor.clear();
    updateShareStatus();
    if (!wanted)
        return;
    // The engine is the notolith-share module, which maps protobuf, Abseil and libdatachannel: it is loaded
    // for a vault that holds shares to resume, or when a command asks (ensureShare), never for a vault that
    // merely has the feature on.
    if (!m_shareDemanded && storedShares().isEmpty()) {
        m_shareFor = root;
        return;
    }
    if (!m_modulesScanned)
        return; // onModulesScanned() runs this again; m_shareFor stays empty so that call is not taken for settled
    m_shareFor = root;
    m_shareDemanded = false;
    m_shareProblem.clear();
    const std::optional<ModuleInfo> module = m_modules->moduleForFeature(QStringLiteral("share"));
    if (!module)
        m_shareProblem = tr("Sharing needs the notolith-share package");
    else if (loadModule(*module, &m_shareProblem) && !m_makeShare)
        m_shareProblem = tr("The notolith-share module did not offer a sharing engine");
    if (!m_shareProblem.isEmpty()) {
        statusBar()->showMessage(m_shareProblem, 8000);
        return;
    }
    m_share.reset(m_makeShare(root, m_shareConfig, this));
    connect(m_share.get(), &ShareEngineBase::joinRequested, this,
            [this](const QByteArray &shareID, const QByteArray &joiner, const QString &name, const QString &safetyNumber) {
                bool approved = false;
                if (m_shareApproval) {
                    approved = m_shareApproval(name, safetyNumber);
                } else {
                    QMessageBox ask(QMessageBox::Question, tr("Share"),
                                    tr("%1 asks to edit a note of yours. Its code is:\n\n%2\n\n"
                                       "Approve it only if that device shows exactly this code.")
                                        .arg(name, safetyNumber),
                                    QMessageBox::NoButton, this);
                    ask.setTextFormat(Qt::PlainText); // the name comes from the other device
                    QPushButton *approve = ask.addButton(tr("Approve"), QMessageBox::AcceptRole);
                    ask.setDefaultButton(ask.addButton(tr("Not now"), QMessageBox::RejectRole));
                    ask.exec();
                    approved = ask.clickedButton() == approve;
                }
                if (!m_share)
                    return;
                if (approved)
                    m_share->approve(shareID, joiner);
                else
                    m_share->refuse(shareID, joiner);
            });
    connect(m_share.get(), &ShareEngineBase::joined, this, &MainWindow::placeJoinedNote);
    connect(m_share.get(), &ShareEngineBase::peersChanged, this, &MainWindow::updateShareStatus);
    connect(m_share.get(), &ShareEngineBase::membersChanged, this, &MainWindow::updateShareStatus);
    connect(m_share.get(), &ShareEngineBase::presenceChanged, this, &MainWindow::updateShareStatus); // here through the owner
    connect(m_share.get(), &ShareEngineBase::failed, this,
            [this](const QByteArray &, const QString &message) { statusBar()->showMessage(message, 8000); });
    if (m_share->hasStoredShares())
        startShare();
#endif
}

#ifdef NOTEAPPC_HAVE_SHARE
bool MainWindow::ensureShare(QString *error)
{
    if (m_share)
        return true;
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!m_painted || m_openMode != OpenMode::Vault || !m_vault.isValid() || !featureOn("share"))
        return fail(tr("Turn on \"Share a note\" in Settings, Core plugins"));
    m_shareDemanded = true;
    if (!m_modulesScanned)
        scanModulesNow(); // a command asked: the one directory listing now, not after the next paint
    updateShare();
    m_shareDemanded = false;
    return m_share ? true : fail(m_shareProblem);
}

bool MainWindow::startShare()
{
    if (!m_share)
        return false;
    if (!m_share->start()) {
        statusBar()->showMessage(tr("Sharing could not start: %1").arg(m_share->error()), 8000);
        return false;
    }
    for (const ShareRecord &record : m_share->shares())
        shareSession(record.shareID);
    syncShareViews();
    updateShareStatus();
    return true;
}

ShareSession *MainWindow::shareSession(const QByteArray &shareID)
{
    if (ShareSession *known = m_sessions.value(shareID))
        return known;
    auto *session = new ShareSession(m_share.get(), shareID, this);
    connect(session, &ShareSession::remoteApplied, this, [this] {
        if (m_settings.boolean(QStringLiteral("files.autosave")))
            armAutosave(); // the views changed with nobody typing: edited() was not emitted
    });
    m_sessions.insert(shareID, session);
    return session;
}

ShareSession *MainWindow::sessionFor(const QString &absolutePath) const
{
    if (m_sessions.isEmpty() || !m_share || !m_vault.contains(absolutePath))
        return nullptr;
    const auto record = m_share->shareFor(m_vault.relative(absolutePath));
    return record ? m_sessions.value(record->shareID) : nullptr;
}

void MainWindow::syncShareViews()
{
    if (m_sessions.isEmpty())
        return;
    QHash<ShareSession *, QList<Leaf *>> shown;
    for (const auto &leaf : m_leaves) {
        if (!leaf->view && !leaf->notePath.isEmpty()) {
            if (ShareSession *session = sessionFor(leaf->notePath))
                shown[session].append(leaf.get());
        }
    }
    for (ShareSession *session : std::as_const(m_sessions)) {
        QList<MarkdownEditor *> keep;
        for (const Leaf *leaf : shown.value(session))
            keep.append(leaf->editor);
        session->retain(keep); // first: a tab that left must not count as showing the note
    }
    for (auto it = shown.cbegin(); it != shown.cend(); ++it) {
        for (Leaf *leaf : it.value()) {
            if (it.key()->attach(leaf->editor, leaf->note.text))
                continue;
            // The file changed while the note was not open here, in a way the
            // shared text could not take in: the shared text wins, the file's
            // version goes to file recovery.
            const bool kept = featureOn("file-recovery") && Snapshots::record(leaf->notePath, leaf->note.text, 0);
            statusBar()->showMessage(
                kept ? tr("%1 changed on disk in a way that clashes with the shared text: the shared text is kept, "
                          "the other version is in file recovery")
                           .arg(QFileInfo(leaf->notePath).fileName())
                     : tr("%1 changed on disk in a way that clashes with the shared text: the shared text is kept")
                           .arg(QFileInfo(leaf->notePath).fileName()),
                8000);
        }
    }
}

QString MainWindow::shareNote(QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    QString engineError;
    if (!ensureShare(&engineError))
        return fail(engineError);
    Leaf *leaf = m_active;
    if (leaf && Vault::isEncryptedNote(leaf->notePath))
        return fail(tr("An encrypted note cannot be shared"));
    if (!leaf || leaf->view || leaf->notePath.isEmpty() || !m_vault.contains(leaf->notePath)
        || !Vault::isNote(leaf->notePath))
        return fail(tr("Open a note of this vault to share it"));
    if (leaf->note.lossy)
        return fail(tr("This note is not valid UTF-8 and cannot be shared"));
    // The file and the shared text start out the same: the note is saved first.
    if (isModified(leaf) && !saveLeaf(leaf, false))
        return fail(tr("The note could not be saved"));
    QString why;
    const auto created = m_share->create(m_vault.relative(leaf->notePath), QFileInfo(leaf->notePath).completeBaseName(),
                                         leaf->editor->noteText(), &why);
    if (!created)
        return fail(why);
    startShare(); // a session for it, and for any share the store gave back
    return created->inviteCode;
}

bool MainWindow::joinShare(const QString &code, QString *error)
{
    if (!ensureShare(error))
        return false;
    if (!m_share->join(code, QString(), error))
        return false;
    startShare();
    statusBar()->showMessage(tr("Asked to join: the note appears once its owner approves this device"), 8000);
    return true;
}

void MainWindow::stopSharingNote()
{
    if (const ShareSession *session = m_active ? sessionFor(m_active->notePath) : nullptr)
        stopShare(session->shareID());
}

void MainWindow::stopShare(QByteArray shareID) // a copy: a caller's may be the session's own, deleted below
{
    ShareSession *session = m_sessions.take(shareID);
    if (!session || !m_share)
        return;
    m_share->clearCaret(shareID);
    delete session; // stops following the editors
    m_share->stop(shareID);
    updateShareStatus();
    statusBar()->showMessage(tr("Stopped sharing this note on this device; the others keep it"), 5000);
}

void MainWindow::showShareDialog(const QByteArray &shareID, const QString &inviteCode)
{
    if (!m_share)
        return;
    auto *dialog = new ShareDialog(m_share.get(), shareID, inviteCode, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this, shareID](int result) {
        if (result == ShareDialog::StopSharing)
            stopShare(shareID);
    });
    dialog->open();
}

void MainWindow::placeJoinedNote(const QByteArray &shareID, const QString &title)
{
    if (!m_share || !m_share->doc(shareID))
        return;
    // The title is the owner's string: a file name of its own making, never a path.
    const QString name = ShareSession::fileNameFor(title, shareID);
    QString relative;
    for (int n = 1;; ++n) {
        relative = QStringLiteral("Shared/") + name + (n == 1 ? QString() : QStringLiteral(" (%1)").arg(n))
                   + QStringLiteral(".md");
        const QFileInfo taken(m_vault.absolute(relative));
        if (!taken.exists() && !taken.isSymLink())
            break;
    }
    Note note;
    note.text = m_share->doc(shareID)->text();
    QString error;
    if (!m_vault.isConfinedPath(relative) || !m_vault.writeNoteNoFollow(relative, note, &error)) {
        statusBar()->showMessage(tr("Joined, but the note could not be placed in Shared/: %1").arg(error), 10000);
        return;
    }
    m_share->setPath(shareID, relative);
    shareSession(shareID);
    const QString path = m_vault.absolute(relative);
    m_indexer->reindex(path);
    openNote(path);
}
#endif

void MainWindow::recordHistory(const QString &absolutePath, const QString &text)
{
    if (Vault::isEncryptedNote(absolutePath))
        return; // its text never reaches the disk, git history included
#ifdef NOTEAPPC_HAVE_HISTORY
    if (!m_noteHistory || !m_vault.contains(absolutePath))
        return;
    const std::shared_ptr<NoteHistory> history = m_noteHistory;
    const QString relative = m_vault.relative(absolutePath);
    // Now and then, fold the loose objects into a pack, where git keeps each
    // version as a delta: never on a save's own path, and only once there
    // are enough to be worth it.
    const bool maybePack = ++m_historyRecords % 256 == 0;
    m_historyPool.start([this, history, relative, text, maybePack] {
        if (!history->isOpen()) { // an existing repository that would not open: say so once, and stop recording
            const QString error = history->error();
            QMetaObject::invokeMethod(this, [this, history, error] {
                if (m_noteHistory != history)
                    return;
                statusBar()->showMessage(tr("Note history is off: %1").arg(error), 6000);
                m_noteHistory.reset();
            });
            return;
        }
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

QString MainWindow::pdfViewerProblem() const
{
    for (const ModuleInfo &module : m_modules->modules()) {
        if (module.featureID != QLatin1String("pdf-viewer"))
            continue;
        if (!module.problem.isEmpty())
            return module.problem; // built for another Notolith, say
        if (!featureOn("pdf-viewer"))
            return tr("The PDF viewer is switched off in Settings, Core plugins");
        return tr("The PDF viewer could not be loaded");
    }
    return tr("PDF support is not installed (the notolith-pdfview package)");
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
    const QString media = mediaKindOf(path);
    if (media == QLatin1String("image") || (!media.isEmpty() && featureOn("media-embeds")))
        return true;
    return m_modules && m_modules->moduleForSuffix(path).has_value(); // after every built-in kind, never before
}

bool MainWindow::openModuleView(const QString &path, QString *error, QWidget **view, const QString &fragment)
{
    const std::optional<ModuleInfo> module = m_modules ? m_modules->moduleForSuffix(path) : std::nullopt;
    if (!module)
        return false;
    if (!loadModule(*module, error))
        return false;
    const ModuleHost::ViewFactory factory = m_moduleViews.value(QFileInfo(path).suffix().toLower());
    if (!factory) {
        *error = tr("Module %1 registered no view for %2").arg(module->id, path);
        return false;
    }
    ModuleView *moduleView = factory(path, m_active->area);
    if (!moduleView)
        return false;
    moduleView->setTheme(m_theme);
    // Connected before load(): what a view says while it loads (a newer file opened read-only) is shown.
    // A view whose load fails is deleted below, which drops these connections.
    Leaf *leaf = m_active;
    connect(moduleView, &ModuleView::modificationChanged, this, [this, leaf](bool modified) {
        if (leaf == m_active)
            updateTitle();
        else
            syncTabs(leaf->group);
        if (modified && m_settings.boolean(QStringLiteral("files.autosave")))
            armAutosave(); // an ink note is kept like a note
    });
    connect(moduleView, &ModuleView::noteActivated, this, &MainWindow::openRelative);
    connect(moduleView, &ModuleView::statusMessage, this,
            [this](const QString &text, int timeout) { statusBar()->showMessage(text, timeout); });
    if (!moduleView->load(path, error)) {
        delete moduleView;
        return false;
    }
    if (!fragment.isEmpty())
        moduleView->openFragment(fragment);
    *view = moduleView;
    return true;
}

void MainWindow::showModuleTab(ModuleView *view, const QString &title)
{
    newTab();
    closeView(m_active);
    m_active->note = Note{};
    m_active->editor->setNoteText(QString());
    m_active->editor->setEnabled(false);
    m_active->editor->hide();
    m_active->inlineTitle->hide();
    view->setParent(m_active->area);
    view->setTheme(m_theme);
    m_active->view = view;
    m_active->viewTitle = title;
    Leaf *leaf = m_active;
    connect(view, &ModuleView::modificationChanged, this, [this, leaf](bool) {
        if (leaf == m_active)
            updateTitle();
        else
            syncTabs(leaf->group); // the asterisk (tabTitle)
    });
    static_cast<QVBoxLayout *>(m_active->area->layout())->addWidget(view, 1);
    view->setFocus();
    syncTabs(m_active->group);
    updateTitle();
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
#ifdef NOTEAPPC_HAVE_IMAGEVIEW
    if (!view && mediaKindOf(path) == QLatin1String("image") && featureOn("image-annotation")) {
        auto *image = new ImageView(m_active->area);
        image->setTheme(m_theme);
        if (!image->load(path, &error)) {
            delete image;
        } else {
            connect(image, &ImageView::statusMessage, this,
                    [this](const QString &text, int timeout) { statusBar()->showMessage(text, timeout); });
            view = image;
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
        canvas->setSnapToObjects(m_settings.boolean(QStringLiteral("canvas.snapToObjects")));
        canvas->setSnapToGrid(m_settings.boolean(QStringLiteral("canvas.snapToGrid")));
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
                    armAutosave(); // a canvas is kept like a note
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
            base->setBeforeWrite([this](const QString &note) { return saveTyping(note); });
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
    if (!view)
        openModuleView(path, &error, &view, fragment);
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
#ifdef NOTEAPPC_HAVE_SHARE
    syncShareViews();
#endif
    view->setFocus();

    syncTabs(m_active->group);
    watchOpenNotes(); // a module's view follows its file
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
    leaf->viewTitle.clear();
    leaf->editor->show();
    leaf->inlineTitle->setVisible(m_settings.boolean(QStringLiteral("appearance.showInlineTitle")));
}

bool MainWindow::isModified(const Leaf *leaf) const
{
    if (auto *module = qobject_cast<ModuleView *>(leaf->view))
        return module->isModified();
#ifdef NOTEAPPC_HAVE_CANVAS
    if (auto *canvas = qobject_cast<CanvasView *>(leaf->view))
        return canvas->isModified();
#endif
    return leaf->editor->isModified();
}

void MainWindow::rememberCurrentPosition()
{
    if (m_active->notePath.isEmpty() || m_active->view)
        return;
    m_settings.setNotePosition(m_active->notePath, m_active->editor->cursorOffset(),
                               m_active->editor->verticalScrollBar()->value(),
                               m_active->editor->foldState(), m_active->editor->topOffset());
    saveSettings();
}

void MainWindow::saveSettings()
{
    m_settings.save(nullptr, 5); // from the UI thread, often (every note switch): never a 200 ms wait on another window's save
    if (!m_settings.takeAdoptedKeys().isEmpty())
        applySettings();
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
    // A file the sidebar is told to leave out is opened without a row to land
    // on; revealing it quietly does nothing (it runs on every note opened).
    if (m_tree->isRowHidden(index.row(), index.parent()))
        return;
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

// --- encrypted notes (docs/ENCRYPTION.md) ------------------------------------

void MainWindow::notebookOutputsChanged(const QString &path, const QString &cacheKey,
                                        const notebook::NotebookRunResult &result)
{
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    if (!isNotebookFile(path))
        return; // only a .ipynb keeps its outputs in the file; any other note keeps them in the cache
    std::shared_ptr<notebook::IpynbRuns> &runs = m_notebookDirty[path];
    if (!runs)
        runs = std::make_shared<notebook::IpynbRuns>();
    runs->results.insert(cacheKey, result);
    for (const auto &leaf : m_leaves) {
        if (!leaf->view && leaf->notePath == path)
            leaf->editor->setModified(true);
    }
    if (m_settings.boolean(QStringLiteral("files.autosave")))
        armAutosave();
#else
    Q_UNUSED(path)
    Q_UNUSED(cacheKey)
    Q_UNUSED(result)
#endif
}

void MainWindow::moveNotebookState(const QString &from, const QString &to)
{
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    if (m_notebookDirty.contains(from))
        m_notebookDirty.insert(to, m_notebookDirty.take(from));
    if (m_notebookImages.contains(from))
        m_notebookImages.insert(to, m_notebookImages.take(from));
#else
    Q_UNUSED(from)
    Q_UNUSED(to)
#endif
}

void MainWindow::pruneNotebookImages()
{
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    for (auto it = m_notebookImages.begin(); it != m_notebookImages.end();) {
        const QString &path = it.key();
        const bool shown = std::any_of(m_leaves.cbegin(), m_leaves.cend(),
                                       [&path](const auto &leaf) { return !leaf->view && leaf->notePath == path; });
        it = shown ? std::next(it) : m_notebookImages.erase(it);
    }
    // The runs a notebook no tab shows were saved or discarded with it; a key left behind would be written
    // into whatever file later takes that name.
    for (auto it = m_notebookDirty.begin(); it != m_notebookDirty.end();) {
        const QString &path = it.key();
        const bool shown = std::any_of(m_leaves.cbegin(), m_leaves.cend(),
                                       [&path](const auto &leaf) { return !leaf->view && leaf->notePath == path; });
        it = shown ? std::next(it) : m_notebookDirty.erase(it);
    }
#endif
}

void MainWindow::showNotebookPictures(MarkdownEditor *editor, const QString &path)
{
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    if (const auto pictures = m_notebookImages.value(path)) {
        // Each cell has its own `image.png`: the scope says whose, for the text as it is now.
        editor->setInlineImages(pictures->images(), [pictures, editor](qsizetype offset) {
            return pictures->scopeAt(editor->noteText(), offset);
        });
        return;
    }
#else
    Q_UNUSED(path)
#endif
    editor->setInlineImages({});
}

bool MainWindow::isNotebookFile(const QString &path) const
{
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    return notebook::isIpynbPath(path) && featureOn("notebooks");
#else
    Q_UNUSED(path)
    return false;
#endif
}

bool MainWindow::readNote(const QString &path, Note &out, QString *error, bool seedOutputs)
{
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    // A Jupyter notebook: its cells as text (docs/NOTEBOOKS.md), outputs seeded into the cache.
    if (isNotebookFile(path)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = file.errorString();
            return false;
        }
        out = Note();
        const QByteArray bytes = file.readAll();
        if (seedOutputs) {
            // The file is the truth from here: what ran before it was read (and was discarded) is not its to save.
            m_notebookDirty.remove(path);
            auto pictures = std::make_shared<notebook::IpynbPictures>();
            const bool read = notebook::readIpynb(bytes, &out.text, error, pictures.get());
            if (pictures->isEmpty())
                m_notebookImages.remove(path);
            else
                m_notebookImages.insert(path, std::move(pictures));
            return read;
        }
        // A look at what is on disk (the change watcher, a save comparing): never seeded, or the outputs of a
        // cell that ran since the file was read would give way to the ones the file still has.
        notebook::IpynbDocument document;
        if (!notebook::parseIpynb(bytes, &document, error))
            return false;
        out.text = notebook::ipynbMarkdown(document);
        return true;
    }
#endif
    if (!Vault::isEncryptedNote(path))
        return Vault::read(path, out, error);
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    if (!featureOn("encrypted-notes")) {
        if (error)
            *error = tr("Encrypted notes are switched off in Core plugins");
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    std::optional<QByteArray> plain = m_encryption->open(file.readAll(), error);
    if (!plain)
        return false;
    out = Vault::decode(*plain);
    crypt::wipe(*plain);
    return true;
#else
    if (error)
        *error = tr("This build of Notolith cannot open encrypted notes");
    return false;
#endif
}

bool MainWindow::writeNote(const QString &path, const Note &note, QString *error)
{
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    // The edited text merged into the notebook on disk: cells it did not touch stay the file's own.
    if (isNotebookFile(path)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = file.errorString();
            return false;
        }
        const auto runs = m_notebookDirty.value(path);
        const QByteArray merged =
            notebook::mergeIpynb(file.readAll(), note.text, runs ? *runs : notebook::IpynbRuns(), error);
        file.close();
        if (merged.isEmpty() || !Vault::writeBytes(path, merged, error))
            return false;
        m_notebookDirty.remove(path); // the file says what those runs produced now
        return true;
    }
#endif
    if (path.endsWith(QLatin1String(".ipynb"), Qt::CaseInsensitive)) {
        // Never as Markdown, whatever the notebooks plugin is believed to be doing: that would replace the
        // notebook's JSON (its outputs, counts and ids) with the cells' text.
        if (error)
            *error = tr("a Jupyter notebook is not written as Markdown (the Notebooks plugin is off)");
        return false;
    }
    if (!Vault::isEncryptedNote(path))
        return Vault::write(path, note, error);
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    QByteArray plain = Vault::encode(note);
    const QByteArray sealed = m_encryption->seal(plain, encryptionSample(path), error);
    crypt::wipe(plain);
    return !sealed.isEmpty() && Vault::writeBytes(path, sealed, error);
#else
    if (error)
        *error = tr("This build of Notolith cannot write encrypted notes");
    return false;
#endif
}

QByteArray MainWindow::encryptionSample(const QString &path) const
{
    const auto bytesOf = [](const QString &file) {
        QFile source(file);
        return source.open(QIODevice::ReadOnly) ? source.readAll() : QByteArray();
    };
    if (!path.isEmpty() && QFileInfo::exists(path))
        return bytesOf(path);
    if (!m_vault.isValid())
        return {};
    QDirIterator files(m_vault.root(), {QStringLiteral("*.mdlock")}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QByteArray bytes = bytesOf(files.next());
        if (!bytes.isEmpty())
            return bytes;
    }
    return {};
}

bool MainWindow::showsEncryptedNote(const MarkdownEditor *editor) const
{
    for (const auto &leaf : m_leaves) {
        if (leaf->editor == editor)
            return Vault::isEncryptedNote(leaf->notePath);
    }
    return false;
}

QString MainWindow::notePathOf(const MarkdownEditor *editor) const
{
    for (const auto &leaf : m_leaves) {
        if (leaf->editor == editor)
            return leaf->notePath;
    }
    return {};
}

bool MainWindow::isSharedNote(const MarkdownEditor *editor) const
{
#ifdef NOTEAPPC_HAVE_SHARE
    const QString path = notePathOf(editor);
    if (sessionFor(path))
        return true;
    if (!m_vault.isValid() || path.isEmpty() || !m_vault.contains(path))
        return false;
    // Sharing not running (feature off, start failed): the store still says the
    // text came from a share. Stopping the share here removes the record, and
    // the note is an ordinary one again.
    const QString relative = m_vault.relative(path);
    const QList<ShareRecord> records = storedShares();
    return std::ranges::any_of(records, [&](const ShareRecord &record) { return record.relativePath == relative; });
#else
    Q_UNUSED(editor);
    return false;
#endif
}

void MainWindow::setEncryptionPromptForTests(std::function<QString(bool, bool)> prompt)
{
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    m_encryption->setPrompt(std::move(prompt));
#else
    Q_UNUSED(prompt);
#endif
}

void MainWindow::saveEncryptedLeaves()
{
    for (const auto &leaf : m_leaves) {
        if (!leaf->view && Vault::isEncryptedNote(leaf->notePath) && leaf->editor->isModified())
            saveLeaf(leaf.get(), true);
    }
}

void MainWindow::closeEncryptedLeaves()
{
    QList<Leaf *> encrypted;
    for (const auto &leaf : m_leaves) {
        if (!leaf->view && Vault::isEncryptedNote(leaf->notePath))
            encrypted << leaf.get();
    }
    for (Leaf *leaf : std::as_const(encrypted)) {
        // One that could not be saved stays, rather than losing its edits.
        if (leaf->editor->isModified()) {
            QMessageBox::warning(this, tr("Encrypted notes locked"),
                                 tr("%1 could not be saved, so it stays open. Save it to encrypt it again.")
                                     .arg(QFileInfo(leaf->notePath).completeBaseName()));
            continue;
        }
        leaf->pinned = false; // locking takes the plaintext away, pinned or not
        closeLeaf(leaf);
    }
}

namespace {

/// Every tab on `from` shows `to` from now on: a note that was encrypted or
/// decrypted is the same document under another name.
template<typename Leaves>
void retarget(Leaves &leaves, const QString &from, const QString &to)
{
    for (const auto &leaf : leaves) {
        if (leaf->notePath == from) {
            leaf->notePath = to;
            leaf->editor->refreshLinkStatus(); // relative links are asked about from the new place
        }
    }
}

} // namespace

void MainWindow::encryptCurrentNote()
{
    flushSaves(); // an autosave in flight would write the plain file back after it is gone
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    const QString from = m_active->notePath;
    if (!m_vault.isValid() || m_active->view || !Vault::isNote(from) || !m_vault.contains(from)) {
        statusBar()->showMessage(tr("Only a note in a vault can be encrypted"), 4000);
        return;
    }
    const QFileInfo info(from);
    const QString to = info.dir().filePath(info.completeBaseName() + QStringLiteral(".mdlock"));
    if (QFileInfo::exists(to)) {
        QMessageBox::warning(this, tr("Encrypt note"), tr("%1 already exists.").arg(QFileInfo(to).fileName()));
        return;
    }
    QMessageBox ask(QMessageBox::Question, tr("Encrypt note"),
                    tr("Encrypt %1? Its text is kept only encrypted on disk and is readable only with the "
                       "password, in this app.\n\nThe plain file is deleted, and so are its file recovery "
                       "snapshots. Copies made before now stay where they are: the note history, backups, "
                       "disk snapshots and other devices.")
                        .arg(info.completeBaseName()),
                    QMessageBox::Cancel, this);
    QPushButton *encrypt = ask.addButton(tr("Encrypt"), QMessageBox::AcceptRole);
    ask.exec();
    if (ask.clickedButton() != encrypt)
        return;

    Note note = m_active->note;
    note.text = m_active->editor->noteText();
    QString error;
    if (!writeNote(to, note, &error)) {
        if (!error.isEmpty())
            QMessageBox::warning(this, tr("Encrypt note"), tr("Could not encrypt it:\n%1").arg(error));
        QFile::remove(to);
        return;
    }
    // Read it back before the plain file goes: a note is never lost to this.
    Note check;
    if (!readNote(to, check, &error) || check.text != note.text) {
        QFile::remove(to);
        QMessageBox::warning(this, tr("Encrypt note"), tr("The encrypted copy did not read back; nothing was changed."));
        return;
    }
    flushSaves(); // a commit queued while the box was open must land before the plain file and its copies go
    Snapshots::forget(from);
    QFile::remove(from);
    retarget(m_leaves, from, to);
    watchOpenNotes();
    m_active->note = Vault::asWritten(note);
    for (const auto &leaf : m_leaves) {
        if (leaf->notePath == to)
            leaf->editor->setModified(false);
    }
    updateTitle();
    statusBar()->showMessage(tr("Encrypted %1").arg(info.completeBaseName()), 4000);
#endif
}

void MainWindow::decryptCurrentNote()
{
    flushSaves(); // an autosave in flight would write the plain file back after it is gone
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    const QString from = m_active->notePath;
    if (m_active->view || !Vault::isEncryptedNote(from))
        return;
    const QFileInfo info(from);
    const QString to = info.dir().filePath(info.completeBaseName() + QStringLiteral(".md"));
    if (QFileInfo::exists(to)) {
        QMessageBox::warning(this, tr("Decrypt note"), tr("%1 already exists.").arg(QFileInfo(to).fileName()));
        return;
    }
    if (QMessageBox::question(this, tr("Decrypt note"),
                              tr("Decrypt %1? Its text is written to disk unencrypted.").arg(info.completeBaseName()))
        != QMessageBox::Yes)
        return;
    Note note = m_active->note;
    note.text = m_active->editor->noteText();
    QString error;
    if (!Vault::write(to, note, &error)) {
        QMessageBox::warning(this, tr("Decrypt note"), tr("Could not write it:\n%1").arg(error));
        return;
    }
    QFile::remove(from);
    retarget(m_leaves, from, to);
    watchOpenNotes();
    m_active->note = Vault::asWritten(note);
    for (const auto &leaf : m_leaves) {
        if (leaf->notePath == to)
            leaf->editor->setModified(false);
    }
    updateTitle();
#endif
}

void MainWindow::newEncryptedNote()
{
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    if (!m_vault.isValid())
        return;
    const QString folder = !m_active->view && m_vault.contains(m_active->notePath)
                               ? QFileInfo(m_active->notePath).absolutePath()
                               : m_vault.root();
    QString path = QDir(folder).filePath(tr("Untitled") + QStringLiteral(".mdlock"));
    for (int n = 1; QFileInfo::exists(path); ++n)
        path = QDir(folder).filePath(tr("Untitled %1").arg(n) + QStringLiteral(".mdlock"));
    QString error;
    if (!writeNote(path, Note{}, &error)) {
        if (!error.isEmpty())
            QMessageBox::warning(this, tr("New encrypted note"), error);
        return;
    }
    openNote(path);
#endif
}

void MainWindow::changeEncryptionPassword()
{
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    const QByteArray sample = encryptionSample();
    if (sample.isEmpty()) {
        statusBar()->showMessage(tr("There are no encrypted notes yet"), 4000);
        return;
    }
    if (!m_encryption->isUnlocked() && !m_encryption->unlock(sample))
        return;
    saveEncryptedLeaves(); // what is on disk is what gets re-sealed

    // Every note opened with the current password first; nothing changes
    // unless all of them do.
    QList<QPair<QString, QByteArray>> notes;
    QDirIterator files(m_vault.root(), {QStringLiteral("*.mdlock")}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        QFile file(path);
        std::optional<QByteArray> plain = file.open(QIODevice::ReadOnly) ? m_encryption->open(file.readAll())
                                                                         : std::nullopt;
        if (!plain) {
            for (auto &[_, text] : notes)
                crypt::wipe(text);
            QMessageBox::warning(this, tr("Change password"),
                                 tr("%1 does not open with the current password, so nothing was changed.")
                                     .arg(m_vault.relative(path)));
            return;
        }
        notes.append({path, *plain});
        crypt::wipe(*plain);
    }
    const QString password = m_encryption->askNewPassword();
    if (password.isEmpty() || !m_encryption->rekey(password)) {
        for (auto &[_, text] : notes)
            crypt::wipe(text);
        return;
    }
    int failed = 0;
    for (auto &[path, text] : notes) {
        const QByteArray sealed = m_encryption->seal(text, QByteArray());
        if (sealed.isEmpty() || !Vault::writeBytes(path, sealed))
            ++failed;
        crypt::wipe(text);
    }
    if (failed > 0)
        QMessageBox::warning(this, tr("Change password"),
                             tr("%n note(s) could not be re-encrypted and still open with the old password.", nullptr,
                                failed));
    else
        statusBar()->showMessage(tr("Password changed for %n encrypted note(s)", nullptr, int(notes.size())), 5000);
#endif
}

bool MainWindow::saveLeaf(Leaf *target, bool quiet, bool background)
{
    if (!background)
        flushSaves(); // the file and the tab's baseline are the last save's before this one reads them
#ifdef NOTEAPPC_HAVE_IMAGEVIEW
    if (qobject_cast<ImageView *>(target->view))
        return true; // never saved here: its marks go to their own file as they are made, so a missing one has nothing to refuse
#endif
    if (isFileView(target->view) && m_lostNotes.contains(target->notePath)) {
        // gone from disk (not found yet, or not found at all): a save would recreate it
        reportSaveError(QFileInfo(target->notePath).fileName(),
                        tr("the file is gone from disk (deleted or moved outside the app)"), quiet);
        return false;
    }
    if (auto *module = qobject_cast<ModuleView *>(target->view)) { // a file-less view too
        const QString shown = target->notePath.isEmpty() ? target->viewTitle : QFileInfo(target->notePath).fileName();
        QString error;
        if (!module->save(&error)) {
            reportSaveError(shown, error, quiet);
            return false;
        }
        if (!target->notePath.isEmpty())
            rememberViewStamp(target->notePath);
        if (!quiet)
            statusBar()->showMessage(tr("Saved %1").arg(shown), 2000);
        return true;
    }
    if (target->notePath.isEmpty())
        return true;
    // An autosave of a notebook the plugin no longer handles would take the background path below, which
    // writes the encoded text straight to the file: refused here, before either path, and again in writeNote().
    if (target->notePath.endsWith(QLatin1String(".ipynb"), Qt::CaseInsensitive) && !isNotebookFile(target->notePath)
        && !target->view) {
        reportSaveError(QFileInfo(target->notePath).fileName(),
                        tr("a Jupyter notebook is not written as Markdown (the Notebooks plugin is off)"), quiet);
        return false;
    }
#ifdef NOTEAPPC_HAVE_CANVAS
    if (auto *canvas = qobject_cast<CanvasView *>(target->view)) {
        QString error;
        if (!canvas->save(QString(), &error)) {
            reportSaveError(target->notePath, error, quiet);
            return false;
        }
        rememberViewStamp(target->notePath);
        if (!quiet)
            statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(target->notePath).fileName()), 2000);
        return true;
    }
#endif
    if (target->view)
        return true; // a base writes each cell edit to its note as it happens

    Note note = target->note;
    note.text = target->editor->noteText();

    // Invalid UTF-8 on disk: what is shown has U+FFFD where the bytes were, so
    // a save changes bytes nobody edited. Never from an autosave; by hand only
    // when the person says so.
    if (note.lossy) {
        if (!isModified(target))
            return true;
        const QString name = QFileInfo(target->notePath).fileName();
        if (quiet) {
            statusBar()->showMessage(
                tr("%1 is not valid UTF-8: it is not saved automatically, because saving would change bytes that "
                   "were not edited. Save it by hand to replace them")
                    .arg(name),
                10000);
            return false;
        }
        if (QMessageBox::question(this, tr("Save"),
                                  tr("%1 is not valid UTF-8. Saving replaces the invalid bytes with U+FFFD and "
                                     "cannot be undone.\n\nSave anyway?")
                                      .arg(name)) != QMessageBox::Yes)
            return false;
        note.lossy = false; // the consent: Vault::write() refuses a lossy note
    }

    // File recovery, for notes in a vault only: a stray note leaves nothing
    // behind. The first save of a note keeps what it held before, too. An
    // encrypted note keeps nothing there: its text never reaches the disk.
    const bool encrypted = Vault::isEncryptedNote(target->notePath);
    const bool snapshots =
        !encrypted && featureOn("file-recovery") && m_vault.isValid() && m_vault.contains(target->notePath);
    // An autosave: the text is encoded here, the commit (fdatasync, 5-25 ms on
    // a flushing disk) and the recovery copies are another thread's, and
    // finishSave() does the rest. A note that needs the notebook merge or a
    // password stays on the synchronous path.
    const bool inBackground = background && !encrypted && !isNotebookFile(target->notePath) && !note.lossy;
    QString firstVersion; // what the note held before its first save here, for the job to keep
    if (snapshots && Snapshots::versions(target->notePath).isEmpty() && note.text != target->note.text) {
        if (inBackground)
            firstVersion = target->note.text;
        else
            Snapshots::record(target->notePath, target->note.text, 0);
    }
    // A change from outside (a sync client) that the watcher has not reported
    // yet - it waits for writes to settle - would be written over unseen.
    QString seenOnDisk = target->note.text; // what the commit expects to replace
    QString externalVersion;                // a change found on disk, for the job to keep
    if (snapshots) {
        Note disk;
        if (readNote(target->notePath, disk, nullptr)) {
            seenOnDisk = disk.text;
            if (disk.text != target->note.text && disk.text != note.text) {
                if (inBackground) {
                    externalVersion = disk.text;
                    statusBar()->showMessage(tr("%1 had changed on disk: that version is in file recovery")
                                                 .arg(QFileInfo(target->notePath).fileName()),
                                             8000);
                } else if (Snapshots::record(target->notePath, disk.text, 0)) {
                    statusBar()->showMessage(tr("%1 had changed on disk: that version is in file recovery")
                                                 .arg(QFileInfo(target->notePath).fileName()),
                                             8000);
                }
            }
        }
    }

    if (inBackground) {
        note = Vault::asWritten(note);
        auto *watcher = new QFutureWatcher<SaveResult>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] { finishSave(watcher); });
        m_saveJobs.append({target->notePath, note, watcher});
        // The recovery copies are kept in the order the versions existed: what
        // the note held, what was on disk, anything that landed in between,
        // and last the save. Another writer inside the app (a plugin, the PDF,
        // image and EPUB sidecars) can land while the commit waits, so the file is
        // looked at again just before it is replaced.
        watcher->setFuture(QtConcurrent::run(
            &m_savePool,
            [path = target->notePath, bytes = Vault::encode(note), text = note.text, firstVersion, externalVersion,
             seenOnDisk, failed = tr("the file could not be written"),
             interval = qint64(m_settings.integer(QStringLiteral("fileRecovery.interval"))) * 60'000, snapshots,
             hook = m_saveJobHook] {
                if (hook)
                    hook();
                if (!firstVersion.isEmpty())
                    Snapshots::record(path, firstVersion, 0);
                if (!externalVersion.isEmpty())
                    Snapshots::record(path, externalVersion, 0);
                if (snapshots) {
                    Note now;
                    if (Vault::read(path, now) && now.text != seenOnDisk && now.text != text)
                        Snapshots::record(path, now.text, 0);
                }
                SaveResult result;
                if (!Vault::writeBytes(path, bytes, &result.error))
                    return SaveResult{result.error.isEmpty() ? failed : result.error};
                if (snapshots)
                    Snapshots::record(path, text, interval);
                return SaveResult{};
            }));
        return true;
    }

    QString error;
    if (!writeNote(target->notePath, note, &error)) {
        if (error.isEmpty())
            error = tr("the password was not given, so it could not be encrypted");
        reportSaveError(target->notePath, error, quiet);
        return false;
    }
    note = Vault::asWritten(note);

    if (snapshots)
        Snapshots::record(target->notePath, note.text,
                          qint64(m_settings.integer(QStringLiteral("fileRecovery.interval"))) * 60'000);
    recordHistory(target->notePath, note.text);
    notifyPlugins("note-saved", target->notePath);
#ifdef NOTEAPPC_HAVE_SYNC
    if (m_sync)
        m_sync->noteChanged();
#endif
    // Every other tab on this note shows this same document, so this saved
    // it too.
    showDiskNote(target->notePath, note);
#ifdef NOTEAPPC_HAVE_SHARE
    if (ShareSession *session = sessionFor(target->notePath))
        session->noteSaved(note.text); // what the file and the shared text agree on now
#endif
    watchOpenNotes(); // a note saved for the first time
    if (!quiet) {
        statusBar()->showMessage(tr("Saved %1").arg(m_vault.isValid() ? m_vault.relative(target->notePath)
                                                                       : QFileInfo(target->notePath).fileName()),
                                 2000);
    }
    if (m_vault.isValid())
        m_indexer->reindex(target->notePath);
    return true;
}


void MainWindow::flushSaves()
{
    while (!m_saveJobs.isEmpty()) {
        QFutureWatcher<SaveResult> *watcher = m_saveJobs.first().watcher;
        watcher->waitForFinished();
        finishSave(watcher); // its queued finished() finds the job gone
    }
}

void MainWindow::finishSave(QFutureWatcher<SaveResult> *watcher)
{
    const auto at = std::find_if(m_saveJobs.begin(), m_saveJobs.end(),
                                 [watcher](const SaveJob &job) { return job.watcher == watcher; });
    if (at == m_saveJobs.end())
        return;
    const SaveJob job = *at;
    m_saveJobs.erase(at);
    const SaveResult result = watcher->result();
    watcher->deleteLater();
    if (!result.error.isEmpty()) {
        reportSaveError(job.path, result.error, true); // the tab stays modified; the next pause tries again
        return;
    }
    recordHistory(job.path, job.note.text);
    notifyPlugins("note-saved", job.path);
#ifdef NOTEAPPC_HAVE_SYNC
    if (m_sync)
        m_sync->noteChanged();
#endif
    // Every tab on the note is as saved as the text it held when the save was
    // queued; one that has been typed in since stays modified (and is saved
    // again) and must not be reset to the older text, as showDiskNote() would.
    bool newer = false;
    for (const auto &leaf : m_leaves) {
        if (leaf->view || leaf->notePath != job.path)
            continue;
        leaf->note = job.note;
        if (leaf->editor->noteText() == job.note.text)
            leaf->editor->setModified(false);
        else
            newer = true;
    }
    if (newer && m_settings.boolean(QStringLiteral("files.autosave")))
        armAutosave();
#ifdef NOTEAPPC_HAVE_SHARE
    if (ShareSession *session = sessionFor(job.path))
        session->noteSaved(job.note.text);
#endif
    watchOpenNotes();
    if (m_vault.isValid())
        m_indexer->reindex(job.path);
}

void MainWindow::showDiskNote(const QString &path, const Note &note)
{
    for (const auto &leaf : m_leaves) {
        if (leaf->view || leaf->notePath != path)
            continue;
        leaf->note = note;
        MarkdownEditor *editor = leaf->editor;
        showNotebookPictures(editor, path);
        const QString &shown = editor->noteText();
        if (shown != note.text) {
            // Only the part that differs, so the caret and the scroll stay
            // where they are, as they do for an edit made in another tab.
            const MarkdownEditor::Hunk hunk = MarkdownEditor::hunkBetween(shown, note.text);
            editor->applyExternalEdit(hunk.at, hunk.removed, hunk.inserted);
        }
        editor->setModified(false);
    }
#ifdef NOTEAPPC_HAVE_SHARE
    // The editors changed without an edited(): the shared text follows.
    if (ShareSession *session = sessionFor(path))
        session->adoptText(note.text);
#endif
}

void MainWindow::watchOpenNotes()
{
    QStringList wanted;
    QStringList wantedFolders;
    for (const auto &leaf : m_leaves) {
        // A module's view follows its file like a note does (ModuleView::reloadFromDisk), and a canvas or an image
        // is told where it went (followLostNotes); the other views do not.
        if ((leaf->view && !isFileView(leaf->view)) || leaf->notePath.isEmpty()
            || wanted.contains(leaf->notePath))
            continue;
        wanted << leaf->notePath;
        if (isFileView(leaf->view))
            rememberViewStamp(leaf->notePath); // not once it is gone: the stamp is what a moved file is known by
        const QString folder = QFileInfo(leaf->notePath).absolutePath();
        if (!wantedFolders.contains(folder))
            wantedFolders << folder;
    }
    for (auto it = m_viewStamps.begin(); it != m_viewStamps.end();)
        it = wanted.contains(it.key()) ? std::next(it) : m_viewStamps.erase(it);
    pruneNotebookImages(); // a tab that went on to another note let its notebook go
    const QStringList watched = m_noteWatcher->files();
    const QStringList watchedFolders = m_noteWatcher->directories();
    QStringList gone;
    QStringList added;
    for (const QString &path : watched) {
        if (!wanted.contains(path))
            gone << path;
    }
    for (const QString &folder : watchedFolders) {
        if (!wantedFolders.contains(folder))
            gone << folder;
    }
    for (const QString &path : std::as_const(wanted)) {
        if (!watched.contains(path) && QFileInfo::exists(path))
            added << path;
    }
    for (const QString &folder : std::as_const(wantedFolders)) {
        if (!watchedFolders.contains(folder) && QFileInfo::exists(folder))
            added << folder;
    }
    if (!gone.isEmpty())
        m_noteWatcher->removePaths(gone);
    // Every path here exists (checked above), so a failure is a watch the
    // system would not give: say so rather than miss outside edits silently.
    if (!added.isEmpty() && !m_noteWatcher->addPaths(added).isEmpty())
        statusBar()->showMessage(tr("An open note cannot be watched (fs.inotify.max_user_watches?): changes made "
                                    "outside Notolith will not show until it is reopened"),
                                 10'000);
}

QString MainWindow::findMovedNote(const QString &lost, const Note &known)
{
    // Limit: a walk of the vault's files by size, once per lost note after the
    // grace (tens of ms at 20 000 notes, only when a note really went); look the
    // size up in the Index instead if folders of notes are ever moved by hand.
    if (!m_vault.isValid() || !m_vault.contains(lost) || Vault::isEncryptedNote(lost))
        return {};
    // A move keeps the bytes: same size first, so only a look-alike is read.
    const qint64 size = Vault::encode(known).size();
    QStringList open;
    for (const auto &leaf : m_leaves)
        open << leaf->notePath;
    QString found;
    QDirIterator files(m_vault.root(), {QStringLiteral("*.") + QFileInfo(lost).suffix()}, QDir::Files,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString candidate = files.next();
        if (candidate == lost || files.fileInfo().size() != size || open.contains(candidate))
            continue;
        Note note;
        if (!readNote(candidate, note, nullptr) || note.text != known.text || note.crlf != known.crlf
            || note.bom != known.bom)
            continue;
        if (!found.isEmpty())
            return {}; // two copies: which one it became is a guess
        found = candidate;
    }
    return found;
}

QString MainWindow::findMovedFile(const QString &lost)
{
    // Limit: the same walk as findMovedNote; a view's file has no text to compare, so the candidate must have
    // the name and the size and modification time the tab last saw (a move keeps all three; a copy-then-delete
    // sync does not, and the tab is then marked missing). One such file not open in a tab, or none: two are a guess.
    const QString stamp = m_viewStamps.value(lost);
    if (stamp.isEmpty() || !m_vault.isValid() || !m_vault.contains(lost))
        return {};
    const QString name = QFileInfo(lost).fileName();
    QStringList open;
    for (const auto &leaf : m_leaves)
        open << leaf->notePath;
    QString found;
    QDirIterator files(m_vault.root(), QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString candidate = files.next();
        if (files.fileName() != name || candidate == lost || open.contains(candidate)
            || stampOf(files.fileInfo()) != stamp)
            continue;
        if (!found.isEmpty())
            return {};
        found = candidate;
    }
    return found;
}

void MainWindow::rememberViewStamp(const QString &path)
{
    const QFileInfo info(path);
    if (info.exists())
        m_viewStamps.insert(path, stampOf(info));
}

void MainWindow::followLostNotes(bool final)
{
    // A note's editor, or a view of one file (the watcher follows exactly those): a file a tab saves to.
    const auto followed = [](const auto &leaf) {
        return !leaf->notePath.isEmpty() && (!leaf->view || isFileView(leaf->view));
    };
    // autosave() skipped a modified view while its file was gone: nothing else would try again before the next edit.
    const auto rearmAutosave = [this](const QString &path) {
        const bool modifiedView = std::any_of(m_leaves.cbegin(), m_leaves.cend(), [&](const auto &leaf) {
            return leaf->notePath == path && isFileView(leaf->view) && isModified(leaf.get());
        });
        if (modifiedView && m_settings.boolean(QStringLiteral("files.autosave")))
            armAutosave();
    };
    QStringList open;
    for (const auto &leaf : m_leaves) {
        if (followed(leaf) && !open.contains(leaf->notePath))
            open << leaf->notePath;
    }
    for (auto it = m_lostNotes.begin(); it != m_lostNotes.end();) // closed since
        it = open.contains(it.key()) ? std::next(it) : m_lostNotes.erase(it);
    for (const QString &path : std::as_const(open)) {
        if (QFileInfo::exists(path)) {
            // Back, as a rewrite or a recreation: read like any other change.
            if (m_lostNotes.remove(path)) {
                m_changedNotes.insert(path);
                m_noteChanges.start();
                for (const auto &group : m_groups) // a view's tab says "missing" while it is, in every split
                    syncTabs(group.get());
                updateTitle();
                rearmAutosave(path);
            }
            continue;
        }
        if (m_lostNotes.value(path, false))
            continue; // already reported
        if (!final) {
            // A sync client deletes before it writes, and a move is found by
            // looking through the vault: not before the grace is over.
            // Limit: one look after the grace (1 s). A file that turns up later (a slow sync client, a copy-then-delete
            // across devices) stays missing, and the destination folders are not watched; look again on a longer timer.
            m_lostNotes.insert(path, false);
            if (!m_lostRetry.isActive())
                m_lostRetry.start();
            continue;
        }
        const auto first = std::find_if(m_leaves.cbegin(), m_leaves.cend(), [&path](const auto &leaf) {
            return !leaf->view && leaf->notePath == path;
        });
        const QString moved = first != m_leaves.cend() ? findMovedNote(path, (*first)->note) : findMovedFile(path);
        if (!moved.isEmpty()) {
            m_lostNotes.remove(path);
            if (m_vault.contains(path) && m_vault.contains(moved)
                && m_fileIcons.renamePrefix(m_vault.relative(path), m_vault.relative(moved)))
                m_fileIcons.save();
            // The caret, the scroll and what is typed stay: only the name changes.
            for (const auto &leaf : m_leaves) {
                if (followed(leaf) && leaf->notePath == path) {
                    leaf->notePath = moved;
                    leaf->editor->refreshLinkStatus(); // relative links are asked about from the new place
                    leaf->inlineTitle->setText(QFileInfo(moved).completeBaseName());
                    followViewPath(leaf->view, moved);
                }
            }
            moveNotebookState(path, moved);
#ifdef NOTEAPPC_HAVE_HISTORY
            if (m_noteHistory && m_vault.contains(moved)) {
                const std::shared_ptr<NoteHistory> history = m_noteHistory;
                const QString from = m_vault.relative(path);
                const QString to = m_vault.relative(moved);
                m_historyPool.start([history, from, to] { history->rename(from, to); });
            }
#endif
            for (const auto &group : m_groups)
                syncTabs(group.get());
            watchOpenNotes();
            updateTitle();
            rearmAutosave(moved);
            statusBar()->showMessage(tr("%1 was moved outside the app: following it to %2")
                                         .arg(QFileInfo(path).fileName(), m_vault.relative(moved)),
                                     6000);
            notifyPlugins("note-renamed", moved);
        } else {
            m_lostNotes[path] = true; // autosave leaves it alone from here
            for (const auto &group : m_groups)
                syncTabs(group.get());
            updateTitle(); // a view's tab says "missing"
            statusBar()->showMessage(first != m_leaves.cend()
                                         ? tr("%1 is gone from disk (deleted or moved outside the app): it is not saved "
                                              "again until you save it yourself")
                                               .arg(QFileInfo(path).fileName())
                                         : tr("%1 is gone from disk (deleted or moved outside the app): its tab is not "
                                              "saved, so the file is not recreated")
                                               .arg(QFileInfo(path).fileName()),
                                     10000);
        }
    }
}

void MainWindow::reloadChangedNotes()
{
    watchOpenNotes(); // a file replaced by a rename dropped out of the watch
    followLostNotes(false);
    for (const QString &path : std::exchange(m_changedNotes, {})) {
        // An own save still being committed is not a change from outside, and
        // waiting for it would block the window: look again when it has landed.
        if (std::any_of(m_saveJobs.cbegin(), m_saveJobs.cend(), [&path](const SaveJob &job) { return job.path == path; })) {
            m_changedNotes.insert(path);
            m_noteChanges.start();
            continue;
        }
        bool moduleFile = false;
        for (const auto &leaf : m_leaves) {
            if (auto *module = leaf->notePath == path ? qobject_cast<ModuleView *>(leaf->view) : nullptr) {
                module->reloadFromDisk();
                moduleFile = true;
            }
        }
        if (moduleFile)
            continue;
        const auto first = std::find_if(m_leaves.cbegin(), m_leaves.cend(), [&path](const auto &leaf) {
            return !leaf->view && leaf->notePath == path;
        });
        // Closed since, or gone for now: a rename over it, or a delete the
        // next save puts back.
        if (first == m_leaves.cend() || !QFileInfo::exists(path))
            continue;
        const bool encrypted = Vault::isEncryptedNote(path);
#ifdef NOTEAPPC_HAVE_ENCRYPTION
        if (encrypted && !m_encryption->isUnlocked())
            continue; // never a password prompt out of nowhere
#endif
        // Limit: every own save is read back to tell it apart from a
        // change (under 1 ms at 200 kB); compare size and mtime first if a
        // note ever gets big enough for that to show.
        Note disk;
        if (!readNote(path, disk, nullptr, !m_notebookDirty.contains(path)))
            continue;
        const Note &known = (*first)->note;
        if (disk.text == known.text && disk.crlf == known.crlf && disk.bom == known.bom && disk.mixed == known.mixed)
            continue; // this window's own save
#ifdef NOTEAPPC_HAVE_SHARE
        // A shared note's file is one more writer of the shared text: what it
        // changed is made in the document, not laid over it. When that clashes
        // with what is being typed, the branch below keeps the typing.
        if (ShareSession *session = sessionFor(path); session && session->foldInDisk(known.text, disk.text)) {
            for (const auto &leaf : m_leaves) {
                if (leaf->view || leaf->notePath != path)
                    continue;
                leaf->note = disk;
                if (leaf->editor->noteText() == disk.text)
                    leaf->editor->setModified(false);
            }
            continue;
        }
#endif
        if (!isModified(first->get())) {
            showDiskNote(path, disk);
            continue;
        }
        // Limit: what is being typed wins and the next save writes it;
        // the version on disk goes to file recovery. A three-way merge
        // (libs/ext/sync's SyncMerge) if that ever is not enough.
        const bool kept = !encrypted && featureOn("file-recovery") && m_vault.isValid() && m_vault.contains(path)
                       && Snapshots::record(path, disk.text, 0);
        for (const auto &leaf : m_leaves) {
            if (!leaf->view && leaf->notePath == path)
                leaf->note = disk;
        }
        const QString name = QFileInfo(path).fileName();
        statusBar()->showMessage(
            kept ? tr("%1 changed on disk while it was being edited: the edits are kept, the other version is "
                      "in file recovery")
                       .arg(name)
                 : tr("%1 changed on disk while it was being edited: the edits are kept").arg(name),
            8000);
    }
}

void MainWindow::reportSaveError(const QString &path, const QString &error, bool quiet)
{
    // An autosave runs while someone types: a box in their face then is
    // worse than the failure, which the next pause tries again anyway. The
    // bar keeps saying so until a save goes through.
    if (quiet) {
        m_lastSaveError = error;
        statusBar()->showMessage(tr("Could not save %1: %2 - will try again").arg(QFileInfo(path).fileName(), error));
        // Nothing else restarts it: only a further edit would.
        if (m_settings.boolean(QStringLiteral("files.autosave")))
            armAutosave();
    } else
        QMessageBox::warning(this, tr("Save"), tr("Could not save %1:\n%2").arg(path, error));
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow())
        m_closing = false; // shown again after a close
    // Focus left the window (another application, a pop-out): what was typed
    // is committed now, in the background, not at the end of the pause.
    // Not while a box is up (it is what took the focus, and its answer may be
    // Discard, or the box may be about to delete the file) nor once the window is closing.
    if (event->type() == QEvent::ActivationChange && !isActiveWindow() && !m_closing
        && !QApplication::activeModalWidget() && (m_autosave.isActive() || m_autosaveMax.isActive())
        && m_settings.boolean(QStringLiteral("files.autosave")))
        autosave(true);
}

bool MainWindow::isBuiltInSuffixForTests(const QString &suffix)
{
    return isBuiltInSuffix(suffix.toLower());
}

void MainWindow::setSaveJobHookForTests(std::function<void()> hook)
{
    m_saveJobHook = std::move(hook);
}

void MainWindow::setAutosaveIntervalsForTests(int pause, int maxWait)
{
    m_autosave.setInterval(pause);
    m_autosaveMax.setInterval(maxWait);
}

int MainWindow::lostStateForTests(const QString &path) const
{
    const auto found = m_lostNotes.constFind(path);
    return found == m_lostNotes.constEnd() ? -1 : int(found.value());
}

qsizetype MainWindow::notebookRunSetsForTests() const
{
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    return m_notebookDirty.size();
#else
    return 0;
#endif
}

qsizetype MainWindow::notebookPictureSetsForTests() const
{
#ifdef NOTEAPPC_HAVE_NOTEBOOK
    return m_notebookImages.size();
#else
    return 0;
#endif
}

void MainWindow::armAutosave()
{
    m_autosave.start();
    if (!m_autosaveMax.isActive())
        m_autosaveMax.start();
}

void MainWindow::autosaveInBackground()
{
    autosave(true);
}

QStringList MainWindow::autosave(bool background)
{
    if (!background)
        flushSaves();
    m_autosaveMax.stop();
    QSet<QString> saved; // a note in two tabs is one document, saved once
    for (const auto &leaf : m_leaves) {
        if (leaf->notePath.isEmpty() || !isModified(leaf.get()) || saved.contains(leaf->notePath))
            continue;
        if (isFileView(leaf->view) ? m_lostNotes.contains(leaf->notePath) : m_lostNotes.value(leaf->notePath, false))
            continue; // gone from disk and not found: writing it back is for the person to decide (a view's: never)
        if (std::any_of(m_saveJobs.cbegin(), m_saveJobs.cend(),
                        [&](const SaveJob &job) { return job.path == leaf->notePath; }))
            continue; // its last save is still being committed; finishSave() arms the next one if it is behind
        saved.insert(leaf->notePath);
        saveLeaf(leaf.get(), true, background);
    }
    return QStringList(saved.cbegin(), saved.cend());
}

void MainWindow::waitForModuleLoad(Leaf *leaf)
{
    const QPointer<ModuleView> view = qobject_cast<ModuleView *>(leaf->view);
    if (!view || !view->isLoading() || !view->isModified())
        return; // nothing to keep: a tab that only has a file to parse closes at once
    QEventLoop loop;
    QTimer poll; // the view says nothing when its parse lands: ask
    QElapsedTimer waited;
    waited.start();
    connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!view || !view->isLoading() || waited.elapsed() > kModuleLoadWaitMs)
            loop.quit();
    });
    poll.start(10);
    loop.exec(QEventLoop::ExcludeUserInputEvents); // no Ctrl+W, tab click or close button can change the tabs under the caller
}

bool MainWindow::maybeSave()
{
    waitForModuleLoad(m_active); // a reload in flight is not a failed save: let it land, then see what is left to keep
    // A file-less module tab (the glyph studio) has no path but keeps the save contract.
    const bool fileLess = m_active->notePath.isEmpty() && qobject_cast<ModuleView *>(m_active->view);
    if ((m_active->notePath.isEmpty() && !fileLess) || !isModified(m_active))
        return true;
    // Obsidian's way: what is typed is kept, never asked about - unless the
    // file is gone and was not found, where saving would bring it back.
    const bool lost = m_lostNotes.value(m_active->notePath, false);
    if (isFileView(m_active->view) && m_lostNotes.contains(m_active->notePath)) { // in the grace too
        // saveLeaf refuses a view whose file is gone (a save would recreate it): only discarding, or keeping the tab, is left
        const QString name = m_vault.isValid() ? m_vault.relative(m_active->notePath) : QFileInfo(m_active->notePath).fileName();
        const auto answer = QMessageBox::question(
            this, tr("Unsaved changes"),
            tr("%1 is gone from disk (deleted or moved outside the app), so its changes cannot be saved. Discard them?")
                .arg(name),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        return answer == QMessageBox::Discard;
    }
    // A save that fails (disk full, read-only folder) falls through to the
    // box below, so leaving is never refused without a word.
    m_lastSaveError.clear();
    if (!lost && m_settings.boolean(QStringLiteral("files.autosave")) && saveLeaf(m_active, true))
        return true;

    const QString shown = fileLess ? m_active->viewTitle
        : (m_vault.isValid() ? m_vault.relative(m_active->notePath) : QFileInfo(m_active->notePath).fileName());
    const bool lossy = m_active->note.lossy;
    QString question =
        lost ? tr("%1 is gone from disk. Save it there again?").arg(shown) : tr("Save changes to %1?").arg(shown);
    if (lossy)
        question = tr("%1 is not valid UTF-8. Saving replaces the invalid bytes with U+FFFD and cannot be undone.\n\n"
                      "Save anyway?")
                       .arg(shown);
    if (!m_lastSaveError.isEmpty())
        question = tr("%1 could not be saved: %2\n\nTry saving again?").arg(shown, m_lastSaveError);
    const auto answer = QMessageBox::question(this, tr("Unsaved changes"), question,
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                              QMessageBox::Save);

    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Discard)
        return true;
    m_active->note.lossy = false; // asked just now
    return save();
}

void MainWindow::onModificationChanged(bool)
{
    updateTitle();
}

void MainWindow::updateTitle()
{
    // A file-less module tab (the glyph studio) shows the mark too: the host saves it like a note.
    const bool fileLess = m_active->notePath.isEmpty() && qobject_cast<ModuleView *>(m_active->view);
    const bool modified = (!m_active->notePath.isEmpty() || fileLess) && isModified(m_active);
    QString title;
    if (!m_active->notePath.isEmpty()) {
        title = (m_vault.isValid() ? m_vault.relative(m_active->notePath) : QFileInfo(m_active->notePath).fileName())
              + QLatin1String(" - ");
    }
    if (m_vault.isValid())
        title += m_vault.name() + QLatin1String(" - ");
    title += QStringLiteral("Notolith");
    setWindowTitle(modified ? QLatin1Char('*') + title : title);

    syncTabs(m_active->group);
    m_vaultLabel->setText(m_vault.isValid() ? m_vault.name() : tr("No vault open"));
    if (m_vault.isValid() || m_openMode == OpenMode::SingleFile)
        m_emptyState->hide();

    if (m_vaultMenuButton) {
        if (m_openMode == OpenMode::SingleFile && !m_active->notePath.isEmpty())
            m_vaultMenuButton->setText(QFileInfo(m_active->notePath).fileName());
        else
            m_vaultMenuButton->setText(m_vault.isValid() ? m_vault.name() : tr("No vault"));
    }
}

void MainWindow::updateVimStatus()
{
    const QString status = m_active ? m_active->editor->vimStatus() : QString();
    m_vimStatus->setText(status);
    m_vimStatus->setVisible(!status.isEmpty());
}

void MainWindow::updateWordCount()
{
    updateVimStatus(); // both are "the status bar, for the tab now in front"
    updateModeButton();
    // A canvas or a base has no words of its own to count.
    const bool note = !m_active->notePath.isEmpty() && !m_active->view;
    m_propertyStatus->setVisible(note);
    m_backlinkStatus->setVisible(note && m_backlinks);
    if (note)
        m_backlinkCountTimer.start();
    updateTextCount();
    updateMetaCount();
    if (note) {
        const qsizetype properties = m_active->editor->properties().size();
        m_propertyStatus->setText(properties == 1 ? tr("1 property")
                                                  : tr("%1 properties").arg(properties));
    }
}

void MainWindow::updateTextCount()
{
    const bool note = !m_active->notePath.isEmpty() && !m_active->view;
    const int words = m_active->editor->wordCount();
    if (!note || !featureOn("word-count") || words < 0) { // < 0: a large note, still being counted
        m_wordCount->clear();
        m_charCount->clear();
    } else if (const auto selected = m_active->editor->selectionCount(); selected.characters > 0) {
        // Obsidian counts what is selected, in place of the note.
        const QLocale locale;
        m_wordCount->setText(tr("%1 words, %2 characters")
                                 .arg(locale.toString(selected.words), locale.toString(selected.characters)));
        m_charCount->clear();
    } else {
        const QLocale locale;
        m_wordCount->setText(tr("%1 words").arg(locale.toString(words)));
        m_charCount->setText(tr("%1 characters").arg(locale.toString(m_active->editor->characterCount())));
    }
    m_wordCount->setVisible(!m_wordCount->text().isEmpty());
    m_charCount->setVisible(!m_charCount->text().isEmpty());
}

void MainWindow::updateMetaCount()
{
    const qsizetype runs = m_metaPane && m_active && m_active->notePath.size() && !m_active->view
                               ? m_active->editor->metaNoteRanges().size()
                               : 0;
    m_metaStatus->setText(runs == 1 ? tr("1 meta note") : tr("%1 meta notes").arg(runs));
    m_metaStatus->setVisible(runs > 0);
}

void MainWindow::jumpMetaNote(int direction)
{
    if (!m_active || m_active->notePath.isEmpty() || m_active->view)
        return;
    int index = 0;
    int count = 0;
    switch (m_active->editor->selectMetaNote(direction, &index, &count)) {
    case MarkdownEditor::MetaJump::None:
        statusBar()->showMessage(tr("No meta notes in this note"), 2500);
        break;
    case MarkdownEditor::MetaJump::Moved:
        statusBar()->showMessage(tr("Meta note %1 of %2").arg(index).arg(count), 2500);
        break;
    case MarkdownEditor::MetaJump::Wrapped:
        statusBar()->showMessage(tr("Meta note %1 of %2 (wrapped around)").arg(index).arg(count), 2500);
        break;
    }
}

void MainWindow::updateBacklinkCount()
{
    if (!m_backlinkStatus->isVisible() || !m_index.isOpen() || !m_vault.isValid())
        return;
    const qsizetype links = m_index.backlinks(m_active->notePath).size();
    m_backlinkStatus->setText(links == 1 ? tr("1 backlink") : tr("%1 backlinks").arg(links));
}

void MainWindow::setViewMode(MarkdownEditor::Mode mode)
{
    MarkdownEditor *editor = m_active->editor;
    if (mode == MarkdownEditor::Mode::Reading) {
        if (editor->mode() != MarkdownEditor::Mode::Reading)
            m_active->editingMode = editor->mode();
    } else {
        m_active->editingMode = mode;
    }
    editor->setMode(mode);
    updateModeButton();
    editor->setFocus();
}

void MainWindow::updateModeButton()
{
    const bool note = !m_active->notePath.isEmpty() && !m_active->view;
    m_modeButton->setVisible(note);
    if (!note)
        return;
    const QColor color = m_theme.color(QStringLiteral("--text-muted"));
    const MarkdownEditor::Mode mode = m_active->editor->mode();
    const char *current = mode == MarkdownEditor::Mode::Reading  ? "mode-reading"
                          : mode == MarkdownEditor::Mode::Source ? "mode-source"
                                                                 : "mode-live";
    for (QAction *action : m_modeButton->menu()->actions()) {
        action->setIcon(Icons::icon(action->data().toString(), color, 16));
        const bool on = action->objectName() == QLatin1String(current);
        action->setChecked(on);
        if (on)
            m_modeButton->setIcon(Icons::icon(action->data().toString(), color, 14));
    }
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    const bool narrow = width() < kNarrowWidth;
    if (m_drawer && narrow == m_narrow)
        openDrawer(m_drawer); // laid out again for the new size
    if (narrow == m_narrow || !m_sidebar || !m_rightSidebar)
        return;
    closeDrawer();
    m_narrow = narrow;
    // A sidebar in a window of its own is not short of width: left as it is.
    const bool leftHere = !popoutHolding(m_sidebar);
    const bool rightHere = !popoutHolding(m_rightSidebar);
    if (narrow) {
        if (leftHere) {
            m_leftBeforeNarrow = m_sidebar->isVisible();
            m_sidebar->hide();
        }
        if (rightHere) {
            m_rightBeforeNarrow = m_rightSidebar->isVisible();
            m_rightSidebar->hide();
        }
    } else if (m_openMode != OpenMode::SingleFile) {
        if (leftHere)
            m_sidebar->setVisible(m_leftBeforeNarrow);
        if (rightHere)
            m_rightSidebar->setVisible(m_rightBeforeNarrow);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!maybeSaveAll()) {
        event->ignore();
        return;
    }
    m_closing = true; // what is still unsaved was discarded or has just been saved
    for (const auto &leaf : m_leaves) {
        if (!leaf->notePath.isEmpty() && !leaf->view)
            m_settings.setNotePosition(leaf->notePath, leaf->editor->cursorOffset(),
                                       leaf->editor->verticalScrollBar()->value(),
                                       leaf->editor->foldState(), leaf->editor->topOffset());
    }
    saveLayout();
    // Every pop-out goes with the main window; the layout above already has them.
    while (!m_popouts.empty())
        deletePopout(m_popouts.back().get());
    m_settings.save();
    QSettings settings = windowSettings();
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
    DimMenu menu(this);
    QAction *newNoteAction = menu.addAction(tr("New note"));
    QAction *newFolderAction = menu.addAction(tr("New folder"));
    QAction *renameAction = nullptr;
    QAction *moveAction = nullptr;
    QAction *deleteAction = nullptr;
    QAction *copyAction = nullptr;
    QAction *openOutsideAction = nullptr;
#ifdef NOTEAPPC_HAVE_PANDOC
    QAction *importAction = nullptr;
#endif
    QAction *showAction = nullptr;
    QAction *iconAction = nullptr;
    QAction *removeIconAction = nullptr;
    const QString path = index.isValid() ? m_files->filePath(index) : QString();
    if (index.isValid()) {
        menu.addSeparator();
        renameAction = menu.addAction(tr("Rename"));
        moveAction = menu.addAction(tr("Move to..."));
        if (!m_files->isDir(index))
            copyAction = menu.addAction(tr("Make a copy"));
        deleteAction = menu.addAction(tr("Delete"));
        menu.addSeparator();
        if (!m_files->isDir(index))
            openOutsideAction = menu.addAction(tr("Open in default app"));
#ifdef NOTEAPPC_HAVE_PANDOC
        if (!m_files->isDir(index) && featureOn("pandoc")) {
            // Looked for now, with the menu: the entries stay when it is missing, drawn disabled, and say why.
            const bool missing = pandoc::locate(m_settings.text(QStringLiteral("pandoc.path"))).isEmpty();
            const auto dim = [missing](QAction *action) {
                if (missing) {
                    action->setProperty("dimmed", true);
                    action->setToolTip(QObject::tr("pandoc was not found: click to see how to get it"));
                }
            };
            if (Vault::isNote(path)) {
                auto *exportMenu = new DimMenu(tr("Export with pandoc"), &menu);
                menu.addMenu(exportMenu);
                dim(exportMenu->menuAction());
                for (const pandoc::Format &format : pandoc::exportFormats()) {
                    QAction *entry = exportMenu->addAction(format.label);
                    entry->setProperty("pandocFormat", format.id);
                    dim(entry);
                }
                QAction *other = exportMenu->addAction(tr("Other format..."));
                other->setProperty("pandocFormat", QStringLiteral("other"));
                dim(other);
            } else if (!pandoc::readerForSuffix(QFileInfo(path).suffix()).isEmpty()) {
                importAction = menu.addAction(tr("Import with pandoc"));
                dim(importAction);
            }
        }
#endif
        showAction = menu.addAction(tr("Show in system explorer"));
        if (fileIconsOn()) {
            menu.addSeparator();
            iconAction = menu.addAction(tr("Change icon..."));
            if (!m_fileIcons.iconFor(m_vault.relative(path)).isEmpty())
                removeIconAction = menu.addAction(tr("Remove icon"));
        }
    }

    QAction *chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == newNoteAction)
        newNoteIn(folderRelativeFor(index));
    else if (chosen == newFolderAction)
        newFolderIn(folderRelativeFor(index));
    else if (chosen == renameAction)
        m_tree->edit(index);
    else if (chosen == moveAction)
        chooseMoveFolder(path);
    else if (chosen == deleteAction)
        deletePathAt(index);
    else if (chosen == copyAction)
        copyPath(path);
    else if (chosen == openOutsideAction)
        openFileExternally(path, /*ask=*/false); // the menu entry is the person's own request
#ifdef NOTEAPPC_HAVE_PANDOC
    else if (chosen == importAction)
        importWithPandoc(path, m_vault.relative(QFileInfo(path).absolutePath()));
    else if (chosen->property("pandocFormat").isValid())
        exportWithPandoc(path, chosen->property("pandocFormat").toString());
#endif
    else if (chosen == showAction)
        // Limit: opens the folder; selecting the file in it needs the
        // FileManager1 D-Bus call, add if the folder alone is not enough.
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_files->isDir(index) ? path : QFileInfo(path).absolutePath()));
    else if (chosen == iconAction)
        chooseFileIcon(path);
    else if (chosen == removeIconAction)
        setFileIcon(path, QString());
}

bool MainWindow::saveTyping(const QString &absolutePath)
{
    for (const auto &leaf : m_leaves)
        if (leaf->notePath == absolutePath && isModified(leaf.get()))
            return saveLeaf(leaf.get(), true); // every tab on the note shows this one text
    return true;
}

QString MainWindow::copyPath(const QString &absolutePath)
{
    const QFileInfo info(absolutePath);
    if (!m_vault.isValid() || !m_vault.contains(absolutePath) || !info.isFile())
        return {};
    saveTyping(absolutePath); // the copy holds what is on screen
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    const QString base = info.completeBaseName();
    QString copy;
    for (int n = 1;; ++n) {
        copy = info.dir().filePath(QStringLiteral("%1 %2%3").arg(base).arg(n).arg(suffix));
        if (!QFileInfo::exists(copy) && !QFileInfo(copy).isSymLink())
            break;
    }
    if (!QFile::copy(absolutePath, copy)) {
        statusBar()->showMessage(tr("Could not copy %1").arg(info.fileName()), 5000);
        return {};
    }
    if (Vault::isNote(copy) || isViewFile(copy) || isNotebookFile(copy)) {
        m_indexer->reindex(copy);
        openNote(copy);
    }
    return copy;
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

QString MainWindow::newNoteFolder() const
{
    const QString location = m_settings.text(QStringLiteral("files.newNoteLocation"));
    QString folder;
    if (location == QLatin1String("current") && m_active && !m_active->notePath.isEmpty())
        folder = QDir(m_vault.root()).relativeFilePath(QFileInfo(m_active->notePath).absolutePath());
    else if (location == QLatin1String("folder"))
        folder = m_settings.text(QStringLiteral("files.newNoteFolder"));
    folder = QDir::cleanPath(folder);
    while (folder.startsWith(QLatin1Char('/')))
        folder.remove(0, 1);
    // "." is the root; anything climbing out of the vault is too.
    if (folder == QLatin1String(".") || folder == QLatin1String("..") || folder.startsWith(QLatin1String("../")))
        folder.clear();
    return folder;
}

void MainWindow::createNoteNamed(const QString &typed)
{
    if (!m_vault.isValid())
        return;
    // A bare name goes where New note puts one (newNoteFolder()); a typed
    // "Folder/Name" lands in that folder from the root, made when missing.
    QString name = typed;
    if (name.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
        name.chop(3);
    // Obsidian's invalid characters - the ones Vault::renamePath refuses.
    static const QRegularExpression invalid(QStringLiteral(R"([\[\]#^|:\\])"));
    const qsizetype slash = name.lastIndexOf(QLatin1Char('/'));
    QString folder = slash < 0 ? newNoteFolder() : QDir::cleanPath(name.left(slash).trimmed());
    if (folder == QLatin1String("."))
        folder.clear(); // [[./Name]]
    const QString base = name.mid(slash + 1).trimmed();
    QString error;
    QString path;
    if (base.isEmpty() || name.contains(invalid) || folder.split(QLatin1Char('/')).contains(QLatin1String("..")))
        error = tr("a note name cannot be empty or hold # ^ [ ] | : \\ or a .. folder");
    else if (const QString existing = m_vault.absolute((folder.isEmpty() ? QString() : folder + QLatin1Char('/')) + base + QLatin1String(".md"));
             QFileInfo(existing).isFile()) {
        // Not indexed yet (a scan in flight): open it, not a numbered copy.
        openNote(existing);
        return;
    } else
        path = m_vault.createNote(folder, base, &error);
    if (path.isEmpty()) {
        statusBar()->showMessage(tr("Could not create the note: %1").arg(error), 5000);
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

    QString error;
    if (!renameVaultPath(oldPath, isDir, newName, &error))
        QMessageBox::warning(this, tr("Rename"), tr("Could not rename %1:\n%2").arg(currentName, error));
}

bool MainWindow::renameVaultPath(const QString &oldPath, bool isDir, const QString &newName, QString *error)
{
    flushSaves(); // an autosave in flight would write the old name back
    // The link rewrite below reads each linking note from disk. Typed text
    // not on disk yet would be written back over it by the next autosave,
    // [[Old]] and all, so it goes first - whatever files.autosave says, a
    // rename is an explicit file operation. The watcher then shows the
    // rewritten note in the open editor. The index may not hold that note's
    // new links yet, so it is named to the rewrite, which reads it from disk.
    const QStringList unindexed = unindexedNotes(autosave());
    QString newPath;
    QString warning;
    if (!m_vault.renamePath(oldPath, newName, &newPath, error, &warning))
        return false;
    followPathChange(oldPath, isDir, newPath, unindexed);
    if (!warning.isEmpty())
        statusBar()->showMessage(tr("%1: %2").arg(QFileInfo(newPath).fileName(), warning), 8000);
    return true;
}

bool MainWindow::moveVaultPath(const QString &oldPath, bool isDir, const QString &targetFolder, QString *error)
{
    // As for a rename: typed text goes to disk before the file moves, and is
    // read from there for its links.
    const QStringList unindexed = unindexedNotes(autosave());
    QString newPath;
    QString warning;
    if (!m_vault.movePath(oldPath, targetFolder, &newPath, error, &warning))
        return false;
    if (newPath != oldPath)
        followPathChange(oldPath, isDir, newPath, unindexed);
    if (!warning.isEmpty())
        statusBar()->showMessage(tr("%1: %2").arg(QFileInfo(newPath).fileName(), warning), 8000);
    return true;
}

QStringList MainWindow::unindexedNotes(const QStringList &saved) const
{
    QStringList relative;
    for (const QString &path : saved + m_indexer->pathsInFlight()) {
        if (m_vault.contains(path) && Vault::isNote(path))
            relative << m_vault.relative(path);
    }
    return relative;
}

void MainWindow::followPathChange(const QString &oldPath, bool isDir, const QString &newPath,
                                  const QStringList &unindexed)
{
    const QString oldRelative = m_vault.relative(oldPath);
    const QString newRelative = m_vault.relative(newPath);
    if (m_fileIcons.renamePrefix(oldRelative, newRelative))
        m_fileIcons.save();
    m_settings.renameNotePosition(oldPath, newPath);
    notifyPlugins("note-renamed", newPath);
#ifdef NOTEAPPC_HAVE_SHARE
    if (m_share) { // a shared note keeps its document under its new name
        for (const ShareRecord &record : m_share->shares()) {
            if (record.relativePath == oldRelative)
                m_share->setPath(record.shareID, newRelative);
            else if (isDir && record.relativePath.startsWith(oldRelative + QLatin1Char('/')))
                m_share->setPath(record.shareID, newRelative + record.relativePath.mid(oldRelative.size()));
        }
    }
#endif
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
        if (m_index.updateLinksAfterRename(oldRelative, newRelative, m_vault, &linkError, unindexed) < 0) {
            statusBar()->showMessage(tr("Renamed, but links could not be updated: %1").arg(linkError),
                                     4000);
        } else if (!linkError.isEmpty()) {
            // A note that is not valid UTF-8 was left as it is.
            statusBar()->showMessage(tr("Renamed, but links in a note were not updated: %1").arg(linkError),
                                     6000);
        }
    }

    // A tab's note may be the renamed item itself, or nested inside a
    // renamed folder; either way its path just moved under it.
    for (const auto &leaf : m_leaves) {
        if (leaf->notePath.isEmpty())
            continue;
        const QString before = leaf->notePath;
        if (leaf->notePath == oldPath) {
            leaf->notePath = newPath;
            leaf->inlineTitle->setText(QFileInfo(newPath).completeBaseName());
        } else if (isDir && leaf->notePath.startsWith(oldPath + QLatin1Char('/'))) {
            leaf->notePath.replace(0, oldPath.size(), newPath);
        }
        if (leaf->notePath != before) { // a view keeps its own path: it must save to the new one
            leaf->editor->refreshLinkStatus(); // relative links are asked about from the new place
            followViewPath(leaf->view, leaf->notePath);
            moveNotebookState(before, leaf->notePath);
        }
    }
    for (const auto &group : m_groups)
        syncTabs(group.get());
    watchOpenNotes();
#ifdef NOTEAPPC_HAVE_SHARE
    syncShareViews();
#endif

    // Picks up the moved path(s) under their new name and drops the old
    // ones, notes and attachments alike - simpler and just as correct as
    // patching the renamed rows by hand, folder rename included.
    m_indexer->rebuild();
    updateTitle();
}

void MainWindow::moveToFolder(const QString &path, const QString &folder)
{
    if (!m_vault.isValid())
        return;
    QString error;
    if (!moveVaultPath(path, QFileInfo(path).isDir(), folder, &error))
        statusBar()->showMessage(tr("Could not move %1: %2").arg(QFileInfo(path).fileName(), error), 5000);
}

void MainWindow::chooseMoveFolder(const QString &path)
{
    if (!m_vault.isValid() || path.isEmpty() || !m_vault.contains(path) || !QFileInfo::exists(path))
        return;
    // Folders as vault-relative paths, "/" the root; not the hidden ones, not
    // the item's own folder or a subfolder of it.
    const QString vaultRoot = QStringLiteral("/");
    QStringList folders{vaultRoot};
    QDirIterator it(m_vault.root(), QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    const QString self = m_vault.relative(path);
    while (it.hasNext()) {
        const QString relative = m_vault.relative(it.next());
        if (relative.startsWith(QLatin1Char('.')) || relative.contains(QLatin1String("/.")))
            continue;
        if (QFileInfo(path).isDir() && (relative == self || relative.startsWith(self + QLatin1Char('/'))))
            continue;
        folders << relative;
    }
    folders.sort(Qt::CaseInsensitive);
    auto *picker = new QuickSwitcher(this);
    picker->setObjectName(QStringLiteral("moveFolderPicker"));
    picker->setPlaceholder(tr("Move %1 to...").arg(QFileInfo(path).fileName()));
    picker->setCandidates(folders);
    connect(picker, &QuickSwitcher::accepted, this, [this, path, vaultRoot, picker](const QString &folder) {
        picker->deleteLater();
        moveToFolder(path, folder == vaultRoot ? m_vault.root() : m_vault.absolute(folder));
    });
    connect(picker, &QDialog::rejected, picker, &QObject::deleteLater);
    picker->popUp();
}

void MainWindow::commitInlineTitle(Leaf *leaf)
{
    const QString current = QFileInfo(leaf->notePath).completeBaseName();
    const QString name = leaf->inlineTitle->text().trimmed();
    if (leaf->notePath.isEmpty() || name == current)
        return;
    QString error;
    if (!m_vault.isValid() || !m_vault.contains(leaf->notePath))
        error = tr("only notes in a vault can be renamed");
    else if (name.startsWith(QLatin1Char('.')))
        error = tr("a name cannot start with a dot");
    else
        renameVaultPath(leaf->notePath, false, name, &error);
    if (!error.isEmpty()) {
        statusBar()->showMessage(tr("Could not rename: %1").arg(error), 4000);
        leaf->inlineTitle->setText(current);
    }
    // On success every tab on the note, this one included, was retitled by
    // renameVaultPath; the field may still hold the untrimmed text.
    leaf->inlineTitle->setText(QFileInfo(leaf->notePath).completeBaseName());
}

bool MainWindow::deleteVaultPath(const QString &path, QString *error)
{
    flushSaves(); // an autosave in flight would write the file back after it is gone
    const QString mode = m_settings.text(QStringLiteral("files.deletedFiles"));
    if (mode == QLatin1String("permanent")) {
        const QFileInfo info(path);
        const bool done = info.isDir() && !info.isSymLink() ? QDir(path).removeRecursively() : QFile::remove(path);
        if (!done && error)
            *error = tr("cannot delete %1").arg(path);
        return done;
    }
    if (mode != QLatin1String("local")) {
        if (QFile::moveToTrash(path))
            return true;
        if (!m_vault.moveToTrash(path, error))
            return false;
        statusBar()->showMessage(tr("%1 has no system trash here: moved to the vault's .trash instead")
                                     .arg(QFileInfo(path).fileName()),
                                 6000);
        return true;
    }
    return m_vault.moveToTrash(path, error);
}

void MainWindow::deletePathAt(const QModelIndex &index)
{
    if (index.isValid())
        deletePath(m_files->filePath(index));
}

void MainWindow::deletePath(const QString &path)
{
    if (path.isEmpty() || !m_vault.isValid() || !m_vault.contains(path) || path == m_vault.root())
        return;
    const QString name = QFileInfo(path).fileName();

    if (m_settings.boolean(QStringLiteral("files.confirmDelete"))) {
        const QString mode = m_settings.text(QStringLiteral("files.deletedFiles"));
        const QString question = mode == QLatin1String("permanent") ? tr("Permanently delete %1?").arg(name)
                               : mode == QLatin1String("local")     ? tr("Move %1 to the vault's trash?").arg(name)
                                                                    : tr("Move %1 to the system trash?").arg(name);
        const auto answer = QMessageBox::question(this, tr("Delete"), question, QMessageBox::Yes | QMessageBox::No,
                                                  QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    QString error;
    if (!deleteVaultPath(path, &error)) {
        QMessageBox::warning(this, tr("Delete"), tr("Could not delete %1:\n%2").arg(name, error));
        return;
    }
    if (m_fileIcons.removeUnder(m_vault.relative(path)))
        m_fileIcons.save();

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
    m_settings.forgetNotePosition(path); // after the tabs closed: closing one records its place
    m_indexer->rebuild();
    updateTitle();
    updateWordCount();
}

} // namespace noteAppC
