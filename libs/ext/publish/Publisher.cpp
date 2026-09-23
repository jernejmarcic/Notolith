#include "Publisher.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtConcurrent>

#include <algorithm>

#include "CodeHighlighter.h"
#include "Index.h"
#include "MathRenderer.h"
#include "PageBody.h"
#include "Parser.h"
#include "Slugs.h"
#include "Theme.h"
#include "Vault.h"

namespace noteAppC::publish {

namespace {

QString escapeHtml(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '&': out += QLatin1String("&amp;"); break;
        case '<': out += QLatin1String("&lt;"); break;
        case '>': out += QLatin1String("&gt;"); break;
        case '"': out += QLatin1String("&quot;"); break;
        default: out += c;
        }
    }
    return out;
}

const Property *findProperty(const QList<Property> &properties, const QString &key)
{
    for (const Property &p : properties) {
        if (p.key.compare(key, Qt::CaseInsensitive) == 0)
            return &p;
    }
    return nullptr;
}

bool valueIsFalse(const Property &p)
{
    return !p.values.isEmpty() && p.values.first().compare(QLatin1String("false"), Qt::CaseInsensitive) == 0;
}

bool valueIsTrue(const Property &p)
{
    return !p.values.isEmpty() && p.values.first().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
}

QByteArray sha1Hex(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex();
}

/// Every note this run will put on the site, in one recursive tree by
/// folder, sorted - what the sidebar and the "no home note" fallback index
/// both walk. Built once; rendering it per page only changes the hrefs.
struct NavNode {
    QMap<QString, NavNode> folders; // QMap keeps folder names sorted for free
    QList<const ParsedNote *> notes;
};

NavNode buildNavTree(const PublishedNotes &notes)
{
    QList<const ParsedNote *> sorted;
    sorted.reserve(notes.size());
    for (const ParsedNote &n : notes)
        sorted.append(&n);
    std::sort(sorted.begin(), sorted.end(), [](const ParsedNote *a, const ParsedNote *b) {
        return a->relativePath.compare(b->relativePath, Qt::CaseInsensitive) < 0;
    });

    NavNode root;
    for (const ParsedNote *n : std::as_const(sorted)) {
        QStringList parts = n->relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (!parts.isEmpty())
            parts.removeLast(); // the filename itself is not a folder
        NavNode *cursor = &root;
        for (const QString &part : std::as_const(parts))
            cursor = &cursor->folders[part];
        cursor->notes.append(n);
    }
    return root;
}

QString renderNav(const NavNode &node, const QString &currentRelativePath, const QString &fromOutputPath)
{
    QString html = QStringLiteral("<ul>");
    for (auto it = node.folders.constBegin(); it != node.folders.constEnd(); ++it) {
        html += QStringLiteral("<li class=\"nav-folder\"><span>%1</span>%2</li>")
                   .arg(escapeHtml(it.key()), renderNav(it.value(), currentRelativePath, fromOutputPath));
    }
    for (const ParsedNote *n : node.notes) {
        const bool current = n->relativePath == currentRelativePath;
        html += QStringLiteral("<li class=\"nav-note%1\"><a href=\"%2\">%3</a></li>")
                   .arg(current ? QStringLiteral(" current") : QString(),
                       Slugs::relativeHref(fromOutputPath, n->outputPath), escapeHtml(n->title));
    }
    html += QStringLiteral("</ul>");
    return html;
}

QString renderBacklinks(const ParsedNote &note, const PublishedNotes &notes, const noteAppC::Index &index,
                        const noteAppC::Vault &vault)
{
    const QList<Backlink> links = index.backlinks(vault.absolute(note.relativePath));
    QStringList items;
    QSet<QString> seen;
    for (const Backlink &link : links) {
        if (seen.contains(link.fromPath) || !notes.contains(link.fromPath))
            continue; // an unpublished note's link to this page is never listed
        seen.insert(link.fromPath);
        const ParsedNote &from = notes.value(link.fromPath);
        items << QStringLiteral("<li><a href=\"%1\">%2</a></li>")
                    .arg(Slugs::relativeHref(note.outputPath, from.outputPath), escapeHtml(from.title));
    }
    if (items.isEmpty())
        return {};
    return QStringLiteral("<section class=\"backlinks\"><h2>Linked mentions</h2><ul>%1</ul></section>")
          .arg(items.join(QString()));
}

