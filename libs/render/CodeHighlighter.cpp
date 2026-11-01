// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "CodeHighlighter.h"

#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>
#endif

#include <QtGlobal>

#include <optional>

namespace noteAppC {

namespace {

/// Where the capture name rides on a mapped format; see SyntaxMap::captureOf.
constexpr int kCaptureProperty = QTextFormat::UserProperty + 41;

} // namespace

SyntaxMap::SyntaxMap(QList<SyntaxStyles> layers)
{
    quint64 hash = 1469598103934665603ULL; // FNV-1a over every key and style
    const auto mix = [&hash](quint64 value) { hash = (hash ^ value) * 1099511628211ULL; };
    bool any = false;
    for (const SyntaxStyles &layer : layers) {
        if (layer.isEmpty())
            continue; // an empty layer changes nothing, so it must not change the signature
        any = true;
        m_layers.append(layer);
        mix(0xFFFF);
        for (auto it = layer.cbegin(); it != layer.cend(); ++it) {
            mix(qHash(it.key(), 0));
            mix(it.value().color.isValid() ? quint64(it.value().color.rgba()) + 1 : 0);
            mix(quint64(it.value().bold) | quint64(it.value().italic) << 1 | quint64(it.value().underline) << 2);
        }
    }
    m_signature = any ? (hash | 1) : 0;
}

SyntaxMap SyntaxMap::tagging()
{
    SyntaxMap map;
    map.m_signature = 2; // distinct from any hash|1 and from "empty"
    return map;
}

const TokenStyle *SyntaxMap::resolve(const QString &capture, QString *matched) const
{
    for (const SyntaxStyles &layer : m_layers) {
        // Longest dotted prefix: drop one trailing segment at a time. The cut
        // is always on a dot, so `functionx` never reads as `function`.
        for (QStringView key(capture); !key.isEmpty();) {
            if (const auto it = layer.constFind(key.toString()); it != layer.cend()) {
                if (matched)
                    *matched = it.key();
                return &it.value();
            }
            const qsizetype dot = key.lastIndexOf(QLatin1Char('.'));
            key = dot < 0 ? QStringView() : key.left(dot);
        }
    }
    return nullptr;
}

QString SyntaxMap::captureOf(const QTextCharFormat &format)
{
    return format.property(kCaptureProperty).toString();
}

namespace {

#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING

/// Collects formats instead of painting, which is what lets the result be
/// appended to a QTextLayout the editor already built.
class Collector : public KSyntaxHighlighting::AbstractHighlighter {
public:
    /// Highlights `text` as lines continuing from `state`, and returns the
    /// state the last line ended in.
    KSyntaxHighlighting::State run(const QString &text, KSyntaxHighlighting::State state,
                                   const SyntaxMap *map)
    {
        formats.clear();
        m_map = map && !map->isEmpty() ? map : nullptr;
        m_base = 0;

        qsizetype from = 0;
        while (from <= text.size()) {
            qsizetype breakAt = text.indexOf(QLatin1Char('\n'), from);
            if (breakAt < 0)
                breakAt = text.size();
            m_base = from;
            state = highlightLine(QStringView(text).mid(from, breakAt - from), state);
            if (breakAt == text.size())
                break;
            from = breakAt + 1;
        }
        return state;
    }

    void setTheme(const KSyntaxHighlighting::Theme &theme) override
    {
        AbstractHighlighter::setTheme(theme);
        m_theme = theme;
        m_formats.clear();
        m_mapped.clear();
    }

    QList<QTextLayout::FormatRange> formats;

protected:
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format) override
    {
        if (length <= 0)
            return;

        // One QTextCharFormat per Kate format, built once. theme() returns a
        // copy, and asking it up to seven times per token was most of what
        // highlighting cost.
        //
        // A map gets a cache of its own, keyed by its signature: ids are
        // unique per Repository, so (map, id) is all that tells two runs
        // apart, and a run without a map still reads m_formats exactly as
        // before. Both go when the theme changes.
        auto &cache = m_map ? mappedCache() : m_formats;
        auto cached = cache.find(format.id());
        if (cached == cache.end())
            cached = cache.insert(format.id(), m_map ? mappedFormatOf(format) : charFormatOf(format));
        if (!cached->has_value())
            return; // the default text style: nothing to add

        QTextLayout::FormatRange range;
        range.start = int(m_base) + offset;
        range.length = length;
        range.format = **cached;
        formats.append(range);
    }

private:
    using FormatCache = QHash<int, std::optional<QTextCharFormat>>;

    FormatCache &mappedCache()
    {
        if (m_mapped.size() > 32)
            m_mapped.clear(); // a few dozen distinct styles at most are ever live
        return m_mapped[m_map->signature()];
    }

