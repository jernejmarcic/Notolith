#include "TemplatePicker.h"

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

TemplatePicker::TemplatePicker(QWidget *parent)
    : QDialog(parent, Qt::Popup)
{
    setObjectName(QStringLiteral("templatePicker"));
    setSizeGripEnabled(false);

    m_query = new QLineEdit(this);
    m_query->setObjectName(QStringLiteral("templatePickerQuery"));
    m_query->setPlaceholderText(tr("Insert template..."));
    m_query->setClearButtonEnabled(false);
    m_query->installEventFilter(this);

    m_results = new QListWidget(this);
    m_results->setObjectName(QStringLiteral("templatePickerResults"));
    m_results->setUniformItemSizes(true);
    m_results->setFrameShape(QFrame::NoFrame);
    m_results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addWidget(m_query);
    layout->addWidget(m_results);

    connect(m_query, &QLineEdit::textChanged, this, &TemplatePicker::refill);
    connect(m_query, &QLineEdit::returnPressed, this, &TemplatePicker::chooseCurrent);
    connect(m_results, &QListWidget::itemActivated, this, &TemplatePicker::chooseCurrent);
    connect(m_results, &QListWidget::itemClicked, this, &TemplatePicker::chooseCurrent);

    resize(480, 340);
}

void TemplatePicker::setTemplates(Templates templates)
{
    m_templates = std::move(templates);
    m_candidates = m_templates->list();
    refill();
}

void TemplatePicker::popUp(const QString &noteTitle)
{
    m_noteTitle = noteTitle;
    m_query->clear();
    if (m_templates)
        m_candidates = m_templates->list(); // the folder may have changed since the last pop
    refill();
    if (QWidget *owner = parentWidget()) {
        const QPoint centre = owner->mapToGlobal(owner->rect().center());
        move(centre.x() - width() / 2, owner->mapToGlobal(QPoint(0, 0)).y() + 80);
    }
    show();
    m_query->setFocus();
}

void TemplatePicker::refill()
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

void TemplatePicker::chooseCurrent()
{
    auto *item = m_results->currentItem();
    if (!item || !m_templates)
        return;

    QString rendered;
    if (m_templates->render(item->text(), m_noteTitle, &rendered))
        emit templateChosen(rendered);
    hide();
}

bool TemplatePicker::eventFilter(QObject *watched, QEvent *event)
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
