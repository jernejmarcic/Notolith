// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "Publisher.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QtConcurrent>

#include <algorithm>
#include <atomic>
#include <functional>

#include "CodeHighlighter.h"
#include "FormatColours.h"
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
    /// This folder's own notes plus every descendant's, filled by
    /// buildNavTree() once construction is done - renderNav()'s other
    /// reason to auto-expand a folder besides it being on the active page's
    /// own path, so an ordinary small vault (almost everything one) stays
    /// fully browsable from any page's sidebar and only a folder that is
    /// itself large enough to be the problem ever collapses.
    int totalNotes = 0;
};

/// Below this, a folder always expands in the nav regardless of which page
/// is being rendered - chosen as "obviously fine to ship on every page"
/// (a few hundred links is a normal-sized page), not measured against any
/// particular budget; the flat 4 000-notes-in-one-folder shape the 20 000
/// note benchmark vault uses is well past it either way.
constexpr int kNavAutoExpandNotes = 300;

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

    // Post-order: a folder's total needs every child's total first.
    const std::function<int(NavNode &)> sumTotals = [&](NavNode &node) -> int {
        int total = node.notes.size();
        for (auto it = node.folders.begin(); it != node.folders.end(); ++it)
            total += sumTotals(it.value());
        node.totalNotes = total;
        return total;
    };
    sumTotals(root);
    return root;
}

/// Every "../" a page needs to reach the site root, purely from how many
/// folder levels its own output path has - see renderNav()'s own comment on
/// why hrefs are computed this way instead of Slugs::relativeHref()'s
/// shortest form.
QString rootRelativePrefix(const QString &outputPath)
{
    return QStringLiteral("../").repeated(outputPath.count(QLatin1Char('/')));
}

/// The nav tree for one "../" depth and one *active* folder path (the
/// current page's own folder, "" for the vault root) - every folder on that
/// path expands, every other folder is named but its notes are not emitted
/// at all, not merely hidden by CSS. Without this, every page's sidebar
/// carries a link to every published note in the vault: fine at a few
/// hundred notes, a genuine correctness problem past a few thousand - the
/// first 20 000-note benchmark of this module wrote pages that averaged
/// 1.5 MB each (30 GB for the whole site) purely from this one tree
/// appearing, fully expanded, on every single page. `expandAll` bypasses
/// the collapsing entirely for the one page that is supposed to list
/// everything - the generated site index used when no home note is set.
///
/// Also deliberately *not* Slugs::relativeHref()'s shortest relative path
/// (which depends on how much of the FROM and TO paths' folders are shared,
/// not just their depth): every href here is `upPrefix + note.outputPath`,
/// always correct - a relative URL resolves the same however many redundant
/// "../y/../" pairs it has to walk - just not always the shortest one. That
/// is what makes this cacheable by (depth, active folder) rather than
/// needing a fresh render per page: every page in the same folder, at the
/// same depth, gets byte-identical nav HTML, so a vault needs only one
/// render per distinct folder (not per note) - the difference between the
/// nav costing O(folders) and O(pages^2) at 20 000 notes, see the session
/// report for the measured numbers.
QString renderNav(const NavNode &node, const QString &upPrefix, const QString &activeFolderPath,
                  bool expandAll = false, const QString &pathSoFar = QString())
{
    QString html = QStringLiteral("<ul>");
    for (auto it = node.folders.constBegin(); it != node.folders.constEnd(); ++it) {
        const QString childPath = pathSoFar.isEmpty() ? it.key() : pathSoFar + QLatin1Char('/') + it.key();
        const bool expand = expandAll || it.value().totalNotes <= kNavAutoExpandNotes
            || activeFolderPath == childPath || activeFolderPath.startsWith(childPath + QLatin1Char('/'));
        if (expand) {
            html += QStringLiteral("<li class=\"nav-folder\"><span>%1</span>%2</li>")
                       .arg(escapeHtml(it.key()),
                           renderNav(it.value(), upPrefix, activeFolderPath, expandAll, childPath));
        } else {
            html += QStringLiteral("<li class=\"nav-folder collapsed\"><span>%1</span></li>")
                       .arg(escapeHtml(it.key()));
        }
    }
    for (const ParsedNote *n : node.notes) {
        html += QStringLiteral("<li class=\"nav-note\"><a href=\"%1%2\">%3</a></li>")
                   .arg(upPrefix, n->outputPath, escapeHtml(n->title));
    }
    html += QStringLiteral("</ul>");
    return html;
}

