#pragma once

#include <QColor>
#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>

namespace noteAppC {

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
