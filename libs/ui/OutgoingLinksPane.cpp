#include "OutgoingLinksPane.h"

#include <QFileInfo>
#include <QHash>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Index.h"
#include "Metadata.h"
#include "SourceMap.h"
#include "Theme.h"
#include "Vault.h"

namespace noteAppC {

namespace {

/// The trimmed source line a link sits on; empty if the note can't be read
/// (deleted since the index ran, say), so the caller can fall back to the
/// link markup itself.
QString contextLine(const QString &notePath, const Range &range)
{
    Note note;
    if (!range.isValid() || !Vault::read(notePath, note))
        return {};
    const SourceMap map(note.text);
    const qsizetype line = map.lineAt(range.start);
    return note.text.mid(map.lineStart(line), map.lineEnd(line) - map.lineStart(line)).trimmed();
}

} // namespace

OutgoingLinksPane::OutgoingLinksPane(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("outgoingLinksPane"));

    m_header = new QLabel(tr("Outgoing links"), this);
    m_header->setObjectName(QStringLiteral("paneHeader"));

    m_list = new QTreeWidget(this);
    // Shares BacklinksPane's list styling: same object name, same look.
    m_list->setObjectName(QStringLiteral("backlinkList"));
    m_list->setHeaderHidden(true);
    m_list->setColumnCount(1);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setIndentation(12);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setWordWrap(true);

    connect(m_list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        const QString path = item->data(0, Qt::UserRole).toString();
        if (!path.isEmpty())
            emit noteActivated(path);
    });
    connect(m_list, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        const QString path = item->data(0, Qt::UserRole).toString();
        if (!path.isEmpty())
            emit noteActivated(path);
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 6, 6);
    layout->setSpacing(6);
    layout->addWidget(m_header);
    layout->addWidget(m_list, 1);
}

void OutgoingLinksPane::setIndex(Index *index)
{
    m_index = index;
    if (m_index)
        connect(m_index, &Index::changed, this, &OutgoingLinksPane::refresh);
    refresh();
}

void OutgoingLinksPane::setTheme(const Theme &theme)
{
    m_faintColor = theme.color(QStringLiteral("--text-faint"));
    refresh();
}

void OutgoingLinksPane::setNote(const QString &path)
{
    m_path = path;
    refresh();
}

void OutgoingLinksPane::refresh()
{
    m_list->clear();
    if (!m_index || !m_index->isOpen() || m_path.isEmpty()) {
        m_header->setText(tr("Outgoing links"));
        return;
    }

    const auto links = m_index->outgoingLinks(m_path);

    QHash<QString, QTreeWidgetItem *> byTarget;
    QStringList unresolvedRows;
    int shownCount = 0;

    for (const Link &link : links) {
        if (link.target.isEmpty())
            continue; // a link into this note's own headings, nothing to show here
        if (link.external)
            continue; // http(s) links aren't part of the vault graph - Index::backlinks() excludes them too
        ++shownCount;

        const QString resolved = m_index->resolve(link.target, m_path);
        const QString context = contextLine(m_path, link.range);
        const QString shown = link.alias.isEmpty() ? link.target : link.alias;
        const QString text = context.isEmpty()
            ? (link.embed ? tr("embed: %1").arg(shown) : QStringLiteral("[[%1]]").arg(shown))
            : context;

        if (resolved.isEmpty()) {
            unresolvedRows.append(text);
            continue;
        }

        QTreeWidgetItem *group = byTarget.value(resolved);
        if (!group) {
            group = new QTreeWidgetItem(m_list);
            group->setText(0, QFileInfo(resolved).completeBaseName());
            group->setData(0, Qt::UserRole, resolved);
            group->setExpanded(true);
            byTarget.insert(resolved, group);
        }

        auto *child = new QTreeWidgetItem(group);
        child->setText(0, text);
        child->setData(0, Qt::UserRole, resolved);
    }

    // Unresolved links get their own group, styled dim, always last.
    if (!unresolvedRows.isEmpty()) {
        auto *group = new QTreeWidgetItem(m_list);
        group->setText(0, tr("Unresolved links"));
        group->setExpanded(true);
        group->setForeground(0, m_faintColor);
        for (const QString &text : unresolvedRows) {
            auto *child = new QTreeWidgetItem(group);
            child->setText(0, text);
            child->setForeground(0, m_faintColor);
        }
    }

    m_header->setText(tr("Outgoing links  %1").arg(shownCount));
}

} // namespace noteAppC
