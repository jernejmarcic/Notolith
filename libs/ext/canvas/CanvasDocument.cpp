// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "CanvasDocument.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>

#include "Theme.h"

namespace noteAppC {

namespace {

constexpr auto kID = "id";
constexpr auto kType = "type";
constexpr auto kX = "x";
constexpr auto kY = "y";
constexpr auto kWidth = "width";
constexpr auto kHeight = "height";
constexpr auto kColor = "color";
constexpr auto kText = "text";
constexpr auto kFile = "file";
constexpr auto kSubpath = "subpath";
constexpr auto kUrl = "url";
constexpr auto kLabel = "label";
constexpr auto kBackground = "background";
constexpr auto kBackgroundStyle = "backgroundStyle";
constexpr auto kFromNode = "fromNode";
constexpr auto kToNode = "toNode";
constexpr auto kFromSide = "fromSide";
constexpr auto kToSide = "toSide";
constexpr auto kFromEnd = "fromEnd";
constexpr auto kToEnd = "toEnd";
constexpr auto kNodes = "nodes";
constexpr auto kEdges = "edges";

QString stringField(const QJsonObject &json, const char *key)
{
    return json.value(QLatin1String(key)).toString();
}

} // namespace

CanvasNodeType canvasNodeTypeFromString(const QString &type)
{
    if (type == QLatin1String("text")) return CanvasNodeType::Text;
    if (type == QLatin1String("file")) return CanvasNodeType::File;
    if (type == QLatin1String("link")) return CanvasNodeType::Link;
    if (type == QLatin1String("group")) return CanvasNodeType::Group;
    return CanvasNodeType::Unknown;
}

QString canvasNodeTypeToString(CanvasNodeType type)
{
    switch (type) {
    case CanvasNodeType::Text: return QStringLiteral("text");
    case CanvasNodeType::File: return QStringLiteral("file");
    case CanvasNodeType::Link: return QStringLiteral("link");
    case CanvasNodeType::Group: return QStringLiteral("group");
    case CanvasNodeType::Unknown: break;
    }
    return QString();
}

QColor resolveCanvasColor(const QString &raw, const Theme &theme)
{
    static const QHash<QString, QString> kPresets{
        {QStringLiteral("1"), QStringLiteral("--color-red")},
        {QStringLiteral("2"), QStringLiteral("--color-orange")},
        {QStringLiteral("3"), QStringLiteral("--color-yellow")},
        {QStringLiteral("4"), QStringLiteral("--color-green")},
        {QStringLiteral("5"), QStringLiteral("--color-cyan")},
        {QStringLiteral("6"), QStringLiteral("--color-purple")},
    };
    if (raw.isEmpty())
        return QColor();
    const auto preset = kPresets.constFind(raw);
    if (preset != kPresets.constEnd())
        return theme.color(preset.value());
    return Theme::parseColor(raw);
}

// --- CanvasNode -------------------------------------------------------

QString CanvasNode::id() const { return stringField(m_json, kID); }
void CanvasNode::setID(const QString &id) { m_json[QLatin1String(kID)] = id; }

CanvasNodeType CanvasNode::type() const { return canvasNodeTypeFromString(stringField(m_json, kType)); }
void CanvasNode::setType(CanvasNodeType type) { m_json[QLatin1String(kType)] = canvasNodeTypeToString(type); }

QRectF CanvasNode::rect() const
{
    return QRectF(m_json.value(QLatin1String(kX)).toDouble(), m_json.value(QLatin1String(kY)).toDouble(),
                  m_json.value(QLatin1String(kWidth)).toDouble(), m_json.value(QLatin1String(kHeight)).toDouble());
}

void CanvasNode::setRect(const QRectF &rect)
{
    // The spec has these as integers; every value this code itself writes
    // conforms even though reading tolerates whatever a file actually has.
    m_json[QLatin1String(kX)] = qRound(rect.x());
    m_json[QLatin1String(kY)] = qRound(rect.y());
    m_json[QLatin1String(kWidth)] = qRound(rect.width());
    m_json[QLatin1String(kHeight)] = qRound(rect.height());
}

QString CanvasNode::color() const { return stringField(m_json, kColor); }
void CanvasNode::setColor(const QString &color)
{
    if (color.isEmpty())
        m_json.remove(QLatin1String(kColor));
    else
        m_json[QLatin1String(kColor)] = color;
}

QString CanvasNode::text() const { return stringField(m_json, kText); }
void CanvasNode::setText(const QString &text) { m_json[QLatin1String(kText)] = text; }

QString CanvasNode::file() const { return stringField(m_json, kFile); }
void CanvasNode::setFile(const QString &file) { m_json[QLatin1String(kFile)] = file; }
QString CanvasNode::subpath() const { return stringField(m_json, kSubpath); }
void CanvasNode::setSubpath(const QString &subpath)
{
    if (subpath.isEmpty())
        m_json.remove(QLatin1String(kSubpath));
    else
        m_json[QLatin1String(kSubpath)] = subpath;
}

QString CanvasNode::url() const { return stringField(m_json, kUrl); }
void CanvasNode::setUrl(const QString &url) { m_json[QLatin1String(kUrl)] = url; }

QString CanvasNode::label() const { return stringField(m_json, kLabel); }
void CanvasNode::setLabel(const QString &label)
{
    if (label.isEmpty())
        m_json.remove(QLatin1String(kLabel));
    else
        m_json[QLatin1String(kLabel)] = label;
}
QString CanvasNode::background() const { return stringField(m_json, kBackground); }
void CanvasNode::setBackground(const QString &background)
{
    if (background.isEmpty())
        m_json.remove(QLatin1String(kBackground));
    else
        m_json[QLatin1String(kBackground)] = background;
}
QString CanvasNode::backgroundStyle() const { return stringField(m_json, kBackgroundStyle); }
void CanvasNode::setBackgroundStyle(const QString &style)
{
    if (style.isEmpty())
        m_json.remove(QLatin1String(kBackgroundStyle));
    else
        m_json[QLatin1String(kBackgroundStyle)] = style;
}

// --- CanvasEdge -------------------------------------------------------

QString CanvasEdge::id() const { return stringField(m_json, kID); }
void CanvasEdge::setID(const QString &id) { m_json[QLatin1String(kID)] = id; }

QString CanvasEdge::fromNode() const { return stringField(m_json, kFromNode); }
void CanvasEdge::setFromNode(const QString &id) { m_json[QLatin1String(kFromNode)] = id; }
QString CanvasEdge::toNode() const { return stringField(m_json, kToNode); }
void CanvasEdge::setToNode(const QString &id) { m_json[QLatin1String(kToNode)] = id; }

QString CanvasEdge::fromSide() const { return stringField(m_json, kFromSide); }
void CanvasEdge::setFromSide(const QString &side)
{
    if (side.isEmpty())
        m_json.remove(QLatin1String(kFromSide));
    else
        m_json[QLatin1String(kFromSide)] = side;
}
QString CanvasEdge::toSide() const { return stringField(m_json, kToSide); }
void CanvasEdge::setToSide(const QString &side)
{
    if (side.isEmpty())
        m_json.remove(QLatin1String(kToSide));
    else
        m_json[QLatin1String(kToSide)] = side;
}

CanvasEdgeEnd CanvasEdge::fromEnd() const
{
    return stringField(m_json, kFromEnd) == QLatin1String("arrow") ? CanvasEdgeEnd::Arrow : CanvasEdgeEnd::None;
}
void CanvasEdge::setFromEnd(CanvasEdgeEnd end)
{
    if (end == CanvasEdgeEnd::None)
        m_json.remove(QLatin1String(kFromEnd)); // the spec's own default
    else
        m_json[QLatin1String(kFromEnd)] = QStringLiteral("arrow");
}
CanvasEdgeEnd CanvasEdge::toEnd() const
{
    return stringField(m_json, kToEnd) == QLatin1String("none") ? CanvasEdgeEnd::None : CanvasEdgeEnd::Arrow;
}
void CanvasEdge::setToEnd(CanvasEdgeEnd end)
{
    if (end == CanvasEdgeEnd::Arrow)
        m_json.remove(QLatin1String(kToEnd)); // the spec's own default
    else
        m_json[QLatin1String(kToEnd)] = QStringLiteral("none");
}

QString CanvasEdge::color() const { return stringField(m_json, kColor); }
void CanvasEdge::setColor(const QString &color)
{
    if (color.isEmpty())
        m_json.remove(QLatin1String(kColor));
    else
        m_json[QLatin1String(kColor)] = color;
}
QString CanvasEdge::label() const { return stringField(m_json, kLabel); }
void CanvasEdge::setLabel(const QString &label)
{
    if (label.isEmpty())
        m_json.remove(QLatin1String(kLabel));
    else
        m_json[QLatin1String(kLabel)] = label;
}

// --- CanvasDocument -----------------------------------------------------

bool CanvasDocument::parse(const QByteArray &json, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = parseError.errorString();
        return false;
    }
    m_root = doc.object();
    m_nodes.clear();
    m_edges.clear();
    for (const QJsonValue &value : m_root.value(QLatin1String(kNodes)).toArray())
        if (value.isObject())
            m_nodes.append(CanvasNode(value.toObject()));
    for (const QJsonValue &value : m_root.value(QLatin1String(kEdges)).toArray())
        if (value.isObject())
            m_edges.append(CanvasEdge(value.toObject()));
    m_originalBytes = json;
    m_dirty = false;
    reindex();
    return true;
}

