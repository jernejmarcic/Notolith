#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

namespace noteAppC::bases {

/// One `formulas:` entry: a name and the expression text (Expression parses
/// it lazily, when a query actually needs it - a base nobody opens compiles
/// nothing).
struct BaseFormula {
    QString name;
    QString expression;
};

enum class SortDirection { Ascending, Descending };

/// One `sort:` entry. `property` is a property-reference string in the same
/// dotted form as `order:` and `groupBy.property` - `file.name`, `note.age`,
/// `formula.ppu`, or a bare frontmatter key - which BaseQuery compiles with
/// the same Expression::parse it uses for everything else.
struct BaseSort {
    QString property;
    SortDirection direction = SortDirection::Ascending;
};

/// `groupBy:`. An empty `property` means the view is not grouped.
struct BaseGroupBy {
    QString property;
    SortDirection direction = SortDirection::Ascending;
    bool isSet() const { return !property.isEmpty(); }
};

/// A parsed `filters:` (or a view's own `filters:`) tree: `and`/`or`/`not` of
/// nested trees and leaf statement strings, exactly the recursive shape
/// https://obsidian.md/help/bases/syntax documents. A default-constructed
/// node (`And` with no children) matches every row - what "no filters" means.
struct FilterNode {
    enum class Kind { Statement, And, Or, Not };
    Kind kind = Kind::And;
    QString statement;          ///< Kind::Statement: the expression source
    QList<FilterNode> children; ///< Kind::And / Or / Not
    bool matchesEverything() const { return kind != Kind::Statement && children.isEmpty(); }
};

/// One `views:` entry. `type` is kept as written ("table", "cards", or
/// anything else a newer Obsidian adds) rather than an enum, so a view type
/// this module does not render is still reported by name instead of being
/// silently dropped.
struct ViewConfig {
    QString type;
    QString name;
    FilterNode filters;   ///< on top of the file's global filters
    QStringList order;    ///< property-reference strings, display order
    QList<BaseSort> sort;
    int limit = -1;       ///< -1: unlimited
    BaseGroupBy groupBy;
    QString imageProperty; ///< cards only: cover-image property, "" = none
    /// `summaries:`: property reference -> a built-in summary's name
    /// ("Average", "Sum", "Earliest", "Filled", ...), shown under its column.
    QHash<QString, QString> summaries;
};

/// A `.base` file: YAML conforming to Obsidian's Bases schema. Parses the
/// pieces this module understands (filters, formulas, property display
/// names, views) and keeps the whole original tree besides, so anything the
/// file contains that this module does not model - `summaries:`, a `list` or
/// `kanban` view's own options, a future key - reads back out of
/// serialize() unchanged. What this module cannot do, because yaml-cpp does
/// not track them, is preserve comments and the file's original formatting;
/// see the module report for what that means for a saved edit.
class BaseFile {
public:
    /// Empty on a YAML or schema error, with `*error` filled.
    static std::optional<BaseFile> parse(const QString &yamlText, QString *error = nullptr);

    /// The full YAML back out - identical to the input for anything this
    /// class did not change.
    QString serialize() const;

    const FilterNode &globalFilters() const;
    const QList<BaseFormula> &formulas() const;
    /// The configured `displayName` for a property reference ("file.ext",
    /// "formula.ppu", a bare frontmatter key), or - when none was configured
    /// - that reference with its scope prefix dropped and capitalised, so a
    /// column always has a readable header.
    QString displayNameFor(const QString &propertyReference) const;
    const QList<ViewConfig> &views() const;

    BaseFile();
    BaseFile(const BaseFile &) = delete;
    BaseFile &operator=(const BaseFile &) = delete;
    BaseFile(BaseFile &&) noexcept;
    BaseFile &operator=(BaseFile &&) noexcept;
    ~BaseFile();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace noteAppC::bases
