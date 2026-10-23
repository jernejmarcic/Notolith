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
/// One cache entry per path, replaced when the file's mtime or size differs
/// from what the entry was read at, so an edited file is read again the next
/// time its block is laid out (the same moment a `.base` embed notices) and
/// the cache never holds two versions of one file. The generation counter
/// drops a read that a newer one has superseded.
///
/// Capped twice, so a large file can neither stall layout nor fill memory:
/// at most kCSVmaxRows data rows are kept and laid out (the rest are counted
/// for the footer), and at most kCSVmaxBytes of the file are read at all.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include "CSVparser.h"

#include <QFile>
#include <QFileInfo>
#include <QtConcurrent>

namespace noteAppC {

namespace {

constexpr int kCSVmaxRows = 500;
// A wide file would otherwise lay out 500 x its columns cells (82 ms for 30
// columns on an idle-priority core): the rows shown shrink with the width.
constexpr int kCSVmaxCells = 8000;
constexpr qint64 kCSVmaxBytes = 32 * 1024 * 1024;

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
                                                          const QString &unreadable)
{
    CSVcacheEntry out;
    out.pending = false;
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
    if (out.partial)
        bytes.truncate(bytes.lastIndexOf('\n') + 1); // whole records only
    const QString text = QString::fromUtf8(bytes);

    CSVtable parsed = parseCSV(text, tsv ? QLatin1Char('\t') : CSVdelimiter(text), kCSVmaxRows + 1);
    if (parsed.rows.isEmpty())
        return out;
    for (const QString &cell : parsed.rows.takeFirst())
        out.table.headers << forDisplay(cell);
    out.hiddenRows = parsed.totalRows - 1 - parsed.rows.size();
    out.table.rows.reserve(parsed.rows.size());
    for (const QStringList &record : std::as_const(parsed.rows)) {
        BaseTableRow row;
        for (const QString &cell : record)
            row.cells << forDisplay(cell);
        out.table.rows << std::move(row);
    }
    return out;
}

bool MarkdownEditor::layoutCSVembed(BlockView &view, const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile())
        return false;

    const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
    const qint64 size = info.size();
    auto it = d->m_CSVcache.find(path);
    if (it == d->m_CSVcache.end() || it->mtime != mtime || it->size != size) {
        CSVcacheEntry entry;
        entry.mtime = mtime;
        entry.size = size;
        entry.generation = ++d->m_CSVgeneration;
        it = d->m_CSVcache.insert(path, entry);
        const quint64 generation = entry.generation;
        const bool tsv = info.suffix().compare(QLatin1String("tsv"), Qt::CaseInsensitive) == 0;
        const QString unreadable = tr("Could not read %1").arg(info.fileName());
        // Through m_lifeline, as every other embed's worker: the editor may
        // be gone by the time the file has been parsed.
        (void)QtConcurrent::run([this, lifeline = d->m_lifeline, path, tsv, unreadable, generation, mtime, size] {
            CSVcacheEntry result = readCSVfile(path, tsv, unreadable);
            result.mtime = mtime;
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
    view.csvPath = path;
    hideWholeBlockText(view);

    if (it->pending)
        return layoutPlainMessage(view, tr("Loading…"), d->m_theme.color(QStringLiteral("--text-muted")));
    if (!it->table.error.isEmpty())
        return layoutPlainMessage(view, it->table.error, d->m_theme.color(QStringLiteral("--text-error")));
    if (it->table.headers.isEmpty())
        return layoutPlainMessage(view, tr("Empty file"), d->m_theme.color(QStringLiteral("--text-muted")));

    const int shown = qBound(1, kCSVmaxCells / int(it->table.headers.size()), kCSVmaxRows);
    const qsizetype hidden = it->hiddenRows + qMax<qsizetype>(0, it->table.rows.size() - shown);
    QString more;
    if (it->partial)
        more = tr("and more rows (the file is over %1 MB)").arg(kCSVmaxBytes >> 20);
    else if (hidden == 1)
        more = tr("and 1 more row");
    else if (hidden > 1)
        more = tr("and %1 more rows").arg(hidden);
    return layoutDataTable(view, it->table, shown, more);
}

void MarkdownEditor::deliverCSV(const QString &path, quint64 generation, CSVcacheEntry result)
{
    auto it = d->m_CSVcache.find(path);
    if (it == d->m_CSVcache.end() || it->generation != generation)
        return; // the file changed again, and a newer read is on its way
    *it = std::move(result);

    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        if (block.csvPath != path)
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
