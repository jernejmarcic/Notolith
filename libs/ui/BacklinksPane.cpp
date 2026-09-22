#include "BacklinksPane.h"

#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Index.h"

namespace noteAppC {

BacklinksPane::BacklinksPane(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("rightSidebar"));

    m_header = new QLabel(tr("Linked mentions"), this);
    m_header->setObjectName(QStringLiteral("paneHeader"));

    m_list = new QTreeWidget(this);
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

void BacklinksPane::setIndex(Index *index)
{
    m_index = index;
    if (m_index)
        connect(m_index, &Index::changed, this, &BacklinksPane::refresh);
    refresh();
}

void BacklinksPane::setNote(const QString &path)
{
    m_path = path;
    refresh();
}

void BacklinksPane::refresh()
{
    m_list->clear();
    if (!m_index || !m_index->isOpen() || m_path.isEmpty()) {
        m_header->setText(tr("Linked mentions"));
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
}

} // namespace noteAppC
