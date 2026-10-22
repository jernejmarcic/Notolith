#include "InkView.h"

#include <QAction>
#include <QActionGroup>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtConcurrent>
#include <QLabel>
#include <QScrollBar>
#include <QToolBar>
#include <QVBoxLayout>

#include "Settings.h"
#include "Theme.h"
#include "Vault.h"
#include "core/InkFormat.h"

namespace noteAppC::ink {

/// What the pool hands back: the parsed document and how the parse went.
struct InkView::Parsed {
    InkDocument document;
    InkReadResult result;
};

InkView::InkView(const Settings *settings, QWidget *parent)
    : ModuleView(parent), m_settings(settings), m_area(new QScrollArea(this)), m_canvas(new InkCanvas),
      m_toolBar(new QToolBar(this))
{
    setObjectName(QStringLiteral("inkView"));
    m_canvas->setObjectName(QStringLiteral("inkCanvas"));
    m_area->setWidget(m_canvas);
    m_area->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_area->setWidgetResizable(false);

    auto *tools = new QActionGroup(this);
    m_penAction = tools->addAction(tr("Pen"));
    m_eraserAction = tools->addAction(tr("Eraser"));
    m_selectAction = tools->addAction(tr("Select"));
    m_selectAction->setObjectName(QStringLiteral("inkSelectAction"));
    m_selectAction->setToolTip(tr("Select with a lasso, then move, resize, copy or delete (L)"));
    m_penAction->setObjectName(QStringLiteral("inkPenAction"));
    m_eraserAction->setObjectName(QStringLiteral("inkEraserAction"));
    for (QAction *a : {m_penAction, m_eraserAction, m_selectAction})
        a->setCheckable(true);
    m_penAction->setChecked(true);
    connect(m_penAction, &QAction::triggered, this, [this] { m_canvas->setTool(InkCanvas::Tool::Pen); });
    connect(m_eraserAction, &QAction::triggered, this, [this] { m_canvas->setTool(InkCanvas::Tool::Eraser); });
    connect(m_selectAction, &QAction::triggered, this, [this] { m_canvas->setTool(InkCanvas::Tool::Lasso); });
    m_selectRectAction = new QAction(tr("Rectangle"), this); // checked: the select tool draws a box; unchecked: a lasso
    m_selectRectAction->setObjectName(QStringLiteral("inkSelectRectAction"));
    m_selectRectAction->setCheckable(true);
    m_selectRectAction->setToolTip(tr("Select with a rectangle instead of a lasso (Shift+L)"));
    connect(m_selectRectAction, &QAction::triggered, this, [this](bool on) {
        m_canvas->setTool(InkCanvas::Tool::Lasso); // one click is enough: the toggle also picks the select tool
        m_canvas->setSelectShape(on ? InkCanvas::SelectShape::Rect : InkCanvas::SelectShape::Lasso);
    });
    m_splitAction = new QAction(tr("Erase parts"), this); // checked: only the touched part of a stroke goes; unchecked: the whole stroke
    m_splitAction->setObjectName(QStringLiteral("inkEraserSplitAction"));
    m_splitAction->setCheckable(true);
    m_splitAction->setToolTip(tr("Erase parts of strokes instead of whole strokes (Shift+E)"));
    connect(m_splitAction, &QAction::triggered, this, [this](bool on) {
        m_canvas->setEraserMode(on ? InkCanvas::EraserMode::Split : InkCanvas::EraserMode::Stroke);
    });
    m_undoAction = new QAction(tr("Undo"), this);
    m_redoAction = new QAction(tr("Redo"), this);
    connect(m_undoAction, &QAction::triggered, m_canvas, &InkCanvas::undo);
    connect(m_redoAction, &QAction::triggered, m_canvas, &InkCanvas::redo);
    m_toolBar->addActions({m_penAction, m_eraserAction, m_splitAction, m_selectAction, m_selectRectAction});
    m_toolBar->addSeparator();
    m_toolBar->addActions({m_undoAction, m_redoAction});
    auto *readOnlyLabel = new QLabel(tr("Read-only: written by a newer Notolith"));
    readOnlyLabel->setObjectName(QStringLiteral("inkReadOnlyLabel"));
    m_readOnlyAction = m_toolBar->addWidget(readOnlyLabel);
    m_readOnlyAction->setVisible(false);
    auto *loadingLabel = new QLabel(tr("Loading..."));
    loadingLabel->setObjectName(QStringLiteral("inkLoadingLabel"));
    m_loadingAction = m_toolBar->addWidget(loadingLabel);
    m_loadingAction->setObjectName(QStringLiteral("inkLoadingAction"));
    m_loadingAction->setVisible(false);
    m_errorLabel = new QLabel;
    m_errorLabel->setObjectName(QStringLiteral("inkErrorLabel"));
    m_errorAction = m_toolBar->addWidget(m_errorLabel);
    m_errorAction->setObjectName(QStringLiteral("inkErrorAction"));
    m_errorAction->setVisible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_toolBar);
    layout->addWidget(m_area, 1);

