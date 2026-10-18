#include "GraphView.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QtConcurrent/QtConcurrentRun>
#include <QFileInfo>
#include <QImage>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineF>
#include <QContextMenuEvent>
#include <QKeyEvent>
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

#include "Icons.h"
#include "Index.h"
#include "Theme.h"

namespace noteAppC {

namespace {

/// The three query forms documented on GraphView: path:, tag:, and a bare
/// substring of the file name. Anything else (quotes, booleans, content:)
/// is out of scope - see the class comment.
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

/// The four sliders' share of the engine's options; theta, damping and the
/// time step stay at their defaults.
ForceLayout::Options forceOptionsFor(const GraphOptions &options)
{
    ForceLayout::Options forceOptions;
    forceOptions.repelStrength = options.repelStrength;
    forceOptions.linkStrength = options.linkStrength;
    forceOptions.linkDistance = options.linkDistance;
    forceOptions.centerStrength = options.centerStrength;
    return forceOptions;
}

/// Nodes and edges for `data` under `options`: runs on a worker, so it
/// reads nothing of the view's.
std::pair<QList<GraphNode>, QList<GraphEdge>> buildGraph(const Index::GraphData &data, const GraphOptions &options)
{
    QHash<QString, GraphNode> byId;
    byId.reserve(data.files.size());
    for (const Index::GraphFileInfo &file : data.files) {
        if (file.isAttachment && !options.showAttachments)
            continue;
        GraphNode node;
        node.id = file.path;
        node.label = QFileInfo(file.path).completeBaseName();
        node.kind = file.isAttachment ? GraphNode::Kind::Attachment : GraphNode::Kind::Note;
        node.tags = file.tags;
        byId.insert(node.id, node);
    }

    if (options.showTags) {
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
        } else if (!options.hideUnresolved) {
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

    if (options.showTags) {
        for (auto it = byId.constBegin(); it != byId.constEnd(); ++it) {
            if (it->kind != GraphNode::Kind::Note)
                continue;
            for (const QString &tag : it->tags)
                edges.append({it->id, QStringLiteral("tag:") + tag});
        }
    }

    return {byId.values(), edges};
}

/// What a search query is asked about a node: it has no note text, so a bare
/// word is looked for in the name.
NoteContext contextFor(const GraphNode &node)
{
    NoteContext context;
    context.path = node.id;
    context.text = node.label;
    for (const QString &tag : node.tags)
        context.tags << NoteTag{tag, Range{0, context.text.size()}};
    return context;
}

QPointF project(QPointF graphPos, double zoom, QPointF pan, QSize size)
{
    return graphPos * zoom + pan + QPointF(size.width() / 2.0, size.height() / 2.0);
}

} // namespace

// ---------------------------------------------------------------------------
// GraphOptions

GraphOptions GraphOptions::readFromVault(const QString &vaultRoot)
{
    QFile file(vaultRoot + QLatin1String("/.obsidian/graph.json"));
    if (!file.open(QIODevice::ReadOnly))
        return GraphOptions{};
    return fromJson(QJsonDocument::fromJson(file.readAll()).object());
}

GraphOptions GraphOptions::fromJson(const QJsonObject &root)
{
    return fromJson(root, GraphOptions{});
}

GraphOptions GraphOptions::fromJson(const QJsonObject &root, const GraphOptions &base)
{
    GraphOptions options = base;
    const auto boolField = [&](const char *key, bool fallback) {
        return root.contains(QLatin1String(key)) ? root.value(QLatin1String(key)).toBool() : fallback;
    };
    const auto numberField = [&](const char *key, double fallback) {
        return root.contains(QLatin1String(key)) ? root.value(QLatin1String(key)).toDouble() : fallback;
    };

    if (root.contains(QLatin1String("search")))
        options.search = root.value(QLatin1String("search")).toString();
    options.showTags = boolField("showTags", options.showTags);
    options.showAttachments = boolField("showAttachments", options.showAttachments);
    options.hideUnresolved = boolField("hideUnresolved", options.hideUnresolved);
    options.showOrphans = boolField("showOrphans", options.showOrphans);
    options.showArrow = boolField("showArrow", options.showArrow);
    options.animate = boolField("animate", options.animate);
    options.collapseFilter = boolField("collapse-filter", options.collapseFilter);
    options.collapseColorGroups = boolField("collapse-color-groups", options.collapseColorGroups);
    options.collapseDisplay = boolField("collapse-display", options.collapseDisplay);
    options.collapseForces = boolField("collapse-forces", options.collapseForces);
    options.textFadeMultiplier = numberField("textFadeMultiplier", options.textFadeMultiplier);
    options.nodeSizeMultiplier = numberField("nodeSizeMultiplier", options.nodeSizeMultiplier);
    options.lineSizeMultiplier = numberField("lineSizeMultiplier", options.lineSizeMultiplier);
    options.centerStrength = numberField("centerStrength", options.centerStrength);
    options.repelStrength = numberField("repelStrength", options.repelStrength);
    options.linkStrength = numberField("linkStrength", options.linkStrength);
    options.linkDistance = numberField("linkDistance", options.linkDistance);
    options.localDepth = std::max(1, int(numberField("localJumps", options.localDepth)));
    options.localIncoming = boolField("localIncoming", options.localIncoming);
    options.localOutgoing = boolField("localOutgoing", options.localOutgoing);

    // Obsidian's own shape: [{query, color:{a, rgb}}], rgb a packed 24-bit int.
    if (root.contains(QLatin1String("colorGroups"))) {
        options.colorGroups.clear();
        for (const QJsonValue &value : root.value(QLatin1String("colorGroups")).toArray()) {
            const QJsonObject group = value.toObject();
            const QString query = group.value(QLatin1String("query")).toString();
            if (query.isEmpty())
                continue;
            const int rgb = group.value(QLatin1String("color")).toObject().value(QLatin1String("rgb")).toInt();
            options.colorGroups.append(GraphColorGroup{query, QColor::fromRgb(rgb)});
        }
    }
    return options;
}

QJsonObject GraphOptions::toJson() const
{
    QJsonArray groups;
    for (const GraphColorGroup &group : colorGroups) {
        if (group.query.trimmed().isEmpty())
            continue; // a row still being typed in: nothing to keep yet
        groups.append(QJsonObject{{QLatin1String("query"), group.query},
                                  {QLatin1String("color"), QJsonObject{{QLatin1String("a"), 1},
                                                                       {QLatin1String("rgb"), int(group.color.rgb() & 0xFFFFFF)}}}});
    }
    return QJsonObject{{QLatin1String("collapse-filter"), collapseFilter},
                       {QLatin1String("search"), search},
                       {QLatin1String("showTags"), showTags},
                       {QLatin1String("showAttachments"), showAttachments},
                       {QLatin1String("hideUnresolved"), hideUnresolved},
                       {QLatin1String("showOrphans"), showOrphans},
                       {QLatin1String("collapse-color-groups"), collapseColorGroups},
                       {QLatin1String("colorGroups"), groups},
                       {QLatin1String("collapse-display"), collapseDisplay},
                       {QLatin1String("showArrow"), showArrow},
                       {QLatin1String("textFadeMultiplier"), textFadeMultiplier},
                       {QLatin1String("nodeSizeMultiplier"), nodeSizeMultiplier},
                       {QLatin1String("lineSizeMultiplier"), lineSizeMultiplier},
                       {QLatin1String("animate"), animate},
                       {QLatin1String("collapse-forces"), collapseForces},
                       {QLatin1String("centerStrength"), centerStrength},
                       {QLatin1String("repelStrength"), repelStrength},
                       {QLatin1String("linkStrength"), linkStrength},
                       {QLatin1String("linkDistance"), linkDistance},
                       {QLatin1String("localJumps"), localDepth},
                       {QLatin1String("localIncoming"), localIncoming},
                       {QLatin1String("localOutgoing"), localOutgoing}};
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
    m_controlsToggle->setObjectName(QStringLiteral("graphControlsToggle"));
    m_controlsToggle->setToolTip(tr("Graph settings"));
    m_controlsToggle->setIcon(Icons::icon(QStringLiteral("settings"), m_palette.text, 16));
    m_controlsToggle->setCheckable(true);
    m_controlsToggle->setAutoRaise(true);
    m_controlsToggle->setCursor(Qt::PointingHandCursor);

    m_controlsPanel = buildControlsPanel();
    m_controlsPanel->setVisible(false);
    connect(m_controlsToggle, &QToolButton::toggled, this, [this](bool open) {
        m_controlsPanel->setVisible(open);
        layoutControls();
    });

    m_controlsToggle->raise();
    m_controlsPanel->raise();

    connect(&m_frameWatcher, &QFutureWatcher<FrameOutput>::finished, this, &GraphView::onFrameFinished);
    connect(&m_dataWatcher, &QFutureWatcher<BuiltGraph>::finished, this, &GraphView::onGraphBuilt);
}

GraphView::~GraphView()
{
    m_frameWatcher.waitForFinished(); // the job paints from members' snapshots, but is not worth outliving
    m_dataWatcher.waitForFinished();
}

void GraphView::reapplyData()
{
    // Filters re-derive the node and edge lists from the index, same as a
    // fresh setIndex(). A caller feeding raw data through setGraphData()
    // has no index to re-read, so only the filters rebuild() owns apply.
    if (m_index)
        applyIndexData();
    else
        rebuild();
}

/// Obsidian's controls card: four folding sections (Filters, Groups, Display,
/// Forces) in a scrolling column pinned top-right. Every control writes one
/// option, tells the owner (optionsChanged(), which persists them) and
/// registers a sync lambda so setOptions() can put the widgets back in step.
QWidget *GraphView::buildControlsPanel()
{
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("graphControlsPanel"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("graphControlsBody"));
    scroll->setWidget(body);
    auto *column = new QVBoxLayout(body);
    column->setContentsMargins(10, 6, 10, 8);
    column->setSpacing(2);

    const auto addSection = [&](const QString &title, bool GraphOptions::*collapsed) {
        auto *header = new QToolButton(body);
        header->setText(title);
        header->setObjectName(QStringLiteral("graph%1Section").arg(title));
        header->setCheckable(true);
        header->setAutoRaise(true);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        header->setCursor(Qt::PointingHandCursor);
        header->setStyleSheet(QStringLiteral("QToolButton{font-weight:600;text-align:left;border:none;padding:4px 0;}"));
        auto *content = new QWidget(body);
        auto *form = new QFormLayout(content);
        form->setContentsMargins(0, 2, 0, 6);
        form->setSpacing(6);
        column->addWidget(header);
        column->addWidget(content);
        const auto show = [header, content](bool open) {
            header->setChecked(open);
            header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            content->setVisible(open);
        };
        connect(header, &QToolButton::toggled, this, [this, show, collapsed](bool open) {
            show(open);
            m_options.*collapsed = !open;
            emit optionsChanged();
            layoutControls();
        });
        show(!(m_options.*collapsed));
        m_controlSyncs.push_back([this, header, show, collapsed] {
            const QSignalBlocker blocker(header);
            show(!(m_options.*collapsed));
        });
        return form;
    };
    const auto addToggle = [&](QFormLayout *form, const QString &label, auto get, auto set) {
        auto *box = new QCheckBox(form->parentWidget());
        box->setChecked(get());
        connect(box, &QCheckBox::toggled, this, [this, set](bool on) {
            set(on);
            emit optionsChanged();
        });
        m_controlSyncs.push_back([box, get] {
            const QSignalBlocker blocker(box);
            box->setChecked(get());
        });
        form->addRow(label, box);
        return box;
    };
    const auto addSlider = [&](QFormLayout *form, const QString &label, int min, int max, auto get, auto set) {
        auto *slider = new QSlider(Qt::Horizontal, form->parentWidget());
        slider->setRange(min, max);
        slider->setValue(get());
        connect(slider, &QSlider::valueChanged, this, [this, set](int value) {
            set(value);
            emit optionsChanged();
        });
        m_controlSyncs.push_back([slider, get] {
            const QSignalBlocker blocker(slider);
            slider->setValue(get());
        });
        form->addRow(label, slider);
        return slider;
    };

    // Filters
    QFormLayout *filters = addSection(tr("Filters"), &GraphOptions::collapseFilter);
    m_filtersForm = filters;
    m_searchEdit = new QLineEdit(filters->parentWidget());
    m_searchEdit->setObjectName(QStringLiteral("graphSearchEdit"));
    m_searchEdit->setPlaceholderText(tr("Search files..."));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(300);
    connect(m_searchDebounce, &QTimer::timeout, this, &GraphView::rebuild);
    m_groupDebounce = new QTimer(this);
    m_groupDebounce->setSingleShot(true);
    m_groupDebounce->setInterval(300);
    connect(m_groupDebounce, &QTimer::timeout, this, [this] {
        assignColorGroups();
        markDirty();
        ensureTimerRunning();
    });
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_options.search = text;
        m_searchDebounce->start(); // a keystroke is not worth a rebuild
        emit optionsChanged();
    });
    m_controlSyncs.push_back([this] {
        const QSignalBlocker blocker(m_searchEdit);
        m_searchEdit->setText(m_options.search);
    });
    filters->addRow(m_searchEdit);
    addToggle(filters, tr("Tags"), [this] { return m_options.showTags; }, [this](bool on) {
        m_options.showTags = on;
        reapplyData();
    });
    addToggle(filters, tr("Attachments"), [this] { return m_options.showAttachments; }, [this](bool on) {
        m_options.showAttachments = on;
        reapplyData();
    });
    addToggle(filters, tr("Unresolved links"), [this] { return !m_options.hideUnresolved; }, [this](bool on) {
        m_options.hideUnresolved = !on;
        reapplyData();
    });
    addToggle(filters, tr("Orphans"), [this] { return m_options.showOrphans; }, [this](bool on) {
        m_options.showOrphans = on;
        reapplyData();
    });
    m_depthSlider = addSlider(filters, tr("Depth"), 1, 5, [this] { return m_options.localDepth; }, [this](int value) {
        m_options.localDepth = value;
        rebuild();
    });
    m_depthSlider->setObjectName(QStringLiteral("graphDepthSlider"));
    m_localIncomingBox = addToggle(filters, tr("Incoming links"), [this] { return m_options.localIncoming; }, [this](bool on) {
        m_options.localIncoming = on;
        rebuild();
    });
    m_localIncomingBox->setObjectName(QStringLiteral("graphLocalIncoming"));
    m_localOutgoingBox = addToggle(filters, tr("Outgoing links"), [this] { return m_options.localOutgoing; }, [this](bool on) {
        m_options.localOutgoing = on;
        rebuild();
    });
    m_localOutgoingBox->setObjectName(QStringLiteral("graphLocalOutgoing"));

