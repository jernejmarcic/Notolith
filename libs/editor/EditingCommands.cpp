/// The editing behaviours from PLAN.md 4.6: auto-pairs, list continuation,
/// checkbox toggling, line moves, the formatting commands and find/replace.
///
/// They live beside MarkdownEditor.cpp rather than in it because they are all
/// the same shape - work out a splice, hand it to `applyEdit`, put the caret
/// where the reader expects it - and the engine file is long enough already.
/// Everything here goes through `applyEdit`, so one command is one undo step
/// and the decoration pass sees exactly one edit.

#include "MarkdownEditor.h"

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
    QString indent;   ///< the leading whitespace, verbatim
    QString bullet;   ///< "-", "*", "+", "1." or "1)"
    QString spacing;  ///< the whitespace after the bullet
    QString checkbox; ///< "[ ] " including its trailing space, or empty
    bool ordered = false;
    bool done = false;
    int number = 0;
    qsizetype checkboxColumn = -1; ///< column of the "[" in the line
    qsizetype contentColumn = 0;   ///< column where the item's own text starts
};

ListLine parseListLine(const QString &line)
{
    static const QRegularExpression head(QStringLiteral(
        "^([ \\t]*)([-*+]|\\d+[.)])([ \\t]+)(\\[([ xX])\\][ \\t]+)?"));

    const auto match = head.match(line);
    ListLine item;
    if (!match.hasMatch())
        return item;

    item.valid = true;
    item.indent = match.captured(1);
    item.bullet = match.captured(2);
    item.spacing = match.captured(3);
    item.checkbox = match.captured(4);
    item.ordered = item.bullet.size() > 1;
    item.number = item.ordered ? item.bullet.chopped(1).toInt() : 0;
    item.done = match.captured(5).compare(QLatin1String("x"), Qt::CaseInsensitive) == 0;
    if (!item.checkbox.isEmpty())
        item.checkboxColumn = match.capturedStart(4);
    item.contentColumn = match.capturedEnd(0);
    return item;
}

/// Renumbers the ordered items of a run in place, per indent level. The
/// outermost level keeps the number it already had, so a list written as
/// "5. 6. 7." stays at 5; a level that is entered by indenting restarts at 1,
/// which is what Obsidian does.
void renumber(QStringList &lines)
{
    struct Level {
        qsizetype width = 0;
        int next = 1;
    };
    QList<Level> stack;

    for (QString &line : lines) {
        const ListLine item = parseListLine(line);
        if (!item.valid)
            continue;

        const qsizetype width = item.indent.size();
        while (!stack.isEmpty() && stack.last().width > width)
            stack.removeLast();
        if (stack.isEmpty())
            stack.append({width, item.number > 0 ? item.number : 1});
        else if (stack.last().width < width)
            stack.append({width, 1});

        if (!item.ordered)
            continue;

        Level &level = stack.last();
        const QString wanted = QString::number(level.next) + item.bullet.right(1);
        line.replace(item.indent.size(), item.bullet.size(), wanted);
        ++level.next;
    }
}

/// One level of list indentation. Obsidian's default is a tab, and the
/// parser counts one tab as one level, so anything else would render nested
/// items at the wrong depth.
const QString &indentUnit()
{
    static const QString unit = QStringLiteral("\t");
    return unit;
}

} // namespace

void MarkdownEditor::setSelectionRange(qsizetype anchor, qsizetype cursor)
{
    m_anchor = qBound<qsizetype>(0, anchor, m_text.size());
    m_cursor = qBound<qsizetype>(0, cursor, m_text.size());
    m_activeBlock = blockAt(m_cursor);
    m_goalX = -1;
    updateLayout();
    ensureCursorVisible();
    m_cursorOn = true;
    viewport()->update();
    emit cursorMoved(m_cursor);
}

// --- auto-pairs -----------------------------------------------------------

