#include "CodeHighlighter.h"

#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>

namespace noteAppC {

namespace {

/// Collects formats instead of painting, which is what lets the result be
/// appended to a QTextLayout the editor already built.
class Collector : public KSyntaxHighlighting::AbstractHighlighter {
public:
    void run(const QString &text)
    {
        formats.clear();
        m_base = 0;

        KSyntaxHighlighting::State state;
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
    }

    QList<QTextLayout::FormatRange> formats;

protected:
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format) override
    {
        if (length <= 0 || format.isDefaultTextStyle(theme()))
            return;

        QTextCharFormat charFormat;
        if (format.hasTextColor(theme()))
            charFormat.setForeground(format.textColor(theme()));
        if (format.hasBackgroundColor(theme()))
            charFormat.setBackground(format.backgroundColor(theme()));
        if (format.isBold(theme()))
            charFormat.setFontWeight(QFont::Bold);
        if (format.isItalic(theme()))
            charFormat.setFontItalic(true);
        if (format.isUnderline(theme()))
            charFormat.setFontUnderline(true);
        if (format.isStrikeThrough(theme()))
            charFormat.setFontStrikeOut(true);

        QTextLayout::FormatRange range;
        range.start = int(m_base) + offset;
        range.length = length;
        range.format = charFormat;
        formats.append(range);
    }

private:
    qsizetype m_base = 0;
};

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
    KSyntaxHighlighting::Repository repository;
    KSyntaxHighlighting::Theme theme;
    mutable Collector collector;
};

CodeHighlighter::CodeHighlighter()
    : d(std::make_unique<Private>())
{
    d->theme = d->repository.defaultTheme(KSyntaxHighlighting::Repository::DarkTheme);
    d->collector.setTheme(d->theme);
}

CodeHighlighter::~CodeHighlighter() = default;

void CodeHighlighter::setTheme(const Theme &theme)
{
    d->theme = d->repository.defaultTheme(theme.isDark()
                                              ? KSyntaxHighlighting::Repository::DarkTheme
                                              : KSyntaxHighlighting::Repository::LightTheme);
    d->collector.setTheme(d->theme);
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
    if (language.isEmpty())
        return false;
    return d->repository.definitionForName(kateNameFor(language)).isValid();
}

QList<QTextLayout::FormatRange> CodeHighlighter::highlight(const QString &text,
                                                           const QString &language) const
{
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

    d->collector.setDefinition(definition);
    d->collector.run(text);
    return d->collector.formats;
}

} // namespace noteAppC
