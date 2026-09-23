#pragma once

#include <QAbstractScrollArea>
#include <QColor>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QIcon>
#include <QKeySequence>
#include <QPageLayout>
#include <QPageSize>
#include <QPixmap>
#include <QPointer>
#include <QTextLayout>
#include <QTimer>

#include <functional>
#include <memory>
#include <span>
#include <optional>
#include <vector>

class QCompleter;
class QFileSystemWatcher;

#include "CodeHighlighter.h"
#include "MathRenderer.h"
#include "FindBar.h"
#include "Decorations.h"
#include "DisplayMap.h"
#include "Parser.h"
#include "CalloutStyle.h"
#include "LanguageStyle.h"
#include "QueryResult.h"
#include "Theme.h"

class QLineEdit;
class QMenu;

namespace noteAppC {

/// How far a callout inside a callout steps in. Wide enough for its own icon,
/// which is drawn in the space this opens up. Shared, because the layout sets
/// the indent and the renderers draw into it.
constexpr qreal kCalloutIndent = 26;

/// The editor engine: one widget, three modes, no drift between them.
///
/// Every top-level block gets a `QTextLayout` over a *display string* built by
/// a DisplayMap. In live preview a block that does not hold the cursor hides
/// its syntax markers; the block the cursor is in shows its raw source, which
/// is exactly what Obsidian does. Source mode hides nothing; reading view
/// hides everything and takes no cursor.
///
/// `QTextLayout` brings HarfBuzz shaping, bidi and line breaking, so this
/// class only has to decide what string to lay out where.
class MarkdownEditor : public QAbstractScrollArea {
    Q_OBJECT

public:
    enum class Mode { LivePreview, Source, Reading };

    explicit MarkdownEditor(QWidget *parent = nullptr);
    ~MarkdownEditor() override;

    void setTheme(const Theme &theme);
    void setMode(Mode mode);

    /// Opens an editor over the value of the property whose line holds
    /// `sourceOffset`, as clicking the value does. False when that value
    /// cannot be rewritten safely: a list, a checkbox (which toggles), or one
    /// that is quoted across lines, carries a comment, or spans several lines.
    bool editPropertyAt(qsizetype sourceOffset);
    /// For a list property: opens an editor after its items that adds one,
    /// or removes item `item`. False when the list cannot be rewritten
    /// safely - see Frontmatter::rewriteList.
    bool addListItemAt(qsizetype sourceOffset);
    bool removeListItemAt(qsizetype sourceOffset, int item);
    /// Opens an editor over item `item` of a list property, to change it.
    bool editListItemAt(qsizetype sourceOffset, int item);
    Mode mode() const { return m_mode; }

    /// Replaces the buffer and clears undo. Returns the caret to the top.
    void setNoteText(const QString &text);
    QString noteText() const { return m_text; }

    bool isModified() const { return m_modified; }
    void setModified(bool modified);

    /// Line numbers in a gutter, the way Obsidian shows them in source mode.
    void setLineNumbers(bool on);
    bool lineNumbers() const { return m_lineNumbers; }

    void setReadableLineLength(bool on);
    void setContentWidth(int pixels);

    /// CommonMark's rule: every newline is a line break. Off, a single newline
    /// inside a paragraph joins, which is what Obsidian does by default.
    void setStrictLineBreaks(bool on);
    bool strictLineBreaks() const { return m_strictLineBreaks; }

    /// A faint vertical rule at each level of list indentation.
    void setIndentGuides(bool on);
    bool indentGuides() const { return m_indentGuides; }

    /// Spaces a Tab inserts where it is not indenting a list item.
    void setTabWidth(int spaces);
    int tabWidth() const { return m_tabWidth; }

    /// Whether Ctrl+K on a selection writes `[[Note]]` or `[Note](Note.md)`.
    void setUseWikilinks(bool on);

    /// Spelling, from the spellcheck module when it is compiled in and
    /// switched on. The editor only asks; it never links a dictionary. An
    /// empty one - no `isCorrect` - turns underlining off.
    struct Spelling {
        std::function<bool(const QString &)> isCorrect;
        std::function<QStringList(const QString &)> suggestions;
        std::function<void(const QString &)> addWord;
    };
    void setSpelling(Spelling spelling);

    /// Types `text` at the caret, replacing any selection, as one undo step:
    /// how a template or a snippet goes in.
    void insertAtCaret(const QString &text) { insertText(text); }
    /// The menu a right click on a misspelt word at `sourceOffset` opens:
    /// its suggestions, then "Add to dictionary". Null anywhere else. Built
    /// rather than shown, so it can be tested.
    std::unique_ptr<QMenu> spellingMenuAt(qsizetype sourceOffset);
    bool useWikilinks() const { return m_useWikilinks; }

