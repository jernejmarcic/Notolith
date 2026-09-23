#include "PageBody.h"

#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <algorithm>

#include "CodeHighlighter.h"
#include "HtmlRenderer.h"
#include "Index.h"
#include "Slugs.h"
#include "Theme.h"
#include "Vault.h"

namespace noteAppC::publish {

namespace {

QString escapeHtml(const QString &text)
{
    QString out;
    out.reserve(text.size() + text.size() / 8);
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '&': out += QLatin1String("&amp;"); break;
        case '<': out += QLatin1String("&lt;"); break;
        case '>': out += QLatin1String("&gt;"); break;
        case '"': out += QLatin1String("&quot;"); break;
        default: out += c;
        }
    }
    return out;
}

QString escapeAttr(const QString &text)
{
    return escapeHtml(text);
}

Range clip(const Range &r, const Range &bound)
{
    return {qMax(r.start, bound.start), qMin(r.end, bound.end)};
}

bool within(const Range &r, const Range &region)
{
    return r.isValid() && r.start >= region.start && r.end <= region.end;
}

/// File extensions this site treats as an inline embed rather than a plain
/// link - Obsidian's own embeddable set, minus what noteAppC has no player
/// for at all (so those still fall back to a plain download link).
QString mediaKindOf(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    static const QSet<QString> images = {QStringLiteral("png"), QStringLiteral("jpg"),
                                         QStringLiteral("jpeg"), QStringLiteral("gif"),
                                         QStringLiteral("svg"), QStringLiteral("webp"),
                                         QStringLiteral("bmp")};
    static const QSet<QString> audio = {QStringLiteral("mp3"), QStringLiteral("wav"),
                                        QStringLiteral("ogg"), QStringLiteral("m4a"),
                                        QStringLiteral("flac"), QStringLiteral("opus"),
                                        QStringLiteral("3gp")};
    static const QSet<QString> video = {QStringLiteral("mp4"), QStringLiteral("webm"),
                                        QStringLiteral("mov"), QStringLiteral("mkv"),
                                        QStringLiteral("ogv")};
    if (images.contains(ext))
        return QStringLiteral("image");
    if (audio.contains(ext))
        return QStringLiteral("audio");
    if (video.contains(ext))
        return QStringLiteral("video");
    if (ext == QLatin1String("pdf"))
        return QStringLiteral("pdf");
    return QStringLiteral("file");
}

/// One node in the block-level forest built per region: enough to sort, dedup
/// by containment, and dispatch on afterwards.
struct BlockNode {
    Range range;
    enum Kind { Callout, Table, CodeFence, MathBlock, HtmlBlock, ListGroup, Heading, Paragraph } kind;
    qsizetype dataIndex = -1; ///< index into the owning Decorations list for this kind
};

/// One span in the inline forest built per text run.
struct InlineNode {
    Range range;
    enum Kind { Bold, Italic, Strike, Code, Highlight, Tag, Html, Math, Link } kind;
    qsizetype dataIndex = -1;
};

bool nodeLess(const Range &a, const Range &b)
{
    if (a.start != b.start)
        return a.start < b.start;
    return a.length() > b.length(); // the enclosing one first at a tie
}

/// The frontmatter block, if Parser found one - it is always decorations
/// .blocks' first entry when present (Parser.cpp prepend()s it).
Range frontmatterRangeOf(const ParsedNote &note)
{
    if (!note.decorations.blocks.isEmpty()
        && note.decorations.blocks.first().kind == BlockSpan::Kind::Frontmatter)
        return note.decorations.blocks.first().range;
    return {};
}

} // namespace

/// Owns the mutable state one render() call threads through its recursion:
/// heading-anchor de-duplication (page-wide - embedded content shares the
/// same document), the embed cycle guard, attachments referenced so far, and
/// which note's text/decorations "note" currently means (changes across an
/// embed boundary, everything else does not). See PageBody.h for why
/// self-links inside embedded content still point at the target's own page
/// rather than at the inlined copy's ids.
class Renderer {
public:
    Renderer(const ParsedNote &topLevel, const RenderEnv &env)
        : m_topLevel(&topLevel)
        , m_env(env)
        , m_pageOutputPath(topLevel.outputPath)
    {
    }

    ContentResult run()
    {
        ContentResult result;
        QString html = QStringLiteral("<h1 class=\"page-title\">") + escapeHtml(m_topLevel->title)
                       + QStringLiteral("</h1>\n");
        html += renderProperties(*m_topLevel);
        html += QStringLiteral("<div class=\"note-body\">");
        const Range frontmatter = frontmatterRangeOf(*m_topLevel);
        const qsizetype bodyStart = frontmatter.isValid() ? frontmatter.end : 0;
        html += emitRegion({bodyStart, qsizetype(m_topLevel->text.size())}, *m_topLevel,
                           /*includeHeadings=*/true);
        html += QStringLiteral("</div>");

        result.html = html;
        result.attachments = m_attachments.values();
        result.codeStyle = codeStyleBlock();
        // Formulas are still placeholder tokens at this point (see
        // renderMath()) - swapped for their LaTeX source here only, so the
        // search index has something to match against instead of a raw
        // token; the real HTML keeps the token for Publisher to resolve.
        QString searchable = html;
        for (const PendingFormula &formula : m_formulas)
            searchable.replace(formula.token, formula.latex);
        result.plainText = plainTextOf(m_topLevel->title, searchable);
        result.formulas = m_formulas;
        return result;
    }

private:
    // --- properties ------------------------------------------------------

    QString renderProperties(const ParsedNote &note)
    {
        QStringList rows;
        for (const Property &property : note.metadata.properties) {
            const QString key = property.key.toLower();
            if (key == QLatin1String("publish") || key == QLatin1String("permalink"))
                continue; // publishing mechanics, not content for the reader
            QString value;
            if (property.type == Property::Type::List) {
                QStringList parts;
                for (const QString &v : property.values)
                    parts << escapeHtml(v);
                value = parts.join(QStringLiteral(", "));
            } else if (!property.values.isEmpty()) {
                value = escapeHtml(property.values.first());
            }
            rows << QStringLiteral("<tr><th>%1</th><td>%2</td></tr>")
                        .arg(escapeHtml(property.key), value);
        }
        if (rows.isEmpty())
            return {};
        return QStringLiteral("<table class=\"properties\">%1</table>\n").arg(rows.join(QString()));
    }

    // --- block level -------------------------------------------------------

