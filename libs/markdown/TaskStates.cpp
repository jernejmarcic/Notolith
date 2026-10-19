#include "TaskStates.h"


#include <QStringList>

#include <algorithm>
#include <atomic>

namespace noteAppC {

namespace {

TaskState state(char ch, const char *name, TaskType type, TaskGlyph glyph, const char *color,
                bool strike, bool mute, char next)
{
    TaskState s;
    s.ch = QLatin1Char(ch);
    s.name = QLatin1String(name);
    s.type = type;
    s.glyph = glyph;
    s.color = QLatin1String(color);
    s.strike = strike;
    s.mute = mute;
    s.next = QLatin1Char(next);
    return s;
}

bool legalChar(const QString &value)
{
    if (value.size() != 1)
        return false;
    const QChar c = value.at(0);
    return !c.isSpace() && c != QLatin1Char('[') && c != QLatin1Char(']') && c != QLatin1Char('\\')
           && !c.isSurrogate() && !isDoneBox(c);
}

TaskType typeFrom(const QString &word, TaskType fallback)
{
    static const QHash<QString, TaskType> words = {
        {QStringLiteral("todo"), TaskType::Todo},
        {QStringLiteral("in-progress"), TaskType::InProgress},
        {QStringLiteral("done"), TaskType::Done},
        {QStringLiteral("cancelled"), TaskType::Cancelled},
        {QStringLiteral("non-task"), TaskType::NonTask}};
    return words.value(word.toLower(), fallback);
}

TaskGlyph glyphFrom(const QString &word, TaskGlyph fallback)
{
    static const QHash<QString, TaskGlyph> words = {
        {QStringLiteral("box"), TaskGlyph::Box},
        {QStringLiteral("tick"), TaskGlyph::Tick},
        {QStringLiteral("half"), TaskGlyph::Half},
        {QStringLiteral("line"), TaskGlyph::Line},
        {QStringLiteral("cross"), TaskGlyph::Cross},
        {QStringLiteral("arrow-right"), TaskGlyph::ArrowRight},
        {QStringLiteral("arrow-left"), TaskGlyph::ArrowLeft},
        {QStringLiteral("star"), TaskGlyph::Star},
        {QStringLiteral("char"), TaskGlyph::Char}};
    return words.value(word.toLower(), fallback);
}

/// Applies one section's keys to `s`. An unknown word keeps the old value.
void apply(TaskState &s, const QMap<QString, QString> &keys)
{
    s.type = typeFrom(keys.value(QStringLiteral("type")), s.type);
    s.glyph = glyphFrom(keys.value(QStringLiteral("glyph")), s.glyph);
    if (keys.contains(QStringLiteral("color")))
        s.color = keys.value(QStringLiteral("color"));
    if (keys.contains(QStringLiteral("text"))) {
        const QString word = keys.value(QStringLiteral("text")).toLower();
        if (word == QLatin1String("strike") || word == QLatin1String("mute")
            || word == QLatin1String("both") || word == QLatin1String("none")) {
            s.strike = word == QLatin1String("strike") || word == QLatin1String("both");
            s.mute = word == QLatin1String("mute") || word == QLatin1String("both");
        }
    }
    if (keys.contains(QStringLiteral("next"))) {
        const QString next = keys.value(QStringLiteral("next"));
        s.next = next.size() == 1 && (next == QLatin1String(" ") || legalChar(next) || isDoneBox(next.at(0)))
                     ? next.at(0)
                     : QLatin1Char('x');
    }
}

std::atomic<std::shared_ptr<const TaskStates>> &slot()
{
    static std::atomic<std::shared_ptr<const TaskStates>> value{
        std::make_shared<const TaskStates>(TaskStates::defaults())};
    return value;
}

} // namespace

TaskStates TaskStates::defaults()
{
    using T = TaskType;
    using G = TaskGlyph;
    TaskStates out;
    out.m_states = {
        state(' ', "todo", T::Todo, G::Box, "--background-modifier-border-hover", false, false, 'x'),
        state('x', "done", T::Done, G::Tick, "--interactive-accent", true, true, ' '),
        state('/', "in-progress", T::InProgress, G::Half, "--interactive-accent", false, false, 'x'),
        state('-', "cancelled", T::Cancelled, G::Line, "--text-muted", true, true, ' '),
        state('~', "dropped", T::Cancelled, G::Cross, "--color-red", true, true, ' '),
        state('>', "forwarded", T::Todo, G::ArrowRight, "--interactive-accent", false, false, 'x'),
        state('<', "scheduled", T::Todo, G::ArrowLeft, "--interactive-accent", false, false, 'x'),
        state('?', "question", T::Todo, G::Char, "--color-blue", false, false, 'x'),
        state('!', "important", T::Todo, G::Char, "--color-orange", false, false, 'x'),
        state('*', "star", T::Todo, G::Star, "--color-yellow", false, false, 'x'),
        state('I', "idea", T::NonTask, G::Char, "--color-yellow", false, false, 'x'),
        state('p', "pro", T::NonTask, G::Char, "--color-green", false, false, 'x'),
        state('c', "con", T::NonTask, G::Char, "--color-red", false, false, 'x'),
    };
    return out;
}

TaskStates TaskStates::fromSections(const QHash<QString, QMap<QString, QString>> &sections)
{
    TaskStates out = defaults();
    // The reader hands sections over unordered; sorting makes "the later one
    // wins" a rule a file can be written against.
    QStringList names = sections.keys();
    std::sort(names.begin(), names.end());
    // Built-in names first: they change a state in place, which a new state
    // claiming the same character may then take over.
    std::stable_partition(names.begin(), names.end(), [&out](const QString &name) {
        return std::any_of(out.m_states.cbegin(), out.m_states.cend(),
                           [&name](const TaskState &s) { return s.name == name; });
    });
    for (const QString &name : std::as_const(names)) {
        if (name == QLatin1String("todo") || name == QLatin1String("done"))
            continue; // Obsidian's own, fixed
        const QMap<QString, QString> &keys = sections[name];
        const auto known = std::find_if(out.m_states.begin(), out.m_states.end(),
                                        [&name](const TaskState &s) { return s.name == name; });
        TaskState s = known != out.m_states.end() ? *known : TaskState();
        if (keys.contains(QStringLiteral("char"))) {
            if (!legalChar(keys.value(QStringLiteral("char"))))
                continue;
            s.ch = keys.value(QStringLiteral("char")).at(0);
        } else if (known == out.m_states.end()) {
            continue; // a new state has to say which character it is
        }
        s.name = name;
        apply(s, keys);
        out.m_states.removeIf([&s](const TaskState &other) { return other.ch == s.ch && other.name != s.name; });
        const auto again = std::find_if(out.m_states.begin(), out.m_states.end(),
                                        [&name](const TaskState &t) { return t.name == name; });
        if (again != out.m_states.end())
            *again = s;
        else
            out.m_states.append(s);
    }
    return out;
}

TaskState TaskStates::at(QChar c) const
{
    if (c == QLatin1Char('X'))
        c = QLatin1Char('x');
    for (const TaskState &s : m_states) {
        if (s.ch == c)
            return s;
    }
    TaskState unknown;
    unknown.ch = c;
    return unknown;
}

TaskState taskStateOf(QChar c)
{
    static const TaskStates builtin = TaskStates::defaults();
    if (c == QLatin1Char(' ') || isDoneBox(c))
        return builtin.at(c);
    return TaskStates::current()->at(c);
}

QChar nextTaskState(QChar c)
{
    return taskStateOf(c).next;
}

std::shared_ptr<const TaskStates> TaskStates::current()
{
    return slot().load();
}

void TaskStates::setCurrent(TaskStates states)
{
    slot().store(std::make_shared<const TaskStates>(std::move(states)));
}

} // namespace noteAppC
