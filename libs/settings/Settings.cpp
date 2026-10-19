#include "Settings.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

namespace noteAppC {

namespace {

/// Every setting, with the value that applies when nothing else says
/// otherwise. Keys are grouped by the page they appear on.
const QVariantMap &defaults()
{
    static const QVariantMap map = [] {
    QVariantMap map = {
        // Appearance
        {QStringLiteral("appearance.theme"), QStringLiteral("dark")}, // dark, oled, light, system
        {QStringLiteral("appearance.scheme"), QString()},             // empty means built-in
        {QStringLiteral("appearance.accentHue"), 258},
        {QStringLiteral("appearance.accentSaturation"), 88},
        {QStringLiteral("appearance.accentLightness"), 66},
        {QStringLiteral("appearance.baseFontSize"), 16},
        {QStringLiteral("appearance.interfaceFont"), QString()},
        {QStringLiteral("appearance.textFont"), QString()},
        {QStringLiteral("appearance.monospaceFont"), QString()},
        {QStringLiteral("appearance.showInlineTitle"), true},
        {QStringLiteral("appearance.fileIcons"), true},
        {QStringLiteral("appearance.showRibbon"), true},

        // Editor
        {QStringLiteral("editor.defaultMode"), QStringLiteral("livePreview")},
        {QStringLiteral("editor.readableLineLength"), true},
        {QStringLiteral("editor.contentWidth"), 700},
        {QStringLiteral("editor.lineNumbers"), true},
        {QStringLiteral("editor.strictLineBreaks"), false},
        {QStringLiteral("editor.spellcheck"), true},
        {QStringLiteral("lookup.server"), QString()},
        {QStringLiteral("lookup.webSearchURL"), QStringLiteral("https://duckduckgo.com/?q=%1")},
        {QStringLiteral("fileRecovery.interval"), 5}, // minutes, Obsidian's default
        {QStringLiteral("fileRecovery.keepDays"), 7},
        {QStringLiteral("editor.autoPairs"), true},
        {QStringLiteral("editor.vimMode"), false},
        {QStringLiteral("editor.showIndentGuides"), true},
        // Lists: how ordered numbers and bullets are drawn (display only; the file is CommonMark).
        {QStringLiteral("editor.listNumberStyle"), QStringLiteral("plain")}, // or "hierarchical": 1., 1.1., 1.2.
        {QStringLiteral("editor.listNumberSuffix"), QStringLiteral(".")},   // after a drawn number; empty: as written
        {QStringLiteral("editor.bulletGlyphs"), QStringLiteral("-=dash,*=dot,+=diamond")},
        {QStringLiteral("editor.bulletGlyphsNested"), true}, // a turn arrow for nested bullets instead of by marker
        {QStringLiteral("editor.alignTables"), true},
        {QStringLiteral("editor.hoverEditButton"), true},
        {QStringLiteral("editor.tableHandles"), true}, // row/column grips, add bars, cell selection on a drawn table
        {QStringLiteral("editor.complexCodeBlocks"), true}, // bg/fg/border/syntax.* on code blocks; nothing configured draws as before
        {QStringLiteral("editor.metaNotes"), false}, // [like this] in --meta-note-color; off: the parse never looks
        {QStringLiteral("editor.metaNoteOpen"), QStringLiteral("[")},   // 1-3 characters
        {QStringLiteral("editor.metaNoteClose"), QStringLiteral("]")},
        // Word and character counts. Empty delimiters mean the meta-note ones.
        {QStringLiteral("editor.countExcludeBetween"), false},
        {QStringLiteral("editor.countExcludeOpen"), QString()},
        {QStringLiteral("editor.countExcludeClose"), QString()},
        {QStringLiteral("editor.countIgnoreFormatting"), false},
        {QStringLiteral("editor.tabWidth"), 4},

        // Files and links
        {QStringLiteral("files.attachmentFolder"), QString()},
        {QStringLiteral("files.newNoteLocation"), QStringLiteral("root")}, // root, current (the open note's folder) or folder
        {QStringLiteral("files.newNoteFolder"), QString()},               // for "folder"
        {QStringLiteral("encryption.unlockAtStartup"), false}, // false: asked when an encrypted note opens
        {QStringLiteral("encryption.autoLockMinutes"), 15},
        {QStringLiteral("files.sortOrder"), QStringLiteral("nameAsc")}, // nameAsc, nameDesc, modifiedDesc, modifiedAsc
        {QStringLiteral("files.confirmDelete"), true},
        {QStringLiteral("files.deletedFiles"), QStringLiteral("system")}, // system trash, local (the vault's .trash) or permanent
        {QStringLiteral("files.autosave"), true}, // Obsidian's way: typed is kept
        {QStringLiteral("sync.serverURL"), QString()},
        {QStringLiteral("sync.email"), QString()},
        {QStringLiteral("notebooks.allowHostTier"), false}, // a note's "#| sandbox: host" is only a request
        // false: each block is a short process of the system's own tool. true: a note's blocks share
        // one Jupyter kernel per language (variables persist, execution counts, rich output) where one is installed.
        {QStringLiteral("notebooks.sharedKernel"), false},
        // `#| sandbox: wasm` blocks run a WebAssembly module in place of the language's interpreter:
        // `python=/path/python.wasm;ruby=/path/ruby.wasm`. A language with no entry is refused there.
        {QStringLiteral("notebooks.wasmModules"), QString()},
        {QStringLiteral("editor.gpuAcceleration"), false},
        // Mermaid diagrams, drawn by an external tool on a thread pool. On is free until a
        // ```mermaid fence is on screen; the tool is a name looked up on PATH and in the cargo bin directories, or a path.
        {QStringLiteral("render.mermaid"), true},
        {QStringLiteral("render.mermaidTool"), QStringLiteral("mmdr")},
        // PlantUML, TikZ and Graphviz fences the same way, each tool run inside bubblewrap's sandbox
        // (so under /usr). TikZ is latex then dvisvgm (found next to it, or on PATH).
        {QStringLiteral("render.plantuml"), true},
        {QStringLiteral("render.plantumlTool"), QStringLiteral("plantuml")},
        {QStringLiteral("render.tikz"), true},
        {QStringLiteral("render.latexTool"), QStringLiteral("latex")},
        {QStringLiteral("render.graphviz"), true},
        {QStringLiteral("render.dotTool"), QStringLiteral("dot")},
    {QStringLiteral("links.useWikilinks"), true},
        {QStringLiteral("links.autoUpdate"), true},

        // Daily notes and templates. A vault's own `.obsidian/daily-notes.json`
        // and `templates.json` are read directly by DailyNotes and Templates
        // (libs/vault) rather than mapped in here, the same as bookmarks.json;
        // these are the fallback for a vault that has never set its own, and
        // Obsidian's own defaults besides.
        {QStringLiteral("dailyNotes.folder"), QString()},
        {QStringLiteral("dailyNotes.format"), QStringLiteral("YYYY-MM-DD")},
        {QStringLiteral("dailyNotes.template"), QString()},
        {QStringLiteral("templates.folder"), QString()},
        {QStringLiteral("templates.dateFormat"), QStringLiteral("YYYY-MM-DD")},
        {QStringLiteral("templates.timeFormat"), QStringLiteral("HH:mm")},
        {QStringLiteral("uniqueNote.folder"), QString()},
        {QStringLiteral("noteComposer.replaceWith"), QStringLiteral("link")}, // or "embed"
        {QStringLiteral("uniqueNote.format"), QStringLiteral("YYYYMMDDHHmm")}, // Obsidian's default

        // Remembered between launches
        {QStringLiteral("state.recentVaults"), QStringList()},
        {QStringLiteral("state.recentFiles"), QStringList()},
        {QStringLiteral("state.notePositions"), QStringList()},
    };
    // Core plugins: every feature on until a person or the vault says not.
    for (const Settings::Feature &feature : Settings::features())
        // Off until asked for: another program's code, and a "/" menu that
        // Obsidian also ships switched off, and the annotating image tab,
        // which replaces the plain picture a person may want as it was.
        map.insert(Settings::featureKey(feature.id),
                   QByteArray(feature.id) != "community-plugins" && QByteArray(feature.id) != "slash-command"
                       && QByteArray(feature.id) != "image-annotation");
    return map;
    }();
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

/// `editor.readableLineLength` is `[editor] readable-line-length` in the file:
/// dotted camelCase is what the code reads best, kebab under a section header
/// is what a person reads best.
QString kebabOf(const QString &name)
{
    QString out;
    for (const QChar c : name) {
        if (c.isUpper()) {
            out.append(QLatin1Char('-'));
            out.append(c.toLower());
        } else {
            out.append(c);
        }
    }
    return out;
}

QString camelOf(const QString &name)
{
    QString out;
    bool upper = false;
    for (const QChar c : name) {
        if (c == QLatin1Char('-')) {
            upper = true;
            continue;
        }
        out.append(upper ? c.toUpper() : c);
        upper = false;
    }
    return out;
}

/// A value as a person would write it: bare true/false, bare numbers, bare
/// text. No quoting and no escaping, because the point of this file is that it
/// can be opened in an editor.
QString textOf(const QVariant &value)
{
    switch (value.typeId()) {
    case QMetaType::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QMetaType::QStringList:
        return value.toStringList().join(QStringLiteral(", "));
    default:
        return value.toString();
    }
}

QVariant valueOf(const QString &text, const QVariant &like)
{
    switch (like.typeId()) {
    case QMetaType::Bool:
        return text.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
            || text == QLatin1String("1") || text.compare(QLatin1String("yes"), Qt::CaseInsensitive) == 0;
    case QMetaType::Int:
        return text.toInt();
    case QMetaType::QStringList: {
        QStringList parts;
        for (const QString &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts))
            parts.append(part.trimmed());
        return parts;
    }
    default:
        return text;
    }
}

/// Reads `[section] key = value` into dotted keys. Comments and blank lines are
/// skipped; an unknown key is kept so the caller can decide.
QVariantMap readINI(const QString &path)
{
    QVariantMap out;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;

    QString section;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';')))
            continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            section = line.mid(1, line.size() - 2).trimmed();
            continue;
        }
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals < 0)
            continue;
        const QString name = camelOf(line.left(equals).trimmed());
        const QString text = line.mid(equals + 1).trimmed();
        out.insert(section.isEmpty() ? name : section + QLatin1Char('.') + name, text);
    }
    return out;
}

