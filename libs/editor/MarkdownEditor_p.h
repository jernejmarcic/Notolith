#pragma once

// The editor's private state: everything MarkdownEditor keeps in `d`, and the
// types that state is made of. Included by the editor's own sources only, so a
// change here rebuilds those and nothing else.

#include "MarkdownEditor.h"

#include <QAbstractScrollArea>
#include <QColor>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QKeySequence>
#include <QMovie>
#include <QPageLayout>
#include <QPageSize>
#include <QPair>
#include <QPixmap>
#include <QPointer>
#include <QSizeF>
#include <QTextLayout>
#include <QTimer>
#include <functional>
#include <memory>
#include <span>
#include <optional>
#include <vector>
#include "BulletGlyphs.h"
#include "CodeHighlighter.h"
#include "Lifeline.h"
#include "MathRenderer.h"
#include "FindBar.h"
#include "Decorations.h"
#include "DisplayMap.h"
#include "Parser.h"
#include "CalloutStyle.h"
#include "LanguageStyle.h"
#include "BaseResult.h"
#include "QueryResult.h"
#include "Theme.h"
#include "PdfArgs.h"
#include "TexCompiler.h"

namespace noteAppC {

/// A format property: the colour of a strikethrough line that is not the
/// text's. Qt draws a strikeout in the text's own colour, so such a line is
/// left to paintStrikeLines and the format's strikeout stays off.
inline constexpr int kStrikeLineProperty = QTextFormat::UserProperty + 1;

/// Strikes `format` through: in the theme's text and line colours where set
/// (`--strikethrough-color`, `--strikethrough-decoration-color`), else as Qt does.
void strikeThrough(QTextCharFormat &format, const Theme &theme);
/// Draws the lines `kStrikeLineProperty` asked for over `layout`, drawn at `origin`.
void paintStrikeLines(QPainter &painter, const QTextLayout &layout, const QPointF &origin);

/// The tallest an explicit `height` / `|WxH` may draw an image, in px.
inline constexpr qreal kMaxEmbedHeight = 4096;

/// The size an image embed draws at, given its natural size and the width
/// available: one of `width` / `height` keeps the aspect ratio, both are taken
/// as given, and the result is scaled down together - the ratio kept - to fit
/// `maxWidth` and, when a height was asked for, `kMaxEmbedHeight`. Shared by a
/// sole image block (layoutImage) and every inline cell (buildEmbedCell).
inline QSizeF embedImageSize(const EmbedSpan &embed, const QSize &natural, qreal maxWidth)
{
    qreal width = embed.width > 0 ? embed.width : natural.width();
    qreal height = width * natural.height() / qMax(1, natural.width());
    if (embed.height > 0) {
        height = embed.height;
        if (embed.width <= 0)
            width = height * natural.width() / qMax(1, natural.height());
    }
    qreal scale = 1;
    if (width > maxWidth)
        scale = maxWidth / width;
    if (embed.height > 0 && height * scale > kMaxEmbedHeight)
        scale = kMaxEmbedHeight / height;
    return {width * scale, height * scale};
}

/// Everything modal editing remembers, in the editor because a count or a
/// register belongs to one buffer. The logic is VimEngine, in VimMode.cpp,
/// a short-lived friend built per keystroke.
struct MarkdownEditor::VimState {
    enum class Mode { Normal, Insert, Visual, VisualLine, VisualBlock };
    bool enabled = false;
    Mode mode = Mode::Normal;
    int count = 0;  ///< typed before the operator
    int count2 = 0; ///< typed after it: 2d3w is 6 words
    QChar op;       ///< d, c or y, waiting for its motion
    QChar pending;  ///< g, r, f, F, t or T, waiting for one more key
    QChar cmdKind;  ///< ':', '/' or '?' while that line is being typed
    QString cmdline;
    QString message;
    QString keys;   ///< the command typed so far, for `.`
    QString lastChange; ///< the last change's keys, replayed by `.`
    QString lastInserted;
    bool recordInsert = false;
    bool replaying = false;
    qsizetype goalCol = -1; ///< column j and k aim for
    bool keepGoal = false;
    QString reg;            ///< the unnamed register; not the clipboard
    bool regLinewise = false;
    bool regBlock = false;  ///< the unnamed register holds a column block
    /// a-z, 0-9 and -: text, and whether it is lines (1) or a block (2)
    struct Register {
        QString text;
        int kind = 0;
    };
    QHash<QChar, Register> regs;
    QChar regName;          ///< chosen by "x for the next command
    QHash<QChar, qsizetype> marks; ///< a-z, ' (before the last jump), < and >
    QChar markChar;         ///< the mark ' or ` is going to
    /// A macro key as typed: replayed by sending it to the editor again.
    struct MacroKey {
        int key = 0;
        int modifiers = 0;
        QString text;
    };
    QHash<QChar, QList<MacroKey>> macros;
    QChar recording;        ///< the register q is recording into
    QList<MacroKey> recorded;
    QChar lastMacro;
    int macroDepth = 0;     ///< @ inside a macro: bounded
    bool macroFailed = false; ///< a motion failed while a macro ran: it stops
    bool regOne = false;    ///< the operator's motion (% { } n N) always fills "1
    /// Block-visual insert (I, A, c): the typed text is copied to the
    /// other lines when the insert ends.
    struct BlockInsert {
        bool active = false;
        qsizetype firstLine = 0; ///< offset of the first line's start
        int lines = 0;
        qsizetype column = 0;
        bool append = false;     ///< pad short lines (A) rather than skip them
        bool toEnd = false;      ///< $A: each line's own end
    } block;
    QList<Range> overrideBlocks; ///< the columns a block selection paints
    QString lastSearch;
    bool searchForward = true;
    QChar findKind, findChar; ///< last f/F/t/T, for ; and ,
    qsizetype insertStart = 0;
    bool hold = false; ///< an insert session is one undo step: pushUndo waits
    /// Visual mode's selection as drawn and edited, which is not the
    /// anchor-to-caret span: it includes the character under the caret, or
    /// whole lines. Honoured only while the caret and anchor are where
    /// the mode left them, so a mouse click quietly cancels it.
    Range overrideRange{-1, -1};
    qsizetype overrideCursor = -1;
    qsizetype overrideAnchor = -1;
    QString lastStatus;
};

/// One row of the frontmatter properties table: the key and its type icon
/// on the left, the typed value on the right. Rectangles are relative to
/// the block's own origin, like a table Cell's.
struct MarkdownEditor::PropertyRow {
    int property = 0;     ///< index into `m_properties`
    QRectF rect;          ///< the whole row
    QRectF control;       ///< the checkbox, when the value is one
    QList<QRectF> chips;  ///< one box per item of a list value
    QList<QRectF> removers; ///< each chip's "x", when the list can be changed
    QRectF adder;           ///< the space after the chips that adds an item
};

/// One embedded file inside a block that holds more than one embed,
/// or mixes an embed with running text, drawn inline - see
/// collectTextEmbeds' comment. Built by the same per-kind logic a
/// sole whole-block embed uses, addressed by its own rect instead of
/// the block's single set of fields in BlockView.
struct MarkdownEditor::EmbedCell {
    Range source; ///< the `![[...]]` span, for a click
    QRectF rect;    ///< block-local
    QPixmap image;  ///< a thumbnail (image or a PDF's first requested page)
    QString label;
    QString sizeText;  ///< set for a file-card cell
    bool isFileCard = false;
    bool panel = false; ///< a small filename caption under a PDF thumbnail
    QString openPath;  ///< what a click opens - the resolved file, or a note target
    bool isNote = false; ///< opens through linkActivated rather than QDesktopServices
    QString cacheKey; ///< a note's card, until its drawing lands in this m_fileEmbedCache entry
};

/// One cell of a rendered table.
struct MarkdownEditor::Cell {
    Range source;
    DisplayMap map;
    std::unique_ptr<QTextLayout> layout;
    QRectF rect;
    Qt::Alignment alignment = Qt::AlignLeft;
    int row = 0;
    int column = 0;
    int lead = 0; ///< display characters trimmed off the front of `layout`'s text
    QColor fill;  ///< an HTML cell's background; invalid when it has none
    QColor ink;   ///< an HTML cell's text colour; invalid when it has none
    bool bold = false; ///< an HTML cell's font-weight
    /// Images drawn inline in the cell's text (layoutTable), rects relative
    /// to the cell's text origin.
    std::vector<EmbedCell> embeds;
};

/// One top-level block: the unit that is laid out, cached and revealed.
/// Blocks outside the viewport carry an estimated height and no layout,
/// which is what keeps a 200 kB note responsive.
/// A callout painted inside another block's layout, with the look it
/// resolved to.
struct MarkdownEditor::NestedCallout {
    const CalloutSpan *span = nullptr;
    CalloutStyle style;
};

/// A fold arrow and, folded, its ellipsis, in viewport coordinates.
struct MarkdownEditor::FoldMark {
    qsizetype site = -1; ///< a heading block's start, or a list item's line start
    bool folded = false;
    QRectF arrow;
    QRectF ellipsis; ///< empty unless folded
    qreal rowTop = 0; ///< its first line, for hover
    qreal rowBottom = 0;
};

struct MarkdownEditor::BlockView {
    BlockSpan span;
    DisplayMap map;
    std::unique_ptr<QTextLayout> layout;
    qreal y = 0;
    qreal height = 0;
    bool active = false;
    bool laidOut = false;
    bool heightEstimated = false;
    /// Indices into the sorted decoration lists that touch this block, so
    /// laying one out never walks the whole document.
    int firstStyle = 0;
    int lastStyle = 0;
    int firstSyntax = 0;
    int lastSyntax = 0;
    /// Filled instead of `layout` when a table renders as a grid.
    std::vector<Cell> cells;
    int headerRows = 0;
    /// A wide table's columns, or a listing's longest line, at their natural
    /// width: when the total exceeds the readable column the block scrolls
    /// sideways inside itself, exactly as it does in Obsidian. 0 for a block
    /// that fits. A fence keeps one offset for all its pieces, on the first.
    qreal sideWidth = 0;
    qreal sideScroll = 0;
    /// Set when this block is a callout, so painting knows its colour,
    /// icon and title without looking the span up again.
    const CalloutSpan *callout = nullptr;
    /// The callout's resolved look: the built-in type, then callouts.ini,
    /// then whatever the callout itself says. Resolved before the block is
    /// laid out, because it can change the body font and so the metrics.
    CalloutStyle calloutStyle;
    /// Callouts nested inside this block, outer first. Only the outermost
    /// callout is a block of its own; the rest are painted into this one's
    /// layout, which is why they are resolved before it is built.
    std::vector<NestedCallout> nested;
    /// A fence in a tab group that is not the tab being shown, or the
    /// blank lines between two of them: no height, nothing painted.
    bool tabHidden = false;
    /// Inside a folded heading's section: it keeps its layout and height, so
    /// opening the fold costs nothing, but takes no room and is never painted
    /// or hit.
    bool foldHidden = false;
    /// Where its geometry ends: a hidden block takes no room.
    qreal bottomY() const { return foldHidden ? y : y + height; }
    /// Set on the fence a tab group is showing: it paints the strip.
    /// An index into m_tabGroups, -1 outside any group.
    int tabGroup = -1;
    /// Inside the tab group the caret is in, which shows every fence.
    /// Tracked so the caret crossing a group's edge relays out the whole
    /// group, not only the block it landed in.
    bool editedAsSource = false;
    /// A single listing whose fences the caret has opened. Its source is
    /// plain text like a group's, but it keeps its panel (drawn out over
    /// the two fence lines, editedListingPanel), run control and output.
    bool editedListing = false;
    /// A fence longer than kCodePieceLines is split into pieces of that
    /// many lines, each a block of its own, so an edit relays out one
    /// piece and not the whole listing. The fence's chrome is drawn across
    /// its pieces: header, label and tabs on the first, caption on the
    /// last. A fence that fits, and every other block, is piece 0 of 1.
    int piece = 0;
    int pieces = 1;
    /// Where highlighting stood at the end of this piece, the state the
    /// next one starts from. Valid when it was worked out from this
    /// piece's current text and the end state of the piece before it.
    CodeHighlighter::State codeEnd;
    bool codeEndValid = false;
    /// The caret's block, in a kind that reveals per element: the syntax
    /// ranges the caret has uncovered. Kept so a caret moving within the
    /// block relays it out only when this actually changes.
    QList<Range> revealed;
    /// The word spelling skipped because the caret was in it. Kept so
    /// the caret moving on to another word relays the block out and the
    /// finished one is checked.
    Range caretWord;
    /// Set when this block is a fenced code block carrying `#|` options.
    const CodeOptions *options = nullptr;
    /// Filled instead of `layout` when this is the frontmatter and live
    /// preview draws it as Obsidian's properties table.
    std::vector<PropertyRow> propertyRows;
    /// For each of the block's list items (listItemsIn order): where its
    /// text starts, block-local, and the item it is nested in (-1 for
    /// none). Siblings share one marker column, as wide as their widest
    /// number, so "10." pushes its list's text over rather than into it.
    std::vector<qreal> listTextX;
    std::vector<int> listParent;
    /// Space above the text: a callout's title row, a code block's header
    /// strip, or the air Obsidian puts over a heading.
    qreal textTop = 0;
    /// Showing its header only. Callouts, `:::` containers, `#| fold`
    /// code blocks and the properties table can all be in this state.
    bool folded = false;
    /// Set when the whole block is one image embed.
    QPixmap image;
    QSize imageSize;
    bool imageCentred = false;
    QString imagePath;
    /// The embed's own subpath/fragment (`![[x.canvas#frag]]`'s
    /// `#frag`), empty for none - kept alongside imagePath so a hooked
    /// embed's input handler can be called with the same two strings
    /// its renderer already was.
    QString embedSubpath;
    /// Filename shown in the header strip of a source-file or PDF embed.
    QString embedLabel;
    /// Draw the code panel and header behind this block.
    bool embedPanel = false;
    /// An audio or video embed's file: the block is a player card.
    QString mediaPath;
    bool mediaVideo = false;

