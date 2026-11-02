// SPDX-License-Identifier: GPL-3.0-or-later
#include "Markmap.h"

#include <QCoreApplication>
#include <QFont>
#include <QHash>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <map>

#include "Frontmatter.h"
#include "Parser.h"

namespace noteAppC {

QString MarkmapNode::plain() const
{
    QString out;
    for (const MarkmapRun &run : runs)
        out += run.text;
    return out;
}

namespace {

// --- parsing -----------------------------------------------------------------

/// What the application's inline parser says of each character of the body.
enum Flag : quint8 { Hidden = 1, Bold = 2, Italic = 4, Code = 8, LinkText = 16, Strike = 32 };

struct Comment {
    qsizetype start;
    QString command; ///< "fold", "foldAll"; empty for any other comment
};

int indentColumns(QStringView whitespace)
{
    int column = 0;
    for (const QChar c : whitespace)
        column = c == QLatin1Char('\t') ? (column / 4 + 1) * 4 : column + 1;
    return column;
}

void readOptions(const QString &frontmatter, MarkmapOptions &options)
{
    static const QRegularExpression line(QStringLiteral("^\\s*(colorFreezeLevel|initialExpandLevel|maxWidth)\\s*:\\s*(-?\\d+)\\s*$"),
                                         QRegularExpression::MultilineOption);
    auto matches = line.globalMatch(frontmatter);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const int value = match.captured(2).toInt();
        if (match.captured(1) == QLatin1String("colorFreezeLevel"))
            options.colorFreezeLevel = qMax(0, value);
        else if (match.captured(1) == QLatin1String("initialExpandLevel"))
            options.initialExpandLevel = value;
        else
            options.maxWidth = qMax(0, value);
    }
}

/// Whether `body[i]` may be in an SVG at all: a control character, U+FFFE or half of a surrogate pair makes
/// the whole document invalid XML, and the fence would fall back to its listing for one stray byte.
bool xmlCharacter(const QString &body, qsizetype i)
{
    const QChar c = body.at(i);
    if (c.isHighSurrogate())
        return i + 1 < body.size() && body.at(i + 1).isLowSurrogate();
    if (c.isLowSurrogate())
        return i > 0 && body.at(i - 1).isHighSurrogate();
    return c.unicode() >= 0x20 || c.isSpace() ? c.unicode() < 0xFFFE : false;
}

/// The runs of `segments` of `body`: hidden characters dropped, whitespace collapsed to single spaces and
/// trimmed at both ends, a run for each stretch of one style.
QList<MarkmapRun> runsOf(const QString &body, const QByteArray &flags, const QList<Range> &segments,
                         const QHash<qsizetype, Substitution> &substitutions)
{
    QString text;
    QByteArray styles;
    const auto push = [&](QChar c, quint8 style) {
        if (c.isSpace()) {
            if (text.isEmpty() || text.back() == QLatin1Char(' '))
                return;
            c = QLatin1Char(' ');
        }
        text += c;
        styles += char(style & ~Hidden);
    };
    bool first = true;
    for (const Range &segment : segments) {
        if (!first)
            push(QLatin1Char(' '), 0); // a continuation line joins with a space
        first = false;
        for (qsizetype i = segment.start; i < segment.end; ++i) {
            // What the note shows in place of source (an escape's backslash, an entity): its text, in the style
            // of the first character it replaces.
            if (const auto found = substitutions.constFind(i); found != substitutions.constEnd()
                && found->range.end <= segment.end && !(quint8(flags.at(i)) & Hidden)) {
                for (const QChar c : found->text)
                    push(c, quint8(flags.at(i)));
                i = found->range.end - 1;
                continue;
            }
            if (!(quint8(flags.at(i)) & Hidden) && xmlCharacter(body, i))
                push(body.at(i), quint8(flags.at(i)));
        }
    }
    while (text.endsWith(QLatin1Char(' '))) {
        text.chop(1);
        styles.chop(1);
    }
    QList<MarkmapRun> runs;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (i == 0 || styles.at(i) != styles.at(i - 1)) {
            const quint8 key = quint8(styles.at(i));
            MarkmapRun run;
            run.bold = key & Bold;
            run.italic = key & Italic;
            run.code = key & Code;
            run.link = key & LinkText;
            run.strike = key & Strike;
            runs.append(run);
        }
        runs.last().text += text.at(i);
    }
    return runs;
}

} // namespace

