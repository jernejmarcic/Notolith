#include "Theme.h"

#include <QFontDatabase>
#include <QRegularExpression>
#include <QStringList>

#include <cmath>

namespace noteAppC {

namespace {

/// Obsidian's base scale, read out of the installed app's stylesheet
/// (`.theme-dark` / `.theme-light` in `obsidian.asar`, 1.13.7).
struct BaseScale {
    const char *base00;
    const char *base05;
    const char *base10;
    const char *base20;
    const char *base25;
    const char *base30;
    const char *base35;
    const char *base40;
    const char *base50;
    const char *base60;
    const char *base70;
    const char *base100;
    const char *red;
    const char *orange;
    const char *yellow;
    const char *green;
    const char *cyan;
    const char *blue;
    const char *purple;
    const char *pink;
};

constexpr BaseScale kDark{
    "#1c1c1c", "#212121", "#232323", "#282828", "#2e2e2e", "#333333",
    "#3f3f3f", "#555555", "#666666", "#999999", "#b3b3b3", "#dadada",
    "#fb464c", "#e9973f", "#e0de71", "#44cf6e", "#53dfdd", "#027aff",
    "#a882ff", "#fa99cd",
};

/// True black, for OLED panels: the backgrounds go to #000000 and the greys
/// above them are pulled down to keep the same contrast steps Obsidian's dark
/// scheme has. Text stays a touch softer than pure white, which is easier to
/// read against black.
constexpr BaseScale kOled{
    "#000000", "#040404", "#070707", "#0c0c0c", "#121212", "#1a1a1a",
    "#262626", "#3a3a3a", "#4d4d4d", "#8a8a8a", "#b8b8b8", "#e4e4e4",
    "#fb464c", "#e9973f", "#e0de71", "#44cf6e", "#53dfdd", "#3b8cff",
    "#a882ff", "#fa99cd",
};

constexpr BaseScale kLight{
    "#ffffff", "#fcfcfc", "#fafafa", "#f6f6f6", "#efefef", "#e4e4e4",
    "#dadada", "#bdbdbd", "#ababab", "#707070", "#5c5c5c", "#222222",
    "#e93147", "#ec7500", "#e0ac00", "#08b94e", "#00bfbc", "#086ddd",
    "#7852ee", "#d53984",
};

/// `color-mix(in oklch, <mono> N%, transparent)` has no QSS equivalent; an
/// rgba() overlay of the same weight is close enough at these small alphas.
QString overlay(bool dark, double alpha)
{
    const int channel = dark ? 255 : 0;
    return QStringLiteral("rgba(%1, %1, %1, %2)").arg(channel).arg(alpha, 0, 'f', 3);
}

QString rgba(const QColor &color, double alpha)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(alpha, 0, 'f', 3);
}

QString hsl(double hue, double saturationPercent, double lightnessPercent)
{
    const QColor color = QColor::fromHslF(std::fmod(hue < 0 ? hue + 360 : hue, 360.0) / 360.0,
                                          qBound(0.0, saturationPercent / 100.0, 1.0),
                                          qBound(0.0, lightnessPercent / 100.0, 1.0));
    return color.name();
}

} // namespace

QString Theme::nameOf(Mode mode)
{
    switch (mode) {
    case Mode::Light:
        return QStringLiteral("light");
    case Mode::Oled:
        return QStringLiteral("oled");
    case Mode::Dark:
        break;
    }
    return QStringLiteral("dark");
}

Theme::Mode Theme::modeFromName(const QString &name)
{
    if (name == QLatin1String("light"))
        return Mode::Light;
    if (name == QLatin1String("oled"))
        return Mode::Oled;
    return Mode::Dark;
}

