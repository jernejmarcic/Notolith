#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "GraphLayout.h"
#include "GraphView.h"
#include "Index.h"
#include "Indexer.h"
#include "Vault.h"

using namespace noteAppC;

namespace {

double median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    if (n == 0)
        return 0.0;
    return n % 2 == 0 ? (values[n / 2 - 1] + values[n / 2]) / 2.0 : values[n / 2];
}

/// A ring plus a couple of long chords per node: enough edges that both the
/// Barnes-Hut repulsion and the springs do real work, without the O(n^2)
/// setup cost of connecting every node to every other one.
QList<std::pair<int, int>> ringGraph(int n, int chordsPerNode)
{
    QList<std::pair<int, int>> edges;
    for (int i = 0; i < n; ++i)
        edges.append({i, (i + 1) % n});
    for (int i = 0; i < n; ++i) {
        for (int c = 1; c <= chordsPerNode; ++c)
            edges.append({i, (i + c * 37 + 1) % n});
    }
    return edges;
}

} // namespace

// ---------------------------------------------------------------------------
// ForceLayout: convergence

TEST(ForceLayoutTest, ConvergesAndThenReportsSettled)
{
    ForceLayout layout;
    const int n = 200;
    layout.reset(n, ringGraph(n, 2));

    // The first handful of steps are the initial burst as a random scatter
    // finds its feet; energy is expected to be noisy there, not falling.
    double peak = 0.0;
    for (int i = 0; i < 5; ++i)
        peak = std::max(peak, layout.step());

    double last = peak;
    int stepsTaken = 0;
    for (; stepsTaken < 3000 && !layout.isSettled(); ++stepsTaken)
        last = layout.step();

    EXPECT_TRUE(layout.isSettled()) << "did not settle within 3000 steps";
    EXPECT_LT(last, peak) << "energy did not fall from its early peak";
    std::printf("[ PERF     ] %d-node ring settled after %d steps (energy %.4f -> %.4f)\n", n,
                stepsTaken, peak, last);
}

TEST(ForceLayoutTest, DraggingWakesASettledLayout)
{
    ForceLayout layout;
    const int n = 50;
    layout.reset(n, ringGraph(n, 1));
    for (int i = 0; i < 3000 && !layout.isSettled(); ++i)
        layout.step();
    ASSERT_TRUE(layout.isSettled()) << "test setup did not settle";

    layout.setPosition(0, QPointF(5000, 5000));
    layout.wake();
    EXPECT_FALSE(layout.isSettled());
}

TEST(ForceLayoutTest, PinnedNodeDoesNotMove)
{
    ForceLayout layout;
    const int n = 30;
    layout.reset(n, ringGraph(n, 1));
    layout.setPosition(0, QPointF(0, 0));
    layout.setPinned(0, true);
    for (int i = 0; i < 50; ++i)
        layout.step();
    EXPECT_EQ(layout.position(0), QPointF(0, 0));
}

// Regression test for the 2026-09-23 crash: test_graph grew to 55.8 GB and
// was OOM-killed. Root cause, confirmed with a standalone reproduction
// outside this suite: the simulation is numerically unstable at 5 000
// nodes - the old kSoftening (1e-6) let two close bodies produce an
// enormous repulsion, plain Euler integration overshot it further every
// step, and the resulting energy went from 7e7 at step 0 to literal +inf
// by step ~200. Once a position was infinite, halving it in the quadtree
// never made it smaller (IEEE 754: inf / 2 == inf), so the halfSize-based
// recursion guard - `< kMinCell`, also broken for NaN since any comparison
// against NaN is false - never fired, and insert() kept calling
// newNode() (a tail call the compiler turned into a loop, not a stack
// overflow) until memory ran out. The fix has three independent layers:
// a raised softening floor and a per-step speed cap in step() so a
// position cannot reach a non-finite value in the first place, a hard
// kMaxTreeDepth cap and a NaN-safe comparison in insert() so the quadtree
// cannot recurse without bound even if one does, and a finite-position
// filter in buildTree() so a bad coordinate cannot poison the bounding
// box. This test exercises the second and third layers directly: every
// node starts at exactly the same point (the coordinator's prime
// suspect - a fresh layout often does, since two continuous random draws
// landing on the same point is what reset() itself normally avoids, but a
// caller placing many nodes by hand, or a future change to reset(), could
// reintroduce it), which used to be handled correctly already (finite
// coincident points were never the bug - see the postmortem above), but is
// worth pinning down permanently now that insert() takes a depth argument.
TEST(ForceLayoutTest, ManyCoincidentNodesStepQuickly)
{
    ForceLayout layout;
    const int n = 4000;
    layout.reset(n, ringGraph(n, 2));
    for (int i = 0; i < n; ++i)
        layout.setPosition(i, QPointF(0, 0));

    QElapsedTimer clock;
    clock.start();
    for (int i = 0; i < 10; ++i)
        layout.step();
    const double elapsedMs = clock.nsecsElapsed() / 1e6;

    std::printf("[ PERF     ] %d exactly-coincident nodes, 10 steps: %.1f ms\n", n, elapsedMs);
    // Generous on purpose - this asserts "did not run away", not a tight
    // performance budget (LargeGraphStepAndFrameStayInsideBudget covers
    // that for a normally-scattered graph). A regression back to unbounded
    // recursion would not finish in seconds, let alone this.
    EXPECT_LT(elapsedMs, 5000.0);
    for (int i = 0; i < n; ++i) {
        const QPointF p = layout.position(i);
        EXPECT_TRUE(std::isfinite(p.x()) && std::isfinite(p.y()));
    }
}

