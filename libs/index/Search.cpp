#include "Search.h"

#include <algorithm>
#include <optional>
#include <memory>

#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace noteAppC {

namespace {

// --- scope ranges: the text a line:/block:/section:/task* field searches --

/// Every line, as a half-open range excluding its newline. Reuses the same
/// line-start scan the editor already pays for on every keystroke, rather
/// than hand-rolling another one here.
QList<Range> lineRanges(const QString &text)
{
    const SourceMap map(text);
    QList<Range> ranges;
    ranges.reserve(map.lineCount());
    for (qsizetype i = 0; i < map.lineCount(); ++i)
        ranges.append({map.lineStart(i), map.lineEnd(i)});
    return ranges;
}

/// A "block" here is a paragraph: a run of non-blank lines, blank lines
/// (whitespace-only) as the separator. Obsidian's own notion of a block is
/// finer (a list item is its own block, for instance); this is the
/// ponytail version - simple, and right often enough that `line:` and
/// `section:` cover the rest. Upgrade path: walk md4qt's block tree instead,
/// the way the editor's own layout does, if a list-item-precision block:
/// query is ever asked for.
QList<Range> blockRanges(const QString &text)
{
    const auto lineEndFrom = [&text](qsizetype at) {
        const qsizetype newline = text.indexOf(QLatin1Char('\n'), at);
        return newline < 0 ? text.size() : newline;
    };
    const auto isBlank = [&text](qsizetype start, qsizetype end) {
        return QStringView(text).mid(start, end - start).trimmed().isEmpty();
    };

    QList<Range> blocks;
    const qsizetype n = text.size();
    qsizetype i = 0;
    while (i < n) {
        qsizetype end = lineEndFrom(i);
        while (i < n && isBlank(i, end)) {
            i = qMin(n, end + 1);
            end = lineEndFrom(i);
        }
        if (i >= n)
            break;
        const qsizetype start = i;
        while (i < n && !isBlank(i, end)) {
            i = qMin(n, end + 1);
            end = lineEndFrom(i);
        }
        blocks.append({start, i > start ? (text.at(i - 1) == QLatin1Char('\n') ? i - 1 : i) : n});
    }
    return blocks;
}

/// A section is a heading and everything under it, down to the next heading
/// of the same or a shallower level (or the end of the note). Text before the
/// first heading, if any, is its own section - a note with no headings at all
/// is one section covering the whole thing, which makes `section:` degrade
/// gracefully to a plain document-wide search.
QList<Range> sectionRanges(const NoteContext &note)
{
    if (note.headings.isEmpty())
        return {{0, note.text.size()}};

    QList<NoteHeading> sorted = note.headings;
    std::sort(sorted.begin(), sorted.end(),
              [](const NoteHeading &a, const NoteHeading &b) { return a.range.start < b.range.start; });

    QList<Range> sections;
    if (sorted.first().range.start > 0)
        sections.append({0, sorted.first().range.start});

    for (qsizetype i = 0; i < sorted.size(); ++i) {
        qsizetype end = note.text.size();
        for (qsizetype j = i + 1; j < sorted.size(); ++j) {
            if (sorted[j].level <= sorted[i].level) {
                end = sorted[j].range.start;
                break;
            }
        }
        sections.append({sorted[i].range.start, end});
    }
    return sections;
}

/// `filter < 0`: every task. `0`: only undone. `1`: only done.
QList<Range> taskRanges(const NoteContext &note, int filter)
{
    QList<Range> ranges;
    for (const NoteTask &task : note.tasks) {
        if (filter == 0 && task.done)
            continue;
        if (filter == 1 && !task.done)
            continue;
        ranges.append(task.range);
    }
    return ranges;
}

// --- the parse tree ---------------------------------------------------------

struct EvalContext {
    const NoteContext &note;
    Range scope; ///< the haystack a leaf searches: a sub-range of note.text
    Qt::CaseSensitivity caseSensitivity;
};

QString quoteForFts(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

} // namespace

/// Base of every node. `evaluate` is the only thing that has to be correct;
/// `translateFts` and `fieldNeeds` are pure optimisations over it - a node
/// that gets them wrong in the conservative direction (translateFts too
/// narrow, fieldNeeds too generous) only costs speed, never correctness,
/// which is why they default to the safe answer (wild, needs-everything is
/// not the default here but every override is deliberate below).
class SearchQueryNode {
public:
    virtual ~SearchQueryNode() = default;
    virtual SearchOutcome evaluate(const EvalContext &ctx) const = 0;
    /// Empty means "no restriction derivable" (wild), never an invalid query.
    virtual QString translateFts() const { return QString(); }
    virtual FieldNeeds fieldNeeds() const { return {}; }
};

namespace {

using NodePtr = std::shared_ptr<const SearchQueryNode>;

/// A bare word or a "quoted phrase" - the only difference between them is
/// whether the parser let spaces into the literal, so one node serves both.
class LiteralNode : public SearchQueryNode {
public:
    explicit LiteralNode(QString text) : m_text(std::move(text)) {}