    // --- PDF and .tex embeds, PdfEmbedBlock.cpp -----------------------
    /// Set when this block is a PDF, or a compiled `.tex`, page embed -
    /// laid out by pdfArgs instead of the single `image` field above,
    /// which a plain one-page PDF still uses when that is all pdfArgs
    /// asks for (see layoutPdf).
    bool isPdfEmbed = false;
    PdfEmbedArgs pdfArgs;
    QString pdfPath;   ///< the PDF actually shown - a .tex's own compiled output
    qint64 pdfMtime = 0;
    QString pdfError;  ///< a bad argument, a missing file, or a failed compile
    int pdfPageCount = 0;
    /// One requested page. Column and grid lay out every page pdfArgs
    /// resolved to; slides lays out the one currently shown. `image` is
    /// null while its render is still in flight off the UI thread -
    /// painted as a placeholder box sized from `pointSize` until then.
    struct PdfCell {
        int page = 0;
        QRectF rect; ///< block-local
        QPixmap image;
        QSizeF pointSize; ///< the page's own size, in PDF points
    };
    std::vector<PdfCell> pdfCells;
    int pdfSlideIndex = 0;
    int pdfSlideCount = 0;
    QRectF pdfPrevRect, pdfNextRect; ///< slide nav, block-local; null when not slides
    QString pdfCounterText;          ///< "3 / 12"

