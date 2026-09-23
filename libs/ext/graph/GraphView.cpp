#include "GraphView.h"

#include <QCheckBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSet>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "Index.h"
#include "Theme.h"

namespace noteAppC {

namespace {

/// The three query forms documented on GraphView: path:, tag:, and a bare
/// substring of the file name. Anything else (quotes, booleans, content:)
/// is out of scope - see the class comment.
bool matchesQuery(const GraphNode &node, const QString &query)
{
    if (query.isEmpty())
        return false;
    if (query.startsWith(QLatin1String("path:")))
        return node.id.contains(query.mid(5), Qt::CaseInsensitive);
    if (query.startsWith(QLatin1String("tag:"))) {
        const QString wanted = query.mid(4);
        for (const QString &tag : node.tags) {
            if (tag.compare(wanted, Qt::CaseInsensitive) == 0
                || tag.startsWith(wanted + QLatin1Char('/'), Qt::CaseInsensitive))
                return true;
        }
        return false;
    }
    return node.label.contains(query, Qt::CaseInsensitive);
}

// Alpha-blends `color` at `alpha` (0-255) over `background` and returns an
// opaque result - measured on the performance guard's 5 000-node graph,
// asking the raster engine to alpha-composite ~10 000 semi-transparent
// lines cost about 100 ms against about 26 ms opaque (a plain microbenchmark
// isolated this to the blend itself, not line count or pen width: read the
// destination pixel, blend, write back, versus just writing it - the classic
// SourceOver-vs-Source cost). Pre-blending once here and drawing the result
// as an ordinary opaque colour looks identical wherever it is painted over
// a flat fill of `background`, which every edge and every unhighlighted
// node in this view is, since paintEvent() fills the background solid
// before drawing anything else. The one place this is not quite exact is
// where two translucent primitives would have overlapped each other (not
// the background) - a rare, few-pixel difference at a line crossing that
// is not worth 4x the frame budget to get exactly right.
QColor blendOverBackground(QColor color, int alpha, QColor background)
{
    const double a = alpha / 255.0;
    const auto mix = [a](int fg, int bg) { return int(std::lround(fg * a + bg * (1.0 - a))); };
    return QColor(mix(color.red(), background.red()), mix(color.green(), background.green()),
                 mix(color.blue(), background.blue()));
}

} // namespace

// ---------------------------------------------------------------------------
// GraphOptions

GraphOptions GraphOptions::readFromVault(const QString &vaultRoot)
{
    GraphOptions options;

    QFile file(vaultRoot + QLatin1String("/.obsidian/graph.json"));
    if (!file.open(QIODevice::ReadOnly))
        return options;

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const auto boolField = [&](const char *key, bool fallback) {
        return root.contains(QLatin1String(key)) ? root.value(QLatin1String(key)).toBool() : fallback;
    };
    const auto numberField = [&](const char *key, double fallback) {
        return root.contains(QLatin1String(key)) ? root.value(QLatin1String(key)).toDouble() : fallback;
    };

    options.showTags = boolField("showTags", options.showTags);
    options.showAttachments = boolField("showAttachments", options.showAttachments);
    options.hideUnresolved = boolField("hideUnresolved", options.hideUnresolved);
    options.showOrphans = boolField("showOrphans", options.showOrphans);
    options.showArrow = boolField("showArrow", options.showArrow);
    options.textFadeMultiplier = numberField("textFadeMultiplier", options.textFadeMultiplier);
    options.nodeSizeMultiplier = numberField("nodeSizeMultiplier", options.nodeSizeMultiplier);
    options.lineSizeMultiplier = numberField("lineSizeMultiplier", options.lineSizeMultiplier);
    options.centerStrength = numberField("centerStrength", options.centerStrength);
    options.repelStrength = numberField("repelStrength", options.repelStrength);
    options.linkStrength = numberField("linkStrength", options.linkStrength);
    options.linkDistance = numberField("linkDistance", options.linkDistance);

    // Obsidian's own shape: [{query, color:{a, rgb}}], rgb a packed 24-bit int.
    for (const QJsonValue &value : root.value(QLatin1String("colorGroups")).toArray()) {
        const QJsonObject group = value.toObject();
        const QString query = group.value(QLatin1String("query")).toString();
        if (query.isEmpty())
            continue;
        const int rgb = group.value(QLatin1String("color")).toObject().value(QLatin1String("rgb")).toInt();
        options.colorGroups.append(GraphColorGroup{query, QColor::fromRgb(rgb)});
    }

    return options;
}

// ---------------------------------------------------------------------------
// GraphView

GraphView::GraphView(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumSize(120, 120);
    m_labelFont = font();
    m_labelFont.setPointSizeF(std::max(7.0, font().pointSizeF() - 1.0));

    m_controlsToggle = new QToolButton(this);
    m_controlsToggle->setText(tr("Filters"));
    m_controlsToggle->setCheckable(true);
    m_controlsToggle->setCursor(Qt::PointingHandCursor);

    m_controlsPanel = buildControlsPanel();
    m_controlsPanel->setVisible(false);
    connect(m_controlsToggle, &QToolButton::toggled, m_controlsPanel, &QWidget::setVisible);

    m_controlsToggle->raise();
    m_controlsPanel->raise();
}

GraphView::~GraphView() = default;

QWidget *GraphView::buildControlsPanel()
{
    auto *panel = new QWidget(this);
    auto *layout = new QFormLayout(panel);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

    const auto addToggle = [&](const QString &label, bool checked, auto onToggled) {
        auto *box = new QCheckBox(panel);
        box->setChecked(checked);
        connect(box, &QCheckBox::toggled, this, onToggled);
        layout->addRow(label, box);
    };
    const auto addSlider = [&](const QString &label, int value, int min, int max, auto onChanged) {
        auto *slider = new QSlider(Qt::Horizontal, panel);
        slider->setRange(min, max);
        slider->setValue(value);
        slider->setFixedWidth(120);
        connect(slider, &QSlider::valueChanged, this, onChanged);
        layout->addRow(label, slider);
    };

    // Filters: each one re-derives the node/edge lists from the index, same
    // as a fresh setIndex() call. A caller feeding raw data via
    // setGraphData() instead (no index attached) simply sees no effect here,
    // since there is nothing of ours left to re-filter.
    addToggle(tr("Tags"), m_options.showTags, [this](bool on) {
        m_options.showTags = on;
        applyIndexData();
    });
    addToggle(tr("Attachments"), m_options.showAttachments, [this](bool on) {
        m_options.showAttachments = on;
        applyIndexData();
    });
    addToggle(tr("Unresolved links"), !m_options.hideUnresolved, [this](bool on) {
        m_options.hideUnresolved = !on;
        applyIndexData();
    });
    addToggle(tr("Orphans"), m_options.showOrphans, [this](bool on) {
        m_options.showOrphans = on;
        applyIndexData();
    });
    addToggle(tr("Arrows"), m_options.showArrow, [this](bool on) {
        m_options.showArrow = on;
        update();
    });

    // Forces: these only need the layout to notice and to start moving
    // again, not a full rebuild - dragging a node does the same wake().
    const auto pushForceOptions = [this] {
        ForceLayout::Options forceOptions = m_layout.options();
        forceOptions.repelStrength = m_options.repelStrength;
        forceOptions.linkStrength = m_options.linkStrength;
        forceOptions.linkDistance = m_options.linkDistance;
        forceOptions.centerStrength = m_options.centerStrength;
        m_layout.setOptions(forceOptions);
        m_layout.wake();
        ensureTimerRunning();
    };
    addSlider(tr("Repel force"), int(m_options.repelStrength), 0, 40, [this, pushForceOptions](int v) {
        m_options.repelStrength = v;
        pushForceOptions();
    });
    addSlider(tr("Link force"), int(m_options.linkStrength * 10), 0, 50, [this, pushForceOptions](int v) {
        m_options.linkStrength = v / 10.0;
        pushForceOptions();
    });
    addSlider(tr("Link distance"), int(m_options.linkDistance), 20, 600, [this, pushForceOptions](int v) {
        m_options.linkDistance = v;
        pushForceOptions();
    });
    addSlider(tr("Center force"), int(m_options.centerStrength * 100), 0, 200, [this, pushForceOptions](int v) {
        m_options.centerStrength = v / 100.0;
        pushForceOptions();
    });

    return panel;
}

void GraphView::layoutControls()
{
    if (!m_controlsToggle)
        return;
    m_controlsToggle->adjustSize();
    m_controlsToggle->move(width() - m_controlsToggle->width() - 8, 8);
    if (m_controlsPanel) {
        m_controlsPanel->adjustSize();
        m_controlsPanel->move(width() - m_controlsPanel->width() - 8,
                              m_controlsToggle->y() + m_controlsToggle->height() + 4);
    }
}

void GraphView::setTheme(const Theme &theme)
{
    m_backgroundColor = theme.color(QStringLiteral("--background-primary"));
    m_nodeColor = theme.color(QStringLiteral("--text-muted"));
    m_edgeColor = theme.color(QStringLiteral("--background-modifier-border"));
    m_textColor = theme.color(QStringLiteral("--text-normal"));
    m_mutedColor = theme.color(QStringLiteral("--text-faint"));
    m_accentColor = theme.color(QStringLiteral("--interactive-accent"));
    update();
}

void GraphView::setOptions(const GraphOptions &options)
{
    m_options = options;
    if (m_index)
        applyIndexData();
    else
        rebuild();
}

void GraphView::setIndex(Index *index)
{
    if (m_index)
        disconnect(m_index, nullptr, this, nullptr);
    m_index = index;
    if (m_index)
        connect(m_index, &Index::changed, this, &GraphView::applyIndexData);
    applyIndexData();
}

void GraphView::applyIndexData()
{
    if (!m_index) {
        setGraphData({}, {});
        return;
    }

    const Index::GraphData data = m_index->graphData();

    QHash<QString, GraphNode> byId;
    byId.reserve(data.files.size());
    for (const Index::GraphFileInfo &file : data.files) {
        if (file.isAttachment && !m_options.showAttachments)
            continue;
        GraphNode node;
        node.id = file.path;
        node.label = QFileInfo(file.path).completeBaseName();
        node.kind = file.isAttachment ? GraphNode::Kind::Attachment : GraphNode::Kind::Note;
        node.tags = file.tags;
        byId.insert(node.id, node);
    }

    if (m_options.showTags) {
        // Snapshot first: byId gains a tag entry inside this loop, and
        // iterating a QHash while inserting into it is unsafe (a rehash can
        // invalidate the iteration).
        const QList<GraphNode> notes = byId.values();
        for (const GraphNode &note : notes) {
            for (const QString &tag : note.tags) {
                const QString tagId = QStringLiteral("tag:") + tag;
                if (!byId.contains(tagId)) {
                    GraphNode tagNode;
                    tagNode.id = tagId;
                    tagNode.label = QLatin1Char('#') + tag;
                    tagNode.kind = GraphNode::Kind::Tag;
                    byId.insert(tagId, tagNode);
                }
            }
        }
    }

    // Orphans are not filtered here: rebuild() drops any node (of any kind)
    // that ends up with no edges once showOrphans is off, which comes to the
    // same thing - a tag or unresolved node only ever exists because
    // something links to it, so it is never an orphan by construction - and
    // means setGraphData() callers get the same filter for free.
    QList<GraphEdge> edges;
    for (const Index::GraphLinkInfo &link : data.links) {
        if (!byId.contains(link.fromPath))
            continue; // the source itself was filtered out (e.g. an attachment, hidden)

        if (!link.toPath.isEmpty()) {
            if (!byId.contains(link.toPath))
                continue; // the target was filtered out too
            edges.append({link.fromPath, link.toPath});
        } else if (!m_options.hideUnresolved) {
            const QString unresolvedID = QStringLiteral("unresolved:") + link.targetName;
            if (!byId.contains(unresolvedID)) {
                GraphNode node;
                node.id = unresolvedID;
                node.label = link.targetName;
                node.kind = GraphNode::Kind::Unresolved;
                byId.insert(unresolvedID, node);
            }
            edges.append({link.fromPath, unresolvedID});
        }
    }

    if (m_options.showTags) {
        for (auto it = byId.constBegin(); it != byId.constEnd(); ++it) {
            if (it->kind != GraphNode::Kind::Note)
                continue;
            for (const QString &tag : it->tags)
                edges.append({it->id, QStringLiteral("tag:") + tag});
        }
    }

    setGraphData(byId.values(), edges);
}

void GraphView::setGraphData(const QList<GraphNode> &nodes, const QList<GraphEdge> &edges)
{
    m_rawNodes = nodes;
    m_rawEdges = edges;
    rebuild();
}

void GraphView::setLocalGraph(bool local, const QString &path, int depth)
{
    m_localMode = local;
    if (!path.isEmpty())
        m_currentNote = path;
    m_options.localDepth = std::max(1, depth);
    rebuild();
}

void GraphView::setCurrentNote(const QString &relativePath)
{
    m_currentNote = relativePath;
    if (m_localMode)
        rebuild();
    else
        update();
}

void GraphView::rebuild()
{
    QList<GraphNode> nodes = m_rawNodes;
    QList<GraphEdge> edges = m_rawEdges;

    if (m_localMode && !m_currentNote.isEmpty()) {
        QHash<QString, QStringList> adjacency;
        for (const GraphEdge &edge : std::as_const(edges)) {
            adjacency[edge.fromID].append(edge.toID);
            adjacency[edge.toID].append(edge.fromID);
        }

        QSet<QString> visited{m_currentNote};
        QStringList frontier{m_currentNote};
        for (int hop = 0; hop < m_options.localDepth && !frontier.isEmpty(); ++hop) {
            QStringList next;
            for (const QString &id : std::as_const(frontier)) {
                for (const QString &neighbour : adjacency.value(id)) {
                    if (!visited.contains(neighbour)) {
                        visited.insert(neighbour);
                        next.append(neighbour);
                    }
                }
            }
            frontier = next;
        }

        QList<GraphNode> localNodes;
        for (const GraphNode &node : std::as_const(nodes)) {
            if (visited.contains(node.id))
                localNodes.append(node);
        }
        QList<GraphEdge> localEdges;
        for (const GraphEdge &edge : std::as_const(edges)) {
            if (visited.contains(edge.fromID) && visited.contains(edge.toID))
                localEdges.append(edge);
        }
        nodes = localNodes;
        edges = localEdges;
    }

    // A first pass over the un-filtered set, purely to learn each node's
    // degree - orphan filtering (showOrphans) needs to know that before it
    // can decide who stays, and it applies uniformly here rather than in
    // applyIndexData(), so setGraphData() callers get it for free too. A tag
    // or unresolved node is never degree 0 by construction (see
    // applyIndexData()), so this never needs a kind-based exception.
    QHash<QString, int> indexOf;
    indexOf.reserve(nodes.size());
    for (int i = 0; i < nodes.size(); ++i)
        indexOf.insert(nodes[i].id, i);

    QVector<int> degree(nodes.size(), 0);
    QList<std::pair<int, int>> rawEdges;
    rawEdges.reserve(edges.size());
    for (const GraphEdge &edge : std::as_const(edges)) {
        const int a = indexOf.value(edge.fromID, -1);
        const int b = indexOf.value(edge.toID, -1);
        if (a < 0 || b < 0 || a == b)
            continue;
        rawEdges.append({a, b});
        ++degree[a];
        ++degree[b];
    }

    QVector<int> remap(nodes.size(), -1);
    m_nodes.clear();
    m_nodes.reserve(nodes.size());
    for (int i = 0; i < nodes.size(); ++i) {
        if (!m_options.showOrphans && degree[i] == 0)
            continue;
        remap[i] = m_nodes.size();
        m_nodes.append(VisualNode{nodes[i], degree[i]});
    }

    m_idToIndex.clear();
    m_idToIndex.reserve(m_nodes.size());
    for (int i = 0; i < m_nodes.size(); ++i)
        m_idToIndex.insert(m_nodes[i].data.id, i);

    m_edgeIndices.clear();
    QList<std::pair<int, int>> layoutEdges;
    layoutEdges.reserve(rawEdges.size());
    for (const auto &edge : std::as_const(rawEdges)) {
        const int a = remap[edge.first];
        const int b = remap[edge.second];
        if (a < 0 || b < 0)
            continue; // an endpoint was orphan-filtered - never happens since an edge implies degree > 0, kept for safety
        m_edgeIndices.append({a, b});
        layoutEdges.append({a, b});
    }

    ForceLayout::Options forceOptions;
    forceOptions.repelStrength = m_options.repelStrength;
    forceOptions.linkStrength = m_options.linkStrength;
    forceOptions.linkDistance = m_options.linkDistance;
    forceOptions.centerStrength = m_options.centerStrength;
    m_layout.setOptions(forceOptions);
    m_layout.reset(m_nodes.size(), layoutEdges);

    m_hoverNode = -1;
    m_dragNode = -1;
    ensureTimerRunning();
    update();
}

void GraphView::stepSimulation(int steps)
{
    for (int i = 0; i < steps; ++i)
        m_layout.step();
    update();
}

void GraphView::ensureTimerRunning()
{
    if (m_nodes.isEmpty() || !isVisible())
        return;

    if (!m_stepTimer) {
        m_stepTimer = new QTimer(this);
        connect(m_stepTimer, &QTimer::timeout, this, [this] {
            // A few steps per frame on a small graph so it settles in a
            // couple of seconds rather than needing patience; one step is
            // already the right amount of work once the graph is big, since
            // each is O(n log n) - see GraphLayout.h.
            const int stepsPerFrame = m_nodes.size() > 1000 ? 1 : 3;
            for (int i = 0; i < stepsPerFrame; ++i)
                m_layout.step();
            update();
            if (m_layout.isSettled() && m_dragNode < 0)
                m_stepTimer->stop(); // the picture stopped moving - stop paying for it
        });
    }
    if (!m_stepTimer->isActive())
        m_stepTimer->start(16);
}

void GraphView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_layout.isSettled())
        ensureTimerRunning();
}