QString wrapPage(const QString &title, const QString &outputPath, const QString &contentHtml,
                 const QString &codeStyle, const QString &backlinksHtml, const NavNode &nav,
                 const QString &currentRelativePath, const Options &options)
{
    const QString cssHref = Slugs::relativeHref(outputPath, QStringLiteral("site.css"));
    const QString searchJsHref = Slugs::relativeHref(outputPath, QStringLiteral("search.js"));
    const QString homeHref = Slugs::relativeHref(outputPath, QStringLiteral("index.html"));
    const QString navHtml = renderNav(nav, currentRelativePath, outputPath);

    QString html = QStringLiteral(
        "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>%1 - %2</title>\n<link rel=\"stylesheet\" href=\"%3\">\n%4</head>\n<body>\n"
        "<div class=\"layout\">\n<nav class=\"sidebar\">\n"
        "<div class=\"site-name\"><a href=\"%5\">%2</a></div>\n"
        "<div class=\"search-box\"><input type=\"search\" id=\"search-input\" "
        "placeholder=\"Search\" autocomplete=\"off\"><ul id=\"search-results\"></ul></div>\n"
        "<div class=\"nav-tree\">%6</div>\n</nav>\n<main>\n<article>%7</article>\n%8</main>\n</div>\n"
        "<script src=\"%9\" defer></script>\n</body>\n</html>\n");
    return html.arg(escapeHtml(title), escapeHtml(options.siteName), cssHref, codeStyle, homeHref,
                    navHtml, contentHtml, backlinksHtml, searchJsHref);
}

