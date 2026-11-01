// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "BaseQuery.h"

#include <cmath>

#include <numeric>

#include <QRegularExpression>
#include <QSet>

#include "Vault.h"

#include <QFileInfo>

#include <algorithm>

namespace noteAppC::bases {

namespace {

/// A frontmatter scalar that is *entirely* a wikilink - `[[Note]]` or
/// `[[Note|Alias]]`, optionally embed-marked (`![[...]]`, harmless here
/// since a property value is text either way) - reads as a Link, the same
/// as Obsidian: "Links in frontmatter properties are automatically
/// recognized as Link objects" (obsidian.md/help/bases/syntax). A value
/// that merely *contains* one mid-sentence stays plain text.
const QRegularExpression &wikilinkPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^\\s*!?\\[\\[([^\\]|]+)(?:\\|([^\\]]+))?\\]\\]\\s*$"));
    return pattern;
}

Value scalarPropertyValue(const QString &raw)
{
    const QRegularExpressionMatch m = wikilinkPattern().match(raw);
    if (m.hasMatch())
        return Value::ofLink(m.captured(1).trimmed(), m.captured(2).trimmed());
    return Value::ofString(raw);
}

/// The same normalisation `link.linksTo()`'s fuzzy match needs, kept as its
/// own copy rather than reaching into Expression.cpp's - this module and
/// Expression are separate translation units, and the rule is four lines
/// duplicated versus a link normaliser made public just for this.
QString normalisedLinkKey(QString target)
{
    target = target.trimmed();
    while (target.startsWith(QLatin1String("./"))) target.remove(0, 2);
    if (target.endsWith(QLatin1String(".md"), Qt::CaseInsensitive)) target.chop(3);
    return target.toCaseFolded();
}

/// A `FilterNode` tree, rendered as one expression string so it can be
/// compiled and evaluated through the ordinary Expression parser instead of
/// this module needing a second tree-walking evaluator just for filters.
QString filterExpressionText(const FilterNode &node)
{
    switch (node.kind) {
    case FilterNode::Kind::Statement:
        return QStringLiteral("(") + node.statement + QLatin1Char(')');
    case FilterNode::Kind::And:
    case FilterNode::Kind::Or: {
        if (node.children.isEmpty())
            return QStringLiteral("true"); // and/or of nothing: no constraint
        QStringList parts;
        for (const FilterNode &child : node.children)
            parts << filterExpressionText(child);
        const QString glue = node.kind == FilterNode::Kind::And ? QStringLiteral(" && ")
                                                                 : QStringLiteral(" || ");
        return QLatin1Char('(') + parts.join(glue) + QLatin1Char(')');
    }
    case FilterNode::Kind::Not: {
        QStringList parts;
        for (const FilterNode &child : node.children)
            parts << filterExpressionText(child);
        // not: [A, B] means "not both", i.e. !(A && B); not: [] negates the
        // vacuous "no constraint" true, which excludes every row - the safer
        // reading of a filter that is presumably not meant to be empty.
        const QString inner = parts.isEmpty() ? QStringLiteral("true") : parts.join(QStringLiteral(" && "));
        return QStringLiteral("!(") + inner + QLatin1Char(')');
    }
    }
    return QStringLiteral("true");
}

Value valueFromProperty(const Property &property)
{
    if (property.values.isEmpty())
        return Value::empty();
    switch (property.type) {
    case Property::Type::List: {
        QList<Value> items;
        items.reserve(property.values.size());
        for (const QString &item : property.values)
            items << scalarPropertyValue(item);
        return Value::ofValues(std::move(items));
    }
    case Property::Type::Number: {
        bool ok = false;
        const double n = property.values.first().toDouble(&ok);
        return ok ? Value::ofNumber(n) : Value::empty();
    }
    case Property::Type::Checkbox:
        return Value::ofBool(property.values.first().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0);
    case Property::Type::Date: {
        const QDate d = QDate::fromString(property.values.first(), Qt::ISODate);
        return d.isValid() ? Value::ofDate(QDateTime(d, QTime(0, 0))) : Value::empty();
    }
    case Property::Type::DateTime: {
        const QDateTime dt = QDateTime::fromString(property.values.first(), Qt::ISODate);
        return dt.isValid() ? Value::ofDate(dt) : Value::empty();
    }
    case Property::Type::Text:
    case Property::Type::Unknown:
    default:
        return scalarPropertyValue(property.values.first());
    }
}

} // namespace