    /// Paints the viewport through OpenGL instead of Qt's raster engine.
    ///
    /// Whether this is faster is a question for the benchmark, not for
    /// intuition: the GL paint engine uploads a glyph cache and re-uploads it
    /// whenever the cache is evicted, which a page of text can do on its own.
    /// Swapping the viewport tears down and rebuilds the backing widget, so
    /// this is meant to be set once at construction, not toggled per frame.
    void setGPUacceleration(bool on);
    bool GPUacceleration() const { return m_GPUacceleration; }
    /// True when this build can do it at all: Qt6::OpenGLWidgets present, and
    /// a platform that can give the window a GL context.
    static bool GPUaccelerationAvailable();

    /// Obsidian's Export to PDF options, with its defaults.
    struct PdfOptions {
        bool includeTitle = true;
        QPageSize pageSize = QPageSize(QPageSize::A4);
        QPageLayout::Orientation orientation = QPageLayout::Portrait;
        qreal marginMillimetres = 25.4; ///< "Default", about an inch
        int downscalePercent = 100;
    };
    /// Renders `text` in reading view, light theme, onto paper, page by page.
    /// A page breaks between blocks; only a block taller than a page is cut.
    /// `resolver` resolves links and embeds as the window's does.
    static bool exportPdf(const QString &path, const QString &text, const QString &title,
                          std::function<QString(const QString &)> resolver,
                          const PdfOptions &options, QString *error = nullptr);
    /// Where each page starts, given every block's top and height: a block
    /// that does not fit starts the next page, one taller than a page runs on
    /// across as many as it takes.
    static QList<qreal> pageStarts(const QList<QPair<qreal, qreal>> &blocks, qreal pageHeight);

    /// Words in the note, frontmatter excluded, as of the last parse; -1 while
    /// a large note's first parse is still out. Counted on the parse thread,
    /// not per keystroke - wordCountChanged says when it moves.
    int wordCount() const { return m_wordCount; }
    /// Source offset of the caret, for tests and for the status bar.
    qsizetype cursorOffset() const { return m_cursor; }
    /// The selected source text, exactly as it is in the file - markers
    /// included, since that is what a copy has to carry.
    QString selectedText() const;
    void setCursorOffset(qsizetype offset);

    /// What the widget currently shows, block by block. The quickest way to
    /// assert live-preview behaviour, and useful when debugging a layout.
    QString displayText() const;
    /// The character format laid out at `sourceOffset`, merged from every
    /// range covering it; empty when its block has no layout yet. The way to
    /// assert what colour a token got without comparing pixels.
    QTextCharFormat formatAt(qsizetype sourceOffset) const;

    /// Height of the whole note as it is currently laid out. What the scroll
    /// bar is built from, and the only honest way to assert block spacing.
    qreal documentHeight() const { return m_totalHeight; }

    /// Pretends the reader clicked into the block holding `offset`, so it
    /// reveals its source. Used by the tests and by "reveal on open".
    void enterBlockAt(qsizetype offset);
    /// Shows tab `member` of the code tab group containing `sourceOffset`.
    /// What a click on a tab does; public for tests and for a keyboard command.
    void selectCodeTab(qsizetype sourceOffset, int member);

    /// One rendered row of a ```` ```query ```` block's results: `rect` in
    /// viewport coordinates, matching what a click there activates - public
    /// so the tests can click a specific result without reimplementing its
    /// layout by hand the way firstCheckboxPoint does for a fixed one.
    struct QueryResultRow {
        QRectF rect;
        QString path;
        qsizetype offset = 0;
    };
    /// The rendered rows of the query block containing `offset`. Empty when
    /// that block is not a query block, is still searching, found nothing,
    /// or has the caret inside it and is showing raw source instead.
    QList<QueryResultRow> queryRowsAt(qsizetype offset) const;

    /// The size of the cached image a rendered HTML block containing
    /// `offset` painted, for the tests - empty when that block is not
    /// rendered HTML (not a Html block, litehtml unavailable, or the caret
    /// is inside it and it is showing raw source instead).
    QSize htmlImageSizeAt(qsizetype offset) const;

    /// Colour a link at `sourceOffset` is painted in, for the tests. Invalid
    /// when there is no link there.
    QColor linkColourAt(qsizetype sourceOffset) const;

    /// Link target under `viewportPoint`, empty when there is none.
    QString linkAt(const QPoint &viewportPoint) const;

    /// Names the `[[` suggester offers. Refreshed from the index.
    void setLinkTargets(const QStringList &targets);

    /// Turns a link target into an absolute path, or an empty string when it
    /// does not resolve. Supplied by the window, which owns the index.
    void setLinkResolver(std::function<QString(const QString &)> resolver);
    /// The index changed, so a link drawn as pointing nowhere may point
    /// somewhere now, or the reverse. Relays out only if one did.
    void refreshLinkStatus();

