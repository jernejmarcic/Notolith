// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QByteArray>
#include <QColor>
#include <QList>
#include <QRectF>
#include <QSet>
#include <QSizeF>
#include <QString>

namespace noteAppC {

/// A ```` ```markmap ```` fence as a mind map, the way the Obsidian markmap
/// plugin and markmap.js.org draw one, with no JavaScript: the fence's
/// headings and list items become a tree, laid out left to right with the root
/// at the left, and drawn as an SVG that plugs into the diagram pipeline
/// (MermaidCache.h's DiagramKind::Markmap; MarkmapBlock.cpp in libs/editor is
/// the editor's renderer; publish and PDF export draw the map from the text themselves).
/// Pure functions of their arguments, safe on any thread once a
/// QGuiApplication exists (the text is measured with QFontMetricsF).

/// What the fence's frontmatter can ask (markmap's own option names, found
/// under a `markmap:` key or at the top level).
struct MarkmapOptions {
    /// Levels count from 1 at the root, as markmap.js's do. Nodes below this level share the colour of
    /// their ancestor at it (1: one colour for the whole map); 0 gives every node its own.
    int colorFreezeLevel = 0;
    /// Nodes at this level or deeper start folded, so 1 shows only the root and 2 the root and its
    /// children; -1 expands everything.
    int initialExpandLevel = -1;
    /// Longest a line of node text may be, in pixels, before it wraps; 0 never wraps.
    int maxWidth = 0;
};

/// A stretch of node text in one style (the inline Markdown of the source:
/// bold, italic, `code`, links, ~~strike~~).
struct MarkmapRun {
    QString text;
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool link = false;
    bool strike = false;
    bool operator==(const MarkmapRun &) const = default;
};

struct MarkmapNode {
    QList<MarkmapRun> runs;
    int parent = -1; ///< an index into MarkmapTree::nodes; -1 for the root
    int depth = 0;   ///< the root is 0
    QList<int> children;
    /// Starts folded: `<!-- markmap: fold -->`, `foldAll` on an ancestor, or initialExpandLevel.
    bool folded = false;
    QString plain() const;
};

/// What a source says. `nodes` is in document order, which is pre-order: a
/// node's index is its identity (the editor's fold state names nodes by it),
/// stable for as long as the source is.
struct MarkmapTree {
    MarkmapOptions options;
    QList<MarkmapNode> nodes; ///< nodes[0] is the root
    /// The root is not in the source: it holds several top-level headings or
    /// items, as markmap.js does, and has no text.
    bool virtualRoot = false;
    /// How many nodes the source holds, which is more than nodes.size() when
    /// kMarkmapMaxNodes cut it.
    int total = 0;
};

/// The most nodes drawn; the rest of a bigger outline is left out and the map says so.
/// (A map this big is about 17000 px tall and the editor rasterises at most 8192 px a
/// side (MermaidBlock.cpp's kMaxPixels), so its text comes out soft, not unreadable-sharp:
/// the ceiling that decides the cap, not the layout.)
constexpr int kMarkmapMaxNodes = 1000;

/// Headings nest by level, list items by indentation under the heading above
/// them, and any other text line is a child of the heading (continuation
/// lines of a list item join it). The inline Markdown comes from the
/// application's own parser. A frontmatter block at the top is read for the
/// options, not drawn.
MarkmapTree parseMarkmap(const QString &source);

/// What a map of a whole note is made from, as markmap.js and the Obsidian
/// plugin make one: the note's own text (headings, lists and paragraphs), less
/// its frontmatter and its fenced code blocks, which parseMarkmap would read
/// as headings and items. Empty when nothing is left.
QString markmapSourceOfNote(const QString &note);

/// What a map is drawn in: the theme's tokens, as JSON (MermaidCache.h's
/// diagramConfig carries them).
struct MarkmapLook {
    QColor text{0x22, 0x22, 0x22};
    QColor muted{0x88, 0x88, 0x88};
    QColor background{0xff, 0xff, 0xff};
    QColor accent{0x70, 0x50, 0xd0};
    QList<QColor> palette; ///< the branch colours, in the order nodes take them
    QString family = QStringLiteral("sans-serif");
    QString monoFamily = QStringLiteral("monospace");
    qreal pixelSize = 15;
};
MarkmapLook markmapLook(const QByteArray &config);

/// Where a click folds a node: the circle at its end.
struct MarkmapToggle {
    int node = 0;
    QRectF rect; ///< in the SVG's own units, which are pixels at scale 1
};

struct MarkmapDrawn {
    QByteArray svg;
    QSizeF size;
    QList<MarkmapToggle> toggles; ///< one per drawn node that has children
    int drawn = 0;                ///< nodes on the map
};

/// The map as an SVG. A node is shown folded when `folded` says so, flipped
/// for each id in `toggled` (the view's own folding on top of the source's).
MarkmapDrawn drawMarkmap(const MarkmapTree &tree, const MarkmapLook &look, const QSet<int> &toggled = {});

} // namespace noteAppC
