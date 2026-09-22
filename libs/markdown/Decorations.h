#pragma once

#include <QList>

#include "SourceMap.h"

namespace noteAppC {

/// What live preview does to a stretch of source.
struct StyleSpan {
    enum class Kind {
        Bold,
        Italic,
        Strikethrough,
        InlineCode,
        Link,       ///< the clickable part of a link or wikilink
        Tag,
        Highlight,
        Heading,    ///< `level` carries 1..6
        Quote,
        CodeBlock,
        Math,
        Html,
    };

    Kind kind = Kind::Bold;
    Range range;
    int level = 0;    ///< heading level, or 0
    QString payload;  ///< link target, tag name, code language - kind decides
};

/// One top-level block: the unit the editor lays out, caches a height for,
/// and switches between raw and rendered when the cursor enters it.
struct BlockSpan {
    enum class Kind { Paragraph, Heading, List, Quote, Code, Table, Html, Math, Rule, Frontmatter };

    Kind kind = Kind::Paragraph;
    Range range;
    int level = 0;
};

/// Everything the editor needs that the index does not: which ranges are
/// syntax to hide, what styling the rest gets, and where the blocks are.
struct Decorations {
    QList<BlockSpan> blocks;
    /// Markers hidden while the cursor is outside the block: `#`, `**`, `[[`,
    /// `==`, fence lines, the target half of an aliased wikilink.
    QList<Range> syntax;
    QList<StyleSpan> styles;

    /// Ranges to hide for the block containing `sourceOffset`, or all of them
    /// when `activeBlock` is not a real offset.
    QList<Range> hiddenFor(const Range &block, qsizetype cursor) const;
};

} // namespace noteAppC