// The actual root cause, not just its quadtree symptom: with the dense
// (~10 000-edge) graph the performance guard below uses and the default
// force options, positions must stay finite and the layout must actually
// settle - not just avoid crashing - within a bounded number of steps.
// Before the kSoftening/speed-cap/alpha-decay fix this test's step loop
// diverged to +inf energy well before 400 iterations; the cooling
// schedule (m_alpha) is what makes settling happen at all, not only
// staying finite along the way.
TEST(ForceLayoutTest, DenseGraphStaysFiniteAndSettles)
{
    ForceLayout layout;
    const int n = 5000;
    layout.reset(n, ringGraph(n, 2));

    bool settled = false;
    for (int i = 0; i < 400 && !settled; ++i) {
        const double energy = layout.step();
        ASSERT_TRUE(std::isfinite(energy)) << "energy diverged at step " << i;
        settled = layout.isSettled();
    }
    EXPECT_TRUE(settled) << "did not settle within 400 steps";

    for (int i = 0; i < n; ++i) {
        const QPointF p = layout.position(i);
        ASSERT_TRUE(std::isfinite(p.x()) && std::isfinite(p.y())) << "node " << i << " diverged";
    }
}

// ---------------------------------------------------------------------------
// GraphView: local graph depth

namespace {

QList<GraphNode> nodesFor(const QStringList &ids)
{
    QList<GraphNode> nodes;
    for (const QString &id : ids) {
        GraphNode node;
        node.id = id;
        node.label = id;
        nodes.append(node);
    }
    return nodes;
}

} // namespace

TEST(GraphViewTest, LocalGraphRespectsDepth)
{
    // A chain A-B-C-D-E: from A, depth 1 reaches only B, depth 2 reaches B
    // and C, and switching back to global mode restores everything.
    const QStringList ids{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"),
                          QStringLiteral("D"), QStringLiteral("E")};
    const QList<GraphEdge> edges{{QStringLiteral("A"), QStringLiteral("B")},
                                 {QStringLiteral("B"), QStringLiteral("C")},
                                 {QStringLiteral("C"), QStringLiteral("D")},
                                 {QStringLiteral("D"), QStringLiteral("E")}};

    GraphView view;
    view.setGraphData(nodesFor(ids), edges);
    ASSERT_EQ(view.nodeCount(), 5);
    ASSERT_EQ(view.edgeCount(), 4);

    view.setLocalGraph(true, QStringLiteral("A"), 1);
    EXPECT_TRUE(view.isLocalGraph());
    EXPECT_EQ(view.nodeCount(), 2);
    EXPECT_TRUE(view.containsNode(QStringLiteral("A")));
    EXPECT_TRUE(view.containsNode(QStringLiteral("B")));
    EXPECT_FALSE(view.containsNode(QStringLiteral("C")));

    view.setLocalGraph(true, QStringLiteral("A"), 2);
    EXPECT_EQ(view.nodeCount(), 3);
    EXPECT_TRUE(view.containsNode(QStringLiteral("C")));
    EXPECT_FALSE(view.containsNode(QStringLiteral("D")));

    view.setLocalGraph(true, QStringLiteral("A"), 10);
    EXPECT_EQ(view.nodeCount(), 5) << "a depth past the graph's diameter reaches everyone";

    view.setLocalGraph(false);
    EXPECT_FALSE(view.isLocalGraph());
    EXPECT_EQ(view.nodeCount(), 5);
}

