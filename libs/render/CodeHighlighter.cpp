#include "CodeHighlighter.h"

#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>
#endif

#include <optional>

namespace noteAppC {

namespace {

#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING

/// Collects formats instead of painting, which is what lets the result be
/// appended to a QTextLayout the editor already built.
class Collector : public KSyntaxHighlighting::AbstractHighlighter {
public:
    /// Highlights `text` as lines continuing from `state`, and returns the
    /// state the last line ended in.
    KSyntaxHighlighting::State run(const QString &text, KSyntaxHighlighting::State state)
    {
        formats.clear();
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
        auto cached = m_formats.find(format.id());
        if (cached == m_formats.end())
            cached = m_formats.insert(format.id(), charFormatOf(format));
        if (!cached->has_value())
            return; // the default text style: nothing to add

        QTextLayout::FormatRange range;
        range.start = int(m_base) + offset;
        range.length = length;
        range.format = **cached;
        formats.append(range);
    }

private:
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
    QHash<int, std::optional<QTextCharFormat>> m_formats; // by Format::id()
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

struct CodeHighlighter::Private {
#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
    KSyntaxHighlighting::Repository repository;
    KSyntaxHighlighting::Theme theme;
    mutable Collector collector;
#endif
};

CodeHighlighter::CodeHighlighter()
    : d(std::make_unique<Private>())
{
#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
    d->theme = d->repository.defaultTheme(KSyntaxHighlighting::Repository::DarkTheme);
    d->collector.setTheme(d->theme);
#endif
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
    d->theme = d->repository.defaultTheme(theme.isDark()
                                              ? KSyntaxHighlighting::Repository::DarkTheme
                                              : KSyntaxHighlighting::Repository::LightTheme);
    d->collector.setTheme(d->theme);
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
    return d->repository.definitionForName(kateNameFor(language)).isValid();
#else
    Q_UNUSED(language);
    return false;
#endif
}

QList<QTextLayout::FormatRange> CodeHighlighter::highlight(const QString &text,
                                                           const QString &language) const
{
    State state;
    return highlight(text, language, &state);
}

QList<QTextLayout::FormatRange> CodeHighlighter::highlight(const QString &text,
                                                           const QString &language,
                                                           State *state) const
{
#ifdef NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING
    if (text.isEmpty())
        return {};

    auto definition = d->repository.definitionForName(kateNameFor(language));
    if (!definition.isValid()) {
        // Fall back on the file-name matcher: "```CMakeLists.txt" and the
        // like still highlight.
        definition = d->repository.definitionForFileName(language);
    }
    if (!definition.isValid())
        return {};

    if (d->collector.definition() != definition)
        d->collector.setDefinition(definition);
    const KSyntaxHighlighting::State start = state->d ? state->d->state : KSyntaxHighlighting::State();
    auto end = std::make_shared<State::Data>();
    end->state = d->collector.run(text, start);
    state->d = std::move(end);
    return d->collector.formats;
#else
    Q_UNUSED(text);
    Q_UNUSED(language);
    Q_UNUSED(state);
    return {};
#endif
}

} // namespace noteAppC
