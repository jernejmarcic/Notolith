/// `![[data.csv]]` and `![[data.tsv]]`: the file drawn as a table, through
/// the same layoutDataTable a ```` ```base ```` block's rows use - so it
/// looks like every other table in the editor, and paintTable's row culling
/// is what keeps a long one cheap to draw. Read this file's neighbour
/// BaseBlock.cpp first; the contract is the same one.
///
/// Reading and parsing happen on a worker thread (QtConcurrent), never in
/// layout: a block shows "Loading..." until deliverCSV lands, the way a base
/// does until its runner answers. libs/editor has the parser itself
/// (CSVparser.h) and needs nothing from libs/ext, so no runner is registered
/// by the window - the editor reads the file on its own.
///
/// One cache entry per path, replaced when the file's mtime, ctime or size differs
/// from what the entry was read at; the old table stays on screen until the
/// new one lands. A QFileSystemWatcher, made on the first CSV embed and not
/// before, holds the cached paths and their folders: a change (debounced)
/// stats them and relays out only the blocks whose file moved, so a file
/// rewritten by another program shows with nothing else happening. The
/// generation counter drops a read that a newer one has superseded. A failed
/// read stays shown until the file's ctime moves (a chmod is how it becomes
/// readable): that is one read again, not one per layout.
///
/// Bounded three ways, so a hostile or merely large file can neither stall
/// layout nor fill memory: rows and cells shown (kCSVmaxRows, kCSVmaxCells -
/// laid out on the UI thread on every width change, so the cell budget is
/// what a 10 ms relayout allows), columns and cell length (kCSVmaxColumns,
/// kCSVmaxCellChars - applied in the parser, before anything is stored), and
/// bytes read (kCSVmaxBytes).

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include "CSVparser.h"

#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QtConcurrent>

namespace noteAppC {

namespace {

constexpr int kCSVmaxRows = 500;
// ponytail: every shown cell is a QTextLayout built on the UI thread on each
// relayout (a width change repeats it per frame). This budget keeps a
// worst-case relayout under 10 ms on the dev machine (numbers in
// docs/EDITOR.md); the upgrade is laying out only the rows near the viewport.
constexpr int kCSVmaxCells = 1500;
constexpr int kCSVmaxColumns = 64;
constexpr int kCSVmaxCellChars = 1000;
constexpr qint64 kCSVmaxBytes = 32 * 1024 * 1024;

// setCSVreadHookForTests': read by worker threads, so atomic.
std::atomic<void (*)(const QString &)> gCSVreadHook{nullptr};

// A cell is drawn through a QTextLayout, where a line break inside one is
// U+2028 rather than a newline.
QString forDisplay(QString cell)
{
    cell.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    cell.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    cell.replace(QLatin1Char('\n'), QChar(QChar::LineSeparator));
    return cell;
}

} // namespace

// Runs on a worker thread: touches nothing but its arguments.
MarkdownEditor::CSVcacheEntry MarkdownEditor::readCSVfile(const QString &path, bool tsv,
                                                          const QString &unreadable,
                                                          const QString &moreColumns)
{
    CSVcacheEntry out;
    out.pending = false;
    if (const auto hook = gCSVreadHook.load())
        hook(path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        out.table.error = unreadable;
        return out;
    }
    QByteArray bytes = file.read(kCSVmaxBytes);
    if (file.error() != QFileDevice::NoError) {
        out.table.error = unreadable;
        return out;
    }
    out.partial = !file.atEnd();
    if (out.partial) {
        // whole records only - unless there is no line break to cut at, when
        // the one long line is all there will be
        const qsizetype lastBreak = bytes.lastIndexOf('\n');
        if (lastBreak >= 0)
            bytes.truncate(lastBreak + 1);
    }
    const QString text = decodeCSV(bytes);
    bytes.clear();

    CSVtable parsed = parseCSV(text, tsv ? QLatin1Char('\t') : CSVdelimiter(text), kCSVmaxRows + 1,
                               kCSVmaxColumns, kCSVmaxCellChars);
    if (parsed.rows.isEmpty())
        return out;
    out.hiddenColumns = parsed.hiddenColumns;
    for (const QString &cell : parsed.rows.takeFirst())
        out.table.headers << forDisplay(cell);
    if (parsed.hiddenColumns > 0)
        out.table.headers << moreColumns.arg(parsed.hiddenColumns);
    out.hiddenRows = parsed.totalRows - 1 - parsed.rows.size();
    out.table.rows.reserve(parsed.rows.size());
    for (const QStringList &record : std::as_const(parsed.rows)) {
        BaseTableRow row;
        for (const QString &cell : record)
            row.cells << forDisplay(cell);
        if (parsed.hiddenColumns > 0)
            row.cells << QString();
        out.table.rows << std::move(row);
    }
    return out;
}

bool MarkdownEditor::layoutCSVembed(BlockView &view, const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile())
        return false; // the resolver only answers with files that exist; this is a race with a delete
    view.CSVpath = path;
    watchCSV(path);