QString siteStylesheet(const noteAppC::Theme &dark, const noteAppC::Theme &light, bool includeLight)
{
    // Plain concatenation rather than one QString::arg() chain: past nine
    // placeholders arg() needs %10, %11... and a %1 immediately followed by
    // a literal digit becomes ambiguous with it - not worth the risk for a
    // token list this size that only grows.
    static const QStringList tokens = {
        QStringLiteral("--background-primary"), QStringLiteral("--background-primary-alt"),
        QStringLiteral("--background-secondary"), QStringLiteral("--background-modifier-border"),
        QStringLiteral("--background-modifier-hover"), QStringLiteral("--text-normal"),
        QStringLiteral("--text-muted"), QStringLiteral("--text-faint"), QStringLiteral("--text-accent"),
        QStringLiteral("--text-on-accent"), QStringLiteral("--text-error"),
        QStringLiteral("--link-unresolved-color"), QStringLiteral("--interactive-accent"),
        QStringLiteral("--interactive-accent-hover"),
    };
    auto vars = [](const noteAppC::Theme &t) {
        QString out;
        for (const QString &token : tokens)
            out += token + QLatin1Char(':') + t.value(token) + QLatin1Char(';');
        return out;
    };

    QString css = QStringLiteral(":root{%1}\n").arg(vars(dark));
    if (includeLight) {
        css += QStringLiteral("@media (prefers-color-scheme: light){:root{%1}}\n").arg(vars(light));
    }
    // Layout and typography: theme-agnostic, reads the variables above. Kept
    // small and hand-written rather than reaching for a CSS framework - a
    // static export has no build step to pull one through, and the whole
    // point is that this ships with no other file.
    css += QStringLiteral(R"CSS(
* { box-sizing: border-box; }
body { margin: 0; background: var(--background-primary); color: var(--text-normal);
  font-family: system-ui, -apple-system, "Segoe UI", sans-serif; line-height: 1.6; }
a { color: var(--text-accent); }
.layout { display: flex; min-height: 100vh; }
.sidebar { width: 260px; flex: none; background: var(--background-secondary);
  border-right: 1px solid var(--background-modifier-border); padding: 16px;
  overflow-y: auto; max-height: 100vh; position: sticky; top: 0; }
.site-name { font-weight: 700; margin-bottom: 12px; }
.site-name a { text-decoration: none; color: var(--text-normal); }
.search-box input { width: 100%; padding: 6px 8px; border-radius: 6px;
  border: 1px solid var(--background-modifier-border); background: var(--background-primary);
  color: var(--text-normal); }
#search-results { list-style: none; margin: 4px 0 0; padding: 0; }
#search-results li a { display: block; padding: 4px 0; }
.nav-tree ul { list-style: none; margin: 4px 0; padding-left: 14px; }
.nav-tree > ul { padding-left: 0; }
.nav-folder > span { color: var(--text-muted); font-size: 0.85em; text-transform: uppercase; }
.nav-note a { text-decoration: none; color: var(--text-muted); }
.nav-note.current a { color: var(--text-accent); font-weight: 600; }
main { flex: 1; padding: 32px 40px; max-width: 820px; }
.page-title { margin-top: 0; }
table.properties { border-collapse: collapse; margin-bottom: 24px; font-size: 0.9em; }
table.properties th { text-align: left; color: var(--text-muted); padding: 2px 12px 2px 0; }
table.properties td { padding: 2px 0; }
.note-body table { border-collapse: collapse; margin: 1em 0; }
.note-body th, .note-body td { border: 1px solid var(--background-modifier-border); padding: 6px 10px; }
blockquote { border-left: 3px solid var(--background-modifier-border); margin: 1em 0;
  padding: 0.2em 1em; color: var(--text-muted); }
.callout { border-radius: 6px; border-left: 4px solid rgb(var(--callout-color));
  background: rgba(var(--callout-color), 0.1); margin: 1em 0; padding: 0.6em 1em; }
@media (prefers-color-scheme: light) {
  .callout { border-left-color: rgb(var(--callout-color-light));
    background: rgba(var(--callout-color-light), 0.1); }
}
.callout-title { display: flex; align-items: center; gap: 6px; font-weight: 600; }
.callout-icon svg { width: 16px; height: 16px; vertical-align: -3px; }
.callout-content > *:first-child { margin-top: 0.4em; }
.callout-content > *:last-child { margin-bottom: 0; }
.code-block { margin: 1em 0; }
.code-header { font-size: 0.85em; color: var(--text-muted); padding: 2px 0; }
.code-block pre { background: var(--background-primary-alt); border-radius: 6px;
  padding: 12px; overflow-x: auto; }
.code-block.numbered .line { display: block; }
.code-block.numbered .line::before { content: attr(data-line); display: inline-block;
  width: 2.5em; color: var(--text-faint); user-select: none; }
.code-block .line.hl { background: var(--background-modifier-hover); }
.math-block { text-align: center; margin: 1em 0; overflow-x: auto; }
.math-block svg, code.math-source { vertical-align: middle; }
.embed { border: 1px solid var(--background-modifier-border); border-radius: 6px;
  padding: 8px 12px; margin: 1em 0; }
.embed-title { font-size: 0.85em; color: var(--text-muted); }
.unresolved { color: var(--link-unresolved-color); text-decoration: line-through dotted; }
.tag { background: var(--background-modifier-hover); border-radius: 10px; padding: 0 8px;
  text-decoration: none; font-size: 0.85em; }
.backlinks { margin-top: 40px; border-top: 1px solid var(--background-modifier-border);
  padding-top: 12px; }
.backlinks ul { padding-left: 20px; }
img, video { max-width: 100%; }
)CSS");
    if (includeLight) {
        // Formulas are baked as literal-coloured SVG (MicroTeX paints with a
        // QColor, not CSS currentColor) for a light background, chosen once
        // in Publisher.cpp - inverted back for the dark default the same way
        // a scanned diagram would be, rather than doubling every formula's
        // render cost to produce two colourways of it.
        css += QStringLiteral(".math-block svg { filter: invert(0.92) hue-rotate(180deg); }\n"
                              "@media (prefers-color-scheme: light) { .math-block svg { filter: none; } }\n");
    }
    return css;
}