    SearchOutcome evaluate(const EvalContext &ctx) const override
    {
        SearchOutcome outcome;
        if (m_text.isEmpty())
            return outcome;
        const QStringView haystack = QStringView(ctx.note.text).mid(ctx.scope.start, ctx.scope.length());
        qsizetype from = 0;
        while (true) {
            const qsizetype hit = haystack.indexOf(m_text, from, ctx.caseSensitivity);
            if (hit < 0)
                break;
            outcome.matches.append({ctx.scope.start + hit, m_text.size()});
            from = hit + m_text.size();
            if (outcome.matches.size() >= 500)
                break; // ponytail: a ceiling against a one-character needle in a huge note
        }
        outcome.matched = !outcome.matches.isEmpty();
        return outcome;
    }

    QString translateFts() const override { return quoteForFts(m_text); }

private:
    QString m_text;
};

/// `/pattern/`, JS-flavoured per Obsidian's docs; QRegularExpression (PCRE2)
/// is close enough for the common case and is what the codebase already has -
/// nothing here claims byte-for-byte JS regex compatibility.
class RegexNode : public SearchQueryNode {
public:
    explicit RegexNode(QRegularExpression regex) : m_regex(std::move(regex)) {}

    SearchOutcome evaluate(const EvalContext &ctx) const override
    {
        SearchOutcome outcome;
        QRegularExpression regex = m_regex;
        if (ctx.caseSensitivity == Qt::CaseInsensitive)
            regex.setPatternOptions(regex.patternOptions() | QRegularExpression::CaseInsensitiveOption);
        auto it = regex.globalMatch(ctx.note.text, ctx.scope.start);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            if (match.capturedStart() >= ctx.scope.end)
                break;
            if (match.capturedEnd() > ctx.scope.end)
                continue; // straddles the scope boundary: not a match inside it
            outcome.matches.append({match.capturedStart(), match.capturedLength()});
            if (outcome.matches.size() >= 500)
                break;
        }
        outcome.matched = !outcome.matches.isEmpty();
        return outcome;
    }

private:
    QRegularExpression m_regex;
};

class NotNode : public SearchQueryNode {
public:
    explicit NotNode(NodePtr child) : m_child(std::move(child)) {}

    SearchOutcome evaluate(const EvalContext &ctx) const override
    {
        // A negated match highlights nothing - there is no "absence" to point
        // at - it only ever flips whether the enclosing scope counts.
        return {!m_child->evaluate(ctx).matched, {}};
    }

    FieldNeeds fieldNeeds() const override { return m_child->fieldNeeds(); }

private:
    NodePtr m_child;
};

class AndNode : public SearchQueryNode {
public:
    explicit AndNode(QList<NodePtr> children) : m_children(std::move(children)) {}

    SearchOutcome evaluate(const EvalContext &ctx) const override
    {
        SearchOutcome outcome;
        outcome.matched = true;
        for (const NodePtr &child : m_children) {
            const SearchOutcome inner = child->evaluate(ctx);
            if (!inner.matched)
                return {};
            outcome.matches += inner.matches;
        }
        return outcome;
    }

    QString translateFts() const override
    {
        QStringList parts;
        for (const NodePtr &child : m_children) {
            const QString t = child->translateFts();
            if (!t.isEmpty())
                parts << t;
        }
        return parts.join(QLatin1String(" AND "));
    }

