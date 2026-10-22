#include "InkView.h"

#include <QAction>
#include <QActionGroup>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QScrollBar>
#include <QToolBar>
#include <QVBoxLayout>

#include "Settings.h"
#include "Theme.h"
#include "Vault.h"
#include "core/InkFormat.h"

namespace noteAppC::ink {

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
    m_penAction->setObjectName(QStringLiteral("inkPenAction"));
    m_eraserAction->setObjectName(QStringLiteral("inkEraserAction"));
    for (QAction *a : {m_penAction, m_eraserAction})
        a->setCheckable(true);
    m_penAction->setChecked(true);
    connect(m_penAction, &QAction::triggered, this, [this] { m_canvas->setTool(InkCanvas::Tool::Pen); });
    connect(m_eraserAction, &QAction::triggered, this, [this] { m_canvas->setTool(InkCanvas::Tool::Eraser); });
    m_undoAction = new QAction(tr("Undo"), this);
    m_redoAction = new QAction(tr("Redo"), this);
    connect(m_undoAction, &QAction::triggered, m_canvas, &InkCanvas::undo);
    connect(m_redoAction, &QAction::triggered, m_canvas, &InkCanvas::redo);
    m_toolBar->addActions({m_penAction, m_eraserAction});
    m_toolBar->addSeparator();
    m_toolBar->addActions({m_undoAction, m_redoAction});
    auto *readOnlyLabel = new QLabel(tr("Read-only: written by a newer Notolith"));
    readOnlyLabel->setObjectName(QStringLiteral("inkReadOnlyLabel"));
    m_readOnlyAction = m_toolBar->addWidget(readOnlyLabel);
    m_readOnlyAction->setVisible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_toolBar);
    layout->addWidget(m_area, 1);

    connect(m_canvas, &InkCanvas::documentEdited, this, &InkView::onEdited);
    connect(m_canvas, &InkCanvas::toolChanged, this, [this](InkCanvas::Tool tool) { // the keys P and E (R14) move the toolbar too
        m_penAction->setChecked(tool != InkCanvas::Tool::Eraser);
        m_eraserAction->setChecked(tool == InkCanvas::Tool::Eraser);
    });
    connect(m_canvas, &InkCanvas::panRequested, this, [this](QPoint delta) {
        m_area->horizontalScrollBar()->setValue(m_area->horizontalScrollBar()->value() - delta.x());
        m_area->verticalScrollBar()->setValue(m_area->verticalScrollBar()->value() - delta.y());
    });
    if (m_settings)
        applySettings(*m_settings); // the window calls applySettings() again after every change
    onEdited();
}

void InkView::applySettings(const Settings &settings)
{
    m_canvas->setDrawWithMouse(settings.value(QStringLiteral("ink.drawWithMouse")).toBool());
    m_canvas->setRawInput(settings.value(QStringLiteral("ink.rawInput")).toBool());
}

void InkView::onEdited()
{
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
    const DiskStamp stamp = stampOf(path); // before reading: a change during the read is seen at the next save
    InkReadResult result;
    if (!loadInkFile(path, m_canvas->document(), &result)) {
        if (error) *error = result.error;
        return false;
    }
    m_path = path;
    m_stamp = stamp;
    m_readOnly = result.readOnly;
    m_canvas->setReadOnly(m_readOnly); // not setEnabled(false): a disabled widget would swallow wheel and touch pan too
    m_penAction->setEnabled(!m_readOnly);
    m_eraserAction->setEnabled(!m_readOnly);
    m_readOnlyAction->setVisible(m_readOnly);
    if (m_readOnly)
        emit statusMessage(tr("%1 was written by a newer Notolith; opened read-only").arg(QFileInfo(path).fileName()), 6000);
    m_canvas->documentReplaced();
    onEdited(); // a reload of a modified view clears the mark: the signal goes out from here
    return true;
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
