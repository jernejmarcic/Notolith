/// HTML in live preview edited where it is drawn. Jernej, 2026-10-02: "For
/// HTML I want rendering of it like in obsidian, but unlike obsidian I want
/// it inline editable just like the markdown", and an HTML table "editable
/// like an actual table element (like in excel for example, clicking on
/// cells, etc.)".
///
/// Nothing here is a second editor. An HTML block whose every tag the editor
/// can lay out itself is turned, after the parse, into the decorations a
/// Markdown note already produces: its tags become hidden syntax revealed by
/// the caret, `<p>` a paragraph, `<h2>` a heading, `<ul>` a list with drawn
/// bullets, `<table>` a TableSpan whose cells are the text between `<td>` and
/// `</td>` - so TableCells.cpp edits it exactly as it edits a pipe table. An
/// entity shows as its character, `<br>` as a line break and a collapsed run
/// of whitespace as one space, through DisplayMap's substitutions.
///
/// The boundary (docs/EDITOR.md, "HTML"): owned are p, div without style=,
/// br, h1-h6, ul/ol/li, blockquote, hr, pre (with a code inside), table
/// with thead/tbody/tfoot/tr/th/td, and inline b strong i em u s del strike
/// mark sub sup kbd code a span (style= only as `color:`). A block with
/// anything else - style= on any other tag, img, svg, iframe, details,
/// a comment, a nested table, misnested tags gumbo had to repair, two blocks
/// on one line - stays HtmlBlock.cpp's litehtml image and its click-to-source.
///
/// The structure comes from gumbo (already litehtml's parser), whose nodes
/// carry byte offsets into the source. It runs in the editor's parse pass,
/// off the UI thread, once per distinct block text: the result is cached
/// block-relative, so a keystroke elsewhere re-parses no HTML at all. A note
/// without HTML pays one test per block. Without gumbo (NOTEAPPC_WITH_HTML
/// off) the pass compiles to nothing and HTML shows as it did before.

#include "MarkdownEditor.h"

#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <memory>
#include <optional>

#ifdef NOTEAPPC_HAVE_GUMBO
#include <gumbo.h>
#endif

