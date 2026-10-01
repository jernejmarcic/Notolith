/// Vim's modal editing for the note editor, optional and off by default.
///
/// Hand-rolled because no vim-emulation library is installed (FakeVim ships
/// with Qt Creator only). It covers the part of vim a note-taker lives in:
/// normal, insert, visual and visual-line modes; counts; the motions
/// h j k l w W b B e E 0 ^ $ gg G { } f F t T ; , n N and gj gk; the operators
/// d c y with any of them, doubled for a line; x X D C s S p P u Ctrl+r J r ~
/// . i a I A o O; text objects (iw aw is as ip ap, quotes, brackets); % * #;
/// > and <; named registers; marks; macros; block-visual (Ctrl+v); and a `:` /
/// `/` / `?` line (`:w :q :wq :x :q! :noh :N` and `:s` with a range).
///
/// It is a friend of MarkdownEditor and works on the editor's own buffer,
/// caret and undo stack, so nothing here is a second copy of anything. An
/// instance lives for one keystroke; everything it remembers is
/// MarkdownEditor::VimState.

#include "MarkdownEditor.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QRegularExpression>

#include <algorithm>
#include <limits>
#include <utility>

namespace noteAppC {

namespace {

using Pos = qsizetype;

constexpr QChar kEsc(0x1b);
constexpr QChar kGoTop(0x01); ///< the internal key for gg
constexpr QChar kCtrlV(0x16); ///< Ctrl+v: block-visual

// --- text helpers over the note's source ------------------------------------

Pos lineStartOf(const QString &t, Pos p)
{
    p = qBound<Pos>(0, p, t.size());
    return p == 0 ? 0 : t.lastIndexOf(QLatin1Char('\n'), p - 1) + 1;
}

/// Just past the last character of the line, a CR before the newline excluded.
Pos lineEndOf(const QString &t, Pos p)
{
    p = qBound<Pos>(0, p, t.size());
    Pos end = t.indexOf(QLatin1Char('\n'), p);
    if (end < 0)
        end = t.size();
    if (end > lineStartOf(t, p) && t.at(end - 1) == QLatin1Char('\r'))
        --end;
    return end;
}

Pos firstNonBlank(const QString &t, Pos p)
{
    Pos i = lineStartOf(t, p);
    const Pos end = lineEndOf(t, p);
    while (i < end && (t.at(i) == QLatin1Char(' ') || t.at(i) == QLatin1Char('\t')))
        ++i;
    return i;
}

/// Start of the line after the one holding `p`, or -1 on the last line.
Pos nextLineStart(const QString &t, Pos p)
{
    const Pos newline = t.indexOf(QLatin1Char('\n'), qBound<Pos>(0, p, t.size()));
    return newline < 0 ? -1 : newline + 1;
}

/// Start of the next line that has text, or -1: the break that ends the note
/// does not open another line, as in vim.
Pos nextLine(const QString &t, Pos p)
{
    const Pos next = nextLineStart(t, p);
    return next >= t.size() ? -1 : next;
}

/// One past the character at `p`, unless that is the line break.
Pos afterChar(const QString &t, Pos p)
{
    return p < t.size() && t.at(p) != QLatin1Char('\n') ? p + 1 : p;
}

Pos lineStartByNumber(const QString &t, Pos number)
{
    Pos p = 0;
    for (; number > 0; --number) {
        const Pos next = nextLineStart(t, p);
        if (next < 0)
            break;
        p = next;
    }
    return p;
}

/// 0 space, 1 punctuation, 2 word. `big` is vim's WORD: anything not a space.
int charClass(QChar c, bool big)
{
    if (c.isSpace())
        return 0;
    if (big)
        return 2;
    return c.isLetterOrNumber() || c == QLatin1Char('_') ? 2 : 1;
}

bool emptyLineAt(const QString &t, Pos p)
{
    return p < t.size() && t.at(p) == QLatin1Char('\n') && (p == 0 || t.at(p - 1) == QLatin1Char('\n'));
}

Pos wordForward(const QString &t, Pos p, bool big)
{
    const Pos size = t.size();
    if (p >= size)
        return size;
    const Pos start = p;
    if (const int cls = charClass(t.at(p), big); cls != 0)
        while (p < size && charClass(t.at(p), big) == cls)
            ++p;
    // Spaces and line breaks, but an empty line is a word of its own.
    while (p < size && charClass(t.at(p), big) == 0) {
        if (p != start && emptyLineAt(t, p))
            break;
        ++p;
    }
    return p;
}

Pos wordBack(const QString &t, Pos p, bool big)
{
    if (p <= 0)
        return 0;
    --p;
    while (charClass(t.at(p), big) == 0) {
        if (emptyLineAt(t, p) || p == 0)
            return p;
        --p;
    }
    const int cls = charClass(t.at(p), big);
    while (p > 0 && charClass(t.at(p - 1), big) == cls)
        --p;
    return p;
}

Pos wordEnd(const QString &t, Pos p, bool big)
{
    const Pos size = t.size();
    if (p + 1 >= size)
        return qMax<Pos>(0, size - 1);
    ++p;
    while (p < size && charClass(t.at(p), big) == 0)
        ++p;
    if (p >= size)
        return size - 1;
    const int cls = charClass(t.at(p), big);
    while (p + 1 < size && charClass(t.at(p + 1), big) == cls)
        ++p;
    return p;
}

bool blankLineAt(const QString &t, Pos p)
{
    return p >= t.size() || t.at(p) == QLatin1Char('\n');
}

Pos paragraphForward(const QString &t, Pos p)
{
    Pos i = lineStartOf(t, p);
    while (i < t.size() && blankLineAt(t, i)) {
        const Pos next = nextLineStart(t, i);
        if (next < 0)
            return t.size();
        i = next;
    }
    while (i < t.size() && !blankLineAt(t, i)) {
        const Pos next = nextLineStart(t, i);
        if (next < 0)
            return t.size();
        i = next;
    }
    return i;
}

Pos paragraphBack(const QString &t, Pos p)
{
    Pos i = lineStartOf(t, p);
    if (i == 0)
        return 0;
    i = lineStartOf(t, i - 1);
    while (i > 0 && blankLineAt(t, i))
        i = lineStartOf(t, i - 1);
    while (i > 0 && !blankLineAt(t, i))
        i = lineStartOf(t, i - 1);
    return i;
}

bool isBlankLine(const QString &t, Pos p)
{
    for (Pos i = lineStartOf(t, p), e = lineEndOf(t, p); i < e; ++i)
        if (!t.at(i).isSpace())
            return false;
    return true;
}

Pos lastLineStart(const QString &t)
{
    Pos last = t.size();
    if (last > 0 && t.at(last - 1) == QLatin1Char('\n'))
        --last;
    return lineStartOf(t, last);
}

/// The run of one character class around `p`, kept to its line.
Range runAround(const QString &t, Pos p, bool big)
{
    const Pos start = lineStartOf(t, p);
    const Pos end = lineEndOf(t, p);
    if (p >= end)
        return {p, p};
    const int cls = charClass(t.at(p), big);
    Pos a = p;
    Pos b = p + 1;
    while (a > start && charClass(t.at(a - 1), big) == cls)
        --a;
    while (b < end && charClass(t.at(b), big) == cls)
        ++b;
    return {a, b};
}

/// iw / aw (and iW / aW): `n` words; aw takes the space after the word, or
/// before it when there is none, or the word after when the caret is on space.
Range wordObject(const QString &t, Pos p, bool big, bool inner, int n)
{
    const Pos start = lineStartOf(t, p);
    const Pos end = lineEndOf(t, p);
    if (p >= end)
        return {p, p};
    const auto nextRun = [&](Pos from) { return from < end ? runAround(t, from, big).end : from; };
    const auto spaceAt = [&](Pos q) { return q < end && charClass(t.at(q), big) == 0; };
    const Range run = runAround(t, p, big);
    const bool onSpace = charClass(t.at(p), big) == 0;
    Pos first = run.start;
    Pos last = run.end;
    if (inner) {
        for (int i = 1; i < n; ++i)
            last = nextRun(last);
        return {first, last};
    }
    bool trailing = true;
    if (onSpace) { // the space, then the word after it
        last = nextRun(last);
        for (int i = 1; i < n; ++i) {
            if (spaceAt(last))
                last = nextRun(last);
            last = nextRun(last);
        }
    } else {
        trailing = spaceAt(last);
        if (trailing)
            last = nextRun(last);
        for (int i = 1; i < n; ++i) {
            last = nextRun(last);
            trailing = spaceAt(last);
            if (trailing)
                last = nextRun(last);
        }
        if (!trailing)
            while (first > start && charClass(t.at(first - 1), big) == 0)
                --first;
    }
    return {first, last};
}

/// is / as: a sentence ends at . ! or ? (and any closing quotes or brackets)
/// followed by space or the paragraph's end.
bool sentenceObject(const QString &t, Pos p, bool inner, int n, Range &out)
{
    if (isBlankLine(t, p))
        return false;
    Pos top = lineStartOf(t, p);
    while (top > 0 && !isBlankLine(t, top - 1))
        top = lineStartOf(t, top - 1);
    Pos bottom = lineStartOf(t, p);
    for (Pos next = nextLine(t, bottom); next >= 0 && !isBlankLine(t, next); next = nextLine(t, bottom))
        bottom = next;
    const Pos end = lineEndOf(t, bottom);
    struct Part {
        Pos start, stop, gap; ///< the sentence, and where the space after it ends
    };
    QList<Part> parts;
    for (Pos i = top; i < end;) {
        Pos j = i;
        Pos stop = end;
        for (; j < end; ++j) {
            if (!QStringView(u".!?").contains(t.at(j)))
                continue;
            Pos k = j + 1;
            while (k < end && QStringView(u")]\"'").contains(t.at(k)))
                ++k;
            if (k >= end || t.at(k).isSpace()) {
                stop = k;
                break;
            }
        }
        Pos gap = stop;
        while (gap < end && t.at(gap).isSpace())
            ++gap;
        parts.append({i, stop, gap});
        i = gap;
    }
    qsizetype k = 0;
    while (k < parts.size() && p >= parts.at(k).gap)
        ++k;
    if (k >= parts.size())
        return false;
    const Part &part = parts.at(k);
    const bool inGap = p >= part.stop;
    const qsizetype lastIndex = qMin<qsizetype>(parts.size() - 1, k + n - 1 + (inGap && !inner ? 1 : 0));
    if (inGap && inner) {
        out = {part.stop, part.gap};
    } else if (inGap) { // the space and the sentence after it
        out = {part.stop, parts.at(lastIndex).stop};
    } else if (inner) {
        out = {part.start, parts.at(lastIndex).stop};
    } else {
        out = {part.start, parts.at(lastIndex).gap};
        if (parts.at(lastIndex).gap == parts.at(lastIndex).stop && k > 0) // none after: the space before
            out.start = parts.at(k - 1).stop;
    }
    return out.length() > 0;
}

/// ip / ap, as whole lines: `out` runs from the first line's start to the last
/// line's end.
bool paragraphObject(const QString &t, Pos p, bool inner, int n, Range &out)
{
    const auto blankAt = [&](Pos line) { return line >= t.size() || isBlankLine(t, line); };
    const auto runStart = [&](Pos line, bool blank) {
        while (line > 0 && blankAt(lineStartOf(t, line - 1)) == blank)
            line = lineStartOf(t, line - 1);
        return line;
    };
    // the first line after the run `line` is in, or -1 at the end of the note
    const auto runNext = [&](Pos line, bool blank) -> Pos {
        while (blankAt(line) == blank) {
            const Pos next = nextLine(t, line);
            if (next < 0)
                return -1;
            line = next;
        }
        return line;
    };
    const Pos line0 = lineStartOf(t, p);
    const bool blank0 = blankAt(line0);
    Pos first = runStart(line0, blank0);
    Pos from = first;
    bool blank = blank0;
    bool atEnd = false;
    for (int i = 0, units = inner ? n : 2 * n; i < units && !atEnd; ++i) {
        const Pos next = runNext(from, blank);
        if (next < 0)
            atEnd = true;
        else
            from = next;
        blank = !blank;
    }
    const Pos lastLine = atEnd ? lastLineStart(t) : lineStartOf(t, from - 1);
    if (!inner && !blank0 && !blankAt(lastLine) && first > 0) { // no blank lines after: take those before
        const Pos before = lineStartOf(t, first - 1);
        if (blankAt(before))
            first = runStart(before, true);
    }
    out = {first, lineEndOf(t, lastLine)};
    return true;
}

/// The "(" before `p` that encloses it, `n` levels out; -1 for none.
Pos findOpen(const QString &t, Pos p, QChar open, QChar close, int n)
{
    Pos i = qMin<Pos>(p, t.size() - 1);
    if (i >= 0 && t.at(i) == close)
        --i; // on the closer: inside the pair it closes
    for (int level = 1;; ++level) {
        int depth = 0;
        Pos found = -1;
        for (; i >= 0; --i) {
            const QChar c = t.at(i);
            if (c == close) {
                ++depth;
            } else if (c == open) {
                if (depth == 0) {
                    found = i;
                    break;
                }
                --depth;
            }
        }
        if (found < 0)
            return -1;
        if (level >= n)
            return found;
        i = found - 1;
    }
}

Pos matchClose(const QString &t, Pos open, QChar openChar, QChar closeChar)
{
    int depth = 0;
    for (Pos i = open + 1; i < t.size(); ++i) {
        const QChar c = t.at(i);
        if (c == openChar) {
            ++depth;
        } else if (c == closeChar) {
            if (depth == 0)
                return i;
            --depth;
        }
    }
    return -1;
}

/// i( a( and the other brackets. A block with its brackets on lines of their
/// own gives the lines between them, whole.
bool bracketObject(const QString &t, Pos p, QChar open, QChar close, bool inner, int n, Range &out,
                   bool &linewise)
{
    const Pos o = findOpen(t, p, open, close, n);
    if (o < 0)
        return false;
    const Pos c = matchClose(t, o, open, close);
    if (c < 0)
        return false;
    if (!inner) {
        out = {o, c + 1};
        return true;
    }
    out = {o + 1, c};
    const Pos closeLine = lineStartOf(t, c);
    const Pos next = nextLineStart(t, o);
    if (next >= 0 && next <= closeLine && closeLine > lineStartOf(t, o)
        && t.mid(o + 1, lineEndOf(t, o) - o - 1).trimmed().isEmpty()
        && t.mid(closeLine, c - closeLine).trimmed().isEmpty()) {
        if (next < closeLine) {
            out = {next, closeLine - 1};
            linewise = true;
        } else {
            out = {next - 1, next - 1};
        }
    }
    return true;
}

/// i" a" and friends: the quoted string around the caret on its line, or the
/// first one after it. a" takes the space after, or before when there is none.
bool quoteObject(const QString &t, Pos p, QChar quote, bool inner, Range &out)
{
    const Pos start = lineStartOf(t, p);
    const Pos end = lineEndOf(t, p);
    QList<Pos> marks;
    for (Pos i = start; i < end; ++i) {
        int slashes = 0;
        for (Pos j = i; j > start && t.at(j - 1) == QLatin1Char('\\'); --j)
            ++slashes;
        if (t.at(i) == quote && slashes % 2 == 0)
            marks.append(i);
    }
    for (qsizetype k = 0; k + 1 < marks.size(); k += 2) {
        if (p > marks.at(k + 1))
            continue;
        const Pos a = marks.at(k);
        const Pos b = marks.at(k + 1);
        if (inner) {
            out = {a + 1, b};
            return true;
        }
        Pos from = a;
        Pos to = b + 1;
        if (to < end && t.at(to).isSpace())
            while (to < end && t.at(to).isSpace())
                ++to;
        else
            while (from > start && t.at(from - 1).isSpace())
                --from;
        out = {from, to};
        return true;
    }
    return false;
}

struct Move {
    Pos pos = 0;
    bool inclusive = false; ///< the character at pos is part of an operator's range
    bool linewise = false;
};

} // namespace

class VimEngine {
public:
    explicit VimEngine(MarkdownEditor &editor)
        : m(editor), s(editor.m_vim), t(editor.m_text)
    {
    }