MarkmapTree parseMarkmap(const QString &source)
{
    MarkmapTree tree;
    const Range front = Frontmatter::find(source);
    if (front.isValid())
        readOptions(source.left(front.end), tree.options);
    const QString body = front.isValid() ? source.mid(front.end) : source;

    // The inline Markdown comes from the application's parser, run once over the body: one flag byte per
    // character says which are markers (hidden) and which style the rest has.
    QByteArray flags(body.size(), 0);
    QHash<qsizetype, Substitution> substitutions; // by where each starts
    {
        const Parser::Result parsed = Parser().parseAll(body);
        for (const Substitution &substitution : parsed.decorations.substitutions)
            substitutions.insert(substitution.range.start, substitution);
        const auto fill = [&](const Range &range, quint8 flag) {
            for (qsizetype i = qMax<qsizetype>(range.start, 0); i < qMin<qsizetype>(range.end, body.size()); ++i)
                flags[i] = char(quint8(flags.at(i)) | flag);
        };
        for (const Range &range : parsed.decorations.syntax)
            fill(range, Hidden);
        for (const StyleSpan &style : parsed.decorations.styles) {
            switch (style.kind) {
            case StyleSpan::Kind::Bold: fill(style.range, Bold); break;
            case StyleSpan::Kind::Italic: fill(style.range, Italic); break;
            case StyleSpan::Kind::InlineCode: fill(style.range, Code); break;
            case StyleSpan::Kind::Link: fill(style.range, LinkText); break;
            case StyleSpan::Kind::Strikethrough: fill(style.range, Strike); break;
            default: break;
            }
        }
    }

    // Comments are the author's commands to the map, not text.
    QList<Comment> comments;
    {
        static const QRegularExpression comment(QStringLiteral("<!--(.*?)-->"), QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression command(QStringLiteral("^\\s*markmap:\\s*([A-Za-z]+)"));
        auto matches = comment.globalMatch(body);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            for (qsizetype i = match.capturedStart(); i < match.capturedEnd(); ++i)
                flags[i] = char(quint8(flags.at(i)) | Hidden);
            const QRegularExpressionMatch known = command.match(match.captured(1));
            comments.append({match.capturedStart(), known.hasMatch() ? known.captured(1) : QString()});
        }
    }

    static const QRegularExpression heading(QStringLiteral("^ {0,3}(#{1,6})(?:[ \\t]+(.*?))?(?:[ \\t]+#+)?[ \\t]*$"));
    static const QRegularExpression item(QStringLiteral("^([ \\t]*)([-*+]|\\d{1,9}[.)])(?:[ \\t]+(.*))?$"));
    static const QRegularExpression commentLine(QStringLiteral("^\\s*(?:<!--.*?-->\\s*)+$"));

    QList<QList<Range>> segments; // per node, where its text is
    tree.nodes.append(MarkmapNode{});        // the root, empty until it is known to be a real one
    segments.append(QList<Range>{});
    struct Open {
        int key; ///< a heading's level, an item's indentation
        int node;
    };
    QList<Open> headings, items;
    QList<int> foldAll;
    int last = -1; // the node the next comment, continuation or fold belongs to
    bool lastIsItem = false, lastIsText = false, blank = true;
    int total = 0;
    qsizetype commentAt = 0;
    const auto make = [&](int parent, const Range &text) {
        ++total;
        if (tree.nodes.size() > kMarkmapMaxNodes)
            return -1;
        MarkmapNode node;
        node.parent = parent;
        node.depth = tree.nodes.at(parent).depth + 1;
        tree.nodes.append(node);
        segments.append({text});
        const int index = int(tree.nodes.size()) - 1;
        tree.nodes[parent].children.append(index);
        return index;
    };
    const auto parentOfHeading = [&] { return headings.isEmpty() ? 0 : headings.constLast().node; };

    for (qsizetype lineStart = 0; lineStart <= body.size();) {
        qsizetype lineEnd = body.indexOf(QLatin1Char('\n'), lineStart);
        const qsizetype next = lineEnd < 0 ? body.size() + 1 : lineEnd + 1;
        if (lineEnd < 0)
            lineEnd = body.size();
        qsizetype contentEnd = lineEnd;
        if (contentEnd > lineStart && body.at(contentEnd - 1) == QLatin1Char('\r'))
            --contentEnd;
        const QString line = body.mid(lineStart, contentEnd - lineStart);
        QStringList commands; // the comments that start on this line (they are sorted)
        for (; commentAt < comments.size() && comments.at(commentAt).start < next; ++commentAt) {
            if (!comments.at(commentAt).command.isEmpty())
                commands << comments.at(commentAt).command;
        }
        if (line.trimmed().isEmpty()) {
            blank = true;
            lineStart = next;
            continue;
        }
        int node = -1;
        if (commentLine.match(line).hasMatch()) {
            node = last; // a command on a line of its own is for the node above it
        } else if (const auto h = heading.match(line); h.hasMatch()) {
            const int level = int(h.capturedLength(1));
            while (!headings.isEmpty() && headings.constLast().key >= level)
                headings.removeLast();
            items.clear();
            const Range text = h.capturedStart(2) >= 0 ? Range{lineStart + h.capturedStart(2), lineStart + h.capturedEnd(2)}
                                                        : Range{lineStart, lineStart};
            node = make(parentOfHeading(), text);
            if (node >= 0)
                headings.append(Open{level, node});
            lastIsItem = lastIsText = false;
            last = node;
        } else if (const auto li = item.match(line); li.hasMatch()) {
            const int column = indentColumns(li.capturedView(1));
            while (!items.isEmpty() && items.constLast().key >= column)
                items.removeLast();
            const Range text = li.capturedStart(3) >= 0 ? Range{lineStart + li.capturedStart(3), lineStart + li.capturedEnd(3)}
                                                         : Range{lineStart, lineStart};
            node = make(items.isEmpty() ? parentOfHeading() : items.constLast().node, text);
            if (node >= 0)
                items.append(Open{column, node});
            lastIsItem = true;
            lastIsText = false;
            last = node;
        } else {
            qsizetype indent = 0;
            while (indent < line.size() && line.at(indent).isSpace())
                ++indent;
            const Range text{lineStart + indent, contentEnd};
            // `last` is -1 when what is above was left out for the cap: its continuation lines go with it,
            // not into nodes of their own (they are not more nodes of the source).
            if (lastIsItem && (!blank || indentColumns(QStringView(line).left(indent)) >= 2)) {
                if (last >= 0)
                    segments[last].append(text); // a continuation of the item above
                node = last;
            } else if (lastIsText && !blank) {
                if (last >= 0)
                    segments[last].append(text); // the next line of the same paragraph
                node = last;
            } else {
                items.clear();
                node = make(parentOfHeading(), text);
                lastIsItem = false;
                lastIsText = true;
                last = node;
            }
        }
        if (node >= 0) {
            for (const QString &command : std::as_const(commands)) {
                if (command == QLatin1String("fold"))
                    tree.nodes[node].folded = true;
                else if (command == QLatin1String("foldAll"))
                    foldAll.append(node);
            }
        }
        blank = false;
        lineStart = next;
    }

    for (int i = 1; i < tree.nodes.size(); ++i)
        tree.nodes[i].runs = runsOf(body, flags, segments.at(i), substitutions);
    tree.total = total;

    // foldAll: the node and everything below it that has something to fold (nodes are in pre-order, so the
    // subtree is the run of nodes after it that are deeper).
    for (const int at : std::as_const(foldAll)) {
        tree.nodes[at].folded = true;
        for (int i = at + 1; i < tree.nodes.size() && tree.nodes.at(i).depth > tree.nodes.at(at).depth; ++i)
            tree.nodes[i].folded = tree.nodes.at(i).folded || !tree.nodes.at(i).children.isEmpty();
    }

    // A single top-level node is the root; several get the empty one at index 0.
    if (tree.nodes.constFirst().children.size() == 1) {
        tree.nodes.removeFirst();
        for (MarkmapNode &node : tree.nodes) {
            node.parent -= 1;
            node.depth -= 1;
            for (int &child : node.children)
                child -= 1;
        }
    } else {
        tree.virtualRoot = true;
    }
    if (tree.options.initialExpandLevel >= 0) {
        for (MarkmapNode &node : tree.nodes) {
            // markmap-view's _initializeData numbers the root 1 (`depth += 1` before the first node) and folds
            // `item.state.depth >= initialExpandLevel`; the root here is 0, so one less.
            if (node.depth >= tree.options.initialExpandLevel - 1 && !node.children.isEmpty())
                node.folded = true;
        }
    }
    return tree;
}

