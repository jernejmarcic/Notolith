/// Obsidian's `slash-command`: "/" at the start of a line or after
/// whitespace opens a popup of commands, fuzzy filtered by what is typed
/// after it. Up/Down select, Enter or Tab run the highlighted one, Escape
/// closes. Not inside a code block, inline code or maths.
///
/// Reuses the `[[` link completer's mechanics (a QCompleter anchored to the
/// caret, `MarkdownEditor::keyPressEvent` letting its popup own the
/// navigation keys) and the command palette's fuzzy scorer
/// (`noteAppC::Fuzzy`, from libs/index - linked here rather than moved,
/// since noteAppC_index already depends on neither libs/editor nor libs/ui).
///
/// The editor does not know what a "command" is: MainWindow hands it a
/// source, fetched once when the popup opens rather than per keystroke, and
/// a way to run a command by id. A null source is the feature off, at no
/// cost to a note that never presses "/".

#include "MarkdownEditor.h"

#include "Fuzzy.h"

#include <QCompleter>
#include <QFontMetricsF>
#include <QListView>
#include <QPainter>
#include <QStandardItemModel>
#include <QStyledItemDelegate>

#include <algorithm>

namespace noteAppC {

namespace {

constexpr int kSlashKeysRole = Qt::UserRole + 1;
constexpr int kMaxSlashResults = 50;

/// A row's hotkey, drawn muted at its right edge - the same look the command
/// palette's own rows have (libs/ui/QuickSwitcher.cpp's DetailDelegate),
/// redrawn here rather than shared: the editor does not link libs/ui.
class SlashCommandDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        const QString keys = index.data(kSlashKeysRole).toString();
        if (keys.isEmpty())
            return;
        painter->save();
        painter->setPen(option.palette.color(QPalette::PlaceholderText));
        painter->drawText(option.rect.adjusted(0, 0, -8, 0), Qt::AlignRight | Qt::AlignVCenter, keys);
        painter->restore();
    }
};

} // namespace

void MarkdownEditor::setSlashCommands(SlashCommandSource source, std::function<void(const QString &)> run)
{
    m_slashSource = std::move(source);
    m_runSlashCommand = std::move(run);
    m_slashCache.clear();
    if (m_slashCompleter)
        m_slashCompleter->popup()->hide();
}

bool MarkdownEditor::slashPopupVisible() const
{
    return m_slashCompleter && m_slashCompleter->popup()->isVisible();
}

QStringList MarkdownEditor::slashSuggestions() const
{
    if (!slashPopupVisible())
        return {};
    QStringList names;
    const QAbstractItemModel *model = m_slashCompleter->model();
    names.reserve(model->rowCount());
    for (int row = 0; row < model->rowCount(); ++row)
        names << model->index(row, 0).data().toString();
    return names;
}

bool MarkdownEditor::cursorInCodeOrMath() const
{
    for (const StyleSpan &style : m_decorations.styles) {
        if ((style.kind == StyleSpan::Kind::CodeBlock || style.kind == StyleSpan::Kind::InlineCode
             || style.kind == StyleSpan::Kind::Math)
            && style.range.contains(m_cursor))
            return true;
    }
    for (const MathSpan &math : m_decorations.maths) {
        if (math.range.contains(m_cursor))
            return true;
    }
    return false;
}

std::optional<QString> MarkdownEditor::slashPrefix() const
{
    if (!m_slashSource || cursorInCodeOrMath())
        return std::nullopt;

    const qsizetype lineStart = m_text.lastIndexOf(QLatin1Char('\n'), qMax<qsizetype>(0, m_cursor - 1)) + 1;
    const QStringView before = QStringView(m_text).mid(lineStart, m_cursor - lineStart);

    const qsizetype slash = before.lastIndexOf(QLatin1Char('/'));
    if (slash < 0)
        return std::nullopt;
    // Only at the start of a line or after whitespace - "a/b" is text, not a
    // command, the same rule Obsidian uses.
    if (slash > 0 && !before.at(slash - 1).isSpace())
        return std::nullopt;
    return before.mid(slash + 1).toString();
}

