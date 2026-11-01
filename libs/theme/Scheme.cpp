#include "Scheme.h"

#include "ConfigFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <cmath>

namespace noteAppC {

namespace {

/// The three accent numbers are not colours, but a scheme must be able to set
/// them: Obsidian's own accent picker writes exactly these.
bool isNumericToken(const QString &token)
{
    return token == QLatin1String("--accent-h") || token == QLatin1String("--accent-s")
        || token == QLatin1String("--accent-l");
}

QString hexOrRgba(const QColor &color)
{
    if (color.alphaF() >= 0.999)
        return color.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alphaF(), 0, 'f', 3);
}

/// Strips `/* ... */`. Obsidian themes are full of them, and a comment can
/// otherwise swallow a brace and misalign every block after it.
QString withoutComments(const QString &css)
{
    QString out;
    out.reserve(css.size());
    for (qsizetype i = 0; i < css.size();) {
        if (css.at(i) == QLatin1Char('/') && i + 1 < css.size()
            && css.at(i + 1) == QLatin1Char('*')) {
            const qsizetype end = css.indexOf(QLatin1String("*/"), i + 2);
            if (end < 0)
                break;
            i = end + 2;
            continue;
        }
        out.append(css.at(i));
        ++i;
    }
    return out;
}

/// One `selector { body }` pair. Blocks nested in an `@media` are returned
/// too, with their own selector, which is where themes often hide a dark
/// variant.
struct CSSblock {
    QString selector;
    QString body;
};

QList<CSSblock> blocksIn(const QString &css)
{
    QList<CSSblock> blocks;
    qsizetype selectorStart = 0;
    for (qsizetype i = 0; i < css.size(); ++i) {
        const QChar c = css.at(i);
        if (c == QLatin1Char('}')) {
            selectorStart = i + 1;
            continue;
        }
        if (c != QLatin1Char('{'))
            continue;

        const QString selector = css.mid(selectorStart, i - selectorStart).simplified();
        // Walk to the matching brace so a nested block does not end this one.
        int depth = 1;
        qsizetype j = i + 1;
        for (; j < css.size() && depth > 0; ++j) {
            if (css.at(j) == QLatin1Char('{'))
                ++depth;
            else if (css.at(j) == QLatin1Char('}'))
                --depth;
        }
        blocks.append({selector, css.mid(i + 1, j - i - 2)});
        selectorStart = i + 1; // a nested block's selector starts here
    }
    return blocks;
}

/// `--name: value;` pairs. The value runs to the first `;` outside brackets,
/// because `rgba(0, 0, 0, .5)` carries commas and `var(--x, #fff)` carries a
/// comma and a fallback.
QHash<QString, QString> declarationsIn(const QString &body)
{
    QHash<QString, QString> out;
    for (qsizetype i = 0; i < body.size();) {
        const qsizetype dash = body.indexOf(QLatin1String("--"), i);
        if (dash < 0)
            break;
        const qsizetype colon = body.indexOf(QLatin1Char(':'), dash);
        if (colon < 0)
            break;
        const QString name = body.mid(dash, colon - dash).trimmed();
        if (name.contains(QLatin1Char(' ')) || name.contains(QLatin1Char('{'))) {
            i = colon + 1;
            continue;
        }

        int depth = 0;
        qsizetype end = colon + 1;
        for (; end < body.size(); ++end) {
            const QChar c = body.at(end);
            if (c == QLatin1Char('(') || c == QLatin1Char('['))
                ++depth;
            else if (c == QLatin1Char(')') || c == QLatin1Char(']'))
                --depth;
            else if (c == QLatin1Char(';') && depth <= 0)
                break;
        }
        QString value = body.mid(colon + 1, end - colon - 1).trimmed();
        if (value.endsWith(QLatin1String("!important")))
            value.chop(10);
        out.insert(name, value.trimmed());
        i = end + 1;
    }
    return out;
}

bool mentions(const QString &selector, const char *needle)
{
    return selector.contains(QLatin1String(needle), Qt::CaseInsensitive);
}

} // namespace

QString Scheme::normaliseColor(const QString &value)
{
    const QColor color = Theme::parseColor(value);
    return color.isValid() ? hexOrRgba(color) : QString();
}

QString Scheme::resolveVars(const QString &value, const QHash<QString, QString> &raw, int depth)
{
    if (depth > 8 || !value.contains(QLatin1String("var(")))
        return value;

    QString out = value;
    const qsizetype start = out.indexOf(QLatin1String("var("));
    int nesting = 0;
    qsizetype end = start + 4;
    for (; end < out.size(); ++end) {
        const QChar c = out.at(end);
        if (c == QLatin1Char('('))
            ++nesting;
        else if (c == QLatin1Char(')')) {
            if (nesting == 0)
                break;
            --nesting;
        }
    }
    if (end >= out.size())
        return out;

    const QString inner = out.mid(start + 4, end - start - 4);
    const qsizetype comma = inner.indexOf(QLatin1Char(','));
    const QString name = (comma < 0 ? inner : inner.left(comma)).trimmed();
    const QString fallback = comma < 0 ? QString() : inner.mid(comma + 1).trimmed();

    QString replacement = raw.value(name, fallback);
    if (replacement.contains(QLatin1String("var(")))
        replacement = resolveVars(replacement, raw, depth + 1);

    out.replace(start, end - start + 1, replacement);
    return resolveVars(out, raw, depth + 1);
}