Theme Theme::builtin(Mode mode)
{
    const BaseScale &scale = mode == Mode::Oled    ? kOled
                             : mode == Mode::Light ? kLight
                                                   : kDark;

    Theme theme;
    theme.m_mode = mode;
    theme.m_tokens = {
        {QStringLiteral("--color-base-00"), QString::fromLatin1(scale.base00)},
        {QStringLiteral("--color-base-05"), QString::fromLatin1(scale.base05)},
        {QStringLiteral("--color-base-10"), QString::fromLatin1(scale.base10)},
        {QStringLiteral("--color-base-20"), QString::fromLatin1(scale.base20)},
        {QStringLiteral("--color-base-25"), QString::fromLatin1(scale.base25)},
        {QStringLiteral("--color-base-30"), QString::fromLatin1(scale.base30)},
        {QStringLiteral("--color-base-35"), QString::fromLatin1(scale.base35)},
        {QStringLiteral("--color-base-40"), QString::fromLatin1(scale.base40)},
        {QStringLiteral("--color-base-50"), QString::fromLatin1(scale.base50)},
        {QStringLiteral("--color-base-60"), QString::fromLatin1(scale.base60)},
        {QStringLiteral("--color-base-70"), QString::fromLatin1(scale.base70)},
        {QStringLiteral("--color-base-100"), QString::fromLatin1(scale.base100)},
        {QStringLiteral("--color-red"), QString::fromLatin1(scale.red)},
        {QStringLiteral("--color-orange"), QString::fromLatin1(scale.orange)},
        {QStringLiteral("--color-yellow"), QString::fromLatin1(scale.yellow)},
        {QStringLiteral("--color-green"), QString::fromLatin1(scale.green)},
        {QStringLiteral("--color-cyan"), QString::fromLatin1(scale.cyan)},
        {QStringLiteral("--color-blue"), QString::fromLatin1(scale.blue)},
        {QStringLiteral("--color-purple"), QString::fromLatin1(scale.purple)},
        {QStringLiteral("--color-pink"), QString::fromLatin1(scale.pink)},
        // Obsidian's default accent.
        {QStringLiteral("--accent-h"), QStringLiteral("258")},
        {QStringLiteral("--accent-s"), QStringLiteral("88")},
        {QStringLiteral("--accent-l"), QStringLiteral("66")},
        // Metrics, same numbers as Obsidian's variables.
        {QStringLiteral("--radius-s"), QStringLiteral("4")},
        {QStringLiteral("--radius-m"), QStringLiteral("8")},
        {QStringLiteral("--radius-l"), QStringLiteral("12")},
        {QStringLiteral("--font-ui-smaller"), QStringLiteral("12")},
        {QStringLiteral("--font-ui-small"), QStringLiteral("13")},
        {QStringLiteral("--font-ui-medium"), QStringLiteral("15")},
        {QStringLiteral("--font-ui-large"), QStringLiteral("20")},
        {QStringLiteral("--font-text-size"), QStringLiteral("16")},
        {QStringLiteral("--file-line-width"), QStringLiteral("700")},
        {QStringLiteral("--ribbon-width"), QStringLiteral("44")},
        {QStringLiteral("--input-height"), QStringLiteral("30")},
    };
    theme.applyDerivedTokens();
    return theme;
}

