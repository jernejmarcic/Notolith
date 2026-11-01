/// Rich-text paste. QTextDocument::setHtml + toMarkdown(GitHub) does the real
/// work and is already linked; what it gets wrong is patched here:
///  - <pre> comes out with a blank line between every line, so each <pre> is
///    lifted out first and written back as a fence (with its language);
///  - Word's lists are <p mso-list> paragraphs with a typed-in bullet, not
///    <li>, so they are rebuilt as <ul>/<ol> before parsing;
///  - font-family in a style makes Qt write backticks, so terminal output in a
///    monospace span would arrive as code: the declaration is stripped (the
///    <code>/<pre> tags still mean code);
///  - non-breaking spaces, data: images (megabytes of base64), a default alt
///    of "image", escapes inside code spans, a lone `\*` or `\[1]` that is
///    plain text, whitespace just inside emphasis, runs of blank lines and
///    trailing spaces.
///  - QTextDocument's HTML parser is quadratic in nesting depth (50 000 nested
///    <b> took 35 s), so deeper than kMaxDepth is refused before it sees it;
///  - toMarkdown wraps paragraphs at 80 columns, so the soft wraps are joined.
/// Every scan for a closing tag is one indexOf, not a lazy regex, which would
/// be quadratic on a page of unclosed tags.
/// Limit: Qt escapes `*` and `_` only partly (it writes `2\*3*4`), so text
/// with several literal stars inside otherwise formatted HTML can still turn
/// into emphasis; a fenced block inside a list item or quote loses its indent.

#include "PasteHTML.h"

#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QTextDocument>

