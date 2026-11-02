// SPDX-License-Identifier: GPL-3.0-or-later
#include "MetaNotesPane.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "SearchPane.h"
#include "Theme.h"

namespace noteAppC {

namespace {

constexpr qsizetype kBefore = 18;       ///< characters of context before the run, within the line: the row is narrow,
                                        ///< and the run must come into view before the clip
constexpr qsizetype kAfter = 40;        ///< and after it
constexpr qsizetype kInnerLimit = 90;   ///< a longer run is elided; the tooltip has all of it
constexpr int kRunData = Qt::UserRole;  ///< run start; +1 is its end

QString elided(const QString &text, qsizetype limit)
{
    return text.size() > limit ? text.left(limit - 1) + QChar(0x2026) : text;
}

QString cssColor(const QColor &color)
{
    return color.name(QColor::HexArgb);
}

} // namespace

MetaNotesPane::MetaNotesPane(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("metaNotesPane"));

    m_header = new QLabel(tr("Meta notes"), this);
    m_header->setObjectName(QStringLiteral("paneHeader"));

    m_filter = new QLineEdit(this);
    // Shares the quick switcher's input styling (Theme.cpp lists this name too).
    m_filter->setObjectName(QStringLiteral("metaNotesFilter"));
    m_filter->setPlaceholderText(tr("Filter meta notes"));
    m_filter->setClearButtonEnabled(true);
    m_filter->installEventFilter(this); // Down moves into the list
    setFocusProxy(m_filter);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("backlinkList")); // the other panes' look
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(1);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setIndentation(12);
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tree->setUniformRowHeights(false);

    m_empty = new QLabel(this);
    m_empty->setObjectName(QStringLiteral("metaNotesEmpty"));
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_empty->setContentsMargins(2, 4, 2, 0);

    const auto activate = [this](QTreeWidgetItem *item) {
        if (!item || !item->data(0, kRunData).isValid())
            return; // a heading group, not a run
        emit jumpRequested({qsizetype(item->data(0, kRunData).toLongLong()),
                            qsizetype(item->data(0, kRunData + 1).toLongLong())});
    };
    connect(m_tree, &QTreeWidget::itemActivated, this, activate);
    connect(m_tree, &QTreeWidget::itemClicked, this, activate);
    connect(m_filter, &QLineEdit::textChanged, this, [this] {
        populate();
        if (vaultMode())
            m_debounce->start();
    });

    m_vaultButton = new QToolButton(this);
    m_vaultButton->setObjectName(QStringLiteral("metaNotesVaultButton"));
    m_vaultButton->setText(tr("Vault"));
    m_vaultButton->setToolTip(tr("List the notes of the whole vault that have meta notes"));
    m_vaultButton->setCheckable(true);
    m_vaultButton->setAutoRaise(true);
    m_vaultButton->setCursor(Qt::PointingHandCursor);
    m_vaultButton->setFocusPolicy(Qt::NoFocus);
    connect(m_vaultButton, &QToolButton::toggled, this, &MetaNotesPane::setVaultMode);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(150); // as the search pane's own box
    connect(m_debounce, &QTimer::timeout, this, &MetaNotesPane::runVaultQuery);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 6, 6);
    layout->setSpacing(6);
    layout->addWidget(m_header);
    auto *filterRow = new QHBoxLayout;
    filterRow->setSpacing(4);
    filterRow->addWidget(m_filter, 1);
    filterRow->addWidget(m_vaultButton);
    layout->addLayout(filterRow);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_empty, 1);

    setTheme(Theme::builtin());
    populate();
}

bool MetaNotesPane::sameDisplay(const QList<Row> &a, const QList<Row> &b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        const Row &l = a.at(i);
        const Row &r = b.at(i);
        if (l.group != r.group || l.line != r.line || l.heading != r.heading || l.inner != r.inner
            || l.before != r.before || l.run != r.run || l.after != r.after)
            return false;
    }
    return true;
}