void MarkdownEditor::ensureSlashCompleter()
{
    if (m_slashCompleter)
        return;
    m_slashCompleter = new QCompleter(new QStandardItemModel(this), this);
    m_slashCompleter->setWidget(this);
    m_slashCompleter->popup()->setObjectName(QStringLiteral("slashCommandPopup"));
    if (auto *list = qobject_cast<QListView *>(m_slashCompleter->popup()))
        list->setUniformItemSizes(true);
    m_slashCompleter->popup()->setItemDelegate(new SlashCommandDelegate(m_slashCompleter->popup()));
    m_slashCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_slashCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_slashCompleter->setFilterMode(Qt::MatchContains);
    // The model is pre-filtered and pre-ranked by Fuzzy::score every
    // keystroke; QCompleter just needs to show every row it is given, which
    // an always-empty prefix against MatchContains does. Its default
    // completionRole is Qt::EditRole, which a QStandardItem built from text
    // alone does not carry - only Qt::DisplayRole - so completionCount()
    // would otherwise read as 0 and the popup would never show.
    m_slashCompleter->setCompletionRole(Qt::DisplayRole);
    connect(m_slashCompleter, qOverload<const QString &>(&QCompleter::activated),
            this, &MarkdownEditor::insertSlashCompletion);
}

void MarkdownEditor::updateSlashSuggestions()
{
    if (!m_slashSource)
        return;
    const auto prefix = slashPrefix();
    const bool wasOpen = m_slashCompleter && m_slashCompleter->popup()->isVisible();
    if (!prefix) {
        if (wasOpen)
            m_slashCompleter->popup()->hide();
        return;
    }

    ensureSlashCompleter();
    if (!wasOpen)
        m_slashCache = m_slashSource(); // fetched on open, not per keystroke

    QList<QPair<int, qsizetype>> ranked;
    ranked.reserve(m_slashCache.size());
    for (qsizetype i = 0; i < m_slashCache.size(); ++i) {
        const auto match = Fuzzy::score(*prefix, m_slashCache.at(i).name);
        if (match.matched)
            ranked.append({match.score, i});
    }
    if (ranked.isEmpty()) {
        m_slashCompleter->popup()->hide();
        return;
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto &l, const auto &r) { return l.first > r.first; });

    auto *model = qobject_cast<QStandardItemModel *>(m_slashCompleter->model());
    model->removeRows(0, model->rowCount());
    m_slashIDs.clear();
    for (const auto &[score, i] : ranked) {
        if (model->rowCount() >= kMaxSlashResults)
            break;
        const SlashCommand &command = m_slashCache.at(i);
        auto *item = new QStandardItem(command.name);
        item->setData(command.keys, kSlashKeysRole);
        model->appendRow(item);
        m_slashIDs.insert(command.name, command.id);
    }
    m_slashCompleter->popup()->setCurrentIndex(m_slashCompleter->completionModel()->index(0, 0));

    const QPointF point = cursorPoint();
    QRect anchor(point.toPoint(), QSize(1, int(QFontMetricsF(font()).height())));
    anchor.moveTopLeft(viewport()->mapTo(this, anchor.topLeft()));
    anchor.setWidth(m_slashCompleter->popup()->sizeHintForColumn(0) + 96);
    m_slashCompleter->complete(anchor);
}

void MarkdownEditor::insertSlashCompletion(const QString &name)
{
    const auto prefix = slashPrefix();
    if (!prefix)
        return;
    // Removes the typed "/query" first, then runs the command - so a command
    // that inserts text (or acts on the selection) does not have to know
    // about the slash syntax it was invoked through.
    const qsizetype at = m_cursor - prefix->size() - 1;
    const QString id = m_slashIDs.value(name);
    applyEdit(at, prefix->size() + 1, QString());
    if (!id.isEmpty() && m_runSlashCommand)
        m_runSlashCommand(id);
}

} // namespace noteAppC
