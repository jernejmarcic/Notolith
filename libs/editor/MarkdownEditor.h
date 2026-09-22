#pragma once

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QTextLayout>
#include <QTimer>

#include <memory>
#include <optional>
#include <vector>

class QCompleter;

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
    /// One top-level block: the unit that is laid out, cached and revealed.
    struct BlockView {
        BlockSpan span;
        DisplayMap map;
        std::unique_ptr<QTextLayout> layout;
        qreal y = 0;
        qreal height = 0;
        bool active = false;
    };

    void reparse();
    void rebuildBlocks();
    void layoutBlock(BlockView &view);
    void layoutAll();
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
    QFont fontFor(const BlockSpan &span) const;

    Theme m_theme = Theme::builtin(Theme::Mode::Dark);
    Parser m_parser;
    Mode m_mode = Mode::LivePreview;

    QString m_text;
    Decorations m_decorations;
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
    bool m_cursorOn = true;
    QTimer m_blink;
};

} // namespace noteAppC