    /// Every direct child of `region`: callouts/tables/code/math/html blocks,
    /// list runs and, when `includeHeadings`, headings - everything else
    /// (real paragraph text) falls out of decorations.paragraphs. Sorted and
    /// deduplicated by containment so a nested node (e.g. a callout inside a
    /// callout) is only ever visited once, from its true parent's own call.
    /// `selfKind`/`selfIndex` exclude one exact entry, the same way and for
    /// the same reason emitInline()'s do: renderCallout() recurses into its
    /// own CalloutSpan's full range to render its body, and without this a
    /// callout whose range fills its region exactly (nothing else inside it)
    /// would immediately re-match itself and recurse forever. A -1 index
    /// (every call except a callout unwrapping itself) never excludes
    /// anything, since no real index is negative.
    QList<BlockNode> directChildren(const Range &region, const ParsedNote &note, bool includeHeadings,
                                    BlockNode::Kind selfKind = BlockNode::Paragraph, qsizetype selfIndex = -1)
    {
        QList<BlockNode> nodes;
        const Decorations &d = note.decorations;
        const auto excluded = [&](BlockNode::Kind kind, qsizetype index) {
            return kind == selfKind && index == selfIndex;
        };
        for (qsizetype i = 0; i < d.callouts.size(); ++i)
            if (!excluded(BlockNode::Callout, i) && within(d.callouts.at(i).range, region))
                nodes.append({d.callouts.at(i).range, BlockNode::Callout, i});
        for (qsizetype i = 0; i < d.tables.size(); ++i)
            if (within(d.tables.at(i).range, region))
                nodes.append({d.tables.at(i).range, BlockNode::Table, i});
        for (qsizetype i = 0; i < d.codeFences.size(); ++i)
            if (within(d.codeFences.at(i).block, region))
                nodes.append({d.codeFences.at(i).block, BlockNode::CodeFence, i});
        for (qsizetype i = 0; i < d.maths.size(); ++i)
            if (d.maths.at(i).display && within(d.maths.at(i).range, region))
                nodes.append({d.maths.at(i).range, BlockNode::MathBlock, i});
        for (qsizetype i = 0; i < d.blocks.size(); ++i)
            if (d.blocks.at(i).kind == BlockSpan::Kind::Html && within(d.blocks.at(i).range, region))
                nodes.append({d.blocks.at(i).range, BlockNode::HtmlBlock, i});
        if (includeHeadings) {
            for (qsizetype i = 0; i < note.metadata.headings.size(); ++i)
                if (within(note.metadata.headings.at(i).range, region))
                    nodes.append({note.metadata.headings.at(i).range, BlockNode::Heading, i});
        }
        for (const Range &r : listGroupRuns(region, d.listItems, note.text))
            nodes.append({r, BlockNode::ListGroup, -1});
        for (qsizetype i = 0; i < d.paragraphs.size(); ++i)
            if (within(d.paragraphs.at(i), region))
                nodes.append({d.paragraphs.at(i), BlockNode::Paragraph, i});

        std::sort(nodes.begin(), nodes.end(),
                 [](const BlockNode &a, const BlockNode &b) { return nodeLess(a.range, b.range); });
        QList<BlockNode> direct;
        qsizetype coverageEnd = region.start;
        for (const BlockNode &n : std::as_const(nodes)) {
            if (n.range.start < coverageEnd)
                continue; // nested inside a sibling already accepted - its own recursion covers it
            direct.append(n);
            coverageEnd = qMax(coverageEnd, n.range.end);
        }
        return direct;
    }

    /// Groups a flat, document-ordered list of list items into maximal runs
    /// (one per separate <ul>/<ol>) - see PageBody design notes on why this,
    /// rather than decorations.blocks' List entry, is what has to carry list
    /// detection inside a callout/quote too, where the parser does not
    /// record a top-level List block at all. Two consecutive items at the
    /// same indent continue the same run when nothing but whitespace sits
    /// between them; a deeper item always continues it (it is this run's own
    /// nested list); a shallower one, or real content in the gap, ends it.
    QList<Range> listGroupRuns(const Range &region, const QList<ListItemSpan> &items, const QString &text)
    {
        QList<Range> runs;
        qsizetype i = 0;
        while (i < items.size()) {
            if (!within(items.at(i).range, region)) {
                ++i;
                continue;
            }
            const int baseIndent = items.at(i).indent;
            const bool baseOrdered = items.at(i).ordered;
            qsizetype start = items.at(i).range.start;
            qsizetype runEnd = items.at(i).range.end;
            ++i;
            while (i < items.size() && within(items.at(i).range, region)) {
                const ListItemSpan &next = items.at(i);
                if (next.indent > baseIndent) {
                    runEnd = qMax(runEnd, next.range.end);
                    ++i;
                    continue;
                }
                // A marker-type change (bullet <-> numbered) starts a new
                // list even with no blank line between - CommonMark's own
                // rule, mirrored here since decorations.listItems only
                // records each line's own marker, not which list it belongs
                // to.
                if (next.indent == baseIndent && next.ordered == baseOrdered
                    && text.mid(runEnd, next.range.start - runEnd).trimmed().isEmpty()) {
                    runEnd = next.range.end;
                    ++i;
                    continue;
                }
                break;
            }
            runs.append({start, runEnd});
        }
        return runs;
    }

    QString emitRegion(const Range &region, const ParsedNote &note, bool includeHeadings,
                       BlockNode::Kind selfKind = BlockNode::Paragraph, qsizetype selfIndex = -1)
    {
        // Belt and braces beyond the specific self-match fix above: nesting
        // this deep (real vaults rarely go past 3-4, a callout inside a
        // callout inside a quote) can only mean a range relationship this
        // function did not anticipate, so it stops rather than trusting one
        // more level - the same reasoning as the embed depth limit below.
        if (++m_blockDepth > 64) {
            --m_blockDepth;
            return QStringLiteral("<p class=\"unresolved\">(too deeply nested)</p>");
        }
        QString out;
        for (const BlockNode &node : directChildren(region, note, includeHeadings, selfKind, selfIndex))
            out += renderBlockNode(node, note);
        --m_blockDepth;
        return out;
    }