void Theme::applyDerivedTokens()
{
    const bool dark = isDark();
    // A pinned token came from a scheme or the user and outranks the scale.
    const auto derive = [this](const QString &token, const QString &value) {
        if (!m_pinned.contains(token))
            m_tokens[token] = value;
    };
    const auto base = [this](const char *name) {
        return m_tokens.value(QLatin1String("--color-base-") + QLatin1String(name));
    };

    const double h = value(QStringLiteral("--accent-h")).toDouble();
    const double s = value(QStringLiteral("--accent-s")).toDouble();
    const double l = value(QStringLiteral("--accent-l")).toDouble();

    // Obsidian: --color-accent-1/-2 shift hue down and lift lightness, by
    // different amounts per mode.
    const QString accent = hsl(h, s, l);
    const QString accent1 = dark ? hsl(h - 3, s * 1.02, l * 1.15) : hsl(h - 1, s * 1.01, l * 1.075);
    const QString accent2 = dark ? hsl(h - 5, s * 1.05, l * 1.29) : hsl(h - 3, s * 1.02, l * 1.15);

    derive(QStringLiteral("--color-accent"), accent);
    derive(QStringLiteral("--color-accent-1"), accent1);
    derive(QStringLiteral("--color-accent-2"), accent2);

    derive(QStringLiteral("--background-primary"), base("00"));
    derive(QStringLiteral("--background-primary-alt"), base("10"));
    derive(QStringLiteral("--background-secondary"), base("20"));
    derive(QStringLiteral("--background-secondary-alt"), dark ? base("30") : base("05"));
    derive(QStringLiteral("--background-modifier-border"), base("30"));
    derive(QStringLiteral("--background-modifier-border-hover"), base("35"));
    derive(QStringLiteral("--background-modifier-border-focus"), base("40"));
    derive(QStringLiteral("--background-modifier-form-field"), dark ? base("25") : base("00"));
    derive(QStringLiteral("--background-modifier-hover"), overlay(dark, 0.067));
    derive(QStringLiteral("--background-modifier-active"), overlay(dark, 0.10));

    derive(QStringLiteral("--text-normal"), base("100"));
    derive(QStringLiteral("--text-muted"), base("70"));
    derive(QStringLiteral("--text-faint"), base("50"));
    derive(QStringLiteral("--text-on-accent"), QStringLiteral("#ffffff"));
    derive(QStringLiteral("--text-error"), m_tokens[QStringLiteral("--color-red")]);
    derive(QStringLiteral("--text-accent"), dark ? accent1 : accent);
    // Obsidian dims an unresolved link rather than giving it its own hue.
    derive(QStringLiteral("--link-unresolved-color"),
           rgba(QColor(dark ? accent1 : accent), 0.45));
    derive(QStringLiteral("--text-selection"), rgba(QColor(accent), dark ? 0.33 : 0.20));

    derive(QStringLiteral("--interactive-normal"), dark ? base("30") : base("00"));
    derive(QStringLiteral("--interactive-hover"), dark ? base("35") : base("10"));
    derive(QStringLiteral("--interactive-accent"), dark ? accent : accent1);
    derive(QStringLiteral("--interactive-accent-hover"), dark ? accent1 : accent2);

    derive(QStringLiteral("--divider-color"), base("30"));
    derive(QStringLiteral("--scrollbar-thumb-bg"), overlay(dark, 0.10));
    derive(QStringLiteral("--scrollbar-active-thumb-bg"), overlay(dark, 0.20));
    derive(QStringLiteral("--nav-item-color"), m_tokens[QStringLiteral("--text-muted")]);
    derive(QStringLiteral("--nav-item-color-active"), m_tokens[QStringLiteral("--text-normal")]);

    // The file explorer, under Obsidian's own names. Sizes are plain numbers
    // (pixels), colours are anything parseColor() takes.
    derive(QStringLiteral("--nav-item-size"), m_tokens[QStringLiteral("--font-ui-small")]);
    derive(QStringLiteral("--nav-item-padding"), QStringLiteral("3"));
    derive(QStringLiteral("--nav-item-weight"), QStringLiteral("400"));
    derive(QStringLiteral("--nav-item-color-hover"), m_tokens[QStringLiteral("--text-normal")]);
    derive(QStringLiteral("--nav-item-background-hover"),
           m_tokens[QStringLiteral("--background-modifier-hover")]);
    derive(QStringLiteral("--nav-item-background-active"),
           m_tokens[QStringLiteral("--background-modifier-active")]);
    derive(QStringLiteral("--nav-item-background-selected"), rgba(QColor(accent), dark ? 0.16 : 0.12));
    derive(QStringLiteral("--nav-collapse-icon-color"), m_tokens[QStringLiteral("--text-faint")]);
    derive(QStringLiteral("--nav-collapse-icon-color-collapsed"),
           m_tokens[QStringLiteral("--text-muted")]);
    derive(QStringLiteral("--nav-indentation-guide-width"), QStringLiteral("1"));
    derive(QStringLiteral("--nav-indentation-guide-color"),
           dark ? QStringLiteral("rgba(255, 255, 255, 0.06)") : QStringLiteral("rgba(0, 0, 0, 0.08)"));
    derive(QStringLiteral("--nav-file-tag-size"), QStringLiteral("11"));
    derive(QStringLiteral("--nav-file-tag-color"), m_tokens[QStringLiteral("--text-muted")]);
    derive(QStringLiteral("--nav-file-tag-background"),
           m_tokens[QStringLiteral("--background-modifier-hover")]);
    derive(QStringLiteral("--nav-heading-color"), m_tokens[QStringLiteral("--text-faint")]);
}

QString Theme::value(const QString &token) const
{
    return m_tokens.value(token);
}

SyntaxStyles Theme::syntaxStyles() const
{
    // Obsidian's code variables, spelled as the capture names they cover.
    static const std::pair<const char *, const char *> aliases[] = {
        {"--code-keyword", "keyword"},     {"--code-string", "string"},
        {"--code-comment", "comment"},     {"--code-function", "function"},
        {"--code-value", "number"},        {"--code-property", "variable"},
        {"--code-operator", "operator"},   {"--code-punctuation", "punctuation"},
        {"--code-tag", "tag"},
    };
    SyntaxStyles out;
    for (const auto &[token, capture] : aliases) {
        const auto it = m_tokens.constFind(QLatin1String(token));
        if (it == m_tokens.cend())
            continue;
        if (const auto style = TokenStyle::parse(it.value()))
            out.insert(QLatin1String(capture), *style);
    }
    static const QString prefix = QStringLiteral("--syntax-");
    for (auto it = m_tokens.constBegin(); it != m_tokens.constEnd(); ++it) {
        if (!it.key().startsWith(prefix))
            continue;
        if (const auto style = TokenStyle::parse(it.value()))
            out.insert(it.key().mid(prefix.size()), *style);
    }
    return out;
}

