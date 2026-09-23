#include "SearchPane.h"

#include <algorithm>

#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QtConcurrent>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Index.h"
#include "Theme.h"

namespace noteAppC {

namespace {

constexpr int kDebounceMs = 150; // long enough to skip most keystrokes, short enough to feel live
constexpr int kResultLimit = 200;

/// The snippet with every highlight in `highlights` (offsets into `snippet`,
/// as SearchSnippet hands them over) wrapped in a coloured <b>. Everything
/// else is HTML-escaped, since a snippet is arbitrary note text.
QString highlightedSnippetHtml(const QString &snippet, QList<SearchMatch> highlights, const QColor &color)
{
    std::sort(highlights.begin(), highlights.end(),
              [](const SearchMatch &a, const SearchMatch &b) { return a.offset < b.offset; });

    QString html;
    qsizetype cursor = 0;
    for (const SearchMatch &match : highlights) {
        if (match.offset < cursor || match.offset + match.length > snippet.size())
            continue; // overlapping or out of range - keep the escaping simple and skip it
        html += snippet.mid(cursor, match.offset - cursor).toHtmlEscaped();
        html += QStringLiteral("<b style=\"color:%1;\">").arg(color.name());
        html += snippet.mid(match.offset, match.length).toHtmlEscaped();
        html += QStringLiteral("</b>");
        cursor = match.offset + match.length;
    }
    html += snippet.mid(cursor).toHtmlEscaped();
    return html;
}

} // namespace

SearchPane::SearchPane(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("searchPane"));

    m_queryEdit = new QLineEdit(this);
    // Shares the quick switcher's input styling: same look, no new QSS rule.
    m_queryEdit->setObjectName(QStringLiteral("quickSwitcherQuery"));
    m_queryEdit->setPlaceholderText(tr("Search the vault..."));
    m_queryEdit->setClearButtonEnabled(true);
    setFocusProxy(m_queryEdit); // "Search in all files" focuses the pane, and means the box

    m_header = new QLabel(tr("Search"), this);
    m_header->setObjectName(QStringLiteral("paneHeader"));

    m_list = new QTreeWidget(this);
    // Shares BacklinksPane's list styling, the way TagsPane already does.
    m_list->setObjectName(QStringLiteral("backlinkList"));
    m_list->setHeaderHidden(true);
    m_list->setColumnCount(2);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setIndentation(12);
    m_list->setWordWrap(true);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &SearchPane::onDebounceElapsed);

    connect(m_queryEdit, &QLineEdit::textChanged, this, &SearchPane::onTextEdited);
    connect(&m_watcher, &QFutureWatcher<QList<SearchResult>>::finished, this,
            &SearchPane::onSearchFinished);

    const auto activate = [this](QTreeWidgetItem *item) {
        if (!item)
            return;
        const QString path = item->data(0, Qt::UserRole).toString();
        if (path.isEmpty())
            return;
        emit matchActivated(path, qsizetype(item->data(0, Qt::UserRole + 1).toLongLong()));
    };
    connect(m_list, &QTreeWidget::itemActivated, this, activate);
    connect(m_list, &QTreeWidget::itemClicked, this, activate);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 6, 6);
    layout->setSpacing(6);
    layout->addWidget(m_queryEdit);
    layout->addWidget(m_header);
    layout->addWidget(m_list, 1);
}

SearchPane::~SearchPane()
{
    // The background lambda only ever captures `index` and `text` by value,
    // never `this`, so it is harmless to let it finish on its own - this just
    // keeps a search from quietly running past the widget that asked for it.
    m_watcher.waitForFinished();
}

void SearchPane::setIndex(Index *index)
{
    m_index = index;
    if (m_index) {
        connect(m_index, &Index::changed, this, [this] {
            if (!m_queryEdit->text().trimmed().isEmpty())
                runSearch(m_queryEdit->text());
        });
    }
}