    /// charFormatOf() with the map's style laid over it. Plain text has no
    /// capture and stays untouched; a token the map does not name keeps the
    /// highlighter's colour but still carries its capture name.
    std::optional<QTextCharFormat> mappedFormatOf(const KSyntaxHighlighting::Format &format) const
    {
        const QString capture = SyntaxMap::captureOf(int(format.textStyle()));
        if (capture.isEmpty())
            return charFormatOf(format);
        QTextCharFormat charFormat = charFormatOf(format).value_or(QTextCharFormat());
        if (const TokenStyle *style = m_map->resolve(capture)) {
            if (style->color.isValid())
                charFormat.setForeground(style->color);
            if (style->bold)
                charFormat.setFontWeight(QFont::Bold);
            if (style->italic)
                charFormat.setFontItalic(true);
            if (style->underline)
                charFormat.setFontUnderline(true);
        }
        charFormat.setProperty(kCaptureProperty, capture);
        return charFormat;
    }

    std::optional<QTextCharFormat> charFormatOf(const KSyntaxHighlighting::Format &format) const
    {
        const KSyntaxHighlighting::Theme &theme = m_theme;
        if (format.isDefaultTextStyle(theme))
            return std::nullopt;

        QTextCharFormat charFormat;
        if (format.hasTextColor(theme))
            charFormat.setForeground(format.textColor(theme));
        if (format.hasBackgroundColor(theme))
            charFormat.setBackground(format.backgroundColor(theme));
        if (format.isBold(theme))
            charFormat.setFontWeight(QFont::Bold);
        if (format.isItalic(theme))
            charFormat.setFontItalic(true);
        if (format.isUnderline(theme))
            charFormat.setFontUnderline(true);
        if (format.isStrikeThrough(theme))
            charFormat.setFontStrikeOut(true);
        return charFormat;
    }

    qsizetype m_base = 0;
    KSyntaxHighlighting::Theme m_theme;
    FormatCache m_formats;                       // by Format::id()
    QHash<quint64, FormatCache> m_mapped;        // by SyntaxMap::signature(), then Format::id()
    const SyntaxMap *m_map = nullptr;            // the run's, null for none
};

#endif // NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING

/// Ids Obsidian and Prism use that Kate spells differently.
const QHash<QString, QString> &aliases()
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("c++"), QStringLiteral("C++")},
        {QStringLiteral("cpp"), QStringLiteral("C++")},
        {QStringLiteral("cxx"), QStringLiteral("C++")},
        {QStringLiteral("c"), QStringLiteral("C")},
        {QStringLiteral("cs"), QStringLiteral("C#")},
        {QStringLiteral("csharp"), QStringLiteral("C#")},
        {QStringLiteral("js"), QStringLiteral("JavaScript")},
        {QStringLiteral("jsx"), QStringLiteral("JavaScript React (JSX)")},
        {QStringLiteral("ts"), QStringLiteral("TypeScript")},
        {QStringLiteral("tsx"), QStringLiteral("TypeScript React (TSX)")},
        {QStringLiteral("py"), QStringLiteral("Python")},
        {QStringLiteral("rs"), QStringLiteral("Rust")},
        {QStringLiteral("sh"), QStringLiteral("Bash")},
        {QStringLiteral("shell"), QStringLiteral("Bash")},
        {QStringLiteral("zsh"), QStringLiteral("Bash")},
        {QStringLiteral("console"), QStringLiteral("Bash")},
        {QStringLiteral("yml"), QStringLiteral("YAML")},
        {QStringLiteral("md"), QStringLiteral("Markdown")},
        {QStringLiteral("tex"), QStringLiteral("LaTeX")},
        {QStringLiteral("html"), QStringLiteral("HTML")},
        {QStringLiteral("vim"), QStringLiteral("Vim Help File")},
        {QStringLiteral("dockerfile"), QStringLiteral("Dockerfile")},
        {QStringLiteral("make"), QStringLiteral("Makefile")},
        {QStringLiteral("plaintext"), QStringLiteral("None")},
        {QStringLiteral("text"), QStringLiteral("None")},
    };
    return map;
}

} // namespace

