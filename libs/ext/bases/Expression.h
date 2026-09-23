#pragma once

#include "Value.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace noteAppC::bases {

/// One row of data an expression can read: a note's file facts and
/// properties, flattened out of libs/index's `Index::BaseNote` (that
/// adaptation is BaseQuery's job, not this module's - Expression itself
/// knows nothing about the index or SQL).
///
/// `fileCtime` is filled lazily by whoever builds the row: a base that never
/// asks for `file.ctime` never pays for the stat() the index does not cache
/// (see the report for why it is not in the index).
struct Row {
    QString noteName;   ///< file.name - basename, no extension
    QString notePath;   ///< file.path - vault-relative
    QString noteFolder;  ///< file.folder
    QString noteExt;     ///< file.ext - without the dot
    qint64 fileSize = 0; ///< file.size, bytes
    QDateTime fileCtime; ///< file.ctime, invalid when not (yet) known
    QDateTime fileMtime; ///< file.mtime
    QStringList tags;    ///< file.tags
    QStringList links;   ///< file.links - each outgoing link's raw target text
    /// note.<key> / bare <key>, keyed case-folded (frontmatter keys are
    /// case-sensitive on write but Bases, like the rest of noteAppC's link
    /// resolution, reads them case-insensitively).
    QHash<QString, Value> properties;
};

/// Resolves `formula.<name>` while an expression is being evaluated for one
/// row. BaseQuery implements this over a `.base` file's `formulas:` map;
/// tests can implement it directly to check formula-to-formula references
/// without a BaseFile at all.
class FormulaSource {
public:
    virtual ~FormulaSource() = default;
    /// Empty + `*error` set on an unknown or circular formula name; empty
    /// with no error for a formula whose own expression evaluated to
    /// Value::empty().
    virtual Value resolveFormula(const QString &name, const Row &row, QString *error) const = 0;

    /// `link.linksTo(file)`'s data: the raw outgoing link targets (the same
    /// shape as `file.links`) of whichever row `linkText` resolves to - by
    /// path or by basename, the same fuzzy match `file.hasLink()` already
    /// uses. Empty when nothing resolves, including when a caller (a plain
    /// FormulaSource in a test, say) never overrides this - `linksTo()` then
    /// answers false rather than failing the whole expression, which is the
    /// right default for "no data source was wired for this".
    virtual QStringList outgoingLinksOf(const QString &linkText) const { Q_UNUSED(linkText); return {}; }
};

/// One parsed expression - property access, literals, arithmetic,
/// comparison, `&&`/`||`/`!`, and calls - compiled once and evaluated many
/// times, which is what keeps a 20 000-row filter or sort under budget.
///
/// Supported, from https://obsidian.md/help/bases/syntax and
/// https://obsidian.md/help/bases/functions:
///  - property access: `note.x` / bare `x`, `note["x"]`, `file.name`,
///    `file.path`, `file.folder`, `file.ext`, `file.size`, `file.ctime`,
///    `file.mtime`, `file.tags`, `file.links`, `formula.x`, `this` (a Link
///    to the row's own file, so `author == this` works);
///  - literals: numbers, `"strings"`/'strings', `true`/`false`;
///  - operators: `+ - * / %`, `== != > < >= <=`, `&& || !`, parentheses;
///  - date arithmetic: `date +/- "duration"` and `date - date`;
///  - global functions: `if` `now` `today` `date` `min` `max` `number`
///    `link(path, display?)`;
///  - any-type: `isTruthy` `isType` `toString`;
///  - string: `contains` `containsAll` `containsAny` `endsWith` `isEmpty`
///    `lower` `replace` `repeat` `reverse` `slice` `split` `startsWith`
///    `title` `trim`;
///  - number: `abs` `ceil` `floor` `isEmpty` `round` `toFixed`;
///  - date: `date` `format` `time` `relative` `isEmpty`;
///  - list: `contains` `containsAll` `containsAny` `isEmpty` `join`
///    `reverse` `slice` `sort` `unique` `flat` `length` (via `.length`,
///    Obsidian's own shorthand for a list's size, seen in the docs' formula
///    examples), `filter(expr)` `map(expr)` `reduce(expr, initial)` with
///    `value`, `index` and `acc` bound, list literals `[1, "a"]`, and
///    `mean` `sum` `min` `max` for a summary's `values`; `contains`/
///    `containsAll`/`containsAny` compare by value, so a list of Links
///    checks `.contains(this)` by the file it resolves to, not its text;
///  - file: `file.hasTag(...)` `file.inFolder(...)` `file.hasLink(...)`
///    `file.hasProperty(...)` `file.asLink(display?)`;
///  - link: a bracketed property value (`[[Note]]`, `[[Note|Alias]]`, or a
///    list of them) is a Link automatically, the same as `file.links`'
///    elements; `==`/`!=` compare by the file they resolve to (falling back
///    to their text when neither side resolves), `.asFile()` (returns the
///    same Link - see Value.h's Type::Link), `.linksTo(file)`.
///
/// Not implemented - each is a clear "unsupported" parse or evaluate error,
/// never a crash: `icon`/`html`/`image` object values and their methods (a
/// Link stands in for Obsidian's separate `file`/`link` split, see Value.h,
/// but icon/html/image are not modelled at all), `object` values,
/// `escapeHTML`, `random`, `file.embeds`/`file.backlinks` (the index this
/// module reads does not carry embeds separately from links, and backlinks
/// - Obsidian's own docs call it "performance heavy" - would need a
/// database query per row, off the thread that owns it; see the module
/// report), regular-expression arguments to `replace`/`split` (a
/// plain-string needle/separator works).
class Expression {
public:
    /// Parses `source`. Null with `*error` filled on a syntax error or a
    /// function/operator this module does not implement.
    static std::unique_ptr<Expression> parse(const QString &source, QString *error = nullptr);

    /// Evaluates against `row`. `formulas` may be null when the expression is
    /// known not to reference any `formula.x` (a plain filter statement,
    /// say); `now` pins `now()`/`today()` for a whole query pass so a filter
    /// and its sort see the same instant and a 20 000-row pass does not make
    /// 20 000 syscalls. Empty `Value` with `*error` set on a runtime error
    /// (an unknown property, a type mismatch); callers treat that as "this
    /// row does not match" / "sorts last", not as a crash.
    Value evaluate(const Row &row, const FormulaSource *formulas, const QDateTime &now,
                   QString *error) const;
    /// The same, with `locals` bound as names ahead of any property: a
    /// summary formula's `values`.
    Value evaluate(const Row &row, const FormulaSource *formulas, const QDateTime &now, QString *error,
                   const QHash<QString, Value> &locals) const;

    /// The `formula.x` names this expression reads, so BaseQuery can detect a
    /// cycle before evaluating anything.
    const QStringList &referencedFormulas() const { return m_formulaRefs; }

    ~Expression();

    /// The parsed tree's node type. Public only so the parser and evaluator
    /// in Expression.cpp - free functions, not members, to keep that file
    /// readable - can name it; its definition is private to that file and
    /// nothing outside this module ever names it.
    struct Node;

private:
    explicit Expression(std::unique_ptr<Node> root, QStringList formulaRefs);
    std::unique_ptr<Node> m_root;
    QStringList m_formulaRefs;
};

} // namespace noteAppC::bases
