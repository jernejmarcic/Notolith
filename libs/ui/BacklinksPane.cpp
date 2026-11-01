// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "BacklinksPane.h"

#include <algorithm>

#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QShowEvent>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Index.h"
#include "Theme.h"

namespace noteAppC {

namespace {

/// `snippet` with every offset in `highlights` wrapped in a coloured <b>,
/// everything else HTML-escaped - the same small helper SearchPane.cpp has,
/// kept separate rather than shared out of libs/index: it needs QColor
/// (QtGui), and libs/index deliberately stops at QtCore/QtSql.
QString highlightedSnippetHtml(const QString &snippet, QList<SearchMatch> highlights, const QColor &color)
{
    std::sort(highlights.begin(), highlights.end(),
              [](const SearchMatch &a, const SearchMatch &b) { return a.offset < b.offset; });

    QString html;
    qsizetype cursor = 0;
    for (const SearchMatch &match : highlights) {
        if (match.offset < cursor || match.offset + match.length > snippet.size())
            continue;
        html += snippet.mid(cursor, match.offset - cursor).toHtmlEscaped();
        html += QStringLiteral("<b style=\"color:%1;\">").arg(color.name());
        html += snippet.mid(match.offset, match.length).toHtmlEscaped();
        html += QStringLiteral("</b>");
        cursor = match.offset + match.length;
    }
    html += snippet.mid(cursor).toHtmlEscaped();
    return html;
}

QTreeWidget *buildMentionList(QWidget *parent)
{
    auto *list = new QTreeWidget(parent);
    list->setObjectName(QStringLiteral("backlinkList"));
    list->setHeaderHidden(true);
    list->setColumnCount(1);
    list->setFrameShape(QFrame::NoFrame);
    list->setIndentation(12);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setWordWrap(true);
    return list;
}

} // namespace

BacklinksPane::BacklinksPane(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("backlinksPane"));

    m_header = new QLabel(tr("Linked mentions"), this);
    m_header->setObjectName(QStringLiteral("paneHeader"));
    m_list = buildMentionList(this);

    m_unlinkedHeader = new QLabel(tr("Unlinked mentions"), this);
    m_unlinkedHeader->setObjectName(QStringLiteral("paneHeader"));
    m_unlinkedList = buildMentionList(this);

    const auto activate = [this](QTreeWidgetItem *item) {
        const QString path = item->data(0, Qt::UserRole).toString();
        if (!path.isEmpty())
            emit noteActivated(path);
    };
    connect(m_list, &QTreeWidget::itemActivated, this, activate);
    connect(m_list, &QTreeWidget::itemClicked, this, activate);
    connect(m_unlinkedList, &QTreeWidget::itemActivated, this, activate);
    connect(m_unlinkedList, &QTreeWidget::itemClicked, this, activate);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 6, 6);
    layout->setSpacing(6);
    layout->addWidget(m_header);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_unlinkedHeader);
    layout->addWidget(m_unlinkedList, 1);
}

void BacklinksPane::setIndex(Index *index)
{
    m_index = index;
    if (m_index)
        connect(m_index, &Index::changed, this, &BacklinksPane::onIndexChanged);
    refresh();
}

void BacklinksPane::setTheme(const Theme &theme)
{
    const QColor highlight = theme.color(QStringLiteral("--text-accent"));
    if (highlight == m_highlightColor)
        return; // every settings pass re-applies the theme
    m_highlightColor = highlight;
    refreshUnlinked();
}

void BacklinksPane::setNote(const QString &path)
{
    m_path = path;
    refresh();
}

void BacklinksPane::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_stale)
        refresh();
}

void BacklinksPane::onIndexChanged()
{
    // Every save lands here; a pane nobody can see waits until it is shown.
    if (isVisible())
        refresh();
    else
        m_stale = true;
}

void BacklinksPane::refresh()
{
    m_stale = false;
    m_list->clear();
    if (!m_index || !m_index->isOpen() || m_path.isEmpty()) {
        m_header->setText(tr("Linked mentions"));
        refreshUnlinked();
        return;
    }

    const auto backlinks = m_index->backlinks(m_path);
    m_header->setText(tr("Linked mentions  %1").arg(backlinks.size()));

    QHash<QString, QTreeWidgetItem *> byNote;
    for (const Backlink &backlink : backlinks) {
        QTreeWidgetItem *parent = byNote.value(backlink.fromPath);
        if (!parent) {
            parent = new QTreeWidgetItem(m_list);
            parent->setText(0, QFileInfo(backlink.fromPath).completeBaseName());
            parent->setData(0, Qt::UserRole, backlink.fromPath);
            parent->setExpanded(true);
            byNote.insert(backlink.fromPath, parent);
        }

        auto *child = new QTreeWidgetItem(parent);
        const QString shown = backlink.link.alias.isEmpty() ? backlink.link.target
                                                            : backlink.link.alias;
        child->setText(0, backlink.link.embed ? tr("embed: %1").arg(shown)
                                              : QStringLiteral("[[%1]]").arg(shown));
        child->setData(0, Qt::UserRole, backlink.fromPath);
    }

    refreshUnlinked();
}

void BacklinksPane::setUnlinkedMentions(bool on)
{
    if (on == m_unlinked)
        return;
    m_unlinked = on;
    m_unlinkedHeader->setVisible(on);
    m_unlinkedList->setVisible(on);
    refreshUnlinked();
}

void BacklinksPane::refreshUnlinked()
{
    m_unlinkedList->clear();
    if (!m_unlinked)
        return;
    if (!m_index || !m_index->isOpen() || m_path.isEmpty()) {
        m_unlinkedHeader->setText(tr("Unlinked mentions"));
        return;
    }

    const QList<SearchResult> mentions = m_index->unlinkedMentions(m_path);
    m_unlinkedHeader->setText(tr("Unlinked mentions  %1").arg(mentions.size()));

    for (const SearchResult &mention : mentions) {
        auto *parent = new QTreeWidgetItem(m_unlinkedList);
        parent->setText(0, QFileInfo(mention.path).completeBaseName());
        parent->setData(0, Qt::UserRole, mention.path);
        parent->setExpanded(true);

        auto *snippetRow = new QTreeWidgetItem(parent);
        snippetRow->setFirstColumnSpanned(true);
        snippetRow->setData(0, Qt::UserRole, mention.path);

        auto *label = new QLabel(m_unlinkedList);
        label->setObjectName(QStringLiteral("searchSnippet"));
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        label->setContentsMargins(4, 0, 4, 4);
        label->setText(highlightedSnippetHtml(mention.snippet, mention.snippetHighlights, m_highlightColor));
        m_unlinkedList->setItemWidget(snippetRow, 0, label);
    }
}

} // namespace noteAppC