    FieldNeeds fieldNeeds() const override
    {
        FieldNeeds needs;
        for (const NodePtr &child : m_children)
            needs = needs | child->fieldNeeds();
        return needs;
    }

private:
    QList<NodePtr> m_children;
};

class OrNode : public SearchQueryNode {
public:
    explicit OrNode(QList<NodePtr> children) : m_children(std::move(children)) {}

    SearchOutcome evaluate(const EvalContext &ctx) const override
    {
        SearchOutcome outcome;
        for (const NodePtr &child : m_children) {
            const SearchOutcome inner = child->evaluate(ctx);
            if (inner.matched) {
                outcome.matched = true;
                outcome.matches += inner.matches;
            }
        }
        return outcome;
    }

    QString translateFts() const override
    {
        QStringList parts;
        for (const NodePtr &child : m_children) {
            const QString t = child->translateFts();
            if (t.isEmpty())
                return QString(); // one untranslatable branch and OR can exclude nothing
            parts << (QLatin1Char('(') + t + QLatin1Char(')'));
        }
        return parts.join(QLatin1String(" OR "));
    }

    FieldNeeds fieldNeeds() const override
    {
        FieldNeeds needs;
        for (const NodePtr &child : m_children)
            needs = needs | child->fieldNeeds();
        return needs;
    }

private:
    QList<NodePtr> m_children;
};

class CaseOverrideNode : public SearchQueryNode {
public:
    CaseOverrideNode(NodePtr child, Qt::CaseSensitivity forced)
        : m_child(std::move(child)), m_forced(forced)
    {
    }

    SearchOutcome evaluate(const EvalContext &ctx) const override
    {
        return m_child->evaluate({ctx.note, ctx.scope, m_forced});
    }

    QString translateFts() const override { return m_child->translateFts(); }
    FieldNeeds fieldNeeds() const override { return m_child->fieldNeeds(); }

private:
    NodePtr m_child;
    Qt::CaseSensitivity m_forced;
};

/// `path:` / `file:`. Neither has a position in the note's text - a path is
/// not part of the body - so a match here never contributes a SearchMatch.
class PathLikeNode : public SearchQueryNode {
public:
    enum class Source { Path, FileName };

    PathLikeNode(Source source, QString literal, QRegularExpression regex, bool isRegex)
        : m_source(source), m_literal(std::move(literal)), m_regex(std::move(regex)), m_isRegex(isRegex)
    {
    }

    SearchOutcome evaluate(const EvalContext &ctx) const override
    {
        const QString subject =
            m_source == Source::Path ? ctx.note.path : QFileInfo(ctx.note.path).fileName();
        bool matched = false;
        if (m_isRegex) {
            QRegularExpression regex = m_regex;
            if (ctx.caseSensitivity == Qt::CaseInsensitive)
                regex.setPatternOptions(regex.patternOptions() | QRegularExpression::CaseInsensitiveOption);
            matched = regex.match(subject).hasMatch();
        } else {
            matched = subject.contains(m_literal, ctx.caseSensitivity);
        }
        return {matched, {}};
    }

private:
    Source m_source;
    QString m_literal;
    QRegularExpression m_regex;
    bool m_isRegex;
};

/// `[property]` - the note has it - and `[property:value]`, a value of it
/// containing `value` (a /regex/ or a "phrase" too); `[property:null]` is a
/// property with no value, as Obsidian has it. Keys match without regard to
/// case. A property has no place in the body to point at, so, like `path:`,
/// a match carries no offsets.
class PropertyNode : public SearchQueryNode {
public:
    PropertyNode(QString key, std::optional<QString> literal, QRegularExpression regex, bool isRegex)
        : m_key(std::move(key)), m_literal(std::move(literal)), m_regex(std::move(regex)), m_isRegex(isRegex)
    {
    }

    SearchOutcome evaluate(const EvalContext &ctx) const override
    {
        for (const NoteProperty &property : ctx.note.properties) {
            if (property.key.compare(m_key, Qt::CaseInsensitive) != 0)
                continue;
            if (!m_literal && !m_isRegex)
                return {true, {}};
            if (!m_isRegex && m_literal->compare(QLatin1String("null"), Qt::CaseInsensitive) == 0) {
                const bool empty = std::all_of(property.values.cbegin(), property.values.cend(),
                                               [](const QString &value) { return value.trimmed().isEmpty(); });
                return {empty, {}};
            }
            for (const QString &value : property.values) {
                bool matched = false;
                if (m_isRegex) {
                    QRegularExpression regex = m_regex;
                    if (ctx.caseSensitivity == Qt::CaseInsensitive)
                        regex.setPatternOptions(regex.patternOptions() | QRegularExpression::CaseInsensitiveOption);
                    matched = regex.match(value).hasMatch();
                } else {
                    matched = value.contains(*m_literal, ctx.caseSensitivity);
                }
                if (matched)
                    return {true, {}};
            }
        }
        return {};
    }

