#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <utility>

#include "GraphLayout.h"

class QMouseEvent;
class QResizeEvent;
class QShowEvent;
class QHideEvent;
class QTimer;
class QToolButton;
class QWheelEvent;

namespace noteAppC {

class Index;
class Theme;

/// One node the graph can show: a note, a tag, an attachment, or a link that
/// resolved to nothing (Obsidian still draws these, dimmer, so a vault owner
/// can see what to create). `id` is the stable key GraphView keys everything
/// by - a vault-relative path for a note or attachment, `"tag:name"` for a
/// tag, `"unresolved:text"` for a link with nowhere to go.
struct GraphNode {
    enum class Kind { Note, Tag, Attachment, Unresolved };

    QString id;
    QString label;
    Kind kind = Kind::Note;
    QStringList tags; ///< a note's own tags, so a `tag:` colour-group query can match it
};

/// One edge, `fromID`/`toID` naming two GraphNode::id values from the same list.
struct GraphEdge {
    QString fromID;
    QString toID;
};

/// A colour group: every node whose id/tags/label matches `query` is tinted
/// `color`, first match wins. See GraphView's class comment for exactly which
/// queries are understood - it is a small, deliberate subset.
struct GraphColorGroup {
    QString query;
    QColor color;
};

/// `.obsidian/graph.json`, read by GraphOptions::readFromVault(), or
/// GraphOptions::defaults() when there is none. Field names match the file's
/// own keys so a value copied out of Obsidian's graph settings drops
/// straight in.
struct GraphOptions {
    bool showTags = false;
    bool showAttachments = false;
    bool hideUnresolved = false;
    bool showOrphans = true;
    bool showArrow = false;
    double textFadeMultiplier = 0.0;
    double nodeSizeMultiplier = 1.0;
    double lineSizeMultiplier = 1.0;
    double centerStrength = 0.5;
    double repelStrength = 10.0;
    double linkStrength = 1.0;
    double linkDistance = 250.0;
    QList<GraphColorGroup> colorGroups;
    int localDepth = 1;

    static GraphOptions defaults() { return GraphOptions{}; }

    /// Reads `<vaultRoot>/.obsidian/graph.json` when present, the way
    /// Vault::loadIgnoreFilters() reads app.json; defaults() otherwise, and
    /// for any field the file does not set. Unknown keys are ignored.
    static GraphOptions readFromVault(const QString &vaultRoot);
};

/// The graph view: global (every note in the vault) or local (one note's
/// neighbourhood to a given depth), laid out by ForceLayout and painted with
/// QPainter. Self-contained - it never reaches into MainWindow; the caller
/// wires noteActivated() to actually open a note, and decides whether an
/// instance is the main-area global graph or a sidebar's local one via
/// setLocalGraph().
///
/// Costs nothing until there is data and the widget is shown: the QTimer
/// driving the simulation is created lazily on first use, stops the moment
/// the layout settles, and stops again in hideEvent() - a graph in a
/// background tab burns no CPU, and one with a hundred notes stops moving
/// within a couple of seconds and then costs nothing at all.
///
/// Colour groups understand three query forms without needing a search
/// engine (Obsidian's own grammar is a whole feature another engineer is
/// building in parallel): `path:text` (substring of the vault-relative
/// path), `tag:name` (an exact tag or one nested under it), and bare text
/// (substring of the note's file name). Left out: quoted phrases, boolean
/// operators, negation, and any `content:`/full-text match.
class GraphView : public QWidget {
    Q_OBJECT

public:
    explicit GraphView(QWidget *parent = nullptr);
    ~GraphView() override;

    void setTheme(const Theme &theme);

    void setOptions(const GraphOptions &options);
    const GraphOptions &options() const { return m_options; }

    /// Pulls Index::graphData() and rebuilds. Reconnects to Index::changed()
    /// so a vault edit refreshes the graph automatically; pass nullptr to
    /// detach (the graph then keeps showing whatever it last had). The Index
    /// is not owned.
    void setIndex(Index *index);
    Index *index() const { return m_index; }

