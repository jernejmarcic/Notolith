// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "SafeHtml.h"

#include <QRegularExpression>
#include <QSet>

#ifdef NOTEAPPC_HAVE_GUMBO
#include <gumbo.h>
#endif

namespace noteAppC::publish::SafeHtml {

bool isSafeURL(const QString &url)
{
    QString text;
    text.reserve(url.size());
    qsizetype start = 0;
    while (start < url.size() && url.at(start).unicode() <= 0x20)
        ++start;
    for (qsizetype i = start; i < url.size(); ++i) {
        const char16_t c = url.at(i).unicode();
        if (c != '\t' && c != '\n' && c != '\r')
            text += url.at(i);
    }
    while (!text.isEmpty() && text.back().unicode() <= 0x20)
        text.chop(1);
    static const QRegularExpression scheme(QStringLiteral("^([A-Za-z][A-Za-z0-9+.-]*):"));
    const QRegularExpressionMatch match = scheme.match(text);
    if (!match.hasMatch())
        return true; // relative
    const QString name = match.captured(1).toLower();
    return name == QLatin1String("http") || name == QLatin1String("https") || name == QLatin1String("mailto");
}

namespace {

QString escape(const QString &text, bool attribute)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '&': out += QLatin1String("&amp;"); break;
        case '<': out += QLatin1String("&lt;"); break;
        case '>': out += QLatin1String("&gt;"); break;
        case '"':
            if (attribute)
                out += QLatin1String("&quot;");
            else
                out += c;
            break;
        default: out += c;
        }
    }
    return out;
}

} // namespace

#ifdef NOTEAPPC_HAVE_GUMBO