    bool key(QKeyEvent *event);

private:
    using Mode = MarkdownEditor::VimState::Mode;
    MarkdownEditor &m;
    MarkdownEditor::VimState &s;
    const QString &t;

    Pos ls(Pos p) const { return lineStartOf(t, p); }
    Pos le(Pos p) const { return lineEndOf(t, p); }
    bool visual() const
    {
        return s.mode == Mode::Visual || s.mode == Mode::VisualLine || s.mode == Mode::VisualBlock;
    }
    int total() const { return qMax(1, s.count) * qMax(1, s.count2); }
    bool hasCount() const { return s.count > 0 || s.count2 > 0; }

    void feed(QChar c);
    void abort();
    void finish(bool change);
    void normalize();
    void go(Pos pos)
    {
        m.m_goalX = -1; // gj and gk aim for a pixel column; any other move forgets it
        m.moveCursor(pos, visual());
    }
    void edit(Pos at, qsizetype removed, const QString &inserted);

    bool motion(QChar key, int n, bool hasCount, Move &out);
    void runMotion(QChar key);
    void displayLines(int direction);
    void applyMove(const Move &mv);
    void lineOperator(QChar op);
    void doOperator(QChar op, Range r, bool linewise, Pos hint, int times = 1);
    void visualOperator(QChar op);
    void shiftLines(QChar op, Range r, bool linewise, int times);

    struct Block {
        Pos first = 0;        ///< start of the top line
        int lines = 1;
        qsizetype c1 = 0, c2 = 0; ///< columns, c2 included
        bool toEnd = false;   ///< $ was pressed: every line runs to its end
    };
    Block blockGeometry() const;
    QList<Range> blockRanges(const Block &g) const;
    void blockOperator(QChar op, int times);
    void blockInsert(bool append);
    void replicateBlockInsert();
    void pasteBlock(const QString &text, bool after);

    using Register = MarkdownEditor::VimState::Register;
    void storeReg(const QString &text, int kind, bool yank);
    bool loadReg(QString &text, int &kind) const;
    void setJumpMark() { s.marks.insert(QLatin1Char('\''), m.m_cursor); }
    void saveVisualMarks();
    void applyObject(bool inner, QChar object);
    bool wordUnder(Pos at, Pos &from, Pos &to) const;
    bool searchMove(bool forward, int n, Pos from, Pos &out);
    void runMacro(QChar name, int times);
    bool address(const QString &x, qsizetype &i, Pos &line) const;
    Pos lineAdd(Pos line, int delta) const;
    void substitute(Pos first, Pos last, const QString &spec);

    void beginGroup();
    void endGroup();
    void enterInsert(Pos at);
    void leaveInsert();
    void enterVisual(Mode mode);
    void exitVisual();
    void syncVisual();

