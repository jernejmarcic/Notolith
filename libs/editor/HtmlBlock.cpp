// SPDX-License-Identifier: GPL-3.0-or-later
/// Raw HTML: a top-level block (`<div>`, `<table>`, `<details>`, an `<img>`
/// with attributes Markdown's own image syntax cannot spell) rendered by
/// litehtml, and the handful of inline tags (`<b>`, `<kbd>`, `<span
/// style="color:...">`...) that show up inside an ordinary paragraph.
///
/// md4qt already gives the parser both kinds as raw HTML - see
/// libs/markdown/Parser.cpp's `MD::ItemType::RawHtml` case - one
/// `BlockSpan::Kind::Html` for a block, one `StyleSpan::Kind::Html` per tag
/// for an inline one. Before this file, both just showed as literal source
/// text (coloured purple, the fallback formatsIn still uses for whatever
/// this file's passes leave alone).
///
/// A block goes through HtmlRenderer (libs/render), which owns every
/// litehtml detail and is compiled out entirely when NOTEAPPC_WITH_HTML is
/// off or litehtml was not found - HtmlRenderer::isAvailable() is the one
/// question this file asks about that. Nothing HtmlRenderer-shaped is
/// touched until layoutHtml actually runs, which is only when a note has a
/// Html block and the caret is not inside it, so a note with no HTML in it
/// never constructs a litehtml document at all.
///
/// Inline tags are cheaper to map straight onto QTextCharFormat than to ask
/// litehtml to lay out a few words at a time - see layoutInlineHtml's own
/// comment for which tags that covers.
///
/// A block made only of tags the editor can lay out itself never gets here:
/// HtmlEdit.cpp turns it into paragraphs, lists and tables edited in place
/// after the parse. Only what that leaves as BlockSpan::Kind::Html is drawn
/// by litehtml.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include "HtmlRenderer.h"

#include <QRegularExpression>

namespace noteAppC {

QSize MarkdownEditor::htmlImageSizeAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return {};
    const BlockView &view = d->m_blocks[size_t(index)];
    return view.isHtml ? view.x().imageSize : QSize();
}