namespace {

const QSet<QString> &allowedTags()
{
    static const QSet<QString> tags{
        QStringLiteral("a"), QStringLiteral("abbr"), QStringLiteral("address"), QStringLiteral("article"),
        QStringLiteral("aside"), QStringLiteral("audio"), QStringLiteral("b"), QStringLiteral("bdi"),
        QStringLiteral("bdo"), QStringLiteral("blockquote"), QStringLiteral("br"), QStringLiteral("caption"),
        QStringLiteral("cite"), QStringLiteral("code"), QStringLiteral("col"), QStringLiteral("colgroup"),
        QStringLiteral("data"), QStringLiteral("dd"), QStringLiteral("del"), QStringLiteral("details"),
        QStringLiteral("dfn"), QStringLiteral("div"), QStringLiteral("dl"), QStringLiteral("dt"),
        QStringLiteral("em"), QStringLiteral("figcaption"), QStringLiteral("figure"), QStringLiteral("footer"),
        QStringLiteral("h1"), QStringLiteral("h2"), QStringLiteral("h3"), QStringLiteral("h4"),
        QStringLiteral("h5"), QStringLiteral("h6"), QStringLiteral("header"), QStringLiteral("hr"),
        QStringLiteral("i"), QStringLiteral("img"), QStringLiteral("ins"), QStringLiteral("kbd"),
        QStringLiteral("li"), QStringLiteral("main"), QStringLiteral("mark"), QStringLiteral("nav"),
        QStringLiteral("ol"), QStringLiteral("p"), QStringLiteral("picture"), QStringLiteral("pre"),
        QStringLiteral("q"), QStringLiteral("rp"), QStringLiteral("rt"), QStringLiteral("ruby"),
        QStringLiteral("s"), QStringLiteral("samp"), QStringLiteral("section"), QStringLiteral("small"),
        QStringLiteral("source"), QStringLiteral("span"), QStringLiteral("strong"), QStringLiteral("sub"),
        QStringLiteral("summary"), QStringLiteral("sup"), QStringLiteral("table"), QStringLiteral("tbody"),
        QStringLiteral("td"), QStringLiteral("tfoot"), QStringLiteral("th"), QStringLiteral("thead"),
        QStringLiteral("time"), QStringLiteral("tr"), QStringLiteral("u"), QStringLiteral("ul"),
        QStringLiteral("var"), QStringLiteral("video"), QStringLiteral("wbr")};
    return tags;
}

bool isVoid(const QString &tag)
{
    static const QSet<QString> voids{QStringLiteral("br"), QStringLiteral("col"), QStringLiteral("hr"),
                                     QStringLiteral("img"), QStringLiteral("source"), QStringLiteral("wbr")};
    return voids.contains(tag);
}

/// Elements whose content is not text for the reader (code, a frame, a form):
/// gone with the element, not unwrapped.
bool dropsContent(const QString &tag)
{
    static const QSet<QString> drop{
        QStringLiteral("script"),   QStringLiteral("style"),  QStringLiteral("iframe"),
        QStringLiteral("frame"),    QStringLiteral("frameset"), QStringLiteral("object"),
        QStringLiteral("embed"),    QStringLiteral("applet"), QStringLiteral("form"),
        QStringLiteral("template"), QStringLiteral("noscript"), QStringLiteral("textarea"),
        QStringLiteral("select"),   QStringLiteral("title"),  QStringLiteral("head"),
        QStringLiteral("noembed"),  QStringLiteral("noframes"), QStringLiteral("xmp"),
        QStringLiteral("plaintext")};
    return drop.contains(tag);
}

bool safeStyle(const QString &value)
{
    const QString lower = value.toLower();
    for (const char *bad : {"url(", "expression", "javascript", "@import", "behavior", "binding", "image-set",
                            "src(", "\\", "/*"})
        if (lower.contains(QLatin1String(bad)))
            return false;
    return true;
}

/// The attribute as it may be written, or an empty string when it may not be.
QString safeAttribute(const QString &tag, const QString &name, const QString &value)
{
    static const QSet<QString> plain{
        QStringLiteral("class"), QStringLiteral("id"),      QStringLiteral("title"), QStringLiteral("lang"),
        QStringLiteral("dir"),   QStringLiteral("hidden"),  QStringLiteral("role"),  QStringLiteral("alt"),
        QStringLiteral("width"), QStringLiteral("height"),  QStringLiteral("colspan"), QStringLiteral("rowspan"),
        QStringLiteral("scope"), QStringLiteral("headers"), QStringLiteral("span"),  QStringLiteral("start"),
        QStringLiteral("type"),  QStringLiteral("reversed"), QStringLiteral("value"), QStringLiteral("open"),
        QStringLiteral("datetime"), QStringLiteral("controls"), QStringLiteral("loop"), QStringLiteral("muted"),
        QStringLiteral("preload"), QStringLiteral("loading"), QStringLiteral("align"), QStringLiteral("valign")};
    bool ok = false;
    if (name.startsWith(QLatin1String("on")) || name == QLatin1String("srcdoc"))
        return {};
    if (name == QLatin1String("style"))
        ok = safeStyle(value);
    else if (name.startsWith(QLatin1String("data-")) || name.startsWith(QLatin1String("aria-")))
        ok = true;
    else if (name == QLatin1String("href"))
        ok = tag == QLatin1String("a") && isSafeURL(value);
    else if (name == QLatin1String("src") || name == QLatin1String("poster"))
        ok = (tag == QLatin1String("img") || tag == QLatin1String("audio") || tag == QLatin1String("video")
              || tag == QLatin1String("source") || name == QLatin1String("poster"))
             && isSafeURL(value);
    else if (name == QLatin1String("cite"))
        ok = (tag == QLatin1String("q") || tag == QLatin1String("blockquote")) && isSafeURL(value);
    else
        ok = plain.contains(name);
    return ok ? QLatin1Char(' ') + name + QLatin1String("=\"") + escape(value, true) + QLatin1Char('"') : QString();
}

QString startTag(const GumboElement &element, const QString &tag)
{
    QString out = QLatin1Char('<') + tag;
    bool link = false;
    for (unsigned i = 0; i < element.attributes.length; ++i) {
        const auto *attribute = static_cast<const GumboAttribute *>(element.attributes.data[i]);
        const QString written = safeAttribute(tag, QString::fromUtf8(attribute->name).toLower(),
                                              QString::fromUtf8(attribute->value));
        link = link || written.startsWith(QLatin1String(" href="));
        out += written;
    }
    if (link)
        out += QLatin1String(" rel=\"noopener noreferrer\"");
    return out + QLatin1Char('>');
}

bool isHTMLElement(const GumboNode *node)
{
    return node->type == GUMBO_NODE_ELEMENT && node->v.element.tag_namespace == GUMBO_NAMESPACE_HTML;
}

QString tagName(const GumboElement &element)
{
    if (element.tag == GUMBO_TAG_UNKNOWN)
        return {}; // a name gumbo does not know is not in the list
    return QString::fromLatin1(gumbo_normalized_tagname(element.tag));
}

QString serialise(const GumboNode *node, int depth)
{
    if (depth > 200)
        return {}; // a pathological nesting is cut, not recursed into
    switch (node->type) {
    case GUMBO_NODE_TEXT:
    case GUMBO_NODE_WHITESPACE:
        return escape(QString::fromUtf8(node->v.text.text), false);
    case GUMBO_NODE_ELEMENT:
    case GUMBO_NODE_TEMPLATE:
        break;
    default:
        return {}; // comments and CDATA
    }
    if (!isHTMLElement(node))
        return {}; // svg and math, with everything inside
    const GumboElement &element = node->v.element;
    const QString tag = tagName(element);
    if (dropsContent(tag))
        return {};
    QString children;
    for (unsigned i = 0; i < element.children.length; ++i)
        children += serialise(static_cast<const GumboNode *>(element.children.data[i]), depth + 1);
    if (!allowedTags().contains(tag))
        return children; // unwrapped
    if (isVoid(tag))
        return startTag(element, tag);
    return startTag(element, tag) + children + QLatin1String("</") + tag + QLatin1Char('>');
}

const GumboNode *firstElement(const GumboNode *root)
{
    const GumboVector &children = root->v.element.children;
    for (unsigned i = 0; i < children.length; ++i) {
        const auto *child = static_cast<const GumboNode *>(children.data[i]);
        if (child->type == GUMBO_NODE_ELEMENT)
            return child;
    }
    return nullptr;
}

} // namespace