    // --- file cards and note embeds, FileEmbedBlock.cpp ---------------
    /// Set when this block is a file noteAppC has no richer renderer
    /// for: a small card (icon, name, size) that opens the file on
    /// click - never a binary file's bytes shown as text.
    bool isFileCard = false;
    QString fileCardPath;
    QString fileCardLabel;
    QString fileCardSize;
    QRectF fileCardRect; ///< block-local; the whole card is the click target
    QPixmap fileCardCover; ///< an EPUB's cover, when it has one and it has been read

    std::vector<EmbedCell> embedCells;

    /// Set when this block is a fenced ```query``` block rendered as
    /// search results: paintBlocks and handleBlockClick both skip the
    /// ordinary code-listing chrome (panel, "Copy", tabs) for it.
    bool isQuery = false;
    /// One clickable result row, in block-local coordinates (add `left`
    /// and `top` to place it in the viewport, the way every other rect
    /// on a BlockView already does).
    struct QueryRow {
        QRectF rect;
        QString path;
        qsizetype offset = 0;
    };
    std::vector<QueryRow> queryRows;
    /// The header's two clickable controls, block-local coordinates -
    /// null-sized when the block has no header yet (still searching, an
    /// error, or an empty search - see layoutQuery). queryHeaderAt and
    /// handleBlockClick both read these rather than recomputing them, so
    /// a click always lands exactly where the paint went.
    QRectF queryCollapseRect;
    QRectF querySortRect;
    /// "N results", as last computed - what queryHeaderAt hands back
    /// without re-running a search from a const accessor.
    QString queryResultCountText;

    /// Set when this block is a fenced ```` ```base ```` block rendered
    /// as a table: paintBlocks and handleBlockClick both skip the
    /// ordinary code-listing chrome for it, the same gate isQuery is for
    /// a ```query``` block.
    bool isBase = false;
    /// The shown view's name, painted above the table - empty shows
    /// nothing extra (an error or a still-loading block shows its
    /// message instead of a table; see layoutBase).
    QString baseViewName;
    /// The view requested for this block - a `.base` embed's
    /// `#fragment`, or empty for a fenced ```` ```base ```` block and for
    /// an embed with no fragment. Part of the cache key alongside
    /// baseYAML, and what deliverBaseResults matches a block back on, so
    /// two embeds of the same file asking for different views never
    /// share a result.
    QString baseRequestedView;
    QString baseYAML; ///< what the table shows: a fence's text, or a .base file's
    /// One clickable row's first cell, block-local coordinates - only
    /// the name column opens its note, mirroring
    /// BaseTableModel::isNameColumn's choice for the real widget.
    struct BaseRow {
        QRectF rect;
        QString path;
    };
    std::vector<BaseRow> baseRows;