    // Groups
    QFormLayout *groups = addSection(tr("Groups"), &GraphOptions::collapseColorGroups);
    m_groupRows = new QWidget(groups->parentWidget());
    m_groupsLayout = new QVBoxLayout(m_groupRows);
    m_groupsLayout->setContentsMargins(0, 0, 0, 0);
    m_groupsLayout->setSpacing(4);
    groups->addRow(m_groupRows);
    auto *newGroup = new QPushButton(tr("New group"), groups->parentWidget());
    newGroup->setObjectName(QStringLiteral("graphNewGroupButton"));
    newGroup->setCursor(Qt::PointingHandCursor);
    connect(newGroup, &QPushButton::clicked, this, [this] {
        static const QColor swatches[] = {QColor(0xe5, 0x48, 0x4d), QColor(0x46, 0xa7, 0x58), QColor(0x3e, 0x63, 0xdd),
                                          QColor(0xf5, 0xd9, 0x0a), QColor(0x8e, 0x4e, 0xc6), QColor(0x12, 0xa5, 0x94)};
        m_options.colorGroups.append(GraphColorGroup{QString(), swatches[m_options.colorGroups.size() % 6]});
        rebuildGroupRows();
        emit optionsChanged();
    });
    groups->addRow(newGroup);

    // Display
    QFormLayout *display = addSection(tr("Display"), &GraphOptions::collapseDisplay);
    addToggle(display, tr("Arrows"), [this] { return m_options.showArrow; }, [this](bool on) {
        m_options.showArrow = on;
        markDirty();
    });
    addSlider(display, tr("Text fade threshold"), -30, 30,
              [this] { return int(std::lround(m_options.textFadeMultiplier * 10)); }, [this](int value) {
                  m_options.textFadeMultiplier = value / 10.0;
                  markDirty();
              })->setObjectName(QStringLiteral("graphTextFadeSlider"));
    addSlider(display, tr("Node size"), 1, 50, [this] { return int(std::lround(m_options.nodeSizeMultiplier * 10)); },
              [this](int value) {
                  m_options.nodeSizeMultiplier = value / 10.0;
                  markDirty();
              });
    addSlider(display, tr("Link thickness"), 1, 50, [this] { return int(std::lround(m_options.lineSizeMultiplier * 10)); },
              [this](int value) {
                  m_options.lineSizeMultiplier = value / 10.0;
                  markDirty();
              });
    addToggle(display, tr("Animate"), [this] { return m_options.animate; }, [this](bool on) {
        m_options.animate = on;
        editLayout([](ForceLayout &layout) { layout.wake(); }); // settle (or not) from here
        ensureTimerRunning();
    });