void SearchPane::setTheme(const Theme &theme)
{
    m_mutedColor = theme.color(QStringLiteral("--text-faint"));
    m_highlightColor = theme.color(QStringLiteral("--text-accent"));
    showResults(m_lastResults); // re-render with the new colours, no new search
}

void SearchPane::setQuery(const QString &text)
{
    m_queryEdit->blockSignals(true);
    m_queryEdit->setText(text);
    m_queryEdit->blockSignals(false);
    m_debounce->stop();

    if (text.trimmed().isEmpty()) {
        showResults({});
        return;
    }
    const SearchQuery parsed = SearchQuery::parse(text);
    if (!parsed.isValid()) {
        showMessage(parsed.error());
        return;
    }
    runSearch(text);
}

void SearchPane::onTextEdited(const QString &text)
{
    if (text.trimmed().isEmpty()) {
        m_debounce->stop();
        showResults({});
        return;
    }
    // Parsing is pure CPU (no database), so validity feedback is immediate
    // even though the actual search waits out the debounce.
    const SearchQuery parsed = SearchQuery::parse(text);
    if (!parsed.isValid()) {
        m_debounce->stop();
        showMessage(parsed.error());
        return;
    }
    m_debounce->start();
}

void SearchPane::onDebounceElapsed()
{
    runSearch(m_queryEdit->text());
}

void SearchPane::runSearch(const QString &text)
{
    if (!m_index || !m_index->isOpen()) {
        showResults({});
        return;
    }
    m_pendingGeneration = ++m_generation;
    Index *index = m_index;
    m_watcher.setFuture(
        QtConcurrent::run([index, text]() { return index->search(text, kResultLimit); }));
}

void SearchPane::onSearchFinished()
{
    if (m_pendingGeneration != m_generation)
        return; // a newer search has already started; this one is stale
    showResults(m_watcher.result());
}

void SearchPane::showMessage(const QString &text)
{
    m_list->clear();
    m_lastResults.clear();
    m_header->setText(text);
}

void SearchPane::showResults(const QList<SearchResult> &results)
{
    m_lastResults = results;
    m_list->clear();

    if (m_queryEdit->text().trimmed().isEmpty()) {
        m_header->setText(tr("Search"));
        return;
    }

    int totalMatches = 0;
    for (const SearchResult &result : results)
        totalMatches += result.matchCount;
    m_header->setText(results.isEmpty()
                          ? tr("No results")
                          : tr("%1 notes, %2 matches").arg(results.size()).arg(totalMatches));

    for (const SearchResult &result : results)
        buildResultItem(result);
}

void SearchPane::buildResultItem(const SearchResult &result)
{
    const qsizetype offset = result.matches.isEmpty() ? 0 : result.matches.first().offset;

    auto *note = new QTreeWidgetItem(m_list);
    note->setText(0, QFileInfo(result.path).completeBaseName());
    note->setText(1, QString::number(result.matchCount));
    note->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    note->setForeground(1, m_mutedColor);
    note->setData(0, Qt::UserRole, result.path);
    note->setData(0, Qt::UserRole + 1, qlonglong(offset));
    note->setExpanded(true);

    auto *snippetRow = new QTreeWidgetItem(note);
    snippetRow->setFirstColumnSpanned(true);
    snippetRow->setData(0, Qt::UserRole, result.path);
    snippetRow->setData(0, Qt::UserRole + 1, qlonglong(offset));

    auto *snippetLabel = new QLabel(m_list);
    snippetLabel->setObjectName(QStringLiteral("searchSnippet"));
    snippetLabel->setTextFormat(Qt::RichText);
    snippetLabel->setWordWrap(true);
    snippetLabel->setContentsMargins(4, 0, 4, 4);
    snippetLabel->setText(highlightedSnippetHtml(result.snippet, result.snippetHighlights, m_highlightColor));
    m_list->setItemWidget(snippetRow, 0, snippetLabel);
}

} // namespace noteAppC
