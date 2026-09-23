/// The ```` ```base ```` block (obsidian.md/help/bases, "Embed a base"): a
/// fenced code block whose language is `base` shows its `.base` YAML as a
/// table - one row per matching note, one column per the shown view's
/// `order:` - in place of the YAML text, mirroring exactly what QueryBlock.cpp
/// already does for a ```query``` block; read that file's own header comment
/// first, since every design choice here is the same one made there for the
/// same reason.
///
/// libs/editor must not link libs/ext/bases (docs/PLUGINS.md's module
/// boundary), so MarkdownEditor only ever calls a callback the window
/// supplies - setBaseRunner - with the block's YAML text, and is handed the
/// result back through BaseDeliver, which may be called from any thread. A
/// real runner's work (bases::evaluateBaseForEditor) is cheap for the sizes
/// this project's own performance budget targets, but layout must never wait
/// on it regardless: a block shows "Loading base..." for however long that
/// takes and relays out only the blocks whose YAML it was evaluating once
/// deliverBaseResults gets called.
///
/// Results are cached by the YAML's own text (m_baseCache), not by block,
/// with the same generation-counter staleness guard QueryCacheEntry uses -
/// see QueryBlock.cpp's header comment for why.

#include "MarkdownEditor.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QScrollBar>
#include <QTextLayout>
#include <QTimer>

