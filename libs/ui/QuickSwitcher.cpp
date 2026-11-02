// SPDX-License-Identifier: GPL-3.0-or-later
#include "QuickSwitcher.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <algorithm>

#include "Fuzzy.h"

namespace noteAppC {

namespace {
constexpr int kMaxResults = 60;
constexpr int kDetailRole = Qt::UserRole + 1;

/// A row's detail - a command's hotkey - drawn muted at its right edge.
class DetailDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        const QString detail = index.data(kDetailRole).toString();
        if (detail.isEmpty())
            return;
        painter->save();
        painter->setPen(option.palette.color(QPalette::PlaceholderText));
        painter->drawText(option.rect.adjusted(0, 0, -8, 0), Qt::AlignRight | Qt::AlignVCenter, detail);
        painter->restore();
    }
};
} // namespace

QuickSwitcher::QuickSwitcher(QWidget *parent)
    : QDialog(parent, Qt::Popup)
{
    setObjectName(QStringLiteral("quickSwitcher"));
    setSizeGripEnabled(false);

    m_query = new QLineEdit(this);
    m_query->setObjectName(QStringLiteral("quickSwitcherQuery"));
    m_query->setPlaceholderText(tr("Find or create a note..."));
    m_query->setClearButtonEnabled(false);
    m_query->installEventFilter(this);

    m_results = new QListWidget(this);
    m_results->setObjectName(QStringLiteral("quickSwitcherResults"));
    m_results->setUniformItemSizes(true);
    m_results->setFrameShape(QFrame::NoFrame);
    m_results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addWidget(m_query);
    layout->addWidget(m_results);

    connect(m_query, &QLineEdit::textChanged, this, &QuickSwitcher::refill);
    connect(m_query, &QLineEdit::returnPressed, this, &QuickSwitcher::chooseCurrent);
    connect(m_results, &QListWidget::itemActivated, this, &QuickSwitcher::chooseCurrent);
    connect(m_results, &QListWidget::itemClicked, this, &QuickSwitcher::chooseCurrent);

    resize(560, 380);
}

void QuickSwitcher::setCandidates(const QStringList &candidates, const QStringList &details,
                                  const QList<QIcon> &icons, const QList<bool> &dimmed)
{
    m_dimmed = dimmed;
    m_candidates = candidates;
    m_details = details;
    m_icons = icons;
    if (!icons.isEmpty())
        m_results->setIconSize({16, 16});
    if (!details.isEmpty() && !dynamic_cast<DetailDelegate *>(m_results->itemDelegate()))
        m_results->setItemDelegate(new DetailDelegate(m_results));
    refill();
}

void QuickSwitcher::setPlaceholder(const QString &text)
{
    m_query->setPlaceholderText(text);
}

void QuickSwitcher::popUp()
{
    m_query->clear();
    refill();
    if (QWidget *owner = parentWidget()) {
        const QPoint centre = owner->mapToGlobal(owner->rect().center());
        move(centre.x() - width() / 2, owner->mapToGlobal(QPoint(0, 0)).y() + 80);
    }
    show();
    m_query->setFocus();
}

void QuickSwitcher::refill()
{
    const QString query = m_query->text();

    QList<QPair<int, qsizetype>> ranked;
    ranked.reserve(m_candidates.size());
    for (qsizetype i = 0; i < m_candidates.size(); ++i) {
        const auto match = Fuzzy::score(query, m_candidates.at(i));
        if (match.matched)
            ranked.append({match.score, i});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto &l, const auto &r) { return l.first > r.first; });

    m_results->clear();
    for (const auto &[score, i] : ranked) {
        if (m_results->count() >= kMaxResults)
            break;
        auto *item = new QListWidgetItem(m_candidates.at(i), m_results);
        if (i < m_details.size())
            item->setData(kDetailRole, m_details.at(i));
        if (i < m_icons.size())
            item->setIcon(m_icons.at(i));
        if (i < m_dimmed.size() && m_dimmed.at(i))
            item->setForeground(m_results->palette().color(QPalette::Disabled, QPalette::Text));
    }
    if (m_results->count() > 0)
        m_results->setCurrentRow(0);
}

void QuickSwitcher::chooseCurrent()
{
    if (auto *item = m_results->currentItem()) {
        emit accepted(item->text());
        hide();
    } else {
        requestCreate();
    }
}

void QuickSwitcher::requestCreate()
{
    const QString name = m_query->text().trimmed();
    if (!m_createAllowed || name.isEmpty())
        return;
    hide();
    emit createRequested(name);
}

bool QuickSwitcher::eventFilter(QObject *watched, QEvent *event)
{
    // Arrow keys belong to the list even while the line edit has focus.
    if (watched == m_query && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (!(key->modifiers() & Qt::ShiftModifier))
                break;
            requestCreate(); // Shift+Enter creates even when something matches
            return true;
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_PageDown:
        case Qt::Key_PageUp:
            QCoreApplication::sendEvent(m_results, event);
            return true;
        case Qt::Key_Escape:
            hide();
            return true;
        default:
            break;
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace noteAppC