    void paste(bool after);
    void replaceChars(QChar c);
    void toggleCase();
    void join();
    void openLine(bool below);
    void undoRedo(bool redo);
    void repeatLastChange();

    void startCommandLine(QChar kind);
    void runCommandLine();
    void exCommand(const QString &text);
    bool searchFrom(bool forward, Pos &out);
    void search(const QString &pattern, bool forward);
};

// --- editing plumbing -------------------------------------------------------

void VimEngine::edit(Pos at, qsizetype removed, const QString &inserted)
{
    if (!s.hold) { // each command outside an insert session is its own undo step
        m.m_lastEdit.invalidate();
        m.pushUndo();
    }
    m.applyEdit(at, removed, inserted);
}

void VimEngine::beginGroup()
{
    if (s.hold)
        return;
    m.m_lastEdit.invalidate();
    m.pushUndo();
    s.hold = true;
}

void VimEngine::endGroup()
{
    if (!s.hold)
        return;
    s.hold = false;
    m.m_lastEdit.invalidate();
    // `i` then Escape changed nothing: no undo step to show for it.
    if (!m.m_undo.isEmpty() && m.m_undo.last().text == t)
        m.m_undo.removeLast();
}

void VimEngine::enterInsert(Pos at)
{
    beginGroup();
    s.mode = Mode::Insert;
    m.moveCursor(at, false);
    s.insertStart = m.m_cursor;
}

void VimEngine::leaveInsert()
{
    if (s.recordInsert && !s.replaying) {
        s.lastInserted = m.m_cursor >= s.insertStart && s.insertStart <= t.size()
                             ? t.mid(s.insertStart, m.m_cursor - s.insertStart)
                             : QString();
    }
    s.recordInsert = false;
    if (s.block.active) {
        replicateBlockInsert();
        s.block.active = false;
    }
    endGroup();
    s.mode = Mode::Normal;
    Pos p = m.m_cursor;
    if (p > ls(p))
        --p;
    m.moveCursor(p, false);
    m.m_goalX = -1;
    s.goalCol = -1;
}

void VimEngine::enterVisual(Mode mode)
{
    s.mode = mode;
    m.moveCursor(m.m_cursor, false); // anchor here
    syncVisual();
}

void VimEngine::saveVisualMarks()
{
    s.marks.insert(QLatin1Char('<'), qMin(m.m_anchor, m.m_cursor));
    s.marks.insert(QLatin1Char('>'), qMax(m.m_anchor, m.m_cursor));
}

void VimEngine::exitVisual()
{
    saveVisualMarks();
    s.mode = Mode::Normal;
    s.overrideRange = {-1, -1};
    s.overrideBlocks.clear();
    m.moveCursor(m.m_cursor, false);
    m.viewport()->update();
}

void VimEngine::syncVisual()
{
    const Pos low = qMin(m.m_anchor, m.m_cursor);
    const Pos high = qMax(m.m_anchor, m.m_cursor);
    s.overrideBlocks.clear();
    if (s.mode == Mode::VisualBlock) { // painted as strips; copied as the span between
        s.overrideBlocks = blockRanges(blockGeometry());
        s.overrideRange = {ls(low), le(high)};
    } else {
        s.overrideRange = s.mode == Mode::VisualLine ? Range{ls(low), le(high)} : Range{low, afterChar(t, high)};
    }
    s.overrideCursor = m.m_cursor;
    s.overrideAnchor = m.m_anchor;
    m.viewport()->update();
}

/// Normal mode keeps the caret on a character, never past the last one.
void VimEngine::normalize()
{
    if (s.mode == Mode::Insert)
        return;
    const Pos p = m.m_cursor;
    const Pos end = le(p);
    if (p >= end && end > ls(p))
        m.moveCursor(end - 1, visual());
    if (visual())
        syncVisual();
}

void VimEngine::abort()
{
    s.keys.clear();
    s.count = s.count2 = 0;
    s.op = s.pending = s.regName = QChar();
    s.keepGoal = false;
}

/// A command ran to completion. `change` makes it what `.` repeats.
void VimEngine::finish(bool change)
{
    if (change && !s.replaying) {
        s.lastChange = s.keys;
        s.lastInserted.clear();
        s.recordInsert = s.mode == Mode::Insert;
    }
    const bool keepGoal = s.keepGoal;
    abort();
    if (!keepGoal)
        s.goalCol = -1;
    normalize();
}

// --- keys ---------------------------------------------------------------------

bool VimEngine::key(QKeyEvent *event)
{
    const int k = event->key();
    switch (k) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_CapsLock:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
        return false;
    default:
        break;
    }
    // A click moved the caret out from under visual mode's selection: that
    // selection is gone, and so is the mode.
    if (visual() && (m.m_cursor != s.overrideCursor || m.m_anchor != s.overrideAnchor)) {
        s.mode = Mode::Normal;
        s.overrideRange = {-1, -1};
        s.overrideBlocks.clear();
        abort();
    }
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    const bool other = event->modifiers() & (Qt::AltModifier | Qt::MetaModifier);
    s.message.clear();

    if (s.mode == Mode::Insert) {
        if (k == Qt::Key_Escape || (ctrl && k == Qt::Key_BracketLeft)) {
            leaveInsert();
            return true;
        }
        return false; // typing, IME, auto-pairs, shortcuts: the editor's own
    }

    if (!s.cmdKind.isNull()) {
        const QString text = event->text();
        if (k == Qt::Key_Escape || (ctrl && k == Qt::Key_BracketLeft)) {
            s.cmdKind = QChar();
            s.cmdline.clear();
        } else if (k == Qt::Key_Return || k == Qt::Key_Enter) {
            runCommandLine();
        } else if (k == Qt::Key_Backspace) {
            if (s.cmdline.isEmpty())
                s.cmdKind = QChar();
            else
                s.cmdline.chop(1);
        } else if (!ctrl && !other && !text.isEmpty() && text.at(0).isPrint()) {
            s.cmdline += text;
        }
        return true;
    }

    if (ctrl) {
        if (k == Qt::Key_R && !other) {
            undoRedo(true);
            return true;
        }
        if (k == Qt::Key_BracketLeft) {
            feed(kEsc);
            return true;
        }
        if (k == Qt::Key_V && !other) { // block-visual, not paste, outside insert mode
            feed(kCtrlV);
            return true;
        }
        return false; // Ctrl+F, Ctrl+B, Ctrl+S ... keep their meaning
    }
    if (other)
        return false;

    switch (k) {
    case Qt::Key_Escape: {
        const bool busy = visual() || !s.op.isNull() || !s.pending.isNull() || !s.regName.isNull() || s.count || s.count2;
        if (!busy) {
            if (m.findVisible()) {
                m.hideFind();
                return true;
            }
            return false; // otherwise Escape is the window's, as without vim
        }
        feed(kEsc);
        return true;
    }
    case Qt::Key_Left:
    case Qt::Key_Backspace:
        feed(QLatin1Char('h'));
        return true;
    case Qt::Key_Right:
        feed(QLatin1Char('l'));
        return true;
    case Qt::Key_Up:
        feed(QLatin1Char('g'));
        feed(QLatin1Char('k'));
        return true;
    case Qt::Key_Down:
        feed(QLatin1Char('g'));
        feed(QLatin1Char('j'));
        return true;
    case Qt::Key_Home:
        feed(QLatin1Char('0'));
        return true;
    case Qt::Key_End:
        feed(QLatin1Char('$'));
        return true;
    case Qt::Key_Delete:
        feed(QLatin1Char('x'));
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        feed(QLatin1Char('j'));
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return true; // would type a tab
    default:
        break;
    }
    const QString text = event->text();
    if (text.isEmpty() || !text.at(0).isPrint())
        return false; // PageUp, function keys: the editor's own
    feed(text.at(0));
    return true;
}