    // Forces: these only need the layout to notice and to start moving
    // again, not a full rebuild - dragging a node does the same wake().
    const auto pushForceOptions = [this] {
        const ForceLayout::Options forceOptions = forceOptionsFor(m_options);
        editLayout([forceOptions](ForceLayout &layout) {
            layout.setOptions(forceOptions);
            layout.wake();
        });
        ensureTimerRunning();
    };
    QFormLayout *forces = addSection(tr("Forces"), &GraphOptions::collapseForces);
    addSlider(forces, tr("Center force"), 0, 200, [this] { return int(std::lround(m_options.centerStrength * 100)); },
              [this, pushForceOptions](int v) {
                  m_options.centerStrength = v / 100.0;
                  pushForceOptions();
              });
    addSlider(forces, tr("Repel force"), 0, 40, [this] { return int(m_options.repelStrength); },
              [this, pushForceOptions](int v) {
                  m_options.repelStrength = v;
                  pushForceOptions();
              })->setObjectName(QStringLiteral("graphRepelSlider"));
    addSlider(forces, tr("Link force"), 0, 50, [this] { return int(std::lround(m_options.linkStrength * 10)); },
              [this, pushForceOptions](int v) {
                  m_options.linkStrength = v / 10.0;
                  pushForceOptions();
              });
    addSlider(forces, tr("Link distance"), 20, 600, [this] { return int(m_options.linkDistance); },
              [this, pushForceOptions](int v) {
                  m_options.linkDistance = v;
                  pushForceOptions();
              });
    column->addStretch(1);