Row rowFromNote(const Index::BaseNote &note)
{
    Row row;
    row.notePath = note.path;
    const QFileInfo info(note.path); // string splitting only - the path is not opened
    row.noteName = info.completeBaseName();
    row.noteExt = info.suffix();
    row.noteFolder = info.path() == QLatin1String(".") ? QString() : info.path();
    row.fileSize = note.size;
    row.fileMtime = QDateTime::fromMSecsSinceEpoch(note.mtime);
    row.tags = note.tags;
    row.links = note.linkTargets;
    for (const Property &property : note.properties)
        row.properties.insert(property.key.toCaseFolded(), valueFromProperty(property));
    return row;
}

// --- BaseQuery::Resolver ------------------------------------------------

/// Resolves `formula.x` for whichever row Evaluator is currently working on.
/// Per-row memoization means a formula referenced twice while evaluating one
/// row (once in a filter, once in an `order:` column, say) is computed once;
/// `m_resolving` catches a cycle - direct or through another formula -
/// before it becomes infinite recursion.
class BaseQuery::Resolver : public FormulaSource {
public:
    explicit Resolver(BaseQuery *owner) : m_owner(owner) {}

    void beginRow(qsizetype rowIndex)
    {
        if (rowIndex != m_currentRow) {
            m_memo.clear();
            m_currentRow = rowIndex;
        }
    }
    void setNow(const QDateTime &now) { m_now = now; }

    Value resolveFormula(const QString &name, const Row &row, QString *error) const override
    {
        if (const auto it = m_memo.constFind(name); it != m_memo.constEnd()) {
            if (error) error->clear();
            return it.value();
        }
        if (m_resolving.contains(name)) {
            if (error) *error = QStringLiteral("circular formula reference: formula.%1").arg(name);
            return Value::empty();
        }
        const BaseFormula *definition = nullptr;
        for (const BaseFormula &f : m_owner->m_file.formulas()) {
            if (f.name == name) { definition = &f; break; }
        }
        if (!definition) {
            if (error) *error = QStringLiteral("unknown formula 'formula.%1'").arg(name);
            return Value::empty();
        }
        const std::shared_ptr<Expression> expr = compileFormula(name, definition->expression);
        if (!expr) {
            if (error) *error = QStringLiteral("formula.%1: invalid expression").arg(name);
            return Value::empty();
        }
        m_resolving.insert(name);
        QString innerError;
        const Value v = expr->evaluate(row, this, m_now, &innerError);
        m_resolving.remove(name);
        if (!innerError.isEmpty()) {
            if (error) *error = QStringLiteral("formula.%1: %2").arg(name, innerError);
            return Value::empty();
        }
        if (error) error->clear();
        m_memo.insert(name, v);
        return v;
    }

