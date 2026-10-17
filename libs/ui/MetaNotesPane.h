#pragma once

#include <QColor>
#include <QList>
#include <QString>
#include <QWidget>

#include "Metadata.h"

class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace noteAppC {

class Theme;

/// The active note's meta notes (editor.metaNotes), in document order and
/// grouped under the heading above each, so a writer can see what is still
/// open before she finishes a draft. A row shows the run's own text and, in
/// muted type beneath it, the line and the words around it; the run the caret
/// is in is highlighted, the way the Outline tracks its section.
///
/// Like OutlinePane it takes no editor: MainWindow feeds it the editor's
/// noteText(), metaNoteRanges() and headings() on every parse. Nothing here is
/// ever hidden or removed - a meta note leaves the text only when it is deleted.
class MetaNotesPane : public QWidget {
    Q_OBJECT

public:
    explicit MetaNotesPane(QWidget *parent = nullptr);

    /// Shows `runs` (outermost, delimiters included, source order) of `text`.
    /// A no-op - collapsed groups stay collapsed, no label is rebuilt - when
    /// every row would read the same; typing above a run only moves its
    /// offsets, which are rewritten in place.
    void setNote(const QString &text, const QList<Range> &runs, const QList<Heading> &headings);
    /// Clears the pane, as when no note is open.
    void clear();
    /// The caret moved to `sourceOffset`: highlights the run it is in or touches.
    void setCursorOffset(qsizetype sourceOffset);
    /// The editor's delimiters: the rows show a run without them, and the
    /// empty state names them.
    void setDelimiters(const QString &open, const QString &close);
    void setTheme(const Theme &theme);

signals:
    /// A row was activated; the editor selects `run`.
    void jumpRequested(const Range &run);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// What one row shows, and where it points.
    struct Row {
        int group = -1;  ///< index of the heading it sits under, -1 before the first
        QString heading; ///< that heading's text
        QString inner;   ///< the run without its delimiters, whitespace collapsed
        QString before, run, after; ///< the words around it on its line, collapsed
        qsizetype line = 0;         ///< 1-based
        Range range;
    };
    static bool sameDisplay(const QList<Row> &a, const QList<Row> &b);

    void populate();
    void updateHighlight();
    QString rowHTML(const Row &row) const;

    QLabel *m_header = nullptr;
    QLineEdit *m_filter = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_empty = nullptr;
    QString m_open = QStringLiteral("[");
    QString m_close = QStringLiteral("]");
    QList<Row> m_rows;
    QList<QTreeWidgetItem *> m_items; ///< one per shown row, in m_rows order of those shown
    QList<qsizetype> m_itemRow;       ///< the m_rows index behind each m_items entry
    qsizetype m_cursor = 0;
    int m_highlighted = -1; ///< index into m_items
    QColor m_normal, m_muted, m_meta, m_highlightBackground;
};

} // namespace noteAppC