/// Rewrites `path` so it carries `values`, keeping every comment, blank line
/// and unknown key exactly where the person left them. A file nobody can edit
/// by hand is not a configuration file.
bool writeINI(const QString &path, const QVariantMap &values, const QSet<QString> &known,
              const QString &header, QString *error)
{
    QStringList lines;
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&existing);
        while (!stream.atEnd())
            lines.append(stream.readLine());
        existing.close();
    }
    const bool fresh = lines.isEmpty();

    QSet<QString> written;
    QList<qsizetype> drop; // lines for a known key the app no longer sets
    QString section;
    for (qsizetype index = 0; index < lines.size(); ++index) {
        QString &line = lines[index];
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))) {
            section = trimmed.mid(1, trimmed.size() - 2).trimmed();
            continue;
        }
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))
            || trimmed.startsWith(QLatin1Char(';')))
            continue;
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals < 0)
            continue;

        const QString name = line.left(equals).trimmed();
        const QString key = section.isEmpty() ? camelOf(name)
                                              : section + QLatin1Char('.') + camelOf(name);
        if (!values.contains(key)) {
            // A key the app owns and no longer sets was reset, so its line
            // goes. A key it has never heard of is the person's, and stays.
            if (known.contains(key))
                drop.append(index);
            continue;
        }
        line = QStringLiteral("%1 = %2").arg(name, textOf(values.value(key)));
        written.insert(key);
    }
    for (auto it = drop.crbegin(); it != drop.crend(); ++it)
        lines.removeAt(*it);

    // Whatever is new gets appended under its section, creating it if needed.
    QMap<QString, QStringList> pending; // section -> "key = value"
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        if (written.contains(it.key()))
            continue;
        const qsizetype dot = it.key().indexOf(QLatin1Char('.'));
        const QString group = dot < 0 ? QString() : it.key().left(dot);
        const QString name = kebabOf(dot < 0 ? it.key() : it.key().mid(dot + 1));
        pending[group].append(QStringLiteral("%1 = %2").arg(name, textOf(it.value())));
    }

    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        // Find the section's last line, or make the section at the end.
        qsizetype insertAt = -1;
        QString current;
        for (qsizetype i = 0; i < lines.size(); ++i) {
            const QString trimmed = lines.at(i).trimmed();
            if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']')))
                current = trimmed.mid(1, trimmed.size() - 2).trimmed();
            if (current == it.key())
                insertAt = i + 1;
        }
        if (insertAt < 0) {
            if (!lines.isEmpty() && !lines.last().trimmed().isEmpty())
                lines.append(QString());
            lines.append(QStringLiteral("[%1]").arg(it.key()));
            insertAt = lines.size();
        }
        for (const QString &entry : it.value())
            lines.insert(insertAt++, entry);
    }

    // A file written for the first time explains itself; one the person has
    // already edited is left to say whatever they made it say.
    if (fresh && !header.isEmpty()) {
        QStringList intro = header.split(QLatin1Char('\n'));
        while (!intro.isEmpty() && intro.last().trimmed().isEmpty())
            intro.removeLast();
        intro.append(QString());
        lines = intro + lines;
    }

    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QTextStream out(&file);
    for (const QString &line : lines)
        out << line << '\n';
    out.flush();
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
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

