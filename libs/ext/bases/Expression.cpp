#include "Expression.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

namespace noteAppC::bases {

namespace {

// --- tokenizer -----------------------------------------------------------

struct Token {
    enum class Kind { Number, String, Ident, Op, LParen, RParen, LBracket, RBracket, Comma, Dot, End };
    Kind kind = Kind::End;
    QString text;
    double number = 0.0;
};

bool tokenize(const QString &source, QList<Token> &out, QString *error)
{
    qsizetype i = 0;
    const qsizetype n = source.size();
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    while (i < n) {
        const QChar c = source.at(i);
        if (c.isSpace()) { ++i; continue; }
        if (c.isDigit()) {
            // A '.' is only ever part of the literal when a digit follows it,
            // so "2.toFixed()" tokenizes as a number then a '.' - the postfix
            // parser's own job - rather than swallowing the dot here.
            qsizetype start = i;
            while (i < n && source.at(i).isDigit())
                ++i;
            if (i < n && source.at(i) == u'.' && i + 1 < n && source.at(i + 1).isDigit()) {
                ++i;
                while (i < n && source.at(i).isDigit())
                    ++i;
            }
            Token t; t.kind = Token::Kind::Number;
            t.number = source.mid(start, i - start).toDouble();
            out << t;
            continue;
        }
        if (c == u'"' || c == u'\'') {
            const QChar quote = c;
            ++i;
            QString value;
            bool closed = false;
            while (i < n) {
                const QChar ch = source.at(i);
                if (ch == quote) { closed = true; ++i; break; }
                if (ch == u'\\' && i + 1 < n) {
                    const QChar next = source.at(i + 1);
                    if (next == u'n') value += u'\n';
                    else if (next == u't') value += u'\t';
                    else value += next; // \\ \" \' and anything else: literal
                    i += 2;
                    continue;
                }
                value += ch;
                ++i;
            }
            if (!closed)
                return fail(QStringLiteral("unterminated string literal"));
            Token t; t.kind = Token::Kind::String; t.text = value;
            out << t;
            continue;
        }
        if (c.isLetter() || c == u'_') {
            qsizetype start = i;
            while (i < n && (source.at(i).isLetterOrNumber() || source.at(i) == u'_'))
                ++i;
            Token t; t.kind = Token::Kind::Ident; t.text = source.mid(start, i - start);
            out << t;
            continue;
        }
        // Multi-character operators before single-character ones.
        static const QStringList multi = {QStringLiteral("&&"), QStringLiteral("||"),
                                          QStringLiteral("=="), QStringLiteral("!="),
                                          QStringLiteral(">="), QStringLiteral("<=")};
        bool matchedMulti = false;
        for (const QString &op : multi) {
            if (source.mid(i, op.size()) == op) {
                Token t; t.kind = Token::Kind::Op; t.text = op;
                out << t;
                i += op.size();
                matchedMulti = true;
                break;
            }
        }
        if (matchedMulti)
            continue;
        switch (c.unicode()) {
        case '(': out << Token{Token::Kind::LParen, {}, 0}; ++i; continue;
        case ')': out << Token{Token::Kind::RParen, {}, 0}; ++i; continue;
        case '[': out << Token{Token::Kind::LBracket, {}, 0}; ++i; continue;
        case ']': out << Token{Token::Kind::RBracket, {}, 0}; ++i; continue;
        case ',': out << Token{Token::Kind::Comma, {}, 0}; ++i; continue;
        case '.': out << Token{Token::Kind::Dot, {}, 0}; ++i; continue;
        case '+': case '-': case '*': case '/': case '%':
        case '!': case '>': case '<':
            out << Token{Token::Kind::Op, QString(c), 0};
            ++i;
            continue;
        default:
            return fail(QStringLiteral("unexpected character '%1'").arg(c));
        }
    }
    out << Token{Token::Kind::End, {}, 0};
    return true;
}

} // namespace

// --- AST -------------------------------------------------------------------

struct Expression::Node {
    enum class Kind { Literal, Path, Unary, Binary, Call, List };
    Kind kind = Kind::Literal;
    Value literal;
    QStringList path;
    QString op;
    QString name;
    std::unique_ptr<Node> a;
    std::unique_ptr<Node> b;
    std::vector<std::unique_ptr<Node>> args;
};

using Node = Expression::Node;

namespace {

// --- parser ----------------------------------------------------------------

// An expression is a vault's text. The parser, the evaluator and the tree's destructor all recurse once per level,
// so what a formula may nest, and how long it may be, is capped well under what a stack holds.
constexpr int kMaxNesting = 200;
constexpr qsizetype kMaxTokens = 4000;

struct DepthGuard {
    explicit DepthGuard(int &depth) : m_depth(depth) { ++m_depth; }
    ~DepthGuard() { --m_depth; }
    DepthGuard(const DepthGuard &) = delete;
    DepthGuard &operator=(const DepthGuard &) = delete;
    int &m_depth;
};

class Parser {
public:
    Parser(const QList<Token> &tokens, QString *error) : m_tokens(tokens), m_error(error) {}

    std::unique_ptr<Node> parseTop()
    {
        auto node = parseOr();
        if (!node)
            return nullptr;
        if (!check(Token::Kind::End)) {
            fail(QStringLiteral("unexpected token after expression"));
            return nullptr;
        }
        return node;
    }

    QStringList formulaRefs;

private:
    const QList<Token> &m_tokens;
    QString *m_error;
    qsizetype m_pos = 0;
    int m_depth = 0;
    bool m_failed = false;

    void fail(const QString &message)
    {
        if (!m_failed && m_error)
            *m_error = message;
        m_failed = true;
    }

    const Token &peek() const { return m_tokens.at(m_pos); }
    bool check(Token::Kind kind) const { return !m_failed && peek().kind == kind; }
    bool checkOp(const QString &op) const { return check(Token::Kind::Op) && peek().text == op; }

    Token advance()
    {
        const Token &t = m_tokens.at(m_pos);
        if (m_pos + 1 < m_tokens.size())
            ++m_pos;
        return t;
    }

    bool expect(Token::Kind kind, const char *what)
    {
        if (!check(kind)) {
            fail(QStringLiteral("expected %1").arg(QString::fromLatin1(what)));
            return false;
        }
        advance();
        return true;
    }

    static std::unique_ptr<Node> makeLiteral(Value v)
    {
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::Literal;
        node->literal = std::move(v);
        return node;
    }

