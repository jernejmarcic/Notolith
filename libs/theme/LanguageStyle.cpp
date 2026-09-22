#include "LanguageStyle.h"

#include "CalloutStyle.h"
#include "ConfigFile.h"

namespace noteAppC {

QString LanguageStyles::defaultPath()
{
    return configFilePath(QStringLiteral("codeblocks.ini"));
}

LanguageStyles LanguageStyles::load(const QString &path)
{
    LanguageStyles styles;
    styles.m_sections = readINIsections(path.isEmpty() ? defaultPath() : path);

    // Aliases are indexed once the whole file is read, so a section's
    // `aliases` key may come before or after the section it names.
    for (auto it = styles.m_sections.cbegin(); it != styles.m_sections.cend(); ++it) {
        const QString aliasList = it.value().value(QStringLiteral("aliases"));
        const QStringList aliases = aliasList.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &alias : aliases) {
            const QString trimmed = alias.trimmed().toLower();
            if (!trimmed.isEmpty())
                styles.m_aliases.insert(trimmed, it.key());
        }
    }
    return styles;
}

LanguageStyle LanguageStyles::resolve(const QString &language) const
{
    LanguageStyle style;
    if (language.isEmpty())
        return style; // empty name, no icon, invalid colour: nothing to look up

    style.name = language; // default, kept unless a section overrides it

    const QString lowered = language.toLower();
    // An alias points at the section that carries the real settings; a name
    // that is already a section key is its own alias, so this finds it too.
    const QString key = m_aliases.value(lowered, lowered);
    if (!m_sections.contains(key))
        return style; // unknown language: name as given, no icon, no colour

    // `inherits` carries the parent section's fields; the child's own keys
    // still win. One level only, same as CalloutStyles.
    const QMap<QString, QString> section = m_sections.value(key);
    QMap<QString, QString> merged;
    const QString parent = section.value(QStringLiteral("inherits")).toLower();
    if (!parent.isEmpty() && parent != key)
        merged = m_sections.value(parent);
    for (auto it = section.cbegin(); it != section.cend(); ++it)
        merged.insert(it.key(), it.value());

    const QString name = merged.value(QStringLiteral("name"));
    if (!name.isEmpty())
        style.name = name;
    style.icon = merged.value(QStringLiteral("icon"));
    const QString color = merged.value(QStringLiteral("color"));
    if (!color.isEmpty())
        style.color = CalloutStyle::parseColor(color);

    return style;
}

QStringList LanguageStyles::definedLanguages() const
{
    QStringList out = m_sections.keys();
    out.sort();
    return out;
}

} // namespace noteAppC