void GraphView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (m_stepTimer)
        m_stepTimer->stop(); // nobody is looking - an unseen graph must not animate for no one
}

void GraphView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutControls();
}

QPointF GraphView::toScreen(QPointF graphPos) const
{
    return graphPos * m_zoom + m_pan + QPointF(width() / 2.0, height() / 2.0);
}

QPointF GraphView::toGraph(QPointF screenPos) const
{
    return (screenPos - m_pan - QPointF(width() / 2.0, height() / 2.0)) / m_zoom;
}

double GraphView::radiusFor(int degree) const
{
    // Sub-linear in degree so a hundred-link hub reads as bigger, not ten
    // times the area of a one-link note.
    return (3.0 + std::sqrt(double(std::max(0, degree))) * 1.6) * m_options.nodeSizeMultiplier;
}

QColor GraphView::colorFor(const VisualNode &node) const
{
    for (const GraphColorGroup &group : std::as_const(m_options.colorGroups)) {
        if (matchesQuery(node.data, group.query))
            return group.color;
    }
    switch (node.data.kind) {
    case GraphNode::Kind::Tag:
        return m_accentColor;
    case GraphNode::Kind::Attachment:
    case GraphNode::Kind::Unresolved:
        return m_mutedColor;
    case GraphNode::Kind::Note:
    default:
        return m_nodeColor;
    }
}