    QString renderBlockNode(const BlockNode &node, const ParsedNote &note)
    {
        switch (node.kind) {
        case BlockNode::Callout:
            return renderCallout(note.decorations.callouts.at(node.dataIndex), node.dataIndex, note);
        case BlockNode::Table:
            return renderTable(note.decorations.tables.at(node.dataIndex), note);
        case BlockNode::CodeFence:
            return renderCodeFence(note.decorations.codeFences.at(node.dataIndex), note);
        case BlockNode::MathBlock:
            return QStringLiteral("<div class=\"math-block\">")
                  + renderMath(note.decorations.maths.at(node.dataIndex), note) + QStringLiteral("</div>\n");
        case BlockNode::HtmlBlock:
            return renderRawHtml(stripHiddenRanges(node.range, note), true);
        case BlockNode::ListGroup:
            return renderListRun(node.range, note);
        case BlockNode::Heading: {
            const Heading &h = note.metadata.headings.at(node.dataIndex);
            const QString anchor = uniqueHeadingAnchor(h.text);
            const int level = qBound(1, h.level, 6);
            return QStringLiteral("<h%1 id=\"%2\">%3</h%1>\n")
                  .arg(level)
                  .arg(anchor, emitInline(h.textRange, note));
        }
        case BlockNode::Paragraph: {
            // A callout/container header line is its own one-line paragraph
            // whenever the very next line starts a different block (a fenced
            // code block, a table, ...) rather than continuing the same
            // prose - decorations.syntax hides the whole line (see
            // detectCallout()), so this paragraph's inline content comes
            // back empty and an empty <p></p> would otherwise sit at the top
            // of the callout's body for no reason.
            const QString content = emitInline(node.range, note);
            if (content.trimmed().isEmpty())
                return {};
            // A <div> (a note/heading/block embed) is not legal inside a
            // <p> - see soleEmbedSpan().
            if (soleEmbedSpan(node.range, note))
                return content + QStringLiteral("\n");
            return QStringLiteral("<p%1>").arg(blockAnchorAttr(node.range, note)) + content
                  + QStringLiteral("</p>\n");
        }
        }
        return {};
    }

    /// ` id="block-xyz"`, for whichever element - a paragraph, a list item -
    /// carries a `^xyz` block-id marker in its own range, so `[[Note#^xyz]]`
    /// has something in the page to actually jump to. Obsidian keeps the
    /// marker itself visible in reading view (ObsidianSyntax::scan() does
    /// not mask it as syntax to hide, and neither does this renderer), so
    /// this only adds the id - it never touches what text is shown.
    QString blockAnchorAttr(const Range &range, const ParsedNote &note) const
    {
        for (const BlockId &id : note.metadata.blockIds) {
            if (range.contains(id.range.start))
                return QStringLiteral(" id=\"%1\"").arg(Slugs::blockAnchor(id.id));
        }
        return {};
    }

    QString uniqueHeadingAnchor(const QString &headingText)
    {
        const QString base = Slugs::headingAnchor(headingText);
        auto it = m_headingAnchors.find(base);
        if (it == m_headingAnchors.end()) {
            m_headingAnchors.insert(base, 2);
            return base;
        }
        const QString candidate = base + QLatin1Char('-') + QString::number(it.value());
        it.value() += 1;
        return candidate;
    }

    // --- lists -------------------------------------------------------------

    QString renderListRun(const Range &run, const ParsedNote &note)
    {
        QList<qsizetype> indices;
        for (qsizetype i = 0; i < note.decorations.listItems.size(); ++i) {
            if (within(note.decorations.listItems.at(i).range, run))
                indices.append(i);
        }
        if (indices.isEmpty())
            return {};
        qsizetype cursor = 0;
        return renderListLevel(indices, cursor, note.decorations.listItems.at(indices.first()).indent, note);
    }

    QString renderListLevel(const QList<qsizetype> &indices, qsizetype &cursor, int levelIndent,
                            const ParsedNote &note)
    {
        const QList<ListItemSpan> &items = note.decorations.listItems;
        const bool ordered = items.at(indices.at(cursor)).ordered;
        QString out = ordered ? QStringLiteral("<ol>") : QStringLiteral("<ul>");
        while (cursor < indices.size() && items.at(indices.at(cursor)).indent == levelIndent) {
            const ListItemSpan &item = items.at(indices.at(cursor));
            qsizetype selfEnd = item.range.end;
            const bool hasChild = cursor + 1 < indices.size()
                && items.at(indices.at(cursor + 1)).indent > levelIndent;
            if (hasChild)
                selfEnd = items.at(indices.at(cursor + 1)).range.start;

            QString cls;
            if (item.task)
                cls = item.done ? QStringLiteral(" class=\"task-list-item done\"")
                                : QStringLiteral(" class=\"task-list-item\"");
            out += QStringLiteral("<li%1%2>").arg(cls, blockAnchorAttr(item.range, note));
            if (item.task) {
                out += QStringLiteral("<input type=\"checkbox\" disabled%1> ")
                      .arg(item.done ? QStringLiteral(" checked") : QString());
            }
            out += emitInline({item.marker.end, selfEnd}, note);
            ++cursor;
            if (hasChild)
                out += renderListLevel(indices, cursor, items.at(indices.at(cursor)).indent, note);
            out += QStringLiteral("</li>");
        }
        out += ordered ? QStringLiteral("</ol>") : QStringLiteral("</ul>");
        return out;
    }

    // --- tables --------------------------------------------------------

    QString renderTable(const TableSpan &table, const ParsedNote &note)
    {
        QString out = QStringLiteral("<table>");
        for (qsizetype row = 0; row < table.rows.size(); ++row) {
            const bool header = row == 0;
            out += QStringLiteral("<tr>");
            const QList<Range> &cells = table.rows.at(row);
            for (qsizetype col = 0; col < cells.size(); ++col) {
                const QString tag = header ? QStringLiteral("th") : QStringLiteral("td");
                QString style;
                if (col < table.alignment.size()) {
                    if (table.alignment.at(col) & Qt::AlignRight)
                        style = QStringLiteral(" style=\"text-align:right\"");
                    else if (table.alignment.at(col) & Qt::AlignHCenter)
                        style = QStringLiteral(" style=\"text-align:center\"");
                }
                out += QStringLiteral("<%1%2>%3</%1>")
                      .arg(tag, style, emitInline(cells.at(col), note));
            }
            out += QStringLiteral("</tr>");
        }
        out += QStringLiteral("</table>\n");
        return out;
    }

    // --- callouts / quotes ----------------------------------------------

