// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFrame>

#include "Theme.h"

class QLabel;
class QLineEdit;
class QToolButton;

namespace noteAppC {

/// Obsidian's in-note find bar: a small panel floating over the editor's own
/// viewport rather than a strip in the window chrome, so every pane searches
/// its own note.
///
/// The bar holds no matches and knows nothing about the text. It reports what
/// the reader asked for; MarkdownEditor finds, highlights and replaces.
class FindBar : public QFrame {
    Q_OBJECT

public:
    explicit FindBar(QWidget *parent = nullptr);

    void setTheme(const Theme &theme);

    /// Ctrl+F shows the search row alone, Ctrl+H adds the replace row.
    void setReplaceShown(bool on);
    bool replaceShown() const;

    /// Focus and select, so a second Ctrl+F retypes over the old query.
    void focusSearch();

    /// `current` is zero-based and -1 when nothing is selected; the label
    /// reads "3 of 17", "No results", or nothing at all for an empty query.
    void setStatus(int current, int total, bool patternValid);

    QString pattern() const;
    QString replacement() const;
    bool caseSensitive() const;
    bool useRegex() const;

signals:
    void queryChanged();
    /// +1 for the next match, -1 for the previous.
    void stepped(int direction);
    void replaceRequested();
    void replaceAllRequested();
    /// Escape, or the close button.
    void dismissed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void restyleSearchField();

    QLineEdit *m_search = nullptr;
    QLineEdit *m_replace = nullptr;
    QToolButton *m_case = nullptr;
    QToolButton *m_regex = nullptr;
    QToolButton *m_close = nullptr;
    QLabel *m_count = nullptr;
    QWidget *m_replaceRow = nullptr;

    QString m_fieldStyle;
    QString m_errorFieldStyle;
    bool m_valid = true;
};

} // namespace noteAppC
