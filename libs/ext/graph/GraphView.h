#pragma once

#include <QColor>
#include <QFont>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "GraphLayout.h"

class QMouseEvent;
class QKeyEvent;
class QContextMenuEvent;
class QResizeEvent;
class QShowEvent;
class QHideEvent;
class QTimer;
class QToolButton;
class QLineEdit;
class QSlider;
class QCheckBox;
class QFormLayout;
class QVBoxLayout;
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
/// straight in. The app keeps its own copy of what the controls change
/// (toJson()/fromJson()); the vault's file is only ever read.
struct GraphOptions {
    QString search;          ///< the Filters search box: only files matching this (search grammar) stay
    bool animate = true;     ///< off: the layout is settled before the picture moves
    bool collapseFilter = true; ///< Obsidian's `collapse-*`: a controls section starts folded
    bool collapseColorGroups = true;
    bool collapseDisplay = true;
    bool collapseForces = true;
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
    /// The local graph follows links into the centre note (`localIncoming`) and
    /// out of it (`localOutgoing`), at every hop. Both on: the plain neighbourhood.
    /// These two key names are this app's own - no real Obsidian graph.json was
    /// at hand to confirm what Obsidian calls them.
    bool localIncoming = true;
    bool localOutgoing = true;

    static GraphOptions defaults() { return GraphOptions{}; }

    /// Obsidian's graph.json shape, plus `animate`; `localJumps` is the local
    /// graph's depth. Missing keys keep `base`'s value, unknown ones are ignored.
    static GraphOptions fromJson(const QJsonObject &root, const GraphOptions &base);
    static GraphOptions fromJson(const QJsonObject &root);
    QJsonObject toJson() const;

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
/// Threading: stepping the layout and painting a frame run on a worker
/// (QtConcurrent), because at 20 000 notes one step is ~30 ms and one frame
/// ~20 ms - together longer than the UI could spare. The contract:
///  - At most one job is in flight. The 16 ms timer is only a pacer: when it
///    fires and no job runs, and the layout is unsettled or the picture is
///    out of date (m_frameDirty), it launches one.
///  - A job owns m_layout while it runs. It steps, then paints a QImage
///    from a FrameInput snapshot (positions, nodes, edges, camera, hover,
///    colours - all values or implicitly shared). Its FrameOutput comes back
///    through the QFutureWatcher on the UI thread.
///  - The UI thread never reads m_layout. It reads m_positions, the snapshot
///    the last job returned, for hit tests, fitToView() and screenPosition().
///    It edits the layout (pin, move, wake, force options) only through
///    editLayout(): applied at once when idle, queued behind the job when not.
///  - rebuild() never waits. It gives the view a fresh ForceLayout (the job
///    holds the old one by shared_ptr) and bumps m_generation, so an output
///    from before the rebuild is dropped when its signal arrives; the job
///    sees the new generation in m_liveGeneration after the step it is on
///    and bails out without painting. The next job launches once the stale
///    one has been delivered. Only the destructor and stepSimulation() (a
///    test hook) wait for a job.
///  - paintEvent() only draws the last frame, mapped from the camera it was
///    rendered with to the current one, so panning and zooming move the old
///    picture instantly while the real frame catches up.
///
/// Colour groups are queries in Obsidian's search grammar (libs/index's
/// SearchQuery): OR, -negation, groups, `path:`, `file:`, `tag:`, regexes.
/// A node carries no note text, so a bare word or `content:` is matched
/// against its name.
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
    /// is not owned. `deferRead`: a graph that is not visible yet takes the
    /// first read when it is shown, not now (a pane built at start that nobody
    /// has opened must not read the whole vault).
    void setIndex(Index *index, bool deferRead = false);
    Index *index() const { return m_index; }

    /// The direct data entry point, independent of Index - what setIndex()
    /// itself feeds through after filtering by options, and what a caller
    /// (or a test) with its own data can use instead.
    void setGraphData(const QList<GraphNode> &nodes, const QList<GraphEdge> &edges);