    /// Delivers the results of one ```query``` block's search, in the order
    /// found; called from any thread. See setQueryRunner.
    using QueryDeliver = std::function<void(QueryResults)>;
    /// Runs `query` and calls `deliver` exactly once with the results, from
    /// any thread - the editor marshals back onto its own thread before
    /// touching a block, so the window never has to. Supplied by the window,
    /// which owns the index; the editor never links libs/index (the module
    /// boundary docs/PLUGINS.md sets out). A search over 20 000 notes takes
    /// about 6 ms (docs/PERFORMANCE.md), but this is still asynchronous:
    /// layout must never wait on it, so a block shows "Searching..." until
    /// `deliver` is called.
    void setQueryRunner(std::function<void(const QString &query, const QueryDeliver &deliver)> runner);
    /// The index changed: every rendered ```query``` block's search is
    /// dropped from the cache and asked again. Debounced, so a burst of
    /// index updates (Index::changed can fire once per save) costs one
    /// search per visible block, not one per change.
    void refreshQueries();

    /// Closes brackets and quotes as you type and wraps a selection instead of
    /// replacing it. Obsidian's `autoPairBrackets` and `autoPairMarkdown` as
    /// one switch, on by default the way Obsidian ships them.
    void setAutoPairs(bool on) { m_autoPairs = on; }
    bool autoPairs() const { return m_autoPairs; }

    // --- editing commands -------------------------------------------------
    // Bound to the same keys Obsidian uses; public so the Phase 4 command
    // palette can reach them without a second implementation.

    /// One editing command, under the id Obsidian gives it where it has one,
    /// so a `hotkeys.json` written for Obsidian rebinds it here as well.
    struct Command {
        QString id;
        QString name;
        QList<QKeySequence> keys; ///< the built-in binding
    };
    static const QList<Command> &commands();
    /// Runs the command with this id. False when there is no such command.
    bool runCommand(const QString &id);
    /// Keys by command id, over the built-in ones; an empty list unbinds.
    /// Undo, redo, copy, cut, paste and select all are not commands: they
    /// keep their standard keys, as they do in Obsidian.
    void setKeyBindings(const QHash<QString, QList<QKeySequence>> &bindings);

    void toggleBold();      ///< Ctrl+B
    void toggleItalic();    ///< Ctrl+I
    void toggleHighlight(); ///< Ctrl+Shift+H
    void insertLink();      ///< Ctrl+K
    void toggleTaskAtCursor(); ///< Ctrl+Enter
    /// Alt+Up / Alt+Down. Moves every line the selection touches.
    void moveLines(int direction);
    void duplicateLines(); ///< Ctrl+Shift+D

    // --- find and replace -------------------------------------------------

    /// Ctrl+F, or Ctrl+H for the replace row as well.
    void showFind(bool withReplace);
    void hideFind();
    bool findVisible() const;
    void setFindQuery(const QString &pattern, bool caseSensitive, bool regex);
    /// +1 for the next match, -1 for the previous; wraps round, as the bar does.
    void findStep(int direction);
    int findMatchCount() const { return int(m_find.matches.size()); }
    /// Zero-based index of the current match, -1 when there is none.
    int findCurrentIndex() const { return m_find.current; }
    /// False when a regex query does not compile; the field turns red.
    bool findPatternValid() const { return m_find.valid; }
    void replaceCurrent(const QString &with);
    /// Returns how many matches were replaced.
    int replaceAll(const QString &with);

    // --- folding ----------------------------------------------------------
    // A `> [!type]-` callout, a `:::` container and a `#| fold: true` code
    // block all fold the same way; so does the properties table. `offset` is
    // anywhere inside the block.

    void toggleFoldAt(qsizetype sourceOffset);
    bool isFoldedAt(qsizetype sourceOffset) const;

    /// The frontmatter properties, as the last parse read them. The properties
    /// table renders from these, and the window's future property pane will.
    const QList<Property> &properties() const { return m_properties; }

