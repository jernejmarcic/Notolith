#include "Settings.h"

#include "ConfigFile.h"
#include "FormatColours.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QUrl>

#include <algorithm>
#include <deque>
#include <utility>

namespace noteAppC {

namespace {

/// Module feature ids are built at runtime; Feature::id is a const char*, so
/// the strings live here for the rest of the process (a deque never moves them).
std::deque<QByteArray> &internedFeatureStrings()
{
    static std::deque<QByteArray> strings;
    return strings;
}

/// Every setting, with the value that applies when nothing else says
/// otherwise. Keys are grouped by the page they appear on.
QVariantMap &defaults()
{
    static QVariantMap map = [] {
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
        {QStringLiteral("editor.foldHeading"), true}, // the arrow on a heading
        {QStringLiteral("editor.foldIndent"), true},  // the arrow on a list item with children
        // Lists: how ordered numbers and bullets are drawn (display only; the file is CommonMark).
        {QStringLiteral("editor.listNumberStyle"), QStringLiteral("plain")}, // or "hierarchical": 1., 1.1., 1.2.
        {QStringLiteral("editor.listNumberSuffix"), QStringLiteral(".")},   // after a drawn number; empty: as written
        {QStringLiteral("editor.bulletGlyphs"), QStringLiteral("-=dash,*=dot,+=diamond")},
        {QStringLiteral("editor.bulletGlyphsNested"), true}, // a turn arrow for nested bullets instead of by marker
        {QStringLiteral("editor.alignTables"), true},
        {QStringLiteral("editor.hoverEditButton"), true},
        {QStringLiteral("editor.tableHandles"), true}, // row/column grips, add bars, cell selection on a drawn table
        {QStringLiteral("editor.remoteReferences"), false}, // a ```reference block or an icon may fetch an http(s) URL: off, nothing touches the network unasked
        {QStringLiteral("editor.codeHighlights"), QStringLiteral("info=blue, warn=orange, error=red, add=green, del=red")},
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

        // Canvas: Obsidian's two snapping toggles.
        {QStringLiteral("canvas.snapToObjects"), true},
        {QStringLiteral("canvas.snapToGrid"), false},

        // Files and links
        {QStringLiteral("files.attachmentFolder"), QString()},
        {QStringLiteral("files.newNoteLocation"), QStringLiteral("root")}, // root, current (the open note's folder) or folder
        {QStringLiteral("files.newNoteFolder"), QString()},               // for "folder"
        {QStringLiteral("encryption.unlockAtStartup"), false}, // false: asked when an encrypted note opens
        {QStringLiteral("encryption.autoLockMinutes"), 15},
        {QStringLiteral("files.sortOrder"), QStringLiteral("nameAsc")}, // nameAsc, nameDesc, modifiedDesc, modifiedAsc
        {QStringLiteral("files.hiddenExtensions"), QStringList()}, // file types the sidebar leaves out; search and links still see them
        {QStringLiteral("files.confirmDelete"), true},
        {QStringLiteral("files.deletedFiles"), QStringLiteral("system")}, // system trash, local (the vault's .trash) or permanent
        {QStringLiteral("files.autosave"), true}, // Obsidian's way: typed is kept
        {QStringLiteral("sync.serverURL"), QString()},
        {QStringLiteral("sync.email"), QString()},
        // A code block bigger than this many bytes of source is not run (a note's own limit never raises it).
        // 0: no limit. Read from the user's file only; a vault cannot set it.
        {QStringLiteral("notebooks.maxRunnableBytes"), 65536},
        // Vaults the person has trusted, as canonical paths. Until then a vault is untrusted: its own hotkeys and
        // feature switches are ignored, and each code block and file open asks. Never read from a vault.
        {QStringLiteral("vault.trusted"), QStringList()},
        // false: each block is a short process of the system's own tool. true: a note's blocks share
        // one Jupyter kernel per language (variables persist, execution counts, rich output) where one is installed.
        {QStringLiteral("notebooks.sharedKernel"), false},
        // `#| sandbox: wasm` blocks run a WebAssembly module in place of the language's interpreter:
        // `python=/path/python.wasm;ruby=/path/ruby.wasm`. A language with no entry is refused there.
        {QStringLiteral("notebooks.wasmModules"), QString()},
        // auto, true or false. auto: OpenGL on a screen with a fractional scale (1.25, 1.67...),
        // where raster cannot move painted pixels on a scroll and repaints the whole note each
        // frame; raster elsewhere, which is faster there. true/false are what older files hold.
        {QStringLiteral("editor.gpuAcceleration"), QStringLiteral("auto")},
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
        // Pandoc export and import (docs/PLUGINS.md): empty path is `pandoc` on PATH, looked for only when a
        // pandoc command is used or the settings are open. Extra arguments are split like a shell would, run by none.
        {QStringLiteral("pandoc.path"), QString()},
        {QStringLiteral("pandoc.extraArguments"), QString()},
        {QStringLiteral("pandoc.referenceDoc"), QString()},
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
    // Formatting colours: empty follows the theme (FormatColours.h).
    for (const FormatRow &row : formatRows())
        for (const FormatColour &colour : row.colours)
            for (const bool dark : {true, false})
                map.insert(formatColourKey(colour, dark), QString());
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
    case QMetaType::Double:
        return text.toDouble();
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

    ensurePrivateDir(QFileInfo(path).absolutePath());
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
    return appDir(XDGroot::Config);
}

QString Settings::configFile()
{
    return QDir(configDirectory()).filePath(QStringLiteral("settings.ini"));
}

QString Settings::indexFile()
{
    return appDir(XDGroot::State, QStringLiteral("vaults.ini"));
}

QString Settings::lockFile()
{
    // The runtime directory is gone with the login session, as a lock held by a process must be. Without
    // one, the lock sits beside the file it guards.
    const QString runtime = appDir(XDGroot::Runtime);
    return runtime.isEmpty() ? indexFile() + QStringLiteral(".lock")
                             : runtime + QStringLiteral("/settings.lock");
}

QString Settings::themesDirectory()
{
    return QDir(configDirectory()).filePath(QStringLiteral("themes"));
}

QString Settings::templatesDirectory()
{
    return QDir(configDirectory()).filePath(QStringLiteral("templates"));
}

QStringList Settings::fileTypes(const QStringList &raw)
{
    static const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
    static const QRegularExpression leading(QStringLiteral("^[*.]+"));
    QStringList out;
    for (const QString &entry : raw) {
        for (const QString &part : entry.split(separators, Qt::SkipEmptyParts)) {
            const QString type = QString(part).remove(leading).toLower();
            if (!type.isEmpty() && !out.contains(type))
                out.append(type);
        }
    }
    return out;
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
    take(out, app, "foldHeading", QStringLiteral("editor.foldHeading"));
    take(out, app, "foldIndent", QStringLiteral("editor.foldIndent"));
    take(out, app, "strictLineBreaks", QStringLiteral("editor.strictLineBreaks"));
    take(out, app, "spellcheck", QStringLiteral("editor.spellcheck"));
    take(out, app, "tabSize", QStringLiteral("editor.tabWidth"));
    take(out, app, "attachmentFolderPath", QStringLiteral("files.attachmentFolder"));
    take(out, app, "newFileLocation", QStringLiteral("files.newNoteLocation"));
    take(out, app, "newFileFolderPath", QStringLiteral("files.newNoteFolder"));
    take(out, app, "alwaysUpdateLinks", QStringLiteral("links.autoUpdate"));
    take(out, app, "promptDelete", QStringLiteral("files.confirmDelete"));
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

static QList<Settings::Feature> &mutableFeatures()
{
    // Obsidian's own names and wording, where it has the feature.
    static QList<Settings::Feature> list = {
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
         QT_TRANSLATE_NOOP("Settings", "Load plugins from ~/.local/share/notolith/plugins, each in its own "
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
        {"csv-embeds", QT_TRANSLATE_NOOP("Settings", "CSV embeds"),
         QT_TRANSLATE_NOOP("Settings", "Show an embedded .csv or .tsv file as a table, read and parsed "
                                       "off the editing thread. Off, it is a plain listing of the file."),
         false},
        {"markmap", QT_TRANSLATE_NOOP("Settings", "Mind maps"),
         QT_TRANSLATE_NOOP("Settings", "Draw markmap code blocks as mind maps, laid out and drawn by Notolith itself "
                                       "(no web view). Off, the block is a plain listing."),
         false},
        {"advanced-code-blocks", QT_TRANSLATE_NOOP("Settings", "Advanced code blocks"),
         QT_TRANSLATE_NOOP("Settings", "Obsidian's Code Styler syntax: title:, ln:, hl:, fold and unwrap on a "
                                       "code block's first line, coloured named highlights, `{python} code` "
                                       "inline code with highlighting, and ```reference blocks that show a file. "
                                       "Off, they are plain text."),
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
                                       "picks one, and ~/.config/notolith/tasks.ini styles them."),
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
        {"pandoc", QT_TRANSLATE_NOOP("Settings", "Pandoc"),
         QT_TRANSLATE_NOOP("Settings", "Export a note as Word, OpenDocument, EPUB, HTML, LaTeX, RTF or slides, and "
                                       "import such files as notes, through the pandoc program, which is installed "
                                       "separately. Nothing runs until you use a pandoc command."),
         false},
        {"sync", QT_TRANSLATE_NOOP("Settings", "Sync"),
         QT_TRANSLATE_NOOP("Settings", "Sync this vault through a server, end-to-end encrypted: the server "
                                       "never sees note contents, file names or tags."),
         true},
        {"share", QT_TRANSLATE_NOOP("Settings", "Share a note"),
         QT_TRANSLATE_NOOP("Settings", "Edit one note live with other people, peer to peer, end-to-end "
                                       "encrypted."),
         false},
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

const QList<Settings::Feature> &Settings::features()
{
    return mutableFeatures();
}

QString Settings::featureKey(const char *id)
{
    return QStringLiteral("plugins.") + camelOf(QString::fromLatin1(id));
}

void Settings::registerDefaults(const QVariantMap &values)
{
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        if (!defaults().contains(it.key()))
            defaults().insert(it.key(), it.value());
}

void Settings::registerFeature(const QString &id, const QString &name, const QString &description)
{
    for (const Feature &feature : features())
        if (id == QLatin1String(feature.id))
            return;
    auto &strings = internedFeatureStrings();
    strings.push_back(id.toUtf8());
    const char *idChars = strings.back().constData();
    strings.push_back(name.toUtf8());
    const char *nameChars = strings.back().constData();
    strings.push_back(description.toUtf8());
    const char *descriptionChars = strings.back().constData();
    mutableFeatures().append({idChars, nameChars, descriptionChars, false});
    defaults().insert(featureKey(idChars), true); // installing the package is the opt-in
}

void Settings::adoptStored()
{
    const QVariantMap stored = readINI(configFile());
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
        if (defaults().contains(it.key()) && !m_user.contains(it.key())) {
            m_user.insert(it.key(), valueOf(it.value().toString(), defaultValue(it.key())));
            m_baseUser.insert(it.key(), m_user.value(it.key()));
        }
    }
    const QVariantMap index = readINI(indexFile());
    for (auto it = index.constBegin(); it != index.constEnd(); ++it) {
        if (defaults().contains(it.key()) && !m_index.contains(it.key())) {
            m_index.insert(it.key(), valueOf(it.value().toString(), defaultValue(it.key())));
            m_baseIndex.insert(it.key(), m_index.value(it.key()));
        }
    }
}

namespace {

/// The user's file as load() reads it: the keys the app knows, typed by their defaults.
QVariantMap readKnown(const QString &path)
{
    QVariantMap out;
    const QVariantMap stored = readINI(path);
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
        if (defaults().contains(it.key()))
            out.insert(it.key(), valueOf(it.value().toString(), Settings::defaultValue(it.key())));
    }
    return out;
}

/// What the app remembers as lists, newest first: how many it keeps, and whether an entry is
/// told by its first field (a note position) or whole.
struct RememberedList {
    const char *key;
    int cap;
    bool byPath;
};
constexpr RememberedList kRemembered[] = {
    {"state.recentVaults", 10, false}, {"state.recentFiles", 10, false}, {"state.notePositions", 50, true}};

/// The merged list, newest first: what this window added or moved to the front (`ours`, the
/// window saving has just acted), then what another window added to `theirs` since `base`, then
/// what both still have, in this window's order, up to the cap - the cap cuts the oldest, never
/// a fresh entry. An entry of `ours` still as `base` had it and gone from `theirs` was removed
/// by another window, so it is not brought back. `touched`: this window opened something, so
/// its first entry leads even when it was first already.
QStringList mergeRemembered(const QStringList &ours, const QStringList &base, const QStringList &theirs,
                            const RememberedList &kind, bool touched)
{
    const auto told = [&kind](const QString &entry) { return kind.byPath ? entry.section(kPositionField, 0, 0) : entry; };
    QStringList merged;
    QSet<QString> seen;
    const auto add = [&](const QString &entry) {
        if (seen.contains(told(entry)))
            return;
        seen.insert(told(entry));
        merged.append(entry);
    };
    for (qsizetype i = 0; i < ours.size(); ++i) {
        if (!base.contains(ours.at(i)) || (touched && i == 0))
            add(ours.at(i));
    }
    for (const QString &entry : theirs) {
        if (!base.contains(entry))
            add(entry);
    }
    for (const QString &entry : ours) {
        if (theirs.contains(entry))
            add(entry);
    }
    while (merged.size() > kind.cap)
        merged.removeLast();
    return merged;
}

/// What is on disk now, with this object's changes since `base` on top.
QVariantMap overlay(QVariantMap disk, const QVariantMap &ours, const QVariantMap &base, const QSet<QString> &touched)
{
    for (auto it = ours.constBegin(); it != ours.constEnd(); ++it) {
        if (base.contains(it.key()) && base.value(it.key()) == it.value() && !touched.contains(it.key()))
            continue;
        const auto *kind = std::find_if(std::begin(kRemembered), std::end(kRemembered),
                                        [&it](const RememberedList &list) { return it.key() == QLatin1String(list.key); });
        if (kind != std::end(kRemembered))
            disk.insert(it.key(), mergeRemembered(it.value().toStringList(), base.value(it.key()).toStringList(),
                                                  disk.value(it.key()).toStringList(), *kind,
                                                  touched.contains(it.key())));
        else
            disk.insert(it.key(), it.value());
    }
    for (auto it = base.constBegin(); it != base.constEnd(); ++it) {
        if (!ours.contains(it.key()))
            disk.remove(it.key()); // reset here
    }
    return disk;
}

} // namespace

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

    settings.m_baseUser = settings.m_user;
    settings.m_baseIndex = settings.m_index;
    settings.m_vault = fromObsidianConfig(vaultRoot);
    // An untrusted vault may switch features off but never on: its core-plugins.json would otherwise
    // turn on, say, notebooks for whoever opens it. Nor may it lower a safety the user has: its app.json
    // could make Delete permanent and silent (files.deletedFiles, files.confirmDelete), and its cssTheme,
    // carried over as the colour scheme's file path, would have the app read any file it names
    // (appearance.scheme: /dev/zero). Those three come only from the user, trusted vault or not.
    if (!settings.isVaultTrusted(vaultRoot)) {
        for (auto it = settings.m_vault.begin(); it != settings.m_vault.end();) {
            if (it.key().startsWith(QLatin1String("plugins.")) && it.value().toBool())
                it = settings.m_vault.erase(it);
            else
                ++it;
        }
        for (const char *key : {"files.deletedFiles", "files.confirmDelete", "appearance.scheme"})
            settings.m_vault.remove(QLatin1String(key));
    }
    return settings;
}

bool Settings::isVaultTrusted(const QString &vaultRoot) const
{
    const QString real = QFileInfo(vaultRoot).canonicalFilePath();
    return !real.isEmpty() && value(QStringLiteral("vault.trusted")).toStringList().contains(real);
}

void Settings::setVaultTrusted(const QString &vaultRoot, bool trusted)
{
    const QString real = QFileInfo(vaultRoot).canonicalFilePath();
    if (real.isEmpty())
        return;
    QStringList list = value(QStringLiteral("vault.trusted")).toStringList();
    list.removeAll(real);
    if (trusted)
        list.append(real);
    setValue(QStringLiteral("vault.trusted"), list);
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

bool Settings::save(QString *error, int lockWaitMs)
{
    ensurePrivateDir(configDirectory());
    ensurePrivateDir(themesDirectory());
    ensurePrivateDir(templatesDirectory());
    ensurePrivateDir(QFileInfo(indexFile()).absolutePath());
    ensurePrivateDir(QFileInfo(lockFile()).absolutePath());

    // Another Notolith process may save between this one's read and its write, and it writes
    // what it merged from the same files: one at a time. Best effort - a lock that cannot be had
    // (a read-only directory fails below anyway) still gets the merge.
    QLockFile lock(lockFile());
    lock.setStaleLockTime(10000); // a save takes about 0.1 ms: 200 ms is long past a live owner
    lock.tryLock(lockWaitMs);

    const QVariantMap user = overlay(readKnown(configFile()), m_user, m_baseUser, {});
    const QVariantMap indexMerged = overlay(readKnown(indexFile()), m_index, m_baseIndex, m_touched);

    // Only what differs from the compiled-in default, so the file stays short
    // enough to read and a later change of default still reaches people.
    QVariantMap settings;
    for (const QString &key : keys()) {
        if (key.startsWith(QLatin1String("state.")))
            continue;
        if (user.contains(key) && user.value(key) != defaultValue(key))
            settings.insert(key, user.value(key));
    }
    static const QString settingsHeader = QStringLiteral(
        "# Notolith settings.\n"
        "#\n"
        "# Every value here overrides a built-in default. Delete a line to go\n"
        "# back to that default. Comments and anything the app does not know\n"
        "# about are kept when it writes this file.\n"
        "#\n"
        "# What the app remembers for itself - recent vaults and notes - lives\n"
        "# in vaults.ini (in the XDG state directory) instead, so this file only\n"
        "# changes when you do.\n");
    QSet<QString> known;
    for (const QString &key : keys())
        known.insert(key);
    if (!writeINI(configFile(), settings, known, settingsHeader, error))
        return false;

    QVariantMap index;
    for (auto it = indexMerged.constBegin(); it != indexMerged.constEnd(); ++it)
        index.insert(it.key(), it.value());
    static const QString indexHeader = QStringLiteral(
        "# What Notolith remembers. Safe to delete; it fills itself in again.\n");
    if (!writeINI(indexFile(), index, known, indexHeader, error))
        return false;

    // This object now holds what the files do. Whatever that changed under it was another window's write,
    // and nothing re-applies it to the live editors and features: the window is told (takeAdoptedKeys()).
    QList<QString> changed = user.keys() + m_user.keys();
    for (const QString &key : std::as_const(changed)) {
        if (m_user.value(key) != user.value(key) && !m_adopted.contains(key))
            m_adopted.append(key);
    }
    m_user = m_baseUser = user;
    m_index = m_baseIndex = indexMerged;
    m_touched.clear();
    return true;
}

QStringList Settings::takeAdoptedKeys()
{
    return std::exchange(m_adopted, {});
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
    // Even when it changed nothing here, an entry named now is the one last used: save() puts it first.
    if (!vaultRoot.isEmpty())
        m_touched.insert(QStringLiteral("state.recentVaults"));
    if (!filePath.isEmpty())
        m_touched.insert(QStringLiteral("state.recentFiles"));
}

std::optional<Settings::NotePosition> Settings::notePosition(const QString &path) const
{
    for (const QString &entry : value(QStringLiteral("state.notePositions")).toStringList()) {
        const QStringList fields = entry.split(kPositionField);
        if (fields.size() < 3 || fields.at(0) != path)
            continue;
        NotePosition position{fields.at(1).toLongLong(), fields.at(2).toInt(), {}};
        for (qsizetype i = 3; i < fields.size(); ++i) {
            // "@<offset>" is the top line; a fold entry starts with 0 or 1, so the two never meet.
            if (fields.at(i).startsWith(QLatin1Char('@')))
                position.top = fields.at(i).mid(1).toLongLong();
            else
                position.folds.append(QUrl::fromPercentEncoding(fields.at(i).toLatin1()));
        }
        return position;
    }
    return std::nullopt;
}

void Settings::setNotePosition(const QString &path, qint64 cursor, int scroll, const QStringList &folds, qint64 top)
{
    if (path.isEmpty())
        return;
    QStringList positions = value(QStringLiteral("state.notePositions")).toStringList();
    const QString prefix = path + kPositionField;
    for (qsizetype i = positions.size() - 1; i >= 0; --i) {
        if (positions.at(i).startsWith(prefix))
            positions.removeAt(i);
    }
    QString entry = prefix + QString::number(cursor) + kPositionField + QString::number(scroll);
    if (top >= 0)
        entry += kPositionField + QLatin1Char('@') + QString::number(top);
    // Fold lines are note text, so they may hold the tab and comma the entry
    // and the list are split on: each goes in percent-encoded.
    for (const QString &fold : folds.first(qMin<qsizetype>(folds.size(), 100)))
        entry += kPositionField + QString::fromLatin1(QUrl::toPercentEncoding(fold));
    positions.prepend(entry);
    while (positions.size() > 50)
        positions.removeLast();
    setValue(QStringLiteral("state.notePositions"), positions);
}

void Settings::renameNotePosition(const QString &oldPath, const QString &newPath)
{
    QStringList positions = value(QStringLiteral("state.notePositions")).toStringList();
    bool changed = false;
    for (QString &entry : positions) {
        const QString path = entry.section(kPositionField, 0, 0);
        if (path == oldPath || path.startsWith(oldPath + QLatin1Char('/'))) {
            entry.replace(0, oldPath.size(), newPath);
            changed = true;
        }
    }
    if (changed)
        setValue(QStringLiteral("state.notePositions"), positions);
}

void Settings::forgetNotePosition(const QString &path)
{
    QStringList positions = value(QStringLiteral("state.notePositions")).toStringList();
    const qsizetype before = positions.size();
    positions.removeIf([&path](const QString &entry) {
        const QString entryPath = entry.section(kPositionField, 0, 0);
        return entryPath == path || entryPath.startsWith(path + QLatin1Char('/'));
    });
    if (positions.size() != before)
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