QHash<QString, QString> Scheme::parseObsidianCSS(const QString &css, Theme::Mode mode)
{
    const QList<CSSblock> blocks = blocksIn(withoutComments(css));
    // An OLED scheme reads an Obsidian theme's dark side; only the base
    // greys differ, and those are ours.
    const bool dark = mode != Theme::Mode::Light;
    const char *wanted = dark ? "theme-dark" : "theme-light";
    const char *other = dark ? "theme-light" : "theme-dark";

    // Raw first, so `var()` can point at anything the file declares anywhere.
    QHash<QString, QString> raw;
    QHash<QString, QString> generic;
    QHash<QString, QString> specific;

    for (const CSSblock &block : blocks) {
        const bool isWanted = mentions(block.selector, wanted);
        const bool isOther = mentions(block.selector, other);
        const bool isGeneric = !isWanted && !isOther
            && (mentions(block.selector, ":root") || mentions(block.selector, "body")
                || block.selector.isEmpty());
        if (!isWanted && !isGeneric)
            continue;

        const QHash<QString, QString> declarations = declarationsIn(block.body);
        for (auto it = declarations.constBegin(); it != declarations.constEnd(); ++it) {
            raw.insert(it.key(), it.value());
            (isWanted ? specific : generic).insert(it.key(), it.value());
        }
    }

    // A `.theme-dark` block outranks a `:root` one however the file orders it.
    QHash<QString, QString> merged = generic;
    for (auto it = specific.constBegin(); it != specific.constEnd(); ++it)
        merged.insert(it.key(), it.value());

    QHash<QString, QString> tokens;
    for (auto it = merged.constBegin(); it != merged.constEnd(); ++it) {
        const QString value = resolveVars(it.value(), raw).trimmed();
        if (isNumericToken(it.key())) {
            QString number = value;
            number.remove(QLatin1Char('%'));
            bool ok = false;
            number.trimmed().toDouble(&ok);
            if (ok)
                tokens.insert(it.key(), number.trimmed());
            continue;
        }
        const QString color = normaliseColor(value);
        if (!color.isEmpty())
            tokens.insert(it.key(), color);
    }
    return tokens;
}

QStringList Scheme::searchPaths(const QString &vaultRoot)
{
    QStringList paths;
    if (!vaultRoot.isEmpty()) {
        paths << QDir(vaultRoot).filePath(QStringLiteral(".obsidian/noteAppC-themes"));
        paths << QDir(vaultRoot).filePath(QStringLiteral(".obsidian/themes"));
    }
    // configFilePath() is the same resolution Settings::themesDirectory()
    // documents, so a theme put where the docs say is the theme found here.
    paths << configFilePath(QStringLiteral("themes"));
    return paths;
}

QList<SchemeInfo> Scheme::available(const QString &vaultRoot)
{
    QList<SchemeInfo> schemes;
    const QStringList paths = searchPaths(vaultRoot);
    for (const QString &path : paths) {
        QDir dir(path);
        if (!dir.exists())
            continue;
        const bool inVault = !vaultRoot.isEmpty() && path.startsWith(vaultRoot);

        for (const QFileInfo &entry :
             dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            schemes.append({entry.completeBaseName(), entry.absoluteFilePath(), false, inVault});
        }
        // A loose .css file, named after itself, read exactly like an
        // Obsidian theme's theme.css - load() already dispatches on the
        // extension, so this only has to be found and named.
        for (const QFileInfo &entry :
             dir.entryInfoList({QStringLiteral("*.css")}, QDir::Files, QDir::Name)) {
            schemes.append({entry.completeBaseName(), entry.absoluteFilePath(), true, inVault});
        }
        // Obsidian installs a community theme as <name>/theme.css.
        for (const QFileInfo &entry : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString css = QDir(entry.absoluteFilePath()).filePath(QStringLiteral("theme.css"));
            if (QFile::exists(css))
                schemes.append({entry.fileName(), css, true, inVault});
        }
    }

    std::sort(schemes.begin(), schemes.end(), [](const SchemeInfo &a, const SchemeInfo &b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });
    return schemes;
}

