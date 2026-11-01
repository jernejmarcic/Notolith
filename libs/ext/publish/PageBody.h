// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include "Decorations.h"
#include "Metadata.h"

namespace noteAppC {
class Theme;
class CodeHighlighter;
} // namespace noteAppC

namespace noteAppC::publish {

/// One note, already read and parsed, plus the two facts about it that only
/// exist relative to the whole publish run: where its page will live and
/// whether it asked to be. Built once per published note before rendering
/// starts (Publisher::parsePhase) and never mutated afterwards, which is what
/// lets rendering run across the thread pool against one shared, read-only
/// table - see PublishedNotes.
struct ParsedNote {
    QString relativePath; ///< vault-relative, e.g. "notes/Second note.md"
    QString outputPath;   ///< site-root-relative .html path, assigned before rendering
    QString title;        ///< frontmatter "title", else the filename
    QString text;          ///< full source, LF-normalised (Vault::read())
    Metadata metadata;
    Decorations decorations;
};

/// Every note that will end up on the site, keyed by vault-relative path.
///
/// This *is* the privacy boundary: it is built once, from only the notes
/// Publisher decided to publish, and never grows afterwards. A link or embed
/// target that resolves (Index::resolve()) to a path not in this map is - by
/// construction, not by a second check anyone could forget - either genuinely
/// unresolved or a real note this run chose not to publish; PageBody cannot
/// tell those apart and does not try, so both render identically: plain text,
/// styled unresolved, nothing of the target's content anywhere in the output.
using PublishedNotes = QHash<QString, ParsedNote>;

/// One formula PageBody found but did not render: `html` carries a unique
/// placeholder token in its place instead. MicroTeX loads its font files
/// through QFontDatabase::addApplicationFont() the first time each one is
/// actually used, and that call is not safe off the Qt GUI thread - a static
/// site with maths would otherwise crash the moment two worker threads (or
/// even one, if it is not the main thread) both render a formula. Publisher
/// resolves every page's formulas afterwards, on the thread that called
/// publish(), and substitutes the token for the real SVG - see Publisher.cpp.
struct PendingFormula {
    QString token;
    QString latex;
    qreal pointSize = 16.0;
};

/// What PageBody::render() produced for one note - the piece Publisher
/// assembles the rest of the page (nav, backlinks, search box) around.
struct ContentResult {
    QString html;       ///< the <article>'s inner HTML: title, properties, body
    QString plainText;  ///< visible text only, for the search index
    /// Vault-relative paths of every image/audio/video/PDF this page's body
    /// actually references (embeds only - a bare link to a PDF is not an
    /// embed and copies nothing). Publisher copies exactly this set, which is
    /// what "only what published pages reference" means in practice.
    QStringList attachments;
    /// `.c0 { color: ... }` etc. for this page's highlighted code, already
    /// wrapped in a <style> tag (with a `@media (prefers-color-scheme:
    /// light)` block too when the site theme is "both"); empty when the note
    /// has no fenced code.
    QString codeStyle;
    /// Every formula this page needs rendered - see PendingFormula.
    QList<PendingFormula> formulas;
};

/// What a render needs beyond the note itself: the rest of the published
/// site to link and embed against, and the render engines a page's math and
/// code need. All by reference; the caller (Publisher) owns the lifetime and
/// decides what is safe to share across threads.
///
/// `resolveCache` stands in for a live Index: Index::resolve() touches the
/// QSqlDatabase connection the Index opened on Publisher's own thread, and
/// Qt requires a connection be used only from the thread that created it -
/// the same class of constraint that ruled out calling MathRenderer from a
/// worker thread. Publisher resolves every link once, up front, on its own
/// thread, and hands every worker this read-only table instead - see its
/// own comment on why this doubles as backlinks() too, once resolution is
/// already known rather than queried separately per page.
struct RenderEnv {
    const PublishedNotes &notes;
    /// Key: target + '\x01' + the linking note's own relativePath (resolve()
    /// takes the same target text different ways depending which note is
    /// asking, via fromPath's folder tie-break). Value: the resolved
    /// vault-relative path, or empty for unresolved.
    const QHash<QString, QString> &resolveCache;
    noteAppC::CodeHighlighter &highlighterDark;
    noteAppC::CodeHighlighter *highlighterLight = nullptr; ///< null unless the site is "both"
    const noteAppC::Theme &themeDark;
    const noteAppC::Theme &themeLight;
    int embedDepthLimit = 4;
    bool mindMaps = true; ///< Options::mindMaps
};

/// Renders one note's Markdown to the site's HTML, honouring Obsidian's own
/// syntax (libs/markdown already parsed it - this only walks the result) and
/// noteAppC's own additions (callout styling attributes, colon-fence
/// containers, code fence options). No I/O beyond what RenderEnv already
/// holds in memory: safe to call from any worker thread.
class PageBody {
public:
    static ContentResult render(const ParsedNote &note, const RenderEnv &env);
};

} // namespace noteAppC::publish
