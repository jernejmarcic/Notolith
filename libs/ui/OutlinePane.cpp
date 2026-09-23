#include "OutlinePane.h"

#include <QFont>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <utility>
#include <vector>

namespace noteAppC {

namespace {

bool headingsEqual(const QList<Heading> &a, const QList<Heading> &b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (a[i].level != b[i].level || a[i].range != b[i].range || a[i].text != b[i].text)
            return false;
    }
    return true;
}

} // namespace

OutlinePane::OutlinePane(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("outlinePane"));

    m_header = new QLabel(tr("Outline"), this);
    m_header->setObjectName(QStringLiteral("paneHeader"));

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("outlineTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(1);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setIndentation(12);
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tree->setWordWrap(true);

    const auto activate = [this](QTreeWidgetItem *item) {
        if (!item)
            return;
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index >= 0 && index < m_headings.size())
            emit jumpRequested(m_headings.at(index).range.start);
    };
    connect(m_tree, &QTreeWidget::itemActivated, this, activate);
    connect(m_tree, &QTreeWidget::itemClicked, this, activate);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 6, 6);
    layout->setSpacing(6);
    layout->addWidget(m_header);
    layout->addWidget(m_tree, 1);
}

void OutlinePane::setHeadings(const QList<Heading> &headings)
{
    if (headingsEqual(headings, m_headings)) {
        // Same headings as last time - most parses, since typing prose does
        // not add or remove one. Rebuilding here would reset every collapsed
        // branch on every keystroke.
        return;
    }
    m_headings = headings;
    rebuildTree();
    updateHighlight();
}

void OutlinePane::clear()
{
    m_headings.clear();
    m_itemsByHeading.clear();
    m_tree->clear();
    m_highlighted = -1;
    m_header->setText(tr("Outline"));
}

void OutlinePane::setCursorOffset(qsizetype sourceOffset)
{
    m_cursor = sourceOffset;
    updateHighlight();
}

void OutlinePane::rebuildTree()
{
    m_tree->clear();
    m_itemsByHeading.clear();
    m_highlighted = -1;
    m_header->setText(m_headings.isEmpty() ? tr("Outline") : tr("Outline  %1").arg(m_headings.size()));

    // A stack of the open ancestors, closest first. A heading closes every
    // entry whose level is not shallower than its own before it looks for a
    // parent, which is how a level skipped in the source (h2 straight to h4)
    // still nests sensibly instead of erroring.
    std::vector<std::pair<int, QTreeWidgetItem *>> open;
    for (qsizetype i = 0; i < m_headings.size(); ++i) {
        const Heading &heading = m_headings.at(i);
        while (!open.empty() && open.back().first >= heading.level)
            open.pop_back();

        QTreeWidgetItem *parent = open.empty() ? nullptr : open.back().second;
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        item->setText(0, heading.text.isEmpty() ? tr("(untitled heading)") : heading.text);
        item->setData(0, Qt::UserRole, int(i));
        item->setExpanded(true);
        m_itemsByHeading.insert(int(i), item);

        open.emplace_back(heading.level, item);
    }
}

void OutlinePane::updateHighlight()
{
    const int next = headingAt(m_cursor);
    if (next == m_highlighted)
        return;

    if (m_highlighted >= 0) {
        if (QTreeWidgetItem *previous = m_itemsByHeading.value(m_highlighted)) {
            QFont font = previous->font(0);
            font.setBold(false);
            previous->setFont(0, font);
        }
    }
    m_highlighted = next;
    if (m_highlighted >= 0) {
        if (QTreeWidgetItem *current = m_itemsByHeading.value(m_highlighted)) {
            QFont font = current->font(0);
            font.setBold(true);
            current->setFont(0, font);
            m_tree->scrollToItem(current);
        }
    }
}

int OutlinePane::headingAt(qsizetype offset) const
{
    int found = -1;
    for (qsizetype i = 0; i < m_headings.size(); ++i) {
        if (m_headings.at(i).range.start > offset)
            break;
        found = int(i);
    }
    return found;
}

} // namespace noteAppC