    std::unique_ptr<Node> parseOr()
    {
        const DepthGuard guard(m_depth);
        if (m_depth > kMaxNesting) {
            fail(QStringLiteral("expression nested too deeply"));
            return nullptr;
        }
        auto node = parseAnd();
        while (!m_failed && checkOp(QStringLiteral("||"))) {
            advance();
            auto rhs = parseAnd();
            if (m_failed) return nullptr;
            auto bin = std::make_unique<Node>();
            bin->kind = Node::Kind::Binary; bin->op = QStringLiteral("||");
            bin->a = std::move(node); bin->b = std::move(rhs);
            node = std::move(bin);
        }
        return m_failed ? nullptr : std::move(node);
    }

    std::unique_ptr<Node> parseAnd()
    {
        auto node = parseComparison();
        while (!m_failed && checkOp(QStringLiteral("&&"))) {
            advance();
            auto rhs = parseComparison();
            if (m_failed) return nullptr;
            auto bin = std::make_unique<Node>();
            bin->kind = Node::Kind::Binary; bin->op = QStringLiteral("&&");
            bin->a = std::move(node); bin->b = std::move(rhs);
            node = std::move(bin);
        }
        return m_failed ? nullptr : std::move(node);
    }

    std::unique_ptr<Node> parseComparison()
    {
        auto node = parseAdditive();
        if (m_failed) return nullptr;
        static const QStringList ops = {QStringLiteral("=="), QStringLiteral("!="),
                                        QStringLiteral(">="), QStringLiteral("<="),
                                        QStringLiteral(">"), QStringLiteral("<")};
        if (check(Token::Kind::Op) && ops.contains(peek().text)) {
            const QString op = advance().text;
            auto rhs = parseAdditive();
            if (m_failed) return nullptr;
            auto bin = std::make_unique<Node>();
            bin->kind = Node::Kind::Binary; bin->op = op;
            bin->a = std::move(node); bin->b = std::move(rhs);
            return bin;
        }
        return node;
    }

    std::unique_ptr<Node> parseAdditive()
    {
        auto node = parseMultiplicative();
        while (!m_failed && check(Token::Kind::Op)
               && (peek().text == QLatin1String("+") || peek().text == QLatin1String("-"))) {
            const QString op = advance().text;
            auto rhs = parseMultiplicative();
            if (m_failed) return nullptr;
            auto bin = std::make_unique<Node>();
            bin->kind = Node::Kind::Binary; bin->op = op;
            bin->a = std::move(node); bin->b = std::move(rhs);
            node = std::move(bin);
        }
        return m_failed ? nullptr : std::move(node);
    }

    std::unique_ptr<Node> parseMultiplicative()
    {
        auto node = parseUnary();
        while (!m_failed && check(Token::Kind::Op)
               && (peek().text == QLatin1String("*") || peek().text == QLatin1String("/")
                   || peek().text == QLatin1String("%"))) {
            const QString op = advance().text;
            auto rhs = parseUnary();
            if (m_failed) return nullptr;
            auto bin = std::make_unique<Node>();
            bin->kind = Node::Kind::Binary; bin->op = op;
            bin->a = std::move(node); bin->b = std::move(rhs);
            node = std::move(bin);
        }
        return m_failed ? nullptr : std::move(node);
    }

    std::unique_ptr<Node> parseUnary()
    {
        const DepthGuard guard(m_depth);
        if (m_depth > kMaxNesting) {
            fail(QStringLiteral("expression nested too deeply"));
            return nullptr;
        }
        if (checkOp(QStringLiteral("!")) || checkOp(QStringLiteral("-"))) {
            const QString op = advance().text;
            auto operand = parseUnary();
            if (m_failed) return nullptr;
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::Unary; node->op = op; node->a = std::move(operand);
            return node;
        }
        return parsePostfix();
    }

    bool parseArgsInto(std::vector<std::unique_ptr<Node>> &args)
    {
        if (check(Token::Kind::RParen))
            return true;
        for (;;) {
            auto arg = parseOr();
            if (m_failed) return false;
            args.push_back(std::move(arg));
            if (check(Token::Kind::Comma)) { advance(); continue; }
            break;
        }
        return true;
    }

    std::unique_ptr<Node> parsePrimary()
    {
        if (check(Token::Kind::Number)) {
            return makeLiteral(Value::ofNumber(advance().number));
        }
        if (check(Token::Kind::String)) {
            return makeLiteral(Value::ofString(advance().text));
        }
        if (check(Token::Kind::Ident) && peek().text == QLatin1String("true")) {
            advance();
            return makeLiteral(Value::ofBool(true));
        }
        if (check(Token::Kind::Ident) && peek().text == QLatin1String("false")) {
            advance();
            return makeLiteral(Value::ofBool(false));
        }
        if (check(Token::Kind::LParen)) {
            advance();
            auto inner = parseOr();
            if (m_failed) return nullptr;
            if (!expect(Token::Kind::RParen, "')'")) return nullptr;
            return inner;
        }
        if (check(Token::Kind::LBracket)) { // a list literal, [1, "a", x]
            advance();
            auto list = std::make_unique<Node>();
            list->kind = Node::Kind::List;
            while (!check(Token::Kind::RBracket)) {
                list->args.push_back(parseOr());
                if (m_failed) return nullptr;
                if (!check(Token::Kind::Comma))
                    break;
                advance();
            }
            if (!expect(Token::Kind::RBracket, "']'")) return nullptr;
            return list;
        }
        if (check(Token::Kind::Ident)) {
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::Path;
            node->path << advance().text;
            return node;
        }
        fail(QStringLiteral("expected a value"));
        return nullptr;
    }

