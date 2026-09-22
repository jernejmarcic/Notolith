#pragma once

#include <QAbstractScrollArea>
#include <QColor>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QPixmap>
#include <QTextLayout>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QCompleter;

#include "CodeHighlighter.h"
#include "FindBar.h"
#include "Decorations.h"
#include "DisplayMap.h"
#include "Parser.h"
#include "Theme.h"

namespace noteAppC {

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

    int wordCount() const;
    /// Source offset of the caret, for tests and for the status bar.
    qsizetype cursorOffset() const { return m_cursor; }
    void setCursorOffset(qsizetype offset);

    /// What the widget currently shows, block by block. The quickest way to
    /// assert live-preview behaviour, and useful when debugging a layout.
    QString displayText() const;

    /// Height of the whole note as it is currently laid out. What the scroll
    /// bar is built from, and the only honest way to assert block spacing.
    qreal documentHeight() const { return m_totalHeight; }

    /// Pretends the reader clicked into the block holding `offset`, so it
    /// reveals its source. Used by the tests and by "reveal on open".
    void enterBlockAt(qsizetype offset);

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
    void textChanged();
    void modificationChanged(bool modified);
    void linkActivated(const QString &target);
    void cursorMoved(qsizetype offset);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
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
        /// Set when this block is a callout, so painting knows its colour,
        /// icon and title without looking the span up again.
        const CalloutSpan *callout = nullptr;
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
        QString imagePath;
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
    void paintCallout(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// Bullets and checkboxes in the margin of a list block.
    void paintListMarkers(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The list item whose marker was hidden at the start of `source`.
    const ListItemSpan *listItemAt(qsizetype source) const;
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
    /// The embed that fills this block on its own, or nullptr.
    const EmbedSpan *soleEmbedOf(const BlockView &view) const;
    /// Loads and scales the image for a block that is one image embed.
    bool layoutImage(BlockView &view);
    /// True when the newline at `source` joins two lines of one paragraph
    /// rather than breaking them apart.
    bool softBreakAt(qsizetype source) const;
    /// Extra space a callout reserves for its title row.
    qreal calloutHeaderHeight() const;
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

    QList<QTextLayout::FormatRange> formatsFor(const BlockView &view) const;
    /// Kate-definition colours for a fenced code block, in the coordinates of
    /// that block's display string.
    QList<QTextLayout::FormatRange> codeFormatsFor(const BlockView &view) const;
    /// The code between the fences, and the language the fence declared.
    Range codeContentOf(const BlockView &view, QString *language) const;
    QFont fontFor(const BlockSpan &span) const;

    Theme m_theme = Theme::builtin(Theme::Mode::Dark);
    Parser m_parser;
    CodeHighlighter m_code;
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
        quint64 version = 0;
    };
    QFutureWatcher<ParseJob> m_parseWatcher;
    quint64 m_textVersion = 0;
    quint64 m_parsedVersion = 0;
    bool m_cursorOn = true;
    QTimer m_blink;
};

} // namespace noteAppC
