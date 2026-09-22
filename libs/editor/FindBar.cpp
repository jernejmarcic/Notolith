#include "FindBar.h"

#include "Icons.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace noteAppC {

namespace {

QToolButton *toggle(const QString &label, const QString &tip)
{
    auto *button = new QToolButton;
    button->setText(label);
    button->setToolTip(tip);
    button->setCheckable(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

} // namespace

FindBar::FindBar(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("findBar"));
    setFrameShape(QFrame::NoFrame);

    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("findField"));
    m_search->setPlaceholderText(tr("Find"));
    m_search->setClearButtonEnabled(true);
    m_search->installEventFilter(this);

    m_count = new QLabel;
    m_count->setObjectName(QStringLiteral("findCount"));
    m_count->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_count->setMinimumWidth(64);

    // Lucide has no chevron-up in the bundled set, so the two step buttons are
    // arrows rather than half an icon pair.
    auto *previous = new QToolButton;
    previous->setText(QStringLiteral("↑"));
    previous->setToolTip(tr("Previous match (Shift+Enter)"));
    previous->setFocusPolicy(Qt::NoFocus);

    auto *next = new QToolButton;
    next->setText(QStringLiteral("↓"));
    next->setToolTip(tr("Next match (Enter)"));
    next->setFocusPolicy(Qt::NoFocus);

    m_case = toggle(QStringLiteral("Aa"), tr("Match case"));
    m_regex = toggle(QStringLiteral(".*"), tr("Use regular expression"));

    m_close = new QToolButton;
    m_close->setToolTip(tr("Close (Esc)"));
    m_close->setFocusPolicy(Qt::NoFocus);
    m_close->setIcon(Icons::icon(QStringLiteral("x"), palette().windowText().color(), 14));

    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->setSpacing(4);
    searchRow->addWidget(m_search, 1);
    searchRow->addWidget(m_count);
    searchRow->addWidget(previous);
    searchRow->addWidget(next);
    searchRow->addWidget(m_case);
    searchRow->addWidget(m_regex);
    searchRow->addWidget(m_close);

    m_replace = new QLineEdit;
    m_replace->setObjectName(QStringLiteral("replaceField"));
    m_replace->setPlaceholderText(tr("Replace"));
    m_replace->installEventFilter(this);

    auto *replaceOne = new QPushButton(tr("Replace"));
    replaceOne->setFocusPolicy(Qt::NoFocus);
    auto *replaceEvery = new QPushButton(tr("Replace all"));
    replaceEvery->setFocusPolicy(Qt::NoFocus);

    m_replaceRow = new QWidget;
    auto *replaceLayout = new QHBoxLayout(m_replaceRow);
    replaceLayout->setContentsMargins(0, 0, 0, 0);
    replaceLayout->setSpacing(4);
    replaceLayout->addWidget(m_replace, 1);
    replaceLayout->addWidget(replaceOne);
    replaceLayout->addWidget(replaceEvery);
    m_replaceRow->hide();

    auto *rows = new QVBoxLayout(this);
    rows->setContentsMargins(8, 8, 8, 8);
    rows->setSpacing(6);
    rows->addLayout(searchRow);
    rows->addWidget(m_replaceRow);

    connect(m_search, &QLineEdit::textChanged, this, &FindBar::queryChanged);
    connect(m_case, &QToolButton::toggled, this, &FindBar::queryChanged);
    connect(m_regex, &QToolButton::toggled, this, &FindBar::queryChanged);
    connect(previous, &QToolButton::clicked, this, [this] { emit stepped(-1); });
    connect(next, &QToolButton::clicked, this, [this] { emit stepped(1); });
    connect(m_close, &QToolButton::clicked, this, &FindBar::dismissed);
    connect(replaceOne, &QPushButton::clicked, this, &FindBar::replaceRequested);
    connect(replaceEvery, &QPushButton::clicked, this, &FindBar::replaceAllRequested);
}