QString SyntaxMap::captureOf(int kateTextStyle)
{
#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
    using Style = KSyntaxHighlighting::Theme::TextStyle;
    switch (Style(kateTextStyle)) {
    case Style::Normal:         break;
    case Style::Keyword:        return QStringLiteral("keyword");
    case Style::ControlFlow:    return QStringLiteral("keyword.control");
    case Style::Operator:       return QStringLiteral("operator");
    case Style::BuiltIn:        return QStringLiteral("function.builtin");
    case Style::Extension:      return QStringLiteral("function.special");
    case Style::Preprocessor:   return QStringLiteral("preproc");
    case Style::Attribute:
    case Style::Annotation:     return QStringLiteral("attribute");
    case Style::Function:       return QStringLiteral("function");
    case Style::Variable:       return QStringLiteral("variable");
    case Style::DataType:       return QStringLiteral("type");
    case Style::Char:
    case Style::SpecialChar:    return QStringLiteral("string.special");
    case Style::String:
    case Style::VerbatimString: return QStringLiteral("string");
    case Style::SpecialString:  return QStringLiteral("string.regex");
    case Style::Import:         return QStringLiteral("namespace");
    case Style::DecVal:
    case Style::BaseN:
    case Style::Float:          return QStringLiteral("number");
    case Style::Constant:       return QStringLiteral("constant");
    case Style::Comment:
    case Style::CommentVar:     return QStringLiteral("comment");
    case Style::Documentation:  return QStringLiteral("comment.doc");
    case Style::RegionMarker:   return QStringLiteral("label");
    case Style::Information:    return QStringLiteral("hint");
    case Style::Warning:        return QStringLiteral("warning");
    case Style::Alert:
    case Style::Error:          return QStringLiteral("error");
    case Style::Others:         return QStringLiteral("primary");
    }
#else
    Q_UNUSED(kateTextStyle);
#endif
    return {};
}

struct CodeHighlighter::Private {
#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
    /// Made on first use: reading every syntax definition's metadata is
    /// 2.3 ms, and every editor has a highlighter - a new tab's, a note
    /// embed's - whether or not its note has a line of code.
    std::unique_ptr<KSyntaxHighlighting::Repository> repository;
    bool dark = true;
    mutable Collector collector;

    KSyntaxHighlighting::Repository &repo()
    {
        if (!repository) {
            repository = std::make_unique<KSyntaxHighlighting::Repository>();
            applyTheme();
        }
        return *repository;
    }
    void applyTheme()
    {
        collector.setTheme(repository->defaultTheme(dark ? KSyntaxHighlighting::Repository::DarkTheme
                                                         : KSyntaxHighlighting::Repository::LightTheme));
    }
#endif
};

CodeHighlighter::CodeHighlighter()
    : d(std::make_unique<Private>())
{
}

CodeHighlighter::~CodeHighlighter() = default;

struct CodeHighlighter::State::Data {
#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
    KSyntaxHighlighting::State state;
#endif
};

CodeHighlighter::State::State() = default;
CodeHighlighter::State::~State() = default;
CodeHighlighter::State::State(const State &other) = default;
CodeHighlighter::State &CodeHighlighter::State::operator=(const State &other) = default;

bool CodeHighlighter::State::operator==(const State &other) const
{
#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
    const KSyntaxHighlighting::State none;
    return (d ? d->state : none) == (other.d ? other.d->state : none);
#else
    Q_UNUSED(other);
    return true;
#endif
}

void CodeHighlighter::setTheme(const Theme &theme)
{
#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
    d->dark = theme.isDark();
    if (d->repository)
        d->applyTheme();
#else
    Q_UNUSED(theme);
#endif
}

bool CodeHighlighter::isAvailable()
{
#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
    return true;
#else
    return false;
#endif
}

QString CodeHighlighter::kateNameFor(const QString &infoString)
{
    // Obsidian takes the first word of the info string as the language.
    const QString first = infoString.trimmed().section(QLatin1Char(' '), 0, 0);
    const QString lowered = first.toLower();
    return aliases().value(lowered, first);
}

bool CodeHighlighter::knows(const QString &language) const
{
#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
    if (language.isEmpty())
        return false;
    return d->repo().definitionForName(kateNameFor(language)).isValid();
#else
    Q_UNUSED(language);
    return false;
#endif
}

QList<QTextLayout::FormatRange> CodeHighlighter::highlight(const QString &text,
                                                           const QString &language,
                                                           const SyntaxMap *map) const
{
    State state;
    return highlight(text, language, &state, map);
}

QList<QTextLayout::FormatRange> CodeHighlighter::highlight(const QString &text,
                                                           const QString &language,
                                                           State *state, const SyntaxMap *map) const
{
#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
    if (text.isEmpty())
        return {};

    auto definition = d->repo().definitionForName(kateNameFor(language));
    if (!definition.isValid()) {
        // Fall back on the file-name matcher: "```CMakeLists.txt" and the
        // like still highlight.
        definition = d->repo().definitionForFileName(language);
    }
    if (!definition.isValid())
        return {};

    if (d->collector.definition() != definition)
        d->collector.setDefinition(definition);
    const KSyntaxHighlighting::State start = state->d ? state->d->state : KSyntaxHighlighting::State();
    auto end = std::make_shared<State::Data>();
    end->state = d->collector.run(text, start, map);
    state->d = std::move(end);
    return d->collector.formats;
#else
    Q_UNUSED(text);
    Q_UNUSED(language);
    Q_UNUSED(state);
    Q_UNUSED(map);
    return {};
#endif
}

} // namespace noteAppC