    /// Global: every node currently in the data. Local: `path`'s
    /// neighbourhood out to `depth` hops, following incoming and outgoing
    /// links as GraphOptions::localIncoming / localOutgoing say, meant for a
    /// sidebar pane. An empty `path` leaves the current note unchanged.
    /// `depth` 0 keeps the current depth (the slider owns it).
    void setLocalGraph(bool local, const QString &path = {}, int depth = 0);
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
    /// Changes whenever the layout is replaced (a different graph), never for
    /// an index update that left the nodes and links as they were.
    int layoutGeneration() const { return m_generation; }
    /// The colour node `id` is drawn in: its colour group's, or its kind's.
    QColor nodeColor(const QString &id) const
    {
        const int index = m_idToIndex.value(id, -1);
        return index < 0 ? QColor() : colorFor(m_nodes.at(index));
    }
    /// Where node `id` is drawn, in widget pixels.
    QPointF screenPosition(const QString &id) const
    {
        return toScreen(m_positions.value(m_idToIndex.value(id, -1)));
    }

    /// Advances the simulation by `steps` steps immediately (waiting for a
    /// running job first), instead of leaving it to the timer a few per
    /// frame, and marks the picture out of date. Real usage never needs this
    /// - the timer in ensureTimerRunning() already does it - but it gives a
    /// performance test a step to time directly, through the same
    /// ForceLayout instance and options a shown GraphView would actually use.
    void stepSimulation(int steps = 1);

    /// Paints the current state to an image synchronously, the way a worker
    /// job does - for a test to time the frame without the widget machinery.
    QImage renderFrameNow() const;

signals:
    /// A control changed an option (not setOptions() - that is the caller's
    /// own write): the owner persists them.
    void optionsChanged();
    /// A note or attachment node was clicked; `relativePath` is its
    /// GraphNode::id. Never fired for a tag or unresolved node - there is
    /// nothing to open.
    void noteActivated(const QString &relativePath);
    /// The node's right-click menu chose "Open in new tab"; same ids as noteActivated().
    void noteActivatedInNewTab(const QString &relativePath);
    /// Escape on the global graph: the owner returns to the note it came from.
    /// Never fired by a local graph (a sidebar pane has nowhere to return to).
    void closeRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct VisualNode {
        GraphNode data;
        int degree = 0;
        int group = -1; ///< the first colour group it matches, or -1
    };

    using BuiltGraph = std::pair<QList<GraphNode>, QList<GraphEdge>>; ///< what a data job returns
    struct Palette {
        QColor background{Qt::black};
        QColor node{Qt::lightGray};
        QColor edge{Qt::darkGray};
        QColor text{Qt::white};
        QColor muted{Qt::gray};
        QColor accent{Qt::cyan};
    };

    /// Everything a frame is painted from, by value or implicitly shared, so a
    /// worker never reads a member the UI thread may be changing.
    struct FrameInput {
        QSize size;
        qreal dpr = 1.0;
        QVector<QPointF> positions;
        QList<VisualNode> nodes;
        QList<std::pair<int, int>> edges;
        double zoom = 1.0;
        QPointF pan;
        int hoverNode = -1;
        QString currentNote;
        GraphOptions options;
        Palette palette;
        QFont font;
        int generation = 0;
        int steps = 0; ///< layout steps to run before painting (a worker job only)
    };
    struct FrameOutput {
        QImage frame;
        QVector<QPointF> positions;
        bool settled = false;
        int generation = 0;
        double zoom = 1.0; ///< the camera the frame was painted with
        QPointF pan;
    };

    void rebuild();
    void assignColorGroups();
    void applyIndexData();
    void onGraphBuilt();
    void ensureTimerRunning();
    void fitToView();
    FrameInput makeFrameInput() const;
    static QImage paintGraph(const FrameInput &input);
    void launchFrame();
    void onFrameFinished();
    void editLayout(std::function<void(ForceLayout &)> edit);
    void flushLayoutEdits();
    void markDirty();
    QWidget *buildControlsPanel();
    void layoutControls();
    void syncControls();
    void showLocalRows(); ///< Depth and the two link switches: local mode only
    void rebuildGroupRows();
    void reapplyData();