    FieldNeeds fieldNeeds() const override { return {false, false, false, true}; }

private:
    QString m_key;
    std::optional<QString> m_literal;
    QRegularExpression m_regex;
    bool m_isRegex;
};

/// `tag:`. A tag matches its own nested children the way Index::notesWithTag
/// already does (`tag:#work` also finds `#work/meeting`), and every match
/// carries the tag's real position, so a tag search highlights like any
/// other.
class TagNode : public SearchQueryNode {
public:
    TagNode(QString literal, QRegularExpression regex, bool isRegex)
        : m_literal(std::move(literal)), m_regex(std::move(regex)), m_isRegex(isRegex)
    {
    }

    SearchOutcome evaluate(const EvalContext &ctx) const override
    {
        SearchOutcome outcome;
        for (const NoteTag &tag : ctx.note.tags) {
            if (!ctx.scope.overlaps(tag.range))
                continue;
            bool matched;
            if (m_isRegex) {
                matched = m_regex.match(tag.name).hasMatch();
            } else {
                matched = tag.name.compare(m_literal, ctx.caseSensitivity) == 0
                    || tag.name.startsWith(m_literal + QLatin1Char('/'), ctx.caseSensitivity);
            }
            if (matched) {
                outcome.matched = true;
                outcome.matches.append(ownText(ctx.note.text, tag));
            }
        }
        return outcome;
    }

    FieldNeeds fieldNeeds() const override { return {true, false, false}; }

private:
    /// An inline tag's range is its own `#tag`. A frontmatter tag's is the
    /// whole property block it came from, which highlighted every line of
    /// the frontmatter; the tag's own word inside it is the match.
    static SearchMatch ownText(const QString &text, const NoteTag &tag)
    {
        const qsizetype start = tag.range.start;
        const qsizetype length = tag.range.length();
        if (length <= tag.name.size() + 1 || tag.name.isEmpty())
            return {start, length};
        const auto isWordChar = [](QChar c) { return c.isLetterOrNumber() || c == u'/' || c == u'_' || c == u'-'; };
        const QStringView block = QStringView(text).mid(start, length);
        for (qsizetype at = block.indexOf(tag.name, 0, Qt::CaseInsensitive); at >= 0;
             at = block.indexOf(tag.name, at + 1, Qt::CaseInsensitive)) {
            const qsizetype end = at + tag.name.size();
            if ((at == 0 || !isWordChar(block.at(at - 1))) && (end == block.size() || !isWordChar(block.at(end))))
                return {start + at, tag.name.size()};
        }
        return {start, length};
    }

    QString m_literal;
    QRegularExpression m_regex;
    bool m_isRegex;
};

class ScopeNode : public SearchQueryNode {
public:
    enum class Kind { Line, Block, Section, Task, TaskTodo, TaskDone };

    ScopeNode(Kind kind, NodePtr child) : m_kind(kind), m_child(std::move(child)) {}

    SearchOutcome evaluate(const EvalContext &ctx) const override
    {
        SearchOutcome outcome;
        for (const Range &range : candidateRanges(ctx.note)) {
            if (range.start < ctx.scope.start || range.end > ctx.scope.end)
                continue; // outside whatever scope this node is itself nested in
            const SearchOutcome inner = m_child->evaluate({ctx.note, range, ctx.caseSensitivity});
            if (inner.matched) {
                outcome.matched = true;
                outcome.matches += inner.matches;
            }
        }
        return outcome;
    }

    // Scoping only restricts *where* a term has to be, not *whether* it is in
    // the document at all - so the inner query's own translation is still a
    // valid superset for the whole note.
    QString translateFts() const override { return m_child->translateFts(); }