TEST(GraphViewTest, AStarIsFramedInANarrowPaneAndItsLeavesSpread)
{
    // A local graph's shape: one note and its eight links, in a sidebar-wide
    // view. At a fixed zoom the leaves sat a link distance (250) out, off a
    // 260 px pane; with 1/d^2 repulsion they never spread and stacked up.
    QStringList ids{QStringLiteral("hub")};
    QList<GraphEdge> edges;
    for (int i = 0; i < 8; ++i) {
        ids << QStringLiteral("leaf%1").arg(i);
        edges.append({QStringLiteral("hub"), ids.last()});
    }
    GraphView view;
    view.resize(260, 500);
    view.setGraphData(nodesFor(ids), edges);
    view.show();
    QTest::qWait(2500); // the step timer runs until the layout settles

    const QRectF pane = QRectF(view.rect()).adjusted(4, 4, -4, -4);
    for (const QString &id : std::as_const(ids))
        EXPECT_TRUE(pane.contains(view.screenPosition(id))) << id.toStdString() << " is off the pane";
    double closest = 1e9;
    for (int a = 1; a < ids.size(); ++a) {
        for (int b = a + 1; b < ids.size(); ++b) {
            const QPointF d = view.screenPosition(ids[a]) - view.screenPosition(ids[b]);
            closest = std::min(closest, std::hypot(d.x(), d.y()));
        }
    }
    EXPECT_GT(closest, 12.0) << "two leaves sit on top of each other";
}

TEST(GraphViewTest, ColourGroupsAreSearchQueries)
{
    QList<GraphNode> nodes = nodesFor({QStringLiteral("Projects/A.md"), QStringLiteral("Journal/B.md"),
                                       QStringLiteral("Home.md")});
    nodes[0].label = QStringLiteral("A");
    nodes[0].tags = {QStringLiteral("work")};
    nodes[1].label = QStringLiteral("B");
    nodes[1].tags = {QStringLiteral("work/meeting")};
    nodes[2].label = QStringLiteral("Home");

    GraphView view;
    GraphOptions options = view.options();
    options.colorGroups = {{QStringLiteral("tag:#work -path:Journal"), Qt::red},
                           {QStringLiteral("Home OR B"), Qt::blue}};
    view.setOptions(options);
    view.setGraphData(nodes, {});
    EXPECT_EQ(view.nodeColor(QStringLiteral("Projects/A.md")), QColor(Qt::red));
    // work/meeting is under #work, but the negation keeps Journal out, so the
    // next group's bare word - matched against the name - takes it.
    EXPECT_EQ(view.nodeColor(QStringLiteral("Journal/B.md")), QColor(Qt::blue));
    EXPECT_EQ(view.nodeColor(QStringLiteral("Home.md")), QColor(Qt::blue));
}

TEST(GraphViewTest, OrphanFilterDropsUnconnectedNotes)
{
    GraphOptions options = GraphOptions::defaults();
    options.showOrphans = false;

    GraphView view;
    view.setOptions(options);
    QList<GraphNode> nodes = nodesFor({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("Lonely")});
    view.setGraphData(nodes, {{QStringLiteral("A"), QStringLiteral("B")}});

    EXPECT_EQ(view.nodeCount(), 2);
    EXPECT_FALSE(view.containsNode(QStringLiteral("Lonely")));
}

// ---------------------------------------------------------------------------
// Index::graphData() over a real (temporary) vault

namespace {

class GraphDataTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        write(QStringLiteral("Home.md"),
              "# Home\n\nSee [[Ideas]] and [[Missing Note]].\n\n#project\n");
        write(QStringLiteral("Ideas.md"),
              "---\ntags: [thinking]\n---\n\nBack to [[Home]]. An embed: ![[diagram.png]]\n");
        write(QStringLiteral("Orphan.md"), "Nobody links here, and this links nowhere.\n");
        // The smallest valid PNG (a 1x1 transparent pixel) - enough to be
        // indexed as an attachment, never opened as an image.
        write(QStringLiteral("diagram.png"),
              QByteArray::fromBase64(
                  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="));
        vault = Vault(dir.path());
    }

    void write(const QString &relative, const QByteArray &bytes)
    {
        QFile file(dir.filePath(relative));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }

    void build(Index &index)
    {
        Indexer indexer(&index);
        QSignalSpy done(&indexer, &Indexer::finished);
        indexer.setVault(vault);
        ASSERT_TRUE(done.wait(20000));
    }

    QTemporaryDir dir;
    Vault vault;
};

} // namespace