    connect(m_canvas, &InkCanvas::documentEdited, this, &InkView::onEdited);
    connect(m_canvas, &InkCanvas::toolChanged, this, [this](InkCanvas::Tool tool) { // the keys P and E (R14) move the toolbar too
        m_penAction->setChecked(tool != InkCanvas::Tool::Eraser && tool != InkCanvas::Tool::Lasso);
        m_eraserAction->setChecked(tool == InkCanvas::Tool::Eraser);
        m_selectAction->setChecked(tool == InkCanvas::Tool::Lasso);
    });
    connect(m_canvas, &InkCanvas::selectShapeChanged, this, [this](InkCanvas::SelectShape shape) { // Shift+L moves the toolbar too
        m_selectRectAction->setChecked(shape == InkCanvas::SelectShape::Rect);
    });
    connect(m_canvas, &InkCanvas::eraserModeChanged, this, [this](InkCanvas::EraserMode mode) { // Shift+E moves the toolbar too
        m_splitAction->setChecked(mode == InkCanvas::EraserMode::Split);
    });
    connect(m_canvas, &InkCanvas::panRequested, this, [this](QPoint delta) {
        m_area->horizontalScrollBar()->setValue(m_area->horizontalScrollBar()->value() - delta.x());
        m_area->verticalScrollBar()->setValue(m_area->verticalScrollBar()->value() - delta.y());
    });
    connect(m_canvas, &InkCanvas::originShifted, this, [this](QPoint delta) { // the Infinite sheet grew left or up: stay on the same ink
        m_area->horizontalScrollBar()->setValue(m_area->horizontalScrollBar()->value() + delta.x());
        m_area->verticalScrollBar()->setValue(m_area->verticalScrollBar()->value() + delta.y());
    });
    if (m_settings)
        applySettings(*m_settings); // the window calls applySettings() again after every change
    onEdited();
}

void InkView::applySettings(const Settings &settings)
{
    m_canvas->setDrawWithMouse(settings.value(QStringLiteral("ink.drawWithMouse")).toBool());
    m_canvas->setRawInput(settings.value(QStringLiteral("ink.rawInput")).toBool());
    const QVariant mb = settings.value(QStringLiteral("ink.tileCacheMB")); // R5; unregistered (a bare test) keeps the default
    m_canvas->setTileCacheMB(mb.isValid() ? mb.toInt() : TileCache::kDefaultMaxBytes / (1024 * 1024));
}