/// target's relativePath -> the relativePath of every published note whose
/// resolved links reach it, built once (see the pre-resolve pass in
/// Publisher::publish()) instead of queried per page through
/// Index::backlinks() - which, like resolve(), is documented same-thread-
/// only, so it could not run from a worker anyway. A pure in-memory lookup
/// here is what lets rendering, backlinks and the file write all happen
/// together in one worker-thread pass below.
using BacklinksMap = QHash<QString, QStringList>;

QString renderBacklinks(const ParsedNote &note, const PublishedNotes &notes, const BacklinksMap &backlinksMap)
{
    const QStringList fromPaths = backlinksMap.value(note.relativePath);
    if (fromPaths.isEmpty())
        return {};
    QStringList items;
    for (const QString &fromPath : fromPaths) {
        const ParsedNote &from = notes.value(fromPath);
        items << QStringLiteral("<li><a href=\"%1\">%2</a></li>")
                    .arg(Slugs::relativeHref(note.outputPath, from.outputPath), escapeHtml(from.title));
    }
    return QStringLiteral("<section class=\"backlinks\"><h2>Linked mentions</h2><ul>%1</ul></section>")
          .arg(items.join(QString()));
}

/// One rendered nav per distinct (depth, active folder) - see renderNav()'s
/// own comment on why the folder matters now, not just the depth.
using NavCache = QHash<QPair<int, QString>, QString>;

/// A page's own containing folder, "" for one at the vault root - what
/// renderNav()'s collapsing keys off, and the cache below.
QString folderOf(const QString &outputPath)
{
    const qsizetype slash = outputPath.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : outputPath.left(slash);
}

QString wrapPage(const QString &title, const QString &outputPath, const QString &contentHtml,
                 const QString &codeStyle, const QString &backlinksHtml,
                 const NavCache &navCache, const QString &currentNavOutputPath, const Options &options)
{
    const QString cssHref = Slugs::relativeHref(outputPath, QStringLiteral("site.css"));
    const QString searchJsHref = Slugs::relativeHref(outputPath, QStringLiteral("search.js"));
    const QString homeHref = Slugs::relativeHref(outputPath, QStringLiteral("index.html"));

    // navCache is filled for every (depth, folder) the site has before
    // rendering starts (see Publisher::publish()) and never written to
    // again, so concurrent reads here from many worker threads are safe
    // without a lock - an empty result would mean that precompute missed a
    // combination, which would be a bug there, not something to paper over
    // with a second render here that a shared QHash cannot safely do off
    // this thread.
    const QString upPrefix = rootRelativePrefix(outputPath);
    const int depth = outputPath.count(QLatin1Char('/'));
    QString navHtml = navCache.value({depth, folderOf(outputPath)});
    if (!currentNavOutputPath.isEmpty()) {
        // One page's own entry, patched in rather than re-rendering the
        // whole tree a second time just to add one CSS class.
        const QString ownHref = upPrefix + currentNavOutputPath;
        navHtml.replace(QStringLiteral("<li class=\"nav-note\"><a href=\"%1\">").arg(ownHref),
                        QStringLiteral("<li class=\"nav-note current\"><a href=\"%1\">").arg(ownHref));
    }

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
        // Formatting colours only where a scheme or the person set one: the
        // rules below fall back to what the page does without them, which
        // `var(--x)` of a variable never declared would not.
        const QHash<QString, QString> set = t.overrides();
        for (const noteAppC::FormatRow &row : noteAppC::formatRows())
            for (const noteAppC::FormatColour &colour : row.colours)
                if (const QString token = QLatin1String(colour.token); set.contains(token))
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
  padding: 0.2em 1em; color: var(--blockquote-color, var(--text-muted)); }
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
.code-block .line.hl-info { background: rgba(76, 141, 255, 0.18); }
.code-block .line.hl-warn { background: rgba(229, 161, 0, 0.2); }
.code-block .line.hl-error, .code-block .line.hl-del { background: rgba(229, 72, 77, 0.18); }
.code-block .line.hl-add { background: rgba(46, 160, 67, 0.2); }
.code-tabs { display: flex; flex-wrap: wrap; margin: 1em 0; position: relative; }
.code-tabs .code-block { margin: 0; }
.code-tab-input { position: absolute; opacity: 0; width: 1px; height: 1px; }
.code-tab-label { order: 0; padding: 4px 12px; cursor: pointer; font-size: 0.85em;
  color: var(--text-muted); border-bottom: 2px solid transparent; }
