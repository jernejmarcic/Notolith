#pragma once

#include <QPlainTextEdit>

namespace noteAppC {

/// Phase 0 note editor: a plain text widget that already behaves like
/// Obsidian's editing surface in the two ways you notice immediately - the
/// text sits in a centred readable column, and lines breathe at 1.5.
///
/// The block-layout engine of Phase 2 replaces the class, not its callers.
class NoteEdit : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit NoteEdit(QWidget *parent = nullptr);

    /// Sets the note text, applies the line height and clears the modified flag.
    void setNoteText(const QString &text);

    /// Obsidian's "Readable line length": cap the text column and centre it.
    void setReadableLineLength(bool on);
    bool readableLineLength() const { return m_readable; }

    /// Width of that column in device-independent pixels (`--file-line-width`).
    void setContentWidth(int pixels);

    /// Proportional line height, 150 meaning Obsidian's 1.5.
    void setLineHeightPercent(int percent);

    /// Space above the first line, in pixels.
    void setTopPadding(int pixels);

    /// Current left/right margin of the text column, so widgets stacked above
    /// the editor (the inline title) can line up with it.
    int sideMargin() const { return m_sideMargin; }

signals:
    void sideMarginChanged(int pixels);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateMargins();
    void applyLineHeight();

    int m_contentWidth = 700;
    int m_sideMargin = 24;
    int m_lineHeightPercent = 150;
    int m_verticalPadding = 28;
    int m_minimumSideMargin = 24;
    bool m_readable = true;
};

} // namespace noteAppC