QHash<QString, QString> Scheme::load(const QString &path, Theme::Mode mode, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    const QByteArray bytes = file.readAll();

    if (!path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
        return parseObsidianCSS(QString::fromUtf8(bytes), mode);

    QJsonParseError parse{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = parse.errorString();
        return {};
    }

    const QJsonObject root = document.object();
    const QString key = mode == Theme::Mode::Light ? QStringLiteral("light")
                                                   : QStringLiteral("dark");
    QHash<QString, QString> tokens;
    // Zed's theme shape: a `syntax` object of dotted capture names, at the
    // top level for every mode and again inside a mode to refine it. Kept as
    // `--syntax-<capture>` tokens so a scheme round-trips through Theme and
    // save() unchanged; the value is "#colour [bold] [italic]", not just a colour.
    for (const QJsonObject &object : {root.value(QStringLiteral("syntax")).toObject(),
                                      root.value(key).toObject().value(QStringLiteral("syntax")).toObject()}) {
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
            tokens.insert(QStringLiteral("--syntax-") + it.key(), it.value().toString());
    }
    // A scheme may carry shared tokens at the top level under "tokens".
    for (const QString &section : {QStringLiteral("tokens"), key}) {
        const QJsonObject object = root.value(section).toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (it.value().isObject())
                continue; // the mode's own `syntax` object, read above
            const QString value = it.value().toVariant().toString();
            if (isNumericToken(it.key())) {
                tokens.insert(it.key(), value);
                continue;
            }
            const QString color = normaliseColor(value);
            tokens.insert(it.key(), color.isEmpty() ? value : color);
        }
    }
    return tokens;
}

double Scheme::contrastRatio(const QColor &a, const QColor &b)
{
    const auto luminance = [](const QColor &color) {
        const auto channel = [](double value) {
            return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF())
            + 0.0722 * channel(color.blueF());
    };
    const double first = luminance(a);
    const double second = luminance(b);
    return (qMax(first, second) + 0.05) / (qMin(first, second) + 0.05);
}

void Scheme::makeUsable(Theme &theme)
{
    const Theme base = Theme::builtin(theme.mode());

    // A window background cannot be see-through: composite it onto the
    // built-in one, which is what the compositor would show underneath.
    for (const QString &token : {QStringLiteral("--background-primary"),
                                 QStringLiteral("--background-primary-alt"),
                                 QStringLiteral("--background-secondary"),
                                 QStringLiteral("--background-secondary-alt")}) {
        const QColor color = theme.color(token);
        if (!color.isValid() || color.alphaF() >= 0.999)
            continue;
        QColor under = base.color(token);
        const double alpha = color.alphaF();
        theme.setToken(token,
                       QColor::fromRgbF(float(color.redF() * alpha + under.redF() * (1 - alpha)),
                                        float(color.greenF() * alpha + under.greenF() * (1 - alpha)),
                                        float(color.blueF() * alpha + under.blueF() * (1 - alpha)))
                           .name(QColor::HexRgb));
    }

    // Text that cannot be read against its own background means the theme
    // expressed it with something we could not resolve. Fall back rather than
    // ship an unreadable window.
    const QColor background = theme.color(QStringLiteral("--background-primary"));
    const QColor fallbackText = base.color(QStringLiteral("--text-normal"));
    for (const auto &pair : {std::pair{QStringLiteral("--text-normal"), 4.5},
                             std::pair{QStringLiteral("--text-muted"), 2.5},
                             std::pair{QStringLiteral("--text-faint"), 1.8}}) {
        const QColor text = theme.color(pair.first);
        if (!text.isValid() || contrastRatio(text, background) < pair.second) {
            // Keep the built-in relationship, re-based on the new background.
            theme.resetToken(pair.first);
            if (contrastRatio(theme.color(pair.first), background) < pair.second)
                theme.setToken(pair.first,
                               contrastRatio(fallbackText, background)
                                       > contrastRatio(QColor(Qt::white), background)
                                   ? fallbackText.name(QColor::HexRgb)
                                   : QStringLiteral("#ffffff"));
        }
    }
}

Theme Scheme::apply(const QString &path, Theme::Mode mode, QString *error)
{
    Theme theme = Theme::builtin(mode);
    const QHash<QString, QString> tokens = load(path, mode, error);
    if (!tokens.isEmpty())
        theme.setTokens(tokens);
    makeUsable(theme);
    return theme;
}

bool Scheme::save(const QString &path, const QString &name, const Theme &dark, const Theme &light,
                  QString *error)
{
    const auto toObject = [](const QHash<QString, QString> &tokens) {
        QJsonObject object;
        for (auto it = tokens.constBegin(); it != tokens.constEnd(); ++it)
            object.insert(it.key(), it.value());
        return object;
    };

    QJsonObject root;
    root.insert(QStringLiteral("name"), name);
    root.insert(QStringLiteral("dark"), toObject(dark.overrides()));
    root.insert(QStringLiteral("light"), toObject(light.overrides()));

    QDir().mkpath(QFileInfo(path).absolutePath());
    // Through a temporary file and rename(2), like every other write here.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace noteAppC
