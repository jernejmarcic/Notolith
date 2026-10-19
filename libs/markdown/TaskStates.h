#pragma once

#include <QChar>
#include <QHash>
#include <QList>
#include <QMap>
#include <QString>

#include <memory>

namespace noteAppC {

/// What a state means, which is what search and styling go by. Obsidian's
/// Tasks plugin uses the same five.
enum class TaskType : quint8 { Todo, InProgress, Done, Cancelled, NonTask };

/// How a state's box is drawn.
enum class TaskGlyph : quint8 { Box, Tick, Half, Line, Cross, ArrowRight, ArrowLeft, Star, Char };

/// One character that can sit in a task's `[ ]`.
struct TaskState {
    QChar ch;
    QString name;
    TaskType type = TaskType::Todo;
    TaskGlyph glyph = TaskGlyph::Char;
    /// A theme token (`--color-red`) or any CSS colour; empty is the accent.
    QString color;
    bool strike = false; ///< the item's text is struck through
    bool mute = false;   ///< ... and drawn in the muted text colour
    QChar next = QLatin1Char('x'); ///< what a click writes
};

/// The table of task states: the defaults, plus whatever tasks.ini adds or
/// changes. `[ ]` and `[x]` are Obsidian's own and fixed. Kept apart from the
/// editor so the indexer's search and the publisher read the same definition.
class TaskStates {
public:
    static TaskStates defaults();
    /// The defaults with a parsed tasks.ini laid over them (see docs/STYLING.md
    /// for the format). A section that is wrong is ignored or partly applied
    /// rather than refused: this is a hand-edited file.
    static TaskStates fromSections(const QHash<QString, QMap<QString, QString>> &sections);

    /// `X` is `x`. A character the table does not know is still a task: a
    /// todo with no name that a click completes.
    TaskState at(QChar c) const;
    /// In table order, for menus.
    const QList<TaskState> &all() const { return m_states; }

    /// The table in force, defaults until something sets one. Read on pool
    /// threads (search) and swapped whole, so it is a shared pointer.
    static std::shared_ptr<const TaskStates> current();
    static void setCurrent(TaskStates states);

private:
    QList<TaskState> m_states;
};

/// A box holding the plain `x` or `X`.
inline bool isDoneBox(QChar c) { return c == QLatin1Char('x') || c == QLatin1Char('X'); }

/// The state for a box holding `c`: `[ ]`, `[x]` and `[X]` answer from two
/// fixed built-ins, anything else from the table in force.
TaskState taskStateOf(QChar c);

/// What a click on a box holding `c` leaves in it. `[ ]` and `[x]` never look
/// at the table, so they behave the same with the feature off.
QChar nextTaskState(QChar c);

/// `[c]`, the box as written in the note.
inline QString taskBox(QChar c) { return QLatin1Char('[') + QString(c) + QLatin1Char(']'); }

} // namespace noteAppC
