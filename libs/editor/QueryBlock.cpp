/// The ```` ```query ```` block (docs: obsidian.md/help/plugins/search,
/// "Embed search results in a note"): a fenced code block whose language is
/// `query` shows the results of its own text as a search - the note list
/// with match counts and highlighted snippets - in place of the query,
/// exactly the way a `$$...$$` block shows a formula instead of LaTeX.
///
/// The search itself is never the editor's job. libs/editor must not link
/// libs/index (docs/PLUGINS.md's module boundary), so MarkdownEditor only
/// ever calls a callback the window supplies - setQueryRunner - with the
/// query text, and is handed the results back through another callback,
/// QueryDeliver, which may be called from any thread. A search over 20 000
/// notes takes about 6 ms (docs/PERFORMANCE.md), but layout must never wait
/// on it regardless, so a block shows "Searching..." for however long that
/// takes and relays out only the blocks whose text it was searching for once
/// deliverQueryResults gets called.
///
/// Results are cached by the query's own text (m_queryCache), not by block:
/// two blocks with the same query share one search. A block's text changing
/// is a cache miss, and the entry the old text is still on record for is
/// simply left alone - nothing has to notice the block moved on to ask for
/// something else. What does need noticing is two requests for the *same*
/// text in flight together, which refreshQueries causes on purpose (the
/// index changed, so the cached answer might be stale) - the generation
/// counter in QueryCacheEntry exists only for that: a delivery is applied
/// only if it is still the most recent request for that text.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include <QScrollBar>
#include <QTextLayout>
#include <QTimer>

#include <algorithm>

namespace noteAppC {

QList<MarkdownEditor::QueryResultRow> MarkdownEditor::queryRowsAt(qsizetype offset) const
{
    QList<QueryResultRow> rows;
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return rows;
    const BlockView &view = d->m_blocks[size_t(index)];
    if (!view.isQuery)
        return rows;
    const qreal left = contentLeft();
    const qreal top = view.y - verticalScrollBar()->value();
    rows.reserve(int(view.queryRows.size()));
    for (const BlockView::QueryRow &row : view.queryRows)
        rows.append({row.rect.translated(left, top), row.path, row.offset});
    return rows;
}

void MarkdownEditor::setQueryRunner(
    std::function<void(const QString &, const QueryDeliver &)> runner)
{
    m_queryRunner = std::move(runner);
    // Every cache entry was answered by the old runner, or is still waiting
    // on it; a stray delivery from it will still be dropped correctly since
    // ensureQuerySearch below hands out a fresh generation for the same
    // text. Dropping the results outright, rather than keeping them until a
    // fresh search lands, avoids showing a search result if the new runner
    // never actually delivers.
    d->m_queryCache.clear();
    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        if (!block.isQuery)
            continue;
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
        touched = true;
    }
    if (touched) {
        updateLayout();
        viewport()->update();
    }
}

