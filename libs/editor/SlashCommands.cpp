// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
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
#include "MarkdownEditor_p.h"

#include "Fuzzy.h"

#include <QCompleter>
#include <QFontMetricsF>
#include <QListView>
#include <QPainter>
#include <QRegularExpression>
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

/// A typed word that is a whole word of the name puts it ahead of fuzzy
/// matches, the shortest name first: `/table` offers "Insert table" before the
/// four "Table: ..." commands. Not for a query of several words, which the
/// fuzzy score already ranks (`/insert table`).
int wordBonus(const QString &query, const QString &name)
{
    if (query.isEmpty() || query.contains(QLatin1Char(' ')))
        return 0;
    const QStringList words = name.split(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")), Qt::SkipEmptyParts);
    for (const QString &word : words) {
        if (word.compare(query, Qt::CaseInsensitive) == 0)
            return 100000 - 1000 * int(name.size()); // outweighs any fuzzy score
    }
    return 0;
}

} // namespace

void MarkdownEditor::setSlashCommands(SlashCommandSource source, std::function<void(const QString &)> run)
{
    d->m_slashSource = std::move(source);
    m_runSlashCommand = std::move(run);
    d->m_slashCache.clear();
    if (d->m_slashCompleter)
        d->m_slashCompleter->popup()->hide();
}

bool MarkdownEditor::slashPopupVisible() const
{
    return d->m_slashCompleter && d->m_slashCompleter->popup()->isVisible();
}

QStringList MarkdownEditor::slashSuggestions() const
{
    if (!slashPopupVisible())
        return {};
    QStringList names;
    const QAbstractItemModel *model = d->m_slashCompleter->model();
    names.reserve(model->rowCount());
    for (int row = 0; row < model->rowCount(); ++row)
        names << model->index(row, 0).data().toString();
    return names;
}

bool MarkdownEditor::cursorInCodeOrMath() const
{
    for (const StyleSpan &style : d->m_decorations.styles) {
        if ((style.kind == StyleSpan::Kind::CodeBlock || style.kind == StyleSpan::Kind::InlineCode
             || style.kind == StyleSpan::Kind::Math)
            && style.range.contains(d->m_cursor))
            return true;
    }
    for (const MathSpan &math : d->m_decorations.maths) {
        if (math.range.contains(d->m_cursor))
            return true;
    }
    return false;
}

std::optional<QString> MarkdownEditor::slashPrefix() const
{
    if (!d->m_slashSource || cursorInCodeOrMath())
        return std::nullopt;

    const qsizetype lineStart = d->m_text.lastIndexOf(QLatin1Char('\n'), qMax<qsizetype>(0, d->m_cursor - 1)) + 1;
    const QStringView before = QStringView(d->m_text).mid(lineStart, d->m_cursor - lineStart);

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
    if (d->m_slashCompleter)
        return;
    d->m_slashCompleter = new QCompleter(new QStandardItemModel(this), this);
    d->m_slashCompleter->setWidget(this);
    d->m_slashCompleter->popup()->setObjectName(QStringLiteral("slashCommandPopup"));
    if (auto *list = qobject_cast<QListView *>(d->m_slashCompleter->popup()))
        list->setUniformItemSizes(true);
    d->m_slashCompleter->popup()->setItemDelegate(new SlashCommandDelegate(d->m_slashCompleter->popup()));
    d->m_slashCompleter->setCompletionMode(QCompleter::PopupCompletion);
    d->m_slashCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    d->m_slashCompleter->setFilterMode(Qt::MatchContains);
    // The model is pre-filtered and pre-ranked by Fuzzy::score every
    // keystroke; QCompleter just needs to show every row it is given, which
    // an always-empty prefix against MatchContains does. Its default
    // completionRole is Qt::EditRole, which a QStandardItem built from text
    // alone does not carry - only Qt::DisplayRole - so completionCount()
    // would otherwise read as 0 and the popup would never show.
    d->m_slashCompleter->setCompletionRole(Qt::DisplayRole);
    connect(d->m_slashCompleter, qOverload<const QString &>(&QCompleter::activated),
            this, &MarkdownEditor::insertSlashCompletion);
}

void MarkdownEditor::updateSlashSuggestions()
{
    if (!d->m_slashSource)
        return;
    const auto prefix = slashPrefix();
    const bool wasOpen = d->m_slashCompleter && d->m_slashCompleter->popup()->isVisible();
    if (!prefix) {
        if (wasOpen)
            d->m_slashCompleter->popup()->hide();
        return;
    }

    ensureSlashCompleter();
    if (!wasOpen)
        d->m_slashCache = d->m_slashSource(); // fetched on open, not per keystroke

    QList<QPair<int, qsizetype>> ranked;
    ranked.reserve(d->m_slashCache.size());
    for (qsizetype i = 0; i < d->m_slashCache.size(); ++i) {
        const auto match = Fuzzy::score(*prefix, d->m_slashCache.at(i).name);
        if (match.matched)
            ranked.append({match.score + wordBonus(*prefix, d->m_slashCache.at(i).name), i});
    }
    if (ranked.isEmpty()) {
        d->m_slashCompleter->popup()->hide();
        return;
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto &l, const auto &r) { return l.first > r.first; });

    auto *model = qobject_cast<QStandardItemModel *>(d->m_slashCompleter->model());
    model->removeRows(0, model->rowCount());
    d->m_slashIDs.clear();
    for (const auto &[score, i] : ranked) {
        if (model->rowCount() >= kMaxSlashResults)
            break;
        const SlashCommand &command = d->m_slashCache.at(i);
        auto *item = new QStandardItem(command.name);
        item->setData(command.keys, kSlashKeysRole);
        model->appendRow(item);
        d->m_slashIDs.insert(command.name, command.id);
    }
    d->m_slashCompleter->popup()->setCurrentIndex(d->m_slashCompleter->completionModel()->index(0, 0));

    const QPointF point = cursorPoint();
    QRect anchor(point.toPoint(), QSize(1, int(QFontMetricsF(font()).height())));
    anchor.moveTopLeft(viewport()->mapTo(this, anchor.topLeft()));
    anchor.setWidth(d->m_slashCompleter->popup()->sizeHintForColumn(0) + 96);
    d->m_slashCompleter->complete(anchor);
}

void MarkdownEditor::insertSlashCompletion(const QString &name)
{
    const auto prefix = slashPrefix();
    if (!prefix)
        return;
    const TypingScope typed(d->m_typing);
    // Removes the typed "/query" first, then runs the command - so a command
    // that inserts text (or acts on the selection) does not have to know
    // about the slash syntax it was invoked through.
    const qsizetype at = d->m_cursor - prefix->size() - 1;
    const QString id = d->m_slashIDs.value(name);
    applyEdit(at, prefix->size() + 1, QString());
    if (!id.isEmpty() && m_runSlashCommand)
        m_runSlashCommand(id);
}

} // namespace noteAppC