    /// `link.linksTo(file)`: the target `linkText` resolves to's own
    /// outgoing links, found by the same path-or-basename fuzzy match
    /// `file.hasLink()` uses - built once per query, from the rows already
    /// held for filtering, not a second database pass.
    QStringList outgoingLinksOf(const QString &linkText) const override
    {
        ensureLinkIndex();
        const QString key = normalisedLinkKey(linkText);
        if (const auto it = m_byPath.constFind(key); it != m_byPath.constEnd())
            return m_owner->m_rows.at(it.value()).links;
        if (const auto it = m_byName.constFind(basenameKey(key)); it != m_byName.constEnd())
            return m_owner->m_rows.at(it.value()).links;
        return {};
    }

private:
    static QString basenameKey(const QString &key)
    {
        const qsizetype slash = key.lastIndexOf(u'/');
        return slash < 0 ? key : key.mid(slash + 1);
    }
    void ensureLinkIndex() const
    {
        if (m_linkIndexBuilt) return;
        m_linkIndexBuilt = true;
        for (qsizetype i = 0; i < m_owner->m_rows.size(); ++i) {
            const Row &row = m_owner->m_rows.at(i);
            m_byPath.insert(normalisedLinkKey(row.notePath), i);
            m_byName.insert(normalisedLinkKey(row.noteName), i);
        }
    }
    std::shared_ptr<Expression> compileFormula(const QString &name, const QString &text) const
    {
        if (const auto it = m_compiledFormulas.constFind(name); it != m_compiledFormulas.constEnd())
            return it.value();
        QString parseError;
        std::shared_ptr<Expression> expr(Expression::parse(text, &parseError).release());
        // Cached even when null, so a broken formula is reported once and
        // not re-parsed for every row that references it.
        m_compiledFormulas.insert(name, expr);
        return expr;
    }

    BaseQuery *m_owner;
    QDateTime m_now;
    qsizetype m_currentRow = -1;
    mutable QHash<QString, Value> m_memo;
    mutable QSet<QString> m_resolving;
    mutable QHash<QString, std::shared_ptr<Expression>> m_compiledFormulas;
    mutable bool m_linkIndexBuilt = false;
    mutable QHash<QString, qsizetype> m_byPath, m_byName;
};

// --- BaseQuery ------------------------------------------------------------

BaseQuery::BaseQuery(const BaseFile &file, QList<Row> rows, const Vault *vault)
    : m_file(file), m_rows(std::move(rows)), m_vault(vault), m_resolver(std::make_unique<Resolver>(this))
{
}

BaseQuery::~BaseQuery() = default;

const ViewConfig &BaseQuery::view() const { return m_file.views().at(m_viewIndex); }

std::shared_ptr<Expression> BaseQuery::compiled(const QString &text) const
{
    if (const auto it = m_compiled.constFind(text); it != m_compiled.constEnd())
        return it.value();
    QString error;
    std::shared_ptr<Expression> expr(Expression::parse(text, &error).release());
    if (!expr)
        warnOnce(QStringLiteral("\"%1\": %2").arg(text, error));
    m_compiled.insert(text, expr);
    return expr;
}

void BaseQuery::warnOnce(const QString &message) const
{
    if (!m_warnings.contains(message))
        m_warnings << message;
}

bool BaseQuery::referencesCtime(const QString &text) const
{
    return text.contains(QLatin1String("ctime"));
}