    std::unique_ptr<Node> parsePostfix()
    {
        auto node = parsePrimary();
        if (m_failed) return nullptr;
        for (;;) {
            if (check(Token::Kind::LParen) && node->kind == Node::Kind::Path
                && node->path.size() == 1) {
                advance();
                auto call = std::make_unique<Node>();
                call->kind = Node::Kind::Call;
                call->name = node->path.first();
                if (!parseArgsInto(call->args)) return nullptr;
                if (!expect(Token::Kind::RParen, "')'")) return nullptr;
                node = std::move(call);
                continue;
            }
            if (check(Token::Kind::Dot)) {
                advance();
                if (!check(Token::Kind::Ident)) { fail(QStringLiteral("expected a name after '.'")); return nullptr; }
                const QString ident = advance().text;
                if (check(Token::Kind::LParen)) {
                    advance();
                    auto call = std::make_unique<Node>();
                    call->kind = Node::Kind::Call;
                    call->name = ident;
                    call->a = std::move(node);
                    if (!parseArgsInto(call->args)) return nullptr;
                    if (!expect(Token::Kind::RParen, "')'")) return nullptr;
                    node = std::move(call);
                } else if (node->kind == Node::Kind::Path) {
                    node->path << ident;
                    if (node->path.size() == 2 && node->path.first() == QLatin1String("formula"))
                        formulaRefs << node->path.last();
                } else {
                    fail(QStringLiteral("cannot access '%1' on a computed value").arg(ident));
                    return nullptr;
                }
                continue;
            }
            if (check(Token::Kind::LBracket)) {
                advance();
                if (!check(Token::Kind::String)) { fail(QStringLiteral("expected a quoted key after '['")); return nullptr; }
                const QString key = advance().text;
                if (!expect(Token::Kind::RBracket, "']'")) return nullptr;
                if (node->kind == Node::Kind::Path) {
                    node->path << key;
                    if (node->path.size() == 2 && node->path.first() == QLatin1String("formula"))
                        formulaRefs << node->path.last();
                } else {
                    fail(QStringLiteral("'[' is not valid on a computed value"));
                    return nullptr;
                }
                continue;
            }
            break;
        }
        return m_failed ? nullptr : std::move(node);
    }
};

// --- duration strings, e.g. "2w", "1M", "7d", "1 week" ---------------------

struct Duration {
    int years = 0, months = 0, weeks = 0, days = 0, hours = 0, minutes = 0, seconds = 0;
};

std::optional<Duration> parseDuration(const QString &text)
{
    static const QRegularExpression shorthand(QStringLiteral("^\\s*(\\d+)\\s*([yMwdhms])\\s*$"));
    static const QRegularExpression worded(
        QStringLiteral("^\\s*(\\d+)\\s*(year|month|week|day|hour|minute|second)s?\\s*$"),
        QRegularExpression::CaseInsensitiveOption);

    Duration d;
    if (auto m = shorthand.match(text); m.hasMatch()) {
        const int n = m.captured(1).toInt();
        const QChar unit = m.captured(2).at(0);
        switch (unit.unicode()) {
        case 'y': d.years = n; break;
        case 'M': d.months = n; break;
        case 'w': d.weeks = n; break;
        case 'd': d.days = n; break;
        case 'h': d.hours = n; break;
        case 'm': d.minutes = n; break;
        case 's': d.seconds = n; break;
        }
        return d;
    }
    if (auto m = worded.match(text); m.hasMatch()) {
        const int n = m.captured(1).toInt();
        const QString unit = m.captured(2).toLower();
        if (unit == QLatin1String("year")) d.years = n;
        else if (unit == QLatin1String("month")) d.months = n;
        else if (unit == QLatin1String("week")) d.weeks = n;
        else if (unit == QLatin1String("day")) d.days = n;
        else if (unit == QLatin1String("hour")) d.hours = n;
        else if (unit == QLatin1String("minute")) d.minutes = n;
        else if (unit == QLatin1String("second")) d.seconds = n;
        return d;
    }
    return std::nullopt;
}

QDateTime applyDuration(const QDateTime &base, const Duration &d, int sign)
{
    QDateTime result = base;
    result = result.addYears(sign * d.years);
    result = result.addMonths(sign * d.months);
    result = result.addDays(sign * (d.weeks * 7 + d.days));
    result = result.addSecs(sign * (qint64(d.hours) * 3600 + qint64(d.minutes) * 60 + d.seconds));
    return result;
}

// --- value helpers -----------------------------------------------------

/// The form `file.hasLink()`, Link equality and `.linksTo()` all compare
/// link targets in: no `./`, no `.md`, case-folded.
QString normaliseLinkTarget(QString target)
{
    target = target.trimmed();
    while (target.startsWith(QLatin1String("./"))) target.remove(0, 2);
    if (target.endsWith(QLatin1String(".md"), Qt::CaseInsensitive)) target.chop(3);
    return target.toCaseFolded();
}
QString basenameOfLink(const QString &target)
{
    const qsizetype slash = target.lastIndexOf(u'/');
    return slash < 0 ? target : target.mid(slash + 1);
}
/// True when `a` and `b` name the same file: by full (normalised) path, or
/// by one being the other's basename - the same "Textbook" ==
/// "Books/Textbook" leniency `file.hasLink()` already gives a bare wikilink
/// target versus a note's full vault path.
bool linkTargetsMatch(const QString &a, const QString &b)
{
    const QString na = normaliseLinkTarget(a), nb = normaliseLinkTarget(b);
    return na == nb || basenameOfLink(na) == nb || na == basenameOfLink(nb);
}

std::optional<double> coerceNumber(const Value &v)
{
    switch (v.type) {
    case Value::Type::Number: return v.number;
    case Value::Type::Bool: return v.boolean ? 1.0 : 0.0;
    case Value::Type::String: {
        bool ok = false;
        const double n = v.text.toDouble(&ok);
        if (ok) return n;
        return std::nullopt;
    }
    default: return std::nullopt;
    }
}

bool valuesEqual(const Value &a, const Value &b)
{
    if (a.type == Value::Type::Empty || b.type == Value::Type::Empty)
        return a.type == b.type;
    if (a.type == b.type) {
        switch (a.type) {
        case Value::Type::Bool: return a.boolean == b.boolean;
        case Value::Type::Number: return a.number == b.number;
        case Value::Type::String: return a.text == b.text;
        case Value::Type::Date: return a.date == b.date;
        case Value::Type::List: return a.strings() == b.strings();
        // "equivalent as long as they point to the same file, or ... their
        // link text must be identical" (obsidian.md/help/bases/syntax).
        case Value::Type::Link: return linkTargetsMatch(a.text, b.text);
        default: return true;
        }
    }
    // A Link compared to anything else (a plain string, say) falls through
    // to the display-string fallback below - "Links can be compared to
    // files such as file or this" (both Links here) already goes through
    // the same-type branch above.
    if (const auto na = coerceNumber(a), nb = coerceNumber(b); na && nb)
        return *na == *nb;
    return a.toDisplayString() == b.toDisplayString();
}

/// -1/0/1, for `< > <= >=`. Unlike equality this refuses a comparison it
/// cannot order rather than falling back to text, since "10" < "9" as text
/// would silently give the wrong answer for what is clearly meant as numbers.
std::optional<int> compareOrder(const Value &a, const Value &b)
{
    if (a.type == b.type) {
        switch (a.type) {
        case Value::Type::Number: return a.number < b.number ? -1 : (a.number > b.number ? 1 : 0);
        case Value::Type::String: return a.text.compare(b.text) < 0 ? -1 : (a.text.compare(b.text) > 0 ? 1 : 0);
        case Value::Type::Date: return a.date < b.date ? -1 : (a.date > b.date ? 1 : 0);
        case Value::Type::Bool: return int(a.boolean) - int(b.boolean);
        default: return std::nullopt;
        }
    }
    if (const auto na = coerceNumber(a), nb = coerceNumber(b); na && nb)
        return *na < *nb ? -1 : (*na > *nb ? 1 : 0);
    return std::nullopt;
}

QString titleCase(const QString &s)
{
    QString out = s;
    bool startOfWord = true;
    for (QChar &ch : out) {
        if (ch.isLetter()) {
            ch = startOfWord ? ch.toUpper() : ch.toLower();
            startOfWord = false;
        } else {
            startOfWord = true;
        }
    }
    return out;
}

/// A number out of an expression as an int within [low, high]; NaN is 0. A plain int(number) is
/// undefined for a number that does not fit, and a formula can write any number.
int clampedInt(double value, int low, int high)
{
    return std::isnan(value) ? std::clamp(0, low, high) : int(std::clamp(value, double(low), double(high)));
}

/// The longest string repeat() makes: a formula in a .base file is a vault's, and "x".repeat(1e9) is not a filter.
constexpr qsizetype kMaxRepeatedLength = 1 << 22;

/// Negative indices count from the end, the way `slice()` is documented to.
qsizetype normaliseIndex(double raw, qsizetype length)
{
    // The cast of a number too big for the type (or NaN) is undefined, so the range is applied first.
    qsizetype i = std::isnan(raw) ? 0 : qsizetype(std::clamp(raw, -1e15, 1e15));
    if (i < 0) i += length;
    return std::clamp<qsizetype>(i, 0, length);
}

QString relativeTime(const QDateTime &date, const QDateTime &now)
{
    const qint64 secs = date.secsTo(now);
    const qint64 abs = std::abs(secs);
    const bool past = secs >= 0;
    auto phrase = [past](qint64 n, const char *unit) {
        const QString plural = n == 1 ? QString::fromLatin1(unit)
                                       : QString::fromLatin1(unit) + QStringLiteral("s");
        return past ? QStringLiteral("%1 %2 ago").arg(n).arg(plural)
                    : QStringLiteral("in %1 %2").arg(n).arg(plural);
    };
    if (abs < 60) return QStringLiteral("just now");
    if (abs < 3600) return phrase(abs / 60, "minute");
    if (abs < 86400) return phrase(abs / 3600, "hour");
    if (abs < 86400 * 30) return phrase(abs / 86400, "day");
    if (abs < 86400 * 365) return phrase(abs / (86400 * 30), "month");
    return phrase(abs / (86400 * 365), "year");
}

/// Moment.js and Qt's date tokens agree except for the year and day letters.
QString translateMomentFormat(const QString &format)
{
    QString out = format;
    out.replace(QStringLiteral("YYYY"), QStringLiteral("yyyy"));
    out.replace(QStringLiteral("YY"), QStringLiteral("yy"));
    out.replace(QStringLiteral("DD"), QStringLiteral("dd"));
    // A lone "D" (day of month) would collide with Qt's own "d"; only whole
    // "DD"/"D" tokens as used in the documented examples are translated.
    static const QRegularExpression loneD(QStringLiteral("(?<![a-zA-Z])D(?![a-zA-Z])"));
    out.replace(loneD, QStringLiteral("d"));
    return out;
}

} // namespace

// --- Evaluator ---------------------------------------------------------

namespace {

class Evaluator {
public:
    Evaluator(const Row &row, const FormulaSource *formulas, const QDateTime &now)
        : m_row(row), m_formulas(formulas), m_now(now) {}