    QString renderCallout(const CalloutSpan &callout, qsizetype calloutIndex, const ParsedNote &note)
    {
        const QColor dark = m_env.themeDark.calloutColor(callout.type);
        const QColor light = m_env.themeLight.calloutColor(callout.type);
        const QString rgbDark = QStringLiteral("%1,%2,%3").arg(dark.red()).arg(dark.green()).arg(dark.blue());
        const QString rgbLight = QStringLiteral("%1,%2,%3").arg(light.red()).arg(light.green()).arg(light.blue());
        const QString style = QStringLiteral(" style=\"--callout-color:%1;--callout-color-light:%2\"")
                                  .arg(rgbDark, rgbLight);
        const QString icon = iconSvg(noteAppC::Theme::calloutIcon(callout.type));
        const QString title = QStringLiteral("<span class=\"callout-icon\">%1</span>"
                                             "<span class=\"callout-title-text\">%2</span>")
                                  .arg(icon, escapeHtml(callout.title));
        // A depth>0 (nested) callout only has its "[!type]" marker hidden by
        // the parser, not the rest of its header line - deliberately, so the
        // live editor can keep the title inline in its own paragraph layout.
        // This renderer draws the title separately instead, so everything
        // from the end of the marker to the end of the title (the
        // connecting space included, not just titleRange itself) goes onto
        // m_extraHides for exactly this body's rendering, or the title would
        // appear twice: once here, once as leftover body text. A no-op for a
        // depth-0 callout, whose whole header line is already hidden.
        const bool pushedHide = callout.titleRange.isValid() && callout.marker.isValid();
        if (pushedHide)
            m_extraHides.append({callout.marker.end, callout.titleRange.end});
        const QString body = QStringLiteral("<div class=\"callout-content\">")
                             + emitRegion(callout.range, note, /*includeHeadings=*/false,
                                         BlockNode::Callout, calloutIndex)
                             + QStringLiteral("</div>");
        if (pushedHide)
            m_extraHides.removeLast();
        const QString dataType = escapeAttr(callout.type);

        if (callout.foldable) {
            return QStringLiteral("<details class=\"callout\" data-callout=\"%1\"%2%3>"
                                  "<summary class=\"callout-title\">%4</summary>%5</details>\n")
                  .arg(dataType, style, callout.collapsed ? QString() : QStringLiteral(" open"),
                      title, body);
        }
        return QStringLiteral("<div class=\"callout\" data-callout=\"%1\"%2>"
                              "<div class=\"callout-title\">%3</div>%4</div>\n")
              .arg(dataType, style, title, body);
    }

    QString iconSvg(const QString &lucideName)
    {
        auto it = m_iconCache.find(lucideName);
        if (it != m_iconCache.end())
            return *it;
        QFile file(QStringLiteral(":/icons/%1.svg").arg(lucideName));
        QString svg;
        if (file.open(QIODevice::ReadOnly))
            svg = QString::fromUtf8(file.readAll());
        m_iconCache.insert(lucideName, svg);
        return svg;
    }

    /// A range's raw source with every overlapping decorations.syntax span
    /// removed - what a code fence, raw HTML block or maths formula needs
    /// for its *literal* text, as opposed to emitInline()'s hide-and-style
    /// walk. Without this, a code fence (or formula, or HTML block) sitting
    /// inside a blockquote/callout would carry every line's "> " quote
    /// marker as literal text: CommonMark dequotes a blockquote's lines
    /// before parsing what is inside it, but the *ranges* Decorations
    /// reports still point into the original, still-quoted source, since
    /// that is the only text a rename or an editor's cursor can work with.
    ///
    /// `excludeSelfContaining`: an *inline* formula's whole text is itself
    /// one decorations.syntax entry (see the comment on Parser.cpp's inline
    /// maths handling: the editor hides the source and draws a formula image
    /// over it), which would otherwise strip the formula down to nothing.
    /// Skipping any hide that fully contains `range` tells that case apart
    /// from a real quote marker or a callout title, neither of which can
    /// ever span the whole thing they sit inside.
    QString stripHiddenRanges(const Range &range, const ParsedNote &note, bool excludeSelfContaining = false)
    {
        if (!range.isValid())
            return {};
        QList<Range> hides;
        const auto collect = [&](const Range &r) {
            if (!range.overlaps(r))
                return;
            if (excludeSelfContaining && r.start <= range.start && r.end >= range.end)
                return;
            hides.append(clip(r, range));
        };
        for (const Range &r : note.decorations.syntax)
            collect(r);
        for (const Range &r : m_extraHides)
            collect(r);
        std::sort(hides.begin(), hides.end(),
                 [](const Range &a, const Range &b) { return a.start < b.start; });
        QString out;
        qsizetype pos = range.start;
        for (const Range &hide : std::as_const(hides)) {
            if (hide.start > pos)
                out += note.text.mid(pos, hide.start - pos);
            pos = qMax(pos, hide.end);
        }
        if (pos < range.end)
            out += note.text.mid(pos, range.end - pos);
        return out;
    }

    // --- code fences -----------------------------------------------------

    const CodeOptions *optionsFor(const Range &content, const ParsedNote &note)
    {
        for (const CodeOptions &options : note.decorations.codeOptions) {
            if (options.block == content)
                return &options;
        }
        return nullptr;
    }

    QString renderCodeFence(const CodeFenceSpan &fence, const ParsedNote &note)
    {
        const QString text = stripHiddenRanges(fence.content, note);
        const CodeOptions *options = optionsFor(fence.content, note);
        const QString lang = fence.language;

        QString header;
        if (options && !options->title.isEmpty()) {
            header = QStringLiteral("<div class=\"code-header\">%1</div>")
                        .arg(escapeHtml(options->title));
        }

        const QStringList lines = text.split(QLatin1Char('\n'));
        const bool numbered = options && options->lineNumbers;
        const QSet<int> highlighted = options ? QSet<int>(options->highlight.begin(), options->highlight.end())
                                              : QSet<int>();
        const int firstLine = options ? options->firstLine : 1;

        QString body = highlightedLines(text, lang);
        // highlightedLines() already HTML-escapes and wraps tokens; split on
        // its own newlines (identical count to `lines`, since highlighting
        // never changes line breaks) to attach per-line numbering/highlight.
        const QStringList highlightedSplit = body.split(QLatin1Char('\n'));
        QString code;
        for (qsizetype i = 0; i < highlightedSplit.size(); ++i) {
            const int lineNo = firstLine + int(i);
            const bool hl = highlighted.contains(lineNo);
            code += QStringLiteral("<span class=\"line%1\"%2>%3</span>\n")
                       .arg(hl ? QStringLiteral(" hl") : QString(),
                           numbered ? QStringLiteral(" data-line=\"%1\"").arg(lineNo) : QString(),
                           highlightedSplit.at(i));
        }

        return QStringLiteral("<div class=\"code-block%1\" data-lang=\"%2\">%3"
                              "<pre><code class=\"language-%2\">%4</code></pre></div>\n")
              .arg(numbered ? QStringLiteral(" numbered") : QString(), escapeAttr(lang), header, code);
    }