    FieldNeeds fieldNeeds() const override
    {
        FieldNeeds needs = m_child->fieldNeeds();
        needs.headings = needs.headings || m_kind == Kind::Section;
        needs.tasks = needs.tasks || m_kind == Kind::Task || m_kind == Kind::TaskTodo || m_kind == Kind::TaskDone;
        return needs;
    }

private:
    QList<Range> candidateRanges(const NoteContext &note) const
    {
        switch (m_kind) {
        case Kind::Line:
            return lineRanges(note.text);
        case Kind::Block:
            return blockRanges(note.text);
        case Kind::Section:
            return sectionRanges(note);
        case Kind::Task:
            return taskRanges(note, -1);
        case Kind::TaskTodo:
            return taskRanges(note, 0);
        case Kind::TaskDone:
            return taskRanges(note, 1);
        }
        return {};
    }

    Kind m_kind;
    NodePtr m_child;
};

// --- the parser --------------------------------------------------------

const QSet<QString> &knownFields()
{
    static const QSet<QString> fields = {
        QStringLiteral("file"),       QStringLiteral("path"),       QStringLiteral("content"),
        QStringLiteral("tag"),        QStringLiteral("line"),       QStringLiteral("block"),
        QStringLiteral("section"),    QStringLiteral("task"),       QStringLiteral("task-todo"),
        QStringLiteral("task-done"),  QStringLiteral("match-case"), QStringLiteral("ignore-case"),
    };
    return fields;
}

/// Recursive-descent, hand-rolled: the grammar is small and every other
/// parser in this codebase (md4qt aside) is written the same way. Grammar:
///
///   query   := orExpr
///   orExpr  := andExpr (WS 'OR' WS andExpr)*
///   andExpr := term+                      -- juxtaposition is AND
///   term    := '-'? primary
///   primary := '(' orExpr ')' | field | phrase | regex | word
///   field   := name ':' value
///   value   := '(' orExpr ')' | phrase | regex | word
class QueryParser {
public:
    explicit QueryParser(const QString &text) : m_text(text) {}

    NodePtr parse(QString *error)
    {
        skipSpace();
        NodePtr root = parseOr();
        skipSpace();
        if (root && m_pos != m_text.size()) {
            m_error = QStringLiteral("unexpected ')' at position %1").arg(m_pos);
            root.reset();
        }
        if (!root && m_error.isEmpty())
            m_error = QStringLiteral("empty query");
        if (error)
            *error = m_error;
        return root;
    }

private:
    QString m_text;
    qsizetype m_pos = 0;
    QString m_error;

    bool atEnd() const { return m_pos >= m_text.size(); }
    QChar peek() const { return atEnd() ? QChar() : m_text.at(m_pos); }
    void skipSpace()
    {
        while (!atEnd() && m_text.at(m_pos).isSpace())
            ++m_pos;
    }
    bool failed() const { return !m_error.isEmpty(); }

    /// Obsidian requires OR in capitals, precisely so a note about "or" does
    /// not turn every search into a union. Only recognised as a whole token.
    bool peekOrKeyword() const
    {
        if (m_pos + 2 > m_text.size())
            return false;
        if (m_text.at(m_pos) != QLatin1Char('O') || m_text.at(m_pos + 1) != QLatin1Char('R'))
            return false;
        return m_pos + 2 == m_text.size() || m_text.at(m_pos + 2).isSpace()
            || m_text.at(m_pos + 2) == QLatin1Char(')');
    }

    NodePtr parseOr()
    {
        QList<NodePtr> parts;
        NodePtr first = parseAnd();
        if (!first)
            return nullptr;
        parts << first;
        while (true) {
            skipSpace();
            if (!peekOrKeyword())
                break;
            m_pos += 2;
            skipSpace();
            NodePtr next = parseAnd();
            if (!next) {
                if (!failed())
                    m_error = QStringLiteral("expected a term after OR");
                return nullptr;
            }
            parts << next;
        }
        if (parts.size() == 1)
            return parts.first();
        return std::make_shared<const OrNode>(parts);
    }

    NodePtr parseAnd()
    {
        QList<NodePtr> parts;
        while (true) {
            skipSpace();
            if (atEnd() || peek() == QLatin1Char(')') || peekOrKeyword())
                break;
            NodePtr term = parseTerm();
            if (!term)
                return nullptr;
            parts << term;
        }
        if (parts.isEmpty()) {
            if (!failed())
                m_error = QStringLiteral("expected a search term at position %1").arg(m_pos);
            return nullptr;
        }
        if (parts.size() == 1)
            return parts.first();
        return std::make_shared<const AndNode>(parts);
    }