bool MarkdownEditor::handleAutoPair(QChar typed)
{
    if (!m_autoPairs || m_mode == Mode::Reading)
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
        m_lastEdit.invalidate();
        applyEdit(selected.start, selected.length(),
                  typed + m_text.mid(selected.start, selected.length()) + close);
        setSelectionRange(selected.start + 1, selected.start + 1 + selected.length());
        return true;
    }

    // Typing the closer that is already there steps over it.
    static const QString closers = QStringLiteral(")]}\"'");
    if (closers.contains(typed) && m_cursor < m_text.size() && m_text.at(m_cursor) == typed) {
        moveCursor(m_cursor + 1, false);
        return true;
    }

    const QChar close = pairCloser(typed, false);
    if (close.isNull())
        return false;

    // CodeMirror's rule, which is what Obsidian runs: close only when what
    // follows can take a closing bracket, so "(" before a word stays alone.
    static const QString closeBefore = QStringLiteral(")]}:;>");
    const QChar next = m_cursor < m_text.size() ? m_text.at(m_cursor) : QChar();
    if (!(next.isNull() || next.isSpace() || closeBefore.contains(next)))
        return false;
    // A quote is its own closer, so it must never open inside a word:
    // "don't" would otherwise become "don''t".
    if (typed == close) {
        const QChar previous = m_cursor > 0 ? m_text.at(m_cursor - 1) : QChar();
        if (previous.isLetterOrNumber() || previous == typed)
            return false;
    }

    pushUndo();
    const qsizetype at = m_cursor;
    applyEdit(at, 0, QString(typed) + close);
    setSelectionRange(at + 1, at + 1);
    return true;
}

bool MarkdownEditor::deleteAutoPair()
{
    if (!m_autoPairs || m_mode == Mode::Reading || selection().length() > 0 || m_cursor <= 0
        || m_cursor >= m_text.size())
        return false;

    const QChar open = m_text.at(m_cursor - 1);
    const QChar close = pairCloser(open, false);
    if (close.isNull() || m_text.at(m_cursor) != close)
        return false;

    pushUndo();
    applyEdit(m_cursor - 1, 2, QString());
    return true;
}

// --- list continuation ----------------------------------------------------

bool MarkdownEditor::handleListEnter()
{
    if (m_mode == Mode::Reading || selection().length() > 0)
        return false;

    const qsizetype lineStart = lineStartOf(m_text, m_cursor);
    const qsizetype lineEnd = lineEndOf(m_text, m_cursor);
    const ListLine item = parseListLine(m_text.mid(lineStart, lineEnd - lineStart));
    if (!item.valid)
        return false;
    // Enter inside the marker itself is just a newline.
    if (m_cursor < lineStart + item.contentColumn)
        return false;

    // The whole run is rewritten in one splice so that the renumbering below
    // is part of the same edit, and so undo takes the caret back in one step.
    const Range run = listRun(m_cursor);
    QStringList lines = m_text.mid(run.start, run.length()).split(QLatin1Char('\n'));
    const qsizetype index = m_text.mid(run.start, lineStart - run.start)
                                .count(QLatin1Char('\n'));
    const qsizetype column = m_cursor - lineStart;

    qsizetype caretLine = index;
    qsizetype caretColumn = 0;

    const QString content = m_text.mid(lineStart + item.contentColumn,
                                       lineEnd - lineStart - item.contentColumn);
    if (content.trimmed().isEmpty()) {
        // An empty item ends the list - after stepping out one level first,
        // if there is a level to step out of.
        if (!item.indent.isEmpty()) {
            QString &line = lines[int(index)];
            const qsizetype removed = line.startsWith(indentUnit())
                                          ? indentUnit().size()
                                          : qMin<qsizetype>(4, item.indent.size());
            line.remove(0, removed);
            caretColumn = item.contentColumn - removed;
        } else {
            lines[int(index)].remove(0, item.contentColumn);
            caretColumn = 0;
        }
    } else {
        // Split the item at the caret; what followed becomes the new item.
        const QString tail = lines.at(int(index)).mid(column);
        lines[int(index)].truncate(int(column));
        QString marker = item.indent;
        marker += item.ordered ? QString::number(item.number + 1) + item.bullet.right(1)
                               : item.bullet;
        marker += item.spacing;
        if (!item.checkbox.isEmpty())
            marker += QStringLiteral("[ ] ");
        lines.insert(int(index) + 1, marker + tail);
        caretLine = index + 1;
        caretColumn = marker.size();
    }

    renumber(lines);

    qsizetype caret = run.start;
    for (qsizetype i = 0; i < caretLine; ++i)
        caret += lines.at(int(i)).size() + 1;
    caret += caretColumn;

    pushUndo();
    m_lastEdit.invalidate();
    applyEdit(run.start, run.length(), lines.join(QLatin1Char('\n')));
    setSelectionRange(caret, caret);
    return true;
}

