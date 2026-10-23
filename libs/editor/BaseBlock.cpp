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
#include "MarkdownEditor_p.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QScrollBar>
#include <QTextLayout>
#include <QTimer>

namespace noteAppC {

namespace {

// The cache (and the in-flight request) is keyed on the YAML together with
// the requested view, not the YAML alone: two embeds of the same .base file
// asking for different views must evaluate, cache and deliver separately.
// Left as plain concatenation, the way FileEmbedBlock.cpp's own cache keys
// are built, rather than a QPair - one QHash<QString, ...> already exists
// for the no-view case and this keeps it exactly as it was for `viewName`
// empty (a fence, or an embed with no fragment).
QString baseCacheKey(const QString &yamlText, const QString &viewName)
{
    return viewName.isEmpty() ? yamlText : yamlText + QLatin1Char('\x1e') + viewName;
}

} // namespace

QList<MarkdownEditor::BaseRowRect> MarkdownEditor::baseRowsAt(qsizetype offset) const
{
    QList<BaseRowRect> rows;
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return rows;
    const BlockView &view = d->m_blocks[size_t(index)];
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
    std::function<void(const QString &, const QString &, const BaseDeliver &)> runner)
{
    m_baseRunner = std::move(runner);
    // Every cache entry was answered by the old runner, or is still waiting
    // on it; a stray delivery from it will still be dropped correctly since
    // ensureBaseSearch below hands out a fresh generation for the same text
    // - see setQueryRunner, the same trade-off for the same reason.
    d->m_baseCache.clear();
    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        if (!block.isBase || !block.CSVpath.isEmpty()) // a CSV embed is no base's
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
    if (!d->m_baseRefreshTimer) {
        d->m_baseRefreshTimer = new QTimer(this);
        d->m_baseRefreshTimer->setSingleShot(true);
        d->m_baseRefreshTimer->setInterval(200);
        connect(d->m_baseRefreshTimer, &QTimer::timeout, this, [this] {
            d->m_baseCache.clear();
            bool touched = false;
            for (BlockView &block : d->m_blocks) {
                if (!block.isBase || !block.CSVpath.isEmpty()) // a CSV embed is no base's
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
    d->m_baseRefreshTimer->start(); // restarts: a burst of index changes costs one refresh
}

const MarkdownEditor::BaseCacheEntry &MarkdownEditor::ensureBaseSearch(const QString &yamlText,
                                                                        const QString &viewName)
{
    const QString key = baseCacheKey(yamlText, viewName);
    auto it = d->m_baseCache.find(key);
    if (it != d->m_baseCache.end())
        return *it;

    BaseCacheEntry entry;
    entry.pending = true;
    entry.generation = ++d->m_baseGeneration;
    it = d->m_baseCache.insert(key, entry);

    if (m_baseRunner) {
        const quint64 generation = entry.generation;
        // May be called from a worker thread (the whole point: parsing and
        // evaluating a base must never run on, or block, the UI thread), so
        // the delivery is marshalled back onto this object's own thread
        // before it touches m_baseCache or a single BlockView, through
        // m_lifeline: the editor may be gone by the time the base is ready.
        m_baseRunner(yamlText, viewName,
                     [this, lifeline = d->m_lifeline, yamlText, viewName, generation](BaseTableData results) {
                         lifeline->post(this, [this, yamlText, viewName, generation,
                                               results = std::move(results)]() mutable {
                             deliverBaseResults(yamlText, viewName, generation, std::move(results));
                         });
                     });
    }
    return *it;
}

void MarkdownEditor::deliverBaseResults(const QString &yamlText, const QString &viewName,
                                        quint64 generation, BaseTableData results)
{
    const QString key = baseCacheKey(yamlText, viewName);
    auto it = d->m_baseCache.find(key);
    // Not the entry's current generation: refreshBases (or a fresh
    // setBaseRunner) started a newer evaluation for the same text while this
    // one was still in flight - see deliverQueryResults, the same check for
    // the same reason.
    if (it == d->m_baseCache.end() || it->generation != generation)
        return;
    it->results = std::move(results);
    it->pending = false;

    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        if (!block.isBase || block.baseYAML != yamlText || block.baseRequestedView != viewName)
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
    if (!d->m_features.baseBlocks)
        return false; // the fence shows as a plain, uncoloured code listing
    const CodeFenceSpan *fence = codeFenceOf(view);
    if (!fence || fence->language.compare(QLatin1String("base"), Qt::CaseInsensitive) != 0)
        return false;

    return layoutBaseTable(view, d->m_text.mid(fence->content.start, fence->content.length()).trimmed());
}

bool MarkdownEditor::layoutPlainMessage(BlockView &view, const QString &text, const QColor &colour)
{
    // A plain, single-line message in place of a table - pending, empty,
    // erroring or a view with no columns all look like this, the same shape
    // layoutQuery's addPlainLine gives a ```query``` block before it has
    // results.
    const qreal lineHeight = lineHeightFor({BlockSpan::Kind::Paragraph, {0, 0}, 0});
    view.layout = std::make_unique<QTextLayout>(text);
    view.layout->setCacheEnabled(true);
    view.layout->setFont(d->m_theme.textFont());
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
}

bool MarkdownEditor::layoutBaseTable(BlockView &view, const QString &yamlText, const QString &viewName)
{
    view.isBase = true;
    view.image = QPixmap();
    view.cells.clear();
    view.baseRows.clear();
    view.baseViewName.clear();
    view.baseYAML = yamlText;
    view.baseRequestedView = viewName;

    const QColor mutedColour = d->m_theme.color(QStringLiteral("--text-muted"));
    const QColor errorColour = d->m_theme.color(QStringLiteral("--text-error"));
    const auto plainMessage = [&](const QString &text, const QColor &colour) {
        return layoutPlainMessage(view, text, colour);
    };

    if (yamlText.isEmpty())
        return plainMessage(tr("Empty base"), mutedColour);
    if (!m_baseRunner)
        return plainMessage(tr("No base source connected"), mutedColour);

    const BaseCacheEntry &entry = ensureBaseSearch(yamlText, viewName);
    if (entry.pending)
        return plainMessage(tr("Loading base…"), mutedColour);
    if (!entry.results.error.isEmpty())
        return plainMessage(entry.results.error, errorColour);
    if (entry.results.headers.isEmpty())
        return plainMessage(tr("This view has no columns"), mutedColour);
    if (entry.results.rows.isEmpty())
        return plainMessage(tr("No results"), mutedColour);

    view.baseViewName = entry.results.shownView >= 0
                              && entry.results.shownView < entry.results.viewNames.size()
                         ? entry.results.viewNames.at(entry.results.shownView)
                         : QString();

    // ponytail: a flat cap rather than virtualised scrolling - the simplest
    // thing that stops a huge base from costing a huge layout. Upgrade path
    // if a vault-sized base view turns out to be a real use case: page the
    // cached BaseTableData itself instead of capping what is shown.
    constexpr int kMaxRows = 50;
    return layoutDataTable(view, entry.results, kMaxRows,
                           entry.results.rows.size() > kMaxRows
                               ? tr("and %1 more").arg(entry.results.rows.size() - kMaxRows)
                               : QString());
}

bool MarkdownEditor::layoutDataTable(BlockView &view, const BaseTableData &data, int maxRows,
                                      const QString &moreText)
{
    const QColor mutedColour = d->m_theme.color(QStringLiteral("--text-muted"));
    // --- the table itself, a base's and a CSV file's alike, laid out the
    // way layoutTable lays a Markdown table out: natural column widths from
    // content, capped so one runaway column cannot push the rest off
    // screen, rows sized to their tallest cell. The cells built here carry
    // no `source`/`map` (there is no note text behind a base row to point
    // at), only what paintTable actually reads: `layout`, `rect`,
    // `alignment`, `row`.

    const bool truncated = !moreText.isEmpty();
    const int shownRows = int(qMin<qsizetype>(data.rows.size(), maxRows));
    const int summaryRow = truncated ? shownRows + 1 : -1;
    // The view's `summaries:`, a footer under its columns, as Obsidian's.
    const bool footer = !data.summaries.isEmpty();
    const int footerRow = footer ? (truncated ? summaryRow : shownRows) + 1 : -1;
    const int totalRows = (footer ? footerRow : (truncated ? summaryRow : shownRows)) + 1; // +1 for the header row

    const QFont font = d->m_theme.textFont();
    const qreal cellLineHeight = QFontMetricsF(font).height() * 1.4;
    const qreal padX = 10;
    const qreal padY = 6;
    const qreal minimum = QFontMetricsF(font).horizontalAdvance(QLatin1Char('m')) * 3;
    const int columns = data.headers.size();
    const QColor textColour = d->m_theme.color(QStringLiteral("--text-normal"));

    QList<qreal> natural(columns, minimum);
    std::vector<Cell> cells;
    cells.reserve(size_t(totalRows) * size_t(columns));

    // Hoisted out of the per-cell work: a table is thousands of cells.
    QFont headerFont = font;
    headerFont.setWeight(QFont::DemiBold);
    const QFontMetricsF metrics(font);
    const QFontMetricsF headerMetrics(headerFont);
    QTextOption wrap;
    wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    // `countsForWidth` false is the "and N more" footer, which spans the
    // whole table rather than deciding how wide its first column is.
    const auto addCell = [&](int row, int column, const QString &text, bool header,
                             const QColor &colour, bool countsForWidth = true) {
        Cell cell;
        cell.row = row;
        cell.column = column;
        cell.layout = std::make_unique<QTextLayout>(text);
        cell.layout->setCacheEnabled(true);
        cell.layout->setTextOption(wrap);
        cell.layout->setFont(header ? headerFont : font);
        QTextLayout::FormatRange range;
        range.start = 0;
        range.length = int(text.size());
        range.format.setForeground(colour);
        cell.layout->setFormats({range});
        if (countsForWidth) {
            natural[column] = qMax(natural.at(column),
                                   (header ? headerMetrics : metrics).horizontalAdvance(text) + 2 * padX);
        }
        cells.push_back(std::move(cell));
    };

    for (int c = 0; c < columns; ++c)
        addCell(0, c, data.headers.at(c), true, textColour);
    for (int r = 0; r < shownRows; ++r) {
        const BaseTableRow &row = data.rows.at(r);
        for (int c = 0; c < columns && c < row.cells.size(); ++c)
            addCell(r + 1, c, row.cells.at(c), false, textColour);
    }
    if (truncated) {
        addCell(summaryRow, 0, moreText, false, mutedColour, false);
    }
    for (int c = 0; footer && c < columns && c < data.summaries.size(); ++c)
        addCell(footerRow, c, data.summaries.at(c), false, mutedColour);

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
    view.sideWidth = x;

    QFont headingFont = d->m_theme.uiFont();
    headingFont.setWeight(QFont::DemiBold);
    view.textTop = view.baseViewName.isEmpty() ? 0 : QFontMetricsF(headingFont).height() + 10;

    QList<qreal> rowHeights(totalRows, cellLineHeight + 2 * padY);
    for (Cell &cell : cells) {
        const qreal width = (cell.row == summaryRow ? x : widths.value(cell.column, minimum)) - 2 * padX;
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
                           cell.row == summaryRow ? x : widths.value(cell.column), rowHeights.value(cell.row));
    }

    // Only a data row's first cell opens its note - BaseTableModel::
    // isNameColumn's choice for the real widget, mirrored here.
    for (const Cell &cell : cells) {
        if (cell.column != 0 || cell.row == 0 || cell.row == summaryRow || cell.row == footerRow)
            continue;
        const int dataIndex = cell.row - 1;
        if (dataIndex < 0 || dataIndex >= shownRows)
            continue;
        if (!data.rows.at(dataIndex).notePath.isEmpty())
            view.baseRows.push_back({cell.rect, data.rows.at(dataIndex).notePath});
    }

    view.cells = std::move(cells);
    view.headerRows = 1;
    view.sideScroll =
        qBound<qreal>(0, view.sideScroll, qMax<qreal>(0, view.sideWidth - contentWidth()));
    view.layout.reset();
    view.height = y;
    view.laidOut = true;
    return true;
}

void MarkdownEditor::paintBase(QPainter &painter, const BlockView &view, qreal left,
                               qreal top) const
{
    if (!view.baseViewName.isEmpty()) {
        QFont headingFont = d->m_theme.uiFont();
        headingFont.setWeight(QFont::DemiBold);
        painter.save();
        painter.setFont(headingFont);
        painter.setPen(d->m_theme.color(QStringLiteral("--text-normal")));
        painter.drawText(QRectF(left, top, contentWidth(), view.textTop),
                         Qt::AlignVCenter | Qt::AlignLeft, view.baseViewName);
        painter.restore();
    }
    paintTable(painter, view, left, top);
}

} // namespace noteAppC