QColor Theme::parseColor(const QString &text)
{
    const QString value = text.trimmed();
    if (value.isEmpty())
        return {};

    const qsizetype open = value.indexOf(QLatin1Char('('));
    if (open > 0 && value.endsWith(QLatin1Char(')'))) {
        const QString function = value.left(open).trimmed().toLower();
        if (function != QLatin1String("rgb") && function != QLatin1String("rgba")
            && function != QLatin1String("hsl") && function != QLatin1String("hsla"))
            return {}; // color-mix(), oklch(), light-dark(): no Qt equivalent
        // Commas, spaces or a slash before the alpha - CSS allows all three.
        static const QRegularExpression number(QStringLiteral("[-+]?[0-9]*\\.?[0-9]+\\s*%?"));
        QList<double> n;
        bool alphaIsPercent = false;
        auto it = number.globalMatch(value.mid(open + 1, value.size() - open - 2));
        while (it.hasNext()) {
            QString token = it.next().captured().trimmed();
            const bool percent = token.endsWith(QLatin1Char('%'));
            if (percent)
                token.chop(1);
            if (n.size() == 3)
                alphaIsPercent = percent;
            n.append(token.toDouble());
        }
        if (n.size() < 3)
            return {};

        double alpha = n.size() > 3 ? n.at(3) : 1.0;
        if (alphaIsPercent)
            alpha /= 100.0;

        QColor color;
        if (function.startsWith(QLatin1String("rgb"))) {
            color = QColor(int(n.at(0)), int(n.at(1)), int(n.at(2)));
        } else {
            double hue = std::fmod(n.at(0), 360.0);
            if (hue < 0)
                hue += 360.0;
            color = QColor::fromHslF(float(hue / 360.0),
                                     float(qBound(0.0, n.at(1) / 100.0, 1.0)),
                                     float(qBound(0.0, n.at(2) / 100.0, 1.0)));
        }
        color.setAlphaF(float(qBound(0.0, alpha, 1.0)));
        return color;
    }

    if (value.compare(QLatin1String("transparent"), Qt::CaseInsensitive) == 0)
        return QColor(0, 0, 0, 0);

    return QColor::fromString(value);
}

QColor Theme::color(const QString &token) const
{
    return parseColor(value(token));
}

void Theme::setToken(const QString &token, const QString &value)
{
    m_tokens[token] = value;
    m_pinned.insert(token);
    applyDerivedTokens();
}

void Theme::setTokens(const QHash<QString, QString> &tokens)
{
    for (auto it = tokens.constBegin(); it != tokens.constEnd(); ++it) {
        m_tokens[it.key()] = it.value();
        m_pinned.insert(it.key());
    }
    applyDerivedTokens();
}

void Theme::resetToken(const QString &token)
{
    m_pinned.remove(token);
    applyDerivedTokens();
}

QHash<QString, QString> Theme::overrides() const
{
    const Theme base = builtin(m_mode);
    QHash<QString, QString> changed;
    for (auto it = m_tokens.constBegin(); it != m_tokens.constEnd(); ++it) {
        if (base.value(it.key()) != it.value())
            changed.insert(it.key(), it.value());
    }
    return changed;
}

void Theme::setAccent(int hue, int saturationPercent, int lightnessPercent)
{
    m_tokens[QStringLiteral("--accent-h")] = QString::number(hue);
    m_tokens[QStringLiteral("--accent-s")] = QString::number(saturationPercent);
    m_tokens[QStringLiteral("--accent-l")] = QString::number(lightnessPercent);
    // The picker moves the accent, so whatever pinned the old one gives way.
    for (const QString &token : {QStringLiteral("--color-accent"),
                                 QStringLiteral("--color-accent-1"),
                                 QStringLiteral("--color-accent-2")})
        m_pinned.remove(token);
    applyDerivedTokens();
}

QString Theme::firstAvailableFamily(const QStringList &families)
{
    const QStringList installed = QFontDatabase::families();
    for (const QString &family : families) {
        if (installed.contains(family, Qt::CaseInsensitive))
            return family;
    }
    return families.isEmpty() ? QString() : families.last();
}

bool Theme::hasFamily(const QString &family)
{
    return !family.isEmpty()
           && QFontDatabase::families().contains(family, Qt::CaseInsensitive);
}

void Theme::setFontOverrides(const QString &interfaceFamily, const QString &textFamily,
                             const QString &monospaceFamily)
{
    // A family nobody has is dropped here rather than handed to QFont, which
    // would substitute something arbitrary for it.
    m_interfaceFont = hasFamily(interfaceFamily.trimmed()) ? interfaceFamily.trimmed() : QString();
    m_textFont = hasFamily(textFamily.trimmed()) ? textFamily.trimmed() : QString();
    m_monospaceFont = hasFamily(monospaceFamily.trimmed()) ? monospaceFamily.trimmed() : QString();
}

QFont Theme::uiFont() const
{
    QFont font(m_interfaceFont.isEmpty()
                   ? firstAvailableFamily({QStringLiteral("Inter"), QStringLiteral("Noto Sans"),
                                           QStringLiteral("DejaVu Sans")})
                   : m_interfaceFont);
    font.setPixelSize(value(QStringLiteral("--font-ui-small")).toInt());
    return font;
}

QFont Theme::textFont() const
{
    // The note body takes its own family when one is set, and the interface
    // font otherwise - which is what Obsidian does too.
    QFont font = uiFont();
    if (!m_textFont.isEmpty())
        font.setFamily(m_textFont);
    font.setPixelSize(value(QStringLiteral("--font-text-size")).toInt());
    return font;
}