namespace noteAppC {

namespace {

constexpr auto kNoCase = QRegularExpression::CaseInsensitiveOption;
constexpr qsizetype kMaxDepth = 128;

using namespace Qt::StringLiterals;

/// The text between the clipboard's fragment markers, when it has them: the
/// rest is the page's head and body wrapper.
QString fragmentOf(const QString &html)
{
    constexpr QLatin1String begin("<!--StartFragment-->");
    constexpr QLatin1String end("<!--EndFragment-->");
    const qsizetype from = html.indexOf(begin);
    const qsizetype to = html.indexOf(end);
    if (from < 0 || to < from)
        return html;
    return html.mid(from + begin.size(), to - from - begin.size());
}

/// `&lt;`, `&#39;`, `&#x27;` and the like. Anything else is left as written.
QString decodeEntities(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const qsizetype semi = text.at(i) == QLatin1Char('&') ? text.indexOf(QLatin1Char(';'), i) : -1;
        if (semi < 0 || semi - i > 8) {
            out += text.at(i);
            continue;
        }
        const QStringView name = QStringView(text).mid(i + 1, semi - i - 1);
        char32_t code = 0;
        if (name == u"lt") code = '<';
        else if (name == u"gt") code = '>';
        else if (name == u"amp") code = '&';
        else if (name == u"quot") code = '"';
        else if (name == u"apos") code = '\'';
        else if (name == u"nbsp") code = ' ';
        else if (name.startsWith(u"#x") || name.startsWith(u"#X")) code = name.mid(2).toUInt(nullptr, 16);
        else if (name.startsWith(u"#")) code = name.mid(1).toUInt();
        if (code == 0 || code > 0x10FFFF) {
            out += text.at(i);
            continue;
        }
        out += QString::fromUcs4(&code, 1);
        i = semi;
    }
    return out;
}

/// The text of an HTML snippet: tags off, entities decoded, <br> a newline.
QString textOf(QString html)
{
    html.replace(QRegularExpression(QStringLiteral("<br\\s*/?>"), kNoCase), QStringLiteral("\n"));
    html.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    QString text = decodeEntities(html);
    if (text.startsWith(QLatin1Char('\n')))
        text.remove(0, 1); // HTML ignores the newline straight after <pre>
    if (text.endsWith(QLatin1Char('\n')))
        text.chop(1);
    return text;
}

/// True when the markup nests deeper than kMaxDepth, or is torn (a `<` with no
/// `>`): QTextDocument is quadratic in depth. Tags the HTML closes by itself
/// (a <p> before a <p>, a stray end tag) are allowed for, void ones never open.
bool tooDeepOrTorn(const QString &html)
{
    static const QSet<QString> voids{u"br"_s, u"img"_s, u"hr"_s, u"meta"_s, u"link"_s, u"input"_s, u"wbr"_s,
                                     u"col"_s, u"area"_s, u"base"_s, u"source"_s, u"track"_s, u"embed"_s, u"param"_s};
    static const QSet<QString> implied{u"p"_s, u"li"_s, u"dt"_s, u"dd"_s, u"tr"_s, u"td"_s, u"th"_s, u"option"_s};
    QList<QString> stack;
    for (qsizetype at = html.indexOf(QLatin1Char('<')); at >= 0; at = html.indexOf(QLatin1Char('<'), at + 1)) {
        if (at + 1 >= html.size())
            return true;
        const QChar next = html.at(at + 1);
        if (html.mid(at, 4) == QLatin1String("<!--")) {
            const qsizetype close = html.indexOf(QLatin1String("-->"), at + 4);
            if (close < 0)
                return true;
            at = close + 2;
            continue;
        }
        if (!next.isLetter() && next != QLatin1Char('/') && next != QLatin1Char('!') && next != QLatin1Char('?'))
            continue; // a lone "<" in text
        const qsizetype gt = html.indexOf(QLatin1Char('>'), at);
        if (gt < 0)
            return true;
        if (next == QLatin1Char('!') || next == QLatin1Char('?')) {
            at = gt;
            continue;
        }
        qsizetype nameStart = at + (next == QLatin1Char('/') ? 2 : 1), nameEnd = nameStart;
        while (nameEnd < gt && (html.at(nameEnd).isLetterOrNumber() || html.at(nameEnd) == QLatin1Char(':')))
            ++nameEnd;
        const QString name = html.mid(nameStart, nameEnd - nameStart).toLower();
        if (next == QLatin1Char('/')) {
            const qsizetype found = stack.lastIndexOf(name);
            if (found >= 0)
                stack.resize(found);
        } else if (!voids.contains(name) && html.at(gt - 1) != QLatin1Char('/')) {
            if (implied.contains(name) && !stack.isEmpty() && stack.last() == name)
                stack.removeLast();
            stack.append(name);
            if (stack.size() > kMaxDepth)
                return true;
        }
        at = gt;
    }
    return false;
}

/// `src` with every `<name ...>body</name>` that `open` matches replaced by
/// `make(match, body)`. The closing tag is found with one indexOf; with none
/// left, nothing after it can be a block either, so the scan ends.
template <typename Make>
QString replaceBlocks(const QString &src, const QRegularExpression &open, QLatin1String closer, Make &&make)
{
    QString out;
    qsizetype last = 0;
    for (qsizetype at = 0;;) {
        const QRegularExpressionMatch m = open.match(src, at);
        if (!m.hasMatch())
            break;
        const qsizetype close = src.indexOf(closer, m.capturedEnd(), Qt::CaseInsensitive);
        const qsizetype end = close < 0 ? -1 : src.indexOf(QLatin1Char('>'), close);
        if (end < 0)
            break;
        out += src.mid(last, m.capturedStart() - last);
        out += make(m, src.mid(m.capturedEnd(), close - m.capturedEnd()));
        last = at = end + 1;
    }
    return out + src.mid(last);
}

/// Word's list paragraphs as real lists. A paragraph is
/// `<p style='mso-list:l0 level2 lfo1'><![if !supportLists]>marker<![endif]>text</p>`;
/// the marker says bullet or number, the level says how deep.
QString wordLists(const QString &html)
{
    static const QRegularExpression para(
        QStringLiteral("<p\\b[^>]*mso-list:\\s*[^;'\"\\s]+\\s+level(\\d+)[^>]*>"), kNoCase);
    static const QRegularExpression numbered(QStringLiteral("^\\s*[0-9A-Za-z]+[.)]"));
    constexpr QLatin1String markerOpen("<![if !supportLists]>"), markerClose("<![endif]>");

    QString out;
    QList<QChar> stack; // 'u' or 'o' per open list; each has an open <li>
    auto close = [&](qsizetype depth) {
        while (stack.size() > depth)
            out += stack.takeLast() == QLatin1Char('o') ? QStringLiteral("</li></ol>") : QStringLiteral("</li></ul>");
    };
    qsizetype last = 0;
    for (qsizetype at = 0;;) {
        const QRegularExpressionMatch m = para.match(html, at);
        if (!m.hasMatch())
            break;
        const qsizetype closeAt = html.indexOf(QLatin1String("</p>"), m.capturedEnd(), Qt::CaseInsensitive);
        if (closeAt < 0)
            break;
        const QString gap = html.mid(last, m.capturedStart() - last);
        if (!gap.trimmed().isEmpty()) {
            close(0);
            out += gap;
        }
        last = at = closeAt + 4;

        QString body = html.mid(m.capturedEnd(), closeAt - m.capturedEnd());
        QChar kind = QLatin1Char('u');
        if (const qsizetype from = body.indexOf(markerOpen, 0, Qt::CaseInsensitive); from >= 0) {
            const qsizetype to = body.indexOf(markerClose, from, Qt::CaseInsensitive);
            if (to >= 0) {
                if (numbered.match(textOf(body.mid(from + markerOpen.size(), to - from - markerOpen.size()))).hasMatch())
                    kind = QLatin1Char('o');
                body.remove(from, to + markerClose.size() - from);
            }
        }
        const qsizetype level = qMin<qsizetype>(m.captured(1).toInt(), stack.size() + 1);
        if (stack.size() >= level) {
            close(level);
            out += QStringLiteral("</li>");
            if (stack.last() != kind)
                out += stack.takeLast() == QLatin1Char('o') ? QStringLiteral("</ol>") : QStringLiteral("</ul>");
        }
        if (stack.size() < level) {
            out += kind == QLatin1Char('o') ? QStringLiteral("<ol>") : QStringLiteral("<ul>");
            stack.append(kind);
        }
        out += QStringLiteral("<li>") + body;
    }
    close(0);
    return out + html.mid(last);
}

/// Markdown for a plain-text-looking line of Qt's output with its `\x`
/// escapes dropped, for comparing against the clipboard's plain text.
QString unescaped(QString md)
{
    static const QRegularExpression escape(QStringLiteral("\\\\([!-/:-@\\[-`{-~])"));
    return md.replace(escape, QStringLiteral("\\1"));
}

/// Qt wraps paragraphs at 80 columns with a trailing space before the break;
/// a real line break has none. Joins the wraps and trims every line's end.
QString unwrapped(const QString &md)
{
    static const QRegularExpression listOrHeading(QStringLiteral("^(?:[-*+] |\\d+[.)] |#|>|\\||```|~~~)"));
    const QStringList lines = md.split(QLatin1Char('\n'));
    QStringList out;
    out.reserve(lines.size());
    for (const QString &line : lines) {
        if (!out.isEmpty()) {
            const QString &prev = out.last();
            const bool wrapped = prev.endsWith(QLatin1Char(' ')) && !prev.endsWith(QLatin1String("  "));
            const bool quoted = prev.startsWith(QLatin1Char('>'));
            const QString body = line.trimmed();
            QStringView rest(line);
            while (quoted && rest.trimmed().startsWith(QLatin1Char('>')))
                rest = rest.trimmed().mid(1);
            rest = rest.trimmed();
            if (wrapped && !prev.trimmed().isEmpty() && !body.isEmpty() && !prev.startsWith(QLatin1Char('|'))
                && !listOrHeading.matchView(rest).hasMatch()) {
                out.last() += rest;
                continue;
            }
        }
        out << line;
    }
    for (QString &line : out) {
        qsizetype end = line.size();
        while (end > 0 && line.at(end - 1).isSpace())
            --end;
        line.truncate(end);
    }
    return out.join(QLatin1Char('\n'));
}

/// The length of the longest run of `ch` in `text`.
qsizetype longestRun(const QString &text, QChar ch)
{
    qsizetype run = 0, longest = 0;
    for (const QChar c : text) {
        run = c == ch ? run + 1 : 0;
        longest = qMax(longest, run);
    }
    return longest;
}

/// A document that never loads anything: a stylesheet @import or an image the
/// page names is not read from disk, whatever the page says.
class NoLoadDocument : public QTextDocument
{
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

/// The fence for `code`: longer than any backtick run inside it.
QString fenced(const QString &code, const QString &language)
{
    const QString fence(qMax<qsizetype>(3, longestRun(code, QLatin1Char('`')) + 1), QLatin1Char('`'));
    return fence + language + QLatin1Char('\n') + code + QLatin1Char('\n') + fence;
}

} // namespace

QString HTMLtoMarkdown(const QString &html, const QString &plain)
{
    if (html.size() > kMaxPasteHTML || tooDeepOrTorn(html))
        return {};

    QString src = fragmentOf(html);
    for (const char *name : {"style", "script", "head", "title"}) {
        const QRegularExpression open(QStringLiteral("<%1\\b[^>]*>").arg(QLatin1String(name)), kNoCase);
        src = replaceBlocks(src, open, QLatin1String(QByteArray("</") + name),
                            [](const QRegularExpressionMatch &, const QString &) { return QString(); });
    }
    static const QRegularExpression msoHidden(QStringLiteral("<!--\\[if[^\\]]*\\]>"), kNoCase);
    src = replaceBlocks(src, msoHidden, QLatin1String("<![endif]-->"),
                        [](const QRegularExpressionMatch &, const QString &) { return QString(); });
    src = wordLists(src);
    static const QRegularExpression msoTags(QStringLiteral("<!\\[(?:if [^\\]]*|endif)\\]>"));
    src.remove(msoTags);
    // Anything quoted is at most a few hundred bytes of font names; bounded so a page of
    // unterminated quotes cannot make every match scan to the end.
    static const QRegularExpression fontFamily(
        QStringLiteral("font-family\\s*:(?:&quot;|&#39;|'[^']{0,200}'|[^;\"']){0,300};?"), kNoCase);
    src.remove(fontFamily);
    static const QRegularExpression nbsp(QStringLiteral("&nbsp;|&#160;|&#xa0;"), kNoCase);
    src.replace(nbsp, QStringLiteral(" "));
    src.replace(QChar(0x00A0), QLatin1Char(' '));
    static const QRegularExpression dataImage(
        QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*(?:\"\\s*(?:data|file|cid):[^\"]*\"|'\\s*(?:data|file|cid):[^']*')[^>]*>"), kNoCase);
    src.remove(dataImage);

    // <pre> out of Qt's hands: each becomes a one-word paragraph, put back as a fence below.
    QStringList codes, languages;
    static const QRegularExpression preOpen(QStringLiteral("<pre\\b[^>]*>"), kNoCase);
    static const QRegularExpression language(QStringLiteral("(?:language|lang)-([\\w+#.-]+)"));
    src = replaceBlocks(src, preOpen, QLatin1String("</pre"),
                        [&](const QRegularExpressionMatch &m, const QString &body) {
        const QString lang = language.match(m.captured(0) + body.left(200)).captured(1);
        languages << lang;
        codes << textOf(body);
        return QStringLiteral("<p>NOTOLITHCODE%1X</p>").arg(codes.size() - 1);
    });

    // Inline code is marked with private-use characters and written as backticks below:
    // whether Qt writes a span at all depends on the machine's fonts, and it escapes inside one.
    constexpr QChar codeOpen(0xE000), codeClose(0xE001);
    src.remove(codeOpen).remove(codeClose);
    static const QRegularExpression codeOpening(QStringLiteral("<(?:code|tt|kbd|samp)\\b[^>]*>"), kNoCase);
    static const QRegularExpression codeClosing(QStringLiteral("</(?:code|tt|kbd|samp)\\s*>"), kNoCase);
    src.replace(codeOpening, QString(codeOpen));
    src.replace(codeClosing, QString(codeClose));

    NoLoadDocument doc;
    doc.setUndoRedoEnabled(false);
    doc.setHtml(src);
    QString md = doc.toMarkdown(QTextDocument::MarkdownDialectGitHub);

    md.replace(QChar(0x00A0), QLatin1Char(' ')).replace(QChar(0x2028), QLatin1Char('\n'));
    md.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    md.replace(QStringLiteral("![image]("), QStringLiteral("![]("));
    md = unwrapped(md);

    // Code spans: the text between the markers, unescaped, in backticks long enough to hold it.
    // Kept apart as placeholders until the tidying below is done, which must not touch a `**`
    // inside code; the placeholder is plain letters, as the fences' are.
    QStringList spans;
    QString tidied;
    qsizetype from = 0;
    for (qsizetype open = md.indexOf(codeOpen); open >= 0; open = md.indexOf(codeOpen, from)) {
        const qsizetype close = md.indexOf(codeClose, open);
        tidied += md.mid(from, open - from);
        if (close < 0) {
            from = open + 1; // an unmatched marker: drop it
            continue;
        }
        const QString code = unescaped(md.mid(open + 1, close - open - 1)).simplified();
        from = close + 1;
        if (code.isEmpty())
            continue;
        const QString ticks(longestRun(code, QLatin1Char('`')) + 1, QLatin1Char('`'));
        const bool pad = code.startsWith(QLatin1Char('`')) || code.endsWith(QLatin1Char('`'));
        spans << ticks + (pad ? QStringLiteral(" ") : QString()) + code + (pad ? QStringLiteral(" ") : QString()) + ticks;
        tidied += QStringLiteral("NOTOLITHSPAN%1X").arg(spans.size() - 1);
    }
    md = tidied + md.mid(from);
    md.remove(codeClose);
    // Qt writes "1.  first"; one space is how a note is written.
    static const QRegularExpression listMarker(QStringLiteral("^([ \\t]*)(\\d+\\.|[-*+])[ \\t]{2,}"),
                                               QRegularExpression::MultilineOption);
    md.replace(listMarker, QStringLiteral("\\1\\2 "));
    static const QRegularExpression loneStar(QStringLiteral("(?<=^|\\s)\\\\\\*(?=\\s|$)"),
                                             QRegularExpression::MultilineOption);
    md.replace(loneStar, QStringLiteral("*"));
    static const QRegularExpression bracketed(QStringLiteral("\\\\\\[([^\\[\\]\\n]*?)\\\\?\\](?![(\\[:])"));
    md.replace(bracketed, QStringLiteral("[\\1]"));

    // Whitespace belongs outside emphasis: "**bold **tail" is not bold.
    static const QRegularExpression emphasis(QStringLiteral("(\\*\\*|~~)([ \\t]*)([^*~\\n]*?)([ \\t]*)\\1"));
    md.replace(emphasis, QStringLiteral("\\2\\1\\3\\1\\4"));
    static const QRegularExpression italic(
        QStringLiteral("(?<![*\\\\\\w])\\*(?![*\\s])([^*~\\n]*?)([ \\t]+)\\*(?!\\*)"));
    md.replace(italic, QStringLiteral("*\\1*\\2"));
    static const QRegularExpression emptyEmphasis(QStringLiteral("(\\*\\*|~~)\\1"));
    md.remove(emptyEmphasis);
    static const QRegularExpression blankRuns(QStringLiteral("\\n{3,}"));
    md.replace(blankRuns, QStringLiteral("\n\n"));
    md = md.trimmed();
    if (md.isEmpty())
        return {};
    static const QRegularExpression spanToken(QStringLiteral("NOTOLITHSPAN(\\d+)X"));
    QString withSpans;
    from = 0;
    for (auto it = spanToken.globalMatch(md); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const qsizetype i = m.captured(1).toLongLong();
        withSpans += md.mid(from, m.capturedStart() - from) + (i < spans.size() ? spans.at(i) : m.captured(0));
        from = m.capturedEnd();
    }
    md = withSpans + md.mid(from);

    // Text that only sits in markup (a terminal's span, a div per line) is the plain text, untouched.
    if (codes.isEmpty() && !plain.trimmed().isEmpty() && unescaped(md).simplified() == plain.simplified())
        return {};

    static const QRegularExpression token(QStringLiteral("NOTOLITHCODE(\\d+)X"));
    QString out;
    from = 0;
    for (auto it = token.globalMatch(md); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const qsizetype i = m.captured(1).toLongLong();
        out += md.mid(from, m.capturedStart() - from);
        if (i >= codes.size()) {
            out += m.captured(0);
        } else {
            // A fence is a block: Qt can leave an <img> or text glued to the paragraph that held it.
            if (!out.isEmpty() && !out.endsWith(QLatin1Char('\n')))
                out += QStringLiteral("\n\n");
            out += fenced(codes.at(i), languages.at(i));
            if (m.capturedEnd() < md.size() && md.at(m.capturedEnd()) != QLatin1Char('\n'))
                out += QStringLiteral("\n\n");
        }
        from = m.capturedEnd();
    }
    return out + md.mid(from);
}

} // namespace noteAppC