namespace noteAppC {

#ifdef NOTEAPPC_HAVE_GUMBO
namespace {

/// One block's structure, offsets from the block's own start.
struct Structure {
    QList<BlockSpan> blocks;
    QList<Range> syntax;
    QList<StyleSpan> styles;
    QList<ListItemSpan> items;
    QList<TableSpan> tables;
    QList<Substitution> substitutions;
    QList<HtmlTextSpan> text;
};

bool isSpace(QChar c)
{
    return c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\n')
           || c == QLatin1Char('\r') || c == QLatin1Char('\f');
}

bool isInlineTag(GumboTag tag)
{
    switch (tag) {
    case GUMBO_TAG_B: case GUMBO_TAG_STRONG: case GUMBO_TAG_I: case GUMBO_TAG_EM:
    case GUMBO_TAG_U: case GUMBO_TAG_S: case GUMBO_TAG_DEL: case GUMBO_TAG_STRIKE:
    case GUMBO_TAG_MARK: case GUMBO_TAG_SUB: case GUMBO_TAG_SUP: case GUMBO_TAG_KBD:
    case GUMBO_TAG_CODE: case GUMBO_TAG_A: case GUMBO_TAG_SPAN: case GUMBO_TAG_BR:
        return true;
    default:
        return false;
    }
}

/// A named entity's text, from gumbo itself - every one HTML defines, not a
/// table kept here. Empty for a name HTML does not know.
QString decodeNamedEntity(const QString &name)
{
    thread_local QHash<QString, QString> known;
    const auto cached = known.constFind(name);
    if (cached != known.constEnd())
        return *cached;
    const QByteArray source = '&' + name.toUtf8() + ';';
    GumboOptions options = kGumboDefaultOptions;
    options.fragment_context = GUMBO_TAG_BODY;
    GumboOutput *output = gumbo_parse_with_options(&options, source.constData(), size_t(source.size()));
    QString decoded;
    const GumboVector &children = output->root->v.element.children;
    if (children.length == 1) {
        const auto *node = static_cast<const GumboNode *>(children.data[0]);
        if (node->type == GUMBO_NODE_TEXT)
            decoded = QString::fromUtf8(node->v.text.text);
    }
    gumbo_destroy_output(&options, output);
    if (decoded == QString::fromUtf8(source))
        decoded.clear(); // not an entity: gumbo left it as written
    known.insert(name, decoded);
    return decoded;
}

QString decodeEntity(QStringView entity) // "&amp;", ";" included
{
    const QStringView body = entity.mid(1, entity.size() - 2);
    if (body.startsWith(QLatin1Char('#'))) {
        bool ok = false;
        const bool hex = body.size() > 1 && (body.at(1) == QLatin1Char('x') || body.at(1) == QLatin1Char('X'));
        const uint code = hex ? body.mid(2).toUInt(&ok, 16) : body.mid(1).toUInt(&ok, 10);
        if (!ok || code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return {};
        const char32_t point = char32_t(code);
        return QString::fromUcs4(&point, 1);
    }
    return decodeNamedEntity(body.toString());
}

/// Walks one gumbo tree, collecting what the editor needs. Any construct
/// outside the owned set clears `ok`, and the block goes to litehtml.
class Builder {
public:
    Builder(const QString &text, const QByteArray &bytes) : m_text(text), m_bytes(bytes)
    {
        // gumbo speaks UTF-8 bytes; the editor UTF-16 code units.
        m_at.reserve(size_t(bytes.size()) + 1);
        for (qsizetype i = 0; i < text.size(); ++i) {
            const char16_t c = text.at(i).unicode();
            int length = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
            if (QChar::isHighSurrogate(c) && i + 1 < text.size() && QChar::isLowSurrogate(text.at(i + 1).unicode())) {
                length = 4;
                for (int b = 0; b < length; ++b)
                    m_at.push_back(i);
                ++i;
                continue;
            }
            for (int b = 0; b < length; ++b)
                m_at.push_back(i);
        }
        m_at.push_back(text.size());
        m_visible.assign(size_t(text.size()), 0);
    }

    std::optional<Structure> run(const GumboNode *root)
    {
        const GumboElement &element = root->v.element;
        if (hasBlockChild(root)) {
            if (hasInlineContent(root))
                return std::nullopt;
            for (unsigned i = 0; i < element.children.length && m_ok; ++i)
                block(child(root, i));
        } else {
            leafInline(root, BlockSpan::Kind::Paragraph, 0);
        }
        if (!m_ok || m_leaves.isEmpty())
            return std::nullopt;
        return finish();
    }

private:
    struct Piece {
        Range range;
        bool lineBreak = false;
    };
    using Context = std::vector<Piece>;

    static const GumboNode *child(const GumboNode *node, unsigned i)
    {
        return static_cast<const GumboNode *>(node->v.element.children.data[i]);
    }

    qsizetype at(unsigned offset) const
    {
        return m_at[qMin<size_t>(offset, m_at.size() - 1)];
    }

    bool implied(const GumboNode *node) const
    {
        return node->parse_flags & GUMBO_INSERTION_BY_PARSER;
    }

    /// Ends with its own end tag, as written - not closed by whatever came
    /// next, which is what gumbo reports in original_end_tag then.
    bool hasEndTag(const GumboNode *node) const
    {
        const GumboElement &element = node->v.element;
        if ((node->parse_flags & GUMBO_INSERTION_IMPLICIT_END_TAG) || element.original_end_tag.length == 0)
            return false;
        GumboStringPiece name = element.original_end_tag;
        gumbo_tag_from_original_text(&name);
        return gumbo_tagn_enum(name.data, unsigned(name.length)) == element.tag;
    }

    Range openTag(const GumboNode *node) const
    {
        const GumboElement &element = node->v.element;
        return {at(element.start_pos.offset), at(element.start_pos.offset + unsigned(element.original_tag.length))};
    }

    Range closeTag(const GumboNode *node) const
    {
        const GumboElement &element = node->v.element;
        return {at(element.end_pos.offset), at(element.end_pos.offset + unsigned(element.original_end_tag.length))};
    }

    /// Where the element's content ends: its end tag, or where gumbo closed it.
    qsizetype contentEnd(const GumboNode *node) const
    {
        return hasEndTag(node) ? closeTag(node).start : at(node->v.element.end_pos.offset);
    }

    qsizetype trimBack(qsizetype from, qsizetype end) const
    {
        while (end > from && isSpace(m_text.at(end - 1)))
            --end;
        return end;
    }

    qsizetype elementEnd(const GumboNode *node) const
    {
        if (hasEndTag(node))
            return closeTag(node).end;
        return trimBack(openTag(node).end, contentEnd(node));
    }

    QString attribute(const GumboNode *node, const char *name) const
    {
        const GumboAttribute *found = gumbo_get_attribute(&node->v.element.attributes, name);
        return found ? QString::fromUtf8(found->value) : QString();
    }

    /// The open tag, checked: written in the source, in order, and with no
    /// style= the editor would have to ignore. Recorded as syntax.
    Range open(const GumboNode *node, bool record = true)
    {
        const GumboElement &element = node->v.element;
        if (implied(node) || element.original_tag.length == 0) {
            if (element.tag != GUMBO_TAG_HTML && element.tag != GUMBO_TAG_TBODY)
                m_ok = false; // a tag gumbo made up while repairing the markup
            return {};
        }
        if (gumbo_get_attribute(&element.attributes, "style")) {
            // Only a span's colour is read; every other style is CSS.
            static const QRegularExpression colourOnly(
                QStringLiteral("^\\s*color\\s*:\\s*[^;:]+;?\\s*$"), QRegularExpression::CaseInsensitiveOption);
            if (element.tag != GUMBO_TAG_SPAN || !colourOnly.match(attribute(node, "style")).hasMatch())
                m_ok = false;
        }
        const Range tag = openTag(node);
        if (tag.start < m_cursor)
            m_ok = false; // out of source order: gumbo moved something
        m_cursor = tag.end;
        if (record && m_ok)
            m_tags.append(tag);
        return tag;
    }

    void close(const GumboNode *node)
    {
        if (!hasEndTag(node))
            return;
        const Range tag = closeTag(node);
        if (tag.start < m_cursor)
            m_ok = false;
        m_cursor = tag.end;
        m_tags.append(tag);
    }

    bool hasBlockChild(const GumboNode *node) const
    {
        const GumboVector &children = node->v.element.children;
        for (unsigned i = 0; i < children.length; ++i) {
            const GumboNode *c = child(node, i);
            if (c->type == GUMBO_NODE_ELEMENT && !isInlineTag(c->v.element.tag))
                return true;
        }
        return false;
    }

    bool hasInlineContent(const GumboNode *node) const
    {
        const GumboVector &children = node->v.element.children;
        for (unsigned i = 0; i < children.length; ++i) {
            const GumboNode *c = child(node, i);
            if (c->type == GUMBO_NODE_TEXT || (c->type == GUMBO_NODE_ELEMENT && isInlineTag(c->v.element.tag)))
                return true;
        }
        return false;
    }

    /// Whitespace between blocks: only checked for order, never shown.
    bool skipWhitespace(const GumboNode *node)
    {
        if (node->type != GUMBO_NODE_WHITESPACE)
            return false;
        const qsizetype start = at(node->v.text.start_pos.offset);
        if (start < m_cursor)
            m_ok = false;
        m_cursor = at(node->v.text.start_pos.offset + unsigned(node->v.text.original_text.length));
        return true;
    }

    void block(const GumboNode *node)
    {
        if (skipWhitespace(node))
            return;
        if (node->type != GUMBO_NODE_ELEMENT) {
            m_ok = false; // text beside blocks, a comment
            return;
        }
        const GumboTag tag = node->v.element.tag;
        switch (tag) {
        case GUMBO_TAG_DIV:
            if (!hasBlockChild(node)) {
                leafInline(node, BlockSpan::Kind::Paragraph, 0);
            } else if (hasInlineContent(node)) {
                m_ok = false;
            } else { // a container: its tags are syntax, its blocks are blocks
                open(node);
                const GumboVector &children = node->v.element.children;
                for (unsigned i = 0; i < children.length && m_ok; ++i)
                    block(child(node, i));
                close(node);
            }
            return;
        case GUMBO_TAG_P:
            leafInline(node, BlockSpan::Kind::Paragraph, 0);
            return;
        case GUMBO_TAG_H1: case GUMBO_TAG_H2: case GUMBO_TAG_H3:
        case GUMBO_TAG_H4: case GUMBO_TAG_H5: case GUMBO_TAG_H6:
            leafInline(node, BlockSpan::Kind::Heading, int(tag - GUMBO_TAG_H1) + 1);
            return;
        case GUMBO_TAG_BLOCKQUOTE:
            quote(node);
            return;
        case GUMBO_TAG_UL:
        case GUMBO_TAG_OL: {
            const qsizetype start = openTag(node).start;
            m_firstItem = true;
            list(node, 0);
            addLeaf(BlockSpan::Kind::List, {start, elementEnd(node)}, 0);
            return;
        }
        case GUMBO_TAG_PRE:
            pre(node);
            return;
        case GUMBO_TAG_HR: {
            const Range tag = open(node);
            if (node->v.element.children.length > 0)
                m_ok = false;
            addLeaf(BlockSpan::Kind::Rule, tag, 0);
            return;
        }
        case GUMBO_TAG_TABLE:
            table(node);
            return;
        default:
            m_ok = false;
            return;
        }
    }

    void addLeaf(BlockSpan::Kind kind, const Range &range, int level)
    {
        if (!m_ok)
            return;
        m_leaves.append({kind, range, level});
    }

    void addText(const GumboNode *node, Context &context)
    {
        const GumboText &text = node->v.text;
        const Range range{at(text.start_pos.offset), at(text.start_pos.offset + unsigned(text.original_text.length))};
        if (range.start < m_cursor)
            m_ok = false;
        m_cursor = range.end;
        context.push_back({range});

        // Entities, each one character on screen; `&amp` without its ";"
        // (which HTML also accepts) shows as written.
        static const QRegularExpression entity(
            QStringLiteral("&(#[0-9]{1,7}|#[xX][0-9a-fA-F]{1,6}|[A-Za-z][A-Za-z0-9]{1,31});"));
        const QStringView view = QStringView(m_text).mid(range.start, range.length());
        for (auto it = entity.globalMatchView(view); it.hasNext();) {
            const QRegularExpressionMatch match = it.next();
            const QString decoded = decodeEntity(match.capturedView(0));
            if (!decoded.isEmpty())
                m_substitutions.append({{range.start + match.capturedStart(), range.start + match.capturedEnd()}, decoded});
        }
    }

    void inlineChildren(const GumboNode *node, Context &context)
    {
        const GumboVector &children = node->v.element.children;
        for (unsigned i = 0; i < children.length && m_ok; ++i)
            inlineNode(child(node, i), context);
    }

    void inlineNode(const GumboNode *node, Context &context)
    {
        if (node->type == GUMBO_NODE_TEXT || node->type == GUMBO_NODE_WHITESPACE) {
            addText(node, context);
            return;
        }
        if (node->type != GUMBO_NODE_ELEMENT || !isInlineTag(node->v.element.tag)) {
            m_ok = false;
            return;
        }
        if (node->v.element.tag == GUMBO_TAG_BR) {
            // One character on screen, a line break: not syntax that hides.
            const Range tag = open(node, false);
            m_substitutions.append({tag, QString(QChar(QChar::LineSeparator))});
            for (qsizetype i = tag.start; i < tag.end; ++i)
                m_visible[size_t(i)] = 1;
            context.push_back({tag, true});
            return;
        }
        const Range tag = open(node);
        inlineChildren(node, context);
        close(node);
        if (!m_ok)
            return;
        const Range content{tag.end, contentEnd(node)};
        if (node->v.element.tag == GUMBO_TAG_A)
            m_styles.append({StyleSpan::Kind::Link, content, 0, attribute(node, "href"), {}, {}});
        else
            m_styles.append({StyleSpan::Kind::HtmlText, content, 0, m_text.mid(tag.start, tag.length()), {}, {}});
    }

    /// HTML's whitespace rule over one run of inline text: each run of
    /// whitespace shows as one space, none at either end. What is not shown
    /// is hidden, never rewritten; a newline kept as the space is a
    /// substitution, not an edit.
    void collapse(const Context &context, bool pre)
    {
        if (pre) {
            for (const Piece &piece : context) {
                for (qsizetype i = piece.range.start; i < piece.range.end; ++i)
                    m_visible[size_t(i)] = 1;
            }
            return;
        }
        auto entity = m_substitutions.cbegin(); // sorted enough: added in source order
        bool space = true; // at the start, a space shows nothing
        qsizetype kept = -1;
        const auto dropKept = [&] {
            if (kept < 0)
                return;
            m_visible[size_t(kept)] = 0;
            m_spaces.removeOne(kept);
            kept = -1;
        };
        for (const Piece &piece : context) {
            if (piece.lineBreak) {
                dropKept();
                space = true;
                continue;
            }
            for (qsizetype i = piece.range.start; i < piece.range.end; ++i) {
                while (entity != m_substitutions.cend() && entity->range.end <= i)
                    ++entity;
                if (entity != m_substitutions.cend() && entity->range.start == i) {
                    for (qsizetype e = i; e < entity->range.end; ++e)
                        m_visible[size_t(e)] = 1;
                    i = entity->range.end - 1;
                    space = false;
                    kept = -1;
                    continue;
                }
                if (!isSpace(m_text.at(i))) {
                    m_visible[size_t(i)] = 1;
                    space = false;
                    kept = -1;
                    continue;
                }
                if (space)
                    continue; // hidden
                m_visible[size_t(i)] = 1;
                space = true;
                kept = i;
                if (m_text.at(i) != QLatin1Char(' '))
                    m_spaces.append(i); // a newline or a tab shows as the space
            }
        }
        dropKept();
    }

    void addTextSpan(const GumboNode *node, Range tag, qsizetype end)
    {
        if (!m_ok)
            return;
        if (!tag.isValid())
            tag = {0, 0}; // the top level's loose text: no tag of its own
        HtmlTextSpan span;
        span.content = {tag.end, end};
        span.openTag = tag;
        const QStringView written = QStringView(m_text).mid(tag.start + 1, qMax<qsizetype>(0, tag.length() - 1));
        qsizetype name = 0;
        while (name < written.size() && written.at(name).isLetterOrNumber())
            ++name;
        span.tag = written.left(name).toString();
        span.closed = hasEndTag(node);
        m_textSpans.append(span);
    }

    /// p, div, h1-h6 and the top level's loose text: one run of inline text.
    void leafInline(const GumboNode *node, BlockSpan::Kind kind, int level)
    {
        const Range tag = open(node);
        Context context;
        inlineChildren(node, context);
        close(node);
        if (!m_ok)
            return;
        collapse(context, false);
        const bool root = !tag.isValid();
        const qsizetype end = root ? m_cursor : contentEnd(node);
        Range content{root ? 0 : tag.end, end};
        if (kind == BlockSpan::Kind::Heading)
            m_styles.append({StyleSpan::Kind::Heading, content, level, {}, {}, {}});
        addTextSpan(node, tag, end);
        addLeaf(kind, {root ? 0 : tag.start, root ? m_cursor : elementEnd(node)}, level);
    }

    /// The newline that shows between two items, or two paragraphs of a
    /// quote: the last one before `at` in what is hidden there. An item on the
    /// line of the one before has none, and the block goes to litehtml.
    void keepBreakBefore(qsizetype at)
    {
        for (qsizetype i = at - 1; i >= 0 && !m_visible[size_t(i)]; --i) {
            if (m_text.at(i) == QLatin1Char('\n')) {
                m_visible[size_t(i)] = 1;
                return;
            }
        }
        m_ok = false;
    }

    void quote(const GumboNode *node)
    {
        const Range tag = open(node);
        bool paragraphs = false;
        const GumboVector &children = node->v.element.children;
        for (unsigned i = 0; i < children.length; ++i) {
            const GumboNode *c = child(node, i);
            paragraphs = paragraphs || (c->type == GUMBO_NODE_ELEMENT && c->v.element.tag == GUMBO_TAG_P);
        }
        if (!paragraphs) {
            Context context;
            inlineChildren(node, context);
            collapse(context, false);
            addTextSpan(node, tag, contentEnd(node));
        } else {
            bool first = true;
            for (unsigned i = 0; i < children.length && m_ok; ++i) {
                const GumboNode *c = child(node, i);
                if (skipWhitespace(c))
                    continue;
                if (c->type != GUMBO_NODE_ELEMENT || c->v.element.tag != GUMBO_TAG_P) {
                    m_ok = false;
                    break;
                }
                const Range p = open(c);
                if (!first)
                    keepBreakBefore(p.start);
                first = false;
                Context context;
                inlineChildren(c, context);
                close(c);
                collapse(context, false);
                addTextSpan(c, p, contentEnd(c));
            }
        }
        close(node);
        addLeaf(BlockSpan::Kind::Quote, {tag.start, elementEnd(node)}, 0);
    }

    void list(const GumboNode *node, int depth)
    {
        open(node);
        const bool ordered = node->v.element.tag == GUMBO_TAG_OL;
        bool startOK = true;
        int number = ordered ? attribute(node, "start").toInt(&startOK) : 0;
        if (!ordered || !startOK)
            number = 1;
        const GumboVector &children = node->v.element.children;
        for (unsigned i = 0; i < children.length && m_ok; ++i) {
            const GumboNode *c = child(node, i);
            if (skipWhitespace(c))
                continue;
            if (c->type != GUMBO_NODE_ELEMENT || c->v.element.tag != GUMBO_TAG_LI) {
                m_ok = false;
                break;
            }
            item(c, depth, ordered, number++);
        }
        close(node);
    }

    void item(const GumboNode *node, int depth, bool ordered, int number)
    {
        const Range tag = open(node);
        if (!m_firstItem)
            keepBreakBefore(tag.start);
        m_firstItem = false;

        ListItemSpan item;
        item.range = {tag.start, elementEnd(node)};
        item.marker = tag;
        item.indent = depth;
        item.ordered = ordered;
        item.number = item.shownNumber = number;
        item.bullet = ordered ? QLatin1Char('.') : QLatin1Char('-');
        const qsizetype itemIndex = m_items.size();
        m_items.append(item);

        // Its own text, then at most its nested lists.
        Context context;
        bool nested = false;
        const GumboVector &children = node->v.element.children;
        for (unsigned i = 0; i < children.length && m_ok; ++i) {
            const GumboNode *c = child(node, i);
            const bool sublist = c->type == GUMBO_NODE_ELEMENT
                                 && (c->v.element.tag == GUMBO_TAG_UL || c->v.element.tag == GUMBO_TAG_OL);
            if (sublist) {
                if (!nested)
                    collapse(context, false);
                nested = true;
                list(c, depth + 1);
            } else if (nested && !skipWhitespace(c)) {
                m_ok = false; // text after a nested list: two runs in one item
            } else if (!nested) {
                inlineNode(c, context);
            }
        }
        if (!nested)
            collapse(context, false);
        close(node);
        m_items[itemIndex].range.end = elementEnd(node);
        addTextSpan(node, tag, contentEnd(node));
    }

    void pre(const GumboNode *node)
    {
        const Range tag = open(node);
        Context context;
        const GumboVector &children = node->v.element.children;
        for (unsigned i = 0; i < children.length && m_ok; ++i) {
            const GumboNode *c = child(node, i);
            if (c->type == GUMBO_NODE_TEXT || c->type == GUMBO_NODE_WHITESPACE) {
                addText(c, context);
            } else if (c->type == GUMBO_NODE_ELEMENT && c->v.element.tag == GUMBO_TAG_CODE) {
                open(c);
                const GumboVector &code = c->v.element.children;
                for (unsigned j = 0; j < code.length && m_ok; ++j) {
                    const GumboNode *t = child(c, j);
                    if (t->type == GUMBO_NODE_TEXT || t->type == GUMBO_NODE_WHITESPACE)
                        addText(t, context);
                    else
                        m_ok = false;
                }
                close(c);
            } else {
                m_ok = false;
            }
        }
        close(node);
        if (!m_ok)
            return;
        // HTML drops the newline right after <pre>; so does the display.
        if (!context.empty()) {
            Range &first = context.front().range;
            if (first.start < first.end && m_text.at(first.start) == QLatin1Char('\r'))
                ++first.start;
            if (first.start < first.end && m_text.at(first.start) == QLatin1Char('\n'))
                ++first.start;
        }
        collapse(context, true);
        addTextSpan(node, tag, contentEnd(node));
        addLeaf(BlockSpan::Kind::Code, {tag.start, elementEnd(node)}, 0);
    }

    void table(const GumboNode *node)
    {
        const Range tag = open(node);
        TableSpan span;
        span.html = true;
        int headRows = -1;
        const auto row = [&](const GumboNode *tr) {
            const Range rowTag = open(tr);
            QList<Range> cells;
            QList<Range> elements;
            bool allHeads = true;
            const GumboVector &children = tr->v.element.children;
            for (unsigned i = 0; i < children.length && m_ok; ++i) {
                const GumboNode *c = child(tr, i);
                if (skipWhitespace(c))
                    continue;
                if (c->type != GUMBO_NODE_ELEMENT
                    || (c->v.element.tag != GUMBO_TAG_TD && c->v.element.tag != GUMBO_TAG_TH)) {
                    m_ok = false;
                    break;
                }
                allHeads = allHeads && c->v.element.tag == GUMBO_TAG_TH;
                const Range cellTag = open(c);
                Context context;
                inlineChildren(c, context);
                close(c);
                collapse(context, false);
                const qsizetype end = contentEnd(c);
                qsizetype start = cellTag.end;
                while (start < end && isSpace(m_text.at(start)))
                    ++start;
                const qsizetype trimmed = trimBack(start, end);
                cells << (start < trimmed ? Range{start, trimmed} : Range{cellTag.end, cellTag.end});
                elements << Range{cellTag.start, elementEnd(c)};
                addTextSpan(c, cellTag, end);
                if (span.rows.isEmpty()) {
                    const QString align = attribute(c, "align").toLower();
                    span.alignment << (align == QLatin1String("right")    ? Qt::AlignRight
                                       : align == QLatin1String("center") ? Qt::AlignHCenter
                                                                          : Qt::AlignLeft);
                }
            }
            close(tr);
            if (cells.isEmpty())
                m_ok = false;
            span.rows << cells;
            span.cellElements << elements;
            span.rowElements << Range{rowTag.start, elementEnd(tr)};
            if (headRows < 0 && span.rows.size() == 1 && allHeads)
                headRows = -2; // a first row of <th> is the header, unless a <thead> says otherwise
        };
        const GumboVector &children = node->v.element.children;
        for (unsigned i = 0; i < children.length && m_ok; ++i) {
            const GumboNode *c = child(node, i);
            if (skipWhitespace(c))
                continue;
            if (c->type != GUMBO_NODE_ELEMENT) {
                m_ok = false;
                break;
            }
            const GumboTag section = c->v.element.tag;
            if (section == GUMBO_TAG_TR) {
                row(c);
            } else if (section == GUMBO_TAG_THEAD || section == GUMBO_TAG_TBODY || section == GUMBO_TAG_TFOOT) {
                open(c);
                const GumboVector &rows = c->v.element.children;
                for (unsigned j = 0; j < rows.length && m_ok; ++j) {
                    const GumboNode *tr = child(c, j);
                    if (skipWhitespace(tr))
                        continue;
                    if (tr->type == GUMBO_NODE_ELEMENT && tr->v.element.tag == GUMBO_TAG_TR)
                        row(tr);
                    else
                        m_ok = false;
                }
                close(c);
                if (section == GUMBO_TAG_THEAD && headRows < 0 && !implied(c))
                    headRows = int(span.rows.size());
            } else {
                m_ok = false; // caption, colgroup
            }
        }
        close(node);
        if (!m_ok || span.rows.isEmpty())
            return;
        span.headerRows = headRows >= 0 ? headRows : headRows == -2 ? 1 : 0;
        for (const QList<Range> &cells : std::as_const(span.rows))
            span.columns = qMax(span.columns, int(cells.size()));
        while (span.alignment.size() < span.columns)
            span.alignment << Qt::AlignLeft;
        span.range = {tag.start, elementEnd(node)};
        m_tables.append(span);
        addLeaf(BlockSpan::Kind::Table, span.range, 0);
    }

    std::optional<Structure> finish()
    {
        Structure out;
        const qsizetype size = m_text.size();

        // Each block element a block of whole lines: what lies between two
        // of them up to the first newline goes with the one before, the rest
        // with the one after. Two on one line cannot be told apart that way.
        out.blocks = m_leaves;
        for (qsizetype i = 0; i < out.blocks.size(); ++i) {
            BlockSpan &leaf = out.blocks[i];
            if (i == 0)
                leaf.range.start = 0;
            if (i + 1 == out.blocks.size()) {
                leaf.range.end = size;
                continue;
            }
            const qsizetype gap = m_text.indexOf(QLatin1Char('\n'), leaf.range.end);
            if (gap < 0 || gap >= out.blocks.at(i + 1).range.start)
                return std::nullopt;
            leaf.range.end = gap; // the block owns that newline (rebuildBlocks)
            m_visible[size_t(gap)] = 1; // and shows it, as a Markdown block does
            out.blocks[i + 1].range.start = gap + 1;
        }
        for (TableSpan &table : m_tables) {
            for (const BlockSpan &leaf : std::as_const(out.blocks)) {
                if (leaf.kind == BlockSpan::Kind::Table && leaf.range.contains(table.range.start))
                    table.range = leaf.range; // tableFor matches the block's start
            }
        }
        // A list block is split at its items' lines; the first item takes
        // the block's own first lines, the `<ul>` among them.
        for (const BlockSpan &leaf : std::as_const(out.blocks)) {
            if (leaf.kind != BlockSpan::Kind::List)
                continue;
            for (ListItemSpan &item : m_items) {
                if (item.indent == 0 && leaf.range.contains(item.range.start)) {
                    item.range.start = leaf.range.start;
                    break;
                }
            }
        }

        // Everything not shown is syntax, one range per tag and per stretch
        // of whitespace on a line, so the caret reveals what it touches.
        std::sort(m_tags.begin(), m_tags.end(), [](const Range &l, const Range &r) { return l.start < r.start; });
        auto tag = m_tags.cbegin();
        for (qsizetype i = 0; i < size;) {
            if (m_visible[size_t(i)]) {
                ++i;
                continue;
            }
            while (tag != m_tags.cend() && tag->end <= i)
                ++tag;
            qsizetype end = i + 1;
            if (tag != m_tags.cend() && tag->start <= i) {
                end = tag->end;
            } else if (!isSpace(m_text.at(i))) {
                return std::nullopt; // something gumbo dropped, a stray `</i>`: not ours to hide
            } else if (m_text.at(i) != QLatin1Char('\n')) {
                const qsizetype next = tag != m_tags.cend() ? tag->start : size;
                while (end < next && !m_visible[size_t(end)] && isSpace(m_text.at(end))
                       && m_text.at(end) != QLatin1Char('\n'))
                    ++end;
            }
            out.syntax.append({i, end});
            out.styles.append({StyleSpan::Kind::HtmlTag, {i, end}, 0, {}, {}, {}});
            i = end;
        }

        out.styles += m_styles;
        out.items = m_items;
        out.tables = m_tables;
        out.text = m_textSpans;
        out.substitutions = m_substitutions;
        for (const qsizetype space : std::as_const(m_spaces))
            out.substitutions.append({{space, space + 1}, QStringLiteral(" ")});
        std::sort(out.substitutions.begin(), out.substitutions.end(),
                  [](const Substitution &l, const Substitution &r) { return l.range.start < r.range.start; });
        std::sort(out.text.begin(), out.text.end(),
                  [](const HtmlTextSpan &l, const HtmlTextSpan &r) { return l.content.start < r.content.start; });
        return out;
    }

    const QString &m_text;
    const QByteArray &m_bytes;
    std::vector<qsizetype> m_at;
    std::vector<char> m_visible;
    qsizetype m_cursor = 0;
    bool m_ok = true;
    bool m_firstItem = true;
    QList<BlockSpan> m_leaves;
    QList<Range> m_tags;
    QList<StyleSpan> m_styles;
    QList<ListItemSpan> m_items;
    QList<TableSpan> m_tables;
    QList<Substitution> m_substitutions;
    QList<qsizetype> m_spaces;
    QList<HtmlTextSpan> m_textSpans;
};

std::shared_ptr<const Structure> structureOf(const QString &block)
{
    // ponytail: one cache per parse thread, emptied when it fills; an LRU if
    // a note ever carries more than 256 distinct HTML blocks.
    thread_local QHash<QString, std::shared_ptr<const Structure>> cache;
    const auto cached = cache.constFind(block);
    if (cached != cache.constEnd())
        return *cached;
    if (cache.size() >= 256)
        cache.clear();

    const QByteArray bytes = block.toUtf8();
    GumboOptions options = kGumboDefaultOptions;
    options.fragment_context = GUMBO_TAG_BODY;
    options.max_errors = 0; // the errors are not read; only the tree is
    GumboOutput *output = gumbo_parse_with_options(&options, bytes.constData(), size_t(bytes.size()));
    std::optional<Structure> structure = Builder(block, bytes).run(output->root);
    gumbo_destroy_output(&options, output);

    std::shared_ptr<const Structure> result;
    if (structure)
        result = std::make_shared<const Structure>(std::move(*structure));
    cache.insert(block, result);
    return result;
}

} // namespace

void expandEditableHTML(Decorations &decorations, const QString &text)
{
    QList<BlockSpan> blocks;
    QList<Range> owned;
    for (const BlockSpan &span : std::as_const(decorations.blocks)) {
        if (span.kind != BlockSpan::Kind::Html) {
            blocks.append(span);
            continue;
        }
        const std::shared_ptr<const Structure> structure =
            structureOf(text.mid(span.range.start, span.range.length()));
        if (!structure) {
            blocks.append(span);
            continue;
        }
        owned.append(span.range);
        const qsizetype by = span.range.start;
        const auto moved = [by](Range range) { return Range{range.start + by, range.end + by}; };
        for (BlockSpan block : structure->blocks) {
            block.range = moved(block.range);
            blocks.append(block);
        }
        for (const Range &range : structure->syntax)
            decorations.syntax.append(moved(range));
        for (StyleSpan style : structure->styles) {
            style.range = moved(style.range);
            decorations.styles.append(style);
        }
        for (ListItemSpan item : structure->items) {
            item.range = moved(item.range);
            item.marker = moved(item.marker);
            decorations.listItems.append(item);
        }
        for (TableSpan table : structure->tables) {
            table.range = moved(table.range);
            for (QList<Range> &row : table.rows) {
                for (Range &cell : row)
                    cell = moved(cell);
            }
            for (QList<Range> &row : table.cellElements) {
                for (Range &cell : row)
                    cell = moved(cell);
            }
            for (Range &row : table.rowElements)
                row = moved(row);
            decorations.tables.append(table);
        }
        for (Substitution substitution : structure->substitutions) {
            substitution.range = moved(substitution.range);
            decorations.substitutions.append(substitution);
        }
        for (HtmlTextSpan span : structure->text) {
            span.content = moved(span.content);
            span.openTag = moved(span.openTag);
            decorations.HTMLtext.append(span);
        }
    }
    if (owned.isEmpty())
        return;
    decorations.blocks = std::move(blocks);
    // The parser's own style over the whole block would paint it purple.
    decorations.styles.removeIf([&owned](const StyleSpan &span) {
        return span.kind == StyleSpan::Kind::Html && owned.contains(span.range);
    });
    std::sort(decorations.tables.begin(), decorations.tables.end(),
              [](const TableSpan &l, const TableSpan &r) { return l.range.start < r.range.start; });
}
#else
void expandEditableHTML(Decorations &, const QString &) {}
#endif

// --- editing what was laid out ------------------------------------------------

const Substitution *MarkdownEditor::shownSubstitution(qsizetype offset) const
{
    const QList<Substitution> &all = m_decorations.substitutions;
    if (all.isEmpty() || m_mode == Mode::Source)
        return nullptr;
    auto it = std::upper_bound(all.cbegin(), all.cend(), offset,
                               [](qsizetype at, const Substitution &s) { return at < s.range.start; });
    if (it == all.cbegin() || (it - 1)->range.end <= offset)
        return nullptr;
    // A block shown as its source - a <pre> under the caret - shows it raw.
    const qsizetype index = blockAt(offset);
    if (index >= 0 && index < qsizetype(m_blocks.size())) {
        const BlockView &view = m_blocks[size_t(index)];
        if (view.active && view.cells.empty() && revealsWholeBlock(view))
            return nullptr;
    }
    return &*(it - 1);
}

qsizetype MarkdownEditor::stepOverSubstitution(qsizetype target, int direction) const
{
    const Substitution *substitution = shownSubstitution(target);
    if (!substitution || substitution->range.start == target)
        return target;
    return direction < 0 ? substitution->range.start : substitution->range.end;
}

const HtmlTextSpan *MarkdownEditor::HTMLtextAt(qsizetype offset) const
{
    const QList<HtmlTextSpan> &all = m_decorations.HTMLtext;
    if (all.isEmpty() || m_mode != Mode::LivePreview)
        return nullptr;
    // Inside a tag the caret has revealed, typing edits the tag.
    const QList<Range> &syntax = m_decorations.syntax;
    auto tag = std::lower_bound(syntax.cbegin(), syntax.cend(), offset,
                                [](const Range &range, qsizetype at) { return range.start < at; });
    if (tag != syntax.cbegin() && (tag - 1)->end > offset && m_text.at((tag - 1)->start) == QLatin1Char('<'))
        return nullptr;
    // The innermost: a nested item starts after the item holding it.
    for (auto it = std::upper_bound(all.cbegin(), all.cend(), offset,
                                    [](qsizetype at, const HtmlTextSpan &s) { return at < s.content.start; });
         it != all.cbegin();) {
        --it;
        if (it->content.start <= offset && offset <= it->content.end)
            return &*it;
    }
    return nullptr;
}

QString MarkdownEditor::HTMLsafe(const QString &text, const HtmlTextSpan &span) const
{
    QString safe = text;
    safe.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    safe.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    safe.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    safe.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    const QString tag = span.tag.toLower();
    if (tag == QLatin1String("td") || tag == QLatin1String("th"))
        safe.replace(QLatin1Char('\n'), QLatin1Char(' ')); // as a pipe table's cell does
    else if (tag != QLatin1String("pre"))
        safe.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return safe;
}

bool MarkdownEditor::handleHTMLenter()
{
    if (selection().length() > 0)
        return false;
    const HtmlTextSpan *span = HTMLtextAt(m_cursor);
    if (!span)
        return false;
    const QString tag = span->tag.toLower();
    if (tag == QLatin1String("pre")) {
        insertText(QStringLiteral("\n"));
        return true;
    }
    // Inside <b> or a link a split would cross its tags: a line break instead.
    bool inInline = false;
    const qsizetype index = blockAt(m_cursor);
    if (index >= 0 && index < qsizetype(m_blocks.size())) {
        const BlockView &view = m_blocks[size_t(index)];
        for (int i = view.firstStyle; i < view.lastStyle && !inInline; ++i) {
            const StyleSpan &style = m_decorations.styles.at(i);
            inInline = (style.kind == StyleSpan::Kind::HtmlText || style.kind == StyleSpan::Kind::Link)
                       && style.range.start < m_cursor && m_cursor < style.range.end;
        }
    }
    if ((tag != QLatin1String("p") && tag != QLatin1String("li")) || inInline) {
        insertText(QStringLiteral("<br>"));
        return true;
    }
    // A new <p> or <li> at the same indentation, closed the way this one is.
    const QString indent = indentBefore(span->openTag.start).value_or(QString());
    QString split = span->closed ? QStringLiteral("</%1>").arg(span->tag) : QString();
    split += QLatin1Char('\n') + indent + QStringLiteral("<%1>").arg(span->tag);
    m_lastEdit.invalidate(); // its own undo step
    insertText(split);
    reparse(); // the new element is one before the next key needs it
    return true;
}

// --- an HTML table's rows and columns ----------------------------------------

namespace {

/// A tag's name as written in the source at `tagStart` ("td", "TH").
QString tagNameAt(const QString &text, qsizetype tagStart)
{
    qsizetype end = tagStart + 1;
    while (end < text.size() && text.at(end).isLetterOrNumber())
        ++end;
    return text.mid(tagStart + 1, end - tagStart - 1);
}

QString sameCase(const QString &name, const QString &like)
{
    return like.size() > 0 && like.at(0).isUpper() ? name.toUpper() : name.toLower();
}

} // namespace

QString MarkdownEditor::HTMLrowText(const TableSpan &table, int like, bool header) const
{
    const QList<Range> &cells = table.cellElements.value(like);
    const QString row = tagNameAt(m_text, table.rowElements.value(like).start);
    const QString cell = cells.isEmpty() ? QStringLiteral("td") : tagNameAt(m_text, cells.first().start);
    const QString name = sameCase(header ? QStringLiteral("th") : QStringLiteral("td"), cell);
    QString text = QStringLiteral("<%1>").arg(row.isEmpty() ? QStringLiteral("tr") : row);
    for (int c = 0; c < table.columns; ++c)
        text += QStringLiteral("<%1></%1>").arg(name);
    return text + QStringLiteral("</%1>").arg(row.isEmpty() ? QStringLiteral("tr") : row);
}

std::optional<QString> MarkdownEditor::indentBefore(qsizetype offset) const
{
    const qsizetype lineStart = m_lines.lineStart(m_lines.lineAt(offset));
    const QString indent = m_text.mid(lineStart, offset - lineStart);
    if (!indent.trimmed().isEmpty())
        return std::nullopt; // something else on the line before it
    return indent;
}

bool MarkdownEditor::applyHTMLtableEdits(QList<HTMLtableEdit> edits, qsizetype table, int row, int column)
{
    if (edits.isEmpty())
        return false;
    std::sort(edits.begin(), edits.end(), [](const HTMLtableEdit &l, const HTMLtableEdit &r) { return l.start > r.start; });
    m_lastEdit.invalidate();
    pushUndo();
    m_formattingTables = true; // an HTML table is never aligned anyway
    for (const HTMLtableEdit &edit : std::as_const(edits))
        applyEdit(edit.start, edit.removed, edit.text);
    m_formattingTables = false;
    m_lastEdit.invalidate();
    // The new row or column has to be cells before the caret goes in.
    reparse();
    const QList<QList<Range>> &rows = m_decorations.tables.value(table).rows;
    const QList<Range> &cells = rows.value(qBound(0, row, int(rows.size()) - 1));
    moveCursor(cells.value(qBound(0, column, int(cells.size()) - 1), Range{m_cursor, m_cursor}).start, false);
    return true;
}

bool MarkdownEditor::insertHTMLrow(qsizetype table, int row, bool after, int column)
{
    const TableSpan &span = m_decorations.tables.at(table);
    // Nothing goes into the header: a row "after" it is the body's first.
    const bool intoHeader = row < span.headerRows;
    if (intoHeader) {
        row = span.headerRows;
        after = false;
    }
    QList<HTMLtableEdit> edits;
    if (after || row >= span.rowElements.size()) {
        const int like = qMin(row, int(span.rowElements.size()) - 1);
        const Range &here = span.rowElements.at(like);
        edits.append({here.end, 0, QLatin1Char('\n') + indentBefore(here.start).value_or(QString())
                                       + HTMLrowText(span, like, false)});
        row = like + 1;
    } else {
        // Above a row that starts its line: on a line of its own, indented alike.
        const Range &here = span.rowElements.at(row);
        const std::optional<QString> indent = indentBefore(here.start);
        edits.append({here.start, 0, HTMLrowText(span, row, false) + (indent ? QLatin1Char('\n') + *indent : QString())});
    }
    return applyHTMLtableEdits(edits, table, row, column);
}

bool MarkdownEditor::fillShortHTMLrow(qsizetype table, int row)
{
    const TableSpan &span = m_decorations.tables.at(table);
    const QList<Range> &elements = span.cellElements.value(row);
    if (elements.isEmpty() || elements.size() >= span.columns)
        return false;
    const QString name = tagNameAt(m_text, elements.last().start);
    QString added;
    for (qsizetype c = elements.size(); c < span.columns; ++c)
        added += QStringLiteral("<%1></%1>").arg(name);
    const qsizetype caret = m_cursor;
    m_lastEdit.invalidate();
    pushUndo();
    applyEdit(elements.last().end, 0, added);
    m_lastEdit.invalidate();
    reparse();
    m_cursor = m_anchor = caret <= elements.last().end ? caret : caret + added.size();
    return true;
}

bool MarkdownEditor::runHTMLtableCommand(const QString &id, qsizetype table, int row, int column)
{
    const TableSpan &span = m_decorations.tables.at(table);
    const bool header = row < span.headerRows;
    const auto lineOf = [this](const Range &element) {
        // An element alone on its lines goes with them; otherwise just itself.
        const qsizetype lineStart = m_lines.lineStart(m_lines.lineAt(element.start));
        const qsizetype lineEnd = m_lines.lineEnd(m_lines.lineAt(qMax(element.start, element.end - 1)));
        const bool alone = m_text.mid(lineStart, element.start - lineStart).trimmed().isEmpty()
                           && m_text.mid(element.end, lineEnd - element.end).trimmed().isEmpty();
        if (alone && lineEnd < m_text.size())
            return Range{lineStart, lineEnd + 1};
        return element;
    };
    const auto swap = [this](QList<HTMLtableEdit> &edits, const Range &a, const Range &b) {
        edits.append({a.start, a.length(), m_text.mid(b.start, b.length())});
        edits.append({b.start, b.length(), m_text.mid(a.start, a.length())});
    };
    QList<HTMLtableEdit> edits;
    if (id.endsWith(QLatin1String("insert-row-before")))
        return insertHTMLrow(table, row, false, column);
    if (id.endsWith(QLatin1String("insert-row-after")))
        return insertHTMLrow(table, row, true, column);
    if (id.endsWith(QLatin1String("delete-row"))) {
        if (header || span.rowElements.size() <= 1)
            return false;
        const Range gone = lineOf(span.rowElements.at(row));
        edits.append({gone.start, gone.length(), QString()});
        return applyHTMLtableEdits(edits, table, qMin(row, int(span.rows.size()) - 2), column);
    }
    if (id.endsWith(QLatin1String("move-row-up")) || id.endsWith(QLatin1String("move-row-down"))) {
        const int other = row + (id.endsWith(QLatin1String("up")) ? -1 : 1);
        if (header || other < span.headerRows || other >= span.rowElements.size())
            return false;
        swap(edits, span.rowElements.at(qMin(row, other)), span.rowElements.at(qMax(row, other)));
        return applyHTMLtableEdits(edits, table, other, column);
    }
    if (id.endsWith(QLatin1String("insert-column-before")) || id.endsWith(QLatin1String("insert-column-after"))) {
        const bool after = id.endsWith(QLatin1String("after"));
        for (int r = 0; r < span.cellElements.size(); ++r) {
            const QList<Range> &cells = span.cellElements.at(r);
            if (cells.isEmpty())
                continue;
            const QString name = sameCase(r < span.headerRows ? QStringLiteral("th") : QStringLiteral("td"),
                                          tagNameAt(m_text, cells.first().start));
            const QString cell = QStringLiteral("<%1></%1>").arg(name);
            if (column < cells.size())
                edits.append({after ? cells.at(column).end : cells.at(column).start, 0, cell});
            else
                edits.append({cells.last().end, 0, cell});
        }
        return applyHTMLtableEdits(edits, table, row, column + (after ? 1 : 0));
    }
    if (id.endsWith(QLatin1String("delete-column"))) {
        if (span.columns <= 1)
            return false;
        for (const QList<Range> &cells : span.cellElements) {
            if (column < cells.size()) {
                const Range gone = lineOf(cells.at(column));
                edits.append({gone.start, gone.length(), QString()});
            }
        }
        return applyHTMLtableEdits(edits, table, row, qMin(column, span.columns - 2));
    }
    if (id.endsWith(QLatin1String("move-column-left")) || id.endsWith(QLatin1String("move-column-right"))) {
        const int other = column + (id.endsWith(QLatin1String("left")) ? -1 : 1);
        if (other < 0 || other >= span.columns)
            return false;
        for (const QList<Range> &cells : span.cellElements) {
            if (qMax(column, other) < cells.size())
                swap(edits, cells.at(qMin(column, other)), cells.at(qMax(column, other)));
        }
        return applyHTMLtableEdits(edits, table, row, other);
    }
    return false;
}

} // namespace noteAppC
