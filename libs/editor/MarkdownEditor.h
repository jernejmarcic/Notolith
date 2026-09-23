#pragma once

#include <QAbstractScrollArea>
#include <QColor>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QIcon>
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

    /// Closes brackets and quotes as you type and wraps a selection instead of
    /// replacing it. Obsidian's `autoPairBrackets` and `autoPairMarkdown` as
    /// one switch, on by default the way Obsidian ships them.
    void setAutoPairs(bool on) { m_autoPairs = on; }
    bool autoPairs() const { return m_autoPairs; }

    // --- editing commands -------------------------------------------------
    // Bound to the same keys Obsidian uses; public so the Phase 4 command
    // palette can reach them without a second implementation.

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

signals:
    /// The word count moved. It is published with each parse, so it lags a
    /// keystroke by the parse debounce instead of costing the keystroke.
    void wordCountChanged(int words);
    void textChanged();
    void modificationChanged(bool modified);
    void linkActivated(const QString &target);
    void cursorMoved(qsizetype offset);

protected:
    void paintEvent(QPaintEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void resizeEvent(QResizeEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
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
    void adoptDecorations(Decorations decorations, QList<Property> properties);
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
    CodeHighlighter m_code;
    Spelling m_spelling;
    /// The inline editor over a property value, while one is open, and the
    /// key it edits: a reparse can renumber the properties under it.
    QPointer<QLineEdit> m_propertyEdit;
    QString m_editedKey;
    /// Where the last updateLayout() moved blocks from, in document y; +inf
    /// when nothing moved. A scroll can reuse the pixels on screen only if
    /// nothing on screen moved.
    qreal m_movedFrom = 0;
    MathRenderer m_math;
    std::function<QString(const QString &)> m_resolver;
    mutable QHash<QString, QPixmap> m_images;
    Mode m_mode = Mode::LivePreview;

    QString m_text;
    SourceMap m_lines; ///< line starts, for the gutter
    Decorations m_decorations; ///< styles and syntax kept sorted by start
    mutable QHash<QString, bool> m_unresolved; ///< link target -> has no note
    QList<Property> m_properties; ///< frontmatter, for the properties table
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
