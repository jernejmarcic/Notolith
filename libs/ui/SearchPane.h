// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QFutureWatcher>
#include <QWidget>

#include "Search.h"

class QLabel;
class QShowEvent;
class QLineEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace noteAppC {

class Index;
class Theme;

/// Obsidian's search pane: a debounced query box over Index::search(),
/// results grouped by note with the matched text bolded in each snippet and
/// a running total in the header.
///
/// Index::search() is written to be safe from any thread (it opens its own
/// SQLite connection), so every query runs on the thread pool via
/// QtConcurrent - a 20 000-note vault must never cost a keystroke's worth of
/// UI-thread time - and a generation counter drops whatever a superseded
/// search returns, the way the editor's own async parse already does.
class SearchPane : public QWidget {
    Q_OBJECT

public:
    explicit SearchPane(QWidget *parent = nullptr);
    ~SearchPane() override;

    void setIndex(Index *index);
    void setTheme(const Theme &theme);

    /// Sets the query text and searches immediately, skipping the debounce -
    /// what a caller handing over a ready-made query wants (TagsPane's
    /// tagActivated, say, becoming `tag:#work`) rather than typing it out.
    void setQuery(const QString &text);
    /// Hides the box for a host that feeds the query itself (the Meta notes
    /// pane's vault mode).
    void setQueryBoxVisible(bool visible);

signals:
    /// `offset` is into the target note's own raw text (UTF-16 code units);
    /// 0 when the match carries no position of its own, which happens only
    /// for a query that is entirely path:/file: fields.
    void matchActivated(const QString &relativePath, qsizetype offset);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onTextEdited(const QString &text);
    void onDebounceElapsed();
    void onSearchFinished();

private:
    void runSearch(const QString &text);
    void showResults(const QList<SearchResult> &results);
    void showMessage(const QString &text);
    void buildResultItem(const SearchResult &result);

    Index *m_index = nullptr;
    QLineEdit *m_queryEdit = nullptr;
    QLabel *m_header = nullptr;
    QTreeWidget *m_list = nullptr;
    QTimer *m_debounce = nullptr;
    QFutureWatcher<QList<SearchResult>> m_watcher;
    quint64 m_generation = 0;
    bool m_stale = false; ///< Index::changed arrived while hidden, with a query to rerun
    quint64 m_pendingGeneration = 0;
    QList<SearchResult> m_lastResults; ///< so a theme change can re-render without a new search
    QColor m_mutedColor;
    QColor m_highlightColor;
};

} // namespace noteAppC
