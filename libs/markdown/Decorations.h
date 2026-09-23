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

    /// For Kind::Link on a wikilink to a heading or a block
    /// (`[[Note#Heading]]`, `[[Note#^block-id]]`): the "#" character alone,
    /// in source coordinates. Live preview draws a heading link's "#" as
    /// " > ", which is how Obsidian shows "Note > Heading"; a block reference
    /// is still shown as written. Invalid for a link with no subpath.
    Range anchorHash;
    /// The text right after `anchorHash`: the heading name, or "^block-id"
    /// with the caret kept. Invalid along with `anchorHash` when there is no
    /// subpath.
    Range anchorText;
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
    Range marker;     ///< `[!type|...]+` - hidden in live preview
    /// Where the title sits in the source. A nested callout keeps its title in
    /// the layout rather than having it drawn for it, so the painter needs to
    /// know which characters to style.
    Range titleRange;
    /// How deeply this callout is nested: 0 is a top-level one, 1 is a
    /// callout inside a callout.
    int depth = 0;
    /// Styling written on the callout itself, which noteAppC takes in the
    /// `|` metadata slot Obsidian already parses and ignores - so
    /// `> [!note|bg=#00FF0022] Title` still opens correctly in Obsidian, just
    /// unstyled. The `:::` container form puts the same keys in its braces.
    /// Sorted, so resolving two layers is deterministic. libs/theme's
    /// CalloutStyle knows what the keys mean; the parser only collects them.
    QMap<QString, QString> attributes;
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

/// A fenced code block's own header. CommonMark reads only the info string's
/// first word as the language - `Code::syntax()`, positioned by
/// `syntaxPos()` - which is why a listing still highlights in Obsidian even
/// when noteAppC has written more after it: `` ```python name="Setup
/// script" icon=U+E73C ``. Everything past the language is noteAppC's own,
/// parsed the way a callout's `|` slot and a `:::` container's `{}` already
/// are, so the same override syntax reads the same way everywhere in a note.
struct CodeFenceSpan {
    Range block;      ///< the whole fenced block, both fences included
    Range content;    ///< the listing between the fences
    QString language; ///< as written, case kept
    QMap<QString, QString> attributes; ///< from the rest of the info string
    Range attributesRange; ///< where those attributes sit on the fence line
    /// How deeply this fence is nested: 0 at top level, >0 inside a quote or
    /// callout - the same count `CalloutSpan::depth` uses.
    int depth = 0;
    /// Adjacent fenced blocks that each carry a `tab` attribute join into one
    /// tabbed block. This is the index into `Decorations::codeFences` of the
    /// group's first member, shared by every fence in the group, or -1
    /// outside any group - which is also what a lone `tab` fence gets, since
    /// it has nobody to share a tab strip with.
    int tabGroup = -1;
    /// The `tab` attribute's value, empty for a bare `tab` - the editor then
    /// labels that tab with the language's own style name instead.
    QString tabLabel;
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
    /// Every fenced code block, any depth, sorted by `block.start`. A block
    /// that also carries `#|` options has an entry here as well as in
    /// `codeOptions` - match the two by `content` against `CodeOptions::block`,
    /// which is the same range under a different name.
    QList<CodeFenceSpan> codeFences;
    /// Markers hidden while the cursor is outside the block: `#`, `**`, `[[`,
    /// `==`, fence lines, the target half of an aliased wikilink.
    QList<Range> syntax;
    QList<StyleSpan> styles;

    /// Ranges to hide for the block containing `sourceOffset`, or all of them
    /// when `activeBlock` is not a real offset.
    QList<Range> hiddenFor(const Range &block, qsizetype cursor) const;
};

} // namespace noteAppC