    NodePtr parseTerm()
    {
        bool negate = false;
        if (peek() == QLatin1Char('-')) {
            negate = true;
            ++m_pos;
        }
        NodePtr primary = parsePrimary();
        if (!primary)
            return nullptr;
        return negate ? std::make_shared<const NotNode>(primary) : primary;
    }

    NodePtr parsePrimary()
    {
        if (atEnd()) {
            m_error = QStringLiteral("expected a search term at position %1").arg(m_pos);
            return nullptr;
        }
        if (peek() == QLatin1Char('(')) {
            ++m_pos;
            skipSpace();
            NodePtr inner = parseOr();
            if (!inner)
                return nullptr;
            skipSpace();
            if (peek() != QLatin1Char(')')) {
                m_error = QStringLiteral("expected ')' at position %1").arg(m_pos);
                return nullptr;
            }
            ++m_pos;
            return inner;
        }
        if (peek() == QLatin1Char('"'))
            return parsePhraseNode();
        if (peek() == QLatin1Char('/'))
            return parseRegexNode();
        if (peek() == QLatin1Char('[') && m_text.mid(m_pos + 1, 1) != QLatin1String("["))
            return parsePropertyNode(); // "[[" is a word: a link's own text

        const QString field = tryReadFieldName();
        if (!field.isEmpty())
            return parseField(field);

        return parseWordNode();
    }

    /// Peeks a `letters-and-hyphens:` prefix; consumes it (name and colon)
    /// only when the name is one this grammar knows, so an ordinary word like
    /// "http://example.com" or "12:30" is never mistaken for a field.
    QString tryReadFieldName()
    {
        qsizetype p = m_pos;
        while (p < m_text.size() && (m_text.at(p).isLetter() || m_text.at(p) == QLatin1Char('-')))
            ++p;
        if (p == m_pos || p >= m_text.size() || m_text.at(p) != QLatin1Char(':'))
            return {};
        const QString name = m_text.mid(m_pos, p - m_pos).toLower();
        if (!knownFields().contains(name))
            return {};
        m_pos = p + 1;
        return name;
    }

    NodePtr parseField(const QString &name)
    {
        if (name == QLatin1String("line"))
            return parseScopeField(ScopeNode::Kind::Line);
        if (name == QLatin1String("block"))
            return parseScopeField(ScopeNode::Kind::Block);
        if (name == QLatin1String("section"))
            return parseScopeField(ScopeNode::Kind::Section);
        if (name == QLatin1String("task"))
            return parseScopeField(ScopeNode::Kind::Task);
        if (name == QLatin1String("task-todo"))
            return parseScopeField(ScopeNode::Kind::TaskTodo);
        if (name == QLatin1String("task-done"))
            return parseScopeField(ScopeNode::Kind::TaskDone);
        if (name == QLatin1String("match-case"))
            return parseCaseField(Qt::CaseSensitive);
        if (name == QLatin1String("ignore-case"))
            return parseCaseField(Qt::CaseInsensitive);
        if (name == QLatin1String("path"))
            return parsePathLikeField(PathLikeNode::Source::Path);
        if (name == QLatin1String("file"))
            return parsePathLikeField(PathLikeNode::Source::FileName);
        if (name == QLatin1String("tag"))
            return parseTagField();
        // "content": exactly the value's own node, content: is transparent.
        return parseFieldValueAsQuery();
    }

