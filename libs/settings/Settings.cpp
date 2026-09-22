#include "Settings.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace noteAppC {

namespace {

/// Every setting, with the value that applies when nothing else says
/// otherwise. Keys are grouped by the page they appear on.
const QVariantMap &defaults()
{
    static const QVariantMap map = {
        // Appearance
        {QStringLiteral("appearance.theme"), QStringLiteral("dark")}, // dark, light, system
        {QStringLiteral("appearance.scheme"), QString()},             // empty means built-in
        {QStringLiteral("appearance.accentHue"), 258},
        {QStringLiteral("appearance.accentSaturation"), 88},
        {QStringLiteral("appearance.accentLightness"), 66},
        {QStringLiteral("appearance.baseFontSize"), 16},
        {QStringLiteral("appearance.interfaceFont"), QString()},
        {QStringLiteral("appearance.textFont"), QString()},
        {QStringLiteral("appearance.monospaceFont"), QString()},
        {QStringLiteral("appearance.showInlineTitle"), true},
        {QStringLiteral("appearance.showRibbon"), true},

        // Editor
        {QStringLiteral("editor.defaultMode"), QStringLiteral("livePreview")},
        {QStringLiteral("editor.readableLineLength"), true},
        {QStringLiteral("editor.contentWidth"), 700},
        {QStringLiteral("editor.lineNumbers"), true},
        {QStringLiteral("editor.strictLineBreaks"), false},
        {QStringLiteral("editor.spellcheck"), true},
        {QStringLiteral("editor.autoPairs"), true},
        {QStringLiteral("editor.showIndentGuides"), true},
        {QStringLiteral("editor.tabWidth"), 4},

        // Files and links
        {QStringLiteral("files.attachmentFolder"), QString()},
        {QStringLiteral("files.confirmDelete"), true},
        {QStringLiteral("links.useWikilinks"), true},
        {QStringLiteral("links.autoUpdate"), true},

        // Remembered between launches
        {QStringLiteral("state.recentVaults"), QStringList()},
        {QStringLiteral("state.recentFiles"), QStringList()},
    };
    return map;
}

/// QSettings stores a QStringList of one element as a bare string, so a list
/// has to be asked for as a list rather than trusted to come back as one.
QVariant coerceToDefaultType(const QVariant &value, const QVariant &fallback)
{
    if (!value.isValid())
        return fallback;
    switch (fallback.typeId()) {
    case QMetaType::QStringList:
        return value.toStringList();
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::Int:
        return value.toInt();
    default:
        return value;
    }
}

QJsonObject readJSONobject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

void take(QVariantMap &out, const QJsonObject &object, const char *obsidianKey,
          const QString &key)
{
    const QJsonValue value = object.value(QLatin1String(obsidianKey));
    if (!value.isUndefined() && !value.isNull())
        out.insert(key, value.toVariant());
}

} // namespace

