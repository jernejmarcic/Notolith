// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

namespace noteAppC {

class Theme;

/// JSON Canvas 1.0 node types (jsoncanvas.org/spec/1.0). `Unknown` is a node
/// whose "type" this build does not recognise - kept and round-tripped, drawn
/// as a plain box, rather than dropped: the file is the truth even for the
/// parts of it this code does not understand.
enum class CanvasNodeType { Text, File, Link, Group, Unknown };

enum class CanvasEdgeEnd { None, Arrow };

CanvasNodeType canvasNodeTypeFromString(const QString &type);
QString canvasNodeTypeToString(CanvasNodeType type);

/// A JSON Canvas colour: `""` (theme default), a preset `"1"`..`"6"` (the
/// theme's red through purple), or a literal `#RRGGBB`/`#RGB`. `raw` is
/// exactly the file's own string, so a value this build does not resolve to
/// anything sensible still round-trips unchanged.
QColor resolveCanvasColor(const QString &raw, const Theme &theme);

/// One node. `json()` is the ground truth: every accessor reads it and every
/// mutator writes into it, so a key neither this class nor its caller ever
/// touches - one a future JSON Canvas version added, or another app's
/// extension - survives untouched. Fields absent from the file read back as
/// the spec's own default.
class CanvasNode {
public:
    CanvasNode() = default;
    explicit CanvasNode(QJsonObject json) : m_json(std::move(json)) {}

    const QJsonObject &json() const { return m_json; }

    QString id() const;
    void setID(const QString &id);

    CanvasNodeType type() const;
    void setType(CanvasNodeType type);

    QRectF rect() const;
    void setRect(const QRectF &rect);
    QPointF pos() const { return rect().topLeft(); }
    QSizeF size() const { return rect().size(); }

    /// Raw form: `""`, `"1"`..`"6"`, or a hex string - see resolveCanvasColor.
    QString color() const;
    void setColor(const QString &color);

    // -- text node --
    QString text() const;
    void setText(const QString &text);

    // -- file node --
    QString file() const;
    void setFile(const QString &file);
    /// A heading or block reference, e.g. "#Some heading"; empty if none.
    QString subpath() const;
    void setSubpath(const QString &subpath);

    // -- link node --
    QString url() const;
    void setUrl(const QString &url);

    // -- group node --
    QString label() const;
    void setLabel(const QString &label);
    QString background() const;
    void setBackground(const QString &background);
    /// "cover", "ratio" or "repeat"; empty means the spec's own default (cover).
    QString backgroundStyle() const;
    void setBackgroundStyle(const QString &style);

private:
    QJsonObject m_json;
};

class CanvasEdge {
public:
    CanvasEdge() = default;
    explicit CanvasEdge(QJsonObject json) : m_json(std::move(json)) {}

    const QJsonObject &json() const { return m_json; }

    QString id() const;
    void setID(const QString &id);

    QString fromNode() const;
    void setFromNode(const QString &id);
    QString toNode() const;
    void setToNode(const QString &id);

    /// "top"/"right"/"bottom"/"left", or empty when the file leaves it
    /// unspecified - the view then picks whichever side faces the other node.
    QString fromSide() const;
    void setFromSide(const QString &side);
    QString toSide() const;
    void setToSide(const QString &side);

    CanvasEdgeEnd fromEnd() const; ///< defaults to None
    void setFromEnd(CanvasEdgeEnd end);
    CanvasEdgeEnd toEnd() const; ///< defaults to Arrow
    void setToEnd(CanvasEdgeEnd end);

    QString color() const;
    void setColor(const QString &color);
    QString label() const;
    void setLabel(const QString &label);

private:
    QJsonObject m_json;
};

/// Reads and writes JSON Canvas 1.0. "The file is the truth": a document that
/// was never edited writes back the exact bytes it read, so opening and
/// saving a file this code only partly understands never rewrites a byte of
/// it. Once it has been edited, save() regenerates the JSON from the current
/// nodes and edges - but every key neither this class nor a caller touched is
/// still sitting in each node's and edge's own JSON object, and the root
/// object keeps every key beyond "nodes" and "edges" too.
class CanvasDocument {
public:
    bool load(const QString &path, QString *error = nullptr);
    /// Parses `json` without touching disk - what load() does internally,
    /// and what the tests use to avoid a temp file.
    bool parse(const QByteArray &json, QString *error = nullptr);

    /// Atomic write (QSaveFile) to `path`, or back to the path load()/a prior
    /// save() used when `path` is empty.
    bool save(const QString &path = QString(), QString *error = nullptr);
    /// The bytes save() would write, without touching disk.
    QByteArray serialize() const;

    QString path() const { return m_path; }
    /// The file was renamed or moved: save() with no path writes there from now on.
    void setPath(const QString &path) { m_path = path; }

    const QList<CanvasNode> &nodes() const { return m_nodes; }
    const QList<CanvasEdge> &edges() const { return m_edges; }

    const CanvasNode *node(const QString &id) const;
    const CanvasEdge *edge(const QString &id) const;
    int nodeIndex(const QString &id) const { return m_nodeIndex.value(id, -1); }
    int edgeIndex(const QString &id) const { return m_edgeIndex.value(id, -1); }

    /// Appends, assigning a fresh id first if `node.id()` is empty or already
    /// taken. Returns the id it ended up with.
    QString addNode(CanvasNode node);
    bool removeNode(const QString &id);
    /// Re-inserts a previously removed node at `index` - what undo does.
    void insertNode(int index, const CanvasNode &node);

    QString addEdge(CanvasEdge edge);
    bool removeEdge(const QString &id);
    void insertEdge(int index, const CanvasEdge &edge);

    /// The only two fields an interactive edit changes on an existing node:
    /// position/size by dragging, and a text node's body by editing it in
    /// place. Everything else a node carries only ever comes from the file
    /// or from a freshly built CanvasNode passed to addNode/insertNode.
    bool setNodeRect(const QString &id, const QRectF &rect);
    bool setNodeText(const QString &id, const QString &text);

    /// A fresh, unused id - a 128-bit UUID with no braces or dashes, which is
    /// all the spec asks of a node/edge id ("unique identifier").
    QString newID() const;

private:
    const CanvasNode *nodeConst(const QString &id) const;
    void reindex();

    QString m_path;
    QByteArray m_originalBytes;
    bool m_dirty = false;
    QJsonObject m_root; ///< keeps root-level keys beyond "nodes"/"edges"
    QList<CanvasNode> m_nodes;
    QList<CanvasEdge> m_edges;
    QHash<QString, int> m_nodeIndex;
    QHash<QString, int> m_edgeIndex;
};

} // namespace noteAppC