    /// `[key]`, `[key:value]`, `[key:"a phrase"]` or `[key:/regex/]`.
    NodePtr parsePropertyNode()
    {
        const qsizetype close = m_text.indexOf(QLatin1Char(']'), m_pos);
        if (close < 0) {
            m_error = QStringLiteral("unterminated '[' at position %1").arg(m_pos);
            return nullptr;
        }
        const QString inside = m_text.mid(m_pos + 1, close - m_pos - 1);
        m_pos = close + 1;
        const qsizetype colon = inside.indexOf(QLatin1Char(':'));
        const QString key = (colon < 0 ? inside : inside.left(colon)).trimmed();
        if (key.isEmpty()) {
            m_error = QStringLiteral("a property name is missing inside '[ ]'");
            return nullptr;
        }
        if (colon < 0)
            return std::make_shared<const PropertyNode>(key, std::nullopt, QRegularExpression(), false);
        QString value = inside.mid(colon + 1).trimmed();
        if (value.size() >= 2 && value.startsWith(QLatin1Char('/')) && value.endsWith(QLatin1Char('/'))) {
            QRegularExpression regex(value.mid(1, value.size() - 2));
            if (!regex.isValid()) {
                m_error = QStringLiteral("invalid regex in '[ ]': %1").arg(regex.errorString());
                return nullptr;
            }
            return std::make_shared<const PropertyNode>(key, std::nullopt, regex, true);
        }
        if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
            value = value.mid(1, value.size() - 2);
        return std::make_shared<const PropertyNode>(key, value, QRegularExpression(), false);
    }

    NodePtr parseFieldValueAsQuery()
    {
        skipSpace();
        if (atEnd()) {
            m_error = QStringLiteral("expected a value after ':' at position %1").arg(m_pos);
            return nullptr;
        }
        if (peek() == QLatin1Char('(')) {
            ++m_pos;
            skipSpace();
            NodePtr inner = parseOr();
            if (!inner)
                return nullptr;
            skipSpace();
            if (peek() != QLatin1Char(')')) {
                m_error = QStringLiteral("expected ')' at position %1").arg(m_pos);
                return nullptr;
            }
            ++m_pos;
            return inner;
        }
        if (peek() == QLatin1Char('"'))
            return parsePhraseNode();
        if (peek() == QLatin1Char('/'))
            return parseRegexNode();
        return parseWordNode();
    }

    NodePtr parseScopeField(ScopeNode::Kind kind)
    {
        NodePtr inner = parseFieldValueAsQuery();
        if (!inner)
            return nullptr;
        return std::make_shared<const ScopeNode>(kind, inner);
    }

    NodePtr parseCaseField(Qt::CaseSensitivity forced)
    {
        NodePtr inner = parseFieldValueAsQuery();
        if (!inner)
            return nullptr;
        return std::make_shared<const CaseOverrideNode>(inner, forced);
    }

    /// A leaf value only: word, "phrase" or /regex/, no groups and no boolean
    /// composition. Nothing in Obsidian's docs shows `path:(a OR b)`, and a
    /// path or a tag is a single string to test, not a document to search.
    bool parseLeafValue(QString *literal, QRegularExpression *regex, bool *isRegex)
    {
        skipSpace();
        if (peek() == QLatin1Char('"')) {
            *literal = readQuoted();
            *isRegex = false;
            return !failed();
        }
        if (peek() == QLatin1Char('/')) {
            const QString pattern = readRegexPattern();
            if (failed())
                return false;
            *regex = QRegularExpression(pattern);
            if (!regex->isValid()) {
                m_error = QStringLiteral("bad regular expression: %1").arg(regex->errorString());
                return false;
            }
            *isRegex = true;
            return true;
        }
        *literal = readBareWord();
        *isRegex = false;
        if (literal->isEmpty()) {
            m_error = QStringLiteral("expected a value after ':' at position %1").arg(m_pos);
            return false;
        }
        return true;
    }

    NodePtr parsePathLikeField(PathLikeNode::Source source)
    {
        QString literal;
        QRegularExpression regex;
        bool isRegex = false;
        if (!parseLeafValue(&literal, &regex, &isRegex))
            return nullptr;
        return std::make_shared<const PathLikeNode>(source, literal, regex, isRegex);
    }

    NodePtr parseTagField()
    {
        QString literal;
        QRegularExpression regex;
        bool isRegex = false;
        if (!parseLeafValue(&literal, &regex, &isRegex))
            return nullptr;
        if (!isRegex && literal.startsWith(QLatin1Char('#')))
            literal.remove(0, 1);
        return std::make_shared<const TagNode>(literal, regex, isRegex);
    }

    NodePtr parseWordNode()
    {
        const QString word = readBareWord();
        if (word.isEmpty()) {
            m_error = QStringLiteral("expected a search term at position %1").arg(m_pos);
            return nullptr;
        }
        return std::make_shared<const LiteralNode>(word);
    }