LaunchTarget resolveLaunchTarget(const QString &argument)
{
    LaunchTarget target;
    if (argument.isEmpty())
        return target;

    const QFileInfo info(argument);
    if (info.isDir()) {
        target.mode = OpenMode::Vault;
        target.vaultRoot = info.absoluteFilePath();
        return target;
    }
    if (!info.exists() && !argument.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
        return target;

    target.filePath = info.absoluteFilePath();

    // A note inside an existing vault should open the whole app, not a lone
    // window: that is what opening it from a file manager means.
    for (QDir dir = info.absoluteDir(); !dir.isRoot(); ) {
        if (QFile::exists(dir.filePath(QStringLiteral(".obsidian")))) {
            target.mode = OpenMode::Vault;
            target.vaultRoot = dir.absolutePath();
            return target;
        }
        if (!dir.cdUp())
            break;
    }

    // Anywhere else: one file, no vault, and nothing written beside it.
    target.mode = OpenMode::SingleFile;
    return target;
}

QString Settings::configDirectory()
{
    // XDG straight from the environment rather than QStandardPaths, which
    // needs an application name set and so answers differently in a tool or a
    // test than it does in the app.
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    const QString base = xdg.isEmpty() ? QDir::homePath() + QStringLiteral("/.config")
                                       : QString::fromLocal8Bit(xdg);
    return QDir(base).filePath(QStringLiteral("noteAppC"));
}

QString Settings::configFile()
{
    return QDir(configDirectory()).filePath(QStringLiteral("noteAppC.conf"));
}

QStringList Settings::keys()
{
    QStringList all = defaults().keys();
    all.sort();
    return all;
}

QVariant Settings::defaultValue(const QString &key)
{
    return defaults().value(key);
}

QVariantMap Settings::fromObsidianConfig(const QString &vaultRoot)
{
    QVariantMap out;
    if (vaultRoot.isEmpty())
        return out;
    const QDir config(QDir(vaultRoot).filePath(QStringLiteral(".obsidian")));
    if (!config.exists())
        return out;

    const QJsonObject app = readJSONobject(config.filePath(QStringLiteral("app.json")));
    take(out, app, "readableLineLength", QStringLiteral("editor.readableLineLength"));
    take(out, app, "showLineNumber", QStringLiteral("editor.lineNumbers"));
    take(out, app, "strictLineBreaks", QStringLiteral("editor.strictLineBreaks"));
    take(out, app, "spellcheck", QStringLiteral("editor.spellcheck"));
    take(out, app, "tabSize", QStringLiteral("editor.tabWidth"));
    take(out, app, "attachmentFolderPath", QStringLiteral("files.attachmentFolder"));
    take(out, app, "alwaysUpdateLinks", QStringLiteral("links.autoUpdate"));
    if (app.contains(QLatin1String("useMarkdownLinks"))) {
        out.insert(QStringLiteral("links.useWikilinks"),
                   !app.value(QLatin1String("useMarkdownLinks")).toBool());
    }
    // Obsidian splits the mode over two keys: which view, and whether the
    // editing one previews inline.
    if (app.contains(QLatin1String("defaultViewMode"))
        || app.contains(QLatin1String("livePreview"))) {
        const bool reading =
            app.value(QLatin1String("defaultViewMode")).toString() == QLatin1String("preview");
        const bool live = app.value(QLatin1String("livePreview")).toBool(true);
        out.insert(QStringLiteral("editor.defaultMode"),
                   reading ? QStringLiteral("reading")
                           : (live ? QStringLiteral("livePreview") : QStringLiteral("source")));
    }

    const QJsonObject appearance =
        readJSONobject(config.filePath(QStringLiteral("appearance.json")));
    const QString theme = appearance.value(QLatin1String("theme")).toString();
    if (theme == QLatin1String("obsidian"))
        out.insert(QStringLiteral("appearance.theme"), QStringLiteral("dark"));
    else if (theme == QLatin1String("moonstone"))
        out.insert(QStringLiteral("appearance.theme"), QStringLiteral("light"));
    else if (theme == QLatin1String("system"))
        out.insert(QStringLiteral("appearance.theme"), QStringLiteral("system"));

    const QString accent = appearance.value(QLatin1String("accentColor")).toString();
    const QColor accentColor = QColor::fromString(accent);
    if (accentColor.isValid()) {
        out.insert(QStringLiteral("appearance.accentHue"), qMax(0, accentColor.hslHue()));
        out.insert(QStringLiteral("appearance.accentSaturation"),
                   qRound(accentColor.hslSaturationF() * 100));
        out.insert(QStringLiteral("appearance.accentLightness"),
                   qRound(accentColor.lightnessF() * 100));
    }
    take(out, appearance, "baseFontSize", QStringLiteral("appearance.baseFontSize"));
    take(out, appearance, "interfaceFontFamily", QStringLiteral("appearance.interfaceFont"));
    take(out, appearance, "textFontFamily", QStringLiteral("appearance.textFont"));
    take(out, appearance, "monospaceFontFamily", QStringLiteral("appearance.monospaceFont"));
    take(out, appearance, "cssTheme", QStringLiteral("appearance.scheme"));
    return out;
}

Settings Settings::load(const QString &vaultRoot)
{
    Settings settings;
    settings.m_vaultRoot = vaultRoot;

    QSettings stored(configFile(), QSettings::IniFormat);
    for (const QString &key : stored.allKeys()) {
        if (defaults().contains(key))
            settings.m_user.insert(key, coerceToDefaultType(stored.value(key), defaultValue(key)));
    }

    settings.m_vault = fromObsidianConfig(vaultRoot);
    return settings;
}

QVariant Settings::value(const QString &key) const
{
    // The user's choice outranks the vault's, which outranks the default.
    if (m_user.contains(key))
        return m_user.value(key);
    if (m_vault.contains(key))
        return coerceToDefaultType(m_vault.value(key), defaultValue(key));
    return defaultValue(key);
}

void Settings::setValue(const QString &key, const QVariant &value)
{
    if (!defaults().contains(key))
        return;
    m_user.insert(key, value);
}

bool Settings::isCustomised(const QString &key) const
{
    return m_user.contains(key) && m_user.value(key) != defaultValue(key);
}

void Settings::reset(const QString &key)
{
    m_user.remove(key);
}

bool Settings::save(QString *error) const
{
    QDir().mkpath(configDirectory());
    QSettings stored(configFile(), QSettings::IniFormat);
    for (const QString &key : keys()) {
        // Only what the user actually changed, so the file stays readable and
        // a later change of default still reaches them.
        if (m_user.contains(key) && m_user.value(key) != defaultValue(key))
            stored.setValue(key, m_user.value(key));
        else
            stored.remove(key);
    }
    stored.sync();
    if (stored.status() != QSettings::NoError) {
        if (error)
            *error = QStringLiteral("could not write %1").arg(configFile());
        return false;
    }
    return true;
}

QStringList Settings::recentVaults() const
{
    return value(QStringLiteral("state.recentVaults")).toStringList();
}

QStringList Settings::recentFiles() const
{
    return value(QStringLiteral("state.recentFiles")).toStringList();
}

void Settings::noteOpened(const QString &vaultRoot, const QString &filePath)
{
    const auto push = [](QStringList list, const QString &entry) {
        if (entry.isEmpty())
            return list;
        list.removeAll(entry);
        list.prepend(entry);
        while (list.size() > 10)
            list.removeLast();
        return list;
    };
    setValue(QStringLiteral("state.recentVaults"), push(recentVaults(), vaultRoot));
    setValue(QStringLiteral("state.recentFiles"), push(recentFiles(), filePath));
}

} // namespace noteAppC