    /// Set when this block is a raw HTML block (top-level `<div>`,
    /// `<table>`, `<details>`... - not inline HTML inside a paragraph)
    /// rendered by litehtml. It paints through `image` exactly like an
    /// embed, so nothing else needs to know it came from HtmlRenderer
    /// rather than an image file; this flag exists only so the "Copy"
    /// button and tab strip a Code-kind block would otherwise carry
    /// never gets a chance to (HTML blocks are BlockSpan::Kind::Html,
    /// which has no such chrome to begin with - kept for symmetry with
    /// isQuery and in case a future BlockSpan kind needs the same gate).
    bool isHtml = false;
    /// Set when a plugin draws this fence (CodeBlockHook.cpp): no
    /// listing chrome, and codeBlockKey finds it again on delivery.
    bool isPluginBlock = false;
    QString codeBlockKey;

    // --- Jupyter kernel output, NotebookBlock.cpp ----------------------
    /// A fenced code block with a language, shown while the notebooks
    /// feature and setNotebookRunner are both on - unlike isPluginBlock,
    /// this never replaces the listing: the source stays, and any
    /// output goes below it, plus a run control alongside "Copy" in the
    /// header (codeLabelOf, paintCodeChrome, handleBlockClick).
    bool notebookEligible = false;
    QString notebookKey; ///< "language|tier|source" - m_notebookCache's own key
    /// What the run control's click re-runs - exactly the language,
    /// source, working directory and sandbox tier appendNotebookOutput
    /// was last called with, whether that came from a fenced block's own
    /// `#|` options or a runnable `![[x.py]]` embed's file. Kept on the
    /// view instead of re-derived at click time so both paths share the
    /// one run() call in runNotebookBlockAt.
    QString notebookLanguage;
    QString notebookSource;
    QString notebookWorkingDir;
    bool notebookSandboxHost = false;
    bool notebookSandboxWasm = false;
    bool notebookCacheOn = true;
    bool notebookShowOutput = true; ///< `#| output: false` clears it
    int notebookTimeout = 0;
    QString notebookKernel;
    QString notebookImplicit;     ///< the note's `implicit-<lang>` code for this block, "" for none
    QString notebookImplicitMain; ///< ... and its `-main` wrapper
    QString notebookBlockID; ///< "f2" (the third fence) or "e<path>": which block this is across edits
    bool notebookStale = false; ///< showing an earlier run of different source
    int notebookCount = 0;      ///< the kernel's execution count, 0 for none
    bool notebookHasRun = false;
    bool notebookRunning = false;
    bool notebookOk = true;
    /// Top of the output area, below the listing (view.textTop + the
    /// listing's own height + caption); == view.height while there is
    /// no output to show.
    qreal notebookOutputTop = 0;
    /// Where the line edit of a pending input() goes, block-local; empty
    /// when this block is not the one asking (NotebookInput.cpp).
    QRectF notebookInputRect;
    /// One output, in the order the kernel produced it. A Text/Error
    /// cell draws through its own QTextLayout (monospace, ANSI already
    /// stripped by the runner); an ImagePng/ImageSvg/Markdown cell
    /// through `image`, `image` already rendered from raw bytes/SVG/
    /// markdown by appendNotebookOutput so painting never re-decodes it.
    struct NotebookOutputCell {
        NotebookOutput::Kind kind = NotebookOutput::Kind::Text;
        std::unique_ptr<QTextLayout> layout;
        QPixmap image;
        QRectF rect; ///< block-local
    };
    std::vector<NotebookOutputCell> notebookOutputs;
    /// Text runs from inline HTML (`<b>`, `<kbd>`, `<span style=...>`...)
    /// resolved to a QTextCharFormat by layoutInlineHtml - added to
    /// formatsIn's output the way spellcheck underlines are. Source
    /// coordinates; formatsIn converts through `view.map` like every
    /// other entry there.
    std::vector<std::pair<Range, QTextCharFormat>> inlineHtmlFormats;

    /// One inline `$...$` formula resolved while laying this block out.
    /// `QTextLayout` has no notion of an inline object, so the lone "$"
    /// the DisplayMap left visible carries letter spacing equal to the
    /// image's width instead, reserving exactly the room the paint step
    /// draws into once it knows which line that character landed on.
    struct InlineMath {
        qsizetype sourcePos = 0;  ///< the visible "$", in source coordinates
        qsizetype displayPos = 0; ///< the same character, in display coordinates
        QPixmap image;
        QSize imageSize;
    };
    std::vector<InlineMath> inlineMaths;
    /// A heading link's "#", which Obsidian shows as " > " between the
    /// note and the heading. The same trick as inline maths: the "#"
    /// stays, in invisible ink and as wide as the separator, and the paint
    /// step draws the separator over it.
    struct AnchorSeparator {
        qsizetype sourcePos = 0;
        qsizetype displayPos = 0;
        QColor colour;
    };
    std::vector<AnchorSeparator> separators;