    Value run(const Node &node, QString *error)
    {
        m_error = error;
        if (m_error) m_error->clear();
        return eval(node);
    }

private:
    const Row &m_row;
    const FormulaSource *m_formulas;
    QDateTime m_now;
    QString *m_error = nullptr;
public:
    QHash<QString, Value> m_locals; ///< names bound for this evaluation: value, index, acc, values
private:

    bool failed() const { return m_error && !m_error->isEmpty(); }
    Value fail(const QString &message)
    {
        if (m_error && m_error->isEmpty())
            *m_error = message;
        return Value::empty();
    }

    Value eval(const Node &node)
    {
        if (failed()) return Value::empty();
        switch (node.kind) {
        case Node::Kind::Literal: return node.literal;
        case Node::Kind::Path: return resolvePath(node.path);
        case Node::Kind::Unary: return evalUnary(node);
        case Node::Kind::Binary: return evalBinary(node);
        case Node::Kind::Call: return evalCall(node);
        case Node::Kind::List: {
            QList<Value> items;
            items.reserve(qsizetype(node.args.size()));
            for (const auto &item : node.args)
                items << eval(*item);
            return failed() ? Value::empty() : Value::ofValues(std::move(items));
        }
        }
        return Value::empty();
    }

    // --- property resolution --------------------------------------------

    Value resolvePath(const QStringList &path)
    {
        if (path.isEmpty())
            return Value::empty();
        if (path.size() >= 2 && path.last() == QLatin1String("length")) {
            const Value base = resolvePath(path.mid(0, path.size() - 1));
            if (failed()) return Value::empty();
            if (base.type == Value::Type::List) return Value::ofNumber(base.list.size());
            if (base.type == Value::Type::String) return Value::ofNumber(base.text.size());
            return fail(QStringLiteral("'.length' is only valid on a list or string"));
        }
        if (path.size() == 1) {
            const QString &head = path.first();
            // filter/map/reduce's `value`, `index` and `acc`, and a summary's
            // `values`, ahead of any property of the same name.
            if (const auto local = m_locals.constFind(head); local != m_locals.constEnd())
                return *local;
            // A reserved identifier, not a property lookup - "author == this"
            // is how the docs' own link-equality example reads.
            if (head == QLatin1String("this"))
                return Value::ofLink(m_row.notePath, m_row.noteName);
            if (head == QLatin1String("file") || head == QLatin1String("note")
                || head == QLatin1String("formula"))
                return fail(QStringLiteral("'%1' is not a value by itself").arg(head));
            return m_row.properties.value(head.toCaseFolded(), Value::empty());
        }
        if (path.size() == 2) {
            const QString &scope = path.first();
            const QString &key = path.at(1);
            if (scope == QLatin1String("file"))
                return fileProperty(key);
            if (scope == QLatin1String("note"))
                return m_row.properties.value(key.toCaseFolded(), Value::empty());
            if (scope == QLatin1String("formula")) {
                if (!m_formulas)
                    return fail(QStringLiteral("formula.%1: no formulas available here").arg(key));
                QString formulaError;
                Value v = m_formulas->resolveFormula(key, m_row, &formulaError);
                if (!formulaError.isEmpty())
                    return fail(formulaError);
                return v;
            }
            return fail(QStringLiteral("nested property path '%1.%2' is not supported; "
                                       "use note[\"%1.%2\"] instead").arg(scope, key));
        }
        return fail(QStringLiteral("property path is too deep"));
    }

