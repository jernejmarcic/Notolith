#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <Qt>

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
    enum class Kind {
        Paragraph,
        Heading,
        List,
        Quote,
        Callout,
        Code,
        Table,
        Html,
        Math,
        Rule,
        Frontmatter,
    };

    Kind kind = Kind::Paragraph;
    Range range;
    int level = 0;
};

/// A table, flattened into what a grid renderer needs: cell source ranges by
/// row and column, and the alignment the delimiter row asked for.
struct TableSpan {
    Range range;
    int columns = 0;
    QList<Qt::Alignment> alignment; ///< per column
    QList<QList<Range>> rows;       ///< rows.first() is the header
};

/// A callout: Obsidian's `> [!note] Title` blockquote, and the `:::` container
/// form from Ideas.md that renders the same way.
struct CalloutSpan {
    Range range;
    QString type;     ///< lower case, as written: note, warning, tip, ...
    QString title;    ///< the text after the marker, or the type capitalised
    bool foldable = false;
    bool collapsed = false;
    Range marker;     ///< `[!type]+` - hidden in live preview
};

/// An `![[image.png]]` or `![alt](image.png)` embed, with the size Obsidian
/// spells in the alias slot (`|300`, `|300x200`).
struct EmbedSpan {
    Range range;
    QString target;
    QString subpath;
    QString alias;
    int width = 0;  ///< 0 means "as wide as it wants, capped by the column"
    int height = 0;
};

/// `#|` option lines at the head of a fenced code block (PLAN.md 16.2), where
/// `#` is whatever the language uses for a line comment.
///
/// noteAppC never runs code, so Quarto's execution keys (`eval`, `echo`,
/// `output`, `warning`, `error`) are read into `extra` and go no further; they
/// are here only so a Quarto notebook still renders instead of showing its
/// options as the first lines of the listing.
struct CodeOptions {
    Range block;   ///< the code between the fences, as the Code BlockSpan has it
    Range range;   ///< the option lines themselves - hidden in live preview
    QString title; ///< `title:`, or `filename:` which Quarto spells instead
    QString caption;
    bool lineNumbers = false;
    int firstLine = 1;    ///< `lines: 5` numbers the listing from 5
    QList<int> highlight; ///< 1-based lines from `highlight: 2,5-7`
    bool wrap = false;
    bool fold = false;
    QMap<QString, QString> extra; ///< every key with no meaning here, as written
};

/// One item of a list, so live preview can hide the source marker, draw a
/// bullet or a checkbox in its place, and indent the text under it.
struct ListItemSpan {
    Range range;
    Range marker;      ///< "  - ", "1. ", including a task's "[x] "
    int indent = 0;    ///< nesting level, from the leading whitespace
    bool ordered = false;
    int number = 0;
    bool task = false;
    bool done = false;
    Range checkbox;    ///< the "[x]" alone, for click-to-toggle
};

/// A LaTeX formula. Obsidian writes display maths between `$$` and inline
/// maths between `$`; both land here with the delimiters recorded separately
/// so live preview can hide them.
struct MathSpan {
    Range range;   ///< including the delimiters
    Range formula; ///< the LaTeX itself
    bool display = false;
};

/// Everything the editor needs that the index does not: which ranges are
/// syntax to hide, what styling the rest gets, and where the blocks are.
struct Decorations {
    QList<BlockSpan> blocks;
    QList<TableSpan> tables;
    QList<CalloutSpan> callouts;
    /// Ranges of every paragraph, nested ones included. A newline inside one
    /// is a soft break - CommonMark joins the lines, and so does Obsidian
    /// unless "Strict line breaks" is on.
    QList<Range> paragraphs;
    QList<EmbedSpan> embeds;
    QList<ListItemSpan> listItems;
    QList<MathSpan> maths;
    /// One entry per fenced code block that carries `#|` options; find a
    /// block's own by matching `block` against its BlockSpan range.
    QList<CodeOptions> codeOptions;
    /// Markers hidden while the cursor is outside the block: `#`, `**`, `[[`,
    /// `==`, fence lines, the target half of an aliased wikilink.
    QList<Range> syntax;
    QList<StyleSpan> styles;

    /// Ranges to hide for the block containing `sourceOffset`, or all of them
    /// when `activeBlock` is not a real offset.
    QList<Range> hiddenFor(const Range &block, qsizetype cursor) const;
};

} // namespace noteAppC