void MetaNotesPane::setNote(const QString &text, const QList<Range> &runs, const QList<Heading> &headings)
{
    QList<Row> rows;
    rows.reserve(runs.size());
    int group = -1;
    qsizetype counted = 0; // one running newline count, not one per run
    qsizetype line = 1;
    for (const Range &range : runs) {
        if (range.start < counted || range.end > text.size())
            continue; // stale offsets between an edit and the reparse
        while (group + 1 < headings.size() && headings.at(group + 1).range.start <= range.start)
            ++group;
        for (; counted < range.start; ++counted)
            line += text.at(counted) == u'\n';

        Row row;
        row.group = group;
        row.heading = group >= 0 ? headings.at(group).text : QString();
        row.line = line;
        row.range = range;
        row.run = text.mid(range.start, range.length()).simplified();
        QString inner = text.mid(range.start, range.length());
        if (inner.startsWith(m_open) && inner.endsWith(m_close) && inner.size() >= m_open.size() + m_close.size())
            inner = inner.mid(m_open.size(), inner.size() - m_open.size() - m_close.size());
        row.inner = inner.simplified();

        const qsizetype lineStart = range.start == 0 ? 0 : text.lastIndexOf(u'\n', range.start - 1) + 1;
        const qsizetype newline = text.indexOf(u'\n', range.end);
        const qsizetype lineEnd = newline < 0 ? text.size() : newline;
        const qsizetype from = qMax(lineStart, range.start - kBefore);
        QString before = text.mid(from, range.start - from);
        if (from > lineStart) // cut mid-word: start at the next whole one
            before = QChar(0x2026) + before.mid(qMax<qsizetype>(0, before.indexOf(u' ')));
        row.before = before.simplified();
        const qsizetype to = qMin(lineEnd, range.end + kAfter);
        row.after = text.mid(range.end, to - range.end).simplified() + (to < lineEnd ? QString(QChar(0x2026)) : QString());
        rows.append(row);
    }

    if (sameDisplay(rows, m_rows)) {
        // Typing above or between the runs only moved them: point the rows at
        // the new offsets and leave every label alone.
        for (qsizetype i = 0; i < m_rows.size(); ++i)
            m_rows[i].range = rows.at(i).range;
        for (qsizetype i = 0; i < m_items.size(); ++i) {
            const Range &range = m_rows.at(m_itemRow.at(i)).range;
            m_items.at(i)->setData(0, kRunData, qlonglong(range.start));
            m_items.at(i)->setData(0, kRunData + 1, qlonglong(range.end));
        }
        updateHighlight();
        return;
    }
    if (sameShape(rows, m_rows)) {
        // Typing inside a run changed its words, not the list: rewrite those
        // rows' labels in place (a populate() is a widget per row, per parse).
        m_rows = rows;
        for (qsizetype i = 0; i < m_items.size(); ++i) {
            const Row &row = m_rows.at(m_itemRow.at(i));
            QTreeWidgetItem *item = m_items.at(i);
            item->setData(0, kRunData, qlonglong(row.range.start));
            item->setData(0, kRunData + 1, qlonglong(row.range.end));
            item->setToolTip(0, row.run);
            if (auto *label = qobject_cast<QLabel *>(m_tree->itemWidget(item, 0))) {
                const QString html = rowHTML(row);
                if (label->text() != html)
                    label->setText(html);
            }
        }
        updateHighlight();
        return;
    }
    m_rows = rows;
    populate();
}

bool MetaNotesPane::sameShape(const QList<Row> &a, const QList<Row> &b) const
{
    if (a.size() != b.size())
        return false;
    const QString needle = m_filter->text().trimmed();
    for (qsizetype i = 0; i < a.size(); ++i) {
        const Row &l = a.at(i);
        const Row &r = b.at(i);
        if (l.group != r.group || l.line != r.line || l.heading != r.heading)
            return false;
        if (!needle.isEmpty()
            && l.inner.contains(needle, Qt::CaseInsensitive) != r.inner.contains(needle, Qt::CaseInsensitive))
            return false; // the filter shows another set of rows
    }
    return true;
}

