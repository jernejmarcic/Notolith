// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "FootnotesPane.h"

#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

namespace noteAppC {

namespace {

bool sameEntries(const QList<MarkdownEditor::FootnoteEntry> &a, const QList<MarkdownEditor::FootnoteEntry> &b)
{
    return std::equal(a.cbegin(), a.cend(), b.cbegin(), b.cend(),
                      [](const MarkdownEditor::FootnoteEntry &l, const MarkdownEditor::FootnoteEntry &r) {
                          return l.number == r.number && l.preview == r.preview && l.jumpOffset == r.jumpOffset;
                      });
}

} // namespace

FootnotesPane::FootnotesPane(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("footnotesPane"));
    m_header = new QLabel(tr("Footnotes"), this);
    m_header->setObjectName(QStringLiteral("paneHeader"));
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("backlinkList")); // the other panes' look
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setWordWrap(true);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        emit jumpRequested(item->data(Qt::UserRole).toLongLong());
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 6, 6);
    layout->setSpacing(6);
    layout->addWidget(m_header);
    layout->addWidget(m_list, 1);
}

void FootnotesPane::setFootnotes(const QList<MarkdownEditor::FootnoteEntry> &footnotes)
{
    if (sameEntries(footnotes, m_shown))
        return;
    m_shown = footnotes;
    m_list->clear();
    m_header->setText(footnotes.isEmpty() ? tr("Footnotes") : tr("Footnotes  %1").arg(footnotes.size()));
    for (const MarkdownEditor::FootnoteEntry &entry : footnotes) {
        auto *item = new QListWidgetItem(QStringLiteral("%1   %2").arg(entry.number).arg(entry.preview), m_list);
        item->setData(Qt::UserRole, qlonglong(entry.jumpOffset));
    }
}

void FootnotesPane::clear()
{
    setFootnotes({});
}

} // namespace noteAppC