int GraphView::nodeAt(QPointF screenPos) const
{
    for (int i = m_nodes.size() - 1; i >= 0; --i) {
        const QPointF center = toScreen(m_layout.position(i));
        const double radius = radiusFor(m_nodes[i].degree) * m_zoom + 3.0;
        const QPointF delta = screenPos - center;
        if (QPointF::dotProduct(delta, delta) <= radius * radius)
            return i;
    }
    return -1;
}

void GraphView::updateHover(QPointF screenPos)
{
    const int hit = nodeAt(screenPos);
    if (hit != m_hoverNode) {
        m_hoverNode = hit;
        setCursor(hit >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void GraphView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), m_backgroundColor);
    if (m_nodes.isEmpty())
        return;

    // Antialiasing is real cost per primitive; a graph big enough to need
    // the performance guard in tests/test_graph.cpp does not get it.
    painter.setRenderHint(QPainter::Antialiasing, m_nodes.size() < 1500);

    const QRectF viewport = rect();

    QSet<int> highlighted;
    if (m_hoverNode >= 0) {
        highlighted.insert(m_hoverNode);
        for (const auto &edge : std::as_const(m_edgeIndices)) {
            if (edge.first == m_hoverNode)
                highlighted.insert(edge.second);
            else if (edge.second == m_hoverNode)
                highlighted.insert(edge.first);
        }
    }
    const bool dimming = m_hoverNode >= 0;

    // Batched rather than one setPen()+drawLine() per edge: at the
    // performance guard's scale (~10 000 edges) that was three quarters of
    // the frame budget by itself (drawLines() lets the raster engine walk
    // one pen change instead of ten thousand). Edges only ever fall into two
    // buckets - dimmed or not - so two passes, each collecting its lines
    // first and drawing them in one call, replace what used to be a
    // per-edge state change.
    QVector<QLineF> normalLines, dimLines;
    normalLines.reserve(m_edgeIndices.size());
    for (const auto &edge : std::as_const(m_edgeIndices)) {
        const QPointF a = toScreen(m_layout.position(edge.first));
        const QPointF b = toScreen(m_layout.position(edge.second));
        if (!viewport.intersects(QRectF(a, b).normalized().adjusted(-4, -4, 4, 4)))
            continue; // culled - neither endpoint is anywhere near the viewport

        const bool dim = dimming && !(highlighted.contains(edge.first) && highlighted.contains(edge.second));
        (dim ? dimLines : normalLines).append(QLineF(a, b));

        if (m_options.showArrow) {
            const QPointF direction = b - a;
            const double length = std::hypot(direction.x(), direction.y());
            if (length > 1.0) {
                const QPointF unit = direction / length;
                const double targetRadius = radiusFor(m_nodes[edge.second].degree) * m_zoom;
                const QPointF tip = b - unit * (targetRadius + 2.0);
                const QPointF normal(-unit.y(), unit.x());
                const double headSize = 4.0 + m_options.lineSizeMultiplier;
                const QPointF left = tip - unit * headSize + normal * (headSize * 0.6);
                const QPointF right = tip - unit * headSize - normal * (headSize * 0.6);
                painter.setBrush(blendOverBackground(m_edgeColor, dim ? 40 : 160, m_backgroundColor));
                painter.setPen(Qt::NoPen);
                const QPointF triangle[3] = {tip, left, right};
                painter.drawPolygon(triangle, 3);
            }
        }
    }
    QPen edgePen;
    edgePen.setWidthF(std::max(0.4, m_options.lineSizeMultiplier * m_zoom));
    if (!normalLines.isEmpty()) {
        edgePen.setColor(blendOverBackground(m_edgeColor, 160, m_backgroundColor));
        painter.setPen(edgePen);
        painter.drawLines(normalLines);
    }
    if (!dimLines.isEmpty()) {
        edgePen.setColor(blendOverBackground(m_edgeColor, 40, m_backgroundColor));
        painter.setPen(edgePen);
        painter.drawLines(dimLines);
    }

    // Text fade threshold: Obsidian reveals labels once zoomed in enough (or
    // hovered). textFadeMultiplier shifts the zoom level that counts as
    // "enough"; we do not know Obsidian's exact curve, so this is a
    // deliberately simple, documented approximation of it.
    const double labelZoomThreshold = m_options.textFadeMultiplier > 0.0 ? m_options.textFadeMultiplier : 1.4;
    const bool showAllLabels = m_zoom >= labelZoomThreshold;
    painter.setFont(m_labelFont);

    // Most nodes share one colour (no colour groups, all Kind::Note), so the
    // usual case is one setBrush() for the whole graph rather than one per
    // node - setPen(Qt::NoPen) similarly only needs to happen once, since
    // nothing in this loop changes the pen unless a ring or a label is
    // about to be drawn, both rare at this scale.
    painter.setPen(Qt::NoPen);
    QColor currentBrush;
    bool haveBrush = false;
    // A second, node-specific level of detail, at the same node count that
    // turns antialiasing off: a settled layout packs its nodes into a
    // cluster far smaller than the viewport (Obsidian's does too), so at
    // performance-guard scale thousands of small circles heavily overlap
    // one another. Measured on that exact case, drawEllipse() cost 6.8 ms
    // for 5 000 overlapping dots against 0.2 ms for the equivalent filled
    // squares - true ellipse rasterisation revisits covered pixels per
    // shape where a rect fill does not. A dot this small reads the same
    // either way once antialiasing is off, which is only true past the
    // same node count this is gated on.
    const bool simplifiedDots = m_nodes.size() >= 1500;
    for (int i = 0; i < m_nodes.size(); ++i) {
        const VisualNode &node = m_nodes[i];
        const QPointF center = toScreen(m_layout.position(i));
        const double radius = radiusFor(node.degree) * m_zoom;
        const QRectF bounds(center.x() - radius, center.y() - radius, radius * 2, radius * 2);
        if (!viewport.intersects(bounds.adjusted(-50, -20, 50, 24)))
            continue; // culled - padded for the label that might sit beside it

        const bool dim = dimming && !highlighted.contains(i);
        QColor fill = colorFor(node);
        if (dim)
            fill.setAlpha(60);
        if (simplifiedDots) {
            painter.fillRect(bounds, fill);
        } else {
            if (!haveBrush || fill != currentBrush) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(fill);
                currentBrush = fill;
                haveBrush = true;
            }
            painter.drawEllipse(center, radius, radius);
        }

        if (node.data.id == m_currentNote) {
            painter.setPen(QPen(m_accentColor, 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, radius + 3, radius + 3);
            painter.setPen(Qt::NoPen); // state left dirty for the next node's ellipse
            haveBrush = false;         // and the brush, since NoBrush was just set too
        }

        const bool showLabel =
            !dim && (showAllLabels || i == m_hoverNode || highlighted.contains(i) || node.data.id == m_currentNote);
        if (showLabel && !node.data.label.isEmpty()) {
            painter.setPen(m_textColor);
            painter.drawText(QRectF(center.x() - 70, center.y() + radius + 2, 140, 16),
                             Qt::AlignHCenter | Qt::AlignTop, node.data.label);
            painter.setPen(Qt::NoPen);
        }
    }
}