bool BaseQuery::openView(int viewIndex, QString *error)
{
    m_result.clear();
    m_warnings.clear();
    m_now = QDateTime::currentDateTime();
    m_resolver->setNow(m_now);

    if (viewIndex < 0 || viewIndex >= m_file.views().size()) {
        if (error) *error = QStringLiteral("no such view");
        return false;
    }
    m_viewIndex = viewIndex;
    const ViewConfig &v = view();

    // file.ctime is not in the index (see Index.h's BaseNote), so it costs a
    // stat() per note - paid once here, and only for a view that might read
    // it, never for one that does not.
    bool needsCtime = referencesCtime(filterExpressionText(m_file.globalFilters()))
                     || referencesCtime(filterExpressionText(v.filters));
    for (const QString &column : v.order) needsCtime = needsCtime || referencesCtime(column);
    for (const BaseSort &s : v.sort) needsCtime = needsCtime || referencesCtime(s.property);
    if (v.groupBy.isSet()) needsCtime = needsCtime || referencesCtime(v.groupBy.property);
    for (const BaseFormula &f : m_file.formulas()) needsCtime = needsCtime || referencesCtime(f.expression);
    if (needsCtime && m_vault) {
        for (Row &row : m_rows) {
            if (!row.fileCtime.isValid())
                row.fileCtime = QFileInfo(m_vault->absolute(row.notePath)).birthTime();
        }
    }

    const bool trivial = m_file.globalFilters().matchesEverything() && v.filters.matchesEverything();
    std::shared_ptr<Expression> filterExpr;
    if (!trivial) {
        const QString filterText = QStringLiteral("(%1) && (%2)")
            .arg(filterExpressionText(m_file.globalFilters()), filterExpressionText(v.filters));
        filterExpr = compiled(filterText);
        if (!filterExpr) {
            if (error) *error = QStringLiteral("view '%1' has an invalid filter").arg(v.name);
            return false;
        }
    }

    m_result.reserve(m_rows.size());
    for (qsizetype i = 0; i < m_rows.size(); ++i) {
        if (filterExpr) {
            m_resolver->beginRow(i);
            QString rowError;
            const Value matched = filterExpr->evaluate(m_rows[i], m_resolver.get(), m_now, &rowError);
            if (!rowError.isEmpty()) { warnOnce(rowError); continue; }
            if (!matched.truthy()) continue;
        }
        m_result << i;
    }

    // Applied least-significant-key first: each stable_sort only reorders
    // ties left by the one before it, so the LAST call here ends up the
    // most significant key - which is why groupBy runs after the sort list,
    // and the sort list itself runs back to front.
    for (auto it = v.sort.crbegin(); it != v.sort.crend(); ++it)
        sortBy(it->property, it->direction);
    if (v.groupBy.isSet())
        sortBy(v.groupBy.property, v.groupBy.direction);

    if (v.limit >= 0 && m_result.size() > v.limit)
        m_result = m_result.mid(0, v.limit);

    if (error) error->clear();
    return true;
}

Value BaseQuery::valueOf(qsizetype rowIndex, const QString &propertyReference) const
{
    if (rowIndex < 0 || rowIndex >= m_rows.size())
        return Value::empty();
    const std::shared_ptr<Expression> expr = compiled(propertyReference);
    if (!expr)
        return Value::empty();
    m_resolver->beginRow(rowIndex);
    QString error;
    Value v = expr->evaluate(m_rows[rowIndex], m_resolver.get(), m_now, &error);
    if (!error.isEmpty()) {
        warnOnce(error);
        return Value::empty();
    }
    return v;
}

QString BaseQuery::summaryOf(const QString &propertyReference) const
{
    const QString name = view().summaries.value(propertyReference);
    if (name.isEmpty())
        return {};
    QList<Value> values;
    values.reserve(m_result.size());
    for (const qsizetype row : m_result)
        values << valueOf(row, propertyReference);
    // The file's own summary, an expression over `values`, before a
    // built-in of the same name.
    if (const QString formula = m_file.summaryFormula(name); !formula.isEmpty()) {
        QString error;
        const std::unique_ptr<Expression> expression = Expression::parse(formula, &error);
        if (!expression) {
            warnOnce(error);
            return {};
        }
        const Value result = expression->evaluate(Row(), nullptr, m_now, &error,
                                                  {{QStringLiteral("values"), Value::ofValues(values)}});
        if (!error.isEmpty())
            warnOnce(error);
        return result.toDisplayString();
    }
    return summarize(name, values);
}