/// Separates the three fields of one `state.notePositions` entry. A tab,
/// because a path can hold a comma - the character `textOf`/`valueOf` already
/// use to join and split the list itself - but essentially never holds a tab.
constexpr QChar kPositionField = QLatin1Char('\t');

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

    // A folder opened as a vault before counts too, even without
    // `.obsidian` - how else would a note in it have been found.
    for (const QString &vault : Settings::load().recentVaults()) {
        if (target.filePath.startsWith(QDir::cleanPath(vault) + QLatin1Char('/'))) {
            target.mode = OpenMode::Vault;
            target.vaultRoot = QDir::cleanPath(vault);
            return target;
        }
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
    return QDir(configDirectory()).filePath(QStringLiteral("settings.ini"));
}

QString Settings::indexFile()
{
    return QDir(configDirectory()).filePath(QStringLiteral("vaults.ini"));
}

QString Settings::themesDirectory()
{
    return QDir(configDirectory()).filePath(QStringLiteral("themes"));
}

QString Settings::templatesDirectory()
{
    return QDir(configDirectory()).filePath(QStringLiteral("templates"));
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
    take(out, app, "newFileLocation", QStringLiteral("files.newNoteLocation"));
    take(out, app, "newFileFolderPath", QStringLiteral("files.newNoteFolder"));
    take(out, app, "alwaysUpdateLinks", QStringLiteral("links.autoUpdate"));
    // Obsidian's "Deleted files": system, local or none.
    const QString trashOption = app.value(QLatin1String("trashOption")).toString();
    if (trashOption == QLatin1String("system") || trashOption == QLatin1String("local"))
        out.insert(QStringLiteral("files.deletedFiles"), trashOption);
    else if (trashOption == QLatin1String("none"))
        out.insert(QStringLiteral("files.deletedFiles"), QStringLiteral("permanent"));
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

    // Newer Obsidian writes {"graph": true, ...}; older wrote the array of
    // the plugins that were on, so any id missing from it was off.
    const QJsonDocument plugins = [&config] {
        QFile file(config.filePath(QStringLiteral("core-plugins.json")));
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()) : QJsonDocument();
    }();
    const QJsonArray enabled = plugins.array();
    for (const Feature &feature : features()) {
        if (!feature.obsidian)
            continue;
        const QString id = QString::fromLatin1(feature.id);
        if (plugins.isObject() && plugins.object().value(id).isBool())
            out.insert(featureKey(feature.id), plugins.object().value(id).toBool());
        // Bases came after the array form: a vault that wrote one never saw it.
        else if (plugins.isArray() && id != QLatin1String("bases"))
            out.insert(featureKey(feature.id), enabled.contains(id));
    }
    return out;
}