    NodePtr parsePhraseNode()
    {
        const QString phrase = readQuoted();
        if (failed())
            return nullptr;
        return std::make_shared<const LiteralNode>(phrase);
    }

    NodePtr parseRegexNode()
    {
        const QString pattern = readRegexPattern();
        if (failed())
            return nullptr;
        QRegularExpression regex(pattern);
        if (!regex.isValid()) {
            m_error = QStringLiteral("bad regular expression: %1").arg(regex.errorString());
            return nullptr;
        }
        return std::make_shared<const RegexNode>(regex);
    }

    QString readBareWord()
    {
        const qsizetype start = m_pos;
        while (!atEnd()) {
            const QChar c = m_text.at(m_pos);
            if (c.isSpace() || c == QLatin1Char('(') || c == QLatin1Char(')'))
                break;
            ++m_pos;
        }
        return m_text.mid(start, m_pos - start);
    }

    /// Assumes peek() == '"'. Supports \" the way the docs' example does.
    QString readQuoted()
    {
        ++m_pos;
        QString out;
        while (!atEnd()) {
            const QChar c = m_text.at(m_pos);
            if (c == QLatin1Char('\\') && m_pos + 1 < m_text.size()
                && m_text.at(m_pos + 1) == QLatin1Char('"')) {
                out += QLatin1Char('"');
                m_pos += 2;
                continue;
            }
            if (c == QLatin1Char('"')) {
                ++m_pos;
                return out;
            }
            out += c;
            ++m_pos;
        }
        m_error = QStringLiteral("unterminated quote at position %1").arg(m_pos);
        return out;
    }

    /// Assumes peek() == '/'. `\/` stays escaped in the returned pattern (the
    /// regex engine treats it as a literal slash too), so it does not end the
    /// pattern early.
    QString readRegexPattern()
    {
        ++m_pos;
        QString out;
        while (!atEnd()) {
            const QChar c = m_text.at(m_pos);
            if (c == QLatin1Char('\\') && m_pos + 1 < m_text.size()) {
                out += c;
                out += m_text.at(m_pos + 1);
                m_pos += 2;
                continue;
            }
            if (c == QLatin1Char('/')) {
                ++m_pos;
                return out;
            }
            out += c;
            ++m_pos;
        }
        m_error = QStringLiteral("unterminated regular expression at position %1").arg(m_pos);
        return out;
    }
};

} // namespace

SearchQuery SearchQuery::parse(const QString &text)
{
    SearchQuery query;
    QueryParser parser(text);
    QString error;
    NodePtr root = parser.parse(&error);
    if (!root) {
        query.m_error = error;
        return query;
    }
    query.m_root = root;
    return query;
}

QString SearchQuery::ftsExpression() const
{
    return m_root ? m_root->translateFts() : QString();
}

FieldNeeds SearchQuery::fieldNeeds() const
{
    return m_root ? m_root->fieldNeeds() : FieldNeeds{};
}

SearchOutcome SearchQuery::evaluate(const NoteContext &note, Qt::CaseSensitivity caseSensitivity) const
{
    if (!m_root)
        return {};
    const EvalContext ctx{note, Range{0, note.text.size()}, caseSensitivity};
    return m_root->evaluate(ctx);
}

SearchSnippet buildSnippet(const QString &text, const QList<SearchMatch> &matches, int radius)
{
    qsizetype anchor = 0;
    for (const SearchMatch &match : matches) {
        if (match.length > 0) {
            anchor = match.offset;
            break;
        }
    }

    const qsizetype start = qMax<qsizetype>(0, anchor - radius);
    const qsizetype end = qMin<qsizetype>(text.size(), anchor + radius);
    QString body = text.mid(start, end - start);
    body.replace(QLatin1Char('\n'), QLatin1Char(' ')); // length-preserving: offsets below stay valid

    const QString prefix = start > 0 ? QStringLiteral("…") : QString();

    SearchSnippet result;
    for (const SearchMatch &match : matches) {
        const qsizetype local = match.offset - start;
        if (local < 0 || local + match.length > body.size())
            continue; // this occurrence falls outside the window we kept
        result.highlights.append({local + prefix.size(), match.length});
    }
    result.text = prefix + body + (end < text.size() ? QStringLiteral("…") : QString());
    return result;
}

} // namespace noteAppC
