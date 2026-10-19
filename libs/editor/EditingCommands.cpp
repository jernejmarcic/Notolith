/// The editing behaviours from PLAN.md 4.6: auto-pairs, list continuation,
/// checkbox toggling, line moves, the formatting commands and find/replace.
///
/// They live beside MarkdownEditor.cpp rather than in it because they are all
/// the same shape - work out a splice, hand it to `applyEdit`, put the caret
/// where the reader expects it - and the engine file is long enough already.
/// Everything here goes through `applyEdit`, so one command is one undo step
/// and the decoration pass sees exactly one edit.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include "Parser.h"
#include "TableFormat.h"

#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QScrollBar>

#include <algorithm>

namespace noteAppC {

namespace {

qsizetype lineStartOf(const QString &text, qsizetype at)
{
    if (at <= 0)
        return 0;
    const qsizetype newline = text.lastIndexOf(QLatin1Char('\n'), at - 1);
    return newline < 0 ? 0 : newline + 1;
}

/// Offset of the newline that ends the line holding `at`, or the end of the
/// document. The newline itself is not part of the line.
qsizetype lineEndOf(const QString &text, qsizetype at)
{
    const qsizetype newline = text.indexOf(QLatin1Char('\n'), at);
    return newline < 0 ? text.size() : newline;
}

/// The closing half of a pair, or a null QChar when `open` does not pair.
/// `markdown` adds Obsidian's `autoPairMarkdown` set, which only ever applies
/// to a selection here: those characters are their own closer, so typing one
/// in running prose must not sprout a second.
QChar pairCloser(QChar open, bool markdown)
{
    switch (open.unicode()) {
    case u'(':
        return QLatin1Char(')');
    case u'[':
        return QLatin1Char(']');
    case u'{':
        return QLatin1Char('}');
    case u'"':
    case u'\'':
        return open;
    default:
        break;
    }
    if (markdown) {
        switch (open.unicode()) {
        case u'*':
        case u'_':
        case u'`':
        case u'=':
        case u'~':
            return open;
        default:
            break;
        }
    }
    return QChar();
}

/// One list item as its own line reads it. Parsed from the text rather than
/// from the decorations, because the decorations are up to a debounce behind
/// while the reader is typing and these commands must not be.
struct ListLine {
    bool valid = false;
    QString quote;    ///< "> " or "> > " when the item sits in a quote or callout, verbatim
    QString indent;   ///< the whitespace between the quote and the bullet, verbatim
    QString bullet;   ///< "-", "*", "+", "1." or "1)"
    QString spacing;  ///< the whitespace after the bullet
    QString checkbox; ///< "[ ] " including its trailing space, or empty
    bool ordered = false;
    QChar state;           ///< what is in the checkbox; null without one
    int number = 0;
    qsizetype checkboxColumn = -1; ///< column of the "[" in the line
    qsizetype contentColumn = 0;   ///< column where the item's own text starts

