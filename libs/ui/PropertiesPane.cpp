// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "PropertiesPane.h"

#include <algorithm>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QShowEvent>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Icons.h"
#include "Index.h"
#include "Theme.h"

namespace noteAppC {

namespace {

QString iconFor(Property::Type type)
{
    switch (type) {
    case Property::Type::List:
        return QStringLiteral("list");
    case Property::Type::Number:
        return QStringLiteral("hash");
    case Property::Type::Checkbox:
        return QStringLiteral("check");
    case Property::Type::Date:
    case Property::Type::DateTime:
        return QStringLiteral("calendar");
    default:
        return QStringLiteral("type");
    }
}

} // namespace

PropertiesPane::PropertiesPane(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("propertiesPane"));

    auto *headerRow = new QWidget(this);
    auto *headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(2);
    m_header = new QLabel(tr("Properties"), headerRow);
    m_header->setObjectName(QStringLiteral("paneHeader"));
    m_sortButton = new QToolButton(headerRow);
    m_sortButton->setObjectName(QStringLiteral("propertiesSortButton"));
    m_sortButton->setAutoRaise(true);
    m_sortButton->setCursor(Qt::PointingHandCursor);
    m_sortButton->setIconSize(QSize(14, 14));
    m_sortButton->setProperty("iconName", QStringLiteral("list"));
    m_sortButton->setToolTip(tr("Sorted by count - click for name order"));
    connect(m_sortButton, &QToolButton::clicked, this, [this] {
        m_sortByName = !m_sortByName;
        m_sortButton->setToolTip(m_sortByName ? tr("Sorted by name - click for count order")
                                              : tr("Sorted by count - click for name order"));
        refresh();
    });
    headerLayout->addWidget(m_header, 1);
    headerLayout->addWidget(m_sortButton);

    m_list = new QTreeWidget(this);
    m_list->setObjectName(QStringLiteral("backlinkList")); // the tags view's look
    m_list->setHeaderHidden(true);
    m_list->setColumnCount(2);
    m_list->setRootIsDecorated(false);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_list, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        emit propertyActivated(item->data(0, Qt::UserRole).toString());
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 6, 6);
    layout->setSpacing(6);
    layout->addWidget(headerRow);
    layout->addWidget(m_list, 1);
}

void PropertiesPane::setIndex(Index *index)
{
    m_index = index;
    if (m_index)
        connect(m_index, &Index::changed, this, &PropertiesPane::onIndexChanged);
    refresh();
}

void PropertiesPane::setTheme(const Theme &theme)
{
    m_sortButton->setIcon(Icons::icon(QStringLiteral("list"), theme.color(QStringLiteral("--text-muted")),
                                      m_sortButton->iconSize().width()));
    const QColor muted = theme.color(QStringLiteral("--text-faint"));
    const QColor icon = theme.color(QStringLiteral("--text-muted"));
    if (muted == m_mutedColor && icon == m_iconColor)
        return;
    m_mutedColor = muted;
    m_iconColor = icon;
    refresh();
}

void PropertiesPane::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_stale)
        refresh();
}

void PropertiesPane::onIndexChanged()
{
    // Every save lands here; a pane nobody can see waits until it is shown.
    if (isVisible())
        refresh();
    else
        m_stale = true;
}

void PropertiesPane::refresh()
{
    m_stale = false;
    m_list->clear();
    if (!m_index || !m_index->isOpen()) {
        m_header->setText(tr("Properties"));
        return;
    }
    QList<Index::PropertyCount> counts = m_index->propertyCounts();
    m_header->setText(tr("Properties  %1").arg(counts.size()));
    std::sort(counts.begin(), counts.end(), [this](const Index::PropertyCount &a, const Index::PropertyCount &b) {
        if (!m_sortByName && a.notes != b.notes)
            return a.notes > b.notes;
        return a.key.localeAwareCompare(b.key) < 0;
    });
    for (const Index::PropertyCount &count : std::as_const(counts)) {
        auto *item = new QTreeWidgetItem(m_list);
        item->setIcon(0, Icons::icon(iconFor(count.type), m_iconColor, 14));
        item->setText(0, count.key);
        item->setData(0, Qt::UserRole, count.key);
        item->setText(1, QString::number(count.notes));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setForeground(1, m_mutedColor);
    }
}

} // namespace noteAppC