const QList<Settings::Feature> &Settings::features()
{
    // Obsidian's own names and wording, where it has the feature.
    static const QList<Feature> list = {
        {"audio-recorder", QT_TRANSLATE_NOOP("Settings", "Audio recorder"),
         QT_TRANSLATE_NOOP("Settings", "Record audio from the microphone into an attachment, embedded in the note."),
         true},
        {"backlink", QT_TRANSLATE_NOOP("Settings", "Backlinks"),
         QT_TRANSLATE_NOOP("Settings", "Show the notes that link to this one in the right sidebar."), true},
        {"unlinked-mentions", QT_TRANSLATE_NOOP("Settings", "Unlinked mentions"),
         QT_TRANSLATE_NOOP("Settings", "Under the backlinks, find every note that names this one without "
                                       "linking it. A search of the vault on each note opened."),
         false},
        {"bases", QT_TRANSLATE_NOOP("Settings", "Bases"),
         QT_TRANSLATE_NOOP("Settings", "Open .base files and base code blocks as tables of notes."), true},
        {"bookmarks", QT_TRANSLATE_NOOP("Settings", "Bookmarks"),
         QT_TRANSLATE_NOOP("Settings", "Bookmark notes, headings and searches in the right sidebar."), true},
        {"command-palette", QT_TRANSLATE_NOOP("Settings", "Command palette"),
         QT_TRANSLATE_NOOP("Settings", "Run any command by name with Ctrl+P."), true},
        {"canvas", QT_TRANSLATE_NOOP("Settings", "Canvas"),
         QT_TRANSLATE_NOOP("Settings", "Open .canvas files as boards of cards and connections."), true},
        {"community-plugins", QT_TRANSLATE_NOOP("Settings", "Community plugins"),
         QT_TRANSLATE_NOOP("Settings", "Load plugins from ~/.local/share/noteAppC/plugins, each in its own "
                                       "sandboxed process, with only the permissions you grant it."),
         false},
        {"daily-notes", QT_TRANSLATE_NOOP("Settings", "Daily notes"),
         QT_TRANSLATE_NOOP("Settings", "Open or create today's note."), true},
        {"html-blocks", QT_TRANSLATE_NOOP("Settings", "HTML blocks"),
         QT_TRANSLATE_NOOP("Settings", "Draw raw HTML in notes as HTML rather than showing its source."), false},
        {"media-embeds", QT_TRANSLATE_NOOP("Settings", "Audio and video embeds"),
         QT_TRANSLATE_NOOP("Settings", "Play embedded audio and video files in the note."), false},
        {"pdf-embeds", QT_TRANSLATE_NOOP("Settings", "PDF embeds"),
         QT_TRANSLATE_NOOP("Settings", "Show embedded PDFs as pages - one, a list, a range or all, in a column, "
                                       "a grid or as slides."),
         false},
        {"tex-embeds", QT_TRANSLATE_NOOP("Settings", "LaTeX embeds"),
         QT_TRANSLATE_NOOP("Settings", "Render an embedded .tex file: maths through MicroTeX, a whole document "
                                       "compiled by the TeX installed, in a sandbox."),
         false},
        {"epub-reader", QT_TRANSLATE_NOOP("Settings", "EPUB reader"),
         QT_TRANSLATE_NOOP("Settings", "Open .epub books in a tab, with highlights and notes kept in a note "
                                       "beside the book."),
         false},
        {"file-recovery", QT_TRANSLATE_NOOP("Settings", "File recovery"),
         QT_TRANSLATE_NOOP("Settings", "Keep snapshots of notes as they are saved, outside the vault."), true},
        {"footnotes", QT_TRANSLATE_NOOP("Settings", "Footnotes view"),
         QT_TRANSLATE_NOOP("Settings", "List the note's footnotes in the right sidebar."), true},
        {"task-states", QT_TRANSLATE_NOOP("Settings", "Checkbox states"),
         QT_TRANSLATE_NOOP("Settings", "Any character in a task's box is a state: [-] cancelled, [/] in progress, "
                                       "[~] dropped, [>] forwarded and more. A click cycles it, a right-click "
                                       "picks one, and ~/.config/noteAppC/tasks.ini styles them."),
         false},
        {"graph", QT_TRANSLATE_NOOP("Settings", "Graph view"),
         QT_TRANSLATE_NOOP("Settings", "Show the links between notes as a graph, and the local graph in the "
                                       "right sidebar."),
         true},
        {"note-history", QT_TRANSLATE_NOOP("Settings", "Note history"),
         QT_TRANSLATE_NOOP("Settings", "Keep every save of a note as a git commit, outside the vault, to "
                                       "browse and restore with \"Show this note's history\"."),
         false},
        {"encrypted-notes", QT_TRANSLATE_NOOP("Settings", "Encrypted notes"),
         QT_TRANSLATE_NOOP("Settings", "Notes kept encrypted on disk (.mdlock) and decrypted only in memory, "
                                       "opened with a password."),
         true},
        {"note-composer", QT_TRANSLATE_NOOP("Settings", "Note composer"),
         QT_TRANSLATE_NOOP("Settings", "Merge a note into another, and extract a selection into a new note."),
         true},
        {"notebooks", QT_TRANSLATE_NOOP("Settings", "Notebooks"),
         QT_TRANSLATE_NOOP("Settings", "Run code blocks through an installed Jupyter kernel, sandboxed, and show "
                                       "their output below them. No kernel starts until you run a block."),
         true},
        {"outgoing-link", QT_TRANSLATE_NOOP("Settings", "Outgoing links"),
         QT_TRANSLATE_NOOP("Settings", "Show the links in this note in the right sidebar."), true},
        {"outline", QT_TRANSLATE_NOOP("Settings", "Outline"),
         QT_TRANSLATE_NOOP("Settings", "Show this note's headings in the right sidebar."), true},
        {"page-preview", QT_TRANSLATE_NOOP("Settings", "Page preview"),
         QT_TRANSLATE_NOOP("Settings", "Hover over a link to a note to see it: with Ctrl held while editing, "
                                       "on its own in reading view."),
         true},
        {"image-annotation", QT_TRANSLATE_NOOP("Settings", "Image annotation"),
         QT_TRANSLATE_NOOP("Settings", "Open images in a tab to highlight, draw on and note them. The image is "
                                       "never changed: the marks are kept in a note beside it."),
         false},
        {"pdf-viewer", QT_TRANSLATE_NOOP("Settings", "PDF viewer"),
         QT_TRANSLATE_NOOP("Settings", "Open PDFs in a tab to read and annotate: highlight, underline, "
                                       "strike out, ink, notes and boxes, saved into the file."),
         false},
        {"random-note", QT_TRANSLATE_NOOP("Settings", "Random note"),
         QT_TRANSLATE_NOOP("Settings", "Open a note picked at random."), true},
        {"properties", QT_TRANSLATE_NOOP("Settings", "Properties view"),
         QT_TRANSLATE_NOOP("Settings", "Show every property in the vault in the right sidebar."), true},
        {"publish", QT_TRANSLATE_NOOP("Settings", "Publish"),
         QT_TRANSLATE_NOOP("Settings", "Export the vault, or its notes marked publish: true, as a static "
                                       "website."),
         true},
        {"sync", QT_TRANSLATE_NOOP("Settings", "Sync"),
         QT_TRANSLATE_NOOP("Settings", "Sync this vault through a server, end-to-end encrypted: the server "
                                       "never sees note contents, file names or tags."),
         true},
        {"slides", QT_TRANSLATE_NOOP("Settings", "Slides"),
         QT_TRANSLATE_NOOP("Settings", "Present a note full screen, one slide per part between --- rules."),
         true},
        {"slash-command", QT_TRANSLATE_NOOP("Settings", "Slash commands"),
         QT_TRANSLATE_NOOP("Settings", "Type \"/\" at the start of a line or after a space to run a command."),
         true},
        {"switcher", QT_TRANSLATE_NOOP("Settings", "Quick switcher"),
         QT_TRANSLATE_NOOP("Settings", "Jump to any note by name with Ctrl+O."), true},
        {"global-search", QT_TRANSLATE_NOOP("Settings", "Search"),
         QT_TRANSLATE_NOOP("Settings", "Search every note from the right sidebar, and run query code "
                                       "blocks."),
         true},
        {"tag-pane", QT_TRANSLATE_NOOP("Settings", "Tags view"),
         QT_TRANSLATE_NOOP("Settings", "Show every tag in the vault in the right sidebar."), true},
        {"templates", QT_TRANSLATE_NOOP("Settings", "Templates"),
         QT_TRANSLATE_NOOP("Settings", "Insert a note from the templates folder."), true},
        {"word-count", QT_TRANSLATE_NOOP("Settings", "Word count"),
         QT_TRANSLATE_NOOP("Settings", "Show the note's word count in the status bar."), true},
        {"word-lookup", QT_TRANSLATE_NOOP("Settings", "Look up words"),
         QT_TRANSLATE_NOOP("Settings", "Right-click a word for its definition from a DICT dictionary "
                                       "server (RFC 2229). The word alone is sent, in plain text, to "
                                       "the first server in ~/.dictrc or /etc/dict/dict.conf that "
                                       "answers, which may be dict.org over the internet. Install "
                                       "dictd with a database (dict-gcide, dict-wn) to stay offline."),
         false},
        {"zk-prefixer", QT_TRANSLATE_NOOP("Settings", "Unique note creator"),
         QT_TRANSLATE_NOOP("Settings", "Create a note named by the date and time, for a Zettelkasten."), true},
        {"workspaces", QT_TRANSLATE_NOOP("Settings", "Workspaces"),
         QT_TRANSLATE_NOOP("Settings", "Save and load named layouts of tabs and splits."), true},
        // Notolith's own: Obsidian has pop-out windows built in, with no switch.
        {"popout-windows", QT_TRANSLATE_NOOP("Settings", "Pop-out windows"),
         QT_TRANSLATE_NOOP("Settings", "Open tabs in windows of their own: right-click a tab, or run "
                                       "\"Open current tab in new window\". Drag a tab out of the window "
                                       "for a new one, or onto a split's edge to tile it."),
         false},
    };
    return list;
}