    Value fileProperty(const QString &key)
    {
        if (key == QLatin1String("name")) return Value::ofString(m_row.noteName);
        if (key == QLatin1String("path")) return Value::ofString(m_row.notePath);
        if (key == QLatin1String("folder")) return Value::ofString(m_row.noteFolder);
        if (key == QLatin1String("ext")) return Value::ofString(m_row.noteExt);
        if (key == QLatin1String("size")) return Value::ofNumber(double(m_row.fileSize));
        if (key == QLatin1String("ctime"))
            return m_row.fileCtime.isValid() ? Value::ofDate(m_row.fileCtime) : Value::empty();
        if (key == QLatin1String("mtime")) return Value::ofDate(m_row.fileMtime);
        if (key == QLatin1String("tags")) return Value::ofList(m_row.tags);
        if (key == QLatin1String("links")) {
            QList<Value> items;
            items.reserve(m_row.links.size());
            for (const QString &target : m_row.links)
                items << Value::ofLink(target);
            return Value::ofValues(std::move(items));
        }
        return fail(QStringLiteral("unknown file property 'file.%1'").arg(key));
    }

    // --- operators ---------------------------------------------------------

    Value evalUnary(const Node &node)
    {
        const Value v = eval(*node.a);
        if (failed()) return Value::empty();
        if (node.op == QLatin1String("!"))
            return Value::ofBool(!v.truthy());
        if (node.op == QLatin1String("-")) {
            const auto n = coerceNumber(v);
            if (!n) return fail(QStringLiteral("cannot negate a %1").arg(Value::typeName(v.type)));
            return Value::ofNumber(-*n);
        }
        return fail(QStringLiteral("unknown unary operator '%1'").arg(node.op));
    }

    Value evalBinary(const Node &node)
    {
        if (node.op == QLatin1String("&&")) {
            const Value a = eval(*node.a);
            if (failed()) return Value::empty();
            if (!a.truthy()) return Value::ofBool(false);
            const Value b = eval(*node.b);
            if (failed()) return Value::empty();
            return Value::ofBool(b.truthy());
        }
        if (node.op == QLatin1String("||")) {
            const Value a = eval(*node.a);
            if (failed()) return Value::empty();
            if (a.truthy()) return Value::ofBool(true);
            const Value b = eval(*node.b);
            if (failed()) return Value::empty();
            return Value::ofBool(b.truthy());
        }

        const Value a = eval(*node.a);
        if (failed()) return Value::empty();
        const Value b = eval(*node.b);
        if (failed()) return Value::empty();

        if (node.op == QLatin1String("=="))
            return Value::ofBool(valuesEqual(a, b));
        if (node.op == QLatin1String("!="))
            return Value::ofBool(!valuesEqual(a, b));
        if (node.op == QLatin1String(">") || node.op == QLatin1String("<")
            || node.op == QLatin1String(">=") || node.op == QLatin1String("<=")) {
            const auto cmp = compareOrder(a, b);
            if (!cmp)
                return fail(QStringLiteral("cannot compare a %1 and a %2")
                               .arg(Value::typeName(a.type), Value::typeName(b.type)));
            if (node.op == QLatin1String(">")) return Value::ofBool(*cmp > 0);
            if (node.op == QLatin1String("<")) return Value::ofBool(*cmp < 0);
            if (node.op == QLatin1String(">=")) return Value::ofBool(*cmp >= 0);
            return Value::ofBool(*cmp <= 0);
        }

        if (node.op == QLatin1String("+")) {
            if (a.type == Value::Type::Date && b.type == Value::Type::String) {
                const auto d = parseDuration(b.text);
                if (!d) return fail(QStringLiteral("not a duration: \"%1\"").arg(b.text));
                return Value::ofDate(applyDuration(a.date, *d, +1));
            }
            if (a.type == Value::Type::Number && b.type == Value::Type::Number)
                return Value::ofNumber(a.number + b.number);
            if (a.type == Value::Type::String || b.type == Value::Type::String)
                return Value::ofString(a.toDisplayString() + b.toDisplayString());
            return fail(QStringLiteral("cannot add a %1 and a %2")
                           .arg(Value::typeName(a.type), Value::typeName(b.type)));
        }
        if (node.op == QLatin1String("-")) {
            if (a.type == Value::Type::Date && b.type == Value::Type::String) {
                const auto d = parseDuration(b.text);
                if (!d) return fail(QStringLiteral("not a duration: \"%1\"").arg(b.text));
                return Value::ofDate(applyDuration(a.date, *d, -1));
            }
            if (a.type == Value::Type::Date && b.type == Value::Type::Date)
                return Value::ofNumber(b.date.secsTo(a.date) / 86400.0);
            const auto na = coerceNumber(a), nb = coerceNumber(b);
            if (na && nb) return Value::ofNumber(*na - *nb);
            return fail(QStringLiteral("cannot subtract a %1 and a %2")
                           .arg(Value::typeName(a.type), Value::typeName(b.type)));
        }
        if (node.op == QLatin1String("*") || node.op == QLatin1String("/")
            || node.op == QLatin1String("%")) {
            const auto na = coerceNumber(a), nb = coerceNumber(b);
            if (!na || !nb)
                return fail(QStringLiteral("'%1' needs two numbers, not a %2 and a %3")
                               .arg(node.op, Value::typeName(a.type), Value::typeName(b.type)));
            if (node.op == QLatin1String("*")) return Value::ofNumber(*na * *nb);
            if (*nb == 0.0) return fail(QStringLiteral("division by zero"));
            if (node.op == QLatin1String("/")) return Value::ofNumber(*na / *nb);
            return Value::ofNumber(std::fmod(*na, *nb));
        }
        return fail(QStringLiteral("unknown operator '%1'").arg(node.op));
    }

    // --- calls ---------------------------------------------------------

    QList<Value> evalArgs(const std::vector<std::unique_ptr<Node>> &args)
    {
        QList<Value> values;
        for (const auto &arg : args) {
            values << eval(*arg);
            if (failed()) break;
        }
        return values;
    }

