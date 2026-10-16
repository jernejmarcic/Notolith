#include "TagsPane.h"

#include <algorithm>
#include <vector>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Icons.h"
#include "Index.h"
#include "Theme.h"

namespace noteAppC {

namespace {

/// One level of the '/'-nested tag tree, rebuilt fresh on every refresh().
struct TagNode {
    QString name;     ///< this segment only
    QString fullPath; ///< dotted with '/', the key of Index::tagCounts()
    int count = 0;
    std::vector<TagNode> children;
};

TagNode &childFor(TagNode &parent, const QString &name, const QString &fullPath)
{
    for (TagNode &child : parent.children) {
        if (child.name == name)
            return child;
    }
    parent.children.push_back(TagNode{name, fullPath, 0, {}});
    return parent.children.back();
}

void insertTag(TagNode &root, const QString &tag)
{
    TagNode *node = &root;
    QString path;
    for (const QString &segment : tag.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        path = path.isEmpty() ? segment : path + QLatin1Char('/') + segment;
        node = &childFor(*node, segment, path);
    }
}

void computeCounts(TagNode &node, const QHash<QString, int> &counts)
{
    // Index::tagCounts() already folds a tag's nested children into it, the
    // way notesWithTag() matches them, so a parent row needs no summing here.
    if (!node.fullPath.isEmpty())
        node.count = counts.value(node.fullPath);
    for (TagNode &child : node.children)
        computeCounts(child, counts);
}

void populateNode(QTreeWidgetItem *parentItem, QTreeWidget *list, std::vector<TagNode> &nodes,
                  bool sortByName, const QColor &mutedColor)
{
    std::sort(nodes.begin(), nodes.end(), [sortByName](const TagNode &a, const TagNode &b) {
        if (!sortByName && a.count != b.count)
            return a.count > b.count;
        return a.name.localeAwareCompare(b.name) < 0;
    });
    for (TagNode &node : nodes) {
        auto *item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(list);
        item->setText(0, node.name);
        item->setText(1, QString::number(node.count));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setForeground(1, mutedColor);
        item->setData(0, Qt::UserRole, node.fullPath);
        item->setExpanded(true);
        populateNode(item, list, node.children, sortByName, mutedColor);
    }
}

} // namespace

TagsPane::TagsPane(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("tagsPane"));

    auto *headerRow = new QWidget(this);
    auto *headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(2);

    m_header = new QLabel(tr("Tags"), headerRow);
    m_header->setObjectName(QStringLiteral("paneHeader"));

    m_sortButton = new QToolButton(headerRow);
    m_sortButton->setObjectName(QStringLiteral("tagsSortButton"));
    m_sortButton->setAutoRaise(true);
    m_sortButton->setCursor(Qt::PointingHandCursor);
    m_sortButton->setIconSize(QSize(14, 14));
    m_sortButton->setProperty("iconName", QStringLiteral("list"));
    m_sortButton->setToolTip(tr("Sorted by count - click for name order"));
    connect(m_sortButton, &QToolButton::clicked, this, &TagsPane::toggleSort);

    headerLayout->addWidget(m_header, 1);
    headerLayout->addWidget(m_sortButton);

    m_list = new QTreeWidget(this);
    // Shares BacklinksPane's list styling: same object name, same look.
    m_list->setObjectName(QStringLiteral("backlinkList"));
    m_list->setHeaderHidden(true);
    m_list->setColumnCount(2);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setIndentation(12);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    connect(m_list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        const QString tag = item->data(0, Qt::UserRole).toString();
        if (!tag.isEmpty())
            emit tagActivated(tag);
    });
    connect(m_list, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        const QString tag = item->data(0, Qt::UserRole).toString();
        if (!tag.isEmpty())
            emit tagActivated(tag);
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 6, 6);
    layout->setSpacing(6);
    layout->addWidget(headerRow);
    layout->addWidget(m_list, 1);
}

void TagsPane::setIndex(Index *index)
{
    m_index = index;
    if (m_index)
        connect(m_index, &Index::changed, this, &TagsPane::refresh);
    refresh();
}

void TagsPane::setTheme(const Theme &theme)
{
    m_sortButton->setIcon(Icons::icon(QStringLiteral("list"),
                                      theme.color(QStringLiteral("--text-muted")),
                                      m_sortButton->iconSize().width()));
    // Every settings pass re-applies the theme; only a new colour needs the
    // list rebuilt, which is a query over every tag in the vault.
    const QColor muted = theme.color(QStringLiteral("--text-faint"));
    if (muted == m_mutedColor)
        return;
    m_mutedColor = muted;
    refresh();
}

void TagsPane::toggleSort()
{
    m_sortByName = !m_sortByName;
    m_sortButton->setToolTip(m_sortByName ? tr("Sorted by name - click for count order")
                                          : tr("Sorted by count - click for name order"));
    refresh();
}

void TagsPane::refresh()
{
    m_list->clear();
    if (!m_index || !m_index->isOpen()) {
        m_header->setText(tr("Tags"));
        return;
    }

    const QStringList tags = m_index->allTags();
    m_header->setText(tr("Tags  %1").arg(tags.size()));

    TagNode root;
    for (const QString &tag : tags)
        insertTag(root, tag);
    computeCounts(root, m_index->tagCounts());
    populateNode(nullptr, m_list, root.children, m_sortByName, m_mutedColor);
}

} // namespace noteAppC
