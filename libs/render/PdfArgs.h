// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>

namespace noteAppC {

/// A PDF (or compiled `.tex`) embed's page arguments: Obsidian's own
/// `#page=`/`#height=` (obsidian.md/help/embeds), extended with a page list,
/// ranges and a layout. Everything after the `#`, keys joined with `&`:
///
///   #page=3                 one page (Obsidian's)
///   #page=2,5,9              several, in the order written
///   #page=3-7                 a range
///   #page=1,3-5,9              mixed
///   #page=all                every page
///   #height=400               Obsidian's box height
///   layout=column|slides|grid  pages stacked, one at a time, or a grid
///   columns=3                  grid's column count
///
/// No `page=` at all is not an error: it is Obsidian's own embedded viewer,
/// the whole document shown one page at a time (`layout=slides`, every
/// page) - see parsePdfEmbedArgs. `layout=column` is the default once pages
/// are actually listed, because a reader who named three pages almost always
/// wants to read them in order, not click through them one at a time.
struct PdfEmbedArgs {
    enum class Layout { Column, Slides, Grid };

    /// True once a `page=` key was seen, `page=all` included - tells "no
    /// arguments at all" apart from "asked for every page", which matters
    /// only for choosing the default layout.
    bool pagesGiven = false;
    bool allPages = false;
    /// 0-based, in the order and with the duplicates the reader wrote
    /// (`1,3-5,9` keeps that order; `1,1` keeps both). Unclamped against any
    /// real page count - resolvePdfPages does that once the document is
    /// open. Empty when pagesGiven is false, or parsing failed.
    QList<int> pages;
    int height = 0; ///< Obsidian's `#height=`, 0 = unset (natural height)
    Layout layout = Layout::Slides; ///< resolved default; see below
    bool layoutGiven = false;
    int columns = 3; ///< `layout=grid&columns=`

    /// Short and human, empty when the fragment parsed cleanly. A bad
    /// argument never crashes the embed: the caller shows this text in the
    /// panel instead of any page.
    QString error;

    bool ok() const { return error.isEmpty(); }
};

/// `subpath` is `EmbedSpan::subpath` verbatim - empty, or starting with `#`.
/// Unknown keys are ignored rather than flagged, so a future Obsidian
/// argument this reads past does not turn into a visible error; a
/// recognised key with a value that does not parse sets `error`.
PdfEmbedArgs parsePdfEmbedArgs(const QString &subpath);

/// Expands `args` against a real page count: `page=all`, or no `page=` key
/// at all, is every page 0..pageCount-1; an explicit list or range is that
/// list with out-of-range pages dropped (never clamped to the last page - a
/// 3-page PDF asked for `#page=1,5` shows page 1 alone, not page 1 twice).
/// Empty when the document has no pages, or nothing requested survived.
QList<int> resolvePdfPages(const PdfEmbedArgs &args, int pageCount);

} // namespace noteAppC