    Value evalCall(const Node &node)
    {
        if (node.a) {
            if (node.a->kind == Node::Kind::Path && node.a->path.size() == 1
                && node.a->path.first() == QLatin1String("file"))
                return evalFileFunction(node.name, node.args);
            const Value receiver = eval(*node.a);
            if (failed()) return Value::empty();
            return dispatchMethod(receiver, node.name, node.args);
        }
        return dispatchGlobal(node.name, node.args);
    }

    Value evalFileFunction(const QString &name, const std::vector<std::unique_ptr<Node>> &argNodes)
    {
        const QList<Value> args = evalArgs(argNodes);
        if (failed()) return Value::empty();
        if (name == QLatin1String("hasTag")) {
            for (const Value &v : args) {
                for (const QString &tag : m_row.tags) {
                    if (tag.compare(v.toDisplayString(), Qt::CaseInsensitive) == 0)
                        return Value::ofBool(true);
                }
            }
            return Value::ofBool(false);
        }
        if (name == QLatin1String("inFolder")) {
            if (args.size() != 1) return fail(QStringLiteral("file.inFolder() takes one argument"));
            QString folder = args.first().toDisplayString();
            while (folder.endsWith(u'/')) folder.chop(1);
            if (folder.isEmpty()) return Value::ofBool(true);
            return Value::ofBool(m_row.noteFolder == folder
                                 || m_row.noteFolder.startsWith(folder + QLatin1Char('/')));
        }
        if (name == QLatin1String("hasLink")) {
            if (args.size() != 1) return fail(QStringLiteral("file.hasLink() takes one argument"));
            const QString wanted = args.first().toDisplayString();
            for (const QString &link : m_row.links) {
                if (linkTargetsMatch(link, wanted))
                    return Value::ofBool(true);
            }
            return Value::ofBool(false);
        }
        if (name == QLatin1String("hasProperty")) {
            if (args.size() != 1) return fail(QStringLiteral("file.hasProperty() takes one argument"));
            return Value::ofBool(m_row.properties.contains(args.first().toDisplayString().toCaseFolded()));
        }
        if (name == QLatin1String("asLink")) {
            if (args.size() > 1) return fail(QStringLiteral("file.asLink() takes at most one argument"));
            return Value::ofLink(m_row.notePath, args.isEmpty() ? m_row.noteName : args.first().toDisplayString());
        }
        return fail(QStringLiteral("unknown function 'file.%1'").arg(name));
    }