    /// `text` highlighted for the dark theme (always) and, when the site is
    /// "both", the light theme too, folded into page-scoped `<span
    /// class="cN">` runs - see PageBody.h on why CSS classes rather than
    /// inline colours, and codeStyleBlock() for where the classes' CSS comes
    /// from.
    QString highlightedLines(const QString &text, const QString &lang)
    {
        const auto dark = m_env.highlighterDark.highlight(text, lang);
        const QList<QTextLayout::FormatRange> light =
            m_env.highlighterLight ? m_env.highlighterLight->highlight(text, lang)
                                   : QList<QTextLayout::FormatRange>();
        if (dark.isEmpty())
            return escapeHtml(text);

        // Boundaries come from the same tokenizer run twice (theme only
        // changes colour, not segmentation), so the same index in `light`
        // covers the same source run as `dark` - falling back to dark-only
        // styling if that ever does not hold, rather than trusting an
        // out-of-bounds index.
        const bool haveLight = light.size() == dark.size();

        QString out;
        qsizetype pos = 0;
        for (qsizetype i = 0; i < dark.size(); ++i) {
            const auto &range = dark.at(i);
            if (range.start > pos)
                out += escapeHtml(text.mid(pos, range.start - pos));
            const QString run = text.mid(range.start, range.length);
            const int cls = classFor(range.format, haveLight ? &light.at(i).format : nullptr);
            out += QStringLiteral("<span class=\"c%1\">%2</span>").arg(cls).arg(escapeHtml(run));
            pos = range.start + range.length;
        }
        if (pos < text.size())
            out += escapeHtml(text.mid(pos));
        return out;
    }

    struct TokenStyle {
        QRgb dark = 0;
        QRgb light = 0;
        bool hasLight = false;
        bool bold = false;
        bool italic = false;

        bool operator==(const TokenStyle &o) const
        {
            return dark == o.dark && light == o.light && hasLight == o.hasLight
                  && bold == o.bold && italic == o.italic;
        }
    };

    int classFor(const QTextCharFormat &darkFormat, const QTextCharFormat *lightFormat)
    {
        TokenStyle style;
        style.dark = darkFormat.foreground().color().rgb();
        style.bold = darkFormat.fontWeight() > QFont::Normal;
        style.italic = darkFormat.fontItalic();
        if (lightFormat) {
            style.light = lightFormat->foreground().color().rgb();
            style.hasLight = true;
        }
        const qsizetype existing = m_codeStyles.indexOf(style);
        if (existing >= 0)
            return int(existing);
        m_codeStyles.append(style);
        return int(m_codeStyles.size() - 1);
    }

    QString codeStyleBlock()
    {
        if (m_codeStyles.isEmpty())
            return {};
        QString dark, light;
        for (qsizetype i = 0; i < m_codeStyles.size(); ++i) {
            const TokenStyle &s = m_codeStyles.at(i);
            const QString weight = s.bold ? QStringLiteral("font-weight:600;") : QString();
            const QString style = s.italic ? QStringLiteral("font-style:italic;") : QString();
            dark += QStringLiteral(".c%1{color:#%2;%3%4}")
                       .arg(i).arg(QColor(s.dark).name(QColor::HexRgb).mid(1), weight, style);
            if (s.hasLight) {
                light += QStringLiteral(".c%1{color:#%2;%3%4}")
                            .arg(i).arg(QColor(s.light).name(QColor::HexRgb).mid(1), weight, style);
            }
        }
        QString css = QStringLiteral("<style>%1").arg(dark);
        if (!light.isEmpty())
            css += QStringLiteral("@media (prefers-color-scheme: light){%1}").arg(light);
        css += QStringLiteral("</style>\n");
        return css;
    }

    // --- maths -------------------------------------------------------------

    /// Records the formula for Publisher to render afterwards (see
    /// PendingFormula) and returns a unique placeholder token in its place. A
    /// null character on each side keeps the token out of reach of anything
    /// that could plausibly appear in real page text, so a plain string
    /// replace in Publisher.cpp can never hit the wrong spot.
    QString renderMath(const MathSpan &math, const ParsedNote &note)
    {
        const QString formula = stripHiddenRanges(math.formula, note, /*excludeSelfContaining=*/true);
        const QString token = QStringLiteral("\x01math-%1\x01").arg(m_formulas.size());
        m_formulas.append({token, formula, math.display ? 20.0 : 16.0});
        return token;
    }

    // --- raw HTML ----------------------------------------------------------

    QString renderRawHtml(const QString &raw, bool block)
    {
        // sanitizeHtml() (HtmlRenderer's own script/on*= stripper, reused
        // rather than duplicated) matches a *paired* "<script>...</script>"
        // within one string. That is right for a whole raw-HTML block
        // ("<div>...<script>x</script>...</div>" arrives here as one block,
        // matched fine), but CommonMark's *inline* raw-HTML grammar matches
        // one tag at a time: "text <script>x</script> more" is parsed as
        // three separate items - "<script>", "x", "</script>" - so within
        // any single one of those three calls there is nothing for a
        // pair-matching regex to pair. Every open or close tag for a
        // dangerous element is therefore also stripped outright, matched or
        // not - defence in depth for iframe/object/embed/link/meta/style
        // (which the live app only ever hides with CSS, safe for a real
        // browser but not for a static site that promises no network
        // request of any kind), and the actual fix for maths of the lone-tag
        // case above.
        QString safe = noteAppC::HtmlRenderer::sanitizeHtml(raw);
        static const QRegularExpression dangerousTag(
            QStringLiteral("</?\\s*(script|style|iframe|object|embed|link|meta)\\b[^>]*>"),
            QRegularExpression::CaseInsensitiveOption);
        safe.remove(dangerousTag);
        return block ? (QStringLiteral("<div class=\"html-block\">") + safe + QStringLiteral("</div>\n"))
                    : safe;
    }

    // --- inline --------------------------------------------------------

    /// `selfKind`/`selfIndex` exclude one exact entry - the span whose own
    /// content is being rendered right now. Without this, unwrapping e.g. a
    /// Bold span by recursing into emitInline(span.range) would immediately
    /// re-find that identical Bold entry (its range trivially lies "within"
    /// itself) and recurse forever - a bold/italic/highlight span's range
    /// starts and ends exactly where its own content does, unlike a
    /// wikilink's (which always strictly contains its target/alias text, so
    /// it never re-matches itself and needs no exclusion). A *different*
    /// entry that happens to share the same range (e.g. a paragraph that is
    /// nothing but one embed, so the paragraph and the link cover identical
    /// characters) is unaffected - only this exact (kind, index) is skipped.
    QList<InlineNode> collectInlineNodes(const Range &range, const ParsedNote &note,
                                         InlineNode::Kind selfKind, qsizetype selfIndex)
    {
        QList<InlineNode> nodes;
        const Decorations &d = note.decorations;
        const auto excluded = [&](InlineNode::Kind kind, qsizetype index) {
            return kind == selfKind && index == selfIndex;
        };
        for (qsizetype i = 0; i < d.styles.size(); ++i) {
            const StyleSpan &s = d.styles.at(i);
            if (!within(s.range, range))
                continue;
            InlineNode::Kind kind;
            switch (s.kind) {
            case StyleSpan::Kind::Bold: kind = InlineNode::Bold; break;
            case StyleSpan::Kind::Italic: kind = InlineNode::Italic; break;
            case StyleSpan::Kind::Strikethrough: kind = InlineNode::Strike; break;
            case StyleSpan::Kind::InlineCode: kind = InlineNode::Code; break;
            case StyleSpan::Kind::Highlight: kind = InlineNode::Highlight; break;
            case StyleSpan::Kind::Tag: kind = InlineNode::Tag; break;
            case StyleSpan::Kind::Html: kind = InlineNode::Html; break;
            case StyleSpan::Kind::Link: kind = InlineNode::Link; break;
            default: continue; // Heading/Quote/CodeBlock/Math(block variant): handled at block level
            }
            if (excluded(kind, i))
                continue;
            nodes.append({s.range, kind, i});
        }
        for (qsizetype i = 0; i < d.maths.size(); ++i) {
            if (d.maths.at(i).display || excluded(InlineNode::Math, i))
                continue;
            if (within(d.maths.at(i).range, range))
                nodes.append({d.maths.at(i).range, InlineNode::Math, i});
        }
        std::sort(nodes.begin(), nodes.end(),
                 [](const InlineNode &a, const InlineNode &b) { return nodeLess(a.range, b.range); });
        return nodes;
    }