    const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
    const qint64 ctime = info.metadataChangeTime().toMSecsSinceEpoch();
    const qint64 size = info.size();
    auto it = d->m_CSVcache.find(path);
    if (it == d->m_CSVcache.end() || it->mtime != mtime || it->ctime != ctime || it->size != size) {
        // A re-read keeps the entry's old table: it stays on screen until
        // the new one lands (a fresh entry has none, and shows "Loading...").
        if (it == d->m_CSVcache.end())
            it = d->m_CSVcache.insert(path, CSVcacheEntry());
        it->mtime = mtime;
        it->ctime = ctime;
        it->size = size;
        it->pending = true;
        it->generation = ++d->m_CSVgeneration;
        if (!it->latest)
            it->latest = std::make_shared<std::atomic<quint64>>();
        it->latest->store(it->generation);
        const quint64 generation = it->generation;
        const auto latest = it->latest;
        const bool tsv = info.suffix().compare(QLatin1String("tsv"), Qt::CaseInsensitive) == 0;
        const QString unreadable = tr("Could not read %1").arg(info.fileName());
        const QString moreColumns = tr("+%1 columns");
        // Through m_lifeline, as every other embed's worker: the editor may
        // be gone by the time the file has been parsed.
        (void)QtConcurrent::run([this, lifeline = d->m_lifeline, latest, path, tsv, unreadable, moreColumns,
                                 generation, mtime, ctime, size] {
            if (latest->load() != generation)
                return; // a newer read of this file has been asked for: not worth the parse
            CSVcacheEntry result;
            try {
                result = readCSVfile(path, tsv, unreadable, moreColumns);
            } catch (...) { // out of memory, say: an error to show, never "Loading..." for good
                result.pending = false;
                result.table = {};
                result.table.error = unreadable;
            }
            result.mtime = mtime;
            result.ctime = ctime;
            result.size = size;
            result.generation = generation;
            lifeline->post(this, [this, path, generation, result = std::move(result)]() mutable {
                deliverCSV(path, generation, std::move(result));
            });
        });
    }

    // Painted as a data table, like a base's: paintBase with no view name.
    view.isBase = true;
    view.image = QPixmap();
    view.cells.clear();
    view.baseRows.clear();
    view.baseViewName.clear();
    view.baseYAML.clear();
    view.baseRequestedView.clear();
    hideWholeBlockText(view);

    const QColor muted = d->m_theme.color(QStringLiteral("--text-muted"));
    // the error first: a failed read being asked again keeps showing it, not "Loading..."
    if (!it->table.error.isEmpty())
        return layoutPlainMessage(view, it->table.error, d->m_theme.color(QStringLiteral("--text-error")));
    if (it->pending && it->table.headers.isEmpty())
        return layoutPlainMessage(view, tr("Loading…"), muted);
    if (it->table.headers.isEmpty())
        return layoutPlainMessage(view, tr("Empty file"), muted);

    const int shown = qBound(1, kCSVmaxCells / int(it->table.headers.size()), kCSVmaxRows);
    const qsizetype hidden = it->hiddenRows + qMax<qsizetype>(0, it->table.rows.size() - shown);
    QStringList more;
    if (it->partial)
        more << tr("and more rows (the file is over %1 MB)").arg(kCSVmaxBytes >> 20);
    else if (hidden == 1)
        more << tr("and 1 more row");
    else if (hidden > 1)
        more << tr("and %1 more rows").arg(hidden);
    if (it->hiddenColumns > 0)
        more << tr("%1 more columns not shown").arg(it->hiddenColumns);
    return layoutDataTable(view, it->table, shown, more.join(QStringLiteral(", ")));
}

void MarkdownEditor::deliverCSV(const QString &path, quint64 generation, CSVcacheEntry result)
{
    auto it = d->m_CSVcache.find(path);
    if (it == d->m_CSVcache.end() || it->generation != generation)
        return; // the file changed again, and a newer read is on its way
    const auto latest = it->latest;
    *it = std::move(result);
    it->latest = latest;
    relayCSVblocks(path);
}