void FindBar::setTheme(const Theme &theme)
{
    const QString border = theme.value(QStringLiteral("--background-modifier-border"));
    const QString field = theme.value(QStringLiteral("--background-modifier-form-field"));
    const QString error = theme.value(QStringLiteral("--text-error"));

    setStyleSheet(QStringLiteral(
                      "#findBar { background: %1; border: 1px solid %2; border-radius: %3px; }"
                      "#findBar QToolButton { border: none; border-radius: 4px; padding: 2px 5px;"
                      " color: %4; }"
                      "#findBar QToolButton:hover { background: %5; }"
                      "#findBar QToolButton:checked { background: %6; color: %7; }"
                      "#findBar QPushButton { border: 1px solid %2; border-radius: 4px;"
                      " padding: 3px 8px; color: %4; background: %8; }"
                      "#findBar QLabel { color: %9; }")
                      .arg(theme.value(QStringLiteral("--background-secondary")), border,
                           theme.value(QStringLiteral("--radius-m")),
                           theme.value(QStringLiteral("--text-normal")),
                           theme.value(QStringLiteral("--background-modifier-hover")),
                           theme.value(QStringLiteral("--interactive-accent")),
                           theme.value(QStringLiteral("--text-on-accent")),
                           theme.value(QStringLiteral("--interactive-normal")),
                           theme.value(QStringLiteral("--text-muted"))));

    m_fieldStyle = QStringLiteral("background: %1; border: 1px solid %2; border-radius: 4px;"
                                  " padding: 3px 6px; color: %3;")
                       .arg(field, border, theme.value(QStringLiteral("--text-normal")));
    // An unparseable regex colours the field instead of throwing anything.
    m_errorFieldStyle = QStringLiteral("background: %1; border: 1px solid %2; border-radius: 4px;"
                                       " padding: 3px 6px; color: %2;")
                            .arg(field, error);

    m_replace->setStyleSheet(m_fieldStyle);
    restyleSearchField();
    m_close->setIcon(Icons::icon(QStringLiteral("x"),
                                 theme.color(QStringLiteral("--text-muted")), 14));
}

void FindBar::restyleSearchField()
{
    m_search->setStyleSheet(m_valid ? m_fieldStyle : m_errorFieldStyle);
}

void FindBar::setReplaceShown(bool on)
{
    m_replaceRow->setVisible(on);
    adjustSize();
}

bool FindBar::replaceShown() const
{
    return !m_replaceRow->isHidden();
}

void FindBar::focusSearch()
{
    m_search->setFocus(Qt::ShortcutFocusReason);
    m_search->selectAll();
}

void FindBar::setStatus(int current, int total, bool patternValid)
{
    if (m_valid != patternValid) {
        m_valid = patternValid;
        restyleSearchField();
    }

    if (m_search->text().isEmpty() || !patternValid)
        m_count->setText(QString());
    else if (total == 0)
        m_count->setText(tr("No results"));
    else
        m_count->setText(tr("%1 of %2").arg(current + 1).arg(total));
}

QString FindBar::pattern() const
{
    return m_search->text();
}

QString FindBar::replacement() const
{
    return m_replace->text();
}

bool FindBar::caseSensitive() const
{
    return m_case->isChecked();
}

bool FindBar::useRegex() const
{
    return m_regex->isChecked();
}

bool FindBar::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress)
        return QFrame::eventFilter(watched, event);

    auto *key = static_cast<QKeyEvent *>(event);
    switch (key->key()) {
    case Qt::Key_Escape:
        emit dismissed();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        // Enter in the replace field replaces, as it does in Obsidian; in the
        // search field it steps, backwards with Shift.
        if (watched == m_replace)
            emit replaceRequested();
        else
            emit stepped(key->modifiers().testFlag(Qt::ShiftModifier) ? -1 : 1);
        return true;
    default:
        break;
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace noteAppC