    /// What makes two items one list: the bullet character, or an ordered
    /// item's delimiter. CommonMark starts a new list when it changes.
    QString kind() const { return ordered ? bullet.right(1) : bullet; }
    /// The item's depth for comparing with its neighbours; a tab counts four.
    qsizetype width() const
    {
        qsizetype width = 0;
        for (const QChar c : indent)
            width += c == QLatin1Char('\t') ? 4 : 1;
        return width;
    }
};

/// `anyState`: any one character in the box is a task's state, not just
/// `[ ]` and `[x]` (ParseOptions::taskStates).
ListLine parseListLine(QStringView line, bool anyState = false)
{
    // The quote prefix is `>` with at most one space after it, so a quoted
    // item's own indentation is what comes after that one space.
    static const QRegularExpression head(QStringLiteral(
        "^((?:[ \\t]*>[ \\t]?)*)([ \\t]*)([-*+]|\\d{1,9}[.)])([ \\t]+)(\\[([ xX])\\][ \\t]+)?"));

    static const QRegularExpression headAny(QStringLiteral(
        "^((?:[ \\t]*>[ \\t]?)*)([ \\t]*)([-*+]|\\d{1,9}[.)])([ \\t]+)(\\[([^\\[\\]\\n\\x{10000}-\\x{10FFFF}])\\][ \\t]+)?"));

    const auto match = (anyState ? headAny : head).matchView(line);
    ListLine item;
    if (!match.hasMatch())
        return item;

    item.valid = true;
    item.quote = match.captured(1);
    item.indent = match.captured(2);
    item.bullet = match.captured(3);
    item.spacing = match.captured(4);
    item.checkbox = match.captured(5);
    item.ordered = item.bullet.size() > 1;
    item.number = item.ordered ? item.bullet.chopped(1).toInt() : 0;
    if (!item.checkbox.isEmpty()) {
        item.state = match.captured(6).at(0);
        item.checkboxColumn = match.capturedStart(5);
    }
    item.contentColumn = match.capturedEnd(0);
    return item;
}

/// Renumbers the ordered items of a run in place, per indent level, from line
/// `from` down. Lines above it are read, never rewritten: a list written as
/// "5. 6. 7." stays at 5, and an irregular "1. 1. 1." is left as typed until an
/// edit reaches it. A level that opens below `from` restarts at 1, which is
/// what Obsidian does when an item is indented. A line that is not a list item
/// (a wrapped continuation, or an item whose marker was just removed) is passed
/// over, so the items after it carry on counting, as CommonMark reads them.
/// Rewriting stops at the first later item at `stopWidth` that already carries
/// the number it should (Obsidian does the same), which bounds the cost of an
/// edit in a long list and leaves what follows as typed.
void renumber(QStringList &lines, qsizetype from, qsizetype stopWidth = -1)
{
    struct Level {
        qsizetype width = 0;
        int next = 1;
        QString kind;
    };
    QList<Level> stack;

    for (qsizetype i = 0; i < lines.size(); ++i) {
        QString &line = lines[i];
        const ListLine item = parseListLine(line);
        if (!item.valid)
            continue;

        const qsizetype width = item.width();
        while (!stack.isEmpty() && stack.last().width > width)
            stack.removeLast();
        const bool rewriting = i >= from;
        if (stack.isEmpty() || stack.last().width < width) {
            const int first = !rewriting || stack.isEmpty() ? (item.ordered ? item.number : 1) : 1;
            stack.append({width, first, item.kind()});
        } else if (stack.last().kind != item.kind()) {
            // A new bullet or delimiter is a new list, which counts from its own number.
            stack.last().kind = item.kind();
            stack.last().next = qMax(item.number, 1);
        }

        if (!item.ordered)
            continue;

        Level &level = stack.last();
        if (rewriting && i > from && width == stopWidth && item.number == level.next)
            break;
        if (rewriting) {
            const QString wanted = QString::number(level.next) + item.bullet.right(1);
            line.replace(item.quote.size() + item.indent.size(), item.bullet.size(), wanted);
        } else {
            level.next = item.number;
        }
        ++level.next;
    }
}

/// One level of list indentation. Obsidian's default is a tab, and the
/// parser counts one tab as one level - except inside a quote, where it
/// measures columns from the line start and wants spaces.
QString indentUnit(const ListLine &item)
{
    return item.quote.isEmpty() ? QStringLiteral("\t") : QStringLiteral("    ");
}

/// How many characters of `item`'s indent one outdent takes off.
qsizetype outdentWidth(const ListLine &item)
{
    const QString unit = indentUnit(item);
    return item.indent.startsWith(unit) ? unit.size() : qMin<qsizetype>(4, item.indent.size());
}

/// The kinds of line a list run is made of.
enum class RunLine { Item, Continuation, Blank, Other };

RunLine classify(QStringView line, const ListLine &base)
{
    const ListLine item = parseListLine(line);
    // Another quote depth is another list.
    if (item.valid)
        return item.quote.count(QLatin1Char('>')) == base.quote.count(QLatin1Char('>')) ? RunLine::Item
                                                                                         : RunLine::Other;
    static const QRegularExpression quotePrefix(QStringLiteral("^(?:[ \\t]*>[ \\t]?)*"));
    const auto prefix = quotePrefix.matchView(line);
    const QStringView quote = line.left(prefix.capturedLength());
    if (quote.count(u'>') != base.quote.count(QLatin1Char('>')))
        return RunLine::Other;
    const QStringView rest = line.mid(quote.size());
    if (rest.trimmed().isEmpty())
        return RunLine::Blank;
    return rest.at(0) == u' ' || rest.at(0) == u'\t' ? RunLine::Continuation : RunLine::Other;
}

} // namespace

void MarkdownEditor::setSelectionRange(qsizetype anchor, qsizetype cursor)
{
    d->m_anchor = qBound<qsizetype>(0, anchor, d->m_text.size());
    d->m_cursor = qBound<qsizetype>(0, cursor, d->m_text.size());
    d->m_activeBlock = blockAt(d->m_cursor);
    d->m_goalX = -1;
    updateLayout();
    ensureCursorVisible();
    d->m_cursorOn = true;
    viewport()->update();
    emit cursorMoved(d->m_cursor);
}

MarkdownEditor::MetaJump MarkdownEditor::selectMetaNote(int direction, int *index, int *count)
{
    const QList<Range> &runs = d->m_decorations.metaNotes;
    if (count)
        *count = int(runs.size());
    if (runs.isEmpty())
        return MetaJump::None;

    // Forward looks from the end of the selection (the caret when there is
    // none), backward from its start; a run the selection already is lies on
    // the wrong side of both, so it is never picked twice.
    const Range from = selection();
    qsizetype found = -1;
    if (direction >= 0) {
        for (qsizetype i = 0; i < runs.size() && found < 0; ++i)
            found = runs.at(i).start >= from.end ? i : -1;
    } else {
        for (qsizetype i = runs.size() - 1; i >= 0 && found < 0; --i)
            found = runs.at(i).end <= from.start ? i : -1;
    }
    const bool wrapped = found < 0;
    if (wrapped)
        found = direction >= 0 ? 0 : runs.size() - 1;

    selectRange(runs.at(found));
    if (index)
        *index = int(found) + 1;
    return wrapped ? MetaJump::Wrapped : MetaJump::Moved;
}

void MarkdownEditor::selectRange(const Range &range)
{
    setSelectionRange(range.start, range.end);
    scrollToOffset(range.start);
    viewport()->update();
}

// --- auto-pairs -----------------------------------------------------------

bool MarkdownEditor::handleAutoPair(QChar typed)
{
    if (!d->m_autoPairs || d->m_mode == Mode::Reading)
        return false;

    // A selection is wrapped, never replaced - and it stays selected, so a
    // second press turns "*text*" into "**text**", which is how Obsidian
    // reaches `**`, `==` and `~~`.
    const Range selected = selection();
    if (selected.length() > 0) {
        const QChar close = pairCloser(typed, true);
        if (close.isNull())
            return false;
        pushUndo();
        d->m_lastEdit.invalidate();
        applyEdit(selected.start, selected.length(),
                  typed + d->m_text.mid(selected.start, selected.length()) + close);
        setSelectionRange(selected.start + 1, selected.start + 1 + selected.length());
        return true;
    }

    // Typing the closer that is already there steps over it.
    static const QString closers = QStringLiteral(")]}\"'");
    if (closers.contains(typed) && d->m_cursor < d->m_text.size() && d->m_text.at(d->m_cursor) == typed) {
        moveCursor(d->m_cursor + 1, false);
        return true;
    }

    const QChar close = pairCloser(typed, false);
    if (close.isNull())
        return false;

    // CodeMirror's rule, which is what Obsidian runs: close only when what
    // follows can take a closing bracket, so "(" before a word stays alone.
    static const QString closeBefore = QStringLiteral(")]}:;>");
    const QChar next = d->m_cursor < d->m_text.size() ? d->m_text.at(d->m_cursor) : QChar();
    if (!(next.isNull() || next.isSpace() || closeBefore.contains(next)))
        return false;
    // A quote is its own closer, so it must never open inside a word:
    // "don't" would otherwise become "don''t".
    if (typed == close) {
        const QChar previous = d->m_cursor > 0 ? d->m_text.at(d->m_cursor - 1) : QChar();
        if (previous.isLetterOrNumber() || previous == typed)
            return false;
    }

    pushUndo();
    const qsizetype at = d->m_cursor;
    applyEdit(at, 0, QString(typed) + close);
    setSelectionRange(at + 1, at + 1);
    return true;
}

bool MarkdownEditor::deleteAutoPair()
{
    if (!d->m_autoPairs || d->m_mode == Mode::Reading || selection().length() > 0 || d->m_cursor <= 0
        || d->m_cursor >= d->m_text.size())
        return false;

    const QChar open = d->m_text.at(d->m_cursor - 1);
    const QChar close = pairCloser(open, false);
    if (close.isNull() || d->m_text.at(d->m_cursor) != close)
        return false;

    pushUndo();
    applyEdit(d->m_cursor - 1, 2, QString());
    return true;
}

// --- list continuation ----------------------------------------------------

namespace {

/// Splices `lines` over `run` in place of what is there - but only the stretch
/// that differs, so an edit near the top of a long list does not rewrite (and
/// re-decorate) all of it, and what did not change stays byte for byte.
struct RunEdit {
    qsizetype at;
    qsizetype removed;
    QString inserted;
};

RunEdit diffRun(const QString &before, const QString &after, qsizetype runStart)
{
    qsizetype prefix = 0;
    const qsizetype shortest = qMin(before.size(), after.size());
    while (prefix < shortest && before.at(prefix) == after.at(prefix))
        ++prefix;
    qsizetype suffix = 0;
    while (suffix < shortest - prefix
           && before.at(before.size() - 1 - suffix) == after.at(after.size() - 1 - suffix))
        ++suffix;
    return {runStart + prefix, before.size() - prefix - suffix,
            after.mid(prefix, after.size() - prefix - suffix)};
}

} // namespace

namespace {

/// A ``` or ~~~ fence line, behind any quote marks and indent.
bool isFenceLine(QStringView line)
{
    qsizetype at = 0;
    while (at < line.size() && (line.at(at) == QLatin1Char('>') || line.at(at).isSpace()))
        ++at;
    const QStringView rest = line.mid(at);
    return rest.startsWith(QLatin1String("```")) || rest.startsWith(QLatin1String("~~~"));
}

} // namespace

bool MarkdownEditor::inCodeFenceAt(qsizetype offset) const
{
    // The decorations are the parse as of 20 ms ago (shifted by every edit
    // since), so a fence typed since then is not in them. Read the fence lines
    // in the unbroken run of lines above the caret: when the parse has seen
    // all of them (a block starts or ends on each) its answer stands; when
    // one is new, the parse's state before the oldest fence, flipped once per
    // fence line, is the answer.
    // ponytail: only that run (at most 100 lines) is read, so a fence typed
    // above a blank line the caret has since crossed is missed until the parse
    // lands; ``` and ~~~ are not told apart.
    const QStringView text(d->m_text);
    qsizetype oldest = -1;
    int fenceLines = 0;
    bool unseen = false;
    constexpr int kLinesLooked = 100; // a 5 000-line listing is not read to its top
    int looked = 0;
    for (qsizetype lineStart = lineStartOf(d->m_text, offset); lineStart > 0 && looked++ < kLinesLooked;) {
        const qsizetype above = lineStartOf(d->m_text, lineStart - 1);
        const qsizetype lineEnd = lineStart - 1;
        const QStringView line = text.mid(above, lineEnd - above);
        if (line.trimmed().isEmpty())
            break;
        if (isFenceLine(line)) {
            oldest = above;
            ++fenceLines;
            unseen = unseen || std::none_of(d->m_decorations.codeFences.cbegin(), d->m_decorations.codeFences.cend(),
                                            [&](const CodeFenceSpan &fence) {
                                                return fence.block.start == above || fence.block.end == lineEnd;
                                            });
        }
        lineStart = above;
    }
    if (!unseen)
        return decoratedFenceAt(offset);
    return (oldest > 0 && decoratedFenceAt(oldest - 1)) != (fenceLines % 2 == 1);
}

bool MarkdownEditor::decoratedFenceAt(qsizetype offset) const
{
    // Sorted by start, and a note can hold thousands: the last fence that
    // begins at or before the offset is the only one that can hold it.
    const QList<CodeFenceSpan> &fences = d->m_decorations.codeFences;
    const auto next = std::upper_bound(fences.cbegin(), fences.cend(), offset,
                                       [](qsizetype at, const CodeFenceSpan &fence) {
                                           return at < fence.block.start;
                                       });
    for (auto it = next; it != fences.cbegin();) {
        --it;
        if (it->block.contains(offset))
            return true;
        if (it->block.end < offset && it->depth == 0)
            break;
    }
    return false;
}

namespace {

/// A delimiter-row cell: dashes, a colon allowed at either end.
bool isDelimiterCell(QStringView cell)
{
    cell = cell.trimmed();
    if (cell.startsWith(QLatin1Char(':')))
        cell = cell.mid(1);
    if (cell.endsWith(QLatin1Char(':')))
        cell.chop(1);
    return !cell.isEmpty() && std::all_of(cell.cbegin(), cell.cend(), [](QChar c) { return c == QLatin1Char('-'); });
}

} // namespace

bool MarkdownEditor::handleTableEnter()
{
    if (!d->m_alignTables || d->m_mode == Mode::Reading || selection().length() > 0)
        return false;

    const qsizetype lineStart = lineStartOf(d->m_text, d->m_cursor);
    const qsizetype lineEnd = lineEndOf(d->m_text, d->m_cursor);
    // Every Enter in prose ends here, after one scan for the first pipe.
    qsizetype lead = lineStart;
    while (lead < lineEnd && (d->m_text.at(lead) == QLatin1Char('>') || d->m_text.at(lead).isSpace()))
        ++lead;
    if (lead >= lineEnd || d->m_text.at(lead) != QLatin1Char('|'))
        return false;
    // Indented four columns or more outside a quote is code, not a table.
    if (lead - lineStart >= 4 && !QStringView(d->m_text).mid(lineStart, lead - lineStart).contains(QLatin1Char('>')))
        return false;
    // The row is finished: the caret is past its closing pipe.
    qsizetype last = lineEnd;
    while (last > lead && d->m_text.at(last - 1).isSpace())
        --last;
    if (last - lead < 2 || d->m_text.at(last - 1) != QLatin1Char('|') || d->m_cursor < last)
        return false;

    // The parser's own rule for cells: an escaped pipe or one in code is text.
    const QList<Range> cells = tableRowCells(d->m_text, lineStart, lineEnd);
    bool anyText = false;
    bool allDelimiters = true;
    for (const Range &cell : cells) {
        anyText = anyText || cell.length() > 0;
        allDelimiters = allDelimiters && isDelimiterCell(QStringView(d->m_text).mid(cell.start, cell.length()));
    }
    if (!anyText || allDelimiters)
        return false; // an empty `| |` or a delimiter row typed on its own

    // A line above that has pipes: this is a row of a table, or of one in
    // the making, and gets no separator pushed in.
    if (lineStart > 0) {
        const qsizetype above = lineStartOf(d->m_text, lineStart - 1);
        if (QStringView(d->m_text).mid(above, lineStart - 1 - above).contains(QLatin1Char('|')))
            return false;
    }
    // A line below: a delimiter row says the table has one already; any other
    // text would become a row of the table about to be written.
    if (lineEnd < d->m_text.size()) {
        const qsizetype below = lineEnd + 1;
        const qsizetype belowEnd = lineEndOf(d->m_text, below);
        qsizetype text = below;
        while (text < belowEnd && (d->m_text.at(text) == QLatin1Char('>') || d->m_text.at(text).isSpace()))
            ++text;
        if (text < belowEnd)
            return false;
    }
    if (inCodeFenceAt(d->m_cursor))
        return false;

    const QString prefix = d->m_text.mid(lineStart, lead - lineStart);
    QString delimiter = prefix + QLatin1Char('|');
    QString body = delimiter;
    for (qsizetype c = 0; c < cells.size(); ++c) {
        delimiter += QStringLiteral(" --- |");
        body += QStringLiteral("   |");
    }
    const QString table = formatTable(d->m_text.mid(lineStart, lineEnd - lineStart) + QLatin1Char('\n')
                                      + delimiter + QLatin1Char('\n') + body);
    rewriteTable(lineStart, lineEnd, table);

    const qsizetype bodyStart = lineStart + table.lastIndexOf(QLatin1Char('\n')) + 1;
    const QList<Range> bodyCells = tableRowCells(d->m_text, bodyStart, lineEndOf(d->m_text, bodyStart));
    moveCursor(bodyCells.value(0, Range{bodyStart, bodyStart}).start, false);
    return true;
}

void MarkdownEditor::insertTable()
{
    if (d->m_mode == Mode::Reading)
        return;
    const qsizetype lineStart = lineStartOf(d->m_text, d->m_cursor);
    const qsizetype lineEnd = lineEndOf(d->m_text, d->m_cursor);
    const bool blank = QStringView(d->m_text).mid(lineStart, lineEnd - lineStart).trimmed().isEmpty();
    // A line with text keeps it: the table goes below, a paragraph of its own.
    const qsizetype at = blank ? lineStart : lineEnd;
    const qsizetype removed = blank ? lineEnd - lineStart : 0;
    QString text = QStringLiteral("|     |     |\n| --- | --- |\n|     |     |");
    const qsizetype tableStart = at + (blank ? 0 : 2);
    if (!blank)
        text.prepend(QStringLiteral("\n\n"));
    // Text straight below would read as one more row.
    if (lineEnd < d->m_text.size()) {
        const qsizetype below = lineEnd + 1;
        if (!QStringView(d->m_text).mid(below, lineEndOf(d->m_text, below) - below).trimmed().isEmpty())
            text += QLatin1Char('\n');
    }
    d->m_lastEdit.invalidate();
    pushUndo();
    applyEdit(at, removed, text);
    d->m_lastEdit.invalidate();
    reparse();
    moveCursor(tableStart + 2, false); // after the first "| "
}

bool MarkdownEditor::handleListEnter(bool lineBreak)
{
    if (d->m_mode == Mode::Reading || selection().length() > 0)
        return false;

    const qsizetype lineStart = lineStartOf(d->m_text, d->m_cursor);
    const qsizetype lineEnd = lineEndOf(d->m_text, d->m_cursor);
    const ListLine item = parseListLine(QStringView(d->m_text).mid(lineStart, lineEnd - lineStart),
                                         d->m_parseOptions.taskStates);
    if (!item.valid)
        return false;
    // Enter inside the marker itself is just a newline.
    if (d->m_cursor < lineStart + item.contentColumn)
        return false;
    // A listing is text: "- x" inside a fence is code, not a list.
    if (inCodeFenceAt(d->m_cursor))
        return false;

    if (lineBreak) {
        // Shift+Enter: a line break that stays in this item, its text lined up
        // under the first line's. No new bullet, no renumbering.
        insertText(QLatin1Char('\n') + item.quote + item.indent
                   + QString(item.contentColumn - item.quote.size() - item.indent.size(), QLatin1Char(' ')));
        return true;
    }

    // The whole run is rewritten in one splice so that the renumbering below
    // is part of the same edit, and so undo takes the caret back in one step.
    const Range run = listRun(d->m_cursor);
    const QString before = d->m_text.mid(run.start, run.length());
    QStringList lines = before.split(QLatin1Char('\n'));
    const qsizetype index = d->m_text.mid(run.start, lineStart - run.start)
                                .count(QLatin1Char('\n'));
    const qsizetype column = d->m_cursor - lineStart;

    qsizetype caretLine = index;
    qsizetype caretColumn = 0;
    qsizetype from = index;
    qsizetype stopWidth = item.width();

    const QString content = d->m_text.mid(lineStart + item.contentColumn,
                                       lineEnd - lineStart - item.contentColumn);
    if (content.trimmed().isEmpty()) {
        // An empty item leaves the list in steps, as Obsidian does: out of a
        // quote level first, then out one indent level, then the marker goes.
        QString &line = lines[int(index)];
        if (!item.quote.isEmpty()) {
            const qsizetype last = item.quote.lastIndexOf(QLatin1Char('>'));
            const qsizetype previous = last > 0 ? item.quote.lastIndexOf(QLatin1Char('>'), last - 1) : -1;
            qsizetype groupStart = previous < 0 ? 0 : previous + 1;
            if (previous >= 0 && groupStart < item.quote.size() && item.quote.at(groupStart) == QLatin1Char(' '))
                ++groupStart;
            line.remove(groupStart, item.quote.size() - groupStart);
            caretColumn = item.contentColumn - (item.quote.size() - groupStart);
            stopWidth = item.width();
        } else if (!item.indent.isEmpty()) {
            const qsizetype removed = outdentWidth(item);
            line.remove(item.quote.size(), removed);
            caretColumn = item.contentColumn - removed;
            stopWidth = item.width() - removed;
        } else {
            line.remove(item.quote.size(), item.contentColumn - item.quote.size());
            caretColumn = item.quote.size();
        }
    } else {
        // Split the item at the caret; what followed becomes the new item.
        const QString tail = lines.at(int(index)).mid(column);
        lines[int(index)].truncate(int(column));
        QString marker = item.quote + item.indent;
        marker += item.ordered ? QString::number(item.number + 1) + item.bullet.right(1)
                               : item.bullet;
        marker += item.spacing;
        if (!item.checkbox.isEmpty())
            marker += QStringLiteral("[ ] ");
        lines.insert(int(index) + 1, marker + tail);
        caretLine = index + 1;
        caretColumn = marker.size();
        from = index + 1;
    }

    const qsizetype sizeBefore = lines.at(int(caretLine)).size();
    renumber(lines, from, stopWidth);
    // A number can change width (9 to 10), and the caret sits after it.
    caretColumn += lines.at(int(caretLine)).size() - sizeBefore;

    qsizetype caret = run.start;
    for (qsizetype i = 0; i < caretLine; ++i)
        caret += lines.at(int(i)).size() + 1;
    caret += caretColumn;

    commitRun(run, before, lines.join(QLatin1Char('\n')), caret);
    return true;
}

bool MarkdownEditor::handleListIndent(int direction)
{
    if (d->m_mode == Mode::Reading || direction == 0 || selection().length() > 0)
        return false;

    const qsizetype lineStart = lineStartOf(d->m_text, d->m_cursor);
    const qsizetype lineEnd = lineEndOf(d->m_text, d->m_cursor);
    const ListLine item = parseListLine(QStringView(d->m_text).mid(lineStart, lineEnd - lineStart),
                                         d->m_parseOptions.taskStates);
    if (!item.valid || inCodeFenceAt(d->m_cursor))
        return false;
    if (direction < 0 && item.indent.isEmpty())
        return true; // already flush left: swallow the key, change nothing

    const Range run = listRun(d->m_cursor);
    const QString before = d->m_text.mid(run.start, run.length());
    QStringList lines = before.split(QLatin1Char('\n'));
    const qsizetype index = d->m_text.mid(run.start, lineStart - run.start)
                                .count(QLatin1Char('\n'));
    if (direction > 0) {
        // CommonMark cannot nest the first item of a list under nothing, and
        // Obsidian refuses the same keystroke; nor does it go a level deeper
        // than the item above it.
        qsizetype above = index - 1;
        while (above >= 0 && !parseListLine(lines.at(int(above))).valid)
            --above;
        if (above < 0 || item.width() > parseListLine(lines.at(int(above))).width())
            return true;
    }

    QString &line = lines[int(index)];
    const qsizetype sizeBefore = line.size();
    if (direction > 0)
        line.insert(item.quote.size(), indentUnit(item));
    else
        line.remove(item.quote.size(), outdentWidth(item));

    renumber(lines, index, parseListLine(lines.at(int(index))).width());

    // The caret keeps its place in the text; one inside the marker goes to
    // where the item's text now starts.
    const ListLine moved = parseListLine(lines.at(int(index)), d->m_parseOptions.taskStates);
    const qsizetype oldColumn = d->m_cursor - lineStart;
    const qsizetype newColumn = oldColumn < item.contentColumn
                                    ? (moved.valid ? moved.contentColumn : 0)
                                    : oldColumn + lines.at(int(index)).size() - sizeBefore;
    qsizetype caret = run.start;
    for (qsizetype i = 0; i < index; ++i)
        caret += lines.at(int(i)).size() + 1;
    caret += newColumn;

    commitRun(run, before, lines.join(QLatin1Char('\n')), caret);
    return true;
}

bool MarkdownEditor::handleListBackspace()
{
    if (d->m_mode == Mode::Reading || selection().length() > 0 || d->m_cursor == 0)
        return false;
    // A marker ends in whitespace, and this runs on every Backspace.
    const QChar before = d->m_text.at(d->m_cursor - 1);
    if (before != QLatin1Char(' ') && before != QLatin1Char('\t'))
        return false;

    const qsizetype lineStart = lineStartOf(d->m_text, d->m_cursor);
    const qsizetype lineEnd = lineEndOf(d->m_text, d->m_cursor);
    const ListLine item = parseListLine(QStringView(d->m_text).mid(lineStart, lineEnd - lineStart),
                                         d->m_parseOptions.taskStates);
    // Only at the very start of the item's text: Backspace there takes the
    // bullet (or number) off and leaves the text, as Obsidian does.
    if (!item.valid || d->m_cursor != lineStart + item.contentColumn || inCodeFenceAt(d->m_cursor))
        return false;

    const Range run = listRun(d->m_cursor);
    const QString beforeRun = d->m_text.mid(run.start, run.length());
    QStringList lines = beforeRun.split(QLatin1Char('\n'));
    const qsizetype index = d->m_text.mid(run.start, lineStart - run.start).count(QLatin1Char('\n'));
    const qsizetype markerStart = item.quote.size() + item.indent.size();
    lines[int(index)].remove(markerStart, item.contentColumn - markerStart);
    renumber(lines, index, item.width()); // the items after it close up

    qsizetype caret = run.start + markerStart;
    for (qsizetype i = 0; i < index; ++i)
        caret += lines.at(int(i)).size() + 1;

    commitRun(run, beforeRun, lines.join(QLatin1Char('\n')), caret);
    return true;
}

void MarkdownEditor::commitRun(const Range &run, const QString &before, const QString &after,
                               qsizetype caret)
{
    // Its own undo step, even straight after typing; undo then takes the
    // whole structural edit back, text and caret.
    d->m_lastEdit.invalidate();
    pushUndo();
    d->m_lastEdit.invalidate();
    const RunEdit edit = diffRun(before, after, run.start);
    applyEdit(edit.at, edit.removed, edit.inserted);
    setSelectionRange(caret, caret);
}

Range MarkdownEditor::listRun(qsizetype offset) const
{
    // The stretch of list items around `offset`, with the wrapped lines and
    // blank lines between them: a loose or multi-paragraph list is still one
    // list, and the items after an insert have to count on past them. It
    // ends at the first line that is neither, or that is in another quote.
    const qsizetype here = lineStartOf(d->m_text, offset);
    const ListLine base = parseListLine(QStringView(d->m_text).mid(here, lineEndOf(d->m_text, here) - here));
    const auto lineAt = [this](qsizetype start) {
        return QStringView(d->m_text).mid(start, lineEndOf(d->m_text, start) - start);
    };

    qsizetype start = here;
    qsizetype cursor = here;
    while (cursor > 0) {
        const qsizetype previous = lineStartOf(d->m_text, cursor - 1);
        const RunLine kind = classify(lineAt(previous), base);
        if (kind == RunLine::Other)
            break;
        cursor = previous;
        if (kind == RunLine::Item)
            start = previous;
    }

    qsizetype end = lineEndOf(d->m_text, here);
    qsizetype pending = end; // the end of the last item line seen
    cursor = end;
    while (cursor < d->m_text.size()) {
        const qsizetype next = cursor + 1;
        const RunLine kind = classify(lineAt(next), base);
        if (kind == RunLine::Other)
            break;
        cursor = lineEndOf(d->m_text, next);
        if (kind == RunLine::Item)
            pending = cursor;
    }
    return {start, pending};
}

// --- checkboxes -----------------------------------------------------------

void MarkdownEditor::toggleTaskAtCursor()
{
    if (d->m_mode == Mode::Reading)
        return;

    const qsizetype lineStart = lineStartOf(d->m_text, d->m_cursor);
    const qsizetype lineEnd = lineEndOf(d->m_text, d->m_cursor);
    const QString line = d->m_text.mid(lineStart, lineEnd - lineStart);
    const ListLine item = parseListLine(line, d->m_parseOptions.taskStates);

    const qsizetype anchor = d->m_anchor;
    const qsizetype caret = d->m_cursor;
    pushUndo();
    d->m_lastEdit.invalidate();

    if (item.valid && item.checkboxColumn >= 0) {
        // A box is three characters whatever it holds, so nothing after it moves.
        applyEdit(lineStart + item.checkboxColumn, 3, taskBox(nextTaskState(item.state)));
        setSelectionRange(anchor, caret);
        return;
    }

    // Obsidian's "Toggle checklist status" also makes a task out of a line
    // that is not one yet, rather than doing nothing.
    const QString inserted = item.valid ? QStringLiteral("[ ] ") : QStringLiteral("- [ ] ");
    const qsizetype at = lineStart + (item.valid ? item.contentColumn : 0);
    applyEdit(at, 0, inserted);
    const qsizetype shift = inserted.size();
    setSelectionRange(anchor >= at ? anchor + shift : anchor,
                      caret >= at ? caret + shift : caret);
}

void MarkdownEditor::setTaskStateAtLine(qsizetype lineStart, QChar c)
{
    if (d->m_mode == Mode::Reading)
        return;
    const QString line = d->m_text.mid(lineStart, lineEndOf(d->m_text, lineStart) - lineStart);
    const ListLine item = parseListLine(line, true);
    if (!item.valid)
        return;

    const qsizetype anchor = d->m_anchor;
    const qsizetype caret = d->m_cursor;
    pushUndo();
    d->m_lastEdit.invalidate();
    if (item.checkboxColumn >= 0) {
        applyEdit(lineStart + item.checkboxColumn, 3, taskBox(c));
        setSelectionRange(anchor, caret);
        return;
    }
    const QString inserted = taskBox(c) + QLatin1Char(' ');
    const qsizetype at = lineStart + item.contentColumn;
    applyEdit(at, 0, inserted);
    setSelectionRange(anchor >= at ? anchor + inserted.size() : anchor,
                      caret >= at ? caret + inserted.size() : caret);
}

bool MarkdownEditor::addTaskStateActions(QMenu &menu, qsizetype offset)
{
    const qsizetype lineStart = lineStartOf(d->m_text, offset);
    const ListLine item = parseListLine(
        QStringView(d->m_text).mid(lineStart, lineEndOf(d->m_text, offset) - lineStart), true);
    if (!item.valid)
        return false;

    const qreal ratio = devicePixelRatioF();
    const QChar held = isDoneBox(item.state) ? QLatin1Char('x') : item.state; // null: no box yet
    for (const TaskState &state : TaskStates::current()->all()) {
        // The box as it is drawn in the note, so the menu is a legend too.
        QPixmap pixmap(QSize(16, 16) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paintTaskBox(painter, QRectF(1, 1, 14, 14), state, font());
        painter.end();

        QString name = state.name;
        name[0] = name.at(0).toUpper();
        QAction *action = menu.addAction(QIcon(pixmap),
                                         name.replace(QLatin1Char('&'), QStringLiteral("&&"))
                                             + QLatin1Char('\t') + taskBox(state.ch));
        action->setCheckable(true);
        action->setChecked(state.ch == held);
        connect(action, &QAction::triggered, this,
                [this, lineStart, c = state.ch] { setTaskStateAtLine(lineStart, c); });
    }
    return true;
}

// --- moving and duplicating lines -----------------------------------------

void MarkdownEditor::moveLines(int direction)
{
    if (d->m_mode == Mode::Reading || direction == 0)
        return;

    const Range selected = selection();
    const qsizetype first = lineStartOf(d->m_text, selected.start);
    const qsizetype last = lineEndOf(d->m_text, selected.end);

    if (direction < 0) {
        if (first == 0)
            return;
        const qsizetype above = lineStartOf(d->m_text, first - 1);
        const QString moved = d->m_text.mid(first, last - first);
        const QString displaced = d->m_text.mid(above, first - 1 - above);
        pushUndo();
        d->m_lastEdit.invalidate();
        applyEdit(above, last - above, moved + QLatin1Char('\n') + displaced);
        const qsizetype shift = first - above;
        setSelectionRange(selected.start - shift, selected.end - shift);
        return;
    }

    if (last >= d->m_text.size())
        return;
    const qsizetype belowEnd = lineEndOf(d->m_text, last + 1);
    const QString moved = d->m_text.mid(first, last - first);
    const QString displaced = d->m_text.mid(last + 1, belowEnd - last - 1);
    pushUndo();
    d->m_lastEdit.invalidate();
    applyEdit(first, belowEnd - first, displaced + QLatin1Char('\n') + moved);
    const qsizetype shift = displaced.size() + 1;
    setSelectionRange(selected.start + shift, selected.end + shift);
}

void MarkdownEditor::duplicateLines()
{
    if (d->m_mode == Mode::Reading)
        return;

    const Range selected = selection();
    const qsizetype first = lineStartOf(d->m_text, selected.start);
    const qsizetype last = lineEndOf(d->m_text, selected.end);
    const QString copied = d->m_text.mid(first, last - first);

    pushUndo();
    d->m_lastEdit.invalidate();
    applyEdit(last, 0, QLatin1Char('\n') + copied);
    // The caret follows the copy, the way every editor's duplicate does.
    const qsizetype shift = copied.size() + 1;
    setSelectionRange(selected.start + shift, selected.end + shift);
}

// --- formatting commands --------------------------------------------------

void MarkdownEditor::toggleWrap(const QString &marker)
{
    if (d->m_mode == Mode::Reading || marker.isEmpty())
        return;

    const Range selected = selection();
    const qsizetype width = marker.size();
    const QStringView text(d->m_text);

    pushUndo();
    d->m_lastEdit.invalidate();

    // Already wrapped, with the markers inside the selection.
    if (selected.length() >= 2 * width && text.mid(selected.start, width) == marker
        && text.mid(selected.end - width, width) == marker) {
        applyEdit(selected.start, selected.length(),
                  d->m_text.mid(selected.start + width, selected.length() - 2 * width));
        setSelectionRange(selected.start, selected.end - 2 * width);
        return;
    }

    // Already wrapped, with the markers just outside it - which is what a
    // reader selecting the words themselves gets.
    if (selected.start >= width && selected.end + width <= d->m_text.size()
        && text.mid(selected.start - width, width) == marker
        && text.mid(selected.end, width) == marker) {
        applyEdit(selected.start - width, selected.length() + 2 * width,
                  d->m_text.mid(selected.start, selected.length()));
        setSelectionRange(selected.start - width, selected.end - width);
        return;
    }

    applyEdit(selected.start, selected.length(),
              marker + d->m_text.mid(selected.start, selected.length()) + marker);
    setSelectionRange(selected.start + width, selected.start + width + selected.length());
}

void MarkdownEditor::toggleBold()
{
    toggleWrap(QStringLiteral("**"));
}

void MarkdownEditor::toggleItalic()
{
    toggleWrap(QStringLiteral("*"));
}

void MarkdownEditor::toggleHighlight()
{
    toggleWrap(QStringLiteral("=="));
}

void MarkdownEditor::insertLink()
{
    if (d->m_mode == Mode::Reading)
        return;

    static const QRegularExpression wrapped(QStringLiteral("^\\[([^\\]]*)\\]\\(([^)]*)\\)$"));
    static const QRegularExpression looksLikeUrl(
        QStringLiteral("^(?:[a-z][a-z0-9+.-]*://|www\\.|mailto:)\\S+$"),
        QRegularExpression::CaseInsensitiveOption);

    const Range selected = selection();
    const QString text = d->m_text.mid(selected.start, selected.length());

    pushUndo();
    d->m_lastEdit.invalidate();

    const auto already = wrapped.match(text);
    if (already.hasMatch()) {
        applyEdit(selected.start, selected.length(), already.captured(1));
        setSelectionRange(selected.start, selected.start + already.captured(1).size());
        return;
    }

    if (text.isEmpty()) {
        // An empty selection opens the form the vault prefers, caret inside it.
        if (d->m_useWikilinks) {
            applyEdit(selected.start, 0, QStringLiteral("[[]]"));
            setSelectionRange(selected.start + 2, selected.start + 2);
        } else {
            applyEdit(selected.start, 0, QStringLiteral("[]()"));
            setSelectionRange(selected.start + 1, selected.start + 1);
        }
        return;
    }

    if (looksLikeUrl.match(text).hasMatch()) {
        // The URL is already known, so the caret belongs in the empty text slot.
        applyEdit(selected.start, selected.length(),
                  QLatin1String("[](") + text + QLatin1Char(')'));
        setSelectionRange(selected.start + 1, selected.start + 1);
        return;
    }

    // Selected text that is not a URL becomes the link's target. A vault that
    // writes wikilinks wants `[[Note]]`, where the target is the text itself
    // and there is no second slot to fill.
    if (d->m_useWikilinks) {
        applyEdit(selected.start, selected.length(),
                  QLatin1String("[[") + text + QLatin1String("]]"));
        const qsizetype end = selected.start + text.size() + 2;
        setSelectionRange(end, end);
        return;
    }

    applyEdit(selected.start, selected.length(), QLatin1Char('[') + text + QLatin1String("]()"));
    const qsizetype slot = selected.start + text.size() + 3;
    setSelectionRange(slot, slot);
}

// --- find and replace -----------------------------------------------------

void MarkdownEditor::setFindQuery(const QString &pattern, bool caseSensitive, bool regex)
{
    d->m_find.pattern = pattern;
    d->m_find.caseSensitive = caseSensitive;
    d->m_find.regex = regex;
    updateFindMatches();
}

void MarkdownEditor::updateFindMatches()
{
    const Range previous = d->m_find.current >= 0 && d->m_find.current < d->m_find.matches.size()
                               ? d->m_find.matches.at(d->m_find.current)
                               : Range{d->m_cursor, d->m_cursor};

    d->m_find.matches.clear();
    d->m_find.valid = true;

    if (!d->m_find.pattern.isEmpty()) {
        if (d->m_find.regex) {
            QRegularExpression expression(d->m_find.pattern,
                                          d->m_find.caseSensitive
                                              ? QRegularExpression::NoPatternOption
                                              : QRegularExpression::CaseInsensitiveOption);
            d->m_find.valid = expression.isValid();
            if (d->m_find.valid) {
                auto it = expression.globalMatch(d->m_text);
                while (it.hasNext()) {
                    const auto match = it.next();
                    if (match.capturedLength() == 0)
                        continue; // an empty match would never advance
                    d->m_find.matches.append({match.capturedStart(), match.capturedEnd()});
                }
            }
        } else {
            const auto sensitivity =
                d->m_find.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
            qsizetype at = 0;
            while ((at = d->m_text.indexOf(d->m_find.pattern, at, sensitivity)) >= 0) {
                d->m_find.matches.append({at, at + d->m_find.pattern.size()});
                at += d->m_find.pattern.size();
            }
        }
    }

    // Stay on the match the reader was looking at, or on the first one after
    // the caret when the query changed under them.
    d->m_find.current = -1;
    for (qsizetype i = 0; i < d->m_find.matches.size(); ++i) {
        if (d->m_find.matches.at(i).start >= previous.start) {
            d->m_find.current = int(i);
            break;
        }
    }
    if (d->m_find.current < 0 && !d->m_find.matches.isEmpty())
        d->m_find.current = 0;

    syncFindBar();
    viewport()->update();
}

void MarkdownEditor::findStep(int direction)
{
    if (d->m_find.matches.isEmpty())
        return;
    const int count = int(d->m_find.matches.size());
    if (d->m_find.current < 0)
        d->m_find.current = direction >= 0 ? 0 : count - 1;
    else
        d->m_find.current = ((d->m_find.current + (direction >= 0 ? 1 : -1)) % count + count) % count;

    const Range match = d->m_find.matches.at(d->m_find.current);
    setSelectionRange(match.start, match.end);
    scrollToOffset(match.start);
    syncFindBar();
    viewport()->update();
}

void MarkdownEditor::scrollToOffset(qsizetype offset)
{
    revealOffset(offset); // a match or a jump inside a fold opens it
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return;

    QScrollBar *bar = verticalScrollBar();
    const qreal top = d->m_blocks.at(size_t(index)).y;
    if (top < bar->value() || top > bar->value() + viewport()->height()) {
        // Setting the bar runs updateLayout() through its own valueChanged
        // connection, which lays out the blocks near the viewport and nothing
        // else. Heights settle as it does, so the second pass is the exact one.
        bar->setValue(int(qMax<qreal>(0, top - viewport()->height() / 3.0)));
    }
    ensureCursorVisible();
}

void MarkdownEditor::replaceCurrent(const QString &with)
{
    if (d->m_find.current < 0 || d->m_find.current >= d->m_find.matches.size())
        return;

    const Range match = d->m_find.matches.at(d->m_find.current);
    const int index = d->m_find.current;
    pushUndo();
    d->m_lastEdit.invalidate();
    applyEdit(match.start, match.length(), with);
    setSelectionRange(match.start + with.size(), match.start + with.size());

    updateFindMatches();
    if (!d->m_find.matches.isEmpty())
        d->m_find.current = qBound(0, index, int(d->m_find.matches.size()) - 1);
    syncFindBar();
    viewport()->update();
}

int MarkdownEditor::replaceAll(const QString &with)
{
    if (d->m_find.matches.isEmpty())
        return 0;

    // One splice over the whole span the matches cover: one undo step, and
    // the decoration pass sees a single edit rather than hundreds.
    const Range span{d->m_find.matches.first().start, d->m_find.matches.last().end};
    QString rewritten;
    rewritten.reserve(span.length());
    qsizetype cursor = span.start;
    for (const Range &match : std::as_const(d->m_find.matches)) {
        rewritten += QStringView(d->m_text).mid(cursor, match.start - cursor);
        rewritten += with;
        cursor = match.end;
    }
    const int replaced = int(d->m_find.matches.size());

    pushUndo();
    d->m_lastEdit.invalidate();
    applyEdit(span.start, span.length(), rewritten);
    setSelectionRange(span.start, span.start);
    updateFindMatches();
    return replaced;
}

QColor MarkdownEditor::findHighlightColor() const
{
    const QColor token = d->m_theme.color(QStringLiteral("--text-highlight-bg"));
    // Obsidian's own default, for a scheme that does not carry the token.
    return token.isValid() ? token : QColor(255, 208, 0, 102);
}

void MarkdownEditor::appendFindHighlights(const BlockView &view,
                                          QList<QTextLayout::FormatRange> &into) const
{
    if (d->m_find.matches.isEmpty())
        return;

    const QColor base = findHighlightColor();
    QColor strong = base;
    strong.setAlphaF(qMin(1.0f, base.alphaF() * 2.2f));

    // Matches are sorted, so only the ones that can touch this block are
    // looked at however long the note is.
    auto it = std::lower_bound(d->m_find.matches.cbegin(), d->m_find.matches.cend(),
                               view.span.range.start,
                               [](const Range &match, qsizetype start) {
                                   return match.end <= start;
                               });
    for (; it != d->m_find.matches.cend() && it->start < view.span.range.end; ++it) {
        const Range shown = view.map.toDisplay(
            {qMax(it->start, view.span.range.start), qMin(it->end, view.span.range.end)});
        if (shown.length() <= 0)
            continue;
        QTextLayout::FormatRange range;
        range.start = int(shown.start);
        range.length = int(shown.length());
        range.format.setBackground(
            int(it - d->m_find.matches.cbegin()) == d->m_find.current ? strong : base);
        into.append(range);
    }
}

void MarkdownEditor::syncFindBar()
{
    if (d->m_findBar)
        d->m_findBar->setStatus(d->m_find.current, int(d->m_find.matches.size()), d->m_find.valid);
}

} // namespace noteAppC