bool MarkdownEditor::handleListIndent(int direction)
{
    if (m_mode == Mode::Reading || direction == 0)
        return false;

    const qsizetype lineStart = lineStartOf(m_text, m_cursor);
    const qsizetype lineEnd = lineEndOf(m_text, m_cursor);
    const ListLine item = parseListLine(m_text.mid(lineStart, lineEnd - lineStart));
    if (!item.valid)
        return false;
    // Tab indents from the marker; further in it is still a tab character.
    // Shift+Tab has nothing else to mean, so it outdents from anywhere.
    if (direction > 0 && m_cursor > lineStart + item.contentColumn)
        return false;
    if (direction < 0 && item.indent.isEmpty())
        return true; // already flush left: swallow the key, change nothing

    const Range run = listRun(m_cursor);
    QStringList lines = m_text.mid(run.start, run.length()).split(QLatin1Char('\n'));
    const qsizetype index = m_text.mid(run.start, lineStart - run.start)
                                .count(QLatin1Char('\n'));
    // CommonMark cannot nest the first item of a list under nothing, and
    // Obsidian refuses the same keystroke.
    if (direction > 0 && index == 0)
        return true;

    QString &line = lines[int(index)];
    qsizetype shift = 0;
    if (direction > 0) {
        line.prepend(indentUnit());
        shift = indentUnit().size();
    } else {
        const qsizetype removed = line.startsWith(indentUnit())
                                      ? indentUnit().size()
                                      : qMin<qsizetype>(4, item.indent.size());
        line.remove(0, removed);
        shift = -removed;
    }

    renumber(lines);

    qsizetype caret = run.start;
    for (qsizetype i = 0; i < index; ++i)
        caret += lines.at(int(i)).size() + 1;
    // Renumbering can change the marker's width, so measure it again.
    const ListLine moved = parseListLine(lines.at(int(index)));
    caret += qMax<qsizetype>(m_cursor - lineStart + shift,
                             moved.valid ? moved.contentColumn : 0);

    pushUndo();
    m_lastEdit.invalidate();
    applyEdit(run.start, run.length(), lines.join(QLatin1Char('\n')));
    setSelectionRange(caret, caret);
    return true;
}

Range MarkdownEditor::listRun(qsizetype offset) const
{
    // ponytail: a wrapped continuation line under an item ends the run, so a
    // list broken across paragraphs renumbers in pieces. Widen this when a
    // note that does that turns up.
    qsizetype start = lineStartOf(m_text, offset);
    while (start > 0) {
        const qsizetype previous = lineStartOf(m_text, start - 1);
        if (!parseListLine(m_text.mid(previous, lineEndOf(m_text, previous) - previous)).valid)
            break;
        start = previous;
    }

    qsizetype end = lineEndOf(m_text, offset);
    while (end < m_text.size()) {
        const qsizetype next = end + 1;
        if (!parseListLine(m_text.mid(next, lineEndOf(m_text, next) - next)).valid)
            break;
        end = lineEndOf(m_text, next);
    }
    return {start, end};
}

// --- checkboxes -----------------------------------------------------------