.code-tab-input:checked + .code-tab-label { color: var(--text-normal);
  border-bottom-color: var(--interactive-accent); }
.code-tab-input:focus-visible + .code-tab-label { outline: 2px solid var(--interactive-accent); }
.code-tab-panel { order: 1; flex: 0 0 100%; display: none; }
.code-tab-input:checked + .code-tab-label + .code-tab-panel { display: block; }
.math-block { text-align: center; margin: 1em 0; overflow-x: auto; }
.diagram { text-align: center; margin: 1em 0; overflow-x: auto; }
.diagram img { max-width: 100%; height: auto; }
.math-block svg, code.math-source { vertical-align: middle; }
.embed { border: 1px solid var(--background-modifier-border); border-radius: 6px;
  padding: 8px 12px; margin: 1em 0; }
.embed-title { font-size: 0.85em; color: var(--text-muted); }
.unresolved { color: var(--link-unresolved-color); text-decoration: line-through dotted; }
.footnote-ref a { text-decoration: none; }
.footnotes { margin-top: 2em; border-top: 1px solid var(--background-modifier-border);
  font-size: 0.9em; color: var(--text-muted); }
.footnote-back { text-decoration: none; }
.footnote-back sup { font-size: 0.7em; }
.tag { background: var(--background-modifier-hover); border-radius: 10px; padding: 0 8px;
  text-decoration: none; font-size: 0.85em; }
.page-title { color: var(--inline-title-color, inherit); }
.note-body h1 { color: var(--h1-color, inherit); }
.note-body h2 { color: var(--h2-color, inherit); }
.note-body h3 { color: var(--h3-color, inherit); }
.note-body h4 { color: var(--h4-color, inherit); }
.note-body h5 { color: var(--h5-color, inherit); }
.note-body h6 { color: var(--h6-color, inherit); }
.note-body strong { color: var(--bold-color, inherit); }
.note-body em { color: var(--italic-color, inherit); }
.note-body del { color: var(--strikethrough-color, inherit);
  text-decoration-color: var(--strikethrough-decoration-color, currentcolor); }
.note-body u { color: var(--underline-color, inherit);
  text-decoration-color: var(--underline-decoration-color, currentcolor); }
.note-body mark { background: var(--text-highlight-bg, Mark); color: var(--text-highlight-fg, MarkText); }
.note-body :not(pre) > code { color: var(--code-normal, inherit); background: var(--code-background, transparent); }
.note-body a { color: var(--link-color, var(--text-accent)); }
.note-body a[href^="http"], .note-body a[href^="mailto:"] {
  color: var(--link-external-color, var(--link-color, var(--text-accent))); }