    // --- footnotes, FootnoteMarks.cpp ----------------------------------
    /// A footnote reference shown as its number (superscript, accent
    /// colour) or a definition's marker shown as a small muted number -
    /// the same invisible-ink trick as inline maths and the heading
    /// separator above: one source character (the opening "[" or "^")
    /// stays in the display, letter-spaced to the number's width, and
    /// the paint step draws the number over it.
    struct FootnoteMark {
        qsizetype sourcePos = 0;  ///< the visible carrier character
        qsizetype displayPos = 0; ///< the same character, in display coordinates
        QString text;             ///< "1", "12.", ...
        QColor colour;
        bool superscript = false; ///< a reference; false is a definition's label
    };
    std::vector<FootnoteMark> footnoteMarks;
    /// True only for the synthetic block reading view appends after the
    /// note's own: the numbered footnote list Obsidian's reading view
    /// shows at the end. `span.range` is a zero-length range past the
    /// end of the text - there is no source here, so unlike every other
    /// block it keeps its own rows instead of a QTextLayout over view.map,
    /// the way the properties table (PropertyRow) already does.
    bool isFootnotesList = false;
    struct FootnotesListRow {
        std::unique_ptr<QTextLayout> layout; ///< "1.  <first line of the text>"
        QRectF rect;    ///< block-local, the whole row - a click jumps here too
        QRectF backRef; ///< block-local, the "return" glyph
        qsizetype jumpOffset = 0; ///< real source offset a click moves the caret to
    };
    std::vector<FootnotesListRow> footnoteListRows;
};

/// The copy button at the right of a code block's header ("Copy" under the
/// pointer, "Copied" after a click), the thing to click to copy the
/// listing. The block's name is on the left (codeTitleOf). Painting and
/// hit-testing both come from codeLabelOf, so they cannot disagree about
/// where it is.
struct MarkdownEditor::CodeLabel {
    QRectF rect;   ///< in viewport coordinates, icon included
    qreal slotLeft = 0; ///< left edge of the room "Copied" would take, so neighbours never move
    QString text;
    QColor colour;
    QFont font;
    bool visible = false;
};

/// A run of adjacent `tab` fences, shown as one block with a tab strip.
struct MarkdownEditor::TabGroup {
    Range span;    ///< the first fence's start to the last fence's end
    /// Where hiding stops: the end of the last fence's listing. A code
    /// block's listing is its own block, and its fences sit in the gap
    /// blocks around it - so the gap after the last listing, which holds
    /// the closing fence and the blank line before the next paragraph,
    /// has to stay, or the group loses the spacing any code block has.
    qsizetype hiddenEnd = 0;
    int first = 0; ///< index into m_decorations.codeFences
    int count = 0;
};

/// One tab in a group's strip. tabsOf computes these for painting and for
/// clicking both, so the two cannot disagree about where a tab is.
struct MarkdownEditor::TabRect {
    QRectF rect;
    QString text;
    QIcon icon;
    int member = 0;
    bool selected = false;
};

// --- table chrome, TableHandles.cpp -------------------------------------
struct MarkdownEditor::TableChrome {
    qsizetype block = -1; ///< index in m_blocks of the hovered table
    TablePart part = TablePart::None;
    int row = -1;    ///< the row under the pointer (a grip's row)
    int column = -1; ///< the column under the pointer (a grip's column)
    bool operator==(const TableChrome &) const = default;
};

/// A grip being dragged, or pressed and not yet moved (a click on it
/// selects the row or column).
struct MarkdownEditor::TableDrag {
    TablePart part = TablePart::None;
    qsizetype block = -1;
    int from = -1;
    int to = -1; ///< the boundary the drop would land on, or for an add bar how many it adds
    bool moved = false;
    QPoint start;
    qreal origin = 0; ///< an add bar: where the drag began along its axis, in the table's own coordinates
};

/// Cells selected as a rectangle: parser rows and columns, caret in the
/// anchor cell. Valid only while the text, the caret and the table are as
/// it left them, so nothing has to clear it.
struct MarkdownEditor::CellSelection {
    qsizetype tableStart = -1;
    qsizetype caret = -1;
    quint64 version = 0;
    int anchorRow = 0, anchorColumn = 0, headRow = 0, headColumn = 0;
};

struct MarkdownEditor::PdfMeta {
    int count = 0;
    QList<QSizeF> sizes;
};

struct MarkdownEditor::PdfPageCacheEntry {
    QPixmap image;
    bool pending = true;
    quint64 generation = 0;
    bool zoomed = false; ///< a render beyond the cell's width, for the zoom budget
    qint64 bytes = 0;
};

/// A `.tex` compile, cached by the file's own path and mtime - mirrors
/// PdfPageCacheEntry, just for TexCompiler::compile instead of a render.
struct MarkdownEditor::TexCacheEntry {
    TexCompiler::Result result;
    bool pending = true;
    quint64 generation = 0;
};

struct MarkdownEditor::EpubCoverEntry {
    bool pending = false;
    QPixmap cover;
};

struct MarkdownEditor::FileEmbedCacheEntry {
    QImage image;
    bool pending = true;
    quint64 generation = 0;
};

struct MarkdownEditor::CodeBlockCacheEntry {
    QImage image;
    bool pending = true;
    quint64 generation = 0;
};

// --- input() from a running block, NotebookInput.cpp ----------------
/// One prompt at a time: a kernel asks one question at a time.
struct MarkdownEditor::NotebookInputState {
    QString key;     ///< m_notebookCache key of the block that is asking
    quint64 generation = 0;
    QString prompt;
    bool password = false;
    NotebookInputReply reply;
    QPointer<QWidget> edit; ///< a QLineEdit (NotebookInput.cpp); QWidget here so the header needs no QLineEdit
};

struct MarkdownEditor::NotebookCacheEntry {
    NotebookRunResult result;
    bool pending = true;
    bool running = false; ///< a forceRun in flight - the header strip shows this, not `pending`
    quint64 generation = 0;
    /// Image/Markdown/Svg outputs, pre-rendered once in
    /// deliverNotebookResult rather than on every layout - one entry per
    /// `result.outputs`, a null QPixmap for a Text/Error slot. Rescaled
    /// rather than rebuilt when contentWidth() no longer matches
    /// `renderedWidth` (a resize): cheap, unlike re-running a nested
    /// editor over Markdown or re-decoding a PNG.
    QList<QPixmap> renderedImages;
    int renderedWidth = 0;
};

/// The cached entry for `queryText`, starting a search through
/// m_queryRunner when there is none yet. Never blocks: a block shows
/// "Searching..." until deliverQueryResults lands.
struct MarkdownEditor::QueryCacheEntry {
    QueryResults results;
    bool pending = true;
    /// Bumped each time a search for this text is (re)started, so a
    /// delivery from a superseded request - refreshQueries restarted one
    /// while it was still in flight - is recognised as stale and dropped.
    quint64 generation = 0;
};

/// The cached entry for `yamlText`, starting an evaluation through
/// m_baseRunner when there is none yet. Mirrors QueryCacheEntry.
struct MarkdownEditor::BaseCacheEntry {
    BaseTableData results;
    bool pending = true;
    quint64 generation = 0;
};

/// The recognised, paired inline tags in `view`'s block, in source
/// order: what layoutInlineHtml hides, and what revealedSyntax shows
/// while the caret touches the element.
struct MarkdownEditor::InlineHTMLpair {
    Range open;
    Range close;
    QTextCharFormat format;
    /// `<script>`: never hidden, the whole element formatted (a warning).
    bool shown = false;
};

struct MarkdownEditor::HTMLtableEdit {
    qsizetype start = 0;
    qsizetype removed = 0;
    QString text;
};

// --- a rendered table edited in place, TableCells.cpp ------------------
/// The table cell holding `source`, its end included, in live preview.
struct MarkdownEditor::CellRef {
    const TableSpan *table = nullptr;
    int row = -1;
    int column = -1;
    Range range;
    bool valid() const { return table != nullptr; }
};

/// A pipe table as the commands see it: its lines (0 the header, 1 the
/// delimiter row) split into cell text, every row padded to the widest.
struct MarkdownEditor::TableText {
    qsizetype index = -1; ///< in m_decorations.tables
    qsizetype start = 0;  ///< the lines' source span
    qsizetype end = 0;
    QString prefix;       ///< a callout's "> ", copied onto every row
    QList<QStringList> rows;
    int columns = 0;
};

/// A press-drag-release gesture that started on a hooked embed's own
/// image, tracked instead of the normal caret/selection: which block
/// (m_blocks is only ever rebuilt by an edit, never by this drag
/// itself), the point the last Drag event was measured from, and
/// whether it moved past a click - a plain click on a card opens it, a
/// real drag only pans.
struct MarkdownEditor::EmbedDrag {
    qsizetype blockIndex = -1;
    QPoint startPoint;
    QPoint lastPoint;
    bool moved = false;
    bool pdf = false; ///< pans a zoomed PDF page instead of feeding a hooked embed
};

struct MarkdownEditor::Snapshot {
    QString text;
    qsizetype cursor = 0;
};

/// Obsidian's in-note find bar: the matches live here because the editor
/// paints them, the bar is only the field and the buttons.
struct MarkdownEditor::FindState {
    QString pattern;
    bool caseSensitive = false;
    bool regex = false;
    bool valid = true;
    QList<Range> matches; ///< sorted, non-overlapping
    int current = -1;
};

/// The parse runs off the UI thread; a generation counter says whether the
/// result still describes the text in the buffer.
struct MarkdownEditor::ParseJob {
    Decorations decorations;
    QList<Property> properties;
    QList<Heading> headings;
    int words = 0;
    quint64 version = 0;
};

struct MarkdownEditor::Private {
    VimState m_vim;
    mutable QHash<QString, PdfMeta> m_pdfMetaCache; ///< "path|mtime" -> page count and sizes
    QHash<QString, PdfPageCacheEntry> m_pdfPageCache; ///< "path|mtime|page|width" -> its render
    quint64 m_pdfGeneration = 0;
    /// How every worker - a search, a base, a PDF page, a .tex compile, a
    /// hooked file embed - posts back here; ended first in ~MarkdownEditor.
    std::shared_ptr<Lifeline> m_lifeline = std::make_shared<Lifeline>();
    /// A slides-layout embed's current page, by the block's own source start
    /// - like m_queryCollapsed, session-only and never written to the note.
    QHash<qsizetype, int> m_pdfSlideIndex;
    /// The last Previous/Next click per embed, and the clock it was timed by
    /// (replaceable for tests): for a moment the button just used keeps part
    /// of the slide too, so quick repeats survive a drifting pointer.
    struct PdfNavUse {
        qint64 at = 0;
        int direction = 0;
    };
    QHash<qsizetype, PdfNavUse> m_pdfNavLast;
    /// A PDF embed's zoom and pan, by block start like m_pdfSlideIndex and
    /// remapped with it by shiftRanges; empty while nothing is zoomed.
    /// ponytail: one state per embed, so every page of a column or grid
    /// shows the same corner; per-cell state if anyone asks.
    struct PdfZoom {
        qreal zoom = 1;
        QPointF pan; ///< the visible window's top-left, in the unzoomed cell's pixels
    };
    QHash<qsizetype, PdfZoom> m_pdfZoom;
    QTimer m_pdfZoomSettle; ///< the hi-res render waits for the wheel to stop
    /// Zoomed rasters in delivery order, and what they weigh: past the
    /// budget the oldest leave m_pdfPageCache and a page falls back to its
    /// base raster. ponytail: FIFO over zoomed keys only, base rasters stay
    /// unbounded as before; a QCache over the whole cache if they get a budget.
    QList<QString> m_pdfZoomKeys;
    qint64 m_pdfZoomBytes = 0;
    /// Empties the raster cache and the zoom accounting that mirrors it, together:
    /// bytes left behind would shrink the budget for nothing.
    void clearPdfPages()
    {
        m_pdfPageCache.clear();
        m_pdfZoomKeys.clear();
        m_pdfZoomBytes = 0;
    }
    qint64 m_pdfZoomBudget = qint64(128) << 20;
    /// The last press on an unzoomed page: that click reveals the embed's
    /// source, and a double click to zoom puts the caret back from here.
    struct PdfPress {
        QPoint point;
        qsizetype cursor = -1;
        qsizetype anchor = 0;
        int scroll = 0;
    };
    PdfPress m_pdfPress;
    std::function<qint64()> m_pdfNavNow = [] { return QDeadlineTimer::current().deadline(); };
    QHash<QString, TexCacheEntry> m_texCache; ///< "path|mtime" -> its compile
    quint64 m_texGeneration = 0;
    QHash<QString, EpubCoverEntry> m_epubCoverCache;
    /// The notes (canonical paths) this editor is drawing inside, outermost
    /// first; empty for a window's editor. Cycle and depth guard.
    QStringList m_embedAncestors;
    QHash<QString, FileEmbedRenderer> m_fileEmbedRenderers; ///< lowercase suffix -> renderer
    QHash<QString, FileEmbedCacheEntry> m_fileEmbedCache; ///< "path|mtime|subpath|width" -> result
    quint64 m_fileEmbedGeneration = 0;
    /// setFileEmbedInputHandler's own table, keyed the same way
    /// m_fileEmbedRenderers is.
    QHash<QString, FileEmbedInputHandler> m_fileEmbedInputHandlers;
    QHash<QString, CodeBlockRenderer> m_codeBlockRenderers; ///< lowercase language -> renderer
    QHash<QString, CodeBlockCacheEntry> m_codeBlockCache; ///< "language|width|theme|source" -> result
    quint64 m_codeBlockGeneration = 0;
    NotebookRunner m_notebookRunner;
    NotebookRunnable m_notebookRunnable;
    NotebookInputState m_notebookInput;
    // --- completion and documentation from a running kernel, NotebookComplete.cpp
    NotebookIntrospector m_notebookIntrospector;
    QCompleter *m_notebookCompleter = nullptr;   ///< built on first use
    QWidget *m_notebookInspect = nullptr;        ///< built on first use
    quint64 m_introGeneration = 0;               ///< a reply for an older request is dropped
    quint64 m_introVersion = 0;                  ///< m_textVersion and caret the request was made at
    qsizetype m_introCursor = 0;
    qsizetype m_introFrom = 0;                   ///< absolute range the open popup replaces
    qsizetype m_introTo = 0;
    QHash<QString, NotebookCacheEntry> m_notebookCache; ///< "language|source" -> result
    quint64 m_notebookGeneration = 0;
    /// blockID -> the cache key it last had output under, so a block whose
    /// source changed can still show that output, marked stale. In memory
    /// only, like the cache above: a closed note shows what the sidecar has.
    QHash<QString, QString> m_notebookBlockKeys;
    Features m_features;
    /// A .base file's text, read again only when its mtime moves.
    QHash<QString, QPair<qint64, QString>> m_baseFiles;
    int m_layoutWidth = -1; ///< contentWidth() at the last invalidateLayouts()
    mutable QHash<int, qreal> m_lineHeights; ///< memoised per block kind and level
    bool m_applyingExternal = false; ///< applyEdit for another view: no caret move, no edited()
    QRect m_caretPainted; ///< last caret rectangle drawn, for the blink's update
    qreal m_touchSlop = 0;         ///< extra hit margin while a touch press is handled
    bool m_touchHandles = false;   ///< the selection came from a finger, so it has handles
    int m_draggingHandle = 0;      ///< -1 the start handle, 1 the end one, 0 none
    Theme m_theme = Theme::builtin(Theme::Mode::Dark);
    /// `~/.config/noteAppC/callouts.ini` and `codeblocks.ini`, reread the
    /// moment either changes on disk - they are the way a person restyles
    /// these, so an edit has to show without a restart.
    CalloutStyles m_calloutStyles = CalloutStyles::load();
    LanguageStyles m_languageStyles = LanguageStyles::load();
    bool m_complexCodeBlocks = true;
    quint64 m_layoutCount = 0;
    SyntaxStyles m_themeSyntax; ///< m_theme's `syntax` layer, rebuilt in setTheme
    QFileSystemWatcher *m_configWatcher = nullptr;
    Parser m_parser;
    CodeHighlighter m_code;
    /// Plays audio and video embeds; made on first use, one file at a time.
    class MediaPlayback *m_media = nullptr;
    /// The link a preview was asked for, so the signals fire on changes only.
    QString m_previewLink;
    /// A key combination, as QKeyCombination::toCombined(), to the command
    /// it runs.
    QHash<int, QString> m_keymap;
    Spelling m_spelling;
    ContextActions m_contextActions;
    /// The inline editor over a property value, while one is open, and the
    /// key it edits: a reparse can renumber the properties under it.
    QPointer<QLineEdit> m_propertyEdit;
    /// The editor over a callout's title, and the source offset its callout
    /// starts at (a reparse can renumber the callouts, not move that).
    QPointer<QLineEdit> m_calloutTitleEdit;
    qsizetype m_calloutTitleAt = -1;
    QString m_editedKey;
    /// What the open editor writes: the scalar value (kEditValue), a new list
    /// item (kEditNewItem), or list item n.
    int m_editedItem = -1;
    /// Where the last updateLayout() moved blocks from, in document y; +inf
    /// when nothing moved. A scroll can reuse the pixels on screen only if
    /// nothing on screen moved.
    qreal m_movedFrom = 0;
    MathRenderer m_math;
    AttachmentSaver m_attachmentSaver;
    FileImporter m_fileImporter;
    mutable QHash<QString, QPixmap> m_images;
    /// What is known of a GIF file, valid while its mtime is (GIFplayer.cpp).
    struct GIFinfo {
        bool known = false;
        qint64 mtime = 0;
        bool animated = false; ///< more than one frame
        bool broken = false;   ///< a movie of it failed: shown as its first frame from now on
        QPixmap ended;         ///< the last frame of a GIF that played out, which it keeps showing
    };
    /// Playing GIF movies by file, and what is known of each GIF file seen. Both
    /// stay empty for a note with no animated GIF.
    mutable QHash<QString, std::shared_ptr<QMovie>> m_GIFs;
    mutable QHash<QString, GIFinfo> m_GIFinfo;
    /// The rect of the paint event being drawn, viewport coordinates; null
    /// outside one (an export paints everything).
    QRectF m_exposed;
    mutable int m_tableCellsPainted = 0; ///< cells the last paintTable drew, for the tests
    Mode m_mode = Mode::LivePreview;
    QHash<QString, QueryCacheEntry> m_queryCache; ///< query text -> its last search
    quint64 m_queryGeneration = 0;
    /// Debounces refreshQueries(): Index::changed can fire once per save, and
    /// a burst of them should cost one re-search per block, not one each.
    QTimer *m_queryRefreshTimer = nullptr;
    /// A query block's collapsed/sort-by-name state, by its own source start
    /// offset - like m_folds, session-only and never written to the note,
    /// and orphaned (harmlessly - it just falls back to the default) if an
    /// edit earlier in the note shifts the block's offset.
    QHash<qsizetype, bool> m_queryCollapsed;
    QHash<qsizetype, bool> m_queryNameSort;
    QHash<QString, BaseCacheEntry> m_baseCache; ///< yamlText[+view] -> its last evaluation
    quint64 m_baseGeneration = 0;
    /// Debounces refreshBases(), the same reason m_queryRefreshTimer does.
    QTimer *m_baseRefreshTimer = nullptr;
    QString m_text;
    SourceMap m_lines; ///< line starts, for the gutter
    Decorations m_decorations; ///< styles and syntax kept sorted by start
    mutable QHash<QString, bool> m_unresolved; ///< link target -> has no note
    QList<Property> m_properties; ///< frontmatter, for the properties table
    QList<Heading> m_headings; ///< as of the last parse, for the outline pane
    std::vector<BlockView> m_blocks; // move-only: QTextLayout is not copyable
    /// Fold state by block start offset. A block with no entry is in the state
    /// its source asked for, so folding survives a reparse without the
    /// decorations having to carry it.
    QHash<qsizetype, bool> m_folds;
    /// A setFoldState() that arrived before the pool parse it needs; applied
    /// when the parse of the current text is adopted.
    QStringList m_pendingFolds;
    /// Settings > Editor: heading sections and indented lists fold.
    bool m_foldHeading = true;
    bool m_foldIndent = true;
    /// The fold site (heading start, list item's line start) the pointer is
    /// over, and the viewport rect that repaints when that changes.
    qsizetype m_hoveredFold = -1;
    QRect m_foldHoverRect;
    mutable qreal m_blockSpacing = 0; ///< memoised, cleared with the theme
    qsizetype m_cursor = 0;
    qsizetype m_anchor = 0;
    QList<MarkdownEditor::RemoteCaret> m_remoteCarets;
    qsizetype m_activeBlock = -1;
    Range m_caretCell{-1, -1}; ///< the table cell the caret was last in, to relay out on leaving it
    bool m_alignTables = true;
    bool m_tableEdited = false; ///< the caret's table was edited; aligned when the caret leaves it
    bool m_formattingTables = false; ///< formatTables' own edits do not count as editing
    bool m_tableHandles = true;
    TableChrome m_tableHover;
    TableDrag m_tableDrag;
    CellSelection m_cellSel;
    qsizetype m_cellPress = -1; ///< table start of a press in a cell, for a drag across cells
    int m_cellPressRow = -1;
    int m_cellPressColumn = -1;
    bool m_cellPressTouch = false; ///< that press was a finger's: a drag scrolls, unless a long press armed it
    bool m_cellArmed = false;      ///< a long press in a cell: the finger now drags a rectangle of cells
    QTimer m_tableScrollTimer;
    QPoint m_tableScrollPoint;
    qreal m_goalX = -1; ///< remembered column for up/down
    /// A freshly opened note renders whole; the block under the caret only
    /// shows its source once the reader actually edits or clicks into it,
    /// which is how Obsidian behaves.
    bool m_interacted = false;
    bool m_modified = false;
    bool m_readable = true;
    bool m_lineNumbers = true; ///< shown in source mode only
    bool m_strictLineBreaks = false; ///< Obsidian's app.json setting
    ParseOptions m_parseOptions = [] { ParseOptions o; o.brackets = true; return o; }();
    bool m_indentGuides = false;
    bool m_outlineNumbering = false;
    QString m_listNumberSuffix = QStringLiteral(".");
    BulletGlyphMap m_bulletGlyphs;
    bool m_nestedBulletGlyphs = true;
    int m_tabWidth = 4;
    bool m_useWikilinks = true;
    bool m_GPUacceleration = false;
    /// The left button is held: what is revealed stays exactly as it was
    /// until release, so a drag never selects across text that just moved.
    bool m_pressing = false;
    /// Where the last press landed, in source coordinates.
    qsizetype m_pressOffset = 0;
    /// A finger held still on the text selects the word under it, the way
    /// a double-click does with a mouse. Only presses from a touchscreen.
    QTimer m_longPress;
    QPoint m_pressPoint;
    EmbedDrag m_embedDrag;
    /// The code block whose label just copied, shown as "Copied" briefly; and
    /// the one under the pointer, which is when a block without a language
    /// shows its "Copy" label at all. Both keyed by the block's source start.
    qsizetype m_copiedBlock = -1;
    /// Tab groups, sorted by start, rebuilt with each parse; and which tab
    /// each shows, keyed by the group's start so it survives a reparse.
    std::vector<TabGroup> m_tabGroups;
    QHash<qsizetype, int> m_selectedTabs;
    qsizetype m_hoveredCodeBlock = -1;
    /// HoverEdit.cpp: index in m_blocks of the first piece of the rendered
    /// block under the pointer that has an edit button, else -1.
    qsizetype m_hoveredBlock = -1;
    bool m_hoverEditButton = true;
    bool m_overEditButton = false;
    /// What the last hover repaint covered, viewport coordinates.
    QRect m_hoverRect;
    /// Start of the block `editBlockSource` selected: while the selection is
    /// exactly that block it shows its source, as a bare caret in it would.
    qsizetype m_sourceReveal = -1;
    Range m_sourceRange; ///< what it selected: the block with its fences
    bool m_autoPairs = true; ///< Obsidian ships both auto-pair settings on
    int m_contentWidth = 700;
    int m_minimumSideMargin = 24;
    int m_topPadding = 28;
    qreal m_totalHeight = 0;
    QList<Snapshot> m_undo;
    QList<Snapshot> m_redo;
    QElapsedTimer m_lastEdit;
    FindState m_find;
    FindBar *m_findBar = nullptr;
    QCompleter *m_completer = nullptr;
    QAbstractItemModel *m_linkModel = nullptr;     ///< names and aliases
    QStringListModel *m_fragmentModel = nullptr;   ///< headings or block ids
    QString m_fragmentKey; ///< what m_fragmentModel holds; empty when stale
    // --- slash commands ------------------------------------------------
    SlashCommandSource m_slashSource;
    QCompleter *m_slashCompleter = nullptr; ///< built lazily: null while unused
    QList<SlashCommand> m_slashCache;       ///< fetched when the popup opens
    QHash<QString, QString> m_slashIDs;     ///< the open popup's name -> id
    QTimer m_parseTimer;
    int m_wordCount = 0;
    QFutureWatcher<ParseJob> m_parseWatcher;
    quint64 m_textVersion = 0;
    quint64 m_parsedVersion = 0;
    bool m_parsePending = false; ///< a pool parse is out and not yet adopted
    bool m_cursorOn = true;
    QTimer m_blink;
};

inline bool MarkdownEditor::pureSource(const BlockView &view) const
{
    return d->m_mode == Mode::Source || view.editedAsSource;
}

inline bool MarkdownEditor::plainSource(const BlockView &view) const
{
    return pureSource(view) || view.editedListing;
}

inline const MarkdownEditor::BlockView &MarkdownEditor::firstPiece(const BlockView &view) const
{
    return *(&view - view.piece);
}

inline const MarkdownEditor::BlockView &MarkdownEditor::lastPiece(const BlockView &view) const
{
    return *(&view - view.piece + view.pieces - 1);
}

} // namespace noteAppC