void MarkdownEditor::toggleTaskAtCursor()
{
    if (m_mode == Mode::Reading)
        return;

    const qsizetype lineStart = lineStartOf(m_text, m_cursor);
    const qsizetype lineEnd = lineEndOf(m_text, m_cursor);
    const QString line = m_text.mid(lineStart, lineEnd - lineStart);
    const ListLine item = parseListLine(line);

    const qsizetype anchor = m_anchor;
    const qsizetype caret = m_cursor;
    pushUndo();
    m_lastEdit.invalidate();

    if (item.valid && item.checkboxColumn >= 0) {
        // "[x]" and "[ ]" are the same width, so nothing after it moves.
        applyEdit(lineStart + item.checkboxColumn, 3,
                  item.done ? QStringLiteral("[ ]") : QStringLiteral("[x]"));
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

// --- moving and duplicating lines -----------------------------------------

void MarkdownEditor::moveLines(int direction)
{
    if (m_mode == Mode::Reading || direction == 0)
        return;

    const Range selected = selection();
    const qsizetype first = lineStartOf(m_text, selected.start);
    const qsizetype last = lineEndOf(m_text, selected.end);

    if (direction < 0) {
        if (first == 0)
            return;
        const qsizetype above = lineStartOf(m_text, first - 1);
        const QString moved = m_text.mid(first, last - first);
        const QString displaced = m_text.mid(above, first - 1 - above);
        pushUndo();
        m_lastEdit.invalidate();
        applyEdit(above, last - above, moved + QLatin1Char('\n') + displaced);
        const qsizetype shift = first - above;
        setSelectionRange(selected.start - shift, selected.end - shift);
        return;
    }

    if (last >= m_text.size())
        return;
    const qsizetype belowEnd = lineEndOf(m_text, last + 1);
    const QString moved = m_text.mid(first, last - first);
    const QString displaced = m_text.mid(last + 1, belowEnd - last - 1);
    pushUndo();
    m_lastEdit.invalidate();
    applyEdit(first, belowEnd - first, displaced + QLatin1Char('\n') + moved);
    const qsizetype shift = displaced.size() + 1;
    setSelectionRange(selected.start + shift, selected.end + shift);
}

void MarkdownEditor::duplicateLines()
{
    if (m_mode == Mode::Reading)
        return;

    const Range selected = selection();
    const qsizetype first = lineStartOf(m_text, selected.start);
    const qsizetype last = lineEndOf(m_text, selected.end);
    const QString copied = m_text.mid(first, last - first);

    pushUndo();
    m_lastEdit.invalidate();
    applyEdit(last, 0, QLatin1Char('\n') + copied);
    // The caret follows the copy, the way every editor's duplicate does.
    const qsizetype shift = copied.size() + 1;
    setSelectionRange(selected.start + shift, selected.end + shift);
}

// --- formatting commands --------------------------------------------------

void MarkdownEditor::toggleWrap(const QString &marker)
{
    if (m_mode == Mode::Reading || marker.isEmpty())
        return;

    const Range selected = selection();
    const qsizetype width = marker.size();
    const QStringView text(m_text);

    pushUndo();
    m_lastEdit.invalidate();

    // Already wrapped, with the markers inside the selection.
    if (selected.length() >= 2 * width && text.mid(selected.start, width) == marker
        && text.mid(selected.end - width, width) == marker) {
        applyEdit(selected.start, selected.length(),
                  m_text.mid(selected.start + width, selected.length() - 2 * width));
        setSelectionRange(selected.start, selected.end - 2 * width);
        return;
    }

    // Already wrapped, with the markers just outside it - which is what a
    // reader selecting the words themselves gets.
    if (selected.start >= width && selected.end + width <= m_text.size()
        && text.mid(selected.start - width, width) == marker
        && text.mid(selected.end, width) == marker) {
        applyEdit(selected.start - width, selected.length() + 2 * width,
                  m_text.mid(selected.start, selected.length()));
        setSelectionRange(selected.start - width, selected.end - width);
        return;
    }

    applyEdit(selected.start, selected.length(),
              marker + m_text.mid(selected.start, selected.length()) + marker);
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
    if (m_mode == Mode::Reading)
        return;

    static const QRegularExpression wrapped(QStringLiteral("^\\[([^\\]]*)\\]\\(([^)]*)\\)$"));
    static const QRegularExpression looksLikeUrl(
        QStringLiteral("^(?:[a-z][a-z0-9+.-]*://|www\\.|mailto:)\\S+$"),
        QRegularExpression::CaseInsensitiveOption);

    const Range selected = selection();
    const QString text = m_text.mid(selected.start, selected.length());

    pushUndo();
    m_lastEdit.invalidate();

    const auto already = wrapped.match(text);
    if (already.hasMatch()) {
        applyEdit(selected.start, selected.length(), already.captured(1));
        setSelectionRange(selected.start, selected.start + already.captured(1).size());
        return;
    }

    if (text.isEmpty()) {
        // An empty selection opens the form the vault prefers, caret inside it.
        if (m_useWikilinks) {
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
    if (m_useWikilinks) {
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
    m_find.pattern = pattern;
    m_find.caseSensitive = caseSensitive;
    m_find.regex = regex;
    updateFindMatches();
}

void MarkdownEditor::updateFindMatches()
{
    const Range previous = m_find.current >= 0 && m_find.current < m_find.matches.size()
                               ? m_find.matches.at(m_find.current)
                               : Range{m_cursor, m_cursor};

    m_find.matches.clear();
    m_find.valid = true;

    if (!m_find.pattern.isEmpty()) {
        if (m_find.regex) {
            QRegularExpression expression(m_find.pattern,
                                          m_find.caseSensitive
                                              ? QRegularExpression::NoPatternOption
                                              : QRegularExpression::CaseInsensitiveOption);
            m_find.valid = expression.isValid();
            if (m_find.valid) {
                auto it = expression.globalMatch(m_text);
                while (it.hasNext()) {
                    const auto match = it.next();
                    if (match.capturedLength() == 0)
                        continue; // an empty match would never advance
                    m_find.matches.append({match.capturedStart(), match.capturedEnd()});
                }
            }
        } else {
            const auto sensitivity =
                m_find.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
            qsizetype at = 0;
            while ((at = m_text.indexOf(m_find.pattern, at, sensitivity)) >= 0) {
                m_find.matches.append({at, at + m_find.pattern.size()});
                at += m_find.pattern.size();
            }
        }
    }

    // Stay on the match the reader was looking at, or on the first one after
    // the caret when the query changed under them.
    m_find.current = -1;
    for (qsizetype i = 0; i < m_find.matches.size(); ++i) {
        if (m_find.matches.at(i).start >= previous.start) {
            m_find.current = int(i);
            break;
        }
    }
    if (m_find.current < 0 && !m_find.matches.isEmpty())
        m_find.current = 0;

    syncFindBar();
    viewport()->update();
}

void MarkdownEditor::findStep(int direction)
{
    if (m_find.matches.isEmpty())
        return;
    const int count = int(m_find.matches.size());
    if (m_find.current < 0)
        m_find.current = direction >= 0 ? 0 : count - 1;
    else
        m_find.current = ((m_find.current + (direction >= 0 ? 1 : -1)) % count + count) % count;

    const Range match = m_find.matches.at(m_find.current);
    setSelectionRange(match.start, match.end);
    scrollToOffset(match.start);
    syncFindBar();
    viewport()->update();
}

void MarkdownEditor::scrollToOffset(qsizetype offset)
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return;

    QScrollBar *bar = verticalScrollBar();
    const qreal top = m_blocks.at(size_t(index)).y;
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
    if (m_find.current < 0 || m_find.current >= m_find.matches.size())
        return;

    const Range match = m_find.matches.at(m_find.current);
    const int index = m_find.current;
    pushUndo();
    m_lastEdit.invalidate();
    applyEdit(match.start, match.length(), with);
    setSelectionRange(match.start + with.size(), match.start + with.size());

    updateFindMatches();
    if (!m_find.matches.isEmpty())
        m_find.current = qBound(0, index, int(m_find.matches.size()) - 1);
    syncFindBar();
    viewport()->update();
}

int MarkdownEditor::replaceAll(const QString &with)
{
    if (m_find.matches.isEmpty())
        return 0;

    // One splice over the whole span the matches cover: one undo step, and
    // the decoration pass sees a single edit rather than hundreds.
    const Range span{m_find.matches.first().start, m_find.matches.last().end};
    QString rewritten;
    rewritten.reserve(span.length());
    qsizetype cursor = span.start;
    for (const Range &match : std::as_const(m_find.matches)) {
        rewritten += QStringView(m_text).mid(cursor, match.start - cursor);
        rewritten += with;
        cursor = match.end;
    }
    const int replaced = int(m_find.matches.size());

    pushUndo();
    m_lastEdit.invalidate();
    applyEdit(span.start, span.length(), rewritten);
    setSelectionRange(span.start, span.start);
    updateFindMatches();
    return replaced;
}

QColor MarkdownEditor::findHighlightColor() const
{
    const QColor token = m_theme.color(QStringLiteral("--text-highlight-bg"));
    // Obsidian's own default, for a scheme that does not carry the token.
    return token.isValid() ? token : QColor(255, 208, 0, 102);
}

void MarkdownEditor::appendFindHighlights(const BlockView &view,
                                          QList<QTextLayout::FormatRange> &into) const
{
    if (m_find.matches.isEmpty())
        return;

    const QColor base = findHighlightColor();
    QColor strong = base;
    strong.setAlphaF(qMin(1.0f, base.alphaF() * 2.2f));

    // Matches are sorted, so only the ones that can touch this block are
    // looked at however long the note is.
    auto it = std::lower_bound(m_find.matches.cbegin(), m_find.matches.cend(),
                               view.span.range.start,
                               [](const Range &match, qsizetype start) {
                                   return match.end <= start;
                               });
    for (; it != m_find.matches.cend() && it->start < view.span.range.end; ++it) {
        const Range shown = view.map.toDisplay(
            {qMax(it->start, view.span.range.start), qMin(it->end, view.span.range.end)});
        if (shown.length() <= 0)
            continue;
        QTextLayout::FormatRange range;
        range.start = int(shown.start);
        range.length = int(shown.length());
        range.format.setBackground(
            int(it - m_find.matches.cbegin()) == m_find.current ? strong : base);
        into.append(range);
    }
}

void MarkdownEditor::syncFindBar()
{
    if (m_findBar)
        m_findBar->setStatus(m_find.current, int(m_find.matches.size()), m_find.valid);
}

} // namespace noteAppC