TEST_F(GraphDataTest, ReturnsEveryFileAndEveryInternalLink)
{
    Index index;
    build(index);

    const Index::GraphData data = index.graphData();

    QStringList paths;
    for (const auto &file : data.files)
        paths << file.path;
    paths.sort();
    EXPECT_EQ(paths, QStringList({QStringLiteral("Home.md"), QStringLiteral("Ideas.md"),
                                  QStringLiteral("Orphan.md"), QStringLiteral("diagram.png")}));

    for (const auto &file : data.files) {
        if (file.path == QStringLiteral("diagram.png")) {
            EXPECT_TRUE(file.isAttachment);
        } else {
            EXPECT_FALSE(file.isAttachment);
        }
        if (file.path == QStringLiteral("Ideas.md")) {
            EXPECT_EQ(file.tags, QStringList({QStringLiteral("thinking")}));
        }
    }

    bool resolvedHomeToIdeas = false, resolvedIdeasToHome = false, resolvedEmbed = false;
    bool sawUnresolved = false;
    for (const auto &link : data.links) {
        if (link.fromPath == QStringLiteral("Home.md") && link.toPath == QStringLiteral("Ideas.md"))
            resolvedHomeToIdeas = true;
        if (link.fromPath == QStringLiteral("Ideas.md") && link.toPath == QStringLiteral("Home.md"))
            resolvedIdeasToHome = true;
        if (link.fromPath == QStringLiteral("Ideas.md") && link.toPath == QStringLiteral("diagram.png"))
            resolvedEmbed = true;
        if (link.fromPath == QStringLiteral("Home.md") && link.toPath.isEmpty()) {
            sawUnresolved = true;
            EXPECT_EQ(link.targetName, QStringLiteral("Missing Note"));
        }
    }
    EXPECT_TRUE(resolvedHomeToIdeas);
    EXPECT_TRUE(resolvedIdeasToHome);
    EXPECT_TRUE(resolvedEmbed);
    EXPECT_TRUE(sawUnresolved);
}

TEST_F(GraphDataTest, FeedsGraphViewEndToEnd)
{
    // Not just that Index and GraphView each work alone: that the shape one
    // hands the other actually builds a graph, with the default options
    // (attachments off, orphans on) applied on the way.
    Index index;
    build(index);

    GraphView view;
    view.setIndex(&index);
    // Home.md, Ideas.md, Orphan.md (kept: showOrphans defaults to true) and
    // the unresolved "Missing Note" node; diagram.png is excluded because
    // showAttachments defaults to false.
    EXPECT_EQ(view.nodeCount(), 4);
    EXPECT_TRUE(view.containsNode(QStringLiteral("unresolved:Missing Note")));
    EXPECT_TRUE(view.containsNode(QStringLiteral("Orphan.md")));
    EXPECT_FALSE(view.containsNode(QStringLiteral("diagram.png")));

    GraphOptions options = view.options();
    options.hideUnresolved = true;
    options.showOrphans = false;
    view.setOptions(options);
    // Orphan.md has no links at all, so it drops; the unresolved node goes too.
    EXPECT_FALSE(view.containsNode(QStringLiteral("unresolved:Missing Note")));
    EXPECT_FALSE(view.containsNode(QStringLiteral("Orphan.md")));
    EXPECT_TRUE(view.containsNode(QStringLiteral("Home.md")));
    EXPECT_TRUE(view.containsNode(QStringLiteral("Ideas.md")));
}

TEST_F(GraphDataTest, AHiddenGraphCatchesUpOnIndexChangesWhenShown)
{
    Index index;
    build(index);
    GraphView view;
    view.setIndex(&index);
    ASSERT_EQ(view.nodeCount(), 4);

    // Hidden: a change must not re-read the vault, only mark it stale.
    emit index.changed();
    view.setGraphData(nodesFor({QStringLiteral("X")}), {});
    emit index.changed();
    EXPECT_EQ(view.nodeCount(), 1);

    view.show();
    EXPECT_EQ(view.nodeCount(), 4);
}

