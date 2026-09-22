#include "CalloutStyle.h"

#include "Theme.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

namespace noteAppC {

namespace {

/// `bold italic underline strikethrough`, in any order and any separator that
/// is not a word character. `normal` clears what an earlier layer set.
void applyEffects(const QString &value, QFont::Weight *weight, bool *italic, bool *underline,
                  bool *strikeOut)
{
    *weight = QFont::Normal;
    *italic = *underline = *strikeOut = false;
    const QStringList words = value.toLower().split(QRegularExpression(QStringLiteral("[^a-z]+")),
                                                    Qt::SkipEmptyParts);
    for (const QString &word : words) {
        if (word == QLatin1String("bold"))
            *weight = QFont::Bold;
        else if (word == QLatin1String("semibold") || word == QLatin1String("demibold"))
            *weight = QFont::DemiBold;
        else if (word == QLatin1String("light"))
            *weight = QFont::Light;
        else if (word == QLatin1String("italic") || word == QLatin1String("oblique"))
            *italic = true;
        else if (word == QLatin1String("underline"))
            *underline = true;
        else if (word == QLatin1String("strikethrough") || word == QLatin1String("strike"))
            *strikeOut = true;
    }
}

QFont fontFrom(const QFont &base, const QString &family, int size, QFont::Weight weight,
               bool italic, bool underline, bool strikeOut)
{
    QFont font = base;
    if (!family.isEmpty())
        font.setFamily(Theme::firstAvailableFamily(family.split(QLatin1Char(','))));
    if (size > 0)
        font.setPixelSize(size);
    font.setWeight(weight);
    font.setItalic(italic);
    font.setUnderline(underline);
    font.setStrikeOut(strikeOut);
    return font;
}

} // namespace

QColor CalloutStyle::parseColor(const QString &value)
{
    const QString text = value.trimmed();
    // Qt reads `#` plus eight hex digits as #AARRGGBB. CSS, and so this file,
    // puts the alpha last. Convert rather than hand it over.
    if (text.size() == 9 && text.startsWith(QLatin1Char('#'))) {
        bool ok = false;
        const uint rgba = text.mid(1).toUInt(&ok, 16);
        if (ok)
            return QColor::fromRgb((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF,
                                   rgba & 0xFF);
    }
    return Theme::parseColor(text);
}

void CalloutStyle::apply(const QMap<QString, QString> &attributes)
{
    for (auto it = attributes.cbegin(); it != attributes.cend(); ++it) {
        const QString key = it.key().toLower();
        const QString value = it.value().trimmed();
        if (value.isEmpty())
            continue;

        if (key == QLatin1String("icon"))
            icon = value;
        else if (key == QLatin1String("bar") || key == QLatin1String("color")
                 || key == QLatin1String("accent"))
            bar = parseColor(value);
        else if (key == QLatin1String("bg") || key == QLatin1String("background"))
            background = parseColor(value);
        else if (key == QLatin1String("font"))
            font = value;
        else if (key == QLatin1String("size"))
            size = value.toInt();
        else if (key == QLatin1String("text-color") || key == QLatin1String("fg"))
            color = parseColor(value);
        else if (key == QLatin1String("style"))
            applyEffects(value, &weight, &italic, &underline, &strikeOut);
        else if (key == QLatin1String("heading-font"))
            headingFont = value;
        else if (key == QLatin1String("heading-size"))
            headingSize = value.toInt();
        else if (key == QLatin1String("heading-color"))
            headingColor = parseColor(value);
        else if (key == QLatin1String("heading-style"))
            applyEffects(value, &headingWeight, &headingItalic, &headingUnderline,
                         &headingStrikeOut);
    }
}

QFont CalloutStyle::bodyFont(const Theme &theme) const
{
    return fontFrom(theme.textFont(), font, size, weight, italic, underline, strikeOut);
}

QFont CalloutStyle::titleFont(const Theme &theme) const
{
    QFont base = theme.textFont();
    return fontFrom(base, headingFont, headingSize, headingWeight, headingItalic, headingUnderline,
                    headingStrikeOut);
}

QString CalloutStyles::defaultPath()
{
    // The same XDG rule Settings uses, spelled out here so libs/theme does not
    // have to depend on libs/settings for one path.
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    const QString base = xdg.isEmpty() ? QDir::homePath() + QStringLiteral("/.config")
                                       : QString::fromLocal8Bit(xdg);
    return QDir(base).filePath(QStringLiteral("noteAppC/callouts.ini"));
}

CalloutStyles CalloutStyles::load(const QString &path)
{
    CalloutStyles styles;
    QFile file(path.isEmpty() ? defaultPath() : path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return styles; // no file is the normal case, not a failure

    QString section;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';')))
            continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            section = line.mid(1, line.size() - 2).trimmed().toLower();
            continue;
        }
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals < 0 || section.isEmpty())
            continue;
        QString value = line.mid(equals + 1).trimmed();
        // A trailing `; comment`, but not a `;` inside quotes or a colour.
        const qsizetype comment = value.indexOf(QLatin1String(" ;"));
        if (comment >= 0)
            value = value.left(comment).trimmed();
        if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
            value = value.mid(1, value.size() - 2);
        styles.m_sections[section].insert(line.left(equals).trimmed().toLower(), value);
    }
    return styles;
}

CalloutStyle CalloutStyles::resolve(const QString &type, const QMap<QString, QString> &attributes,
                                    const Theme &theme) const
{
    CalloutStyle style;
    const QString key = type.toLower();

    // Layer one: what the built-in type is, which is all Obsidian has.
    style.icon = Theme::calloutIcon(key);
    style.bar = theme.calloutColor(key);

    // Layer two: the file. An `inherits` key lets one section build on
    // another, which is the only thing that makes a family of types bearable.
    const QMap<QString, QString> section = m_sections.value(key);
    const QString parent = section.value(QStringLiteral("inherits")).toLower();
    if (!parent.isEmpty() && parent != key)
        style.apply(m_sections.value(parent));
    style.apply(section);

    // Layer three: the callout itself.
    style.apply(attributes);

    if (!style.background.isValid()) {
        QColor tint = style.bar;
        tint.setAlpha(26); // Obsidian's own callout fill, 10%
        style.background = tint;
    }
    if (!style.headingColor.isValid())
        style.headingColor = style.bar;
    return style;
}

QStringList CalloutStyles::definedTypes() const
{
    QStringList out = m_sections.keys();
    out.sort();
    return out;
}

} // namespace noteAppC
