/// Vim's modal editing for the note editor, optional and off by default.
///
/// Hand-rolled because no vim-emulation library is installed (FakeVim ships
/// with Qt Creator only). It covers the part of vim a note-taker lives in:
/// normal, insert, visual and visual-line modes; counts; the motions
/// h j k l w W b B e E 0 ^ $ gg G { } f F t T ; , n N and gj gk; the operators
/// d c y with any of them, doubled for a line; x X D C s S p P u Ctrl+r J r ~
/// . i a I A o O; and a `:` / `/` / `?` line (`:w :q :wq :x :q! :noh :N`).
/// Not covered, deliberately: text objects, named registers, marks, macros,
/// block-visual, `%`.
///
/// It is a friend of MarkdownEditor and works on the editor's own buffer,
/// caret and undo stack, so nothing here is a second copy of anything. An
/// instance lives for one keystroke; everything it remembers is
/// MarkdownEditor::VimState.

#include "MarkdownEditor.h"

#include <QKeyEvent>

#include <algorithm>
#include <limits>

namespace noteAppC {

namespace {

using Pos = qsizetype;

constexpr QChar kEsc(0x1b);
constexpr QChar kGoTop(0x01); ///< the internal key for gg

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
    bool visual() const { return s.mode == Mode::Visual || s.mode == Mode::VisualLine; }
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
    void doOperator(QChar op, Range r, bool linewise, Pos hint);
    void visualOperator(QChar op);

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

void VimEngine::exitVisual()
{
    s.mode = Mode::Normal;
    s.overrideRange = {-1, -1};
    m.moveCursor(m.m_cursor, false);
    m.viewport()->update();
}

void VimEngine::syncVisual()
{
    const Pos low = qMin(m.m_anchor, m.m_cursor);
    const Pos high = qMax(m.m_anchor, m.m_cursor);
    s.overrideRange = s.mode == Mode::VisualLine ? Range{ls(low), le(high)} : Range{low, afterChar(t, high)};
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
    s.op = s.pending = QChar();
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
        return false; // Ctrl+F, Ctrl+B, Ctrl+S ... keep their meaning
    }
    if (other)
        return false;

    switch (k) {
    case Qt::Key_Escape: {
        const bool busy = visual() || !s.op.isNull() || !s.pending.isNull() || s.count || s.count2;
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
    switch (c.unicode()) {
    case 'h': case 'j': case 'k': case 'l': case 'w': case 'W': case 'b': case 'B':
    case 'e': case 'E': case '0': case '^': case '$': case 'G': case '{': case '}':
    case ';': case ',':
        runMotion(c);
        return;
    case 'n': case 'N':
        runMotion(c);
        return;
    case 'g': case 'f': case 'F': case 't': case 'T':
        s.pending = c;
        return;
    case 'd': case 'c': case 'y':
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
        enterInsert(firstNonBlank(t, m.m_cursor));
        finish(true);
        return;
    case 'A':
        enterInsert(le(m.m_cursor));
        finish(true);
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
    case 'V': {
        const Mode wanted = c == QLatin1Char('v') ? Mode::Visual : Mode::VisualLine;
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
    case 'N': {
        if (s.lastSearch.isEmpty())
            return false;
        const bool forward = (key == QLatin1Char('n')) == s.searchForward;
        Pos p = cur;
        for (int i = 0; i < n; ++i) {
            // searchFrom reads the caret, so walk it between repeats
            m.m_cursor = p;
            if (!searchFrom(forward, p)) {
                m.m_cursor = cur;
                s.message = QStringLiteral("E486: Pattern not found: ") + s.lastSearch;
                return false;
            }
        }
        m.m_cursor = cur;
        out.pos = p;
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
    go(mv.pos);
    finish(false);
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
void VimEngine::doOperator(QChar op, Range r, bool linewise, Pos hint)
{
    const bool yank = op == QLatin1Char('y');
    const bool change = op == QLatin1Char('c');
    s.op = QChar();
    s.reg = t.mid(r.start, r.end - r.start);
    s.regLinewise = linewise;
    if (linewise)
        s.reg += QLatin1Char('\n');

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
    const Pos low = qMin(m.m_anchor, m.m_cursor);
    const Pos high = qMax(m.m_anchor, m.m_cursor);
    const bool linewise = s.mode == Mode::VisualLine;
    const Range r = linewise ? Range{ls(low), le(high)} : Range{low, afterChar(t, high)};
    s.mode = Mode::Normal;
    s.overrideRange = {-1, -1};
    m.moveCursor(m.m_cursor, false);
    doOperator(op, r, linewise, low);
}

// --- single-key commands -----------------------------------------------------------

void VimEngine::paste(bool after)
{
    if (s.reg.isEmpty()) {
        abort();
        return;
    }
    QString text;
    for (int i = 0; i < total(); ++i)
        text += s.reg;
    const Pos cur = m.m_cursor;
    if (s.regLinewise) {
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

void VimEngine::exCommand(const QString &text)
{
    bool isNumber = false;
    const qsizetype line = text.toLongLong(&isNumber);
    if (isNumber) {
        go(firstNonBlank(t, lineStartByNumber(t, qMax<qsizetype>(0, line - 1))));
        finish(false);
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
    go(p);
    finish(false);
}

// --- MarkdownEditor's side ------------------------------------------------------------

bool MarkdownEditor::handleVimKey(QKeyEvent *event)
{
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