void MetaNotesPane::clear()
{
    m_rows.clear();
    populate();
}

void MetaNotesPane::setCursorOffset(qsizetype sourceOffset)
{
    m_cursor = sourceOffset;
    updateHighlight();
}

void MetaNotesPane::setDelimiters(const QString &open, const QString &close)
{
    if (open == m_open && close == m_close)
        return;
    m_open = open;
    m_close = close;
    // The rows were stripped with the old pair; the next parse rebuilds them,
    // and until then only the empty-state hint can be wrong.
    populate();
}

void MetaNotesPane::setTheme(const Theme &theme)
{
    m_theme = theme;
    if (m_search)
        m_search->setTheme(theme);
    const QColor normal = theme.color(QStringLiteral("--text-normal"));
    const QColor muted = theme.color(QStringLiteral("--text-faint"));
    const QColor meta = theme.color(QStringLiteral("--meta-note-color"));
    const QColor background = theme.color(QStringLiteral("--background-modifier-active"));
    if (normal == m_normal && muted == m_muted && meta == m_meta && background == m_highlightBackground)
        return; // every settings pass re-applies the theme
    m_normal = normal;
    m_muted = muted;
    m_meta = meta;
    m_highlightBackground = background;
    populate();
}

void MetaNotesPane::setIndex(Index *index)
{
    m_index = index;
    if (m_search)
        m_search->setIndex(index);
}

bool MetaNotesPane::vaultMode() const
{
    return m_vaultButton->isChecked();
}

void MetaNotesPane::setVaultMode(bool on)
{
    if (m_vaultButton->isChecked() != on) {
        m_vaultButton->setChecked(on); // toggled() calls back here
        return;
    }
    if (on && !m_search) {
        // Built on the first press: a writer who never asks for the vault view
        // never pays for it. It has names of its own so the window's Search
        // pane stays the only "searchPane" for findChild and the tools.
        m_search = new SearchPane(this);
        m_search->setObjectName(QStringLiteral("metaVaultSearch"));
        m_search->findChild<QLineEdit *>()->setObjectName(QStringLiteral("metaVaultQuery"));
        m_search->findChild<QTreeWidget *>()->setObjectName(QStringLiteral("metaVaultList"));
        m_search->setQueryBoxVisible(false);
        m_search->setIndex(m_index);
        m_search->setTheme(m_theme);
        connect(m_search, &SearchPane::matchActivated, this, &MetaNotesPane::noteActivated);
        static_cast<QVBoxLayout *>(layout())->addWidget(m_search, 1);
    }
    updateMode();
    if (on)
        runVaultQuery();
}

void MetaNotesPane::runVaultQuery()
{
    if (!m_search)
        return;
    const QString filter = m_filter->text().trimmed();
    // A phrase, as this note's filter matches it: the text to find, not search
    // syntax. Limit: a trailing backslash escapes the closing quote.
    QString quoted = filter;
    quoted.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    m_search->setQuery(filter.isEmpty() ? QStringLiteral("meta:") : QStringLiteral("meta:\"%1\"").arg(quoted));
}

void MetaNotesPane::updateMode()
{
    const bool vault = vaultMode();
    const bool any = m_tree->topLevelItemCount() > 0;
    m_header->setVisible(!vault); // the search pane has its own header, "N notes"
    m_tree->setVisible(!vault && any);
    m_empty->setVisible(!vault && !any);
    if (m_search)
        m_search->setVisible(vault);
}

QString MetaNotesPane::rowHTML(const Row &row) const
{
    const QString inner = row.inner.isEmpty() ? tr("(empty meta note)") : elided(row.inner, kInnerLimit);
    QString html = QStringLiteral("<span style=\"color:%1;\">%2</span><br>")
                       .arg(cssColor(m_normal), inner.toHtmlEscaped());
    html += QStringLiteral("<span style=\"color:%1;font-size:11px;\">L%2 &middot; %3<span style=\"color:%4;\">%5</span>%6</span>")
                .arg(cssColor(m_muted), QString::number(row.line),
                     row.before.isEmpty() ? QString() : row.before.toHtmlEscaped() + QLatin1Char(' '),
                     cssColor(m_meta), elided(row.run, kInnerLimit).toHtmlEscaped(),
                     row.after.isEmpty() ? QString() : QLatin1Char(' ') + row.after.toHtmlEscaped());
    return html;
}