/// A minimal, dependency-free substring search. Fetches the JSON index lazily
/// (only once the box is actually used) and resolves every result href
/// against the script's own URL, which is what lets one search.js file work
/// from a page at any depth - see PageBody's own relative-href rule.
QString searchScript()
{
    return QStringLiteral(R"JS(
(function () {
  var input = document.getElementById('search-input');
  var results = document.getElementById('search-results');
  if (!input || !results) return;
  var indexUrl = new URL('search-index.json', document.currentScript.src);
  var data = null;
  function withData(cb) {
    if (data) { cb(); return; }
    fetch(indexUrl).then(function (r) { return r.json(); })
      .then(function (j) { data = j; cb(); })
      .catch(function () { data = []; cb(); });
  }
  input.addEventListener('input', function () {
    var q = input.value.trim().toLowerCase();
    results.innerHTML = '';
    if (!q) return;
    withData(function () {
      var matches = data.filter(function (p) {
        return p.title.toLowerCase().indexOf(q) !== -1 || p.text.toLowerCase().indexOf(q) !== -1;
      }).slice(0, 20);
      matches.forEach(function (p) {
        var li = document.createElement('li');
        var a = document.createElement('a');
        a.href = new URL(p.href, indexUrl).href;
        a.textContent = p.title;
        li.appendChild(a);
        results.appendChild(li);
      });
    });
  });
})();
)JS");
}

} // namespace

Publisher::Publisher(const noteAppC::Vault &vault, const noteAppC::Index &index)
    : m_vault(vault)
    , m_index(index)
{
}

