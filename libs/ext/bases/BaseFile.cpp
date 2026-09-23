#include "BaseFile.h"

#include <yaml-cpp/yaml.h>

namespace noteAppC::bases {

namespace {

QString scalarOf(const YAML::Node &node)
{
    return node.IsScalar() ? QString::fromStdString(node.Scalar()) : QString();
}

FilterNode parseFilterNode(const YAML::Node &node)
{
    FilterNode result; // default: And with no children, matches everything
    if (!node.IsDefined() || node.IsNull())
        return result;

    if (node.IsScalar()) {
        result.kind = FilterNode::Kind::Statement;
        result.statement = scalarOf(node);
        return result;
    }
    if (node.IsSequence()) {
        result.kind = FilterNode::Kind::And;
        for (const auto &item : node)
            result.children << parseFilterNode(item);
        return result;
    }
    if (node.IsMap()) {
        // The documented shape is one of and/or/not per map, e.g.
        //   or:
        //     - file.hasTag("tag")
        //     - and: [...]
        for (const auto &entry : node) {
            const QString key = scalarOf(entry.first).toLower();
            result.kind = key == QLatin1String("or")    ? FilterNode::Kind::Or
                         : key == QLatin1String("not")   ? FilterNode::Kind::Not
                                                          : FilterNode::Kind::And;
            const YAML::Node &value = entry.second;
            if (value.IsSequence()) {
                for (const auto &item : value)
                    result.children << parseFilterNode(item);
            } else {
                result.children << parseFilterNode(value);
            }
            break; // exactly one and/or/not key per node, by the schema
        }
        return result;
    }
    return result;
}

QHash<QString, QString> parseDisplayNames(const YAML::Node &node)
{
    QHash<QString, QString> out;
    if (!node.IsMap())
        return out;
    for (const auto &entry : node) {
        const QString key = scalarOf(entry.first);
        const YAML::Node &config = entry.second;
        if (config.IsMap() && config["displayName"])
            out.insert(key, scalarOf(config["displayName"]));
    }
    return out;
}

QList<BaseFormula> parseFormulas(const YAML::Node &node)
{
    QList<BaseFormula> out;
    if (!node.IsMap())
        return out;
    for (const auto &entry : node) {
        if (!entry.second.IsScalar())
            continue; // a malformed formula is skipped, not fatal
        out << BaseFormula{scalarOf(entry.first), scalarOf(entry.second)};
    }
    return out;
}

SortDirection parseDirection(const YAML::Node &node)
{
    if (node.IsScalar() && scalarOf(node).startsWith(QLatin1String("DESC"), Qt::CaseInsensitive))
        return SortDirection::Descending;
    return SortDirection::Ascending;
}

QList<ViewConfig> parseViews(const YAML::Node &node)
{
    QList<ViewConfig> out;
    if (!node.IsSequence())
        return out;
    for (const auto &entry : node) {
        if (!entry.IsMap())
            continue;
        ViewConfig view;
        if (entry["type"]) view.type = scalarOf(entry["type"]);
        if (entry["name"]) view.name = scalarOf(entry["name"]);
        if (entry["limit"] && entry["limit"].IsScalar()) {
            try { view.limit = entry["limit"].as<int>(); } catch (const YAML::Exception &) {}
        }
        if (entry["filters"])
            view.filters = parseFilterNode(entry["filters"]);
        if (entry["order"] && entry["order"].IsSequence()) {
            for (const auto &item : entry["order"])
                if (item.IsScalar()) view.order << scalarOf(item);
        }
        // A view that names no columns still lists its notes, by name, as
        // Obsidian's does; without this it had nothing to draw at all. Only
        // the parsed view: serialize() writes back the file's own YAML.
        if (view.order.isEmpty())
            view.order << QStringLiteral("file.name");
        if (entry["sort"] && entry["sort"].IsSequence()) {
            for (const auto &item : entry["sort"]) {
                if (!item.IsMap() || !item["property"])
                    continue;
                view.sort << BaseSort{scalarOf(item["property"]), parseDirection(item["direction"])};
            }
        }
        if (entry["groupBy"] && entry["groupBy"].IsMap() && entry["groupBy"]["property"]) {
            view.groupBy.property = scalarOf(entry["groupBy"]["property"]);
            view.groupBy.direction = parseDirection(entry["groupBy"]["direction"]);
        }
        // Cards' cover-image property: the views page describes the feature
        // but its help article carries no YAML example, so this key name -
        // `image:`, alongside `type`/`name`/`order` at the same indent - is
        // this module's best inference, not a confirmed spelling.
        if (entry["image"] && entry["image"].IsScalar())
            view.imageProperty = scalarOf(entry["image"]);
        if (entry["summaries"] && entry["summaries"].IsMap()) {
            for (const auto &item : entry["summaries"]) {
                if (item.first.IsScalar() && item.second.IsScalar())
                    view.summaries.insert(scalarOf(item.first), scalarOf(item.second));
            }
        }
        out << view;
    }
    return out;
}

} // namespace

struct BaseFile::Impl {
    YAML::Node root;
    FilterNode globalFilters;
    QList<BaseFormula> formulas;
    QHash<QString, QString> summaryFormulas; ///< top-level `summaries:`
    QHash<QString, QString> displayNames;
    QList<ViewConfig> views;
};

BaseFile::BaseFile() : m_impl(std::make_unique<Impl>()) {}
BaseFile::BaseFile(BaseFile &&) noexcept = default;
BaseFile &BaseFile::operator=(BaseFile &&) noexcept = default;
BaseFile::~BaseFile() = default;

std::optional<BaseFile> BaseFile::parse(const QString &yamlText, QString *error)
{
    YAML::Node root;
    try {
        root = YAML::Load(yamlText.toStdString());
    } catch (const YAML::Exception &e) {
        if (error) *error = QString::fromStdString(e.what());
        return std::nullopt;
    }
    if (!root.IsDefined() || root.IsNull()) {
        // An empty file is a base with nothing configured yet, not an error.
        root = YAML::Node(YAML::NodeType::Map);
    }
    if (!root.IsMap()) {
        if (error) *error = QStringLiteral("a .base file must be a YAML mapping");
        return std::nullopt;
    }

    BaseFile file;
    file.m_impl->root = root;
    file.m_impl->globalFilters = parseFilterNode(root["filters"]);
    file.m_impl->formulas = parseFormulas(root["formulas"]);
    for (const BaseFormula &summary : parseFormulas(root["summaries"]))
        file.m_impl->summaryFormulas.insert(summary.name, summary.expression);
    file.m_impl->displayNames = parseDisplayNames(root["properties"]);
    file.m_impl->views = parseViews(root["views"]);
    if (file.m_impl->views.isEmpty()) {
        // Obsidian still opens a base with no `views:` as one implicit table
        // of everything, unfiltered - so a hand-written file with only
        // `filters:`/`formulas:` does something rather than showing nothing.
        ViewConfig implicit;
        implicit.type = QStringLiteral("table");
        implicit.name = QStringLiteral("All notes");
        file.m_impl->views << implicit;
    }
    return file;
}

QString BaseFile::serialize() const
{
    YAML::Emitter out;
    out << m_impl->root;
    return QString::fromStdString(out.c_str());
}

const FilterNode &BaseFile::globalFilters() const { return m_impl->globalFilters; }
const QList<BaseFormula> &BaseFile::formulas() const { return m_impl->formulas; }
QString BaseFile::summaryFormula(const QString &name) const { return m_impl->summaryFormulas.value(name); }
const QList<ViewConfig> &BaseFile::views() const { return m_impl->views; }

QString BaseFile::displayNameFor(const QString &propertyReference) const
{
    const auto it = m_impl->displayNames.constFind(propertyReference);
    if (it != m_impl->displayNames.constEnd() && !it.value().isEmpty())
        return it.value();

    QString key = propertyReference;
    for (const QString &prefix : {QStringLiteral("note."), QStringLiteral("file."),
                                  QStringLiteral("formula.")}) {
        if (key.startsWith(prefix)) { key.remove(0, prefix.size()); break; }
    }
    if (key.isEmpty())
        return propertyReference;
    return key.left(1).toUpper() + key.mid(1);
}

} // namespace noteAppC::bases