void VimEngine::feed(QChar c)
{
    if (c == kEsc) {
        abort();
        if (visual())
            exitVisual();
        return;
    }
    if (!s.replaying)
        s.keys += c;

    if (!s.pending.isNull()) {
        const QChar pending = s.pending;
        s.pending = QChar();
        if (pending == QLatin1Char('r')) {
            replaceChars(c);
        } else if (pending == QLatin1Char('g')) {
            if (c == QLatin1Char('g'))
                runMotion(kGoTop);
            else if (c == QLatin1Char('j') || c == QLatin1Char('k'))
                displayLines(c == QLatin1Char('j') ? 1 : -1);
            else
                abort();
        } else if (pending == QLatin1Char('i') || pending == QLatin1Char('a')) {
            applyObject(pending == QLatin1Char('i'), c);
        } else if (pending == QLatin1Char('"')) { // "x: the register for the next command
            if (c.isLetter() || c.isDigit() || QStringView(u"\"+*_-").contains(c))
                s.regName = c == QLatin1Char('"') ? QChar() : c;
            else
                abort();
        } else if (pending == QLatin1Char('m')) {
            if (c.isLetter() && c.unicode() < 128)
                s.marks.insert(c, m.m_cursor);
            finish(false);
        } else if (pending == QLatin1Char('\'') || pending == QLatin1Char('`')) {
            s.markChar = c == QLatin1Char('`') ? QLatin1Char('\'') : c;
            runMotion(pending);
        } else if (pending == QLatin1Char('q')) {
            if (c.isLetter() && c.unicode() < 128) {
                s.recorded = c.isUpper() ? s.macros.value(c.toLower()) : QList<MarkdownEditor::VimState::MacroKey>();
                s.recording = c;
            }
            abort();
        } else if (pending == QLatin1Char('@')) {
            const QChar name = c == QLatin1Char('@') ? s.lastMacro : c.toLower();
            const int times = qMin(total(), 10000);
            abort();
            if (s.macros.contains(name)) {
                s.lastMacro = name;
                runMacro(name, times);
            }
        } else { // f F t T
            s.findKind = pending;
            s.findChar = c;
            runMotion(pending);
        }
        return;
    }

    if (c.isDigit() && (c != QLatin1Char('0') || (s.op.isNull() ? s.count : s.count2) > 0)) {
        int &n = s.op.isNull() ? s.count : s.count2;
        n = qMin(n * 10 + c.digitValue(), 100000);
        return;
    }

    const bool vis = visual();
    if ((c == QLatin1Char('i') || c == QLatin1Char('a')) && (!s.op.isNull() || vis)) {
        s.pending = c; // a text object
        return;
    }
    switch (c.unicode()) {
    case 'h': case 'j': case 'k': case 'l': case 'w': case 'W': case 'b': case 'B':
    case 'e': case 'E': case '0': case '^': case '$': case 'G': case '{': case '}':
    case ';': case ',': case '%': case '*': case '#':
        runMotion(c);
        return;
    case 'n': case 'N':
        runMotion(c);
        return;
    case 'g': case 'f': case 'F': case 't': case 'T': case '\'': case '`':
        s.pending = c;
        return;
    case 'd': case 'c': case 'y': case '>': case '<':
        if (vis)
            visualOperator(c);
        else if (s.op.isNull())
            s.op = c;
        else if (s.op == c)
            lineOperator(c);
        else
            abort();
        return;
    default:
        break;
    }

    // Everything else needs a bare command: an operator waiting for a motion
    // makes it meaningless, as in vim.
    if (!s.op.isNull()) {
        abort();
        return;
    }
    switch (c.unicode()) {
    case 'x':
        if (vis) { visualOperator(QLatin1Char('d')); return; }
        s.op = QLatin1Char('d');
        runMotion(QLatin1Char('l'));
        return;
    case 'X':
        s.op = QLatin1Char('d');
        runMotion(QLatin1Char('h'));
        return;
    case 'D':
        s.op = QLatin1Char('d');
        runMotion(QLatin1Char('$'));
        return;
    case 'C':
        s.op = QLatin1Char('c');
        runMotion(QLatin1Char('$'));
        return;
    case 's':
        if (vis) { visualOperator(QLatin1Char('c')); return; }
        s.op = QLatin1Char('c');
        runMotion(QLatin1Char('l'));
        return;
    case 'S':
        lineOperator(QLatin1Char('c'));
        return;
    case 'Y':
        lineOperator(QLatin1Char('y'));
        return;
    case '"': case 'm': case '@':
        s.pending = c;
        return;
    case 'q':
        if (!s.recording.isNull()) { // the q that ends it was recorded with the rest
            if (!s.recorded.isEmpty())
                s.recorded.removeLast();
            s.macros.insert(s.recording.toLower(), s.recorded);
            s.recording = QChar();
            abort();
        } else if (vis) {
            abort();
        } else {
            s.pending = c;
        }
        return;
    case 'p':
        paste(true);
        return;
    case 'P':
        paste(false);
        return;
    case 'u':
        undoRedo(false);
        return;
    case 'J':
        join();
        return;
    case 'r':
        if (vis)
            abort();
        else
            s.pending = c;
        return;
    case '~':
        toggleCase();
        return;
    case '.':
        s.keys.clear();
        repeatLastChange();
        return;
    case 'i':
        enterInsert(m.m_cursor);
        finish(true);
        return;
    case 'a':
        enterInsert(m.m_cursor < le(m.m_cursor) ? m.m_cursor + 1 : m.m_cursor);
        finish(true);
        return;
    case 'I':
    case 'A':
        if (s.mode == Mode::VisualBlock) {
            blockInsert(c == QLatin1Char('A'));
        } else {
            enterInsert(c == QLatin1Char('I') ? firstNonBlank(t, m.m_cursor) : le(m.m_cursor));
            finish(true);
        }
        return;
    case 'o':
        if (vis) { // the other end of the selection
            const Pos cursor = m.m_cursor;
            m.m_cursor = m.m_anchor;
            m.m_anchor = cursor;
            m.moveCursor(m.m_cursor, true);
            finish(false);
            return;
        }
        openLine(true);
        return;
    case 'O':
        openLine(false);
        return;
    case 'v':
    case 'V':
    case 0x16: { // Ctrl+v
        const Mode wanted = c == QLatin1Char('v') ? Mode::Visual
                            : c == QLatin1Char('V') ? Mode::VisualLine
                                                    : Mode::VisualBlock;
        if (s.mode == wanted) {
            abort();
            exitVisual();
        } else if (vis) {
            s.mode = wanted;
            abort();
            syncVisual();
        } else {
            abort();
            enterVisual(wanted);
        }
        return;
    }
    case ':':
    case '/':
    case '?':
        if (vis && c == QLatin1Char(':')) { // :'<,'> works on the selection
            saveVisualMarks();
            exitVisual();
            startCommandLine(c);
            s.cmdline = QStringLiteral("'<,'>");
            return;
        }
        startCommandLine(c);
        return;
    default:
        abort(); // unknown: swallowed, so it types nothing
        return;
    }
}

// --- motions ---------------------------------------------------------------------

bool VimEngine::motion(QChar key, int n, bool hasCount, Move &out)
{
    const Pos cur = m.m_cursor;
    const bool op = !s.op.isNull();
    out = {cur, false, false};

    switch (key.unicode()) {
    case 'h':
        out.pos = qMax(ls(cur), cur - n);
        return true;
    case 'l': {
        const Pos end = le(cur);
        const Pos limit = op || end == ls(cur) ? end : end - 1;
        out.pos = qMin(cur + n, limit);
        return true;
    }
    case 'j':
    case 'k': {
        const bool down = key == QLatin1Char('j');
        Pos line = ls(cur);
        for (int i = 0; i < n; ++i) {
            if (down) {
                const Pos next = nextLine(t, line);
                if (next < 0) {
                    if (i == 0)
                        return false;
                    break;
                }
                line = next;
            } else {
                if (line == 0) {
                    if (i == 0)
                        return false;
                    break;
                }
                line = ls(line - 1);
            }
        }
        if (s.goalCol < 0)
            s.goalCol = cur - ls(cur);
        const Pos end = le(line);
        out.pos = qMin(line + s.goalCol, end > line ? end - 1 : line);
        out.linewise = true;
        s.keepGoal = true;
        return true;
    }
    case 'w':
    case 'W':
    case 'e':
    case 'E': {
        const bool big = key == QLatin1Char('W') || key == QLatin1Char('E');
        bool toEnd = key == QLatin1Char('e') || key == QLatin1Char('E');
        // cw changes the word, not the space after it: like ce, except that
        // the first step ends the word the caret is in rather than the next.
        bool inWord = false;
        if (!toEnd && op && s.op == QLatin1Char('c') && cur < t.size() && charClass(t.at(cur), big) != 0)
            toEnd = inWord = true;
        Pos p = cur;
        Pos last = cur;
        for (int i = 0; i < n; ++i) {
            last = p;
            if (inWord && i == 0) {
                const int cls = charClass(t.at(p), big);
                while (p + 1 < t.size() && charClass(t.at(p + 1), big) == cls)
                    ++p;
            } else {
                p = toEnd ? wordEnd(t, p, big) : wordForward(t, p, big);
            }
        }
        // dw on the last word of a line stops at the line's end rather than
        // eating the break and the next line's first word.
        if (!toEnd && op && p > last && ls(p) != ls(last))
            p = le(last);
        out.pos = p;
        out.inclusive = toEnd;
        return true;
    }
    case 'b':
    case 'B': {
        Pos p = cur;
        for (int i = 0; i < n; ++i)
            p = wordBack(t, p, key == QLatin1Char('B'));
        out.pos = p;
        return true;
    }
    case '0':
        out.pos = ls(cur);
        return true;
    case '^':
        out.pos = firstNonBlank(t, cur);
        return true;
    case '$': {
        Pos line = ls(cur);
        for (int i = 1; i < n; ++i) {
            const Pos next = nextLine(t, line);
            if (next < 0)
                break;
            line = next;
        }
        const Pos end = le(line);
        out.pos = end > line ? end - 1 : end;
        out.inclusive = true;
        s.goalCol = std::numeric_limits<int>::max(); // j and k keep to the line ends
        s.keepGoal = true;
        return true;
    }
    case 'G':
    case 1: { // G, gg
        Pos last = t.size();
        if (last > 0 && t.at(last - 1) == QLatin1Char('\n'))
            --last;
        const Pos line = key == kGoTop ? (hasCount ? lineStartByNumber(t, n - 1) : 0)
                                       : (hasCount ? lineStartByNumber(t, n - 1) : ls(last));
        out.pos = firstNonBlank(t, line);
        out.linewise = true;
        return true;
    }
    case '}': {
        Pos p = cur;
        for (int i = 0; i < n; ++i)
            p = paragraphForward(t, p);
        out.pos = p;
        return true;
    }
    case '{': {
        Pos p = cur;
        for (int i = 0; i < n; ++i)
            p = paragraphBack(t, p);
        out.pos = p;
        return true;
    }
    case 'f':
    case 'F':
    case 't':
    case 'T':
    case ';':
    case ',': {
        QChar kind = key;
        if (key == QLatin1Char(';') || key == QLatin1Char(',')) {
            if (s.findKind.isNull())
                return false;
            kind = s.findKind;
            if (key == QLatin1Char(','))
                kind = kind.isUpper() ? kind.toLower() : kind.toUpper();
        }
        const bool forward = kind == QLatin1Char('f') || kind == QLatin1Char('t');
        const bool till = kind == QLatin1Char('t') || kind == QLatin1Char('T');
        const Pos start = ls(cur);
        const Pos end = le(cur);
        Pos p = cur;
        int found = 0;
        // ; after a t must not stay put against the character it stopped before
        const Pos skip = till && (key == QLatin1Char(';') || key == QLatin1Char(',')) ? 1 : 0;
        for (p = forward ? cur + 1 + skip : cur - 1 - skip; p >= start && p < end; p += forward ? 1 : -1) {
            if (t.at(p) == s.findChar && ++found == n)
                break;
        }
        if (found != n)
            return false;
        out.pos = till ? (forward ? p - 1 : p + 1) : p;
        out.inclusive = forward;
        return true;
    }
    case 'n':
    case 'N':
    case '*':
    case '#': {
        bool forward;
        Pos from = cur;
        if (key == QLatin1Char('*') || key == QLatin1Char('#')) {
            Pos a, b;
            if (!wordUnder(cur, a, b)) {
                s.message = QStringLiteral("E348: No string under cursor");
                return false;
            }
            s.lastSearch = QStringLiteral("\\b") + QRegularExpression::escape(t.mid(a, b - a))
                           + QStringLiteral("\\b");
            s.searchForward = forward = key == QLatin1Char('*');
            from = a; // # from the middle of a word must not find that word again
        } else {
            if (s.lastSearch.isEmpty())
                return false;
            forward = (key == QLatin1Char('n')) == s.searchForward;
        }
        return searchMove(forward, n, from, out.pos);
    }
    case '%': {
        // the first bracket at or after the caret on its line, and its partner
        static constexpr QStringView pairs = u"()[]{}";
        const Pos end = le(cur);
        Pos i = cur;
        while (i < end && pairs.indexOf(t.at(i)) < 0)
            ++i;
        if (i >= end)
            return false;
        const qsizetype k = pairs.indexOf(t.at(i));
        const bool opening = k % 2 == 0;
        const QChar open = pairs.at(opening ? k : k - 1);
        const QChar close = pairs.at(opening ? k + 1 : k);
        const Pos other = opening ? matchClose(t, i, open, close) : findOpen(t, i, open, close, 1);
        if (other < 0)
            return false;
        out.pos = other;
        out.inclusive = true;
        return true;
    }
    case '\'':
    case '`': {
        const auto it = s.marks.constFind(s.markChar);
        if (it == s.marks.constEnd()) {
            s.message = QStringLiteral("E20: Mark not set");
            return false;
        }
        const Pos pos = qBound<Pos>(0, *it, t.size());
        out.pos = key == QLatin1Char('\'') ? firstNonBlank(t, pos) : pos;
        out.linewise = key == QLatin1Char('\'');
        return true;
    }
    default:
        return false;
    }
}