QString sanitize(const QString &raw, bool singleTag)
{
    const QString text = raw.trimmed();
    if (singleTag && text.startsWith(QLatin1String("</"))) {
        static const QRegularExpression end(QStringLiteral("^</\\s*([A-Za-z][A-Za-z0-9]*)\\s*>$"));
        const QRegularExpressionMatch match = end.match(text);
        const QString tag = match.hasMatch() ? match.captured(1).toLower() : QString();
        return allowedTags().contains(tag) && !isVoid(tag) ? QLatin1String("</") + tag + QLatin1Char('>') : QString();
    }

    const QByteArray bytes = (singleTag ? text : raw).toUtf8();
    GumboOptions options = kGumboDefaultOptions;
    options.fragment_context = GUMBO_TAG_BODY;
    options.fragment_namespace = GUMBO_NAMESPACE_HTML;
    options.max_errors = 0;
    GumboOutput *output = gumbo_parse_with_options(&options, bytes.constData(), size_t(bytes.size()));
    QString out;
    // One tag with nothing after it: the start tag alone, as CommonMark hands it over.
    const bool oneTag = singleTag && text.startsWith(QLatin1Char('<')) && text.indexOf(QLatin1Char('<'), 1) < 0;
    if (oneTag) {
        if (const GumboNode *node = firstElement(output->root); node && isHTMLElement(node)) {
            const QString tag = tagName(node->v.element);
            if (allowedTags().contains(tag))
                out = startTag(node->v.element, tag);
        }
    } else {
        const GumboVector &children = output->root->v.element.children;
        for (unsigned i = 0; i < children.length; ++i)
            out += serialise(static_cast<const GumboNode *>(children.data[i]), 0);
    }
    gumbo_destroy_output(&options, output);
    return out;
}

#else // !NOTEAPPC_HAVE_GUMBO

QString sanitize(const QString &raw, bool)
{
    return escape(raw, false); // no parser to trust: the markup shows as text
}

#endif

} // namespace noteAppC::publish::SafeHtml