QFont Theme::monospaceFont() const
{
    QFont font(m_monospaceFont.isEmpty()
                   ? firstAvailableFamily({QStringLiteral("JetBrains Mono"),
                                           QStringLiteral("Roboto Mono"),
                                           QStringLiteral("Noto Sans Mono"),
                                           QStringLiteral("DejaVu Sans Mono")})
                   : m_monospaceFont);
    font.setFixedPitch(true);
    font.setPixelSize(value(QStringLiteral("--font-ui-medium")).toInt());
    return font;
}

QColor Theme::calloutColor(const QString &type) const
{
    // Obsidian's own mapping of the 13 callout types onto its named colours.
    static const QHash<QString, QString> colours = {
        {QStringLiteral("note"), QStringLiteral("--color-blue")},
        {QStringLiteral("abstract"), QStringLiteral("--color-cyan")},
        {QStringLiteral("summary"), QStringLiteral("--color-cyan")},
        {QStringLiteral("tldr"), QStringLiteral("--color-cyan")},
        {QStringLiteral("info"), QStringLiteral("--color-blue")},
        {QStringLiteral("todo"), QStringLiteral("--color-blue")},
        {QStringLiteral("tip"), QStringLiteral("--color-cyan")},
        {QStringLiteral("hint"), QStringLiteral("--color-cyan")},
        {QStringLiteral("important"), QStringLiteral("--color-cyan")},
        {QStringLiteral("success"), QStringLiteral("--color-green")},
        {QStringLiteral("check"), QStringLiteral("--color-green")},
        {QStringLiteral("done"), QStringLiteral("--color-green")},
        {QStringLiteral("question"), QStringLiteral("--color-orange")},
        {QStringLiteral("help"), QStringLiteral("--color-orange")},
        {QStringLiteral("faq"), QStringLiteral("--color-orange")},
        {QStringLiteral("warning"), QStringLiteral("--color-orange")},
        {QStringLiteral("caution"), QStringLiteral("--color-orange")},
        {QStringLiteral("attention"), QStringLiteral("--color-orange")},
        {QStringLiteral("failure"), QStringLiteral("--color-red")},
        {QStringLiteral("fail"), QStringLiteral("--color-red")},
        {QStringLiteral("missing"), QStringLiteral("--color-red")},
        {QStringLiteral("danger"), QStringLiteral("--color-red")},
        {QStringLiteral("error"), QStringLiteral("--color-red")},
        {QStringLiteral("bug"), QStringLiteral("--color-red")},
        {QStringLiteral("example"), QStringLiteral("--color-purple")},
        {QStringLiteral("quote"), QStringLiteral("--text-muted")},
        {QStringLiteral("cite"), QStringLiteral("--text-muted")},
    };
    return color(colours.value(type.toLower(), QStringLiteral("--color-blue")));
}

QString Theme::calloutIcon(const QString &type)
{
    static const QHash<QString, QString> icons = {
        {QStringLiteral("note"), QStringLiteral("pencil")},
        {QStringLiteral("abstract"), QStringLiteral("clipboard-list")},
        {QStringLiteral("summary"), QStringLiteral("clipboard-list")},
        {QStringLiteral("tldr"), QStringLiteral("clipboard-list")},
        {QStringLiteral("info"), QStringLiteral("info")},
        {QStringLiteral("todo"), QStringLiteral("circle-check")},
        {QStringLiteral("tip"), QStringLiteral("flame")},
        {QStringLiteral("hint"), QStringLiteral("flame")},
        {QStringLiteral("important"), QStringLiteral("flame")},
        {QStringLiteral("success"), QStringLiteral("check")},
        {QStringLiteral("check"), QStringLiteral("check")},
        {QStringLiteral("done"), QStringLiteral("check")},
        {QStringLiteral("question"), QStringLiteral("circle-help")},
        {QStringLiteral("help"), QStringLiteral("circle-help")},
        {QStringLiteral("faq"), QStringLiteral("circle-help")},
        {QStringLiteral("warning"), QStringLiteral("triangle-alert")},
        {QStringLiteral("caution"), QStringLiteral("triangle-alert")},
        {QStringLiteral("attention"), QStringLiteral("triangle-alert")},
        {QStringLiteral("failure"), QStringLiteral("x")},
        {QStringLiteral("fail"), QStringLiteral("x")},
        {QStringLiteral("missing"), QStringLiteral("x")},
        {QStringLiteral("danger"), QStringLiteral("zap")},
        {QStringLiteral("error"), QStringLiteral("zap")},
        {QStringLiteral("bug"), QStringLiteral("bug")},
        {QStringLiteral("example"), QStringLiteral("list")},
        {QStringLiteral("quote"), QStringLiteral("quote")},
        {QStringLiteral("cite"), QStringLiteral("quote")},
    };
    return icons.value(type.toLower(), QStringLiteral("pencil"));
}