    /// The direct data entry point, independent of Index - what setIndex()
    /// itself feeds through after filtering by options, and what a caller
    /// (or a test) with its own data can use instead.
    void setGraphData(const QList<GraphNode> &nodes, const QList<GraphEdge> &edges);

    /// Global: every node currently in the data. Local: `path`'s
    /// neighbourhood out to `depth` hops (both directions - Obsidian's local
    /// graph does not distinguish incoming from outgoing), meant for a
    /// sidebar pane. An empty `path` leaves the current note unchanged.
    void setLocalGraph(bool local, const QString &path = {}, int depth = 1);
    bool isLocalGraph() const { return m_localMode; }

    /// The active note: the local graph's centre, and the ring drawn around
    /// it in the global graph. Does not switch modes by itself, so the
    /// caller can update it on every note change and let setLocalGraph()
    /// decide separately whether that pane is in local mode at all.
    void setCurrentNote(const QString &relativePath);
    QString currentNote() const { return m_currentNote; }

    /// The rendered/simulated set after filtering and (in local mode) the
    /// depth cut - what a status label ("342 notes, 890 links") would show,
    /// and what tests/test_graph.cpp checks setLocalGraph()'s depth against.
    int nodeCount() const { return m_nodes.size(); }
    int edgeCount() const { return m_edgeIndices.size(); }
    bool containsNode(const QString &id) const { return m_idToIndex.contains(id); }

    /// Advances the simulation by `steps` steps immediately, instead of
    /// waiting for the timer to do a few per frame, and repaints once
    /// afterwards. Real usage never needs this - the timer in
    /// ensureTimerRunning() already does it - but it gives a performance
    /// test a step to time directly, through the same ForceLayout instance
    /// and options a shown GraphView would actually use.
    void stepSimulation(int steps = 1);

signals:
    /// A note or attachment node was clicked; `relativePath` is its
    /// GraphNode::id. Never fired for a tag or unresolved node - there is
    /// nothing to open.
    void noteActivated(const QString &relativePath);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct VisualNode {
        GraphNode data;
        int degree = 0;
    };

    void rebuild();
    void applyIndexData();
    void ensureTimerRunning();
    QWidget *buildControlsPanel();
    void layoutControls();

    QPointF toScreen(QPointF graphPos) const;
    QPointF toGraph(QPointF screenPos) const;
    int nodeAt(QPointF screenPos) const;
    void updateHover(QPointF screenPos);
    QColor colorFor(const VisualNode &node) const;
    double radiusFor(int degree) const;

    Index *m_index = nullptr;
    GraphOptions m_options;

    QColor m_backgroundColor{Qt::black};
    QColor m_nodeColor{Qt::lightGray};
    QColor m_edgeColor{Qt::darkGray};
    QColor m_textColor{Qt::white};
    QColor m_mutedColor{Qt::gray};
    QColor m_accentColor{Qt::cyan};

    bool m_localMode = false;
    QString m_currentNote;

    QList<GraphNode> m_rawNodes; // last full dataset, pre local-graph filtering
    QList<GraphEdge> m_rawEdges;

    QList<VisualNode> m_nodes; // the rendered/simulated set - all of m_rawNodes, or a local slice
    QList<std::pair<int, int>> m_edgeIndices; // into m_nodes
    QHash<QString, int> m_idToIndex;

    ForceLayout m_layout;
    QTimer *m_stepTimer = nullptr;

    QToolButton *m_controlsToggle = nullptr;
    QWidget *m_controlsPanel = nullptr;

    double m_zoom = 1.0;
    QPointF m_pan{0, 0};

    int m_hoverNode = -1;
    int m_dragNode = -1;
    QPointF m_pressPos;
    bool m_panning = false;
    QPointF m_panStart;
    QPointF m_panOrigin;

    QFont m_labelFont;
};

} // namespace noteAppC