    QString emitInline(const Range &range, const ParsedNote &note,
                       InlineNode::Kind selfKind = InlineNode::Link, qsizetype selfIndex = -1)
    {
        if (!range.isValid() || range.end <= range.start)
            return {};
        // Same reasoning as emitRegion()'s own depth cap: real markup never
        // nests this deep, so this catches any self-match this class did not
        // think of rather than trusting the recursion blindly.
        struct DepthGuard {
            int &depth;
            explicit DepthGuard(int &d) : depth(d) { ++depth; }
            ~DepthGuard() { --depth; }
        } guard(m_inlineDepth);
        if (m_inlineDepth > 128)
            return QStringLiteral("(too deeply nested)");

        const QList<InlineNode> spans = collectInlineNodes(range, note, selfKind, selfIndex);
        QList<Range> hides;
        for (const Range &r : note.decorations.syntax) {
            if (range.overlaps(r))
                hides.append(clip(r, range));
        }
        // A nested callout's title text is not itself parser-hidden syntax
        // (only its "[!type]" marker is - see the comment in renderCallout()
        // on why a depth>0 header is not a whole hidden line the way a
        // top-level one is); m_extraHides is how it still disappears from
        // its own body instead of appearing twice.
        for (const Range &r : m_extraHides) {
            if (range.overlaps(r))
                hides.append(clip(r, range));
        }
        std::sort(hides.begin(), hides.end(),
                 [](const Range &a, const Range &b) { return a.start < b.start; });

        QString out;
        qsizetype pos = range.start;
        qsizetype si = 0, hi = 0;
        qsizetype coverageEnd = range.start;
        while (pos < range.end) {
            while (si < spans.size() && spans.at(si).range.start < coverageEnd)
                ++si; // already inside a sibling span just emitted
            if (si < spans.size() && spans.at(si).range.start == pos) {
                out += renderInlineNode(spans.at(si), note);
                pos = spans.at(si).range.end;
                coverageEnd = qMax(coverageEnd, pos);
                ++si;
                continue;
            }
            while (hi < hides.size() && hides.at(hi).end <= pos)
                ++hi;
            if (hi < hides.size() && hides.at(hi).start <= pos) {
                pos = hides.at(hi).end;
                ++hi;
                continue;
            }
            qsizetype nextSpan = si < spans.size() ? spans.at(si).range.start : range.end;
            qsizetype nextHide = hi < hides.size() ? hides.at(hi).start : range.end;
            qsizetype stop = qMin(qMin(nextSpan, nextHide), range.end);
            if (stop <= pos) {
                ++pos; // defensive: never spin on malformed/zero-length data
                continue;
            }
            out += escapeHtml(note.text.mid(pos, stop - pos));
            pos = stop;
        }
        return out;
    }

    QString renderInlineNode(const InlineNode &node, const ParsedNote &note)
    {
        switch (node.kind) {
        case InlineNode::Bold:
            return QStringLiteral("<strong>") + emitInline(node.range, note, node.kind, node.dataIndex)
                  + QStringLiteral("</strong>");
        case InlineNode::Italic:
            return QStringLiteral("<em>") + emitInline(node.range, note, node.kind, node.dataIndex)
                  + QStringLiteral("</em>");
        case InlineNode::Strike:
            return QStringLiteral("<del>") + emitInline(node.range, note, node.kind, node.dataIndex)
                  + QStringLiteral("</del>");
        case InlineNode::Code:
            return QStringLiteral("<code>") + escapeHtml(stripHiddenRanges(node.range, note))
                  + QStringLiteral("</code>");
        case InlineNode::Highlight:
            return QStringLiteral("<mark>") + emitInline(node.range, note, node.kind, node.dataIndex)
                  + QStringLiteral("</mark>");
        case InlineNode::Tag:
            return renderTag(note.decorations.styles.at(node.dataIndex).payload);
        case InlineNode::Html:
            return renderRawHtml(stripHiddenRanges(node.range, note), false);
        case InlineNode::Math:
            return renderMath(note.decorations.maths.at(node.dataIndex), note);
        case InlineNode::Link:
            return renderLinkNode(note.decorations.styles.at(node.dataIndex), note);
        }
        return {};
    }

    QString renderTag(const QString &name)
    {
        return QStringLiteral("<a class=\"tag\" href=\"%1\">#%2</a>")
              .arg(escapeAttr(Slugs::relativeHref(m_pageOutputPath, Slugs::tagPagePath(name))),
                  escapeHtml(name));
    }

    // --- links and embeds ------------------------------------------------

    struct Resolved {
        bool ok = false;
        QString relativePath;
        bool isNote = false;
        bool published = false;
    };

    Resolved resolveTarget(const QString &target, const QString &fromRelativePath) const
    {
        Resolved r;
        if (target.isEmpty()) {
            r.ok = true;
            r.relativePath = fromRelativePath;
            r.isNote = true;
            r.published = m_env.notes.contains(fromRelativePath);
            return r;
        }
        const QString fromAbsolute = m_env.vault.absolute(fromRelativePath);
        const QString resolved = m_env.index.resolve(target, fromAbsolute);
        if (resolved.isEmpty())
            return r;
        r.ok = true;
        r.relativePath = resolved;
        r.isNote = noteAppC::Vault::isNote(resolved);
        r.published = r.isNote && m_env.notes.contains(resolved);
        return r;
    }