QString summarize(const QString &name, const QList<Value> &values)
{
    const QString key = name.toLower();
    const auto blank = [](const Value &value) {
        return value.isEmpty() || (value.type == Value::Type::String && value.text.isEmpty())
            || (value.type == Value::Type::List && value.list.isEmpty());
    };
    const auto count = [&values](auto keep) {
        return QString::number(std::count_if(values.cbegin(), values.cend(), keep));
    };
    if (key == QLatin1String("empty"))
        return count(blank);
    if (key == QLatin1String("filled"))
        return count([&](const Value &value) { return !blank(value); });
    if (key == QLatin1String("unique")) {
        QSet<QString> seen;
        for (const Value &value : values) {
            if (!blank(value))
                seen.insert(value.toDisplayString());
        }
        return QString::number(seen.size());
    }
    if (key == QLatin1String("checked"))
        return count([](const Value &value) { return value.type == Value::Type::Bool && value.boolean; });
    if (key == QLatin1String("unchecked"))
        return count([](const Value &value) { return value.type == Value::Type::Bool && !value.boolean; });

    QList<double> numbers;
    QList<QDateTime> dates;
    for (const Value &value : values) {
        if (value.type == Value::Type::Number && std::isfinite(value.number))
            numbers << value.number;
        else if (value.type == Value::Type::Date && value.date.isValid())
            dates << value.date;
    }
    if (dates.size() > numbers.size()) { // a date column
        if (dates.isEmpty())
            return {};
        std::sort(dates.begin(), dates.end());
        if (key == QLatin1String("earliest"))
            return Value::ofDate(dates.constFirst()).toDisplayString();
        if (key == QLatin1String("latest"))
            return Value::ofDate(dates.constLast()).toDisplayString();
        if (key == QLatin1String("range")) {
            const qint64 days = dates.constFirst().daysTo(dates.constLast());
            return days == 1 ? QStringLiteral("1 day") : QStringLiteral("%1 days").arg(days);
        }
        return {};
    }
    if (numbers.isEmpty())
        return {};
    std::sort(numbers.begin(), numbers.end());
    const double sum = std::accumulate(numbers.cbegin(), numbers.cend(), 0.0);
    const double mean = sum / double(numbers.size());
    // To three places, as Obsidian's own example summary rounds a mean.
    const auto shown = [](double number) {
        return Value::ofNumber(std::round(number * 1000) / 1000).toDisplayString();
    };
    if (key == QLatin1String("sum"))
        return shown(sum);
    if (key == QLatin1String("average"))
        return shown(mean);
    if (key == QLatin1String("min"))
        return shown(numbers.constFirst());
    if (key == QLatin1String("max"))
        return shown(numbers.constLast());
    if (key == QLatin1String("range"))
        return shown(numbers.constLast() - numbers.constFirst());
    if (key == QLatin1String("median")) {
        const qsizetype middle = numbers.size() / 2;
        return shown(numbers.size() % 2 ? numbers.at(middle) : (numbers.at(middle - 1) + numbers.at(middle)) / 2);
    }
    if (key == QLatin1String("stddev")) {
        double squares = 0;
        for (const double number : std::as_const(numbers))
            squares += (number - mean) * (number - mean);
        return shown(std::sqrt(squares / double(numbers.size()))); // over the column, not a sample
    }
    return {};
}

void BaseQuery::sortBy(const QString &propertyReference, SortDirection direction)
{
    const std::shared_ptr<Expression> expr = compiled(propertyReference);

    // Each row's value is computed once for this pass, not once per
    // comparison - std::stable_sort makes O(n log n) comparisons.
    QHash<qsizetype, Value> values;
    values.reserve(int(m_result.size()));
    for (qsizetype idx : std::as_const(m_result)) {
        if (!expr) { values.insert(idx, Value::empty()); continue; }
        m_resolver->beginRow(idx);
        QString rowError;
        Value v = expr->evaluate(m_rows[idx], m_resolver.get(), m_now, &rowError);
        if (!rowError.isEmpty()) { warnOnce(rowError); v = Value::empty(); }
        values.insert(idx, v);
    }

    std::stable_sort(m_result.begin(), m_result.end(), [&](qsizetype a, qsizetype b) {
        const Value &va = values.value(a);
        const Value &vb = values.value(b);
        if (va.isEmpty() != vb.isEmpty())
            return !va.isEmpty(); // blanks last, regardless of direction
        if (va.isEmpty())
            return false;
        const int cmp = compareForSort(va, vb);
        return direction == SortDirection::Ascending ? cmp < 0 : cmp > 0;
    });
}

QString BaseQuery::groupKeyOf(qsizetype rowIndex) const
{
    if (m_viewIndex < 0 || !view().groupBy.isSet())
        return QString();
    return valueOf(rowIndex, view().groupBy.property).toDisplayString();
}

} // namespace noteAppC::bases