.note-body a.tag { color: var(--tag-color, var(--link-color, var(--text-accent))); }
.note-body .footnote-ref a { color: var(--footnote-color, var(--link-color, var(--text-accent))); }
.note-body li::marker { color: var(--list-marker-color, inherit); }
.note-body .task-list-item.done { color: var(--checklist-done-color, inherit); }
.note-body input[type=checkbox] { accent-color: var(--checkbox-color, auto); }
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
    // already excludes them. Each result is moved into `notes` below and
    // nowhere else, rather than kept in both `parsed` and `notes` for the
    // rest of the run: doubling every note's text/Metadata/Decorations in
    // memory for a run that never needs the `parsed` copy again is exactly
    // the kind of thing that turns into gigabytes at 20 000 notes. --------
    struct Parsed {
        QString relativePath;
        QString text;
        Metadata metadata;
        Decorations decorations;
        bool ok = false;
    };
    const auto parseOne = [this, &options](const QString &relativePath) -> Parsed {
        static thread_local noteAppC::Parser parser; // Parser: "cheap but not thread safe"
        Parsed out;
        out.relativePath = relativePath;
        Note note;
        QString readError;
        if (!noteAppC::Vault::read(m_vault.absolute(relativePath), note, &readError))
            return out;
        out.text = note.text;
        const auto result = parser.parseAll(out.text, m_vault.absolute(relativePath), options.parse);
        out.metadata = result.metadata;
        out.decorations = result.decorations;
        out.ok = true;
        return out;
    };
    QList<Parsed> parsed = QtConcurrent::blockingMapped(publishedPaths, parseOne);

    // --- output path assignment: permalink property first, default slug
    // otherwise, then a single dedup pass so the guarantee is absolute
    // regardless of where a collision came from. -----------------------
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

    PublishedNotes notes;
    notes.reserve(outputPaths.size());
    qsizetype cursor = 0;
    for (Parsed &p : parsed) {
        if (!p.ok)
            continue;
        ParsedNote note;
        note.relativePath = p.relativePath;
        note.outputPath = outputPaths.at(cursor++);
        const Property *title = findProperty(p.metadata.properties, QStringLiteral("title"));
        note.title = (title && !title->values.isEmpty()) ? title->values.first()
                                                          : QFileInfo(p.relativePath).completeBaseName();
        note.text = std::move(p.text);
        note.metadata = std::move(p.metadata);
        note.decorations = std::move(p.decorations);
        notes.insert(note.relativePath, std::move(note));
    }
    parsed.clear();
    parsed.squeeze(); // the above moved out of every element; drop the now-empty backing array too

    // --- resolve every link once, and invert it into backlinks, both on
    // this thread before any rendering starts. Index::resolve() and
    // Index::backlinks() both touch the QSqlDatabase connection this Index
    // opened, which - like MathRenderer's font loading being tied to the Qt
    // GUI thread - Qt requires stays on the thread that created it. Working
    // this out up front, once, in memory, rather than querying it per link
    // from whichever worker thread is rendering a page is also what makes
    // the render-and-write pass below safe to run in parallel at all. -----
    QHash<QString, QString> resolveCache;
    BacklinksMap backlinksMap;
    for (const ParsedNote &note : notes) {
        QSet<QString> linkedFromThisNote; // dedup: only add this note once per target, however many times it links there
        for (const Link &link : note.metadata.links) {
            if (link.external || link.target.isEmpty())
                continue;
            const QString key = link.target + QLatin1Char('\x01') + note.relativePath;
            auto cached = resolveCache.find(key);
            if (cached == resolveCache.end())
                cached = resolveCache.insert(key, m_index.resolve(link.target, m_vault.absolute(note.relativePath)));
            const QString &resolved = cached.value();
            if (resolved.isEmpty() || resolved == note.relativePath || !notes.contains(resolved))
                continue;
            if (linkedFromThisNote.contains(resolved))
                continue;
            linkedFromThisNote.insert(resolved);
            backlinksMap[resolved].append(note.relativePath);
        }
    }
    for (auto it = backlinksMap.begin(); it != backlinksMap.end(); ++it)
        std::sort(it.value().begin(), it.value().end()); // deterministic order, for a stable incremental diff

    // --- tag -> published notes, needed both for the tag pages themselves
    // and (next) to know every output path's folder depth before rendering
    // starts. ---------------------------------------------------------
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
    QHash<QString, QString> tagOutputPaths; // tag name -> its page's output path, computed once
    for (auto it = tagToPaths.constBegin(); it != tagToPaths.constEnd(); ++it)
        tagOutputPaths.insert(it.key(), Slugs::tagPagePath(it.key()));

    // --- nav: one render per distinct (depth, active folder) combination
    // actually used by some page, every one of them right now rather than
    // lazily - a page rendered on a worker thread below only ever *reads*
    // navCache, which is what makes a plain QHash safe to share without a
    // mutex around every lookup (Qt's own guarantee: concurrent reads of a
    // container nothing is concurrently writing to are safe). See
    // renderNav()'s own comment for why the folder is part of the key at
    // all, not just the depth. --------------------------------------------
    const NavNode nav = buildNavTree(notes);
    NavCache navCache;
    {
        QSet<QPair<int, QString>> combinations{{0, QString()}}; // index.html
        for (const ParsedNote &note : notes) {
            combinations.insert({note.outputPath.count(QLatin1Char('/')), folderOf(note.outputPath)});
        }
        for (const QString &tagOutputPath : std::as_const(tagOutputPaths)) {
            combinations.insert(
                {tagOutputPath.count(QLatin1Char('/')), folderOf(tagOutputPath)});
        }
        for (const QPair<int, QString> &combo : std::as_const(combinations)) {
            navCache.insert(combo,
                            renderNav(nav, QStringLiteral("../").repeated(combo.first), combo.second));
        }
    }

    // --- manifest: loaded before writing starts, so writeIfChanged() can
    // compare every write (pages, attachments, tag pages, assets) against
    // what the previous run left, in one pass rather than a second walk
    // over the output directory afterwards. -------------------------------
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
    QMutex assemblyMutex; // guards every mutable thing below shared across workers
    QSet<QString> allAttachments;
    QJsonArray searchIndex;

    const auto writeIfChanged = [&](const QString &relPath, const QByteArray &bytes) {
        // Called both from worker threads (the merged render-and-write pass)
        // and later from this thread alone (tag pages, assets) - always
        // under assemblyMutex either way, held by the caller in the first
        // case and taken here for the second.
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

    // --- phase B: render, then - maths permitting - finish the page
    // (backlinks, nav, write) right there on the same worker thread instead
    // of collecting every page's full rendered HTML into memory first and
    // writing it all in a second pass afterwards: at 20 000 notes the
    // difference is between peak memory tracking "the site's parsed
    // metadata" and tracking that *plus* every page's complete rendered
    // HTML at once. A note with maths in it cannot finish here - see
    // RenderEnv and MathRenderer's own comments on why that call has to
    // stay on this thread - so its ContentResult is handed to
    // `deferredForMaths` instead and finished below, serially, once
    // rendering is done. Two thread-local highlighters (one per theme mode)
    // mirror Indexer.cpp's own `thread_local Parser` pattern. -------------
    const noteAppC::Theme themeDark =
        options.codeThemeDark.value_or(noteAppC::Theme::builtin(noteAppC::Theme::Mode::Dark));
    const noteAppC::Theme themeLight =
        options.codeThemeLight.value_or(noteAppC::Theme::builtin(noteAppC::Theme::Mode::Light));
    static std::atomic<quint64> publishCount{0};
    const quint64 publishID = ++publishCount;
    const bool bothThemes = options.theme == Options::ThemeMode::Both;
    QList<QPair<QString, ContentResult>> deferredForMaths;
    std::atomic<int> donePages{0};

    const auto finishPage = [&](const ParsedNote &note, ContentResult &content) {
        // Called with assemblyMutex already held.
        for (const QString &attachment : content.attachments)
            allAttachments.insert(attachment);
        const QString backlinks = renderBacklinks(note, notes, backlinksMap);
        const QString page = wrapPage(note.title, note.outputPath, content.html, content.codeStyle,
                                      backlinks, navCache, note.outputPath, options);
        writeIfChanged(note.outputPath, page.toUtf8());
        ++m_counts.pages;
        QJsonObject entry;
        entry[QStringLiteral("title")] = note.title;
        entry[QStringLiteral("href")] = note.outputPath;
        entry[QStringLiteral("text")] = content.plainText.left(20000);
        searchIndex.append(entry);
    };

    const QStringList notePaths = notes.keys();
    const auto renderAndWriteOne = [&](const QString &relativePath) {
        static thread_local noteAppC::CodeHighlighter highlighterDark;
        static thread_local noteAppC::CodeHighlighter highlighterLight;
        // Per publish, not per thread: a pool thread outlives the run, and
        // the next one may be for another scheme.
        static thread_local quint64 readyFor = 0;
        if (readyFor != publishID) {
            highlighterDark.setTheme(themeDark);
            highlighterLight.setTheme(themeLight);
            readyFor = publishID;
        }
        RenderEnv env{notes,
                     resolveCache,
                     highlighterDark,
                     bothThemes ? &highlighterLight : nullptr,
                     themeDark,
                     themeLight,
                     4,
                     options.mindMaps};
        const ParsedNote &note = notes.value(relativePath);
        ContentResult content = PageBody::render(note, env);

        QMutexLocker lock(&assemblyMutex);
        if (!content.formulas.isEmpty()) {
            deferredForMaths.append({relativePath, std::move(content)});
        } else {
            finishPage(note, content);
        }
        if (progress) {
            const int done = donePages.fetch_add(1) + 1;
            if (done == total || done % 500 == 0)
                progress(done, total);
        }
    };
    QtConcurrent::blockingMap(notePaths, renderAndWriteOne);

    // --- maths: on this thread only (see RenderEnv's own comment). The
    // colour is baked into the SVG rather than left as CSS currentColor -
    // MicroTeX paints with a literal QColor - so a "both" site renders for
    // light backgrounds and inverts itself under dark via CSS (site.css),
    // the same trick as an inverted diagram image. Also used below for the
    // home note's second render (index.html), which needs the same
    // resolution and runs on this same thread anyway. ---------------------
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
    for (auto &pair : deferredForMaths) {
        resolveFormulas(pair.second);
        finishPage(notes.value(pair.first), pair.second);
    }
    deferredForMaths.clear();

    if (progress)
        progress(total, total);

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
    for (auto it = tagToPaths.constBegin(); it != tagToPaths.constEnd(); ++it) {
        const QString &tagOutputPath = tagOutputPaths.value(it.key());
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
                                      QString(), navCache, QString(), options);
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
        const QString ownOutputPath = atRoot.outputPath; // the nav's own entry for it - before the override below
        atRoot.outputPath = QStringLiteral("index.html");
        static thread_local noteAppC::CodeHighlighter highlighterDark;
        static thread_local noteAppC::CodeHighlighter highlighterLight;
        highlighterDark.setTheme(themeDark);
        highlighterLight.setTheme(themeLight);
        RenderEnv env{notes, resolveCache, highlighterDark,
                     bothThemes ? &highlighterLight : nullptr, themeDark, themeLight, 4, options.mindMaps};
        ContentResult content = PageBody::render(atRoot, env);
        resolveFormulas(content);
        const QString backlinks = renderBacklinks(atRoot, notes, backlinksMap);
        const QString page = wrapPage(atRoot.title, atRoot.outputPath, content.html, content.codeStyle,
                                      backlinks, navCache, ownOutputPath, options);
        writeIfChanged(QStringLiteral("index.html"), page.toUtf8());
    } else {
        const QString listHtml = QStringLiteral("<h1 class=\"page-title\">%1</h1>%2")
                                     .arg(escapeHtml(options.siteName),
                                         renderNav(nav, QString(), QString(), /*expandAll=*/true));
        const QString page = wrapPage(options.siteName, QStringLiteral("index.html"), listHtml, QString(),
                                      QString(), navCache, QString(), options);
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