QString Settings::featureKey(const char *id)
{
    return QStringLiteral("plugins.") + camelOf(QString::fromLatin1(id));
}

Settings Settings::load(const QString &vaultRoot)
{
    Settings settings;
    settings.m_vaultRoot = vaultRoot;

    const QVariantMap stored = readINI(configFile());
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
        if (defaults().contains(it.key()))
            settings.m_user.insert(it.key(), valueOf(it.value().toString(), defaultValue(it.key())));
    }

    // What the app remembers lives apart from what a person sets, so opening a
    // note never rewrites the file they hand-edit.
    const QVariantMap index = readINI(indexFile());
    for (auto it = index.constBegin(); it != index.constEnd(); ++it) {
        if (defaults().contains(it.key()))
            settings.m_index.insert(it.key(), valueOf(it.value().toString(), defaultValue(it.key())));
    }

    settings.m_vault = fromObsidianConfig(vaultRoot);
    return settings;
}

QVariant Settings::value(const QString &key) const
{
    // The user's choice outranks the vault's, which outranks the default.
    if (m_user.contains(key))
        return m_user.value(key);
    if (m_index.contains(key))
        return m_index.value(key);
    if (m_vault.contains(key))
        return coerceToDefaultType(m_vault.value(key), defaultValue(key));
    return defaultValue(key);
}