void VimEngine::runMotion(QChar key)
{
    Move mv;
    if (!motion(key, total(), hasCount(), mv)) {
        abort();
        return;
    }
    if (!s.op.isNull()) {
        applyMove(mv);
        return;
    }
    if (QStringView(u"G\x01%nN*#{}'`").contains(key))
        setJumpMark();
    go(mv.pos);
    finish(false);
}

/// Repeat a search `n` times from `from`; the caret is walked along because
/// searchFrom reads it.
bool VimEngine::searchMove(bool forward, int n, Pos from, Pos &out)
{
    const Pos cur = m.m_cursor;
    Pos p = from;
    for (int i = 0; i < n; ++i) {
        m.m_cursor = p;
        if (!searchFrom(forward, p)) {
            m.m_cursor = cur;
            s.message = QStringLiteral("E486: Pattern not found: ") + s.lastSearch;
            return false;
        }
    }
    m.m_cursor = cur;
    out = p;
    return true;
}

/// The word under the caret, or the next one on its line.
bool VimEngine::wordUnder(Pos at, Pos &from, Pos &to) const
{
    const Pos end = le(at);
    const auto word = [](QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); };
    while (at < end && !word(t.at(at)))
        ++at;
    if (at >= end)
        return false;
    from = at;
    to = at + 1;
    while (from > ls(at) && word(t.at(from - 1)))
        --from;
    while (to < end && word(t.at(to)))
        ++to;
    return true;
}

/// gj and gk: by drawn line, which a wrapped paragraph needs and the editor
/// already knows how to do.
void VimEngine::displayLines(int direction)
{
    if (!s.op.isNull()) {
        abort();
        return;
    }
    for (int i = 0; i < total(); ++i)
        m.moveVertically(direction, visual());
    finish(false);
}

void VimEngine::applyMove(const Move &mv)
{
    const Pos cur = m.m_cursor;
    Pos a = qMin(cur, mv.pos);
    Pos b = qMax(cur, mv.pos);
    bool linewise = mv.linewise;
    Range r;
    if (linewise) {
        r = {ls(a), le(b)};
    } else if (mv.inclusive) {
        r = {a, afterChar(t, b)};
    } else {
        // An exclusive motion that ends in column 0 of a later line stops at
        // the end of the line before it (and takes whole lines when it began
        // at the start of one): d} deletes a paragraph, not its blank line.
        if (b > a && b == ls(b) && ls(b) != ls(a)) {
            --b;
            if (a <= firstNonBlank(t, a)) {
                linewise = true;
                r = {ls(a), le(b)};
            }
        }
        if (!linewise)
            r = {a, b};
    }
    doOperator(s.op, r, linewise, mv.pos);
}

void VimEngine::lineOperator(QChar op)
{
    Pos last = ls(m.m_cursor);
    for (int i = 1; i < total(); ++i) {
        const Pos next = nextLine(t, last);
        if (next < 0)
            break;
        last = next;
    }
    s.op = op;
    doOperator(op, {ls(m.m_cursor), le(last)}, true, m.m_cursor);
}

/// `r` is the lines (or characters) the operator covers, `hint` where a
/// yank leaves the caret when it moved backwards.
void VimEngine::doOperator(QChar op, Range r, bool linewise, Pos hint, int times)
{
    const bool yank = op == QLatin1Char('y');
    const bool change = op == QLatin1Char('c');
    s.op = QChar();
    if (op == QLatin1Char('>') || op == QLatin1Char('<')) {
        shiftLines(op, r, linewise, times);
        return;
    }
    storeReg(linewise ? t.mid(r.start, r.end - r.start) + QLatin1Char('\n') : t.mid(r.start, r.end - r.start),
             linewise ? 1 : 0, yank);

    if (yank) {
        go(linewise ? qMin(m.m_cursor, hint) : r.start);
        finish(false);
        return;
    }

    if (!linewise) {
        if (change)
            beginGroup();
        if (r.length() > 0)
            edit(r.start, r.length(), QString());
        if (change) {
            s.mode = Mode::Insert;
            m.moveCursor(r.start, false);
            s.insertStart = r.start;
        } else {
            go(r.start);
        }
        finish(true);
        return;
    }

    if (change) { // cc keeps the (now empty) line
        beginGroup();
        if (r.length() > 0)
            edit(r.start, r.length(), QString());
        s.mode = Mode::Insert;
        m.moveCursor(r.start, false);
        s.insertStart = r.start;
        finish(true);
        return;
    }

    // dd: the lines and one line break, the one before them for the last line.
    const Pos newline = t.indexOf(QLatin1Char('\n'), r.end);
    const Pos to = newline < 0 ? t.size() : newline + 1;
    Pos from = r.start;
    if (newline < 0 && from > 0)
        --from;
    if (to > from)
        edit(from, to - from, QString());
    Pos p = qMin(from, t.size());
    if (p == t.size() && p > 0 && t.at(p - 1) == QLatin1Char('\n'))
        --p; // deleted the last line: land on the one above
    go(firstNonBlank(t, p));
    finish(true);
}

void VimEngine::visualOperator(QChar op)
{
    if (s.mode == Mode::VisualBlock) {
        blockOperator(op, total());
        return;
    }
    const Pos low = qMin(m.m_anchor, m.m_cursor);
    const Pos high = qMax(m.m_anchor, m.m_cursor);
    const bool linewise = s.mode == Mode::VisualLine;
    const Range r = linewise ? Range{ls(low), le(high)} : Range{low, afterChar(t, high)};
    const int times = total();
    saveVisualMarks();
    s.mode = Mode::Normal;
    s.overrideRange = {-1, -1};
    s.overrideBlocks.clear();
    m.moveCursor(m.m_cursor, false);
    doOperator(op, r, linewise, low, times);
}

/// > and <: the lines `r` covers move by the editor's tab width, `times` over.
void VimEngine::shiftLines(QChar op, Range r, bool linewise, int times)
{
    const Pos first = ls(r.start);
    const Pos last = linewise ? ls(r.end) : ls(qMax(r.start, r.end - 1));
    const QString unit = QString(m.m_tabWidth, QLatin1Char(' '));
    QString out;
    Pos end = first;
    for (Pos p = first;;) {
        const Pos newline = t.indexOf(QLatin1Char('\n'), p);
        end = newline < 0 ? t.size() : newline; // a CR stays with its line
        QString line = t.mid(p, end - p);
        if (op == QLatin1Char('>')) {
            if (!line.isEmpty() && line != QLatin1String("\r"))
                line.prepend(unit.repeated(times));
        } else {
            for (int i = 0; i < times; ++i) {
                if (line.startsWith(QLatin1Char('\t'))) {
                    line.remove(0, 1);
                } else {
                    int spaces = 0;
                    while (spaces < m.m_tabWidth && spaces < line.size() && line.at(spaces) == QLatin1Char(' '))
                        ++spaces;
                    line.remove(0, spaces);
                }
            }
        }
        out += line;
        if (p == last || newline < 0)
            break;
        out += QLatin1Char('\n');
        p = newline + 1;
    }
    if (out != t.mid(first, end - first))
        edit(first, end - first, out);
    go(firstNonBlank(t, first));
    finish(true);
}