    /// The one embed a paragraph is nothing but, or nullptr. A note/heading/
    /// block embed renders a <div>, which cannot legally sit inside a <p> -
    /// this is what tells renderBlockNode() to skip that wrapper for exactly
    /// this shape (`![[Note]]` alone on its own line is the common case: an
    /// embed always starts its own paragraph, since CommonMark never merges
    /// two "lines" that would parse as different block starts).
    const StyleSpan *soleEmbedSpan(const Range &range, const ParsedNote &note) const
    {
        const StyleSpan *found = nullptr;
        for (const StyleSpan &s : note.decorations.styles) {
            if (s.kind != StyleSpan::Kind::Link || !within(s.range, range))
                continue;
            const Link *link = linkFor(s, note);
            if (!link || !link->embed)
                continue;
            if (found)
                return nullptr; // more than one embed: keep the safe, always-valid <p> wrapping
            found = &s;
        }
        if (!found)
            return nullptr;

        QList<Range> occupied = {found->range};
        for (const Range &r : note.decorations.syntax)
            if (range.overlaps(r))
                occupied.append(clip(r, range));
        for (const Range &r : m_extraHides)
            if (range.overlaps(r))
                occupied.append(clip(r, range));
        std::sort(occupied.begin(), occupied.end(),
                 [](const Range &a, const Range &b) { return a.start < b.start; });
        qsizetype pos = range.start;
        for (const Range &r : std::as_const(occupied)) {
            if (r.start > pos && !note.text.mid(pos, r.start - pos).trimmed().isEmpty())
                return nullptr; // real text alongside the embed
            pos = qMax(pos, r.end);
        }
        if (pos < range.end && !note.text.mid(pos, range.end - pos).trimmed().isEmpty())
            return nullptr;
        return found;
    }

    /// Finds the metadata.links entry a StyleSpan::Kind::Link points at, by
    /// its exact range - both are written from the same source offsets, see
    /// Parser.cpp, so this always finds one for a well-formed style list.
    const Link *linkFor(const StyleSpan &span, const ParsedNote &note) const
    {
        for (const Link &link : note.metadata.links) {
            if (link.range == span.range)
                return &link;
        }
        return nullptr;
    }

    QString wikilinkText(const Link &link, const StyleSpan &span, const ParsedNote &note)
    {
        if (!link.alias.isEmpty()) {
            const Range aliasRange{link.range.end - 2 - link.alias.size(), link.range.end - 2};
            return emitInline(aliasRange, note);
        }
        if (span.anchorHash.isValid() && link.subpath.startsWith(QLatin1Char('#'))) {
            const QString prefix = link.target.isEmpty() ? QString() : emitInline(link.targetRange, note);
            if (link.subpath.startsWith(QLatin1String("#^")))
                return prefix.isEmpty() ? QStringLiteral("(block reference)") : prefix;
            const QString heading = emitInline(span.anchorText, note);
            return prefix.isEmpty() ? heading : prefix + QStringLiteral(" &gt; ") + heading;
        }
        return link.target.isEmpty() ? escapeHtml(link.subpath) : emitInline(link.targetRange, note);
    }

    QString anchorFragment(const QString &subpath, const ParsedNote &target) const
    {
        if (subpath.isEmpty())
            return {};
        if (subpath.startsWith(QLatin1String("#^"))) {
            return Slugs::blockAnchor(subpath.mid(2));
        }
        const QString headingText = subpath.mid(1).trimmed();
        for (const Heading &h : target.metadata.headings) {
            if (h.text.compare(headingText, Qt::CaseInsensitive) == 0)
                return Slugs::headingAnchor(h.text);
        }
        return {};
    }

    QString renderLinkNode(const StyleSpan &span, const ParsedNote &note)
    {
        const Link *link = linkFor(span, note);
        if (!link)
            return emitInline(span.range, note); // defensive: should not happen

        if (link->external) {
            const QString text = link->wiki ? wikilinkText(*link, span, note)
                                           : escapeHtml(link->alias.isEmpty() ? link->target : link->alias);
            return QStringLiteral("<a href=\"%1\" rel=\"noopener noreferrer\">%2</a>")
                  .arg(escapeAttr(link->target), text);
        }

        const Resolved resolved = resolveTarget(link->target, note.relativePath);
        if (link->embed)
            return renderEmbed(*link, resolved, note);

        const QString text = link->wiki ? wikilinkText(*link, span, note)
                                       : escapeHtml(link->alias.isEmpty() ? link->target : link->alias);
        if (!resolved.ok || (resolved.isNote && !resolved.published)) {
            return QStringLiteral("<span class=\"unresolved\">%1</span>").arg(text);
        }
        if (!resolved.isNote) {
            // A link straight to an attachment (not an embed): offered as a
            // plain download, not copied unless something actually embeds it.
            return QStringLiteral("<span class=\"unresolved\" title=\"attachments are not published on their own\">%1</span>")
                  .arg(text);
        }
        const ParsedNote &targetNote = m_env.notes.value(resolved.relativePath);
        const QString anchor = anchorFragment(link->subpath, targetNote);
        const bool sameAsTopLevel = targetNote.relativePath == m_topLevel->relativePath;
        QString href = sameAsTopLevel ? QString() : Slugs::relativeHref(m_pageOutputPath, targetNote.outputPath);
        if (!anchor.isEmpty())
            href += QLatin1Char('#') + anchor;
        if (href.isEmpty())
            href = QStringLiteral("#");
        return QStringLiteral("<a href=\"%1\">%2</a>").arg(escapeAttr(href), text);
    }

    /// `note` names the note the embed syntax was *written in* - unused here
    /// since an embed's own href/anchor are always relative to the page
    /// (m_pageOutputPath) or the embedded note itself, never to the writer -
    /// kept as a parameter so every render*() taking a source note has the
    /// same shape and a future embed rule (e.g. relative attachment paths)
    /// does not have to change every call site to add it back.
    QString renderEmbed(const Link &link, const Resolved &resolved, const ParsedNote &note)
    {
        Q_UNUSED(note);
        if (!resolved.ok || (resolved.isNote && !resolved.published)) {
            return QStringLiteral("<span class=\"unresolved embed-missing\">%1</span>")
                  .arg(escapeHtml(link.target.isEmpty() ? link.subpath : link.target));
        }
        if (!resolved.isNote)
            return renderAttachmentEmbed(link, resolved.relativePath, note);

        // A note/heading/block embed.
        if (m_embedStack.contains(resolved.relativePath)) {
            return QStringLiteral("<div class=\"embed embed-cycle\">circular embed: %1</div>")
                  .arg(escapeHtml(link.target));
        }
        if (m_embedDepth >= m_env.embedDepthLimit) {
            return QStringLiteral("<div class=\"embed embed-limit\">%1</div>")
                  .arg(escapeHtml(link.target));
        }
        const ParsedNote &target = m_env.notes.value(resolved.relativePath);
        m_embedStack.insert(resolved.relativePath);
        ++m_embedDepth;
        QString inner;
        if (link.subpath.isEmpty()) {
            const Range fm = frontmatterRangeOf(target);
            inner = emitRegion({fm.isValid() ? fm.end : 0, qsizetype(target.text.size())}, target, true);
        } else if (link.subpath.startsWith(QLatin1String("#^"))) {
            inner = renderBlockEmbed(link.subpath.mid(2), target);
        } else {
            inner = renderHeadingEmbed(link.subpath.mid(1).trimmed(), target);
        }
        --m_embedDepth;
        m_embedStack.remove(resolved.relativePath);

        const QString sourceHref = Slugs::relativeHref(m_pageOutputPath, target.outputPath);
        return QStringLiteral("<div class=\"embed\"><div class=\"embed-title\">"
                              "<a href=\"%1\">%2</a></div><div class=\"embed-body\">%3</div></div>\n")
              .arg(escapeAttr(sourceHref), escapeHtml(target.title), inner);
    }