void GraphView::wheelEvent(QWheelEvent *event)
{
    const QPointF cursor = event->position();
    const QPointF graphPointUnderCursor = toGraph(cursor);

    const double factor = std::pow(1.0015, event->angleDelta().y());
    m_zoom = std::clamp(m_zoom * factor, 0.05, 8.0);

    // Re-derive pan so the point under the cursor stays under it - zooming
    // "about the pointer" rather than about the widget's centre.
    m_pan += cursor - toScreen(graphPointUnderCursor);

    event->accept();
    update();
}

void GraphView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_pressPos = event->position();
    const int hit = nodeAt(event->position());
    if (hit >= 0) {
        m_dragNode = hit;
        m_layout.setPinned(hit, true);
        m_layout.wake();
        ensureTimerRunning();
    } else {
        m_panning = true;
        m_panStart = event->position();
        m_panOrigin = m_pan;
    }
}

void GraphView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragNode >= 0) {
        m_layout.setPosition(m_dragNode, toGraph(event->position()));
        m_layout.wake();
        ensureTimerRunning();
        update();
        return;
    }
    if (m_panning) {
        m_pan = m_panOrigin + (event->position() - m_panStart);
        update();
        return;
    }
    updateHover(event->position());
}

void GraphView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    const bool wasClick = (event->position() - m_pressPos).manhattanLength() < 4.0;
    if (m_dragNode >= 0) {
        if (wasClick) {
            const GraphNode &data = m_nodes[m_dragNode].data;
            if (data.kind == GraphNode::Kind::Note || data.kind == GraphNode::Kind::Attachment)
                emit noteActivated(data.id);
        }
        m_layout.setPinned(m_dragNode, false);
        m_layout.wake();
        ensureTimerRunning();
        m_dragNode = -1;
    }
    m_panning = false;
}

void GraphView::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_hoverNode >= 0) {
        m_hoverNode = -1;
        update();
    }
}

} // namespace noteAppC
