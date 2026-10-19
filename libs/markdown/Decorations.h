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
        /// A footnote reference `[^label]`/`^[text]`, or a definition's
        /// `[^label]:` marker - `range` is the whole reference, or just the
        /// definition's marker (not its body). Inline, like a link.
        Footnote,
        /// Inline HTML in a block the editor lays out itself
        /// (libs/editor/HtmlEdit.cpp): `range` is the element's text,
        /// `payload` its open tag as written (`<u>`, `<span
        /// style="color:red">`), styled by MarkdownEditor::htmlTagFormat.
        HtmlText,
        /// A tag, or a run of collapsed whitespace, in such a block. Formats
        /// nothing: it only makes the caret reveal it by touching it, the way
        /// an inline element's markers are, rather than by being on its line.
        HtmlTag,
        /// A meta note - `[like this]`, delimiters included - when the
        /// parser was asked for them (ParseOptions). Formats only; the same
        /// ranges are in Decorations::metaNotes.
        MetaNote,
        /// A bare `[text]` - no link, footnote, task or callout - when the
        /// parser was asked for them (ParseOptions::brackets). Formats only.
        BareBracket,
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
    int headerRows = 1;             ///< 0 for an HTML table with no `<th>` row
    /// An HTML `<table>` the editor lays out itself (libs/editor/HtmlEdit.cpp)
    /// rather than a pipe table: `rows` are the text between each cell's
    /// tags, and the two lists below are what its row and column commands
    /// rewrite. Never aligned by TableFormat.
    bool html = false;
    /// html: a plain grid - no rowspan or colspan, every row with `columns`
    /// cells - which is what the row and column handles can reorder.
    bool simple = true;
    QList<Range> rowElements;         ///< html: each `<tr>` through its `</tr>`
    QList<QList<Range>> cellElements; ///< html: each cell, its own tags included
    /// An HTML cell's style= as written: CSS colour text for color and
    /// background (empty when absent), and whether font-weight asks for bold.
    struct CellLook {
        QString color;
        QString background;
        bool bold = false;
    };
    QList<QList<CellLook>> cellLooks; ///< html: parallel to `rows`
};

/// Source that live preview shows as other text: an entity reference (`&amp;`)
/// as the character it names, `<br>` as a line break, a collapsed newline as a
/// space. The caret steps over one as a single character.
struct Substitution {
    Range range;
    QString text;
};

/// An element that holds text in an HTML block the editor lays out itself:
/// a `<p>`, an `<li>`, a heading, a cell, a `<pre>`. Typing in `content`
/// escapes `&`, `<` and `>`; Enter splits a `<p>` or an `<li>`.
struct HtmlTextSpan {
    Range content;  ///< between its tags, a nested list included
    Range openTag;
    QString tag;    ///< the name as written, case kept: "li", "P"
    bool closed = true; ///< written with its end tag
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
/// Stage 13's rest gives Quarto's execution keys real meaning - `eval`,
/// `echo`, `output`, `include`, `cache` - and noteAppC's own `sandbox`,
/// `timeout` and `kernel` (not Quarto keys at all).
/// `warning` and `error` still just fall into `extra` and go no
/// further; they are here only so a Quarto notebook still renders instead of
/// showing its options as the first lines of the listing.
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
    bool eval = true;  ///< `eval: false` - the block never runs, no run control at all
    bool echo = true;  ///< `echo: false` - live preview shows only the run control and
                       ///< output, never the listing; the caret entering the block reveals it
    /// `sandbox: host` - tier 0, opt-in: the kernel runs as the user, unsandboxed.
    /// Anything else (absent, or `bubblewrap`) is the tier-1 default.
    bool sandboxHost = false;
    /// `sandbox: wasm` - the block runs as a WebAssembly module under wasmtime,
    /// stricter than the default; never set together with `sandboxHost`.
    bool sandboxWasm = false;
    /// `output: false` - the block still runs and is cached, its output is
    /// just not shown (Quarto's key). `include: false` is echo and output both.
    bool output = true;
    /// `cache: false` - nothing is kept: a reopened note shows no output and a
    /// run is not written to the cache. Quarto's key, but there it is opt-in.
    bool cache = true;
    int timeoutSeconds = 0; ///< `timeout: 5` - stop a run after 5 s; 0 is the default limit
    /// `kernel:` - empty for the setting's choice, "off" to run with the system's
    /// own tool, "on" for the language's Jupyter kernel shared by the note's
    /// blocks, or a kernelspec's name (`python3`) to pick that one.
    QString kernel;
    /// `implicit: false` - the note's `implicit-<lang>` frontmatter code is not
    /// added to this block when it runs.
    bool implicit = true;
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
    Range marker;      ///< "  - ", "1. ", including a task's "[x] "; after a quote's ">"
    int indent = 0;    ///< nesting level: how many lists it sits inside
    bool ordered = false;
    int number = 0;    ///< as typed
    /// '-', '*' or '+' for a bullet; '.' or ')' after an ordered item's number.
    QChar bullet;
    /// What an ordered item counts as, CommonMark's way: the list's first
    /// number, then one more for each item, whatever was typed after it.
    int shownNumber = 0;
    /// "2.1.3": the counts of the ordered items it sits in, then its own,
    /// for Word-style numbering. Empty for a bullet.
    QString outline;
    bool task = false;
    /// The character in a task's box: ' ', 'x', 'X', and with
    /// ParseOptions::taskStates any other single one. Null when not a task.
    QChar state;
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

/// A footnote reference: `[^label]` with a definition somewhere in the note,
/// or Obsidian's inline `^[text]`.
struct FootnoteSpan {
    Range range;    ///< "[^1]" or "^[text]"
    QString label;  ///< empty for an inline footnote
    Range content;  ///< the definition's text, or the inline text
    int number = 0; ///< 1-based, in the order footnotes are first referenced
};

/// `[^label]: text`, continued by indented lines.
struct FootnoteDefinition {
    QString label;
    Range range;   ///< the whole definition, marker included
    Range content; ///< after "[^label]: "
    /// The number of the footnote that references this label, matching
    /// `FootnoteSpan::number` - 0 if nothing in the note references it, in
    /// which case live preview and reading view leave it as written.
    int number = 0;
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
    /// Sorted by position. A reference to a label no definition names is
    /// not a footnote, as in Obsidian: it stays text.
    QList<FootnoteSpan> footnotes;
    QList<FootnoteDefinition> footnoteDefinitions;
    /// Entity references in Markdown text come from Parser; those in HTML
    /// blocks, `<br>` and collapsed whitespace from the editor's own pass over
    /// them (HtmlEdit.cpp). Disjoint; the editor sorts them by position.
    QList<Substitution> substitutions;
    QList<HtmlTextSpan> HTMLtext;
    /// Every meta note in the note, outermost only, delimiters included,
    /// sorted by position. Empty unless the parse was asked for them.
    QList<Range> metaNotes;
    /// What the word and character counts leave out, sorted, merged and
    /// disjoint: the excluded runs and, if asked, the unrendered syntax
    /// (ParseOptions). Empty when the count is of everything.
    QList<Range> uncounted;

    /// Ranges to hide for the block containing `sourceOffset`, or all of them
    /// when `activeBlock` is not a real offset.
    QList<Range> hiddenFor(const Range &block, qsizetype cursor) const;
};

} // namespace noteAppC
