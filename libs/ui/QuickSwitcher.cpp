#include "QuickSwitcher.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>
#include <algorithm>

#include "Fuzzy.h"

namespace noteAppC {

namespace {
constexpr int kMaxResults = 60;
}

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

void QuickSwitcher::setCandidates(const QStringList &candidates)
{
    m_candidates = candidates;
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

    QList<QPair<int, QString>> ranked;
    ranked.reserve(m_candidates.size());
    for (const QString &candidate : m_candidates) {
        const auto match = Fuzzy::score(query, candidate);
        if (match.matched)
            ranked.append({match.score, candidate});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto &l, const auto &r) { return l.first > r.first; });

    m_results->clear();
    for (const auto &[score, candidate] : ranked) {
        if (m_results->count() >= kMaxResults)
            break;
        m_results->addItem(candidate);
    }
    if (m_results->count() > 0)
        m_results->setCurrentRow(0);
}

void QuickSwitcher::chooseCurrent()
{
    if (auto *item = m_results->currentItem()) {
        emit accepted(item->text());
        hide();
    }
}

bool QuickSwitcher::eventFilter(QObject *watched, QEvent *event)
{
    // Arrow keys belong to the list even while the line edit has focus.
    if (watched == m_query && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
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