    QString renderHeadingEmbed(const QString &headingText, const ParsedNote &target)
    {
        const QList<Heading> &headings = target.metadata.headings;
        qsizetype index = -1;
        for (qsizetype i = 0; i < headings.size(); ++i) {
            if (headings.at(i).text.compare(headingText, Qt::CaseInsensitive) == 0) {
                index = i;
                break;
            }
        }
        if (index < 0)
            return QStringLiteral("<span class=\"unresolved\">%1</span>").arg(escapeHtml(headingText));
        const int level = headings.at(index).level;
        qsizetype end = target.text.size();
        for (qsizetype i = index + 1; i < headings.size(); ++i) {
            if (headings.at(i).level <= level) {
                end = headings.at(i).range.start;
                break;
            }
        }
        return emitRegion({headings.at(index).range.start, end}, target, true);
    }

    QString renderBlockEmbed(const QString &blockId, const ParsedNote &target)
    {
        qsizetype markerStart = -1;
        for (const BlockId &id : target.metadata.blockIds) {
            if (id.id == blockId) {
                markerStart = id.range.start;
                break;
            }
        }
        if (markerStart < 0)
            return QStringLiteral("<span class=\"unresolved\">^%1</span>").arg(escapeHtml(blockId));

        // The smallest known structural range containing the marker - a
        // paragraph, a list item, a table, or a whole callout when the block
        // id sits on a callout's own last line with nothing more specific
        // around it.
        Range best;
        auto consider = [&](const Range &r) {
            if (r.contains(markerStart) && (!best.isValid() || r.length() < best.length()))
                best = r;
        };
        for (const Range &p : target.decorations.paragraphs)
            consider(p);
        for (const ListItemSpan &item : target.decorations.listItems)
            consider(item.range);
        for (const TableSpan &t : target.decorations.tables)
            consider(t.range);
        for (const CalloutSpan &c : target.decorations.callouts)
            consider(c.range);
        if (!best.isValid())
            return QStringLiteral("<span class=\"unresolved\">^%1</span>").arg(escapeHtml(blockId));
        return QStringLiteral("<p>") + emitInline(best, target) + QStringLiteral("</p>");
    }

    QString renderAttachmentEmbed(const Link &link, const QString &relativePath, const ParsedNote &note)
    {
        Q_UNUSED(note);
        m_attachments.insert(relativePath);
        const QString href = attachmentHref(relativePath);
        const QString kind = mediaKindOf(relativePath);
        const QString alias = link.alias.isEmpty() ? QFileInfo(relativePath).fileName() : link.alias;

        if (kind == QLatin1String("image")) {
            static const QRegularExpression wxh(QStringLiteral("^(\\d+)(?:x(\\d+))?$"));
            const auto match = wxh.match(link.alias.trimmed());
            QString dims;
            if (match.hasMatch()) {
                dims = QStringLiteral(" width=\"%1\"").arg(match.captured(1));
                if (!match.captured(2).isEmpty())
                    dims += QStringLiteral(" height=\"%1\"").arg(match.captured(2));
            }
            const QString altText = match.hasMatch() ? QFileInfo(relativePath).fileName() : alias;
            return QStringLiteral("<img src=\"%1\" alt=\"%2\"%3 loading=\"lazy\">")
                  .arg(escapeAttr(href), escapeAttr(altText), dims);
        }
        if (kind == QLatin1String("audio"))
            return QStringLiteral("<audio controls src=\"%1\"></audio>").arg(escapeAttr(href));
        if (kind == QLatin1String("video"))
            return QStringLiteral("<video controls src=\"%1\"></video>").arg(escapeAttr(href));
        if (kind == QLatin1String("pdf")) {
            QString pageHref = href;
            if (link.subpath.startsWith(QLatin1String("#page=")))
                pageHref += link.subpath;
            return QStringLiteral("<a class=\"pdf-embed\" href=\"%1\">%2</a>")
                  .arg(escapeAttr(pageHref), escapeHtml(alias));
        }
        return QStringLiteral("<a href=\"%1\">%2</a>").arg(escapeAttr(href), escapeHtml(alias));
    }

    QString attachmentHref(const QString &relativePath) const
    {
        // Attachments are copied to the same relative path they hold in the
        // vault, under an "attachments/" root that cannot collide with any
        // note's own slug (those never start with it - see Publisher.cpp).
        return Slugs::relativeHref(m_pageOutputPath, QStringLiteral("attachments/") + relativePath);
    }

    // --- search text -----------------------------------------------------

    static QString plainTextOf(const QString &title, const QString &html)
    {
        QString text = html;
        static const QRegularExpression tag(QStringLiteral("<[^>]*>"));
        text.remove(tag);
        text.replace(QLatin1String("&lt;"), QLatin1String("<"));
        text.replace(QLatin1String("&gt;"), QLatin1String(">"));
        text.replace(QLatin1String("&quot;"), QLatin1String("\""));
        text.replace(QLatin1String("&amp;"), QLatin1String("&"));
        return (title + QLatin1Char(' ') + text).simplified();
    }

    const ParsedNote *m_topLevel;
    const RenderEnv &m_env;
    QString m_pageOutputPath;
    QSet<QString> m_attachments;
    QSet<QString> m_embedStack;
    int m_embedDepth = 0;
    int m_blockDepth = 0;
    int m_inlineDepth = 0;
    QList<Range> m_extraHides;
    QHash<QString, int> m_headingAnchors;
    QHash<QString, QString> m_iconCache;
    QList<TokenStyle> m_codeStyles;
    QList<PendingFormula> m_formulas;
};

ContentResult PageBody::render(const ParsedNote &note, const RenderEnv &env)
{
    Renderer renderer(note, env);
    return renderer.run();
}

} // namespace noteAppC::publish