// --- registers ------------------------------------------------------------------------

/// Where a yank, delete or change goes: "x if one was chosen, and always the
/// unnamed register. kind: 0 characters, 1 lines, 2 a column block.
void VimEngine::storeReg(const QString &text, int kind, bool yank)
{
    const QChar name = s.regName;
    if (name == QLatin1Char('_'))
        return;
    Register r{text, kind};
    if (name.isLetter()) {
        const QChar low = name.toLower();
        if (name.isUpper() && s.regs.contains(low)) { // "A appends
            const Register &old = s.regs[low];
            r.text = old.text + (old.kind != 1 && kind == 1 ? QStringLiteral("\n") : QString())
                     + text + (old.kind == 1 && kind != 1 ? QStringLiteral("\n") : QString());
            r.kind = old.kind == 1 || kind == 1 ? 1 : old.kind;
        }
        s.regs.insert(low, r);
    } else if (name == QLatin1Char('+') || name == QLatin1Char('*')) {
        QGuiApplication::clipboard()->setText(text);
    }
    s.reg = r.text;
    s.regLinewise = r.kind == 1;
    s.regBlock = r.kind == 2;
    if (yank && name.isNull())
        s.regs.insert(QLatin1Char('0'), r);
}

bool VimEngine::loadReg(QString &text, int &kind) const
{
    const QChar name = s.regName;
    if (name == QLatin1Char('+') || name == QLatin1Char('*')) {
        text = QGuiApplication::clipboard()->text();
        kind = text.endsWith(QLatin1Char('\n')) ? 1 : 0;
    } else if (name.isLetter() || name == QLatin1Char('0')) {
        const auto it = s.regs.constFind(name.toLower());
        if (it == s.regs.constEnd())
            return false;
        text = it->text;
        kind = it->kind;
    } else if (name == QLatin1Char('_')) {
        return false;
    } else {
        text = s.reg;
        kind = s.regBlock ? 2 : (s.regLinewise ? 1 : 0);
    }
    return !text.isEmpty();
}

// --- text objects ---------------------------------------------------------------------

/// i{x} / a{x} after an operator or in visual mode.
void VimEngine::applyObject(bool inner, QChar object)
{
    const Pos cur = m.m_cursor;
    const int n = total();
    Range r;
    bool linewise = false;
    bool found = false;
    switch (object.unicode()) {
    case 'w': case 'W':
        r = wordObject(t, cur, object == QLatin1Char('W'), inner, n);
        found = r.length() > 0;
        break;
    case 's':
        found = sentenceObject(t, cur, inner, n, r);
        break;
    case 'p':
        found = linewise = paragraphObject(t, cur, inner, n, r);
        break;
    case '"': case '\'': case '`':
        found = quoteObject(t, cur, object, inner, r);
        break;
    default: {
        static constexpr QStringView pairs = u"()[]{}<>";
        QChar key = object == QLatin1Char('b') ? QLatin1Char('(') : object == QLatin1Char('B') ? QLatin1Char('{') : object;
        const qsizetype k = pairs.indexOf(key);
        if (k >= 0) {
            const qsizetype open = k - k % 2;
            found = bracketObject(t, cur, pairs.at(open), pairs.at(open + 1), inner, n, r, linewise);
        }
        break;
    }
    }
    if (!found) {
        abort();
        return;
    }
    if (!visual()) {
        doOperator(s.op, r, linewise, r.start);
        return;
    }
    if (linewise && s.mode == Mode::Visual)
        s.mode = Mode::VisualLine;
    if (r.length() <= 0) {
        abort();
        return;
    }
    m.moveCursor(r.start, false);
    m.moveCursor(linewise ? r.end : r.end - 1, true);
    finish(false);
}

// --- block-visual -------------------------------------------------------------------------

VimEngine::Block VimEngine::blockGeometry() const
{
    const Pos a = ls(m.m_anchor);
    const Pos b = ls(m.m_cursor);
    Block g;
    g.first = qMin(a, b);
    for (Pos p = g.first; p < qMax(a, b);) {
        const Pos next = nextLineStart(t, p);
        if (next < 0)
            break;
        p = next;
        ++g.lines;
    }
    const qsizetype ca = m.m_anchor - a;
    const qsizetype cb = m.m_cursor - b;
    g.c1 = qMin(ca, cb);
    g.c2 = qMax(ca, cb);
    g.toEnd = s.goalCol == std::numeric_limits<int>::max();
    return g;
}

/// One range per line: the block's columns, cut short where a line ends.
QList<Range> VimEngine::blockRanges(const Block &g) const
{
    QList<Range> out;
    Pos p = g.first;
    for (int i = 0; i < g.lines; ++i) {
        const Pos end = le(p);
        const Pos from = qMin(p + g.c1, end);
        out.append({from, g.toEnd ? end : qMax(from, qMin(p + g.c2 + 1, end))});
        p = nextLineStart(t, p);
        if (p < 0)
            break;
    }
    return out;
}

void VimEngine::blockOperator(QChar op, int times)
{
    const Block g = blockGeometry();
    const QList<Range> ranges = blockRanges(g);
    saveVisualMarks();
    s.mode = Mode::Normal;
    s.overrideRange = {-1, -1};
    s.overrideBlocks.clear();
    m.moveCursor(m.m_cursor, false);
    if (op == QLatin1Char('>') || op == QLatin1Char('<')) {
        doOperator(op, {g.first, le(ls(ranges.constLast().start))}, true, g.first, times);
        return;
    }
    QStringList pieces;
    for (const Range &r : ranges)
        pieces << t.mid(r.start, r.length());
    storeReg(pieces.join(QLatin1Char('\n')), 2, op == QLatin1Char('y'));
    if (op == QLatin1Char('y')) {
        go(ranges.constFirst().start);
        finish(false);
        return;
    }
    beginGroup(); // the lines are one undo step
    for (qsizetype i = ranges.size() - 1; i >= 0; --i) // from the bottom, so offsets hold
        if (ranges.at(i).length() > 0)
            edit(ranges.at(i).start, ranges.at(i).length(), QString());
    if (op == QLatin1Char('c')) {
        s.mode = Mode::Insert;
        m.moveCursor(ranges.constFirst().start, false);
        s.insertStart = m.m_cursor;
        s.block = {true, g.first, int(ranges.size()), g.c1, false, g.toEnd};
    } else {
        endGroup();
        go(ranges.constFirst().start);
    }
    finish(true);
}

/// I and A in block-visual: insert on the first line; the text typed is copied
/// to the other lines when the insert ends.
void VimEngine::blockInsert(bool append)
{
    const Block g = blockGeometry();
    saveVisualMarks();
    s.mode = Mode::Normal;
    s.overrideRange = {-1, -1};
    s.overrideBlocks.clear();
    m.moveCursor(m.m_cursor, false);
    const qsizetype column = append ? g.c2 + 1 : g.c1;
    Pos at = g.first + column;
    const Pos end = le(g.first);
    if (append && g.toEnd) {
        at = end;
    } else if (at > end) {
        if (append) { // a short first line is padded out to the block
            beginGroup();
            edit(end, 0, QString(at - end, QLatin1Char(' ')));
        } else {
            at = end;
        }
    }
    enterInsert(at);
    s.block = {true, g.first, g.lines, column, append, g.toEnd};
    finish(true);
}

void VimEngine::replicateBlockInsert()
{
    const QString typed = m.m_cursor >= s.insertStart && s.insertStart <= t.size()
                              ? t.mid(s.insertStart, m.m_cursor - s.insertStart)
                              : QString();
    if (typed.isEmpty() || typed.contains(QLatin1Char('\n')))
        return;
    const auto &b = s.block;
    QList<std::pair<Pos, QString>> inserts;
    Pos p = b.firstLine;
    for (int i = 0; i < b.lines; ++i) {
        if (i > 0) {
            const Pos end = le(p);
            Pos at = b.toEnd ? end : p + b.column;
            QString pad;
            if (at > end) {
                if (!b.append)
                    at = -1; // I skips lines that stop short of the block
                else {
                    pad = QString(at - end, QLatin1Char(' '));
                    at = end;
                }
            }
            if (at >= 0)
                inserts.append({at, pad + typed});
        }
        p = nextLineStart(t, p);
        if (p < 0)
            break;
    }
    for (qsizetype i = inserts.size() - 1; i >= 0; --i)
        m.applyEdit(inserts.at(i).first, 0, inserts.at(i).second);
    m.moveCursor(qMin(s.insertStart + 1, le(s.insertStart)), false); // the block's corner, once Escape steps back
}