bool MarkdownEditor::layoutHtml(BlockView &view)
{
    if (view.span.kind != BlockSpan::Kind::Html)
        return false;
    if (!d->m_features.HTMLblocks)
        return false; // source shows; litehtml is never asked to build a document
    if (!HtmlRenderer::isAvailable())
        return false; // NOTEAPPC_WITH_HTML off, or litehtml not found: source shows, as today

    const QString raw = d->m_text.mid(view.span.range.start, view.span.range.length());
    if (raw.trimmed().isEmpty())
        return false;

    view.isHtml = true;
    view.extrasMut().image = QPixmap();
    view.extrasMut().cells.clear();

    // A script never runs (litehtml has no interpreter, and sanitise() strips
    // it anyway); its source is shown in its place instead, red and bold, so
    // what the note would have run is in plain sight.
    static const QRegularExpression scriptElement(
        QStringLiteral("<script\\b[^>]*>.*?(</script\\s*>|$)"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    QString shown = raw;
    const QString scriptStyle = QStringLiteral("<code style=\"color:%1;font-weight:bold;white-space:pre-wrap\">")
                                    .arg(d->m_theme.color(QStringLiteral("--text-error")).name());
    for (qsizetype from = 0;;) {
        const QRegularExpressionMatch match = scriptElement.match(shown, from);
        if (!match.hasMatch())
            break;
        const QString replacement = scriptStyle + match.captured(0).toHtmlEscaped() + QStringLiteral("</code>");
        shown.replace(match.capturedStart(), match.capturedLength(), replacement);
        from = match.capturedStart() + replacement.size();
    }
    const QString html = resolveHtmlImages(shown);
    const qreal ratio = devicePixelRatioF();
    const int width = contentWidth();
    const QColor text = d->m_theme.color(QStringLiteral("--text-normal"));
    const QColor background = d->m_theme.color(QStringLiteral("--background-primary"));
    // Cached by exactly what render() takes: the markup (already resolved to
    // real paths, so an image that starts resolving does invalidate this),
    // the width, and the two theme colours a re-skin can change. Everything
    // else HtmlRenderer::Theme carries (fonts, the link/border/code colours)
    // moves only when text/background do in this theme's own token set.
    const QString key = QStringLiteral("html|%1|%2|%3|%4")
                            .arg(html)
                            .arg(width)
                            .arg(text.name(QColor::HexArgb))
                            .arg(background.name(QColor::HexArgb));

    QPixmap pixmap = d->m_images.value(key);
    if (pixmap.isNull()) {
        HtmlRenderer::Theme theme;
        theme.text = text;
        theme.background = background;
        theme.link = d->m_theme.color(QStringLiteral("--text-accent"));
        theme.border = d->m_theme.color(QStringLiteral("--background-modifier-border"));
        theme.codeBackground = d->m_theme.color(QStringLiteral("--background-primary-alt"));
        theme.textFont = d->m_theme.textFont();
        theme.codeFont = d->m_theme.monospaceFont();
        // Every local image, a CSS url() and an unquoted src too, is the vault's to find: the note's own
        // resolver, or nothing - never a path as written (which could be absolute, or climb out).
        const QImage image = HtmlRenderer::render(html, theme, width, QString(), ratio, QString(), nullptr,
                                                  [this](const QString &path) {
                                                      return m_resolver ? m_resolver(path) : QString();
                                                  });
        if (image.isNull())
            return false; // falls back to showing the source, not to an error
        pixmap = QPixmap::fromImage(image);
        d->m_images.insert(key, pixmap);
    }

    const int height = int(pixmap.height() / pixmap.devicePixelRatio());
    view.extrasMut().image = pixmap;
    view.extrasMut().imageSize = QSize(int(pixmap.width() / pixmap.devicePixelRatio()), height);
    view.extrasMut().imageCentred = false;
    view.layout.reset();
    view.height = height + 8;
    view.laidOut = true;
    return true;
}

QString MarkdownEditor::resolveHtmlImages(const QString &html) const
{
    if (!m_resolver)
        return html;

    // Rewrites only the quoted value of a `src=` attribute, not whole tags:
    // simpler, and just as correct here, since nothing else in an HTML
    // block's markup is named "src".
    static const QRegularExpression src(QStringLiteral("\\bsrc\\s*=\\s*(\"([^\"]*)\"|'([^']*)')"),
                                        QRegularExpression::CaseInsensitiveOption);

    QString out;
    out.reserve(html.size());
    qsizetype last = 0;
    for (auto it = src.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const bool doubleQuoted = match.capturedStart(2) != -1;
        const QString value = doubleQuoted ? match.captured(2) : match.captured(3);
        out += html.mid(last, match.capturedStart() - last);
        if (value.isEmpty() || value.contains(QStringLiteral("://")) || value.startsWith(QLatin1String("data:"))) {
            out += match.captured(0); // remote, embedded, or empty: HtmlRenderer never fetches it either way
        } else {
            const QString resolved = m_resolver(value);
            const QChar quote = doubleQuoted ? QLatin1Char('"') : QLatin1Char('\'');
            out += QLatin1String("src=") + quote + resolved + quote;
        }
        last = match.capturedEnd();
    }
    out += html.mid(last);
    return out;
}

QList<MarkdownEditor::InlineHTMLpair> MarkdownEditor::inlineHTMLpairs(const BlockView &view) const
{
    QList<InlineHTMLpair> pairs;
    // A whole-block HTML element is litehtml's job (layoutHtml, above), not
    // this pairing pass - the two never touch the same span.
    if (view.span.kind == BlockSpan::Kind::Html)
        return pairs;

    // Litehtml per inline span was tried first and dropped: a `<kbd>` or a
    // coloured `<span>` is a handful of characters inside a paragraph
    // that is itself already laid out by QTextLayout, so asking litehtml to
    // lay out and rasterise a two-word document for each one would cost far
    // more than it draws - and the block's own QTextLayout already does
    // everything these tags need (bold, italic, underline, strike-through,
    // a monospace/background run, a colour). Recognised: `<b>`/`<strong>`,
    // `<i>`/`<em>`, `<u>`, `<s>`/`<del>`/`<strike>`, `<kbd>`/`<code>`,
    // `<mark>`, `<sup>`, `<sub>`, and `<span style="color:...">` (only the
    // `color:` declaration is read; anything else in `style=` is left as it
    // is written, not parsed as CSS). Everything else - `<div>` inline,
    // `<table>`, an unrecognised tag, a mismatched or unmatched one - is left
    // showing as raw source, the way an unrenderable formula is.
    struct OpenTag {
        QString name;
        Range range;
    };
    std::vector<OpenTag> stack;

    for (int i = view.firstStyle; i < view.lastStyle; ++i) {
        const StyleSpan &span = d->m_decorations.styles.at(i);
        if (span.kind != StyleSpan::Kind::Html || !span.range.overlaps(view.span.range))
            continue;

        const QString raw = d->m_text.mid(span.range.start, span.range.length());
        const bool closing = raw.startsWith(QLatin1String("</"));
        static const QRegularExpression nameRe(QStringLiteral("^</?\\s*([a-zA-Z][a-zA-Z0-9]*)"));
        const QRegularExpressionMatch match = nameRe.match(raw);
        if (!match.hasMatch())
            continue;
        const QString name = match.captured(1).toLower();

        if (!closing) {
            // An <img> is an embed (an EmbedSpan), drawn or left as source,
            // and never has a closer to wait for.
            if (raw.endsWith(QLatin1String("/>")) || name == QLatin1String("br")
                || name == QLatin1String("img"))
                continue; // self-closing or void: nothing to pair with
            stack.push_back({name, span.range});
            continue;
        }
        if (stack.empty() || stack.back().name != name)
            continue; // an unmatched or mismatched closer

        const OpenTag open = stack.back();
        stack.pop_back();
        const std::optional<QTextCharFormat> format =
            htmlTagFormat(name, d->m_text.mid(open.range.start, open.range.length()));
        if (name == QLatin1String("script")) {
            QTextCharFormat warning;
            warning.setForeground(d->m_theme.color(QStringLiteral("--text-error")));
            warning.setFontWeight(QFont::Bold);
            pairs.append({open.range, span.range, warning, true});
        } else if (format) {
            pairs.append({open.range, span.range, *format});
        }
    }
    return pairs;
}

void MarkdownEditor::layoutInlineHtml(BlockView &view, QList<Range> &hidden)
{
    // Tags the caret has revealed stay as written, still formatting the text
    // between them, the way `**bold**` keeps its weight with its asterisks shown.
    for (const InlineHTMLpair &pair : inlineHTMLpairs(view)) {
        if (pair.shown) {
            view.extrasMut().inlineHtmlFormats.push_back({Range{pair.open.start, pair.close.end}, pair.format});
            continue;
        }
        if (!view.revealed.contains(pair.open))
            hidden.append(pair.open);
        if (!view.revealed.contains(pair.close))
            hidden.append(pair.close);
        view.extrasMut().inlineHtmlFormats.push_back({Range{pair.open.end, pair.close.start}, pair.format});
    }
}

std::optional<QTextCharFormat> MarkdownEditor::htmlTagFormat(const QString &name,
                                                             const QString &openTag) const
{
    QTextCharFormat format;
    if (name == QLatin1String("b") || name == QLatin1String("strong")) {
        format.setFontWeight(QFont::Bold);
    } else if (name == QLatin1String("i") || name == QLatin1String("em")) {
        format.setFontItalic(true);
    } else if (name == QLatin1String("u")) {
        format.setUnderlineStyle(QTextCharFormat::SingleUnderline);
        if (const QColor line = d->m_theme.color(QStringLiteral("--underline-decoration-color")); line.isValid())
            format.setUnderlineColor(line);
        if (const QColor text = d->m_theme.color(QStringLiteral("--underline-color")); text.isValid())
            format.setForeground(text);
    } else if (name == QLatin1String("s") || name == QLatin1String("del")
              || name == QLatin1String("strike")) {
        strikeThrough(format, d->m_theme);
    } else if (name == QLatin1String("kbd") || name == QLatin1String("code")) {
        format.setFont(d->m_theme.monospaceFont());
        format.setForeground(d->m_theme.color(QStringLiteral("--code-normal")));
        format.setBackground(d->m_theme.color(QStringLiteral("--code-background")));
    } else if (name == QLatin1String("mark")) {
        format.setBackground(d->m_theme.color(QStringLiteral("--text-highlight-bg")));
        format.setForeground(d->m_theme.color(QStringLiteral("--text-highlight-fg")));
    } else if (name == QLatin1String("sup")) {
        format.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
    } else if (name == QLatin1String("sub")) {
        format.setVerticalAlignment(QTextCharFormat::AlignSubScript);
    } else if (name == QLatin1String("span")) {
        // Only a `color:` declaration is read - the one property a note
        // actually reaches for `<span style="color: ...">` to get. Anything
        // else in the attribute is left alone rather than parsed as CSS.
        static const QRegularExpression colorRe(QStringLiteral("color\\s*:\\s*([^;\"']+)"),
                                                QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = colorRe.match(openTag);
        if (!match.hasMatch())
            return std::nullopt;
        const QColor colour(match.captured(1).trimmed());
        if (!colour.isValid())
            return std::nullopt;
        format.setForeground(colour);
    } else {
        return std::nullopt;
    }
    return format;
}

} // namespace noteAppC