void InkView::onEdited()
{
    // A page layout is a column of pages centred in the view; a sheet starts at the left edge, so growing it
    // left or up can be followed by scrolling (a centred widget that gets wider moves by half the growth).
    const auto layout = m_canvas->document().settings().layout;
    const bool sheet = layout == LayoutKind::SemiInfinite || layout == LayoutKind::Infinite;
    const Qt::Alignment want = (sheet ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignTop;
    if (m_area->alignment() != want)
        m_area->setAlignment(want);
    m_undoAction->setEnabled(m_canvas->document().canUndo() && !m_readOnly);
    m_redoAction->setEnabled(m_canvas->document().canRedo() && !m_readOnly);
    const bool modified = isModified();
    if (modified != m_lastModified) {
        m_lastModified = modified;
        emit modificationChanged(modified);
    }
}

InkView::DiskStamp InkView::stampOf(const QString &path)
{
    const QFileInfo info(path); // a new object: stats the file now
    return info.exists() ? DiskStamp{info.size(), info.lastModified().toMSecsSinceEpoch()} : DiskStamp{};
}

bool InkView::load(const QString &path, QString *error)
{
    if (m_settings)
        applySettings(*m_settings); // R5: the tile budget is read when a file opens
    if (m_loadWatcher) { // a parse of an older load is still running: its result is not wanted
        m_loadWatcher->disconnect(this);
        m_loadWatcher->deleteLater();
        m_loadWatcher = nullptr;
        m_loadingAction->setVisible(false);
    }
    const DiskStamp stamp = stampOf(path); // before reading: a change during the read is seen at the next save
    if (stamp.size > kOffThreadParseBytes && stamp.size <= kMaxInflatedBytes) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = tr("%1 could not be opened: %2").arg(QFileInfo(path).fileName(), file.errorString());
            return false;
        }
        const QByteArray bytes = file.readAll(); // the read is cheap, the parse is not (about ten times the file in time and memory)
        const QString name = QFileInfo(path).fileName();
        m_loadingAction->setVisible(true);
        m_canvas->setReadOnly(true); // no pen on a document that is about to be replaced
        m_loadWatcher = new QFutureWatcher<Parsed>(this);
        connect(m_loadWatcher, &QFutureWatcherBase::finished, this, [this, path, stamp, name] {
            Parsed parsed = m_loadWatcher->result();
            m_loadWatcher->deleteLater();
            m_loadWatcher = nullptr;
            m_loadingAction->setVisible(false);
            if (!parsed.result.ok) {
                m_stamp = stamp; // the unreadable file is looked at once, not at every autosave (as in resolveExternalChange)
                emit statusMessage(parsed.result.error, 8000);
                if (m_path.isEmpty()) { // nothing was ever shown: a blank tab that takes a pen and cannot save is a trap
                    m_loadFailed = true;
                    m_canvas->setReadOnly(true);
                    m_penAction->setEnabled(false);
                    m_eraserAction->setEnabled(false);
                    m_splitAction->setEnabled(false);
                    m_selectAction->setEnabled(false);
                    m_selectRectAction->setEnabled(false);
                    m_errorLabel->setText(parsed.result.error);
                    m_errorAction->setVisible(true);
                } else {
                    m_canvas->setReadOnly(m_readOnly); // the document it had before this load
                }
                return;
            }
            m_canvas->document() = std::move(parsed.document);
            adopt(path, stamp, parsed.result);
        });
        m_loadWatcher->setFuture(QtConcurrent::run([bytes, name] {
            Parsed parsed;
            parsed.result = readInk(bytes, parsed.document, name);
            return parsed;
        }));
        return true;
    }
    InkReadResult result;
    if (!loadInkFile(path, m_canvas->document(), &result)) {
        m_canvas->setReadOnly(m_readOnly || m_loadFailed); // an interrupted big load had made it read-only for its parse
        if (error) *error = result.error;
        return false;
    }
    adopt(path, stamp, result);
    return true;
}

