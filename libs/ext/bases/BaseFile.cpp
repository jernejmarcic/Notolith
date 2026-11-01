// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "BaseFile.h"

#include <yaml-cpp/yaml.h>

#include <QRegularExpression>

#include <vector>

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
    // A key that is not there is an undefined node, and asking one its type throws.
    if (node.IsDefined() && node.IsScalar() && scalarOf(node).startsWith(QLatin1String("DESC"), Qt::CaseInsensitive))
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

// --- rewriteViewSort: a minimal, comment-preserving text edit ------------
//
// yaml-cpp's own Node::Mark() gives every scalar's (line, column, byte pos)
// in the *original* source, which is enough to find "the sort: key's whole
// line-block" and "the next sibling's line" without a hand-rolled YAML
// scanner - the surgery works entirely in std::string byte offsets (never
// QString UTF-16 indices, which Mark().pos does not match against non-ASCII
// text) and never touches YAML::Emitter, so nothing it does not understand
// - a comment, an odd indent someone chose by hand - moves.

struct OrderedKey {
    std::string name;
    long lineStart = -1; ///< byte offset of the first character on the key's line (its own indent included)
    long column = -1;
};

/// `node`'s own top-level scalar keys, in source order, with where each
/// one's line begins. std::nullopt on anything this module will not try to
/// edit around: a flow mapping, a non-scalar key, or sibling keys sitting at
/// different columns (yaml-cpp allows it; a person hand-editing a `.base`
/// file is not expected to).
///
/// `lineStart` is deliberately not validated to be "plain spaces then the
/// key": a view map's *first* key normally shares its line with the
/// sequence's own `- ` (`  - type: table`), and that is not something to
/// reject - it is the ordinary way a `.base` file is written. What that
/// shape rules out is using that particular key's lineStart to *remove* a
/// span of text (the `- ` would go with it); rewriteViewSort below checks
/// for exactly that case, once, at the one place it matters, rather than
/// this function refusing the whole view over its very first key.
std::optional<std::vector<OrderedKey>> orderedMapKeys(const YAML::Node &node)
{
    if (!node.IsMap())
        return std::nullopt;
    std::vector<OrderedKey> keys;
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar())
            return std::nullopt;
        const YAML::Mark mark = it->first.Mark();
        if (mark.pos < 0 || mark.column < 0 || mark.pos < mark.column)
            return std::nullopt;
        keys.push_back({it->first.Scalar(), mark.pos - mark.column, mark.column});
    }
    if (keys.empty())
        return std::nullopt;
    for (const OrderedKey &k : keys) {
        if (k.column != keys.front().column)
            return std::nullopt; // exotic indentation: bail rather than corrupt it
    }
    return keys;
}

/// A property reference as a plain YAML scalar, quoted only if it needs to
/// be (a bare key with a space, say) - `file.name`/`formula.x`/most
/// frontmatter keys never do.
std::string yamlSafeScalar(const QString &text)
{
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.\\-]*$"));
    if (plain.match(text).hasMatch())
        return text.toStdString();
    QString escaped = text;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return "\"" + escaped.toStdString() + "\"";
}

} // namespace

std::optional<QString> BaseFile::rewriteViewSort(const QString &yamlText, int viewIndex,
                                                  const QString &propertyReference, SortDirection direction)
{
    const std::string source = yamlText.toStdString();
    YAML::Node root;
    try {
        root = YAML::Load(source);
    } catch (const YAML::Exception &) {
        return std::nullopt;
    }
    if (!root.IsMap())
        return std::nullopt;
    const YAML::Node views = root["views"];
    if (!views.IsSequence() || viewIndex < 0 || viewIndex >= int(views.size()))
        return std::nullopt;
    const YAML::Node view = views[viewIndex];
    const std::optional<std::vector<OrderedKey>> viewKeys = orderedMapKeys(view);
    if (!viewKeys)
        return std::nullopt;

    // Where this view's own last key ends: the next view's line, else the
    // next top-level key after "views" (root's own keys, same technique),
    // else end of file.
    long viewEndBoundary = long(source.size());
    if (viewIndex + 1 < int(views.size())) {
        const YAML::Mark next = views[viewIndex + 1].Mark();
        if (next.pos < 0 || next.column < 0 || next.pos < next.column)
            return std::nullopt;
        viewEndBoundary = next.pos - next.column;
    } else if (const std::optional<std::vector<OrderedKey>> rootKeys = orderedMapKeys(root)) {
        for (std::size_t i = 0; i < rootKeys->size(); ++i) {
            if (rootKeys->at(i).name == "views") {
                if (i + 1 < rootKeys->size())
                    viewEndBoundary = rootKeys->at(i + 1).lineStart;
                break;
            }
        }
    }
    // A root this exotic (non-scalar top-level keys, say) but a plain-shaped
    // view is still safe to edit: fall back to end-of-file rather than bail.

    long removeStart = -1, removeEnd = -1, insertAt = -1;
    for (std::size_t i = 0; i < viewKeys->size(); ++i) {
        if (viewKeys->at(i).name == "sort") {
            if (i == 0) {
                // This key's line starts with the sequence's own "- ", not
                // plain indentation (see orderedMapKeys) - removing "from
                // the start of this line" would delete the view's own list
                // marker along with it. Vanishingly rare (`sort:` is almost
                // never a view's first field) and not worth the extra
                // bookkeeping to rewrite in place instead of bailing.
                return std::nullopt;
            }
            removeStart = viewKeys->at(i).lineStart;
            removeEnd = (i + 1 < viewKeys->size()) ? viewKeys->at(i + 1).lineStart : viewEndBoundary;
            break;
        }
    }
    if (removeStart < 0)
        insertAt = viewEndBoundary; // no existing sort: - append after this view's last key

    const int keyIndent = int(viewKeys->front().column);
    const std::string direction8 = direction == SortDirection::Descending ? "DESC" : "ASC";
    std::string block = std::string(std::size_t(keyIndent), ' ') + "sort:\n"
                       + std::string(std::size_t(keyIndent + 2), ' ') + "- property: " + yamlSafeScalar(propertyReference) + "\n"
                       + std::string(std::size_t(keyIndent + 4), ' ') + "direction: " + direction8 + "\n";

    std::string result;
    if (removeStart >= 0) {
        result = source.substr(0, std::size_t(removeStart)) + block + source.substr(std::size_t(removeEnd));
    } else {
        if (insertAt > 0 && insertAt == long(source.size()) && !source.empty() && source.back() != '\n')
            block = "\n" + block;
        result = source.substr(0, std::size_t(insertAt)) + block + source.substr(std::size_t(insertAt));
    }
    return QString::fromStdString(result);
}

QString BaseFile::newFileTemplate()
{
    return QStringLiteral("views:\n  - type: table\n");
}

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