/// p of a column block: each piece goes in the same column of successive lines,
/// lines added at the end when there are not enough.
void VimEngine::pasteBlock(const QString &text, bool after)
{
    const QStringList pieces = text.split(QLatin1Char('\n'));
    const Pos cur = m.m_cursor;
    const Pos top = ls(cur);
    const qsizetype column = cur - top + (after && cur < le(cur) ? 1 : 0);
    qsizetype width = 0;
    for (const QString &piece : pieces)
        width = qMax(width, piece.size());
    QString out;
    Pos p = top;
    Pos lastLine = top;
    bool exists = true;
    for (qsizetype i = 0; i < pieces.size(); ++i) {
        QString line = exists ? t.mid(p, le(p) - p) : QString();
        const QString rest = line.mid(column);
        if (line.size() < column)
            line += QString(column - line.size(), QLatin1Char(' '));
        out += line.left(column) + pieces.at(i);
        if (!rest.isEmpty())
            out += QString(width - pieces.at(i).size(), QLatin1Char(' ')) + rest;
        if (i + 1 < pieces.size())
            out += QLatin1Char('\n');
        if (exists) {
            lastLine = p;
            const Pos next = nextLine(t, p);
            if (next < 0)
                exists = false;
            else
                p = next;
        }
    }
    edit(top, le(lastLine) - top, out);
    go(top + column);
    finish(true);
}

// --- macros -----------------------------------------------------------------------------

/// @x: the recorded keys go back through the editor's own key handling, so
/// whatever they did - insert-mode typing included - they do again.
void VimEngine::runMacro(QChar name, int times)
{
    if (s.macroDepth >= 8) // a macro that runs itself
        return;
    const QList<MarkdownEditor::VimState::MacroKey> keys = s.macros.value(name);
    ++s.macroDepth;
    for (int i = 0; i < times; ++i) {
        for (const auto &k : keys) {
            QKeyEvent event(QEvent::KeyPress, k.key, Qt::KeyboardModifiers(k.modifiers), k.text);
            m.keyPressEvent(&event);
        }
    }
    --s.macroDepth;
}

// --- single-key commands -----------------------------------------------------------

void VimEngine::paste(bool after)
{
    QString reg;
    int kind = 0;
    if (!loadReg(reg, kind)) {
        abort();
        return;
    }
    if (kind == 2) {
        pasteBlock(reg, after);
        return;
    }
    QString text;
    for (int i = 0; i < total(); ++i)
        text += reg;
    const Pos cur = m.m_cursor;
    if (kind == 1) {
        if (!after) {
            const Pos at = ls(cur);
            edit(at, 0, text);
            go(firstNonBlank(t, at));
        } else if (const Pos next = nextLineStart(t, cur); next >= 0) {
            edit(next, 0, text);
            go(firstNonBlank(t, next));
        } else { // the last line has no break after it to open a line with
            const Pos at = t.size();
            edit(at, 0, QLatin1Char('\n') + text.chopped(1));
            go(firstNonBlank(t, at + 1));
        }
    } else {
        const Pos at = after && cur < le(cur) ? cur + 1 : cur;
        edit(at, 0, text);
        go(at + text.size() - 1);
    }
    finish(true);
}

void VimEngine::replaceChars(QChar c)
{
    const Pos cur = m.m_cursor;
    const int n = total();
    if (!c.isPrint() || cur + n > le(cur)) {
        abort();
        return;
    }
    edit(cur, n, QString(n, c));
    go(cur + n - 1);
    finish(true);
}

void VimEngine::toggleCase()
{
    const Pos cur = m.m_cursor;
    const qsizetype n = qMin<qsizetype>(total(), le(cur) - cur);
    if (n <= 0) {
        abort();
        return;
    }
    QString text = t.mid(cur, n);
    for (QChar &c : text)
        c = c.isUpper() ? c.toLower() : c.toUpper();
    edit(cur, n, text);
    go(cur + n);
    finish(true);
}

void VimEngine::join()
{
    const Pos start = ls(m.m_cursor);
    Pos last = start;
    const int lines = qMax(2, total());
    int joined = 0;
    for (int i = 1; i < lines; ++i) {
        const Pos next = nextLine(t, last);
        if (next < 0)
            break;
        last = next;
        ++joined;
    }
    if (joined == 0) {
        abort();
        return;
    }
    const QStringList parts = t.mid(start, le(last) - start).split(QLatin1Char('\n'));
    QString result = parts.constFirst();
    Pos caret = qMax<Pos>(0, result.size() - 1);
    for (qsizetype i = 1; i < parts.size(); ++i) {
        qsizetype from = 0;
        while (from < parts.at(i).size() && parts.at(i).at(from).isSpace())
            ++from;
        const QString next = parts.at(i).mid(from);
        if (!next.isEmpty() && !result.isEmpty() && !result.endsWith(QLatin1Char(' '))
            && !result.endsWith(QLatin1Char('\t')) && !next.startsWith(QLatin1Char(')')))
            result += QLatin1Char(' ');
        caret = qMax<Pos>(0, result.size() - 1); // on the space, or the line's last character
        result += next;
    }
    edit(start, le(last) - start, result);
    go(start + caret);
    finish(true);
}

void VimEngine::openLine(bool below)
{
    const Pos cur = m.m_cursor;
    beginGroup();
    if (below) {
        const Pos end = le(cur);
        edit(end, 0, QStringLiteral("\n"));
        s.mode = Mode::Insert;
        m.moveCursor(end + 1, false);
    } else {
        const Pos begin = ls(cur);
        edit(begin, 0, QStringLiteral("\n"));
        s.mode = Mode::Insert;
        m.moveCursor(begin, false);
    }
    s.insertStart = m.m_cursor;
    finish(true);
}

void VimEngine::undoRedo(bool redo)
{
    for (int i = 0; i < total(); ++i) {
        if (redo ? m.m_redo.isEmpty() : m.m_undo.isEmpty())
            break;
        redo ? m.redo() : m.undo();
    }
    m.moveCursor(m.m_cursor, false);
    finish(false);
}

void VimEngine::repeatLastChange()
{
    if (s.lastChange.isEmpty() || s.replaying) {
        abort();
        return;
    }
    abort();
    s.replaying = true;
    const QString keys = s.lastChange;
    for (const QChar c : keys)
        feed(c);
    if (s.mode == Mode::Insert) {
        if (!s.lastInserted.isEmpty())
            m.applyEdit(m.m_cursor, 0, s.lastInserted);
        leaveInsert();
    }
    s.replaying = false;
    finish(false);
}

// --- the : and / line ---------------------------------------------------------------

void VimEngine::startCommandLine(QChar kind)
{
    abort();
    s.cmdKind = kind;
    s.cmdline.clear();
}

void VimEngine::runCommandLine()
{
    const QChar kind = s.cmdKind;
    const QString text = s.cmdline;
    s.cmdKind = QChar();
    s.cmdline.clear();
    if (kind == QLatin1Char(':'))
        exCommand(text.trimmed());
    else
        search(text, kind == QLatin1Char('/'));
}

Pos VimEngine::lineAdd(Pos line, int delta) const
{
    for (; delta > 0; --delta) {
        const Pos next = nextLine(t, line);
        if (next < 0)
            break;
        line = next;
    }
    for (; delta < 0 && line > 0; ++delta)
        line = ls(line - 1);
    return line;
}

/// One line address: a number, `.`, `$`, `'x`, with +N / -N after it. `line`
/// comes back as that line's start.
bool VimEngine::address(const QString &x, qsizetype &i, Pos &line) const
{
    Pos base = -1;
    if (i < x.size()) {
        const QChar c = x.at(i);
        if (c == QLatin1Char('.')) {
            base = ls(m.m_cursor);
            ++i;
        } else if (c == QLatin1Char('$')) {
            base = lastLineStart(t);
            ++i;
        } else if (c.isDigit()) {
            qsizetype number = 0;
            for (; i < x.size() && x.at(i).isDigit(); ++i)
                number = qMin<qsizetype>(number * 10 + x.at(i).digitValue(), 100000000);
            base = lineStartByNumber(t, qMax<qsizetype>(0, number - 1));
        } else if (c == QLatin1Char('\'') && i + 1 < x.size()) {
            const auto it = s.marks.constFind(x.at(i + 1));
            if (it == s.marks.constEnd())
                return false;
            base = ls(*it);
            i += 2;
        }
    }
    while (i < x.size() && (x.at(i) == QLatin1Char('+') || x.at(i) == QLatin1Char('-'))) {
        const int sign = x.at(i++) == QLatin1Char('+') ? 1 : -1;
        int amount = 0;
        bool digits = false;
        for (; i < x.size() && x.at(i).isDigit(); ++i, digits = true)
            amount = qMin(amount * 10 + x.at(i).digitValue(), 1000000);
        base = lineAdd(base < 0 ? ls(m.m_cursor) : base, sign * (digits ? amount : 1));
    }
    if (base < 0)
        return false;
    line = base;
    return true;
}