/// The document is in the canvas: take over what the file said about it.
void InkView::adopt(const QString &path, const DiskStamp &stamp, const InkReadResult &result)
{
    m_path = path;
    m_stamp = stamp;
    m_loadFailed = false;
    m_errorAction->setVisible(false);
    m_readOnly = result.readOnly;
    m_canvas->setReadOnly(m_readOnly); // not setEnabled(false): a disabled widget would swallow wheel and touch pan too
    m_penAction->setEnabled(!m_readOnly);
    m_eraserAction->setEnabled(!m_readOnly);
    m_splitAction->setEnabled(!m_readOnly);
    m_selectAction->setEnabled(!m_readOnly);
    m_selectRectAction->setEnabled(!m_readOnly);
    m_readOnlyAction->setVisible(m_readOnly);
    if (m_readOnly)
        emit statusMessage(tr("%1 was written by a newer Notolith; opened read-only").arg(QFileInfo(path).fileName()), 6000);
    {
        const QSignalBlocker noCompensation(m_canvas); // a different document is not a shift of the old one: no scrolling to compensate
        m_canvas->documentReplaced();
    }
    onEdited(); // a reload of a modified view clears the mark: the signal goes out from here
}

/// "<name> (conflict yyyy-MM-dd HHmmss).ink" beside the file; the sync engine's own copies are
/// "(sync conflict, <device>, <stamp>)" but a module has no device ID. A second one in the same
/// second gets a counter.
QString InkView::conflictPathFor(const QString &path)
{
    const QFileInfo info(path);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HHmmss"));
    const QDir dir = info.dir();
    QString candidate = QStringLiteral("%1 (conflict %2).ink").arg(info.completeBaseName(), stamp);
    for (int n = 2; dir.exists(candidate); ++n)
        candidate = QStringLiteral("%1 (conflict %2 %3).ink").arg(info.completeBaseName(), stamp).arg(n);
    return dir.filePath(candidate);
}

/// The file changed since this view read or wrote it (another machine's sync, another tab): never
/// overwrite it. Nothing local to lose: show the disk version. Local edits: keep them in a conflict
/// file, then show the disk version.
bool InkView::resolveExternalChange(QString *error)
{
    const QString name = QFileInfo(m_path).fileName();
    QString conflict;
    if (isModified()) {
        conflict = conflictPathFor(m_path);
        if (!Vault::writeBytes(conflict, writeInk(m_canvas->document()), error))
            return false; // the edits stay in the view; the disk file is untouched
    }
    const DiskStamp now = stampOf(m_path);
    QString why;
    if (!load(m_path, &why)) {
        m_stamp = now; // the unreadable file is looked at once, not at every autosave
        if (error)
            *error = conflict.isEmpty() ? tr("%1 changed on disk and cannot be read: %2").arg(name, why)
                                        : tr("%1 changed on disk and cannot be read (%2); your edits are in %3")
                                              .arg(name, why, QFileInfo(conflict).fileName());
        return false;
    }
    emit statusMessage(conflict.isEmpty()
                           ? tr("%1 changed on disk: reloaded").arg(name)
                           : tr("%1 changed on disk while you were editing it: your version is in %2")
                                 .arg(name, QFileInfo(conflict).fileName()),
                       8000);
    return true;
}

bool InkView::save(QString *error)
{
    if (m_loadWatcher) { // the canvas holds the document the load is about to replace
        if (error) *error = tr("still loading");
        return false;
    }
    if (m_readOnly) {
        if (error) *error = tr("read-only: written by a newer Notolith");
        return false;
    }
    if (m_path.isEmpty()) { // nothing loaded: there is no file to write
        if (error) *error = tr("no file");
        return false;
    }
    if (const DiskStamp now = stampOf(m_path); now.size >= 0 && now != m_stamp)
        return resolveExternalChange(error);
    // The vault's atomic replace (spec section 5): writeInk() builds the bytes, the host writes them.
    if (!Vault::writeBytes(m_path, writeInk(m_canvas->document()), error))
        return false;
    m_stamp = stampOf(m_path);
    m_canvas->document().markSaved();
    onEdited();
    return true;
}

void InkView::setTheme(const Theme &theme)
{
    QPalette palette = m_area->viewport()->palette();
    palette.setColor(QPalette::Window, theme.color(QStringLiteral("--background-secondary")));
    m_area->viewport()->setPalette(palette);
    m_area->viewport()->setAutoFillBackground(true);
}

} // namespace noteAppC::ink