    rebuildGroupRows();
    syncControls();
    return scroll;
}

/// One row per colour group: its query, a swatch that opens a colour dialog,
/// and a remove button. Rebuilt whole - groups are a handful.
void GraphView::rebuildGroupRows()
{
    while (QLayoutItem *item = m_groupsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    for (int i = 0; i < m_options.colorGroups.size(); ++i) {
        auto *row = new QWidget(m_groupRows);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        auto *query = new QLineEdit(m_options.colorGroups.at(i).query, row);
        query->setPlaceholderText(tr("Enter query..."));
        auto *swatch = new QToolButton(row);
        swatch->setFixedSize(22, 22);
        swatch->setCursor(Qt::PointingHandCursor);
        const auto paintSwatch = [this, swatch, i] {
            swatch->setStyleSheet(QStringLiteral("QToolButton{background:%1;border:1px solid palette(mid);border-radius:11px;}")
                                      .arg(m_options.colorGroups.at(i).color.name()));
        };
        paintSwatch();
        auto *remove = new QToolButton(row);
        remove->setText(QStringLiteral("x"));
        remove->setAutoRaise(true);
        remove->setToolTip(tr("Delete group"));
        remove->setCursor(Qt::PointingHandCursor);
        layout->addWidget(query, 1);
        layout->addWidget(swatch);
        layout->addWidget(remove);
        m_groupsLayout->addWidget(row);

        connect(query, &QLineEdit::textChanged, this, [this, i](const QString &text) {
            if (i >= m_options.colorGroups.size())
                return;
            m_options.colorGroups[i].query = text;
            m_groupDebounce->start(); // a keystroke is not worth rescanning every node
            emit optionsChanged();
        });
        connect(swatch, &QToolButton::clicked, this, [this, i, paintSwatch] {
            const QColor picked = QColorDialog::getColor(m_options.colorGroups.at(i).color, this, tr("Group colour"));
            // The dialog is modal but the event loop runs: the list may have shrunk under it.
            if (!picked.isValid() || i >= m_options.colorGroups.size())
                return;
            m_options.colorGroups[i].color = picked;
            paintSwatch();
            markDirty();
            ensureTimerRunning();
            emit optionsChanged();
        });
        connect(remove, &QToolButton::clicked, this, [this, i] {
            m_options.colorGroups.removeAt(i);
            rebuildGroupRows();
            assignColorGroups();
            markDirty();
            ensureTimerRunning();
            emit optionsChanged();
        });
    }
}

void GraphView::showLocalRows()
{
    if (!m_filtersForm)
        return;
    m_filtersForm->setRowVisible(m_depthSlider, m_localMode);
    m_filtersForm->setRowVisible(m_localIncomingBox, m_localMode);
    m_filtersForm->setRowVisible(m_localOutgoingBox, m_localMode);
}

/// Puts every control back in step with m_options - after setOptions(), or
/// when the local/global mode (which shows the Depth slider) changes.
void GraphView::syncControls()
{
    if (!m_filtersForm)
        return;
    for (const auto &sync : m_controlSyncs)
        sync();
    showLocalRows();
    layoutControls();
}

void GraphView::layoutControls()
{
    if (!m_controlsToggle)
        return;
    m_controlsToggle->adjustSize();
    m_controlsToggle->move(width() - m_controlsToggle->width() - 8, 8);
    if (m_controlsPanel) {
        // A fixed-width card, as tall as its sections need but never past the
        // pane: the scroll area takes up the rest.
        const int panelWidth = std::min(270, std::max(120, width() - 16));
        const int wanted = static_cast<QScrollArea *>(m_controlsPanel)->widget()->sizeHint().height() + 4;
        const int available = std::max(60, height() - m_controlsToggle->y() - m_controlsToggle->height() - 16);
        m_controlsPanel->resize(panelWidth, std::min(wanted, available));
        m_controlsPanel->move(width() - m_controlsPanel->width() - 8,
                              m_controlsToggle->y() + m_controlsToggle->height() + 4);
    }
}

void GraphView::setTheme(const Theme &theme)
{
    m_palette.background = theme.color(QStringLiteral("--background-primary"));
    m_palette.node = theme.color(QStringLiteral("--text-muted"));
    m_palette.edge = theme.color(QStringLiteral("--background-modifier-border"));
    m_palette.text = theme.color(QStringLiteral("--text-normal"));
    m_palette.muted = theme.color(QStringLiteral("--text-faint"));
    m_palette.accent = theme.color(QStringLiteral("--interactive-accent"));
    m_controlsToggle->setIcon(Icons::icon(QStringLiteral("settings"), m_palette.text, 16));
    m_controlsPanel->setStyleSheet(
        QStringLiteral("#graphControlsPanel{background:%1;border:1px solid %2;border-radius:8px;}"
                       "#graphControlsBody{background:transparent;}")
            .arg(theme.color(QStringLiteral("--background-secondary")).name(),
                 theme.color(QStringLiteral("--background-modifier-border")).name()));
    markDirty();
}

void GraphView::setOptions(const GraphOptions &options)
{
    m_options = options;
    rebuildGroupRows();
    syncControls();
    reapplyData();
}

void GraphView::setIndex(Index *index)
{
    if (m_index)
        disconnect(m_index, nullptr, this, nullptr);
    m_index = index;
    if (m_index) {
        // A graph nobody is looking at (a background tab, a closed sidebar)
        // must not re-read the whole vault on every save; it catches up in
        // showEvent().
        connect(m_index, &Index::changed, this, [this] {
            if (isVisible())
                applyIndexData();
            else
                m_staleData = true;
        });
    }
    applyIndexData();
}

void GraphView::applyIndexData()
{
    if (!m_index) {
        setGraphData({}, {});
        return;
    }

    // Off the UI thread: the two queries and 40 000 link resolutions are
    // ~100 ms at 20 000 notes. The newest request wins; an older one's
    // result is dropped (setGraphData() also supersedes a request).
    const int request = ++m_dataRequest;
    m_pendingDataRequest = request;
    m_dataWatcher.setFuture(QtConcurrent::run(
        [snapshot = m_index->graphSnapshot(), options = m_options] {
            return buildGraph(Index::graphDataIn(snapshot), options);
        }));
}

void GraphView::onGraphBuilt()
{
    if (m_pendingDataRequest != m_dataRequest)
        return;
    const BuiltGraph built = m_dataWatcher.result();
    setGraphData(built.first, built.second);
}

void GraphView::setGraphData(const QList<GraphNode> &nodes, const QList<GraphEdge> &edges)
{
    ++m_dataRequest; // whatever applyIndexData() has in flight is older than this
    m_rawNodes = nodes;
    m_rawEdges = edges;
    rebuild();
}

void GraphView::setLocalGraph(bool local, const QString &path, int depth)
{
    m_localMode = local;
    showLocalRows();
    if (!path.isEmpty())
        m_currentNote = path;
    if (depth > 0) {
        m_options.localDepth = depth;
        syncControls();
    }
    rebuild();
}

void GraphView::setCurrentNote(const QString &relativePath)
{
    m_currentNote = relativePath;
    if (!m_localMode)
        markDirty();
    else if (isVisible())
        rebuild();
    else
        m_staleLayout = true; // every note switch would otherwise rebuild an unseen pane
}

void GraphView::rebuild()
{
    // The job owns the layout it holds: leave it that one, take a new one for
    // the new graph, and drop whatever the old job hands back. Waiting here
    // froze the UI for a whole job (up to 250 ms with Animate off).
    m_layout = std::make_shared<ForceLayout>();
    m_liveGeneration->store(++m_generation);
    m_layoutEdits.clear(); // aimed at nodes that may no longer exist

    QList<GraphNode> nodes = m_rawNodes;
    QList<GraphEdge> edges = m_rawEdges;

    if (m_localMode && !m_currentNote.isEmpty()) {
        // Which neighbours a hop may follow: through an outgoing link (id
        // links to it) and/or an incoming one (it links to id).
        QHash<QString, QStringList> adjacency;
        for (const GraphEdge &edge : std::as_const(edges)) {
            if (m_options.localOutgoing)
                adjacency[edge.fromID].append(edge.toID);
            if (m_options.localIncoming)
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

    // The Filters search box: files that do not match go, then the edges
    // into them, then any tag or unresolved node nothing is left linking to.
    const SearchQuery filter = SearchQuery::parse(m_options.search);
    if (!m_options.search.trimmed().isEmpty() && filter.isValid()) {
        QSet<QString> kept;
        QList<GraphNode> matching;
        for (const GraphNode &node : std::as_const(nodes)) {
            const bool isFile = node.kind == GraphNode::Kind::Note || node.kind == GraphNode::Kind::Attachment;
            if (isFile && !filter.evaluate(contextFor(node)).matched)
                continue;
            kept.insert(node.id);
            matching.append(node);
        }
        QList<GraphEdge> remaining;
        QSet<QString> linked;
        for (const GraphEdge &edge : std::as_const(edges)) {
            if (kept.contains(edge.fromID) && kept.contains(edge.toID)) {
                remaining.append(edge);
                linked.insert(edge.fromID);
                linked.insert(edge.toID);
            }
        }
        nodes.clear();
        for (const GraphNode &node : std::as_const(matching)) {
            const bool isFile = node.kind == GraphNode::Kind::Note || node.kind == GraphNode::Kind::Attachment;
            if (isFile || linked.contains(node.id))
                nodes.append(node);
        }
        edges = remaining;
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

    // Where every node stands now, so a rebuild that keeps a node keeps it in
    // place: every note save and every filter toggle lands here, and a graph
    // that re-scatters on each of them is unusable.
    QHash<QString, QPointF> keep;
    keep.reserve(m_nodes.size());
    for (int i = 0; i < m_nodes.size(); ++i)
        keep.insert(m_nodes[i].data.id, m_positions.value(i));

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
    assignColorGroups();

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

    m_layout->setOptions(forceOptionsFor(m_options));
    m_layout->reset(m_nodes.size(), layoutEdges);
    int kept = 0;
    for (int i = 0; i < m_nodes.size(); ++i) {
        const auto it = keep.constFind(m_nodes[i].data.id);
        if (it != keep.constEnd()) {
            m_layout->setPosition(i, it.value());
            ++kept;
        }
    }

    m_positions = m_layout->positions();
    m_settled = m_nodes.isEmpty();

    m_hoverNode = -1;
    m_dragNode = -1;
    // Survivors stay where they are, so the camera stays too; the step timer
    // keeps framing the graph while the newcomers settle, until the user moves it.
    if (kept == 0) {
        m_cameraMoved = false;
        fitToView();
        m_frame = QImage(); // a different graph: nothing to show until the first job lands
    }
    markDirty();
    ensureTimerRunning();
}

void GraphView::stepSimulation(int steps)
{
    m_frameWatcher.waitForFinished();
    m_liveGeneration->store(++m_generation); // the finished job's output is older than this
    flushLayoutEdits();
    for (int i = 0; i < steps; ++i)
        m_layout->step();
    m_positions = m_layout->positions();
    m_settled = m_layout->isSettled();
    markDirty();
}

QImage GraphView::renderFrameNow() const
{
    return paintGraph(makeFrameInput());
}

void GraphView::markDirty()
{
    m_frameDirty = true;
    update();
}

void GraphView::editLayout(std::function<void(ForceLayout &)> edit)
{
    m_settled = false; // every edit wakes the layout
    m_woken = true;
    if (m_jobInFlight) {
        m_layoutEdits.push_back(std::move(edit));
        return;
    }
    flushLayoutEdits();
    edit(*m_layout);
}

void GraphView::flushLayoutEdits()
{
    for (const auto &edit : m_layoutEdits)
        edit(*m_layout);
    m_layoutEdits.clear();
}

GraphView::FrameInput GraphView::makeFrameInput() const
{
    FrameInput input;
    input.size = size();
    input.dpr = devicePixelRatioF();
    input.positions = m_positions;
    input.nodes = m_nodes;
    input.edges = m_edgeIndices;
    input.zoom = m_zoom;
    input.pan = m_pan;
    input.hoverNode = m_hoverNode;
    input.currentNote = m_currentNote;
    input.options = m_options;
    input.palette = m_palette;
    input.font = m_labelFont;
    input.generation = m_generation;
    return input;
}

void GraphView::launchFrame()
{
    flushLayoutEdits();
    m_woken = false;
    FrameInput input = makeFrameInput();
    // A few steps per frame on a small graph so it settles in a couple of
    // seconds rather than needing patience; one step is already the right
    // amount of work once the graph is big, since each is O(n log n) - see
    // GraphLayout.h.
    input.steps = m_settled ? 0 : !m_options.animate ? 300 : (m_nodes.size() > 1000 ? 1 : 3);
    m_frameDirty = false;
    m_jobInFlight = true;
    m_frameWatcher.setFuture(QtConcurrent::run([input = std::move(input), layout = m_layout,
                                                live = m_liveGeneration]() mutable {
        FrameOutput output;
        output.generation = input.generation;
        // Animate off: settle before the picture moves, up to 300 steps - but
        // never more than a quarter second a job, which a rebuild may have
        // to wait out; the next job carries on.
        QElapsedTimer budget;
        budget.start();
        for (int i = 0; i < input.steps; ++i) {
            layout->step();
            if (live->load() != input.generation)
                return output; // superseded by a rebuild: nothing to paint, nothing to keep
            if (!input.options.animate && (layout->isSettled() || budget.elapsed() > 250))
                break;
        }
        input.positions = layout->positions();
        output.positions = input.positions;
        output.settled = layout->isSettled();
        output.generation = input.generation;
        output.zoom = input.zoom;
        output.pan = input.pan;
        output.frame = paintGraph(input);
        return output;
    }));
}

void GraphView::onFrameFinished()
{
    m_jobInFlight = false;
    const FrameOutput output = m_frameWatcher.result();
    if (output.generation != m_generation)
        return; // from before a rebuild: its positions belong to nodes that are gone
    m_positions = output.positions;
    if (m_dragNode >= 0 && m_dragNode < m_positions.size())
        m_positions[m_dragNode] = m_dragPos; // the job had not seen the latest move yet
    m_frame = output.frame;
    m_frameZoom = output.zoom;
    m_framePan = output.pan;
    // An edit made while the job ran woke the layout; the job's own verdict
    // says it was settled.
    m_settled = output.settled && !m_woken;
    if (!m_cameraMoved)
        fitToView();
    update();
}

void GraphView::ensureTimerRunning()
{
    if (m_nodes.isEmpty() || !isVisible())
        return;

    if (!m_stepTimer) {
        m_stepTimer = new QTimer(this);
        // Only a pacer: a job is launched when none runs and there is
        // something to show or to settle, and the timer stops once there is
        // not - a settled, still picture costs nothing.
        connect(m_stepTimer, &QTimer::timeout, this, [this] {
            if (m_jobInFlight)
                return;
            if (m_settled && !m_frameDirty && m_dragNode < 0) {
                m_stepTimer->stop();
                return;
            }
            launchFrame();
        });
    }
    if (!m_stepTimer->isActive())
        m_stepTimer->start(16);
}

void GraphView::fitToView()
{
    // Follows the layout as it spreads until the user zooms or pans, the way
    // Obsidian frames a graph when it opens: at a fixed zoom a local graph's
    // few nodes sit a link distance (250) apart and miss a 260 px pane.
    if (m_nodes.isEmpty() || width() <= 0 || height() <= 0)
        return;
    // Min and max by hand: QRectF's |= skips an empty rect, and a point is one.
    double left = 0, top = 0, right = 0, bottom = 0;
    bool any = false;
    for (int i = 0; i < m_nodes.size(); ++i) {
        const QPointF p = m_positions.value(i);
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()))
            continue;
        left = any ? std::min(left, p.x()) : p.x();
        right = any ? std::max(right, p.x()) : p.x();
        top = any ? std::min(top, p.y()) : p.y();
        bottom = any ? std::max(bottom, p.y()) : p.y();
        any = true;
    }
    const QRectF bounds(QPointF(left, top), QPointF(right, bottom));
    const double margin = 40.0;
    const double zoomX = (width() - 2 * margin) / std::max(1.0, bounds.width());
    const double zoomY = (height() - 2 * margin) / std::max(1.0, bounds.height());
    const double zoom = std::clamp(std::min(zoomX, zoomY), 0.05, 1.0);
    const QPointF pan = -bounds.center() * zoom;
    if (zoom != m_zoom || pan != m_pan) {
        m_zoom = zoom;
        m_pan = pan;
        markDirty();
    }
}

void GraphView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // The note changed while hidden: show its neighbourhood now, from the data
    // held, not the old one's until the read below lands.
    if (m_staleLayout)
        rebuild();
    if (m_staleData)
        applyIndexData();
    m_staleData = m_staleLayout = false;
    if (!m_settled || m_frameDirty)
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
    m_frameDirty = true;
    ensureTimerRunning();
}

QPointF GraphView::toScreen(QPointF graphPos) const
{
    return project(graphPos, m_zoom, m_pan, size());
}

QPointF GraphView::toGraph(QPointF screenPos) const
{
    return (screenPos - m_pan - QPointF(width() / 2.0, height() / 2.0)) / m_zoom;
}

double GraphView::radiusFor(int degree, const GraphOptions &options)
{
    // Sub-linear in degree so a hundred-link hub reads as bigger, not ten
    // times the area of a one-link note. Obsidian's nodes are small dots:
    // at 3 + 1.6 sqrt(degree) a ten-link note was 8 px and a 300-note vault
    // read as a blob.
    return (2.0 + std::sqrt(double(std::max(0, degree)))) * options.nodeSizeMultiplier;
}

void GraphView::assignColorGroups()
{
    // Obsidian's colour groups are search queries, so they get the search
    // grammar - OR, -negation, (groups), path:, file:, tag:, /regex/. A node
    // has no text here, so a bare word is looked for in its name, which is
    // what one means in a colour group. Parsed and matched once per
    // rebuild; painting only reads the result.
    QList<SearchQuery> queries;
    for (const GraphColorGroup &group : std::as_const(m_options.colorGroups))
        queries << SearchQuery::parse(group.query);
    for (VisualNode &node : m_nodes) {
        node.group = -1;
        if (queries.isEmpty())
            continue;
        const NoteContext context = contextFor(node.data);
        for (int i = 0; i < queries.size(); ++i) {
            if (queries.at(i).isValid() && !m_options.colorGroups.at(i).query.trimmed().isEmpty()
                && queries.at(i).evaluate(context).matched) {
                node.group = i;
                break;
            }
        }
    }
}

QColor GraphView::colorFor(const VisualNode &node, const GraphOptions &options, const Palette &palette)
{
    if (node.group >= 0 && node.group < options.colorGroups.size())
        return options.colorGroups.at(node.group).color;
    switch (node.data.kind) {
    case GraphNode::Kind::Tag:
        return palette.accent;
    case GraphNode::Kind::Attachment:
    case GraphNode::Kind::Unresolved:
        return palette.muted;
    case GraphNode::Kind::Note:
    default:
        return palette.node;
    }
}

int GraphView::nodeAt(QPointF screenPos) const
{
    for (int i = m_nodes.size() - 1; i >= 0; --i) {
        const QPointF center = toScreen(m_positions.value(i));
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
        markDirty();
        ensureTimerRunning();
    }
}

void GraphView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), m_palette.background);
    if (m_frame.isNull())
        return;
    // The frame was painted with the camera it was launched under; panning and
    // zooming since then just move that picture, and a real frame follows.
    const QPointF center(width() / 2.0, height() / 2.0);
    const double k = m_zoom / m_frameZoom;
    if (k != 1.0 || m_pan != m_framePan) {
        painter.translate(center + m_pan);
        painter.scale(k, k);
        painter.translate(-(center + m_framePan));
    }
    painter.drawImage(QPointF(0, 0), m_frame);
}