    /// The note's headings, as the last parse read them, in source order. The
    /// outline pane builds its tree from these instead of re-parsing.
    const QList<Heading> &headings() const { return m_headings; }

signals:
    /// The word count moved. It is published with each parse, so it lags a
    /// keystroke by the parse debounce instead of costing the keystroke.
    void wordCountChanged(int words);
    /// A parse landed, so headings() reflects the current text. Fired even
    /// when the heading list did not actually change - cheap to ignore, and
    /// the outline pane only rebuilds a QTreeWidget from it.
    void headingsChanged();
    void textChanged();
    void modificationChanged(bool modified);
    void linkActivated(const QString &target);
    void cursorMoved(qsizetype offset);
    /// The pointer came to rest on a link to a note, the way Obsidian's page
    /// preview wants it: plain hover in reading view, with Ctrl held while
    /// editing. `globalPos` is where the pointer is.
    void linkHovered(const QString &target, const QPoint &globalPos);
    /// It left that link, or let go of it by leaving the editor.
    void linkHoverEnded();
    /// A result in a rendered ```query``` block was clicked: `relativePath`
    /// is the note it matched, `offset` the first match inside it.
    void queryResultActivated(const QString &relativePath, qsizetype offset);

protected:
    void paintEvent(QPaintEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void resizeEvent(QResizeEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    /// Selects the word around `offset`, for a double-click or a long press.
    void selectWordAt(qsizetype offset);
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    /// One row of the frontmatter properties table: the key and its type icon
    /// on the left, the typed value on the right. Rectangles are relative to
    /// the block's own origin, like a table Cell's.
    struct PropertyRow {
        int property = 0;     ///< index into `m_properties`
        QRectF rect;          ///< the whole row
        QRectF control;       ///< the checkbox, when the value is one
        QList<QRectF> chips;  ///< one box per item of a list value
        QList<QRectF> removers; ///< each chip's "x", when the list can be changed
        QRectF adder;           ///< the space after the chips that adds an item
    };

    /// One cell of a rendered table.
    struct Cell {
        Range source;
        DisplayMap map;
        std::unique_ptr<QTextLayout> layout;
        QRectF rect;
        Qt::Alignment alignment = Qt::AlignLeft;
        int row = 0;
        int column = 0;
    };

    /// One top-level block: the unit that is laid out, cached and revealed.
    /// Blocks outside the viewport carry an estimated height and no layout,
    /// which is what keeps a 200 kB note responsive.
    /// A callout painted inside another block's layout, with the look it
    /// resolved to.
    struct NestedCallout {
        const CalloutSpan *span = nullptr;
        CalloutStyle style;
    };

    struct BlockView {
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
        /// A table keeps its columns at their natural width; when the total
        /// exceeds the readable column the table scrolls sideways inside its
        /// own block, exactly as it does in Obsidian.
        qreal tableWidth = 0;
        qreal tableScroll = 0;
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
        /// Set on the fence a tab group is showing: it paints the strip.
        /// An index into m_tabGroups, -1 outside any group.
        int tabGroup = -1;
        /// Inside the tab group the caret is in, which shows every fence.
        /// Tracked so the caret crossing a group's edge relays out the whole
        /// group, not only the block it landed in.
        bool inCaretGroup = false;
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
        /// Filename shown in the header strip of a source-file or PDF embed.
        QString embedLabel;
        /// Draw the code panel and header behind this block.
        bool embedPanel = false;
        /// An audio or video embed's file: the block is a player card.
        QString mediaPath;
        bool mediaVideo = false;

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
    };

    /// Parses on the calling thread. Used when the whole note changes.
    void reparse();
    /// Starts a parse on the thread pool; the editor never waits for it.
    void startParse();
    void adoptDecorations(Decorations decorations, QList<Property> properties, QList<Heading> headings);
    void rebuildBlocks();
    void indexDecorations();
    void layoutBlock(BlockView &view);
    /// Lays a table out as a grid. False when this block is not a table or
    /// its structure is missing, so the caller falls back to plain text.
    bool layoutTable(BlockView &view);
    void paintTable(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The table under `viewportPoint` that is wider than the column, or null.
    BlockView *scrollableTableAt(const QPoint &viewportPoint);
    void paintCallout(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// Bullets and checkboxes in the margin of a list block.
    void paintListMarkers(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The list item in `view` whose marker was hidden at the start of `source`.
    const ListItemSpan *listItemAt(const BlockView &view, qsizetype source) const;
    /// The list items that start inside `block`.
    std::span<const ListItemSpan> listItemsIn(const Range &block) const;
    /// Checkbox rectangle of `item` in viewport coordinates, for hit testing.
    QRectF checkboxRect(const BlockView &view, const ListItemSpan &item) const;
    void toggleTask(const ListItemSpan &item);
    const CalloutSpan *calloutFor(const BlockView &view) const;

    // --- block renderers, implemented in BlockRenderers.cpp ---------------

    /// Lays the frontmatter out as Obsidian's properties table. False when
    /// there is nothing to show, so the caller falls back to plain text.
    bool layoutProperties(BlockView &view);
    void paintProperties(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The panel, the header strip, the line-number gutter and the highlighted
    /// rows of a fenced code block.
    void paintCodeChrome(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The first piece's share of the chrome: the `#|` header, the tab strip
    /// or just the label, over `panel`, which spans the whole fence.
    void paintCodeHeader(QPainter &painter, const BlockView &view, qreal left, qreal top,
                         const QRectF &panel) const;
    /// `#| highlight` rows and `#| line-numbers` for one piece.
    void paintCodeRows(QPainter &painter, const BlockView &view, qreal left, qreal top,
                       const QRectF &panel) const;
    QRectF bandOf(const BlockView &view, const Range &source) const;

    /// The label on the side of a code block's header: its language's style
    /// name and icon, and the thing to click to copy the listing. Painting and
    /// hit-testing both come from codeLabelOf, so they cannot disagree about
    /// where it is.
    struct CodeLabel {
        QRectF rect;   ///< in viewport coordinates, icon included
        QString text;
        QIcon icon;
        QColor colour;
        QFont font;
        bool visible = false;
    };
    CodeLabel codeLabelOf(const BlockView &view, qreal left, qreal top) const;
    const CodeFenceSpan *codeFenceOf(const BlockView &view) const;
    LanguageStyle codeStyleOf(const BlockView &view) const;
    LanguageStyle styleOfFence(const CodeFenceSpan &fence) const;

    /// A run of adjacent `tab` fences, shown as one block with a tab strip.
    struct TabGroup {
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
    struct TabRect {
        QRectF rect;
        QString text;
        QIcon icon;
        int member = 0;
        bool selected = false;
    };
    void collectTabGroups();
    const TabGroup *tabGroupAt(qsizetype source) const;
    /// The tab group the caret is in, its closing fence's end included.
    const TabGroup *caretTabGroup() const;
    int selectedTabOf(const TabGroup &group) const;
    std::vector<TabRect> tabsOf(const BlockView &view, qreal left, qreal top) const;
    void copyCodeBlock(const BlockView &view);
    const BlockView *codeLabelAt(const QPoint &point) const;
    void paintCodeLabel(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    int nestingDepthAt(const BlockView &view, qsizetype source) const;
    void hideFoldedNestedCallouts(const BlockView &view, QList<Range> &hidden) const;
    /// True for the kinds that show all their source when the caret is in
    /// them - tables, code, maths, callouts - as Obsidian does with its widgets.
    /// Paragraphs, headings, lists and plain quotes reveal per element instead.
    bool revealsWholeBlock(const BlockView &view) const;
    QList<Range> revealedSyntax(const BlockView &view) const;
    QRectF nestedFoldHandleRect(const BlockView &view, const NestedCallout &nested, qreal left,
                                qreal top) const;
    void paintNestedCallouts(QPainter &painter, const BlockView &view, qreal left,
                             qreal top) const;
    void paintNestedCodePanels(QPainter &painter, const BlockView &view, qreal left,
                               qreal top) const;
    void paintIndentGuides(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// Panel and filename header for a source-file or PDF embed.
    void paintEmbedChrome(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    void paintCheckBox(QPainter &painter, const QRectF &rect, bool done) const;
    /// The disclosure triangle every foldable block carries.
    void paintFoldArrow(QPainter &painter, const QRectF &rect, bool folded,
                        const QColor &colour) const;
    /// `#|` options of this block, or nullptr. Matched on the block's start:
    /// `rebuildBlocks` takes the newline after a block into its span, so the
    /// ends no longer compare equal.
    const CodeOptions *codeOptionsFor(const BlockView &view) const;
    qreal codeHeaderHeight() const;
    /// Space under the listing for a `#| caption:`, 0 when there is none.
    qreal codeCaptionHeight(const BlockView &view) const;
    qreal propertyRowHeight() const;
    /// Width a code block reserves for `#| lines:` numbers, 0 when it has none.
    qreal codeGutterWidth(const BlockView &view) const;
    /// Obsidian's `--p-spacing`: one rem of the text font, which is the unit
    /// every block margin is made of.
    qreal blockSpacing() const;
    /// Where this block's disclosure triangle is, in viewport coordinates.
    QRectF foldHandleRect(const BlockView &view, qreal left, qreal top) const;
    bool isFoldable(const BlockView &view) const;
    /// What the source itself says: `[!note]-` and `::: details` start folded.
    bool defaultFoldOf(const BlockView &view) const;
    void setFolded(const BlockView &view, bool folded);
    /// True when the press landed on a fold handle or a property control, so
    /// the caret must not move.
    bool handleBlockClick(const QPoint &point);
    /// Flips a checkbox property, rewriting only that value's bytes.
    void toggleProperty(int index);
    /// The key's line, and where on it the value is, when its value can be
    /// replaced without touching anything else; both invalid otherwise.
    Range editableValueOf(const Property &property) const;
    bool listIsRewritable(const Property &property) const;
    int propertyAt(qsizetype sourceOffset) const;
    /// The laid-out row of property `index`, and the table's top in viewport
    /// coordinates; null when the table is folded or not laid out.
    const PropertyRow *propertyRowOf(int index, qreal *top) const;
    void openPropertyEditor(int index, const QRectF &cell, const QString &text, int item);
    /// Writes a list property's items back, as one undo step.
    bool replaceListEntry(int index, const QStringList &raws, const QStringList &values);
    /// Closes the property editor, writing what it holds when `commit`.
    void finishPropertyEdit(bool commit);
    bool eventFilter(QObject *watched, QEvent *event) override;
    /// The embed that fills this block on its own, or nullptr.
    const EmbedSpan *soleEmbedOf(const BlockView &view) const;
    /// A block that is nothing but an embed: an image, a PDF page, or a
    /// source file shown as a read-only code block.
    bool layoutEmbed(BlockView &view);
    bool layoutImage(BlockView &view, const EmbedSpan &embed, const QString &path);
    bool layoutPdf(BlockView &view, const EmbedSpan &embed, const QString &path);
    bool layoutSourceFile(BlockView &view, const EmbedSpan &embed, const QString &path);
    /// A block that is one `$$...$$` formula, laid out by MicroTeX.
    bool layoutMath(BlockView &view);
    const MathSpan *displayMathOf(const BlockView &view) const;
    /// Renders every inline `$...$` formula the block's own maths span list
    /// touches, and drops a formula's hide range back out of `hidden` when
    /// MicroTeX cannot draw it - so DisplayMap shows the raw source rather
    /// than an empty gap. Must run before `view.map` is built from `hidden`.
    void layoutInlineMaths(BlockView &view, QList<Range> &hidden);
    /// Draws the images `layoutInlineMaths` recorded, once the block's own
    /// QTextLayout exists and can say which line each one's "$" landed on.
    void paintInlineMaths(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// `[[Note#Heading]]` reads "Note > Heading" and `[[#Heading]]` reads
    /// "Heading", as they do in Obsidian, while the link is rendered. Hides a
    /// same-note link's "#" and records the others' separators, so it must run
    /// before `view.map` is built from `hidden`.
    void layoutAnchorSeparators(BlockView &view, QList<Range> &hidden);
    void paintAnchorSeparators(QPainter &painter, const BlockView &view, qreal left,
                               qreal top) const;

    // --- query blocks, implemented in QueryBlock.cpp -----------------------

    /// A fenced ```` ```query ```` block: renders its search results (a note
    /// list with match counts and highlighted snippets) instead of the query
    /// text, the way live preview renders any other block - false for every
    /// other fence, which falls through to the ordinary code listing.
    bool layoutQuery(BlockView &view);
    /// The cached entry for `queryText`, starting a search through
    /// m_queryRunner when there is none yet. Never blocks: a block shows
    /// "Searching..." until deliverQueryResults lands.
    struct QueryCacheEntry {
        QueryResults results;
        bool pending = true;
        /// Bumped each time a search for this text is (re)started, so a
        /// delivery from a superseded request - refreshQueries restarted one
        /// while it was still in flight - is recognised as stale and dropped.
        quint64 generation = 0;
    };
    const QueryCacheEntry &ensureQuerySearch(const QString &queryText);
    /// Called back (marshalled onto this thread) when a search started by
    /// ensureQuerySearch finishes. Ignored if `generation` is no longer the
    /// one on record for `queryText`.
    void deliverQueryResults(const QString &queryText, quint64 generation, QueryResults results);

    // --- HTML blocks and inline HTML, implemented in HtmlBlock.cpp --------

    /// A block that is nothing but raw HTML (`<div>`, `<table>`,
    /// `<details>`...) at the top level, rendered by litehtml into a cached
    /// image the way a formula or an embed is. False when litehtml is not
    /// compiled in (NOTEAPPC_WITH_HTML off, or it was not found), or the
    /// block has no visible content - either way the source shows instead.
    bool layoutHtml(BlockView &view);
    /// Resolves `<img src="...">` against `m_resolver`, the same one embeds
    /// use, and drops anything with a network scheme - litehtml never
    /// fetches a remote image. Local paths are rewritten to what the
    /// resolver returns; everything else about the markup is untouched.
    QString resolveHtmlImages(const QString &html) const;
    /// Pairs up inline HTML tags (`<b>...</b>`, `<span style="color:...">
    /// ...</span>`) within one block into QTextCharFormat runs, hiding the
    /// tags themselves the way `**bold**`'s asterisks are hidden. Must run
    /// before `view.map` is built from `hidden`, like layoutInlineMaths.
    /// Skipped for a block-level HTML span - that is litehtml's job, not
    /// this pairing pass.
    void layoutInlineHtml(BlockView &view, QList<Range> &hidden);
    /// The QTextCharFormat for a recognised inline tag, or nullopt for one
    /// this pass leaves as raw source (unknown tags, and anything in a
    /// `style=` attribute besides `color:`). `openTag` is the opening tag's
    /// own source text, for `<span style="...">`'s colour.
    std::optional<QTextCharFormat> htmlTagFormat(const QString &name, const QString &openTag) const;
    /// True when the newline at `source` joins two lines of one paragraph
    /// rather than breaking them apart.
    bool softBreakAt(qsizetype source) const;
    /// Extra space a callout reserves for its title row.
    qreal calloutHeaderHeight(const CalloutStyle &style) const;
    QList<QTextLayout::FormatRange> formatsIn(const BlockView &view, const Range &source,
                                              const DisplayMap &map) const;
    const TableSpan *tableFor(const BlockView &view) const;
    /// Heights, positions, and layouts for the blocks near the viewport.
    void updateLayout();
    void invalidateLayouts();
    qreal estimatedHeight(const BlockView &view) const;
    qreal lineHeightFor(const BlockSpan &span) const;
    mutable QHash<int, qreal> m_lineHeights; ///< memoised per block kind and level
    /// Moves every range at or after `at` by `delta`, so one keystroke does
    /// not need a reparse before the caret's block can be redrawn.
    void shiftRanges(qsizetype at, qsizetype delta);
    void applyEdit(qsizetype at, qsizetype removed, const QString &inserted);
    void updateScrollRange();
    int contentLeft() const;
    int contentWidth() const;
    /// Width reserved for the line-number gutter, 0 when it is not shown.
    int gutterWidth() const;
    void paintGutter(QPainter &painter, const BlockView &view, qreal top) const;

    /// True when a wikilink target has no note behind it, so live preview can
    /// dim it the way Obsidian does. Cached per parse: the resolver reaches
    /// the index, and a note full of links is laid out on every keystroke.
    bool isUnresolvedLink(const QString &target) const;
    bool resolvesToNothing(const QString &target) const;

    qsizetype blockAt(qsizetype sourceOffset) const;
    qsizetype offsetAt(const QPoint &viewportPoint) const;
    QPointF cursorPoint() const;
    void ensureCursorVisible();

    void insertText(const QString &text);
    void removeSelectionOrChar(int direction);
    void moveCursor(qsizetype offset, bool keepAnchor);
    void moveVertically(int lines, bool keepAnchor);
    Range selection() const;
    void pushUndo();
    void undo();
    void redo();

    // --- editing commands, implemented in EditingCommands.cpp -------------

    /// `applyEdit` leaves the caret after what it inserted; a command that
    /// wants it elsewhere - or wants the selection kept on the text it just
    /// moved - says so with this.
    void setSelectionRange(qsizetype anchor, qsizetype cursor);
    /// True when the keystroke was an auto-pair and needs no further handling.
    bool handleAutoPair(QChar typed);
    /// Backspace between an empty pair takes both halves.
    bool deleteAutoPair();
    /// Enter inside a list item: continue it, or end the list.
    bool handleListEnter();
    /// Tab / Shift+Tab on a list item. `direction` is +1 in, -1 out.
    bool handleListIndent(int direction);
    /// The run of consecutive list lines around `offset`: the stretch one
    /// structural edit renumbers, rewritten as a single splice.
    Range listRun(qsizetype offset) const;
    /// Wraps or unwraps the selection in `marker`; the whole of Ctrl+B/I and
    /// Ctrl+Shift+H.
    void toggleWrap(const QString &marker);

    void updateFindMatches();
    /// Puts the current match on screen with the viewport-only layout path.
    void scrollToOffset(qsizetype offset);
    void syncFindBar();
    void positionFindBar();
    /// `--text-highlight-bg`, or Obsidian's own default when the theme has no
    /// such token.
    QColor findHighlightColor() const;
    /// Background ranges for the matches inside `view`, in display offsets.
    void appendFindHighlights(const BlockView &view,
                              QList<QTextLayout::FormatRange> &into) const;

    /// Text typed after the nearest unclosed `[[` before the caret; unset when
    /// the caret is not inside a wikilink.
    std::optional<QString> wikilinkPrefix() const;
    void updateSuggestions();
    void insertCompletion(const QString &completion);

    QList<QTextLayout::FormatRange> formatsFor(BlockView &view);
    /// A wavy underline under each word of `view` the dictionary rejects.
    void appendSpellingFormats(const BlockView &view, QList<QTextLayout::FormatRange> &formats) const;
    /// The words of `view` spelling looks at, in source coordinates: prose
    /// only, never code, links, tags or maths, and not the word being typed.
    QList<Range> checkedWordsOf(const BlockView &view) const;
    /// The word the caret is in or just after, while spelling is on.
    Range wordAtCaret() const;
    /// Kate-definition colours for a fenced code block, in the coordinates of
    /// that block's display string. Not const: a piece records the state it
    /// ended in, for the piece after it.
    QList<QTextLayout::FormatRange> codeFormatsFor(BlockView &view);
    /// Highlights one piece from the end state of the piece before it and
    /// records where it ended; the formats are relative to `*content`.
    QList<QTextLayout::FormatRange> highlightPiece(BlockView &view, Range *content);
    /// Brings the end state of every piece before `view` up to date, so
    /// `view` can start from the one just before it.
    void updateCodeStatesBefore(BlockView &view);
    const BlockView &firstPiece(const BlockView &view) const { return *(&view - view.piece); }
    const BlockView &lastPiece(const BlockView &view) const
    {
        return *(&view - view.piece + view.pieces - 1);
    }
    QList<QTextLayout::FormatRange> nestedCodeFormatsFor(const BlockView &view) const;
    QList<BlockSpan> plainParagraphs() const;
    /// The code between the fences, and the language the fence declared.
    /// For one piece of a long fence: that piece's share of the code, and the
    /// fence's language.
    Range codeContentOf(const BlockView &view, QString *language) const;
    QFont fontFor(const BlockSpan &span) const;

    Theme m_theme = Theme::builtin(Theme::Mode::Dark);
    /// `~/.config/noteAppC/callouts.ini` and `codeblocks.ini`, reread the
    /// moment either changes on disk - they are the way a person restyles
    /// these, so an edit has to show without a restart.
    CalloutStyles m_calloutStyles = CalloutStyles::load();
    LanguageStyles m_languageStyles = LanguageStyles::load();
    QFileSystemWatcher *m_configWatcher = nullptr;
    void watchStyleFiles();
    void reloadStyleFiles();
    Parser m_parser;
    /// Every block between `exposedTop` and `exposedBottom`, which are in
    /// viewport coordinates for a document scrolled to `scroll`. The screen
    /// and a PDF page both paint through this.
    void paintBlocks(QPainter &painter, qreal scroll, qreal exposedTop, qreal exposedBottom) const;
    CodeHighlighter m_code;
    /// Plays audio and video embeds; made on first use, one file at a time.
    class MediaPlayback *m_media = nullptr;
    bool layoutMedia(BlockView &view, const QString &path, bool video);
    void paintMedia(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// A player card's parts, in viewport coordinates: the picture (empty for
    /// audio), the play button and the progress bar.
    QRectF mediaScreenRect(const BlockView &view, qreal left, qreal top) const;
    QRectF mediaButtonRect(const BlockView &view, qreal left, qreal top) const;
    QRectF mediaBarRect(const BlockView &view, qreal left, qreal top) const;
    /// Play, pause or seek, for a click at `point` on a player card.
    bool handleMediaClick(const BlockView &view, const QPoint &point, qreal left, qreal top);
    /// The link a preview was asked for, so the signals fire on changes only.
    QString m_previewLink;
    void updateLinkHover(const QPoint &viewportPoint, bool control, const QPoint &globalPos);
    /// A key combination, as QKeyCombination::toCombined(), to the command
    /// it runs.
    QHash<int, QString> m_keymap;
    Spelling m_spelling;
    /// The inline editor over a property value, while one is open, and the
    /// key it edits: a reparse can renumber the properties under it.
    QPointer<QLineEdit> m_propertyEdit;
    QString m_editedKey;
    /// What the open editor writes: the scalar value (kEditValue), a new list
    /// item (kEditNewItem), or list item n.
    int m_editedItem = -1;
    static constexpr int kEditValue = -1;
    static constexpr int kEditNewItem = -2;
    /// Where the last updateLayout() moved blocks from, in document y; +inf
    /// when nothing moved. A scroll can reuse the pixels on screen only if
    /// nothing on screen moved.
    qreal m_movedFrom = 0;
    MathRenderer m_math;
    std::function<QString(const QString &)> m_resolver;
    mutable QHash<QString, QPixmap> m_images;
    Mode m_mode = Mode::LivePreview;

    std::function<void(const QString &, const QueryDeliver &)> m_queryRunner;
    QHash<QString, QueryCacheEntry> m_queryCache; ///< query text -> its last search
    quint64 m_queryGeneration = 0;
    /// Debounces refreshQueries(): Index::changed can fire once per save, and
    /// a burst of them should cost one re-search per block, not one each.
    QTimer *m_queryRefreshTimer = nullptr;

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
    mutable qreal m_blockSpacing = 0; ///< memoised, cleared with the theme

    qsizetype m_cursor = 0;
    qsizetype m_anchor = 0;
    qsizetype m_activeBlock = -1;
    qreal m_goalX = -1; ///< remembered column for up/down

    /// A freshly opened note renders whole; the block under the caret only
    /// shows its source once the reader actually edits or clicks into it,
    /// which is how Obsidian behaves.
    bool m_interacted = false;
    bool m_modified = false;
    bool m_readable = true;
    bool m_lineNumbers = true; ///< shown in source mode only
    bool m_strictLineBreaks = false; ///< Obsidian's app.json setting
    bool m_indentGuides = false;
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
    /// The code block whose label just copied, shown as "Copied" briefly; and
    /// the one under the pointer, which is when a block without a language
    /// shows its "Copy" label at all. Both keyed by the block's source start.
    qsizetype m_copiedBlock = -1;
    /// Tab groups, sorted by start, rebuilt with each parse; and which tab
    /// each shows, keyed by the group's start so it survives a reparse.
    std::vector<TabGroup> m_tabGroups;
    QHash<qsizetype, int> m_selectedTabs;
    qsizetype m_hoveredCodeBlock = -1;
    bool m_autoPairs = true; ///< Obsidian ships both auto-pair settings on
    int m_contentWidth = 700;
    int m_minimumSideMargin = 24;
    int m_topPadding = 28;
    qreal m_totalHeight = 0;

    struct Snapshot {
        QString text;
        qsizetype cursor = 0;
    };
    QList<Snapshot> m_undo;
    QList<Snapshot> m_redo;
    QElapsedTimer m_lastEdit;

    /// Obsidian's in-note find bar: the matches live here because the editor
    /// paints them, the bar is only the field and the buttons.
    struct FindState {
        QString pattern;
        bool caseSensitive = false;
        bool regex = false;
        bool valid = true;
        QList<Range> matches; ///< sorted, non-overlapping
        int current = -1;
    };
    FindState m_find;
    FindBar *m_findBar = nullptr;

    QCompleter *m_completer = nullptr;
    QTimer m_parseTimer;
    /// The parse runs off the UI thread; a generation counter says whether the
    /// result still describes the text in the buffer.
    struct ParseJob {
        Decorations decorations;
        QList<Property> properties;
        QList<Heading> headings;
        int words = 0;
        quint64 version = 0;
    };
    int m_wordCount = 0;
    void setWordCount(int words);
    QFutureWatcher<ParseJob> m_parseWatcher;
    quint64 m_textVersion = 0;
    quint64 m_parsedVersion = 0;
    bool m_cursorOn = true;
    QTimer m_blink;
};

} // namespace noteAppC