void VimEngine::exCommand(const QString &whole)
{
    // [range]command: % is every line, 3,5 and .,+2 and '<,'> are spans.
    qsizetype i = 0;
    Pos first = -1;
    Pos last = -1;
    bool ranged = false;
    if (whole.startsWith(QLatin1Char('%'))) {
        first = 0;
        last = lastLineStart(t);
        ranged = true;
        i = 1;
    } else if (address(whole, i, first)) {
        ranged = true;
        last = first;
        if (i < whole.size() && (whole.at(i) == QLatin1Char(',') || whole.at(i) == QLatin1Char(';'))) {
            ++i;
            if (!address(whole, i, last)) {
                s.message = QStringLiteral("E14: Invalid address");
                return;
            }
        }
    } else if (whole.startsWith(QLatin1Char('\''))) { // a mark that is not set
        s.message = QStringLiteral("E20: Mark not set");
        return;
    }
    if (first > last)
        std::swap(first, last);
    const QString text = whole.mid(i).trimmed();
    if (ranged && text.isEmpty()) {
        setJumpMark();
        go(firstNonBlank(t, last));
        finish(false);
        return;
    }
    if (text.startsWith(QLatin1Char('s')) && (text.size() == 1 || !text.at(1).isLetterOrNumber())
        && text.size() > 1 && !text.at(1).isSpace()) {
        substitute(ranged ? first : ls(m.m_cursor), ranged ? last : ls(m.m_cursor), text);
        return;
    }
    if (text == QLatin1String("w") || text == QLatin1String("write")) {
        emit m.vimSaveRequested();
    } else if (text == QLatin1String("q") || text == QLatin1String("quit")) {
        if (m.isModified())
            s.message = QStringLiteral("E37: No write since last change (add ! to override)");
        else
            emit m.vimCloseRequested();
    } else if (text == QLatin1String("q!") || text == QLatin1String("quit!")) {
        m.setModified(false);
        emit m.vimCloseRequested();
    } else if (text == QLatin1String("wq") || text == QLatin1String("x") || text == QLatin1String("xit")) {
        emit m.vimSaveRequested();
        emit m.vimCloseRequested();
    } else if (text == QLatin1String("noh") || text == QLatin1String("nohlsearch")) {
        m.setFindQuery(QString(), false, true);
    } else if (!text.isEmpty()) {
        s.message = QStringLiteral("E492: Not an editor command: ") + text;
    }
}

/// :s/pattern/replacement/[g][i] on the lines first..last (line starts). The
/// delimiter may be any punctuation; & and \\1 .. \\9 name the match and its groups.
void VimEngine::substitute(Pos first, Pos last, const QString &spec)
{
    const QChar delim = spec.at(1);
    QStringList parts;
    QString part;
    for (qsizetype j = 2; j < spec.size(); ++j) {
        const QChar c = spec.at(j);
        if (c == QLatin1Char('\\') && j + 1 < spec.size()) {
            if (spec.at(j + 1) == delim)
                part += delim; // an escaped delimiter is just that character
            else
                part += QString(c) + spec.at(j + 1);
            ++j;
        } else if (c == delim && parts.size() < 2) {
            parts << part;
            part.clear();
        } else {
            part += c;
        }
    }
    parts << part;
    const QString replacement = parts.value(1);
    const QString flags = parts.value(2);
    const QString pattern = parts.constFirst().isEmpty() ? s.lastSearch : parts.constFirst();
    const QRegularExpression regex(pattern, flags.contains(QLatin1Char('i'))
                                                ? QRegularExpression::CaseInsensitiveOption
                                                : QRegularExpression::NoPatternOption);
    if (pattern.isEmpty() || !regex.isValid()) {
        s.message = QStringLiteral("E486: Pattern not found: ") + pattern;
        return;
    }
    s.lastSearch = pattern;
    const bool global = flags.contains(QLatin1Char('g'));
    QString out;
    qsizetype done = 0, lines = 0;
    Pos changedAt = -1;
    Pos end = first;
    for (Pos p = first;;) {
        const Pos newline = t.indexOf(QLatin1Char('\n'), p);
        end = newline < 0 ? t.size() : newline;
        QString line = t.mid(p, end - p);
        const bool cr = line.endsWith(QLatin1Char('\r'));
        if (cr)
            line.chop(1);
        QString result;
        qsizetype from = 0, count = 0;
        for (auto it = regex.globalMatch(line); it.hasNext();) {
            const QRegularExpressionMatch match = it.next();
            result += line.mid(from, match.capturedStart() - from);
            for (qsizetype k = 0; k < replacement.size(); ++k) {
                const QChar c = replacement.at(k);
                if (c == QLatin1Char('&')) {
                    result += match.captured(0);
                } else if (c == QLatin1Char('\\') && k + 1 < replacement.size()) {
                    const QChar e = replacement.at(++k);
                    if (e.isDigit())
                        result += match.captured(e.digitValue());
                    else if (e == QLatin1Char('r'))
                        result += QLatin1Char('\n');
                    else if (e == QLatin1Char('t'))
                        result += QLatin1Char('\t');
                    else
                        result += e;
                } else {
                    result += c;
                }
            }
            from = match.capturedEnd();
            ++count;
            if (!global)
                break;
        }
        result += line.mid(from);
        if (count > 0) {
            done += count;
            ++lines;
            changedAt = out.size();
        }
        out += result + (cr ? QStringLiteral("\r") : QString());
        if (p == last || newline < 0)
            break;
        out += QLatin1Char('\n');
        p = newline + 1;
    }
    if (done == 0) {
        s.message = QStringLiteral("E486: Pattern not found: ") + pattern;
        return;
    }
    edit(first, end - first, out);
    go(firstNonBlank(t, first + changedAt));
    if (done > 2)
        s.message = QStringLiteral("%1 substitutions on %2 lines").arg(done).arg(lines);
    finish(false);
}

/// The first match after (or before) the caret, wrapping, from the editor's
/// own find matches, so they are also what the editor highlights.
bool VimEngine::searchFrom(bool forward, Pos &out)
{
    if (m.m_find.pattern != s.lastSearch || !m.m_find.regex) {
        const bool sensitive = s.lastSearch != s.lastSearch.toLower(); // smartcase
        m.setFindQuery(s.lastSearch, sensitive, true);
    }
    const QList<Range> &matches = m.m_find.matches;
    if (!m.m_find.valid || matches.isEmpty())
        return false;
    const Pos cur = m.m_cursor;
    if (forward) {
        const auto it = std::find_if(matches.cbegin(), matches.cend(),
                                     [cur](const Range &r) { return r.start > cur; });
        out = (it == matches.cend() ? matches.constFirst() : *it).start;
    } else {
        const auto it = std::find_if(matches.crbegin(), matches.crend(),
                                     [cur](const Range &r) { return r.start < cur; });
        out = (it == matches.crend() ? matches.constLast() : *it).start;
    }
    return true;
}

void VimEngine::search(const QString &pattern, bool forward)
{
    if (!pattern.isEmpty())
        s.lastSearch = pattern;
    s.searchForward = forward;
    Pos p;
    if (s.lastSearch.isEmpty()) {
        abort();
        return;
    }
    if (!searchFrom(forward, p)) {
        s.message = QStringLiteral("E486: Pattern not found: ") + s.lastSearch;
        abort();
        return;
    }
    setJumpMark();
    go(p);
    finish(false);
}

// --- MarkdownEditor's side ------------------------------------------------------------

bool MarkdownEditor::handleVimKey(QKeyEvent *event)
{
    // q{x} records the keys as they come, replays are not recorded again
    if (!m_vim.recording.isNull() && m_vim.macroDepth == 0) {
        switch (event->key()) {
        case Qt::Key_Shift: case Qt::Key_Control: case Qt::Key_Alt: case Qt::Key_AltGr:
        case Qt::Key_Meta: case Qt::Key_CapsLock: case Qt::Key_Super_L: case Qt::Key_Super_R:
            break;
        default:
            m_vim.recorded.append({event->key(), int(event->modifiers()), event->text()});
        }
    }
    const bool handled = VimEngine(*this).key(event);
    if (handled) {
        m_cursorOn = true;
        viewport()->update();
    }
    emitVimStatus();
    return handled;
}

void MarkdownEditor::setVimMode(bool on)
{
    if (on == m_vim.enabled)
        return;
    const QString shown = m_vim.lastStatus; // what listeners last heard
    if (on) {
        m_vim = VimState();
        m_vim.enabled = true;
        // A fresh editor starts in normal mode, the caret on a character.
        const qsizetype end = lineEndOf(m_text, m_cursor);
        if (m_cursor >= end && end > lineStartOf(m_text, m_cursor))
            moveCursor(end - 1, false);
    } else {
        if (m_vim.hold) { // leave an open insert session undoable
            m_vim.hold = false;
            m_lastEdit.invalidate();
        }
        m_vim = VimState();
        moveCursor(m_cursor, false);
    }
    m_vim.lastStatus = shown;
    viewport()->update();
    emitVimStatus();
}

QString MarkdownEditor::vimModeName() const
{
    if (!m_vim.enabled)
        return QString();
    switch (m_vim.mode) {
    case VimState::Mode::Normal:
        return QStringLiteral("normal");
    case VimState::Mode::Insert:
        return QStringLiteral("insert");
    case VimState::Mode::Visual:
        return QStringLiteral("visual");
    case VimState::Mode::VisualLine:
        return QStringLiteral("visual line");
    case VimState::Mode::VisualBlock:
        return QStringLiteral("visual block");
    }
    return QString();
}

QString MarkdownEditor::vimStatus() const
{
    if (!m_vim.enabled)
        return QString();
    if (!m_vim.cmdKind.isNull())
        return m_vim.cmdKind + m_vim.cmdline;
    if (!m_vim.message.isEmpty())
        return m_vim.message;
    QString text = QStringLiteral("-- ") + vimModeName().toUpper() + QStringLiteral(" --");
    if (!m_vim.keys.isEmpty())
        text += QStringLiteral("  ") + m_vim.keys;
    if (!m_vim.recording.isNull())
        text += QStringLiteral("  recording @") + m_vim.recording;
    return text;
}

void MarkdownEditor::emitVimStatus()
{
    const QString status = vimStatus();
    if (status == m_vim.lastStatus)
        return;
    m_vim.lastStatus = status;
    emit vimStatusChanged(status);
}

} // namespace noteAppC