QString markmapSourceOfNote(const QString &note)
{
    const Range front = Frontmatter::find(note);
    const QString body = front.isValid() ? note.mid(front.end) : note;
    QList<Range> code;
    for (const CodeFenceSpan &fence : Parser().parseAll(body).decorations.codeFences)
        code.append(fence.block);
    std::sort(code.begin(), code.end(), [](const Range &a, const Range &b) { return a.start < b.start; });
    QString out;
    qsizetype from = 0;
    for (const Range &block : std::as_const(code)) {
        if (block.start < from)
            continue; // inside one already cut
        out += body.mid(from, block.start - from) + QLatin1Char('\n');
        from = qMin<qsizetype>(block.end, body.size());
    }
    out += body.mid(from);
    return out.trimmed().isEmpty() ? QString() : out;
}

// --- drawing -----------------------------------------------------------------

MarkmapLook markmapLook(const QByteArray &config)
{
    MarkmapLook look;
    const QJsonObject json = QJsonDocument::fromJson(config).object();
    const auto colour = [&](const char *key, QColor &into) {
        const QColor parsed(json.value(QLatin1String(key)).toString());
        if (parsed.isValid())
            into = parsed;
    };
    colour("text", look.text);
    colour("muted", look.muted);
    colour("background", look.background);
    colour("accent", look.accent);
    for (const QJsonValue &value : json.value(QLatin1String("palette")).toArray()) {
        if (const QColor parsed(value.toString()); parsed.isValid())
            look.palette.append(parsed);
    }
    if (look.palette.isEmpty())
        look.palette.append(look.accent);
    if (const QString family = json.value(QLatin1String("font")).toString(); !family.isEmpty())
        look.family = family;
    if (const QString family = json.value(QLatin1String("mono")).toString(); !family.isEmpty())
        look.monoFamily = family;
    return look;
}