bool Publisher::publish(const Options &options, const ProgressCallback &progress, QString *error)
{
    m_counts = {};

    // --- output directory safety -----------------------------------------
    // "The file is the truth: publishing never writes into the vault."
    // A dedicated subfolder of the vault (e.g. <vault>/_site) is fine - the
    // caller named it explicitly, same as any other outputDir - but the
    // vault root itself is refused outright: that is the one path a stray
    // '.'/empty argument could produce by accident, and the one write here
    // could plausibly clobber real notes.
    if (options.outputDir.isEmpty()) {
        if (error) *error = QStringLiteral("no output directory given");
        return false;
    }
    const QString outDir = QDir::cleanPath(QFileInfo(options.outputDir).absoluteFilePath());
    const QString vaultRoot = QDir::cleanPath(m_vault.root());
    if (outDir == vaultRoot) {
        if (error) *error = QStringLiteral("refusing to publish into the vault root itself");
        return false;
    }
    if (!QDir().mkpath(outDir)) {
        if (error) *error = QStringLiteral("could not create %1").arg(outDir);
        return false;
    }

    // --- phase 0: who gets published (cheap - already-indexed properties,
    // no file re-read) ------------------------------------------------------
    const QList<noteAppC::Index::BaseNote> allNotes = m_index.allNotesForBases();
    QStringList publishedPaths;
    for (const auto &n : allNotes) {
        const Property *publishProp = findProperty(n.properties, QStringLiteral("publish"));
        const bool explicitFalse = publishProp && valueIsFalse(*publishProp);
        const bool explicitTrue = publishProp && valueIsTrue(*publishProp);
        const bool include = options.onlyMarked ? explicitTrue : !explicitFalse;
        if (include)
            publishedPaths << n.path;
        else
            ++m_counts.skipped;
    }
    std::sort(publishedPaths.begin(), publishedPaths.end());
    const int total = publishedPaths.size();
    if (progress)
        progress(0, total);

    // --- phase A: parse every published note, in parallel - the only
    // moment an unpublished note's bytes are read at all is never: this list
    // already excludes them. ------------------------------------------------
    struct Parsed {
        QString relativePath;
        QString text;
        Metadata metadata;
        Decorations decorations;
        bool ok = false;
    };
    const auto parseOne = [this](const QString &relativePath) -> Parsed {
        static thread_local noteAppC::Parser parser; // Parser: "cheap but not thread safe"
        Parsed out;
        out.relativePath = relativePath;
        Note note;
        QString readError;
        if (!noteAppC::Vault::read(m_vault.absolute(relativePath), note, &readError))
            return out;
        out.text = note.text;
        const auto result = parser.parseAll(out.text, m_vault.absolute(relativePath));
        out.metadata = result.metadata;
        out.decorations = result.decorations;
        out.ok = true;
        return out;
    };
    const QList<Parsed> parsed = QtConcurrent::blockingMapped(publishedPaths, parseOne);

    // --- output path assignment: permalink property first, default slug
    // otherwise, then a single dedup pass so the guarantee is absolute
    // regardless of where a collision came from. -----------------------
    PublishedNotes notes;
    QStringList outputPaths;
    outputPaths.reserve(parsed.size());
    for (const Parsed &p : parsed) {
        if (!p.ok) {
            ++m_counts.skipped;
            continue;
        }
        QString path;
        if (const Property *permalink = findProperty(p.metadata.properties, QStringLiteral("permalink"))) {
            if (!permalink->values.isEmpty())
                path = Slugs::permalinkOverride(permalink->values.first());
        }
        if (path.isEmpty())
            path = Slugs::defaultPagePath(p.relativePath);
        outputPaths << path;
    }
    Slugs::deduplicate(outputPaths);

    qsizetype cursor = 0;
    for (const Parsed &p : parsed) {
        if (!p.ok)
            continue;
        ParsedNote note;
        note.relativePath = p.relativePath;
        note.outputPath = outputPaths.at(cursor++);
        const Property *title = findProperty(p.metadata.properties, QStringLiteral("title"));
        note.title = (title && !title->values.isEmpty()) ? title->values.first()
                                                          : QFileInfo(p.relativePath).completeBaseName();
        note.text = p.text;
        note.metadata = p.metadata;
        note.decorations = p.decorations;
        notes.insert(note.relativePath, note);
    }

    // --- phase B: render every page, in parallel. Two thread-local
    // highlighters (one per theme mode) mirror Indexer.cpp's own
    // `thread_local Parser` pattern. Maths is deliberately *not* rendered
    // here: MicroTeX loads each font file it needs through
    // QFontDatabase::addApplicationFont() the first time that font is
    // actually used, and Qt requires that call to happen on the GUI thread -
    // a mutex only serialises which worker thread runs it, it does not make
    // it the right thread, and the first formula rendered this way
    // segfaulted immediately in testing. PageBody instead leaves a
    // placeholder token per formula (ContentResult::formulas); they are all
    // resolved below, after this parallel pass, on the thread that called
    // publish(). -----------------------------------------------------------
    const noteAppC::Theme themeDark = noteAppC::Theme::builtin(noteAppC::Theme::Mode::Dark);
    const noteAppC::Theme themeLight = noteAppC::Theme::builtin(noteAppC::Theme::Mode::Light);
    const bool bothThemes = options.theme == Options::ThemeMode::Both;

    QStringList notePaths = notes.keys();
    const auto renderOne = [&](const QString &relativePath) -> QPair<QString, ContentResult> {
        static thread_local noteAppC::CodeHighlighter highlighterDark;
        static thread_local noteAppC::CodeHighlighter highlighterLight;
        static thread_local bool ready = false;
        if (!ready) {
            highlighterDark.setTheme(themeDark);
            highlighterLight.setTheme(themeLight);
            ready = true;
        }
        RenderEnv env{notes,
                     m_index,
                     m_vault,
                     highlighterDark,
                     bothThemes ? &highlighterLight : nullptr,
                     themeDark,
                     themeLight,
                     4};
        return {relativePath, PageBody::render(notes.value(relativePath), env)};
    };
    const QList<QPair<QString, ContentResult>> rendered = QtConcurrent::blockingMapped(notePaths, renderOne);
    QHash<QString, ContentResult> contents;
    contents.reserve(rendered.size());
    for (const auto &pair : rendered)
        contents.insert(pair.first, pair.second);

    if (progress)
        progress(total, total);

    // --- maths, on this thread only (see the comment above). The colour is
    // baked into the SVG rather than left as CSS currentColor - MicroTeX
    // paints with a literal QColor - so a "both" site renders for light
    // backgrounds and inverts itself under dark via CSS (site.css), the same
    // trick as an inverted diagram image. Also used below for the home
    // note's second render (index.html), which needs the same resolution and
    // runs on this same thread anyway. ------------------------------------
    noteAppC::MathRenderer math;
    const QColor mathColour = options.theme == Options::ThemeMode::Dark
                                  ? themeDark.color(QStringLiteral("--text-normal"))
                                  : themeLight.color(QStringLiteral("--text-normal"));
    const auto resolveFormulas = [&](ContentResult &content) {
        for (const PendingFormula &formula : content.formulas) {
            const QString svg = math.renderSvg(formula.latex, formula.pointSize, mathColour, 900);
            const QString replacement = svg.isEmpty()
                ? QStringLiteral("<code class=\"math-source\">%1</code>").arg(escapeHtml(formula.latex))
                : svg;
            content.html.replace(formula.token, replacement);
        }
    };
    for (auto it = contents.begin(); it != contents.end(); ++it)
        resolveFormulas(*it);

    // --- assemble the site: manifest-guarded writes only from here on -----
    const NavNode nav = buildNavTree(notes);
    const QString manifestPath = outDir + QStringLiteral("/noteappc-publish-manifest.json");
    QHash<QString, QByteArray> oldManifest;
    {
        QFile f(manifestPath);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
            for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
                oldManifest.insert(it.key(), it.value().toString().toUtf8());
        }
    }
    QHash<QString, QByteArray> newManifest;

    const auto writeIfChanged = [&](const QString &relPath, const QByteArray &bytes) {
        const QByteArray hash = sha1Hex(bytes);
        newManifest.insert(relPath, hash);
        if (oldManifest.value(relPath) == hash) {
            ++m_counts.unchanged;
            return;
        }
        const QString fullPath = outDir + QLatin1Char('/') + relPath;
        QDir().mkpath(QFileInfo(fullPath).path());
        QFile f(fullPath);
        if (f.open(QIODevice::WriteOnly))
            f.write(bytes);
    };

    QSet<QString> allAttachments;
    QJsonArray searchIndex;
    for (const ParsedNote &note : notes) {
        const ContentResult &content = contents.value(note.relativePath);
        for (const QString &attachment : content.attachments)
            allAttachments.insert(attachment);

        const QString backlinks = renderBacklinks(note, notes, m_index, m_vault);
        const QString page = wrapPage(note.title, note.outputPath, content.html, content.codeStyle,
                                      backlinks, nav, note.relativePath, options);
        writeIfChanged(note.outputPath, page.toUtf8());
        ++m_counts.pages;

        QJsonObject entry;
        entry[QStringLiteral("title")] = note.title;
        entry[QStringLiteral("href")] = note.outputPath;
        entry[QStringLiteral("text")] = content.plainText.left(20000);
        searchIndex.append(entry);
    }

    // --- attachments: copied only because a published page's embed
    // resolved to them (ContentResult::attachments), never speculatively. --
    for (const QString &relativePath : std::as_const(allAttachments)) {
        Note note;
        QString readError;
        const QString absolute = m_vault.absolute(relativePath);
        QFile source(absolute);
        if (!source.open(QIODevice::ReadOnly))
            continue;
        const QByteArray bytes = source.readAll();
        writeIfChanged(QStringLiteral("attachments/") + relativePath, bytes);
        ++m_counts.attachments;
    }

    // --- tag pages ----------------------------------------------------
    QHash<QString, QStringList> tagToPaths;
    for (const ParsedNote &note : notes) {
        QStringList seen;
        for (const Tag &tag : note.metadata.tags) {
            if (seen.contains(tag.name))
                continue;
            seen << tag.name;
            tagToPaths[tag.name].append(note.relativePath);
        }
    }
    for (auto it = tagToPaths.constBegin(); it != tagToPaths.constEnd(); ++it) {
        const QString tagOutputPath = Slugs::tagPagePath(it.key());
        QStringList items;
        QStringList sortedPaths = it.value();
        std::sort(sortedPaths.begin(), sortedPaths.end());
        for (const QString &path : std::as_const(sortedPaths)) {
            const ParsedNote &n = notes.value(path);
            items << QStringLiteral("<li><a href=\"%1\">%2</a></li>")
                        .arg(Slugs::relativeHref(tagOutputPath, n.outputPath), escapeHtml(n.title));
        }
        const QString content = QStringLiteral("<h1 class=\"page-title\">#%1</h1><ul>%2</ul>")
                                    .arg(escapeHtml(it.key()), items.join(QString()));
        const QString page = wrapPage(QStringLiteral("#") + it.key(), tagOutputPath, content, QString(),
                                      QString(), nav, QString(), options);
        writeIfChanged(tagOutputPath, page.toUtf8());
    }

    // --- index.html: the home note re-rendered at the site root so its
    // relative links are correct there too, or a generated list. -----------
    QString homeRelativePath;
    if (!options.homeNote.isEmpty()) {
        const QString resolved = m_index.resolve(options.homeNote);
        if (!resolved.isEmpty() && notes.contains(resolved))
            homeRelativePath = resolved;
    }
    if (!homeRelativePath.isEmpty()) {
        ParsedNote atRoot = notes.value(homeRelativePath);
        atRoot.outputPath = QStringLiteral("index.html");
        static thread_local noteAppC::CodeHighlighter highlighterDark;
        static thread_local noteAppC::CodeHighlighter highlighterLight;
        highlighterDark.setTheme(themeDark);
        highlighterLight.setTheme(themeLight);
        RenderEnv env{notes,    m_index, m_vault,    highlighterDark,
                     bothThemes ? &highlighterLight : nullptr, themeDark, themeLight, 4};
        ContentResult content = PageBody::render(atRoot, env);
        resolveFormulas(content);
        const QString backlinks = renderBacklinks(atRoot, notes, m_index, m_vault);
        const QString page = wrapPage(atRoot.title, atRoot.outputPath, content.html, content.codeStyle,
                                      backlinks, nav, atRoot.relativePath, options);
        writeIfChanged(QStringLiteral("index.html"), page.toUtf8());
    } else {
        const QString listHtml = QStringLiteral("<h1 class=\"page-title\">%1</h1>%2")
                                     .arg(escapeHtml(options.siteName), renderNav(nav, QString(), QStringLiteral("index.html")));
        const QString page = wrapPage(options.siteName, QStringLiteral("index.html"), listHtml, QString(),
                                      QString(), nav, QString(), options);
        writeIfChanged(QStringLiteral("index.html"), page.toUtf8());
    }

    // --- shared assets --------------------------------------------------
    writeIfChanged(QStringLiteral("site.css"), siteStylesheet(themeDark, themeLight, bothThemes).toUtf8());
    writeIfChanged(QStringLiteral("search.js"), searchScript().toUtf8());
    writeIfChanged(QStringLiteral("search-index.json"),
                   QJsonDocument(searchIndex).toJson(QJsonDocument::Compact));

    // --- incremental cleanup: delete only what an earlier run of this same
    // exporter wrote and no longer would. ----------------------------------
    for (auto it = oldManifest.constBegin(); it != oldManifest.constEnd(); ++it) {
        if (newManifest.contains(it.key()))
            continue;
        QFile::remove(outDir + QLatin1Char('/') + it.key());
        ++m_counts.removed;
    }

    QJsonObject manifestObject;
    for (auto it = newManifest.constBegin(); it != newManifest.constEnd(); ++it)
        manifestObject[it.key()] = QString::fromUtf8(it.value());
    QFile manifestFile(manifestPath);
    if (manifestFile.open(QIODevice::WriteOnly))
        manifestFile.write(QJsonDocument(manifestObject).toJson(QJsonDocument::Compact));

    return true;
}

} // namespace noteAppC::publish
