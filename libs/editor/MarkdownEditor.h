#pragma once

#include <QAbstractScrollArea>
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

    void setReadableLineLength(bool on);
    void setContentWidth(int pixels);

    int wordCount() const;
    /// Source offset of the caret, for tests and for the status bar.
    qsizetype cursorOffset() const { return m_cursor; }
    void setCursorOffset(qsizetype offset);

    /// What the widget currently shows, block by block. The quickest way to
    /// assert live-preview behaviour, and useful when debugging a layout.
    QString displayText() const;

    /// Pretends the reader clicked into the block holding `offset`, so it
    /// reveals its source. Used by the tests and by "reveal on open".
    void enterBlockAt(qsizetype offset);

    /// Link target under `viewportPoint`, empty when there is none.
    QString linkAt(const QPoint &viewportPoint) const;

    /// Names the `[[` suggester offers. Refreshed from the index.
    void setLinkTargets(const QStringList &targets);

    /// Turns a link target into an absolute path, or an empty string when it
    /// does not resolve. Supplied by the window, which owns the index.
    void setLinkResolver(std::function<QString(const QString &)> resolver);

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
        /// Set when the whole block is one image embed.
        QPixmap image;
        QSize imageSize;
        QString imagePath;
    };

    /// Parses on the calling thread. Used when the whole note changes.
    void reparse();
    /// Starts a parse on the thread pool; the editor never waits for it.
    void startParse();
    void adoptDecorations(Decorations decorations);
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
    Decorations m_decorations; ///< styles and syntax kept sorted by start
    std::vector<BlockView> m_blocks; // move-only: QTextLayout is not copyable

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
    bool m_strictLineBreaks = false; ///< Obsidian's app.json setting
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

    QCompleter *m_completer = nullptr;
    QTimer m_parseTimer;
    /// The parse runs off the UI thread; a generation counter says whether the
    /// result still describes the text in the buffer.
    struct ParseJob {
        Decorations decorations;
        quint64 version = 0;
    };
    QFutureWatcher<ParseJob> m_parseWatcher;
    quint64 m_textVersion = 0;
    quint64 m_parsedVersion = 0;
    bool m_cursorOn = true;
    QTimer m_blink;
};

} // namespace noteAppC