void MetaNotesPane::populate()
{
    m_tree->clear();
    m_items.clear();
    m_itemRow.clear();
    m_highlighted = -1;

    const QString needle = m_filter->text().trimmed();
    QTreeWidgetItem *groupItem = nullptr;
    int groupIndex = -2;
    for (qsizetype i = 0; i < m_rows.size(); ++i) {
        const Row &row = m_rows.at(i);
        if (!needle.isEmpty() && !row.inner.contains(needle, Qt::CaseInsensitive))
            continue;
        if (row.group != groupIndex) {
            groupIndex = row.group;
            groupItem = nullptr;
            if (row.group >= 0) {
                groupItem = new QTreeWidgetItem(m_tree);
                groupItem->setText(0, row.heading.isEmpty() ? tr("(untitled heading)") : row.heading);
                groupItem->setForeground(0, m_muted);
                groupItem->setFlags(Qt::ItemIsEnabled);
                groupItem->setExpanded(true);
            }
        }
        auto *item = groupItem ? new QTreeWidgetItem(groupItem) : new QTreeWidgetItem(m_tree);
        item->setData(0, kRunData, qlonglong(row.range.start));
        item->setData(0, kRunData + 1, qlonglong(row.range.end));
        item->setToolTip(0, row.run);

        auto *label = new QLabel(m_tree);
        label->setTextFormat(Qt::RichText);
        label->setContentsMargins(4, 2, 4, 2);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setText(rowHTML(row));
        m_tree->setItemWidget(item, 0, label);
        item->setSizeHint(0, QSize(0, label->sizeHint().height() + 10));
        m_items.append(item);
        m_itemRow.append(i);
    }

    const bool any = !m_items.isEmpty();
    m_header->setText(m_rows.isEmpty()   ? tr("Meta notes")
                      : needle.isEmpty() ? tr("Meta notes  %1").arg(m_rows.size())
                                         : tr("Meta notes  %1 of %2").arg(m_items.size()).arg(m_rows.size()));
    updateMode();
    m_empty->setStyleSheet(QStringLiteral("color:%1;").arg(cssColor(m_muted)));
    m_empty->setText(m_rows.isEmpty() ? tr("No meta notes in this note. Write one between %1 and %2.").arg(m_open, m_close)
                                      : tr("No meta notes match."));
    updateHighlight();
}

void MetaNotesPane::updateHighlight()
{
    int next = -1;
    for (qsizetype i = 0; i < m_items.size(); ++i) {
        const Range &range = m_rows.at(m_itemRow.at(i)).range;
        if (range.start <= m_cursor && m_cursor <= range.end) {
            next = int(i);
            break;
        }
    }
    if (next == m_highlighted)
        return;

    const auto paint = [this](int index, bool on) {
        if (index < 0)
            return;
        auto *label = qobject_cast<QLabel *>(m_tree->itemWidget(m_items.at(index), 0));
        if (!label)
            return;
        label->setProperty("current", on);
        label->setStyleSheet(on ? QStringLiteral("QLabel { background: %1; border-radius: 4px; }")
                                      .arg(cssColor(m_highlightBackground))
                                : QString());
    };
    paint(m_highlighted, false);
    m_highlighted = next;
    paint(m_highlighted, true);
    if (m_highlighted >= 0)
        m_tree->scrollToItem(m_items.at(m_highlighted));
}

bool MetaNotesPane::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_filter && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Down && !m_items.isEmpty()) {
        m_tree->setFocus();
        if (!m_tree->currentItem())
            m_tree->setCurrentItem(m_items.first());
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace noteAppC