// ---------------------------------------------------------------------------
// Performance guard: the plan's budget is 5 000 nodes / ~10 000 edges at 60 fps.

namespace {

QList<GraphNode> makeNodes(int n)
{
    QList<GraphNode> nodes;
    nodes.reserve(n);
    for (int i = 0; i < n; ++i) {
        GraphNode node;
        node.id = QStringLiteral("note-%1").arg(i);
        node.label = node.id;
        nodes.append(node);
    }
    return nodes;
}

QList<GraphEdge> makeEdges(const QList<GraphNode> &nodes, int edgesPerNode)
{
    QList<GraphEdge> edges;
    const int n = nodes.size();
    edges.reserve(n * edgesPerNode);
    for (int i = 0; i < n; ++i) {
        for (int c = 1; c <= edgesPerNode; ++c)
            edges.append({nodes[i].id, nodes[(i + c * 97 + 1) % n].id});
    }
    return edges;
}

} // namespace

TEST(GraphPerformanceTest, LargeGraphStepAndFrameStayInsideBudget)
{
    constexpr int kNodes = 5000;
    constexpr int kEdgesPerNode = 2; // ~10 000 edges

    GraphView view;
    view.resize(1600, 1000);
    const QList<GraphNode> nodes = makeNodes(kNodes);
    view.setGraphData(nodes, makeEdges(nodes, kEdgesPerNode));
    ASSERT_EQ(view.nodeCount(), kNodes);
    ASSERT_GT(view.edgeCount(), kNodes);

    // Pull the initial random scatter toward the centre first (outside the
    // timed loop) so the timed frame paints a realistic, mostly-on-screen
    // graph rather than one still spread over a disc wider than the
    // viewport - Obsidian's own graph is judged on the settled picture, not
    // the first frame of the burst.
    view.stepSimulation(150);

    std::vector<double> stepTimes;
    QElapsedTimer clock;
    for (int i = 0; i < 15; ++i) {
        clock.start();
        view.stepSimulation(1);
        stepTimes.push_back(clock.nsecsElapsed() / 1e6);
    }
    const double stepMedian = median(stepTimes);

    QImage image(1600, 1000, QImage::Format_ARGB32_Premultiplied);
    std::vector<double> frameTimes;
    for (int i = 0; i < 12; ++i) {
        image.fill(Qt::black);
        QPainter painter(&image);
        clock.start();
        view.render(&painter);
        painter.end();
        frameTimes.push_back(clock.nsecsElapsed() / 1e6);
    }
    const double frameMedian = median(frameTimes);

    std::printf("[ PERF     ] %d nodes / %d edges: step %.3f ms, frame %.3f ms (median of %zu / %zu runs)\n",
                view.nodeCount(), view.edgeCount(), stepMedian, frameMedian, stepTimes.size(),
                frameTimes.size());

    EXPECT_LT(stepMedian, 16.0);
    EXPECT_LT(frameMedian, 16.0);
}

TEST(GraphPerformanceTest, StepCostIsNotQuadraticInNodeCount)
{
    constexpr int kSmall = 1000;
    constexpr int kLarge = 5000; // 5x kSmall

    const auto medianStepMs = [](int n) {
        GraphView view;
        const QList<GraphNode> nodes = makeNodes(n);
        view.setGraphData(nodes, makeEdges(nodes, 2));
        view.stepSimulation(20); // past the first, most irregular steps

        std::vector<double> times;
        QElapsedTimer clock;
        for (int i = 0; i < 10; ++i) {
            clock.start();
            view.stepSimulation(1);
            times.push_back(clock.nsecsElapsed() / 1e6);
        }
        return median(times);
    };

    const double smallMs = std::max(0.001, medianStepMs(kSmall));
    const double largeMs = medianStepMs(kLarge);
    const double sizeRatio = double(kLarge) / double(kSmall);
    const double timeRatio = largeMs / smallMs;

    std::printf("[ PERF     ] %d nodes: %.3f ms/step, %d nodes: %.3f ms/step (%.1fx nodes, %.1fx time)\n",
                kSmall, smallMs, kLarge, largeMs, sizeRatio, timeRatio);

    // O(n^2) would cost 25x (5x nodes, squared) for 5x the nodes; an O(n log
    // n) Barnes-Hut step costs well under that. The multiplier leaves room
    // for machine noise without accepting a quadratic regression.
    EXPECT_LT(timeRatio, sizeRatio * 5.0);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir configHome;
    QTemporaryDir cacheHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