QImage GraphView::paintGraph(const FrameInput &in)
{
    QImage image(in.size * in.dpr, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(in.dpr);
    image.fill(in.palette.background);
    if (in.nodes.isEmpty())
        return image;
    QPainter painter(&image);
    const auto toScreen = [&](QPointF graphPos) { return project(graphPos, in.zoom, in.pan, in.size); };
    const auto radiusFor = [&](int degree) { return GraphView::radiusFor(degree, in.options); };
    const QColor background = in.palette.background;
    const QColor edgeColor = in.palette.edge;
    const QList<VisualNode> &nodes = in.nodes;
    const QList<std::pair<int, int>> &edges = in.edges;
    const GraphOptions &options = in.options;

    // Antialiasing is real cost per primitive; a graph big enough to need
    // the performance guard in tests/test_graph.cpp does not get it.
    painter.setRenderHint(QPainter::Antialiasing, nodes.size() < 1500);

    const QRectF viewport(QPointF(0, 0), QSizeF(in.size));

    QSet<int> highlighted;
    if (in.hoverNode >= 0) {
        highlighted.insert(in.hoverNode);
        for (const auto &edge : edges) {
            if (edge.first == in.hoverNode)
                highlighted.insert(edge.second);
            else if (edge.second == in.hoverNode)
                highlighted.insert(edge.first);
        }
    }
    const bool dimming = in.hoverNode >= 0;

    // Batched rather than one setPen()+drawLine() per edge: at the
    // performance guard's scale (~10 000 edges) that was three quarters of
    // the frame budget by itself (drawLines() lets the raster engine walk
    // one pen change instead of ten thousand). Edges only ever fall into two
    // buckets - dimmed or not - so two passes, each collecting its lines
    // first and drawing them in one call, replace what used to be a
    // per-edge state change.
    QVector<QLineF> normalLines, dimLines;
    normalLines.reserve(edges.size());
    for (const auto &edge : edges) {
        const QPointF a = toScreen(in.positions.value(edge.first));
        const QPointF b = toScreen(in.positions.value(edge.second));
        if (!viewport.intersects(QRectF(a, b).normalized().adjusted(-4, -4, 4, 4)))
            continue; // culled - neither endpoint is anywhere near the viewport

        const bool dim = dimming && !(highlighted.contains(edge.first) && highlighted.contains(edge.second));
        (dim ? dimLines : normalLines).append(QLineF(a, b));

        if (options.showArrow) {
            const QPointF direction = b - a;
            const double length = std::hypot(direction.x(), direction.y());
            if (length > 1.0) {
                const QPointF unit = direction / length;
                const double targetRadius = radiusFor(nodes[edge.second].degree) * in.zoom;
                const QPointF tip = b - unit * (targetRadius + 2.0);
                const QPointF normal(-unit.y(), unit.x());
                const double headSize = 4.0 + options.lineSizeMultiplier;
                const QPointF left = tip - unit * headSize + normal * (headSize * 0.6);
                const QPointF right = tip - unit * headSize - normal * (headSize * 0.6);
                painter.setBrush(blendOverBackground(edgeColor, dim ? 40 : 160, background));
                painter.setPen(Qt::NoPen);
                const QPointF triangle[3] = {tip, left, right};
                painter.drawPolygon(triangle, 3);
            }
        }
    }
    QPen edgePen;
    edgePen.setWidthF(std::max(0.4, options.lineSizeMultiplier * in.zoom));
    if (!normalLines.isEmpty()) {
        edgePen.setColor(blendOverBackground(edgeColor, 160, background));
        painter.setPen(edgePen);
        painter.drawLines(normalLines);
    }
    if (!dimLines.isEmpty()) {
        edgePen.setColor(blendOverBackground(edgeColor, 40, background));
        painter.setPen(edgePen);
        painter.drawLines(dimLines);
    }

    // Text fade threshold: labels fade in as the view zooms in (Obsidian's
    // slider, -3..3, moves where that happens). The threshold t is 0.8 at 0
    // and scales by 1.3 per step, so -3..3 spans 0.36..1.76; the labels are
    // invisible below 0.75 t and fully drawn from 1.25 t. A small vault fits
    // at zoom 1 and so opens labelled, as in Obsidian; a big one fits far
    // out and shows its labels as it is zoomed into. A hovered node, its
    // neighbours and the current note always get theirs.
    const double fadeThreshold = 0.8 * std::pow(1.3, -options.textFadeMultiplier);
    const double fadeAlpha = std::clamp((in.zoom - 0.75 * fadeThreshold) / (0.5 * fadeThreshold), 0.0, 1.0);
    painter.setFont(in.font);


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
    const bool simplifiedDots = nodes.size() >= 1500;
    for (int i = 0; i < nodes.size(); ++i) {
        const VisualNode &node = nodes[i];
        const QPointF center = toScreen(in.positions.value(i));
        const double radius = radiusFor(node.degree) * in.zoom;
        const QRectF bounds(center.x() - radius, center.y() - radius, radius * 2, radius * 2);
        if (!viewport.intersects(bounds.adjusted(-50, -20, 50, 24)))
            continue; // culled - padded for the label that might sit beside it

        const bool dim = dimming && !highlighted.contains(i);
        QColor fill = colorFor(node, options, in.palette);
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

        if (node.data.id == in.currentNote) {
            painter.setPen(QPen(in.palette.accent, 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, radius + 3, radius + 3);
            painter.setPen(Qt::NoPen); // state left dirty for the next node's ellipse
            haveBrush = false;         // and the brush, since NoBrush was just set too
        }

        const bool pinned = i == in.hoverNode || highlighted.contains(i) || node.data.id == in.currentNote;
        const double labelAlpha = dim ? 0.0 : pinned ? 1.0 : fadeAlpha;
        if (labelAlpha > 0.0 && !node.data.label.isEmpty()) {
            QColor labelColor = in.palette.text;
            labelColor.setAlphaF(labelAlpha);
            painter.setPen(labelColor);
            painter.drawText(QRectF(center.x() - 70, center.y() + radius + 2, 140, 16),
                             Qt::AlignHCenter | Qt::AlignTop, node.data.label);
            painter.setPen(Qt::NoPen);
        }
    }
    painter.end();
    return image;
}

void GraphView::wheelEvent(QWheelEvent *event)
{
    const QPointF cursor = event->position();
    const QPointF graphPointUnderCursor = toGraph(cursor);

    const double factor = std::pow(1.0015, event->angleDelta().y());
    m_zoom = std::clamp(m_zoom * factor, 0.05, 8.0);
    m_cameraMoved = true;

    // Re-derive pan so the point under the cursor stays under it - zooming
    // "about the pointer" rather than about the widget's centre.
    m_pan += cursor - toScreen(graphPointUnderCursor);

    event->accept();
    markDirty();
    ensureTimerRunning();
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
        m_cameraMoved = true; // a refit would slide the node out from under the pointer
        m_dragPos = m_positions.value(hit);
        editLayout([hit](ForceLayout &layout) {
            layout.setPinned(hit, true);
            layout.wake();
        });
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
        m_dragPos = toGraph(event->position());
        const int node = m_dragNode;
        const QPointF to = m_dragPos;
        if (node < m_positions.size())
            m_positions[node] = to; // hit tests see the move at once; the layout follows
        editLayout([node, to](ForceLayout &layout) {
            layout.setPosition(node, to);
            layout.wake();
        });
        markDirty();
        ensureTimerRunning();
        return;
    }
    if (m_panning) {
        m_pan = m_panOrigin + (event->position() - m_panStart);
        m_cameraMoved = true;
        markDirty();
        ensureTimerRunning();
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
        const int node = m_dragNode;
        editLayout([node](ForceLayout &layout) {
            layout.setPinned(node, false);
            layout.wake();
        });
        ensureTimerRunning();
        m_dragNode = -1;
    }
    m_panning = false;
}

void GraphView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && !m_localMode) {
        emit closeRequested();
        return;
    }
    QWidget::keyPressEvent(event);
}

void GraphView::contextMenuEvent(QContextMenuEvent *event)
{
    const int hit = nodeAt(event->pos());
    if (hit < 0) {
        event->ignore();
        return;
    }
    const GraphNode &data = m_nodes[hit].data;
    if (data.kind != GraphNode::Kind::Note && data.kind != GraphNode::Kind::Attachment) {
        event->ignore(); // a tag or an unresolved link has nothing to open
        return;
    }
    const QString id = data.id;
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("graphNodeMenu"));
    QAction *open = menu.addAction(tr("Open"));
    QAction *openInTab = menu.addAction(tr("Open in new tab"));
    event->accept();
    const QAction *chosen = menu.exec(event->globalPos());
    if (chosen == open)
        emit noteActivated(id);
    else if (chosen == openInTab)
        emit noteActivatedInNewTab(id);
}

void GraphView::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_hoverNode >= 0) {
        m_hoverNode = -1;
        markDirty();
        ensureTimerRunning();
    }
}

} // namespace noteAppC