QPalette Theme::palette() const
{
    const QColor primary = color(QStringLiteral("--background-primary"));
    const QColor secondary = color(QStringLiteral("--background-secondary"));
    const QColor normal = color(QStringLiteral("--text-normal"));
    const QColor muted = color(QStringLiteral("--text-muted"));
    const QColor faint = color(QStringLiteral("--text-faint"));
    const QColor accent = color(QStringLiteral("--interactive-accent"));

    QPalette palette;
    palette.setColor(QPalette::Window, secondary);
    palette.setColor(QPalette::WindowText, normal);
    palette.setColor(QPalette::Base, primary);
    palette.setColor(QPalette::AlternateBase, color(QStringLiteral("--background-primary-alt")));
    palette.setColor(QPalette::Text, normal);
    palette.setColor(QPalette::PlaceholderText, faint);
    palette.setColor(QPalette::Button, color(QStringLiteral("--interactive-normal")));
    palette.setColor(QPalette::ButtonText, normal);
    palette.setColor(QPalette::ToolTipBase, secondary);
    palette.setColor(QPalette::ToolTipText, normal);
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, color(QStringLiteral("--text-on-accent")));
    palette.setColor(QPalette::Link, color(QStringLiteral("--text-accent")));
    palette.setColor(QPalette::Mid, color(QStringLiteral("--divider-color")));
    palette.setColor(QPalette::Disabled, QPalette::Text, faint);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, faint);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, faint);
    palette.setColor(QPalette::Inactive, QPalette::Highlight,
                     color(QStringLiteral("--background-modifier-border")));
    palette.setColor(QPalette::Inactive, QPalette::HighlightedText, muted);
    return palette;
}

QString Theme::styleSheet() const
{
    QString sheet = QStringLiteral(R"QSS(
* {
    outline: none;
}

QMainWindow, QDialog {
    background: %{--background-secondary};
}

/* Ribbon ------------------------------------------------------------- */

QWidget#ribbon {
    background: %{--background-secondary};
    border-right: 1px solid %{--divider-color};
}
QToolButton#ribbonButton {
    background: transparent;
    border: none;
    border-radius: %{--radius-s}px;
    padding: 7px;
}
QToolButton#ribbonButton:hover {
    background: %{--background-modifier-hover};
}
QToolButton#ribbonButton:pressed {
    background: %{--background-modifier-active};
}

/* Sidebar ------------------------------------------------------------ */

QWidget#sidebar, QWidget#sidebarHeader {
    background: %{--background-secondary};
}
QLabel#vaultName {
    color: %{--text-muted};
    font-size: %{--font-ui-small}px;
    font-weight: 600;
    padding-left: 4px;
}
QToolButton#headerButton {
    background: transparent;
    border: none;
    border-radius: %{--radius-s}px;
    padding: 4px;
}
QToolButton#headerButton:hover {
    background: %{--background-modifier-hover};
}

QTreeView#fileExplorer {
    background: %{--background-secondary};
    border: none;
    padding: 2px 6px 8px 6px;
    color: %{--nav-item-color};
    font-size: %{--nav-item-size}px;
    font-weight: %{--nav-item-weight};
    show-decoration-selected: 0;
    outline: 0;
}
/* The hover and selected pills are drawn by FileRowDelegate inside the row.
   A background here would also fill the indent gutter left of the row, so
   both stay transparent. The row height comes from the delegate. */
QTreeView#fileExplorer::item {
    padding: 0 6px;
    border: none;
}
QTreeView#fileExplorer::item:hover,
QTreeView#fileExplorer::item:selected,
QTreeView#fileExplorer::item:selected:active {
    background: transparent;
}
/* Chevrons and indent guides are drawn by FileTree::drawBranches. */
QTreeView#fileExplorer::branch {
    background: transparent;
    image: none;
}
QTreeView#fileExplorer QLineEdit {
    background: %{--background-modifier-form-field};
    border: 1px solid %{--interactive-accent};
    border-radius: %{--radius-s}px;
    padding: 0 4px;
}

/* Main area: tab strip, inline title, editor -------------------------- */

QWidget#mainArea, QWidget#editorArea {
    background: %{--background-primary};
}
QWidget#tabStrip {
    background: %{--background-secondary};
}
QTabBar {
    background: transparent;
    border: none;
    qproperty-drawBase: 0;
}
QTabBar::tab {
    background: transparent;
    color: %{--text-faint};
    font-size: %{--font-ui-small}px;
    padding: 9px 16px;
    margin: 0;
    min-width: 90px;
    max-width: 260px;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
}
QTabBar::tab:hover {
    color: %{--text-muted};
}
QTabBar::tab:selected {
    background: %{--background-primary};
    color: %{--text-normal};
}