namespace noteAppC {

QList<MarkdownEditor::BaseRowRect> MarkdownEditor::baseRowsAt(qsizetype offset) const
{
    QList<BaseRowRect> rows;
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return rows;
    const BlockView &view = m_blocks[size_t(index)];
    if (!view.isBase)
        return rows;
    const qreal left = contentLeft();
    const qreal top = view.y - verticalScrollBar()->value();
    rows.reserve(int(view.baseRows.size()));
    for (const BlockView::BaseRow &row : view.baseRows)
        rows.append({row.rect.translated(left, top), row.path});
    return rows;
}

void MarkdownEditor::setBaseRunner(
    std::function<void(const QString &, const BaseDeliver &)> runner)
{
    m_baseRunner = std::move(runner);
    // Every cache entry was answered by the old runner, or is still waiting
    // on it; a stray delivery from it will still be dropped correctly since
    // ensureBaseSearch below hands out a fresh generation for the same text
    // - see setQueryRunner, the same trade-off for the same reason.
    m_baseCache.clear();
    bool touched = false;
    for (BlockView &block : m_blocks) {
        if (!block.isBase)
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

void MarkdownEditor::refreshBases()
{
    if (!m_baseRefreshTimer) {
        m_baseRefreshTimer = new QTimer(this);
        m_baseRefreshTimer->setSingleShot(true);
        m_baseRefreshTimer->setInterval(200);
        connect(m_baseRefreshTimer, &QTimer::timeout, this, [this] {
            m_baseCache.clear();
            bool touched = false;
            for (BlockView &block : m_blocks) {
                if (!block.isBase)
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
        });
    }
    m_baseRefreshTimer->start(); // restarts: a burst of index changes costs one refresh
}

const MarkdownEditor::BaseCacheEntry &MarkdownEditor::ensureBaseSearch(const QString &yamlText)
{
    auto it = m_baseCache.find(yamlText);
    if (it != m_baseCache.end())
        return *it;

    BaseCacheEntry entry;
    entry.pending = true;
    entry.generation = ++m_baseGeneration;
    it = m_baseCache.insert(yamlText, entry);

    if (m_baseRunner) {
        const quint64 generation = entry.generation;
        // May be called from a worker thread (the whole point: parsing and
        // evaluating a base must never run on, or block, the UI thread), so
        // the delivery is marshalled back onto this object's own thread
        // before it touches m_baseCache or a single BlockView - see
        // ensureQuerySearch for why capturing `this` by raw pointer is safe.
        m_baseRunner(yamlText, [this, yamlText, generation](BaseTableData results) {
            QMetaObject::invokeMethod(
                this,
                [this, yamlText, generation, results = std::move(results)]() mutable {
                    deliverBaseResults(yamlText, generation, std::move(results));
                },
                Qt::QueuedConnection);
        });
    }
    return *it;
}

void MarkdownEditor::deliverBaseResults(const QString &yamlText, quint64 generation,
                                        BaseTableData results)
{
    auto it = m_baseCache.find(yamlText);
    // Not the entry's current generation: refreshBases (or a fresh
    // setBaseRunner) started a newer evaluation for the same text while this
    // one was still in flight - see deliverQueryResults, the same check for
    // the same reason.
    if (it == m_baseCache.end() || it->generation != generation)
        return;
    it->results = std::move(results);
    it->pending = false;

    bool touched = false;
    for (BlockView &block : m_blocks) {
        if (!block.isBase)
            continue;
        const CodeFenceSpan *fence = codeFenceOf(block);
        if (!fence)
            continue;
        if (m_text.mid(fence->content.start, fence->content.length()).trimmed() != yamlText)
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

bool MarkdownEditor::layoutBase(BlockView &view)
{
    if (view.span.kind != BlockSpan::Kind::Code)
        return false;
    const CodeFenceSpan *fence = codeFenceOf(view);
    if (!fence || fence->language.compare(QLatin1String("base"), Qt::CaseInsensitive) != 0)
        return false;

    view.isBase = true;
    view.image = QPixmap();
    view.cells.clear();
    view.baseRows.clear();
    view.baseViewName.clear();

    const QString yamlText = m_text.mid(fence->content.start, fence->content.length()).trimmed();

    const QColor mutedColour = m_theme.color(QStringLiteral("--text-muted"));
    const QColor errorColour = m_theme.color(QStringLiteral("--text-error"));
    const qreal lineHeight = lineHeightFor({BlockSpan::Kind::Paragraph, {0, 0}, 0});

    // A plain, single-line message in place of a table - pending, empty,
    // erroring or a view with no columns all look like this, the same shape
    // layoutQuery's addPlainLine gives a ```query``` block before it has
    // results.
    const auto plainMessage = [&](const QString &text, const QColor &colour) {
        view.layout = std::make_unique<QTextLayout>(text);
        view.layout->setCacheEnabled(true);
        view.layout->setFont(m_theme.textFont());
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        view.layout->setTextOption(option);
        QTextLayout::FormatRange range;
        range.start = 0;
        range.length = int(text.size());
        range.format.setForeground(colour);
        view.layout->setFormats({range});

        view.layout->beginLayout();
        qreal height = 0;
        while (true) {
            QTextLine line = view.layout->createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(contentWidth());
            line.setPosition(QPointF(0, height));
            height += lineHeight;
        }
        view.layout->endLayout();

        view.textTop = 4;
        view.height = qMax(height, lineHeight) + view.textTop + 4;
        view.laidOut = true;
        return true;
    };

    if (yamlText.isEmpty())
        return plainMessage(tr("Empty base"), mutedColour);
    if (!m_baseRunner)
        return plainMessage(tr("No base source connected"), mutedColour);

    const BaseCacheEntry &entry = ensureBaseSearch(yamlText);
    if (entry.pending)
        return plainMessage(tr("Loading base…"), mutedColour);
    if (!entry.results.error.isEmpty())
        return plainMessage(entry.results.error, errorColour);
    if (entry.results.headers.isEmpty())
        return plainMessage(tr("This view has no columns"), mutedColour);
    if (entry.results.rows.isEmpty())
        return plainMessage(tr("No results"), mutedColour);

    // --- the table itself, laid out the way layoutTable lays a Markdown
    // table out: natural column widths from content, capped so one runaway
    // column cannot push the rest off screen, rows sized to their tallest
    // cell. The cells built here carry no `source`/`map` (there is no note
    // text behind a base row to point at), only what paintTable actually
    // reads: `layout`, `rect`, `alignment`, `row`.

    view.baseViewName = entry.results.shownView >= 0
                              && entry.results.shownView < entry.results.viewNames.size()
                         ? entry.results.viewNames.at(entry.results.shownView)
                         : QString();

    // ponytail: a flat cap rather than virtualised scrolling - the simplest
    // thing that stops a huge base from costing a huge layout. Upgrade path
    // if a vault-sized base view turns out to be a real use case: page the
    // cached BaseTableData itself instead of capping what is shown.
    constexpr int kMaxRows = 50;
    const bool truncated = entry.results.rows.size() > kMaxRows;
    const int shownRows = truncated ? kMaxRows : int(entry.results.rows.size());
    const int summaryRow = truncated ? shownRows + 1 : -1;
    const int totalRows = (truncated ? summaryRow : shownRows) + 1; // +1 for the header row

    const QFont font = m_theme.textFont();
    const qreal cellLineHeight = QFontMetricsF(font).height() * 1.4;
    const qreal padX = 10;
    const qreal padY = 6;
    const qreal minimum = QFontMetricsF(font).horizontalAdvance(QLatin1Char('m')) * 3;
    const int columns = entry.results.headers.size();
    const QColor textColour = m_theme.color(QStringLiteral("--text-normal"));

    QList<qreal> natural(columns, minimum);
    std::vector<Cell> cells;

    const auto addCell = [&](int row, int column, const QString &text, bool header,
                             const QColor &colour) {
        Cell cell;
        cell.row = row;
        cell.column = column;
        cell.layout = std::make_unique<QTextLayout>(text);
        cell.layout->setCacheEnabled(true);
        QTextOption wrap;
        wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        cell.layout->setTextOption(wrap);
        QFont cellFont = font;
        if (header)
            cellFont.setWeight(QFont::DemiBold);
        cell.layout->setFont(cellFont);
        QTextLayout::FormatRange range;
        range.start = 0;
        range.length = int(text.size());
        range.format.setForeground(colour);
        cell.layout->setFormats({range});
        natural[column] =
            qMax(natural.at(column), QFontMetricsF(cellFont).horizontalAdvance(text) + 2 * padX);
        cells.push_back(std::move(cell));
    };

    for (int c = 0; c < columns; ++c)
        addCell(0, c, entry.results.headers.at(c), true, textColour);
    for (int r = 0; r < shownRows; ++r) {
        const BaseTableRow &row = entry.results.rows.at(r);
        for (int c = 0; c < columns && c < row.cells.size(); ++c)
            addCell(r + 1, c, row.cells.at(c), false, textColour);
    }
    if (truncated) {
        addCell(summaryRow, 0, tr("and %1 more").arg(entry.results.rows.size() - kMaxRows), false,
               mutedColour);
    }

    // Obsidian does not squeeze a table into the readable column - see
    // layoutTable's own comment, which this mirrors exactly.
    const qreal available = contentWidth();
    const qreal columnCap = qMax(minimum * 4, available * 0.66);
    QList<qreal> widths;
    QList<qreal> offsets;
    qreal x = 0;
    for (const qreal width : std::as_const(natural)) {
        const qreal used = qBound(minimum, width, columnCap);
        widths << used;
        offsets << x;
        x += used;
    }
    view.tableWidth = x;

    QFont headingFont = m_theme.uiFont();
    headingFont.setWeight(QFont::DemiBold);
    view.textTop = view.baseViewName.isEmpty() ? 0 : QFontMetricsF(headingFont).height() + 10;

    QList<qreal> rowHeights(totalRows, cellLineHeight + 2 * padY);
    for (Cell &cell : cells) {
        const qreal width = widths.value(cell.column, minimum) - 2 * padX;
        cell.layout->beginLayout();
        qreal height = 0;
        while (true) {
            QTextLine line = cell.layout->createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(qMax<qreal>(10, width));
            line.setPosition(QPointF(0, height));
            height += cellLineHeight;
        }
        cell.layout->endLayout();
        rowHeights[cell.row] = qMax(rowHeights.at(cell.row), height + 2 * padY);
    }

    QList<qreal> rowTops;
    qreal y = view.textTop; // the heading strip, reserved above the grid
    for (const qreal height : std::as_const(rowHeights)) {
        rowTops << y;
        y += height;
    }
    for (Cell &cell : cells) {
        cell.rect = QRectF(offsets.value(cell.column), rowTops.value(cell.row),
                           widths.value(cell.column), rowHeights.value(cell.row));
    }

    // Only a data row's first cell opens its note - BaseTableModel::
    // isNameColumn's choice for the real widget, mirrored here.
    for (const Cell &cell : cells) {
        if (cell.column != 0 || cell.row == 0 || cell.row == summaryRow)
            continue;
        const int dataIndex = cell.row - 1;
        if (dataIndex < 0 || dataIndex >= shownRows)
            continue;
        view.baseRows.push_back({cell.rect, entry.results.rows.at(dataIndex).notePath});
    }

    view.cells = std::move(cells);
    view.headerRows = 1;
    view.tableScroll =
        qBound<qreal>(0, view.tableScroll, qMax<qreal>(0, view.tableWidth - contentWidth()));
    view.layout.reset();
    view.height = y;
    view.laidOut = true;
    return true;
}

void MarkdownEditor::paintBase(QPainter &painter, const BlockView &view, qreal left,
                               qreal top) const
{
    if (!view.baseViewName.isEmpty()) {
        QFont headingFont = m_theme.uiFont();
        headingFont.setWeight(QFont::DemiBold);
        painter.save();
        painter.setFont(headingFont);
        painter.setPen(m_theme.color(QStringLiteral("--text-normal")));
        painter.drawText(QRectF(left, top, contentWidth(), view.textTop),
                         Qt::AlignVCenter | Qt::AlignLeft, view.baseViewName);
        painter.restore();
    }
    paintTable(painter, view, left, top);
}

} // namespace noteAppC