namespace {

constexpr qreal kPadX = 8;       ///< between a node's text and the ends of its underline
constexpr qreal kPadY = 3;
constexpr qreal kGapX = 52;      ///< from a node's circle to the start of its children
constexpr qreal kGapY = 6;       ///< between the subtrees of two siblings
constexpr qreal kCircle = 6;     ///< the fold circle's radius
constexpr qreal kMargin = 16;

/// A piece of a line: a stretch of one style.
struct Piece {
    QString text;
    quint8 style = 0; ///< MarkmapRun's flags, as Flag
    qreal x = 0;
};
struct Line {
    QList<Piece> pieces;
    qreal width = 0;
};

class Fonts {
public:
    Fonts(const MarkmapLook &look) : m_look(look) {}
    QFont font(quint8 style, bool root) const
    {
        QFont font(style & Code ? m_look.monoFamily : m_look.family);
        font.setPixelSize(int(std::round(m_look.pixelSize * (style & Code ? 0.92 : 1.0))));
        font.setBold(style & Bold || root);
        font.setItalic(style & Italic);
        return font;
    }
    const QFontMetricsF &metrics(quint8 style, bool root)
    {
        const int key = (style & (Bold | Italic | Code)) | (root ? 64 : 0);
        auto it = m_metrics.find(key);
        if (it == m_metrics.end())
            it = m_metrics.emplace(key, QFontMetricsF(font(style, root))).first;
        return it->second;
    }

private:
    const MarkmapLook &m_look;
    std::map<int, QFontMetricsF> m_metrics;
};

struct Shape {
    QList<Line> lines;
    qreal width = 0;  ///< the box: text and padding
    qreal height = 0;
    // The tree layout: where the node's subtree sits.
    bool shown = false;
    bool open = false;      ///< its children are drawn
    qreal subtreeHeight = 0;
    qreal centerRel = 0;    ///< the node's centre below the top of its subtree
    qreal childTopRel = 0;  ///< where its children's block starts below the top of its subtree
    qreal top = 0;          ///< absolute, of the subtree
    qreal x = 0;
    qreal centerY() const { return top + centerRel; }
};

QList<Line> wrap(const QList<MarkmapRun> &runs, Fonts &fonts, qreal maxWidth, bool root)
{
    static const QRegularExpression token(QStringLiteral("\\S+\\s*|\\s+"));
    QList<Line> lines(1);
    qreal lineWidth = 0;
    for (const MarkmapRun &run : runs) {
        const quint8 style = quint8((run.bold ? Bold : 0) | (run.italic ? Italic : 0) | (run.code ? Code : 0) | (run.link ? LinkText : 0)
                                    | (run.strike ? Strike : 0));
        const QFontMetricsF &metrics = fonts.metrics(style, root);
        auto matches = token.globalMatch(run.text);
        while (matches.hasNext()) {
            const QString text = matches.next().captured();
            const bool space = text.trimmed().isEmpty();
            // A line breaks after a space, never inside a word.
            if (maxWidth > 0 && lineWidth > 0 && !space && lineWidth + metrics.horizontalAdvance(text.trimmed()) > maxWidth) {
                lines.append(Line{});
                lineWidth = 0;
            }
            if (space && lineWidth == 0)
                continue;
            Line &line = lines.last();
            if (line.pieces.isEmpty() || line.pieces.constLast().style != style)
                line.pieces.append(Piece{QString(), style, 0});
            line.pieces.last().text += text;
            lineWidth += metrics.horizontalAdvance(text);
        }
    }
    // Where each piece starts, and each line as wide as its text: not its trailing space.
    for (Line &line : lines) {
        qreal x = 0;
        for (Piece &piece : line.pieces) {
            piece.x = x;
            x += fonts.metrics(piece.style, root).horizontalAdvance(piece.text);
        }
        if (!line.pieces.isEmpty()) {
            const Piece &end = line.pieces.constLast();
            QString kept = end.text;
            while (kept.endsWith(QLatin1Char(' ')))
                kept.chop(1);
            const QFontMetricsF &metrics = fonts.metrics(end.style, root);
            x -= metrics.horizontalAdvance(end.text) - metrics.horizontalAdvance(kept);
        }
        line.width = x;
    }
    return lines;
}

QString num(qreal value)
{
    return QString::number(value, 'f', 1);
}

} // namespace

