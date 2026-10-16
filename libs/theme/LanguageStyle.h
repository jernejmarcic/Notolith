#pragma once

#include <QColor>
#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

namespace noteAppC {

/// What one syntax token looks like: a colour and up to three flags. No size
/// or font - a code block's monospace metrics stay put whatever its colours do.
struct TokenStyle {
    QColor color; ///< invalid: keep the highlighter's own colour
    bool bold = false;
    bool italic = false;
    bool underline = false;

    bool operator==(const TokenStyle &) const = default;

    /// `#RRGGBBAA [bold] [italic] [underline]`, the words in any order and the
    /// colour optional. Nullopt for an empty value or a word it does not know,
    /// so a typo drops that one rule instead of colouring something at random.
    static std::optional<TokenStyle> parse(const QString &value);
};

/// Dotted capture name (`function.builtin`) -> style, as the `syntax.*` keys
/// of codeblocks.ini, a fence and a theme's `syntax` object spell it.
using SyntaxStyles = QMap<QString, TokenStyle>;

/// How a fenced code block's header shows its language: the label, an
/// optional icon and a tint for both.
///
/// Deliberately the same shape as CalloutStyle - a name, an icon, a colour,
/// configured once in a file and looked up by key - because a code block's
/// header and a callout's are the same kind of problem.
struct LanguageStyle {
    /// The label a code block's header shows. Default: the language exactly
    /// as written after ``` , so an unconfigured language still reads fine.
    QString name;
    /// Any form Icons::icon() takes. Empty means no icon - simplicity first,
    /// and the default, since most languages will never get one configured.
    QString icon;
    /// Label and icon tint. Invalid means the caller falls back to the
    /// theme's --text-muted.
    QColor color;

    // The colour knobs: panel fill, plain text, border and token colours.
    // All invalid/empty by default, which is what keeps an unconfigured block
    // drawing exactly as it did before they existed.
    QColor bg;           ///< panel fill; invalid: the theme's code background
    QColor fg;           ///< plain code text; invalid: --text-normal
    QColor border;       ///< 1 px panel border; invalid: none
    SyntaxStyles syntax; ///< `syntax.<capture>` entries, see CodeHighlighter's SyntaxMap

    /// Applies `bg`, `fg`, `border` and `syntax.*` keys (lowercase, as the ini
    /// reader and the fence parser both give them) over what is already set.
    /// Unknown keys are ignored, like CalloutStyle::apply.
    void applyColours(const QMap<QString, QString> &keys);
    bool hasColours() const { return bg.isValid() || fg.isValid() || border.isValid() || !syntax.isEmpty(); }
};

/// `~/.config/noteAppC/codeblocks.ini`: one section per language, reachable
/// through other info-string spellings via `aliases`.
///
///     [python]
///     name = Python
///     icon = U+E73C          ; or a Lucide id, a file path, an https URL
///     color = #3776ABff      ; CSS order, alpha last
///     aliases = py, python3  ; other info-string spellings that mean this section
///     inherits = other       ; optional, one level, like callouts
///     bg = #1E2A3Aff         ; panel fill, plain text and 1 px border
///     fg = #D8DEE9ff
///     border = #3776AB80
///     syntax.keyword = #C678DD bold   ; any capture name, colour + bold/italic/underline
///
/// Loading a file that is not there is not an error; it is the normal case -
/// nobody has to create this file for code blocks to keep working.
class LanguageStyles {
public:
    /// `defaultPath()` when `path` is empty.
    static LanguageStyles load(const QString &path = {});
    /// `$XDG_CONFIG_HOME` or `~/.config`, then `noteAppC/codeblocks.ini`.
    static QString defaultPath();

    /// Case-insensitive, and reached through any of a section's `aliases`
    /// too. An unknown language resolves to its name exactly as given, no
    /// icon, invalid colour. An empty language resolves to an empty name.
    LanguageStyle resolve(const QString &language) const;

    /// Section names the file defines, sorted. For the settings page.
    QStringList definedLanguages() const;

private:
    QHash<QString, QMap<QString, QString>> m_sections;
    QHash<QString, QString> m_aliases; // alias (lowercase) -> canonical section name
};

} // namespace noteAppC