void MarkdownEditor::relayCSVblocks(const QString &path)
{
    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        if (!block.isBase || block.CSVpath != path) // the flag is in the hot half of the struct
            continue;
        block.layout.reset();
        block.cells.clear();
        block.laidOut = false;
        block.heightEstimated = false;
        touched = true;
    }
    if (touched) {
        updateLayout();
        viewport()->update();
    }
}

void MarkdownEditor::watchCSV(const QString &path)
{
    // ponytail: one watcher per editor, a watch per path asked about and no
    // eviction before the feature goes off - fine for a note's handful of
    // tables, wasteful for an editor that walks hundreds of notes (hover
    // previews). Upgrade: one watcher shared by every editor, and an LRU of
    // 32 entries that unwatches what it drops.
    if (!d->m_CSVwatcher) {
        d->m_CSVwatcher = new QFileSystemWatcher(this);
        d->m_CSVrefresh = new QTimer(this);
        d->m_CSVrefresh->setSingleShot(true);
        d->m_CSVrefresh->setInterval(200); // a write in place can come in more than one go
        connect(d->m_CSVwatcher, &QFileSystemWatcher::fileChanged, this, &MarkdownEditor::CSVfileEvent);
        connect(d->m_CSVwatcher, &QFileSystemWatcher::directoryChanged, this, &MarkdownEditor::CSVfileEvent);
        connect(d->m_CSVrefresh, &QTimer::timeout, this, &MarkdownEditor::refreshVaultFiles);
    }
    // The file, and its folder: a program that saves by writing a new file
    // and renaming it over the old one makes inotify drop the file's watch.
    const QString folder = QFileInfo(path).absolutePath();
    if (!d->m_CSVwatcher->directories().contains(folder))
        d->m_CSVwatcher->addPath(folder);
    if (!d->m_CSVwatcher->files().contains(path))
        d->m_CSVwatcher->addPath(path);
}

void MarkdownEditor::CSVfileEvent()
{
    if (!d->m_CSVpendingSince.isValid())
        d->m_CSVpendingSince.start();
    // A debounce, but a file appended to every 100 ms must still show up:
    // past a second of waiting the running timer is left to fire.
    if (!d->m_CSVrefresh->isActive() || d->m_CSVpendingSince.elapsed() < 1000)
        d->m_CSVrefresh->start();
}

void MarkdownEditor::refreshCSVfiles()
{
    d->m_CSVpendingSince.invalidate();
    const QStringList paths = d->m_CSVcache.keys();
    for (const QString &path : paths) {
        const QFileInfo info(path);
        const auto cached = d->m_CSVcache.constFind(path);
        if (!info.isFile()) {
            // Gone: its read goes too, or a restore with the same mtime and
            // size (a move out of the trash, cp -p) would look like nothing
            // happened. The block lays out again as whatever the resolver
            // now makes of the link.
            d->m_CSVcache.remove(path);
            relayCSVblocks(path);
            continue;
        }
        watchCSV(path); // put back what a rename-over-save took away
        // A read already under way for this very mtime, ctime and size has its
        // own relayout coming; one that has moved on from its entry needs one.
        if (info.lastModified().toMSecsSinceEpoch() != cached->mtime
            || info.metadataChangeTime().toMSecsSinceEpoch() != cached->ctime || info.size() != cached->size)
            relayCSVblocks(path);
    }
}

void MarkdownEditor::dropCSVcache()
{
    d->m_CSVcache.clear();
    if (d->m_CSVwatcher) {
        d->m_CSVwatcher->removePaths(d->m_CSVwatcher->files());
        d->m_CSVwatcher->removePaths(d->m_CSVwatcher->directories());
    }
}

void MarkdownEditor::setCSVreadHookForTests(void (*hook)(const QString &))
{
    gCSVreadHook.store(hook);
}

int MarkdownEditor::CSVcacheSize() const
{
    return int(d->m_CSVcache.size());
}

QList<QStringList> MarkdownEditor::tableCellTextsAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return {};
    const BlockView &view = d->m_blocks[size_t(index)];
    int rows = 0, columns = 0;
    for (const Cell &cell : view.cells) {
        rows = qMax(rows, cell.row + 1);
        columns = qMax(columns, cell.column + 1);
    }
    QList<QStringList> grid(rows, QStringList(columns));
    for (const Cell &cell : view.cells) {
        if (cell.layout)
            grid[cell.row][cell.column] = cell.layout->text();
    }
    return grid;
}

} // namespace noteAppC