MarkmapDrawn drawMarkmap(const MarkmapTree &tree, const MarkmapLook &look, const QSet<int> &toggled)
{
    MarkmapDrawn drawn;
    const int count = int(tree.nodes.size());
    QList<Shape> shapes(count);
    Fonts fonts(look);
    const QFontMetricsF &plain = fonts.metrics(0, false);
    const qreal lineHeight = std::ceil(plain.height() * 1.2);

    // Which nodes are on the map: the root, and below any node that is not folded.
    shapes[0].shown = true;
    for (int i = 0; i < count; ++i) {
        const MarkmapNode &node = tree.nodes.at(i);
        Shape &shape = shapes[i];
        if (!shape.shown)
            continue;
        shape.open = !node.children.isEmpty() && node.folded == toggled.contains(i); // folded, unless toggled
        for (const int child : node.children)
            shapes[child].shown = shape.open;
        ++drawn.drawn;
        const bool root = i == 0 && !tree.virtualRoot;
        shape.lines = wrap(node.runs, fonts, tree.options.maxWidth, root);
        qreal text = 0;
        for (const Line &line : std::as_const(shape.lines))
            text = qMax(text, line.width);
        shape.width = node.runs.isEmpty() ? 0 : text + 2 * kPadX;
        shape.height = (node.runs.isEmpty() ? 1 : shape.lines.size()) * lineHeight + 2 * kPadY;
    }

    // Bottom-up (children come after their parent): each subtree's height, its node centred on its children.
    for (int i = count - 1; i >= 0; --i) {
        Shape &shape = shapes[i];
        if (!shape.shown)
            continue;
        const MarkmapNode &node = tree.nodes.at(i);
        if (!shape.open) {
            shape.subtreeHeight = shape.height;
            shape.centerRel = shape.height / 2;
            continue;
        }
        qreal block = 0, firstCenter = 0, lastCenter = 0;
        for (const int child : node.children) {
            if (child != node.children.constFirst())
                block += kGapY;
            const qreal center = block + shapes[child].centerRel;
            if (child == node.children.constFirst())
                firstCenter = center;
            lastCenter = center;
            block += shapes[child].subtreeHeight;
        }
        const qreal nodeCenter = (firstCenter + lastCenter) / 2;
        const qreal top = qMin<qreal>(0, nodeCenter - shape.height / 2);
        const qreal bottom = qMax<qreal>(block, nodeCenter + shape.height / 2);
        shape.subtreeHeight = bottom - top;
        shape.centerRel = nodeCenter - top;
        shape.childTopRel = -top;
    }

    // Top-down: where everything is.
    shapes[0].top = kMargin;
    shapes[0].x = kMargin;
    qreal right = 0;
    for (int i = 0; i < count; ++i) {
        Shape &shape = shapes[i];
        if (!shape.shown)
            continue;
        right = qMax(right, shape.x + shape.width + kCircle);
        qreal y = shape.top + shape.childTopRel;
        for (const int child : tree.nodes.at(i).children) {
            if (!shapes[child].shown)
                break;
            shapes[child].x = shape.x + shape.width + kGapX;
            shapes[child].top = y;
            y += shapes[child].subtreeHeight + kGapY;
        }
    }

    // Colours go by first appearance over the whole tree (not only what is drawn), so folding a branch
    // does not recolour the others; colorFreezeLevel hands a deep node its ancestor's.
    QList<int> colourOf(count, 0);
    {
        const int freeze = tree.options.colorFreezeLevel;
        QList<int> owner(count);
        QHash<int, int> assigned;
        for (int i = 0; i < count; ++i) {
            int at = i;
            // markmap-view's util.ts keeps `path.split('.').slice(0, colorFreezeLevel)`: the first N ids of the
            // path, so the node at depth N (the root being 1) lends its colour; the root here is 0, so N - 1.
            while (freeze > 0 && tree.nodes.at(at).depth >= freeze)
                at = tree.nodes.at(at).parent;
            owner[i] = at;
            if (!assigned.contains(at))
                assigned.insert(at, int(assigned.size()));
            colourOf[i] = assigned.value(at) % look.palette.size();
        }
    }
    const auto branch = [&](int node) { return look.palette.at(colourOf.at(node)); };
    const auto strokeWidth = [](int depth) { return 1.5 + 1.5 / qMax(1, depth); };

    const int notShown = tree.total - (int(tree.nodes.size()) - (tree.virtualRoot ? 1 : 0));
    QString notice;
    if (notShown > 0) {
        notice = QCoreApplication::translate("Markmap", "Showing the first %1 of %2 nodes").arg(tree.total - notShown).arg(tree.total);
    }
    qreal bottom = shapes[0].top + shapes[0].subtreeHeight;
    qreal width = right + kMargin;
    if (!notice.isEmpty()) {
        width = qMax(width, kMargin * 2 + plain.horizontalAdvance(notice) * 0.9);
        bottom += 8 + lineHeight;
    }
    const qreal height = bottom + kMargin;
    drawn.size = QSizeF(std::ceil(width), std::ceil(height));

    QString out;
    out.reserve(count * 400);
    out += QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%1\" height=\"%2\" viewBox=\"0 0 %1 %2\">\n")
               .arg(drawn.size.width()).arg(drawn.size.height());
    out += QStringLiteral("<rect width=\"%1\" height=\"%2\" rx=\"6\" fill=\"%3\"/>\n")
               .arg(drawn.size.width()).arg(drawn.size.height()).arg(look.background.name());

    // Links first, so the underlines and circles lie over their ends.
    out += QStringLiteral("<g fill=\"none\" stroke-linecap=\"round\">\n");
    for (int i = 1; i < count; ++i) {
        const Shape &shape = shapes.at(i);
        if (!shape.shown)
            continue;
        const Shape &parent = shapes.at(tree.nodes.at(i).parent);
        const qreal x1 = parent.x + parent.width + kCircle, y1 = parent.centerY() + parent.height / 2;
        const qreal x2 = shape.x, y2 = shape.centerY() + shape.height / 2;
        const qreal middle = (x1 + x2) / 2;
        out += QStringLiteral("<path d=\"M%1 %2C%3 %2 %3 %4 %5 %4\" stroke=\"%6\" stroke-width=\"%7\"/>\n")
                   .arg(num(x1), num(y1), num(middle), num(y2), num(x2), branch(i).name(), num(strokeWidth(tree.nodes.at(i).depth)));
    }
    out += QStringLiteral("</g>\n");

    const auto family = [](const QString &name) { return name.toHtmlEscaped(); }; // which escapes the quote too
    for (int i = 0; i < count; ++i) {
        const Shape &shape = shapes.at(i);
        if (!shape.shown)
            continue;
        const MarkmapNode &node = tree.nodes.at(i);
        const bool root = i == 0 && !tree.virtualRoot;
        const QColor colour = i == 0 ? look.accent : branch(i);
        const qreal underline = shape.centerY() + shape.height / 2;
        if (shape.width > 0) {
            out += QStringLiteral("<line class=\"underline\" x1=\"%1\" y1=\"%2\" x2=\"%3\" y2=\"%2\" stroke=\"%4\" stroke-width=\"%5\" "
                                  "stroke-linecap=\"round\"/>\n")
                       .arg(num(shape.x), num(underline), num(shape.x + shape.width), colour.name(), num(strokeWidth(node.depth)));
        }
        const qreal textTop = shape.centerY() - shape.height / 2 + kPadY;
        for (int l = 0; l < shape.lines.size(); ++l) {
            const qreal baseline = textTop + l * lineHeight + (lineHeight - plain.height()) / 2 + plain.ascent();
            for (const Piece &piece : shape.lines.at(l).pieces) {
                const QFontMetricsF &metrics = fonts.metrics(piece.style, root);
                const QString shown = piece.text.trimmed();
                if (shown.isEmpty())
                    continue;
                int lead = 0;
                while (lead < piece.text.size() && piece.text.at(lead).isSpace())
                    ++lead;
                const qreal x = shape.x + kPadX + piece.x + metrics.horizontalAdvance(piece.text.left(lead));
                const qreal w = metrics.horizontalAdvance(shown);
                const QFont font = fonts.font(piece.style, root);
                if (piece.style & Code) { // a tint behind it, as markmap.js gives inline code
                    out += QStringLiteral("<rect class=\"code\" x=\"%1\" y=\"%2\" width=\"%3\" height=\"%4\" rx=\"3\" fill=\"%5\" fill-opacity=\"0.22\"/>\n")
                               .arg(num(x - 2), num(baseline - plain.ascent() - 1), num(w + 4), num(plain.height() + 2), look.muted.name());
                }
                out += QStringLiteral("<text x=\"%1\" y=\"%2\" font-family=\"%3\" font-size=\"%4\" fill=\"%5\"%6%7>%8</text>\n")
                           .arg(num(x), num(baseline), family(font.family()), QString::number(font.pixelSize()),
                                (piece.style & LinkText ? look.accent : look.text).name(),
                                font.bold() ? QStringLiteral(" font-weight=\"bold\"") : QString(),
                                font.italic() ? QStringLiteral(" font-style=\"italic\"") : QString(), shown.toHtmlEscaped());
                if (piece.style & LinkText) {
                    out += QStringLiteral("<line class=\"link\" x1=\"%1\" y1=\"%2\" x2=\"%3\" y2=\"%2\" stroke=\"%4\" stroke-width=\"1\"/>\n")
                               .arg(num(x), num(baseline + 2), num(x + w), look.accent.name());
                }
                if (piece.style & Strike) {
                    out += QStringLiteral("<line class=\"strike\" x1=\"%1\" y1=\"%2\" x2=\"%3\" y2=\"%2\" stroke=\"%4\" stroke-width=\"1\"/>\n")
                               .arg(num(x), num(baseline - plain.xHeight() / 2), num(x + w), look.text.name());
                }
            }
        }
        if (!node.children.isEmpty()) {
            const qreal cx = shape.x + shape.width, cy = underline;
            out += QStringLiteral("<circle class=\"fold\" cx=\"%1\" cy=\"%2\" r=\"%3\" stroke=\"%4\" stroke-width=\"1.5\" fill=\"%5\"/>\n")
                       .arg(num(cx), num(cy), num(kCircle), colour.name(), (shape.open ? look.background : colour).name());
            drawn.toggles.append({i, QRectF(cx - kCircle - 4, cy - kCircle - 4, 2 * (kCircle + 4), 2 * (kCircle + 4))});
        }
    }
    if (!notice.isEmpty()) {
        out += QStringLiteral("<text x=\"%1\" y=\"%2\" font-family=\"%3\" font-size=\"%4\" fill=\"%5\">%6</text>\n")
                   .arg(num(kMargin), num(bottom - lineHeight / 2 + plain.ascent() / 2), family(look.family),
                        QString::number(int(look.pixelSize * 0.9)), look.muted.name(), notice.toHtmlEscaped());
    }
    out += QStringLiteral("</svg>\n");
    drawn.svg = out.toUtf8();
    return drawn;
}

} // namespace noteAppC