QLineEdit#inlineTitle {
    color: %{--text-normal};
    font-size: 30px;
    font-weight: 700;
    background: %{--background-primary};
    border: none;
    padding: 0;
    selection-background-color: %{--text-selection};
    selection-color: %{--text-normal};
}

QPlainTextEdit {
    background: %{--background-primary};
    color: %{--text-normal};
    border: none;
    selection-background-color: %{--text-selection};
    selection-color: %{--text-normal};
}

/* Right sidebar: linked mentions ------------------------------------- */

QWidget#rightSidebar {
    background: %{--background-secondary};
    border-left: 1px solid %{--divider-color};
}
QLabel#paneHeader {
    color: %{--text-faint};
    font-size: %{--font-ui-smaller}px;
    font-weight: 600;
    padding-left: 2px;
}
QTreeWidget#backlinkList {
    background: transparent;
    border: none;
    color: %{--text-muted};
    font-size: %{--font-ui-small}px;
    show-decoration-selected: 0;
}
QTreeWidget#backlinkList::item {
    padding: 3px 4px;
    border-radius: %{--radius-s}px;
    min-height: 20px;
}
QTreeWidget#backlinkList::item:hover {
    background: %{--background-modifier-hover};
    color: %{--text-normal};
}
QTreeWidget#backlinkList::branch {
    background: transparent;
    image: none;
}
QTreeWidget#backlinkList::branch:has-children:!has-siblings:closed,
QTreeWidget#backlinkList::branch:closed:has-children:has-siblings {
    image: url(:/icons/branch-collapsed.svg);
}
QTreeWidget#backlinkList::branch:open:has-children:!has-siblings,
QTreeWidget#backlinkList::branch:open:has-children:has-siblings {
    image: url(:/icons/branch-expanded.svg);
}

/* Quick switcher ------------------------------------------------------ */

QDialog#quickSwitcher {
    background: %{--background-secondary};
    border: 1px solid %{--background-modifier-border};
    border-radius: %{--radius-l}px;
}
QLineEdit#quickSwitcherQuery {
    background: %{--background-modifier-form-field};
    border: 1px solid %{--background-modifier-border};
    border-radius: %{--radius-m}px;
    padding: 8px 10px;
    color: %{--text-normal};
    font-size: %{--font-ui-medium}px;
}
QLineEdit#quickSwitcherQuery:focus {
    border-color: %{--interactive-accent};
}
QListWidget#quickSwitcherResults {
    background: transparent;
    border: none;
    color: %{--text-muted};
    font-size: %{--font-ui-small}px;
}
QListWidget#quickSwitcherResults::item {
    padding: 6px 8px;
    border-radius: %{--radius-s}px;
}
QListWidget#quickSwitcherResults::item:selected {
    background: %{--background-modifier-active};
    color: %{--text-normal};
}

/* The [[ suggester popup */
QCompleter QAbstractItemView, QListView#completerPopup, QListView#notebookCompletePopup {
    background: %{--background-secondary};
    border: 1px solid %{--background-modifier-border};
    border-radius: %{--radius-m}px;
    padding: 4px;
    color: %{--text-muted};
    font-size: %{--font-ui-small}px;
    outline: none;
}
QCompleter QAbstractItemView::item, QListView#completerPopup::item, QListView#notebookCompletePopup::item {
    padding: 5px 8px;
    border-radius: %{--radius-s}px;
}
QCompleter QAbstractItemView::item:selected, QListView#completerPopup::item:selected,
QListView#notebookCompletePopup::item:selected {
    background: %{--background-modifier-hover};
    color: %{--text-normal};
}

/* Documentation popup of a notebook block (Shift+Tab) */
QPlainTextEdit#notebookInspect {
    background: %{--background-secondary};
    color: %{--text-normal};
    border: 1px solid %{--background-modifier-border};
    border-radius: %{--radius-m}px;
    padding: 6px;
}

/* Chrome -------------------------------------------------------------- */

QSplitter::handle {
    background: %{--divider-color};
}
QSplitter::handle:horizontal {
    width: 1px;
}
QSplitter::handle:hover {
    background: %{--interactive-accent};
}

QMenu {
    background: %{--background-secondary};
    color: %{--text-normal};
    border: 1px solid %{--background-modifier-border};
    border-radius: %{--radius-m}px;
    padding: 6px;
}
QMenu::item {
    padding: 6px 28px 6px 12px;
    border-radius: %{--radius-s}px;
}
QMenu::item:selected {
    background: %{--background-modifier-hover};
}
QMenu::item:disabled {
    color: %{--text-faint};
    background: transparent;
}
QMenu::right-arrow {
    image: url(:/icons/branch-collapsed.svg);
    width: 12px;
    height: 12px;
    margin-right: 6px;
}
QMenu::indicator {
    width: 0;
    image: none;
}
QMenu::separator {
    height: 1px;
    background: %{--background-modifier-border};
    margin: 6px 8px;
}