void Settings::setValue(const QString &key, const QVariant &value)
{
    if (!defaults().contains(key))
        return;
    if (key.startsWith(QLatin1String("state.")))
        m_index.insert(key, value);
    else
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
    QDir().mkpath(themesDirectory());
    QDir().mkpath(templatesDirectory());

    // Only what differs from the compiled-in default, so the file stays short
    // enough to read and a later change of default still reaches people.
    QVariantMap settings;
    for (const QString &key : keys()) {
        if (key.startsWith(QLatin1String("state.")))
            continue;
        if (m_user.contains(key) && m_user.value(key) != defaultValue(key))
            settings.insert(key, m_user.value(key));
    }
    static const QString settingsHeader = QStringLiteral(
        "# Notolith settings.\n"
        "#\n"
        "# Every value here overrides a built-in default. Delete a line to go\n"
        "# back to that default. Comments and anything the app does not know\n"
        "# about are kept when it writes this file.\n"
        "#\n"
        "# What the app remembers for itself - recent vaults and notes - lives\n"
        "# in vaults.ini instead, so this file only changes when you do.\n");
    QSet<QString> known;
    for (const QString &key : keys())
        known.insert(key);
    if (!writeINI(configFile(), settings, known, settingsHeader, error))
        return false;

    QVariantMap index;
    for (auto it = m_index.constBegin(); it != m_index.constEnd(); ++it)
        index.insert(it.key(), it.value());
    static const QString indexHeader = QStringLiteral(
        "# What Notolith remembers. Safe to delete; it fills itself in again.\n");
    return writeINI(indexFile(), index, known, indexHeader, error);
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

std::optional<Settings::NotePosition> Settings::notePosition(const QString &path) const
{
    for (const QString &entry : value(QStringLiteral("state.notePositions")).toStringList()) {
        const QStringList fields = entry.split(kPositionField);
        if (fields.size() == 3 && fields.at(0) == path)
            return NotePosition{fields.at(1).toLongLong(), fields.at(2).toInt()};
    }
    return std::nullopt;
}

void Settings::setNotePosition(const QString &path, qint64 cursor, int scroll)
{
    if (path.isEmpty())
        return;
    QStringList positions = value(QStringLiteral("state.notePositions")).toStringList();
    const QString prefix = path + kPositionField;
    for (qsizetype i = positions.size() - 1; i >= 0; --i) {
        if (positions.at(i).startsWith(prefix))
            positions.removeAt(i);
    }
    positions.prepend(prefix + QString::number(cursor) + kPositionField + QString::number(scroll));
    while (positions.size() > 50)
        positions.removeLast();
    setValue(QStringLiteral("state.notePositions"), positions);
}

QStringList Settings::rememberedNotes() const
{
    QStringList paths;
    for (const QString &entry : value(QStringLiteral("state.notePositions")).toStringList())
        paths.append(entry.section(kPositionField, 0, 0));
    return paths;
}

} // namespace noteAppC
