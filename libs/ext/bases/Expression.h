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
};

/// One parsed expression - property access, literals, arithmetic,
/// comparison, `&&`/`||`/`!`, and calls - compiled once and evaluated many
/// times, which is what keeps a 20 000-row filter or sort under budget.
///
/// Supported, from https://obsidian.md/help/bases/syntax and
/// https://obsidian.md/help/bases/functions:
///  - property access: `note.x` / bare `x`, `note["x"]`, `file.name`,
///    `file.path`, `file.folder`, `file.ext`, `file.size`, `file.ctime`,
///    `file.mtime`, `file.tags`, `file.links`, `formula.x`;
///  - literals: numbers, `"strings"`/'strings', `true`/`false`;
///  - operators: `+ - * / %`, `== != > < >= <=`, `&& || !`, parentheses;
///  - date arithmetic: `date +/- "duration"` and `date - date`;
///  - global functions: `if` `now` `today` `date` `min` `max` `number`;
///  - any-type: `isTruthy` `isType` `toString`;
///  - string: `contains` `containsAll` `containsAny` `endsWith` `isEmpty`
///    `lower` `replace` `repeat` `reverse` `slice` `split` `startsWith`
///    `title` `trim`;
///  - number: `abs` `ceil` `floor` `isEmpty` `round` `toFixed`;
///  - date: `date` `format` `time` `relative` `isEmpty`;
///  - list: `contains` `containsAll` `containsAny` `isEmpty` `join`
///    `reverse` `slice` `sort` `unique` `length` (via `.length`, Obsidian's
///    own shorthand for a list's size, seen in the docs' formula examples);
///  - file: `file.hasTag(...)` `file.inFolder(...)` `file.hasLink(...)`
///    `file.hasProperty(...)`.
///
/// Not implemented - each is a clear "unsupported" parse or evaluate error,
/// never a crash: `list.filter/map/reduce` (would need a mini-interpreter
/// with an implicit loop variable), `link`/`file`/`icon`/`html`/`image`
/// object values and their methods, `object` values, `escapeHTML`,
/// `random`, regular-expression arguments to `replace`/`split`
/// (a plain-string needle/separator works).
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