void MarkdownEditor::refreshQueries()
{
    if (!d->m_queryRefreshTimer) {
        d->m_queryRefreshTimer = new QTimer(this);
        d->m_queryRefreshTimer->setSingleShot(true);
        d->m_queryRefreshTimer->setInterval(200);
        connect(d->m_queryRefreshTimer, &QTimer::timeout, this, [this] {
            d->m_queryCache.clear();
            bool touched = false;
            for (BlockView &block : d->m_blocks) {
                if (!block.isQuery)
                    continue;
                block.layout.reset();
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
    d->m_queryRefreshTimer->start(); // restarts: a burst of index changes costs one refresh
}

const MarkdownEditor::QueryCacheEntry &MarkdownEditor::ensureQuerySearch(const QString &queryText)
{
    auto it = d->m_queryCache.find(queryText);
    if (it != d->m_queryCache.end())
        return *it;

    QueryCacheEntry entry;
    entry.pending = true;
    entry.generation = ++d->m_queryGeneration;
    it = d->m_queryCache.insert(queryText, entry);

    if (m_queryRunner) {
        const quint64 generation = entry.generation;
        // The runner may call this from a worker thread (that is the whole
        // point - a search must never run on, or block, the UI thread), so
        // the delivery is marshalled back onto this object's own thread
        // before it touches m_queryCache or a single BlockView, through
        // m_lifeline: the editor may be gone by the time a search finishes.
        m_queryRunner(queryText, [this, lifeline = d->m_lifeline, queryText, generation](QueryResults results) {
            lifeline->post(this, [this, queryText, generation, results = std::move(results)]() mutable {
                deliverQueryResults(queryText, generation, std::move(results));
            });
        });
    }
    return *it;
}

void MarkdownEditor::deliverQueryResults(const QString &queryText, quint64 generation,
                                         QueryResults results)
{
    auto it = d->m_queryCache.find(queryText);
    // Not the entry's current generation: refreshQueries (or a fresh
    // setQueryRunner) started a newer request for the same text while this
    // one was still in flight, and that one's delivery, not this one, is
    // what any block waiting on it should show.
    if (it == d->m_queryCache.end() || it->generation != generation)
        return;
    it->results = std::move(results);
    it->pending = false;

    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        if (!block.isQuery)
            continue;
        const CodeFenceSpan *fence = codeFenceOf(block);
        if (!fence)
            continue;
        if (d->m_text.mid(fence->content.start, fence->content.length()).trimmed() != queryText)
            continue;
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
        touched = true;
    }
    if (touched) {
        updateLayout();
        viewport()->update();
    }
}

bool MarkdownEditor::layoutQuery(BlockView &view)
{
    if (view.span.kind != BlockSpan::Kind::Code)
        return false;
    if (!d->m_features.queryBlocks)
        return false; // the fence shows as a plain, uncoloured code listing
    const CodeFenceSpan *fence = codeFenceOf(view);
    if (!fence || fence->language.compare(QLatin1String("query"), Qt::CaseInsensitive) != 0)
        return false;

    view.isQuery = true;
    view.extrasMut().image = QPixmap();
    view.cells.clear();

    const QString queryText = d->m_text.mid(fence->content.start, fence->content.length()).trimmed();
    // The header's collapsed/sort-by-name state is remembered by the
    // block's own source position, the same key m_folds uses - see
    // toggleQueryCollapsed.
    const qsizetype blockKey = view.span.range.start;
    const bool collapsed = d->m_queryCollapsed.value(blockKey, false);
    const bool sortByName = d->m_queryNameSort.value(blockKey, false);

    const QColor pathColour = d->m_theme.color(QStringLiteral("--text-accent"));
    const QColor mutedColour = d->m_theme.color(QStringLiteral("--text-muted"));
    const QColor textColour = d->m_theme.color(QStringLiteral("--text-normal"));
    const QColor accentColour = d->m_theme.color(QStringLiteral("--text-accent"));
    const QColor highlightBg = d->m_theme.color(QStringLiteral("--color-yellow"));
    const QColor highlightFg = d->m_theme.color(QStringLiteral("--color-base-00"));

    struct Row {
        qsizetype start = 0;
        qsizetype end = 0;
        QString path;
        qsizetype offset = 0;
    };
    QString display;
    QList<QTextLayout::FormatRange> formats;
    QList<Row> rows;
    // Character spans of the header's two controls, within `display` - set
    // only once a header is actually drawn (a search still pending, empty,
    // or with no runner shows a single plain message and no header at all,
    // the same as before this block gained one).
    qsizetype collapseStart = -1, collapseEnd = -1;
    qsizetype sortStart = -1, sortEnd = -1;

    const auto addPlainLine = [&](const QString &line, const QColor &colour) {
        QTextLayout::FormatRange range;
        range.start = int(display.size());
        range.length = int(line.size());
        range.format.setForeground(colour);
        formats.append(range);
        display += line;
    };

    if (queryText.isEmpty()) {
        addPlainLine(tr("Empty search"), mutedColour);
    } else if (!m_queryRunner) {
        addPlainLine(tr("No search source connected"), mutedColour);
    } else {
        const QueryCacheEntry &entry = ensureQuerySearch(queryText);
        if (entry.pending) {
            addPlainLine(tr("Searching “%1”…").arg(queryText), mutedColour);
        } else {
            // --- the header: obsidian.md/help/plugins/search's "Embed
            // search results in a note" shows a result count over the list,
            // a collapse toggle and a sort control (that page's own search
            // pane section lists "File name (A to Z)" among the sort
            // orders - modified time and match count are not there, so
            // only a name/best-match toggle is offered here).
            view.queryResultCountText = entry.results.size() == 1
                                            ? tr("1 result")
                                            : tr("%1 results").arg(entry.results.size());
            const QString collapseGlyph =
                collapsed ? QStringLiteral("▸ ") : QStringLiteral("▾ ");

            collapseStart = display.size();
            addPlainLine(collapseGlyph + view.queryResultCountText, textColour);
            collapseEnd = display.size();

            display += QStringLiteral("    ");

            sortStart = display.size();
            addPlainLine(sortByName ? tr("Sort: File name") : tr("Sort: Best match"), accentColour);
            sortEnd = display.size();

            if (!collapsed) {
                display += QChar(QChar::LineSeparator);
                display += QChar(QChar::LineSeparator); // blank line under the header

                if (entry.results.isEmpty()) {
                    addPlainLine(tr("No results for “%1”").arg(queryText), mutedColour);
                } else {
                    QueryResults sorted = entry.results;
                    if (sortByName) {
                        std::sort(sorted.begin(), sorted.end(),
                                 [](const QueryResult &a, const QueryResult &b) {
                                     return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
                                 });
                    }
                    for (const QueryResult &result : sorted) {
                        const qsizetype rowStart = display.size();

                        QTextLayout::FormatRange pathRange;
                        pathRange.start = int(display.size());
                        pathRange.length = int(result.path.size());
                        pathRange.format.setForeground(pathColour);
                        pathRange.format.setFontWeight(QFont::Bold);
                        formats.append(pathRange);
                        display += result.path;

                        const QString count = result.matchCount == 1
                                                  ? tr("  · 1 match")
                                                  : tr("  · %1 matches").arg(result.matchCount);
                        QTextLayout::FormatRange countRange;
                        countRange.start = int(display.size());
                        countRange.length = int(count.size());
                        countRange.format.setForeground(mutedColour);
                        formats.append(countRange);
                        display += count;
                        display += QChar(QChar::LineSeparator);

                        if (!result.snippet.isEmpty()) {
                            const qsizetype snippetStart = display.size();
                            QTextLayout::FormatRange snippetRange;
                            snippetRange.start = int(snippetStart);
                            snippetRange.length = int(result.snippet.size());
                            snippetRange.format.setForeground(textColour);
                            formats.append(snippetRange);
                            display += result.snippet;
                            for (const QueryMatch &hit : result.snippetHighlights) {
                                if (hit.offset < 0 || hit.offset + hit.length > result.snippet.size())
                                    continue; // a stale offset from a snippet that changed shape
                                QTextLayout::FormatRange hitRange;
                                hitRange.start = int(snippetStart + hit.offset);
                                hitRange.length = int(hit.length);
                                hitRange.format.setBackground(highlightBg);
                                hitRange.format.setForeground(highlightFg);
                                formats.append(hitRange);
                            }
                            display += QChar(QChar::LineSeparator);
                        }

                        rows.append({rowStart, display.size(), result.path, result.firstOffset});
                        display += QChar(QChar::LineSeparator); // blank spacer between results
                    }
                    while (display.endsWith(QChar(QChar::LineSeparator)))
                        display.chop(1);
                }
            }
        }
    }

    const QFont bodyFont = d->m_theme.textFont();
    view.layout = std::make_unique<QTextLayout>(display);
    view.layout->setCacheEnabled(true);
    view.layout->setFont(bodyFont);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    view.layout->setTextOption(option);
    view.layout->setFormats(formats);

    const qreal lineHeight = lineHeightFor({BlockSpan::Kind::Paragraph, {0, 0}, 0});
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

    view.queryRows.clear();
    for (const Row &row : rows) {
        QRectF rect;
        for (int i = 0; i < view.layout->lineCount(); ++i) {
            const QTextLine line = view.layout->lineAt(i);
            if (line.textStart() + line.textLength() <= row.start || line.textStart() >= row.end)
                continue;
            const QRectF lineRect(0, view.textTop + line.y(), contentWidth(), line.height());
            rect = rect.isNull() ? lineRect : rect.united(lineRect);
        }
        if (!rect.isNull())
            view.queryRows.push_back({rect, row.path, row.offset});
    }

    // The header's two controls sit side by side on one line, unlike a
    // result row (which owns its lines outright), so their rects are the
    // tight x-bounds of their own span on that line rather than the line's
    // full width - a click between them must not land on either.
    const auto inlineSpanRect = [&](qsizetype start, qsizetype end) -> QRectF {
        if (start < 0)
            return QRectF();
        for (int i = 0; i < view.layout->lineCount(); ++i) {
            const QTextLine line = view.layout->lineAt(i);
            if (start < line.textStart() || start >= line.textStart() + line.textLength())
                continue;
            const qreal x1 = line.cursorToX(int(start));
            const qreal x2 = line.cursorToX(int(qMin<qsizetype>(end, line.textStart() + line.textLength())));
            return QRectF(x1, view.textTop + line.y(), x2 - x1, line.height());
        }
        return QRectF();
    };
    view.queryCollapseRect = inlineSpanRect(collapseStart, collapseEnd);
    view.querySortRect = inlineSpanRect(sortStart, sortEnd);

    return true;
}

void MarkdownEditor::toggleQueryCollapsed(qsizetype offset)
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return;
    BlockView &view = d->m_blocks[size_t(index)];
    if (!view.isQuery)
        return;
    const qsizetype key = view.span.range.start;
    d->m_queryCollapsed.insert(key, !d->m_queryCollapsed.value(key, false));
    view.layout.reset();
    view.laidOut = false;
    view.heightEstimated = false;
    updateLayout();
    viewport()->update();
}

bool MarkdownEditor::queryCollapsedAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return false;
    const BlockView &view = d->m_blocks[size_t(index)];
    if (!view.isQuery)
        return false;
    return d->m_queryCollapsed.value(view.span.range.start, false);
}

void MarkdownEditor::cycleQuerySort(qsizetype offset)
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return;
    BlockView &view = d->m_blocks[size_t(index)];
    if (!view.isQuery)
        return;
    const qsizetype key = view.span.range.start;
    d->m_queryNameSort.insert(key, !d->m_queryNameSort.value(key, false));
    view.layout.reset();
    view.laidOut = false;
    view.heightEstimated = false;
    updateLayout();
    viewport()->update();
}

MarkdownEditor::QueryHeader MarkdownEditor::queryHeaderAt(qsizetype offset) const
{
    QueryHeader header;
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return header;
    const BlockView &view = d->m_blocks[size_t(index)];
    if (!view.isQuery || !view.queryCollapseRect.isValid())
        return header; // no header yet: still searching, empty, or no runner
    const qreal left = contentLeft();
    const qreal top = view.y - verticalScrollBar()->value();
    header.collapseRect = view.queryCollapseRect.translated(left, top);
    header.sortRect = view.querySortRect.translated(left, top);
    header.collapsed = d->m_queryCollapsed.value(view.span.range.start, false);
    header.sortByName = d->m_queryNameSort.value(view.span.range.start, false);
    header.countText = view.queryResultCountText;
    return header;
}

} // namespace noteAppC
