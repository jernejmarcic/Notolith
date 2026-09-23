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

#include <QScrollBar>
#include <QTextLayout>
#include <QTimer>

namespace noteAppC {

QList<MarkdownEditor::QueryResultRow> MarkdownEditor::queryRowsAt(qsizetype offset) const
{
    QList<QueryResultRow> rows;
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return rows;
    const BlockView &view = m_blocks[size_t(index)];
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
    m_queryCache.clear();
    bool touched = false;
    for (BlockView &block : m_blocks) {
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
    if (!m_queryRefreshTimer) {
        m_queryRefreshTimer = new QTimer(this);
        m_queryRefreshTimer->setSingleShot(true);
        m_queryRefreshTimer->setInterval(200);
        connect(m_queryRefreshTimer, &QTimer::timeout, this, [this] {
            m_queryCache.clear();
            bool touched = false;
            for (BlockView &block : m_blocks) {
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
    m_queryRefreshTimer->start(); // restarts: a burst of index changes costs one refresh
}

const MarkdownEditor::QueryCacheEntry &MarkdownEditor::ensureQuerySearch(const QString &queryText)
{
    auto it = m_queryCache.find(queryText);
    if (it != m_queryCache.end())
        return *it;

    QueryCacheEntry entry;
    entry.pending = true;
    entry.generation = ++m_queryGeneration;
    it = m_queryCache.insert(queryText, entry);

    if (m_queryRunner) {
        const quint64 generation = entry.generation;
        // The runner may call this from a worker thread (that is the whole
        // point - a search must never run on, or block, the UI thread), so
        // the delivery is marshalled back onto this object's own thread
        // before it touches m_queryCache or a single BlockView. Capturing
        // `this` by raw pointer is safe here: a QueuedConnection with a
        // QObject context, which invokeMethod(this, ...) is, is simply not
        // delivered if `this` was destroyed first.
        m_queryRunner(queryText, [this, queryText, generation](QueryResults results) {
            QMetaObject::invokeMethod(
                this,
                [this, queryText, generation, results = std::move(results)]() mutable {
                    deliverQueryResults(queryText, generation, std::move(results));
                },
                Qt::QueuedConnection);
        });
    }
    return *it;
}

void MarkdownEditor::deliverQueryResults(const QString &queryText, quint64 generation,
                                         QueryResults results)
{
    auto it = m_queryCache.find(queryText);
    // Not the entry's current generation: refreshQueries (or a fresh
    // setQueryRunner) started a newer request for the same text while this
    // one was still in flight, and that one's delivery, not this one, is
    // what any block waiting on it should show.
    if (it == m_queryCache.end() || it->generation != generation)
        return;
    it->results = std::move(results);
    it->pending = false;

    bool touched = false;
    for (BlockView &block : m_blocks) {
        if (!block.isQuery)
            continue;
        const CodeFenceSpan *fence = codeFenceOf(block);
        if (!fence)
            continue;
        if (m_text.mid(fence->content.start, fence->content.length()).trimmed() != queryText)
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
    const CodeFenceSpan *fence = codeFenceOf(view);
    if (!fence || fence->language.compare(QLatin1String("query"), Qt::CaseInsensitive) != 0)
        return false;

    view.isQuery = true;
    view.image = QPixmap();
    view.cells.clear();

    const QString queryText = m_text.mid(fence->content.start, fence->content.length()).trimmed();

    const QColor pathColour = m_theme.color(QStringLiteral("--text-accent"));
    const QColor mutedColour = m_theme.color(QStringLiteral("--text-muted"));
    const QColor textColour = m_theme.color(QStringLiteral("--text-normal"));
    const QColor highlightBg = m_theme.color(QStringLiteral("--color-yellow"));
    const QColor highlightFg = m_theme.color(QStringLiteral("--color-base-00"));

    struct Row {
        qsizetype start = 0;
        qsizetype end = 0;
        QString path;
        qsizetype offset = 0;
    };
    QString display;
    QList<QTextLayout::FormatRange> formats;
    QList<Row> rows;

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
        } else if (entry.results.isEmpty()) {
            addPlainLine(tr("No results for “%1”").arg(queryText), mutedColour);
        } else {
            for (const QueryResult &result : entry.results) {
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

    const QFont bodyFont = m_theme.textFont();
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

    return true;
}

} // namespace noteAppC