    QPointF toScreen(QPointF graphPos) const;
    QPointF toGraph(QPointF screenPos) const;
    int nodeAt(QPointF screenPos) const;
    void updateHover(QPointF screenPos);
    static QColor colorFor(const VisualNode &node, const GraphOptions &options, const Palette &palette);
    QColor colorFor(const VisualNode &node) const { return colorFor(node, m_options, m_palette); }
    static double radiusFor(int degree, const GraphOptions &options);
    double radiusFor(int degree) const { return radiusFor(degree, m_options); }

    Index *m_index = nullptr;
    GraphOptions m_options;

    Palette m_palette;

    bool m_localMode = false;
    bool m_deferRead = false;    ///< setIndex(): no read of the vault while hidden, option changes included
    bool m_staleData = false;   ///< the index changed while hidden
    bool m_staleLayout = false; ///< the local graph's note changed while hidden
    QString m_currentNote;

    QList<GraphNode> m_rawNodes; // last full dataset, pre local-graph filtering
    QList<GraphEdge> m_rawEdges;

    QList<VisualNode> m_nodes; // the rendered/simulated set - all of m_rawNodes, or a local slice
    QList<std::pair<int, int>> m_edgeIndices; // into m_nodes
    QHash<QString, int> m_idToIndex;

    /// Owned by the job while one runs - see the class comment. A shared_ptr so
    /// rebuild() can replace it under a job that still holds the old one.
    std::shared_ptr<ForceLayout> m_layout = std::make_shared<ForceLayout>();
    /// The generation a job should still be working for; a job whose own
    /// generation differs has been superseded and stops stepping.
    std::shared_ptr<std::atomic<int>> m_liveGeneration = std::make_shared<std::atomic<int>>(0);
    QVector<QPointF> m_positions; ///< the UI thread's snapshot of m_layout
    QTimer *m_stepTimer = nullptr;

    QFutureWatcher<FrameOutput> m_frameWatcher;
    QFutureWatcher<BuiltGraph> m_dataWatcher; ///< applyIndexData()'s job
    int m_dataRequest = 0;        ///< bumped by every request and every direct setGraphData()
    int m_pendingDataRequest = 0; ///< the request the job in m_dataWatcher answers
    std::vector<std::function<void(ForceLayout &)>> m_layoutEdits; ///< queued while a job runs
    QImage m_frame;           ///< the last rendered picture, painted by paintEvent()
    double m_frameZoom = 1.0; ///< the camera m_frame was painted with
    QPointF m_framePan;
    bool m_frameDirty = false; ///< the picture no longer matches the state
    bool m_settled = true;     ///< the last job's verdict - never m_layout.isSettled() live
    bool m_jobInFlight = false; ///< launched and not yet delivered to onFrameFinished(): never launch over it
    bool m_woken = false;      ///< an edit woke the layout since the last launch, whatever that job says
    int m_generation = 0;
    QPointF m_dragPos;         ///< graph coordinates of the node being dragged

    QToolButton *m_controlsToggle = nullptr;
    QWidget *m_controlsPanel = nullptr;
    QWidget *m_groupRows = nullptr;
    QVBoxLayout *m_groupsLayout = nullptr;
    QFormLayout *m_filtersForm = nullptr;
    QSlider *m_depthSlider = nullptr;
    QCheckBox *m_localIncomingBox = nullptr; ///< local mode's Incoming/Outgoing switches
    QCheckBox *m_localOutgoingBox = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QTimer *m_searchDebounce = nullptr;
    QTimer *m_groupDebounce = nullptr; ///< a group query's keystrokes, before the nodes are rescanned
    std::vector<std::function<void()>> m_controlSyncs; ///< each pushes one option into its widget

    double m_zoom = 1.0;
    QPointF m_pan{0, 0};
    bool m_cameraMoved = false; ///< zoomed or panned by hand: stop fitting the view

    int m_hoverNode = -1;
    int m_dragNode = -1;
    QPointF m_pressPos;
    bool m_panning = false;
    QPointF m_panStart;
    QPointF m_panOrigin;

    QFont m_labelFont;
};

} // namespace noteAppC