bool CanvasDocument::load(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (!parse(file.readAll(), error))
        return false;
    m_path = path;
    return true;
}

QByteArray CanvasDocument::serialize() const
{
    // Nothing changed since load()/the last save(): the exact bytes on disk,
    // formatting and all - the strongest form of "never rewrite what was not
    // edited".
    if (!m_dirty)
        return m_originalBytes;

    QJsonObject root = m_root; // keeps every root key beyond nodes/edges
    QJsonArray nodesArray;
    for (const CanvasNode &node : m_nodes)
        nodesArray.append(node.json());
    QJsonArray edgesArray;
    for (const CanvasEdge &edge : m_edges)
        edgesArray.append(edge.json());
    root[QLatin1String(kNodes)] = nodesArray;
    root[QLatin1String(kEdges)] = edgesArray;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool CanvasDocument::save(const QString &path, QString *error)
{
    const QString target = path.isEmpty() ? m_path : path;
    if (target.isEmpty()) {
        if (error)
            *error = QStringLiteral("no path to save to");
        return false;
    }
    if (QFileInfo(target).isSymLink()) { // QSaveFile would write the link's target
        if (error)
            *error = QStringLiteral("%1 is a link, so it was not written").arg(target);
        return false;
    }
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray bytes = serialize();
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    m_path = target;
    m_originalBytes = bytes;
    m_dirty = false;
    return true;
}

const CanvasNode *CanvasDocument::node(const QString &id) const
{
    const int index = m_nodeIndex.value(id, -1);
    return index >= 0 ? &m_nodes.at(index) : nullptr;
}

const CanvasEdge *CanvasDocument::edge(const QString &id) const
{
    const int index = m_edgeIndex.value(id, -1);
    return index >= 0 ? &m_edges.at(index) : nullptr;
}

QString CanvasDocument::addNode(CanvasNode node)
{
    if (node.id().isEmpty() || m_nodeIndex.contains(node.id()))
        node.setID(newID());
    m_nodes.append(node);
    m_nodeIndex.insert(node.id(), int(m_nodes.size()) - 1);
    m_dirty = true;
    return node.id();
}

bool CanvasDocument::removeNode(const QString &id)
{
    const int index = m_nodeIndex.value(id, -1);
    if (index < 0)
        return false;
    m_nodes.removeAt(index);
    reindex();
    m_dirty = true;
    return true;
}

void CanvasDocument::insertNode(int index, const CanvasNode &node)
{
    m_nodes.insert(qBound(0, index, int(m_nodes.size())), node);
    reindex();
    m_dirty = true;
}

QString CanvasDocument::addEdge(CanvasEdge edge)
{
    if (edge.id().isEmpty() || m_edgeIndex.contains(edge.id()))
        edge.setID(newID());
    m_edges.append(edge);
    m_edgeIndex.insert(edge.id(), int(m_edges.size()) - 1);
    m_dirty = true;
    return edge.id();
}

bool CanvasDocument::removeEdge(const QString &id)
{
    const int index = m_edgeIndex.value(id, -1);
    if (index < 0)
        return false;
    m_edges.removeAt(index);
    reindex();
    m_dirty = true;
    return true;
}

void CanvasDocument::insertEdge(int index, const CanvasEdge &edge)
{
    m_edges.insert(qBound(0, index, int(m_edges.size())), edge);
    reindex();
    m_dirty = true;
}

bool CanvasDocument::setNodeRect(const QString &id, const QRectF &rect)
{
    const int index = m_nodeIndex.value(id, -1);
    if (index < 0)
        return false;
    m_nodes[index].setRect(rect);
    m_dirty = true;
    return true;
}

bool CanvasDocument::setNodeText(const QString &id, const QString &text)
{
    const int index = m_nodeIndex.value(id, -1);
    if (index < 0)
        return false;
    m_nodes[index].setText(text);
    m_dirty = true;
    return true;
}

QString CanvasDocument::newID() const
{
    QString id;
    do {
        id = QUuid::createUuid().toString(QUuid::Id128);
    } while (m_nodeIndex.contains(id) || m_edgeIndex.contains(id));
    return id;
}

void CanvasDocument::reindex()
{
    m_nodeIndex.clear();
    m_nodeIndex.reserve(int(m_nodes.size()));
    for (int i = 0; i < m_nodes.size(); ++i)
        m_nodeIndex.insert(m_nodes.at(i).id(), i);
    m_edgeIndex.clear();
    m_edgeIndex.reserve(int(m_edges.size()));
    for (int i = 0; i < m_edges.size(); ++i)
        m_edgeIndex.insert(m_edges.at(i).id(), i);
}

} // namespace noteAppC