QStatusBar {
    background: %{--background-secondary};
    color: %{--text-faint};
    border-top: 1px solid %{--divider-color};
}
QStatusBar::item {
    border: none;
}
QStatusBar QLabel {
    color: %{--text-faint};
    font-size: %{--font-ui-smaller}px;
    padding: 0 8px;
}
QStatusBar QToolButton {
    color: %{--text-faint};
    font-size: %{--font-ui-smaller}px;
    background: transparent;
    border: none;
    border-radius: %{--radius-s}px;
    padding: 2px 6px;
    margin: 1px 0;
}
QStatusBar QToolButton:hover, QStatusBar QToolButton:pressed, QStatusBar QToolButton:open {
    background: %{--background-modifier-hover};
    color: %{--text-muted};
}
QStatusBar QToolButton::menu-indicator {
    image: none;
    width: 0;
}

QScrollBar:vertical {
    background: transparent;
    width: 12px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: %{--scrollbar-thumb-bg};
    border-radius: 6px;
    min-height: 32px;
    margin: 2px;
}
QScrollBar::handle:vertical:hover {
    background: %{--scrollbar-active-thumb-bg};
}
QScrollBar:horizontal {
    background: transparent;
    height: 12px;
}
QScrollBar::handle:horizontal {
    background: %{--scrollbar-thumb-bg};
    border-radius: 6px;
    min-width: 32px;
    margin: 2px;
}
QScrollBar::add-line, QScrollBar::sub-line {
    width: 0;
    height: 0;
}
QScrollBar::add-page, QScrollBar::sub-page {
    background: transparent;
}

/* Inputs of the settings page and of dialogs: Obsidian's rounded fields. */
QWidget#settingsPage QLineEdit, QWidget#settingsPage QComboBox, QWidget#settingsPage QSpinBox,
QDialog QLineEdit, QDialog QComboBox, QDialog QSpinBox {
    background: %{--background-modifier-form-field};
    color: %{--text-normal};
    border: 1px solid %{--background-modifier-border};
    border-radius: %{--radius-s}px;
    padding: 4px 8px;
    min-height: 20px;
    selection-background-color: %{--text-selection};
    selection-color: %{--text-normal};
}
QWidget#settingsPage QLineEdit:focus, QWidget#settingsPage QComboBox:focus, QWidget#settingsPage QSpinBox:focus,
QDialog QLineEdit:focus, QDialog QComboBox:focus, QDialog QSpinBox:focus {
    border-color: %{--interactive-accent};
}
QWidget#settingsPage QComboBox:hover, QDialog QComboBox:hover {
    background: %{--interactive-hover};
}
QComboBox::drop-down {
    border: none;
    width: 22px;
}
QComboBox::down-arrow {
    image: url(:/icons/branch-expanded.svg);
    width: 12px;
    height: 12px;
}
QComboBox QAbstractItemView {
    background: %{--background-secondary};
    border: 1px solid %{--background-modifier-border};
    border-radius: %{--radius-m}px;
    padding: 4px;
    color: %{--text-normal};
    outline: none;
    selection-background-color: %{--background-modifier-hover};
    selection-color: %{--text-normal};
}
QComboBox QAbstractItemView::item {
    padding: 5px 8px;
    min-height: 20px;
    border-radius: %{--radius-s}px;
}
QSpinBox::up-button, QSpinBox::down-button {
    subcontrol-origin: border;
    width: 16px;
    border: none;
    background: transparent;
}
QSpinBox::up-button { subcontrol-position: top right; }
QSpinBox::down-button { subcontrol-position: bottom right; }
QSpinBox::up-arrow {
    image: url(:/icons/spin-up.svg);
    width: 10px;
    height: 10px;
}
QSpinBox::down-arrow {
    image: url(:/icons/branch-expanded.svg);
    width: 10px;
    height: 10px;
}

QPushButton {
    background: %{--interactive-normal};
    color: %{--text-normal};
    border: 1px solid transparent;
    border-radius: %{--radius-s}px;
    padding: 6px 14px;
    min-height: %{--input-height}px;
}
QPushButton:hover {
    background: %{--interactive-hover};
}
QPushButton:focus {
    border-color: %{--interactive-accent};
    outline: none;
}
QPushButton:default {
    background: %{--interactive-accent};
    color: %{--text-on-accent};
}
QPushButton:default:hover {
    background: %{--interactive-accent-hover};
}

QToolTip {
    background: %{--background-secondary};
    color: %{--text-normal};
    border: 1px solid %{--background-modifier-border};
    border-radius: %{--radius-s}px;
    padding: 4px 8px;
}
)QSS");

    for (auto it = m_tokens.constBegin(); it != m_tokens.constEnd(); ++it)
        sheet.replace(QLatin1String("%{") + it.key() + QLatin1Char('}'), it.value());
    return sheet;
}

} // namespace noteAppC
