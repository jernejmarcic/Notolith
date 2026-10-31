#include "CalloutStyle.h"

#include "ConfigFile.h"
#include "Theme.h"

#include <QRegularExpression>

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
        else if (key == QLatin1String("bar") || key == QLatin1String("accent"))
            bar = parseColor(value);
        else if (key == QLatin1String("bg") || key == QLatin1String("background"))
            background = parseColor(value);
        else if (key == QLatin1String("font"))
            font = value;
        else if (key == QLatin1String("size"))
            size = value.toInt();
        // `color` is the body text, by the format's design. It once also meant the rail, which painted a callout's rail
        // in the colour meant for its words.
        else if (key == QLatin1String("color") || key == QLatin1String("text-color")
                 || key == QLatin1String("fg"))
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
    return configFilePath(QStringLiteral("callouts.ini"));
}

CalloutStyles CalloutStyles::load(const QString &path)
{
    CalloutStyles styles;
    styles.m_sections = readINIsections(path.isEmpty() ? defaultPath() : path);
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

QStringList CalloutStyles::knownTypes() const
{
    QStringList out = {QStringLiteral("note"),     QStringLiteral("abstract"), QStringLiteral("info"),
                       QStringLiteral("todo"),     QStringLiteral("tip"),      QStringLiteral("success"),
                       QStringLiteral("question"), QStringLiteral("warning"),  QStringLiteral("failure"),
                       QStringLiteral("danger"),   QStringLiteral("bug"),      QStringLiteral("example"),
                       QStringLiteral("quote")};
    for (const QString &type : definedTypes()) {
        if (!out.contains(type))
            out.append(type);
    }
    return out;
}

} // namespace noteAppC
