#pragma once

#include <QList>
#include <QString>

#include "SourceMap.h"

namespace noteAppC {

/// The mapping between what a note says and what live preview shows.
///
/// Live preview hides syntax markers - the `#` of a heading, the `**` of a
/// bold run, the brackets of a wikilink - while the cursor is elsewhere, and
/// reveals them when the cursor enters that block. Both states are the same
/// buffer; only the *display string* differs. Every hit test, cursor move and
/// selection goes through this map, so the buffer never learns about
/// rendering.
///
/// Positions are offsets into the whole document, not into the block, so a
/// map can be built per block and still speak the buffer's coordinates.
class DisplayMap {
public:
    /// One stretch of source that survives into the display string.
    struct Run {
        qsizetype displayStart = 0;
        qsizetype sourceStart = 0;
        qsizetype length = 0;
    };

    DisplayMap() = default;

    /// `hidden` need not be sorted or disjoint; overlaps are merged and
    /// anything outside `block` is ignored.
    static DisplayMap build(const QString &source, const Range &block,
                            const QList<Range> &hidden);

    /// A map that hides nothing, which is what an active block and source
    /// mode both use.
    static DisplayMap identity(const QString &source, const Range &block);

    const QString &text() const { return m_text; }
    const Range &block() const { return m_block; }
    const QList<Run> &runs() const { return m_runs; }
    bool hidesAnything() const;

    /// Display offset -> source offset. Clamped to the block.
    qsizetype toSource(qsizetype displayOffset) const;

    /// Source offset -> display offset. A hidden offset collapses to the
    /// boundary that follows it, which is where a cursor should land when the
    /// text under it disappears.
    qsizetype toDisplay(qsizetype sourceOffset) const;

    bool isHidden(qsizetype sourceOffset) const;

    /// The display range covering a source range, for painting a selection.
    Range toDisplay(const Range &sourceRange) const;

    /// The source moved by `delta` in front of this block: an edit above it.
    /// The display string is unchanged, so a cached layout stays valid.
    void shift(qsizetype delta);

private:
    QString m_text;
    Range m_block;
    QList<Run> m_runs;
};

} // namespace noteAppC