    Value dispatchGlobal(const QString &name, const std::vector<std::unique_ptr<Node>> &argNodes)
    {
        if (name == QLatin1String("if")) {
            if (argNodes.size() != 2 && argNodes.size() != 3)
                return fail(QStringLiteral("if() takes 2 or 3 arguments"));
            const Value cond = eval(*argNodes[0]);
            if (failed()) return Value::empty();
            if (cond.truthy()) return eval(*argNodes[1]);
            if (argNodes.size() == 3) return eval(*argNodes[2]);
            return Value::empty();
        }
        if (name == QLatin1String("now")) return Value::ofDate(m_now);
        if (name == QLatin1String("today")) return Value::ofDate(QDateTime(m_now.date(), QTime(0, 0)));

        const QList<Value> args = evalArgs(argNodes);
        if (failed()) return Value::empty();

        if (name == QLatin1String("date")) {
            if (args.size() != 1 || args.first().type != Value::Type::String)
                return fail(QStringLiteral("date() takes one string argument"));
            const QString text = args.first().text;
            QDateTime dt = QDateTime::fromString(text, Qt::ISODate);
            if (!dt.isValid()) dt = QDateTime::fromString(text, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            if (!dt.isValid()) {
                // A date with no time at all - QDateTime::fromString(ISODate)
                // is documented to accept this too, but a plain QDate parse
                // is what Frontmatter.cpp relies on for the same string
                // shape, so the same fallback is used here rather than
                // trusting one parser to cover both.
                const QDate justDate = QDate::fromString(text, Qt::ISODate);
                if (justDate.isValid()) dt = QDateTime(justDate, QTime(0, 0));
            }
            if (!dt.isValid()) return fail(QStringLiteral("not a date: \"%1\"").arg(text));
            return Value::ofDate(dt);
        }
        if (name == QLatin1String("min") || name == QLatin1String("max")) {
            if (args.isEmpty()) return fail(QStringLiteral("%1() takes at least one argument").arg(name));
            double best = 0; bool have = false;
            for (const Value &v : args) {
                const auto n = coerceNumber(v);
                if (!n) return fail(QStringLiteral("%1() needs numbers").arg(name));
                if (!have || (name == QLatin1String("min") ? *n < best : *n > best)) { best = *n; have = true; }
            }
            return Value::ofNumber(best);
        }
        if (name == QLatin1String("number")) {
            if (args.size() != 1) return fail(QStringLiteral("number() takes one argument"));
            const auto n = coerceNumber(args.first());
            return n ? Value::ofNumber(*n) : Value::empty();
        }
        if (name == QLatin1String("link")) {
            if (args.isEmpty() || args.size() > 2)
                return fail(QStringLiteral("link() takes one or two arguments"));
            return Value::ofLink(args.first().toDisplayString(),
                                 args.size() > 1 ? args.at(1).toDisplayString() : QString());
        }
        return fail(QStringLiteral("unknown function '%1'").arg(name));
    }

    Value dispatchMethod(const Value &receiver, const QString &name,
                         const std::vector<std::unique_ptr<Node>> &argNodes)
    {
        // Universal, regardless of the receiver's type.
        if (name == QLatin1String("isEmpty")) return Value::ofBool(isEmptyValue(receiver));
        if (name == QLatin1String("isTruthy")) return Value::ofBool(receiver.truthy());
        if (name == QLatin1String("toString")) return Value::ofString(receiver.toDisplayString());
        if (name == QLatin1String("isType")) {
            const QList<Value> args = evalArgs(argNodes);
            if (failed()) return Value::empty();
            if (args.size() != 1) return fail(QStringLiteral("isType() takes one argument"));
            return Value::ofBool(Value::typeName(receiver.type).compare(
                                     args.first().toDisplayString(), Qt::CaseInsensitive) == 0);
        }

        if (receiver.type == Value::Type::List
            && (name == QLatin1String("filter") || name == QLatin1String("map") || name == QLatin1String("reduce")))
            return iterateList(receiver.list, name, argNodes);

        const QList<Value> args = evalArgs(argNodes);
        if (failed()) return Value::empty();

        switch (receiver.type) {
        case Value::Type::String: return dispatchString(receiver.text, name, args);
        case Value::Type::Number: return dispatchNumber(receiver.number, name, args);
        case Value::Type::Date: return dispatchDate(receiver.date, name, args);
        case Value::Type::List: return dispatchList(receiver.list, name, args);
        case Value::Type::Link: return dispatchLink(receiver, name, args);
        default:
            return fail(QStringLiteral("no function '%1' for a %2 value")
                           .arg(name, Value::typeName(receiver.type)));
        }
    }

    Value dispatchLink(const Value &link, const QString &name, const QList<Value> &args)
    {
        // File folded into Link (see Value.h): ".asFile()" is identity.
        if (name == QLatin1String("asFile")) return link;
        if (name == QLatin1String("linksTo")) {
            if (!needArgs(args, 1, name)) return Value::empty();
            const QString wanted = args.first().type == Value::Type::Link ? args.first().text
                                                                          : args.first().toDisplayString();
            if (!m_formulas) return Value::ofBool(false); // no data source wired: nothing to check
            for (const QString &outgoing : m_formulas->outgoingLinksOf(link.text)) {
                if (linkTargetsMatch(outgoing, wanted))
                    return Value::ofBool(true);
            }
            return Value::ofBool(false);
        }
        return fail(QStringLiteral("no function 'link.%1'").arg(name));
    }

    static bool isEmptyValue(const Value &v)
    {
        switch (v.type) {
        case Value::Type::Empty: return true;
        case Value::Type::Bool: return false;
        case Value::Type::Number: return false;
        case Value::Type::String: return v.text.isEmpty();
        case Value::Type::Date: return !v.date.isValid();
        case Value::Type::List: return v.list.isEmpty();
        case Value::Type::Link: return v.text.isEmpty();
        }
        return true;
    }

    Value dispatchString(const QString &text, const QString &name, const QList<Value> &args)
    {
        if (name == QLatin1String("contains"))
            return needArgs(args, 1, name) ? Value::ofBool(text.contains(args[0].toDisplayString())) : Value::empty();
        if (name == QLatin1String("containsAll")) {
            for (const Value &v : args) if (!text.contains(v.toDisplayString())) return Value::ofBool(false);
            return Value::ofBool(true);
        }
        if (name == QLatin1String("containsAny")) {
            for (const Value &v : args) if (text.contains(v.toDisplayString())) return Value::ofBool(true);
            return Value::ofBool(false);
        }
        if (name == QLatin1String("endsWith"))
            return needArgs(args, 1, name) ? Value::ofBool(text.endsWith(args[0].toDisplayString())) : Value::empty();
        if (name == QLatin1String("startsWith"))
            return needArgs(args, 1, name) ? Value::ofBool(text.startsWith(args[0].toDisplayString())) : Value::empty();
        if (name == QLatin1String("lower")) return Value::ofString(text.toLower());
        if (name == QLatin1String("title")) return Value::ofString(titleCase(text));
        if (name == QLatin1String("trim")) return Value::ofString(text.trimmed());
        if (name == QLatin1String("reverse")) {
            QString r = text;
            std::reverse(r.begin(), r.end());
            return Value::ofString(r);
        }
        if (name == QLatin1String("repeat")) {
            if (!needArgs(args, 1, name)) return Value::empty();
            const int times = clampedInt(args[0].number, 0, std::numeric_limits<int>::max());
            if (!text.isEmpty() && times > kMaxRepeatedLength / text.size())
                return fail(QStringLiteral("repeat() would make a string over %1 characters").arg(kMaxRepeatedLength));
            return Value::ofString(text.repeated(times));
        }
        if (name == QLatin1String("replace")) {
            if (!needArgs(args, 2, name)) return Value::empty();
            QString r = text;
            return Value::ofString(r.replace(args[0].toDisplayString(), args[1].toDisplayString()));
        }
        if (name == QLatin1String("slice")) {
            if (args.isEmpty()) return fail(QStringLiteral("slice() takes at least one argument"));
            const qsizetype start = normaliseIndex(args[0].number, text.size());
            const qsizetype end = args.size() > 1 ? normaliseIndex(args[1].number, text.size()) : text.size();
            return Value::ofString(end > start ? text.mid(start, end - start) : QString());
        }
        if (name == QLatin1String("split")) {
            if (!needArgs(args, 1, name)) return Value::empty();
            return Value::ofList(text.split(args[0].toDisplayString()));
        }
        return fail(QStringLiteral("no function 'string.%1'").arg(name));
    }

    Value dispatchNumber(double number, const QString &name, const QList<Value> &args)
    {
        if (name == QLatin1String("abs")) return Value::ofNumber(std::abs(number));
        if (name == QLatin1String("ceil")) return Value::ofNumber(std::ceil(number));
        if (name == QLatin1String("floor")) return Value::ofNumber(std::floor(number));
        if (name == QLatin1String("round")) {
            const int digits = args.isEmpty() ? 0 : clampedInt(args[0].number, -20, 20);
            const double factor = std::pow(10, digits);
            return Value::ofNumber(std::round(number * factor) / factor);
        }
        if (name == QLatin1String("toFixed")) {
            const int precision = needArgs(args, 1, name) ? clampedInt(args[0].number, 0, 100) : 0; // JavaScript's range
            return Value::ofString(QString::number(number, 'f', precision));
        }
        return fail(QStringLiteral("no function 'number.%1'").arg(name));
    }

    Value dispatchDate(const QDateTime &date, const QString &name, const QList<Value> &args)
    {
        if (name == QLatin1String("date")) return Value::ofDate(QDateTime(date.date(), QTime(0, 0)));
        if (name == QLatin1String("time")) return Value::ofString(date.time().toString(QStringLiteral("HH:mm:ss")));
        if (name == QLatin1String("relative")) return Value::ofString(relativeTime(date, m_now));
        if (name == QLatin1String("format")) {
            if (!needArgs(args, 1, name)) return Value::empty();
            return Value::ofString(date.toString(translateMomentFormat(args[0].toDisplayString())));
        }
        return fail(QStringLiteral("no function 'date.%1'").arg(name));
    }

    Value dispatchList(const QList<Value> &list, const QString &name, const QList<Value> &args)
    {
        // Membership compares by value (valuesEqual): a tag "2" and a number
        // 2 are still the same entry (both stringify the same when the types
        // differ), and a list of Links checks `.contains(this)` by the file
        // it resolves to - "authors.contains(this)" is the docs' own example
        // - not by display text, which an alias could give a different value.
        const auto has = [&list](const Value &needle) {
            for (const Value &item : list) if (valuesEqual(item, needle)) return true;
            return false;
        };
        if (name == QLatin1String("contains"))
            return needArgs(args, 1, name) ? Value::ofBool(has(args[0])) : Value::empty();
        if (name == QLatin1String("containsAll")) {
            for (const Value &v : args) if (!has(v)) return Value::ofBool(false);
            return Value::ofBool(true);
        }
        if (name == QLatin1String("containsAny")) {
            for (const Value &v : args) if (has(v)) return Value::ofBool(true);
            return Value::ofBool(false);
        }
        const QStringList text = Value::ofValues(list).strings();
        if (name == QLatin1String("join"))
            return needArgs(args, 1, name) ? Value::ofString(text.join(args[0].toDisplayString()))
                                           : Value::empty();
        if (name == QLatin1String("reverse")) {
            QList<Value> r = list;
            std::reverse(r.begin(), r.end());
            return Value::ofValues(r);
        }
        if (name == QLatin1String("sort")) {
            QList<Value> r = list;
            std::stable_sort(r.begin(), r.end(), [](const Value &a, const Value &b) { return compareForSort(a, b) < 0; });
            return Value::ofValues(r);
        }
        if (name == QLatin1String("unique")) {
            QList<Value> r;
            QStringList seen;
            for (const Value &v : list) {
                const QString key = v.toDisplayString();
                if (!seen.contains(key)) { seen << key; r << v; }
            }
            return Value::ofValues(r);
        }
        if (name == QLatin1String("flat")) {
            QList<Value> r;
            for (const Value &v : list) {
                if (v.type == Value::Type::List) r << v.list; else r << v;
            }
            return Value::ofValues(r);
        }
        if (name == QLatin1String("slice")) {
            if (args.isEmpty()) return fail(QStringLiteral("slice() takes at least one argument"));
            const qsizetype start = normaliseIndex(args[0].number, list.size());
            const qsizetype end = args.size() > 1 ? normaliseIndex(args[1].number, list.size()) : list.size();
            return Value::ofValues(end > start ? list.mid(start, end - start) : QList<Value>());
        }
        // Not in Obsidian's list reference, but its own summary example
        // reads `values.mean()`: the numeric aggregates a summary wants.
        if (name == QLatin1String("mean") || name == QLatin1String("sum") || name == QLatin1String("min")
            || name == QLatin1String("max")) {
            QList<double> numbers;
            for (const Value &v : list) {
                if (const auto n = coerceNumber(v)) numbers << *n;
            }
            if (numbers.isEmpty()) return Value::empty();
            if (name == QLatin1String("min")) return Value::ofNumber(*std::min_element(numbers.cbegin(), numbers.cend()));
            if (name == QLatin1String("max")) return Value::ofNumber(*std::max_element(numbers.cbegin(), numbers.cend()));
            const double sum = std::accumulate(numbers.cbegin(), numbers.cend(), 0.0);
            return Value::ofNumber(name == QLatin1String("sum") ? sum : sum / double(numbers.size()));
        }
        return fail(QStringLiteral("no function 'list.%1'").arg(name));
    }

    /// filter/map/reduce: the argument is an expression evaluated once per
    /// element, with `value` and `index` (and reduce's `acc`) bound.
    Value iterateList(const QList<Value> &list, const QString &name,
                      const std::vector<std::unique_ptr<Node>> &argNodes)
    {
        const bool reduce = name == QLatin1String("reduce");
        if (argNodes.size() != (reduce ? 2u : 1u))
            return fail(QStringLiteral("%1() takes %2 argument(s)").arg(name).arg(reduce ? 2 : 1));
        const QHash<QString, Value> outer = m_locals; // nested calls restore what they shadow
        Value acc;
        if (reduce) {
            acc = eval(*argNodes[1]);
            if (failed()) return Value::empty();
        }
        QList<Value> out;
        for (qsizetype i = 0; i < list.size() && !failed(); ++i) {
            m_locals.insert(QStringLiteral("value"), list.at(i));
            m_locals.insert(QStringLiteral("index"), Value::ofNumber(double(i)));
            if (reduce)
                m_locals.insert(QStringLiteral("acc"), acc);
            const Value result = eval(*argNodes[0]);
            if (name == QLatin1String("filter")) {
                if (result.truthy()) out << list.at(i);
            } else if (name == QLatin1String("map")) {
                out << result;
            } else {
                acc = result;
            }
        }
        m_locals = outer;
        if (failed()) return Value::empty();
        return reduce ? acc : Value::ofValues(out);
    }

    bool needArgs(const QList<Value> &args, int count, const QString &name)
    {
        if (args.size() < count) {
            fail(QStringLiteral("%1() takes %2 argument(s)").arg(name).arg(count));
            return false;
        }
        return true;
    }
};

} // namespace

// --- Expression --------------------------------------------------------

Expression::Expression(std::unique_ptr<Node> root, QStringList formulaRefs)
    : m_root(std::move(root)), m_formulaRefs(std::move(formulaRefs))
{
    m_formulaRefs.removeDuplicates();
}

Expression::~Expression() = default;

std::unique_ptr<Expression> Expression::parse(const QString &source, QString *error)
{
    QList<Token> tokens;
    QString localError;
    if (!tokenize(source, tokens, &localError)) {
        if (error) *error = localError;
        return nullptr;
    }
    if (tokens.size() > kMaxTokens) {
        if (error) *error = QStringLiteral("expression too long");
        return nullptr;
    }
    Parser parser(tokens, &localError);
    auto root = parser.parseTop();
    if (!root) {
        if (error) *error = localError.isEmpty() ? QStringLiteral("invalid expression") : localError;
        return nullptr;
    }
    return std::unique_ptr<Expression>(new Expression(std::move(root), parser.formulaRefs));
}

Value Expression::evaluate(const Row &row, const FormulaSource *formulas, const QDateTime &now,
                           QString *error) const
{
    return evaluate(row, formulas, now, error, {});
}

Value Expression::evaluate(const Row &row, const FormulaSource *formulas, const QDateTime &now, QString *error,
                           const QHash<QString, Value> &locals) const
{
    Evaluator evaluator(row, formulas, now);
    evaluator.m_locals = locals;
    QString localError;
    Value v = evaluator.run(*m_root, &localError);
    if (error) *error = localError;
    return localError.isEmpty() ? v : Value::empty();
}

} // namespace noteAppC::bases
