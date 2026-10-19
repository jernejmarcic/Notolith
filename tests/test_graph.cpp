#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#include <QApplication>
#include <QDir>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QPainter>
#include <QSignalSpy>
#include <QContextMenuEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTest>

#include "GraphLayout.h"
#include "GraphView.h"
#include "Index.h"
#include "Indexer.h"
#include "Vault.h"
#include "Perf.h"

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

// Obsidian's graph (d3-force) eases into place; an under-damped one rings:
// nodes overshoot their spring length and swing back and forth. Ringing shows
// as nodes reversing direction step after step (before: 32% of moves
// reversed between steps 20 and 150, 277 steps to rest; d3's 0.6 damping and
// degree-scaled springs: 0.1%, 175 steps).
TEST(ForceLayoutTest, SettlesWithoutRinging)
{
    ForceLayout layout;
    const int n = 200;
    layout.reset(n, ringGraph(n, 2));
    QVector<QPointF> previous = layout.positions();
    QVector<QPointF> lastMove(n);
    long moving = 0, reversed = 0;
    int steps = 0;
    for (; steps < 1500 && !layout.isSettled(); ++steps) {
        layout.step();
        const QVector<QPointF> &now = layout.positions();
        for (int i = 0; i < n; ++i) {
            const QPointF move = now[i] - previous[i];
            if (steps >= 20 && steps < 150 && std::hypot(move.x(), move.y()) > 0.05
                && std::hypot(lastMove[i].x(), lastMove[i].y()) > 0.05) {
                ++moving;
                if (QPointF::dotProduct(move, lastMove[i]) < 0)
                    ++reversed;
            }
            lastMove[i] = move;
        }
        previous = now;
    }
    const double fraction = moving ? double(reversed) / double(moving) : 0.0;
    std::printf("[ PERF     ] ring: settled after %d steps, %.3f of moves 20-150 reversed\n", steps, fraction);
    EXPECT_TRUE(layout.isSettled());
    EXPECT_LE(steps, 200) << "slow to come to rest";
    EXPECT_LT(fraction, 0.005) << "nodes keep swinging back and forth";
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

// Obsidian's local graph has Incoming and Outgoing switches: from B, A -> B is
// an incoming link and B -> C an outgoing one.
TEST(GraphViewTest, LocalGraphCanFollowOnlyIncomingOrOnlyOutgoingLinks)
{
    GraphView view;
    view.resize(600, 500);
    view.setGraphData(nodesFor({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}),
                      {{QStringLiteral("A"), QStringLiteral("B")}, {QStringLiteral("B"), QStringLiteral("C")}});
    view.setLocalGraph(true, QStringLiteral("B"), 1);
    EXPECT_EQ(view.nodeCount(), 3);

    GraphOptions options = view.options();
    options.localOutgoing = false;
    view.setOptions(options);
    EXPECT_TRUE(view.containsNode(QStringLiteral("A")));
    EXPECT_FALSE(view.containsNode(QStringLiteral("C"))) << "outgoing off: B's link to C is not followed";

    options.localOutgoing = true;
    options.localIncoming = false;
    view.setOptions(options);
    EXPECT_FALSE(view.containsNode(QStringLiteral("A"))) << "incoming off: A's link to B is not followed";
    EXPECT_TRUE(view.containsNode(QStringLiteral("C")));

    // The switches sit in the Filters section, local mode only, and persist.
    auto *incoming = view.findChild<QCheckBox *>(QStringLiteral("graphLocalIncoming"));
    auto *outgoing = view.findChild<QCheckBox *>(QStringLiteral("graphLocalOutgoing"));
    ASSERT_NE(incoming, nullptr);
    ASSERT_NE(outgoing, nullptr);
    EXPECT_FALSE(incoming->isChecked());
    QSignalSpy changed(&view, &GraphView::optionsChanged);
    incoming->setChecked(true);
    outgoing->setChecked(false);
    EXPECT_EQ(changed.count(), 2);
    EXPECT_TRUE(view.containsNode(QStringLiteral("A")));
    EXPECT_FALSE(view.containsNode(QStringLiteral("C")));

    const GraphOptions restored = GraphOptions::fromJson(view.options().toJson());
    EXPECT_TRUE(restored.localIncoming);
    EXPECT_FALSE(restored.localOutgoing);
}

// Escape leaves the global graph page (the owner returns to the note it came
// from); a sidebar's local graph has nowhere to return to.
TEST(GraphViewTest, EscapeAsksTheOwnerToCloseTheGlobalGraphOnly)
{
    GraphView view;
    QSignalSpy closing(&view, &GraphView::closeRequested);
    QTest::keyClick(&view, Qt::Key_Escape);
    EXPECT_EQ(closing.count(), 1);
    view.setLocalGraph(true, QStringLiteral("A"));
    QTest::keyClick(&view, Qt::Key_Escape);
    EXPECT_EQ(closing.count(), 1);
}

// A right-click on a note node offers Open and Open in new tab; elsewhere nothing.
TEST(GraphViewTest, RightClickingANodeOffersOpenAndOpenInNewTab)
{
    GraphView view;
    view.resize(500, 400);
    view.setGraphData(nodesFor({QStringLiteral("A.md"), QStringLiteral("B.md")}),
                      {{QStringLiteral("A.md"), QStringLiteral("B.md")}});
    view.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&view));
    QTest::qWait(200);
    QSignalSpy opened(&view, &GraphView::noteActivated);
    QSignalSpy openedInTab(&view, &GraphView::noteActivatedInNewTab);

    // Picks the menu entry named `text` from the popup the event opens.
    const auto rightClickAndChoose = [&](const QPoint &at, const QString &text) {
        QTimer::singleShot(100, [text] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (QAction *action : menu->actions()) {
                if (action->text() == text) {
                    menu->setActiveAction(action);
                    QTest::keyClick(menu, Qt::Key_Return);
                    return;
                }
            }
            menu->close();
        });
        QContextMenuEvent event(QContextMenuEvent::Mouse, at, view.mapToGlobal(at));
        QApplication::sendEvent(&view, &event);
        return event.isAccepted();
    };

    const auto onB = [&] { return view.screenPosition(QStringLiteral("B.md")).toPoint(); }; // the layout may still drift
    EXPECT_TRUE(rightClickAndChoose(onB(), QStringLiteral("Open in new tab")));
    ASSERT_EQ(openedInTab.count(), 1);
    EXPECT_EQ(openedInTab.first().first().toString(), QStringLiteral("B.md"));
    EXPECT_EQ(opened.count(), 0);

    EXPECT_TRUE(rightClickAndChoose(onB(), QStringLiteral("Open")));
    ASSERT_EQ(opened.count(), 1);
    EXPECT_EQ(opened.first().first().toString(), QStringLiteral("B.md"));
    EXPECT_EQ(openedInTab.count(), 1);

    EXPECT_FALSE(rightClickAndChoose(QPoint(5, 5), QStringLiteral("Open"))) << "no node there, no menu";
    EXPECT_EQ(opened.count(), 1);
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

TEST(GraphViewTest, RebuildKeepsSurvivingNodesInPlace)
{
    // Every note save rebuilds the graph; it must not re-scatter what is there.
    const QStringList ids{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")};
    const QList<GraphEdge> edges{{QStringLiteral("A"), QStringLiteral("B")},
                                 {QStringLiteral("B"), QStringLiteral("C")}};
    GraphView view;
    view.resize(600, 400);
    view.setGraphData(nodesFor(ids), edges);
    view.stepSimulation(50);
    const QPointF before = view.screenPosition(QStringLiteral("A"));

    view.setGraphData(nodesFor(ids + QStringList{QStringLiteral("D")}),
                      edges + QList<GraphEdge>{{QStringLiteral("C"), QStringLiteral("D")}});
    ASSERT_EQ(view.nodeCount(), 4);
    const QPointF after = view.screenPosition(QStringLiteral("A"));
    EXPECT_LT(std::hypot(after.x() - before.x(), after.y() - before.y()), 1.0);
}

TEST(GraphViewTest, ADraggedNodeFollowsThePointerWhileAWorkerStepsTheLayout)
{
    const QStringList ids{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")};
    GraphView view;
    view.resize(500, 400);
    view.setGraphData(nodesFor(ids), {{QStringLiteral("A"), QStringLiteral("B")},
                                      {QStringLiteral("B"), QStringLiteral("C")}});
    view.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&view));
    QTest::qWait(1500);

    const QPoint grab = view.screenPosition(QStringLiteral("A")).toPoint();
    const QPoint drop = grab + QPoint(40, 30);
    QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, grab);
    QTest::mouseMove(&view, drop);
    // The snapshot answers hit tests at once; the layout catches up on the worker.
    const QPointF held = view.screenPosition(QStringLiteral("A"));
    EXPECT_LT(std::hypot(held.x() - drop.x(), held.y() - drop.y()), 2.0);
    QTest::qWait(300);
    // screenPosition() reads the UI's own snapshot, which the UI overwrites
    // itself whatever the layout did with the pin; copy the layout's positions
    // into it first, so this fails if setPinned()/setPosition() never landed.
    view.stepSimulation(0);
    const QPointF later = view.screenPosition(QStringLiteral("A"));
    EXPECT_LT(std::hypot(later.x() - drop.x(), later.y() - drop.y()), 2.0) << "the pinned node slid away";
    QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, drop);
    QTest::qWait(300); // and the layout settles again without a crash
    EXPECT_EQ(view.nodeCount(), 3);
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

TEST(GraphViewTest, SearchFilterDropsNonMatchingNotes)
{
    QList<GraphNode> nodes = nodesFor({QStringLiteral("Projects/A.md"), QStringLiteral("Projects/B.md"),
                                       QStringLiteral("Journal/C.md")});
    nodes[0].tags = {QStringLiteral("work")};
    GraphNode tag;
    tag.id = QStringLiteral("tag:work");
    tag.label = QStringLiteral("#work");
    tag.kind = GraphNode::Kind::Tag;
    nodes << tag;
    const QList<GraphEdge> edges{{QStringLiteral("Projects/A.md"), QStringLiteral("Projects/B.md")},
                                 {QStringLiteral("Journal/C.md"), QStringLiteral("Projects/B.md")},
                                 {QStringLiteral("Projects/A.md"), QStringLiteral("tag:work")}};
    GraphView view;
    view.setGraphData(nodes, edges);
    ASSERT_EQ(view.nodeCount(), 4);

    GraphOptions options = view.options();
    options.search = QStringLiteral("path:Projects");
    view.setOptions(options);
    EXPECT_FALSE(view.containsNode(QStringLiteral("Journal/C.md")));
    EXPECT_TRUE(view.containsNode(QStringLiteral("Projects/A.md")));
    EXPECT_TRUE(view.containsNode(QStringLiteral("tag:work"))) << "a tag still linked from a kept note stays";
    EXPECT_EQ(view.edgeCount(), 2);

    options.search = QStringLiteral("path:Journal");
    view.setOptions(options);
    EXPECT_EQ(view.nodeCount(), 1) << "the tag and the notes it linked to are gone";
}

TEST(GraphViewTest, DepthSliderDrivesLocalGraph)
{
    const QStringList ids{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D")};
    GraphView view;
    view.resize(600, 500);
    view.setGraphData(nodesFor(ids), {{QStringLiteral("A"), QStringLiteral("B")},
                                      {QStringLiteral("B"), QStringLiteral("C")},
                                      {QStringLiteral("C"), QStringLiteral("D")}});
    auto *slider = view.findChild<QSlider *>(QStringLiteral("graphDepthSlider"));
    ASSERT_NE(slider, nullptr);
    view.setLocalGraph(true, QStringLiteral("A"), 1);
    EXPECT_EQ(view.nodeCount(), 2);
    QSignalSpy changed(&view, &GraphView::optionsChanged);
    slider->setValue(3);
    EXPECT_EQ(view.nodeCount(), 4);
    EXPECT_EQ(view.options().localDepth, 3);
    EXPECT_EQ(changed.count(), 1);
    view.setLocalGraph(true, QStringLiteral("A")); // no depth: the slider's value stands
    EXPECT_EQ(view.nodeCount(), 4);
}

TEST(GraphViewTest, GroupsEditorAddsAGroup)
{
    GraphView view;
    view.resize(600, 500);
    view.setGraphData(nodesFor({QStringLiteral("Projects/A.md"), QStringLiteral("B.md")}), {});
    auto *add = view.findChild<QPushButton *>(QStringLiteral("graphNewGroupButton"));
    ASSERT_NE(add, nullptr);
    const QColor before = view.nodeColor(QStringLiteral("Projects/A.md"));
    add->click();
    ASSERT_EQ(view.options().colorGroups.size(), 1);
    EXPECT_EQ(view.nodeColor(QStringLiteral("Projects/A.md")), before) << "an empty query colours nothing";

    // The new row's query box, found by its placeholder.
    QLineEdit *query = nullptr;
    for (auto *edit : view.findChildren<QLineEdit *>()) {
        if (edit->placeholderText() == QStringLiteral("Enter query..."))
            query = edit;
    }
    ASSERT_NE(query, nullptr);
    query->setText(QStringLiteral("path:Projects"));
    // The tint follows a typing pause, not every keystroke; the option is kept at once.
    EXPECT_EQ(view.options().colorGroups.first().query, QStringLiteral("path:Projects"));
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return view.nodeColor(QStringLiteral("Projects/A.md")) == view.options().colorGroups.first().color;
    }, 2000));
    EXPECT_EQ(view.nodeColor(QStringLiteral("B.md")), before);
}

TEST(GraphViewTest, AForceSliderRestartsASettledLayout)
{
    const QStringList ids{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D")};
    GraphView view;
    view.resize(500, 400);
    view.setGraphData(nodesFor(ids), {{QStringLiteral("A"), QStringLiteral("B")},
                                      {QStringLiteral("B"), QStringLiteral("C")},
                                      {QStringLiteral("C"), QStringLiteral("D")}});
    view.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&view));
    QTest::qWait(2500); // settled, the timer stopped
    const QPointF resting = view.screenPosition(QStringLiteral("A"));
    auto *repel = view.findChild<QSlider *>(QStringLiteral("graphRepelSlider"));
    ASSERT_NE(repel, nullptr);
    repel->setValue(repel->maximum()); // a settled graph must wake and move
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            const QPointF now = view.screenPosition(QStringLiteral("A"));
            return std::hypot(now.x() - resting.x(), now.y() - resting.y()) > 1.0;
        },
        2000));
}

TEST(GraphViewTest, OptionsRoundTripThroughJson)
{
    GraphOptions options;
    options.search = QStringLiteral("tag:#x");
    options.showTags = true;
    options.animate = false;
    options.localDepth = 3;
    options.collapseDisplay = false;
    options.textFadeMultiplier = -1.5;
    options.colorGroups = {{QStringLiteral("path:A"), QColor(0x12, 0x34, 0x56)}};

    const GraphOptions back = GraphOptions::fromJson(options.toJson());
    EXPECT_EQ(back.search, options.search);
    EXPECT_TRUE(back.showTags);
    EXPECT_FALSE(back.animate);
    EXPECT_EQ(back.localDepth, 3);
    EXPECT_FALSE(back.collapseDisplay);
    EXPECT_DOUBLE_EQ(back.textFadeMultiplier, -1.5);
    ASSERT_EQ(back.colorGroups.size(), 1);
    EXPECT_EQ(back.colorGroups.first().color.rgb() & 0xFFFFFF, 0x123456u);
    // Obsidian's own spelling of the same group and fold state.
    const QJsonObject obsidian = options.toJson();
    EXPECT_EQ(obsidian.value(QLatin1String("collapse-display")).toBool(), false);
    EXPECT_EQ(obsidian.value(QLatin1String("colorGroups")).toArray().first().toObject().value(QLatin1String("color")).toObject().value(QLatin1String("rgb")).toInt(), 0x123456);
}

TEST(GraphViewTest, WithAnimationOffTheLayoutIsSettledBeforeTheFirstFrame)
{
    const QStringList ids{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D")};
    GraphView view;
    view.resize(500, 400);
    GraphOptions options = view.options();
    options.animate = false;
    view.setOptions(options);
    view.setGraphData(nodesFor(ids), {{QStringLiteral("A"), QStringLiteral("B")},
                                      {QStringLiteral("B"), QStringLiteral("C")},
                                      {QStringLiteral("C"), QStringLiteral("D")}});
    const QPointF scattered = view.screenPosition(QStringLiteral("A"));
    view.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&view));
    // One job carries it to rest: the first picture already shows the result.
    ASSERT_TRUE(QTest::qWaitFor([&] { return view.screenPosition(QStringLiteral("A")) != scattered; }, 3000));
    const QPointF first = view.screenPosition(QStringLiteral("A"));
    QTest::qWait(300);
    const QPointF later = view.screenPosition(QStringLiteral("A"));
    EXPECT_LT(std::hypot(later.x() - first.x(), later.y() - first.y()), 3.0);
}

namespace {
// Labels are text-coloured pixels: count how many the frame holds.
int textPixels(const QImage &frame)
{
    int count = 0;
    for (int y = 0; y < frame.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
        for (int x = 0; x < frame.width(); ++x)
            count += qRed(row[x]) > 200 && qGreen(row[x]) > 200; // the default palette's white text
    }
    return count;
}

QList<GraphEdge> chainOf(const QStringList &ids)
{
    QList<GraphEdge> edges;
    for (int i = 1; i < ids.size(); ++i)
        edges.append({ids.at(i - 1), ids.at(i)});
    return edges;
}
} // namespace

TEST(GraphViewTest, ASmallGraphOpensLabelled)
{
    const QStringList ids{QStringLiteral("alpha"), QStringLiteral("beta"), QStringLiteral("gamma"),
                          QStringLiteral("delta")};
    GraphView view;
    view.resize(500, 400);
    view.setGraphData(nodesFor(ids), chainOf(ids));
    view.stepSimulation(300);
    EXPECT_GT(textPixels(view.renderFrameNow()), 0) << "four notes fit at full size, labels and all";
}

TEST(GraphViewTest, LabelsFadeInAsTheViewZoomsIn)
{
    QStringList ids;
    for (int i = 0; i < 400; ++i)
        ids << QStringLiteral("note%1").arg(i);
    GraphView view;
    view.resize(500, 400);
    view.setGraphData(nodesFor(ids), chainOf(ids));
    view.stepSimulation(300);
    const int far = textPixels(view.renderFrameNow());
    // Zoom in about the centre: the same graph, nearer.
    for (int i = 0; i < 15; ++i) {
        QWheelEvent wheel(QPointF(250, 200), QPointF(250, 200), QPoint(), QPoint(0, 120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&view, &wheel);
    }
    const int near = textPixels(view.renderFrameNow());
    EXPECT_EQ(far, 0) << "a big graph fitted far out shows no label";
    EXPECT_GT(near, 0) << "labels appear once zoomed in";
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

TEST_F(GraphDataTest, TheWorkerThreadReadMatchesTheSynchronousRead)
{
    Index index;
    build(index);
    const Index::GraphData sync = index.graphData();
    const Index::GraphData worker = Index::graphDataIn(index.graphSnapshot());
    ASSERT_EQ(worker.files.size(), sync.files.size());
    ASSERT_EQ(worker.links.size(), sync.links.size());
    for (qsizetype i = 0; i < sync.links.size(); ++i) {
        EXPECT_EQ(worker.links[i].fromPath, sync.links[i].fromPath);
        EXPECT_EQ(worker.links[i].toPath, sync.links[i].toPath);
    }
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
    ASSERT_TRUE(QTest::qWaitFor([&] { return view.nodeCount() == 4; }, 5000)); // read on a worker
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
    ASSERT_TRUE(QTest::qWaitFor([&] { return view.nodeCount() == 2; }, 5000));
    // Orphan.md has no links at all, so it drops; the unresolved node goes too.
    EXPECT_FALSE(view.containsNode(QStringLiteral("unresolved:Missing Note")));
    EXPECT_FALSE(view.containsNode(QStringLiteral("Orphan.md")));
    EXPECT_TRUE(view.containsNode(QStringLiteral("Home.md")));
    EXPECT_TRUE(view.containsNode(QStringLiteral("Ideas.md")));
}

// An index update about some other note (every autosave, sync tick and file
// watcher event fires Index::changed) must not restart a graph that is
// sitting still: a fresh layout reheated every node and the graph boiled.
TEST_F(GraphDataTest, AnIndexUpdateThatChangesNothingLeavesALayoutAlone)
{
    Index index;
    build(index);
    GraphView view;
    view.resize(500, 400);
    view.setIndex(&index);
    ASSERT_TRUE(QTest::qWaitFor([&] { return view.nodeCount() == 4; }, 5000));
    view.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&view));
    QTest::qWait(2500); // settled, the timer stopped
    const int generation = view.layoutGeneration();
    const QPointF resting = view.screenPosition(QStringLiteral("Home.md"));

    for (int i = 0; i < 5; ++i) {
        emit index.changed();
        QTest::qWait(100); // the re-read runs on a worker
    }
    EXPECT_EQ(view.layoutGeneration(), generation) << "the layout was replaced and reheated";
    EXPECT_EQ(view.screenPosition(QStringLiteral("Home.md")), resting);
}

TEST_F(GraphDataTest, AHiddenGraphCatchesUpOnIndexChangesWhenShown)
{
    Index index;
    build(index);
    GraphView view;
    view.setIndex(&index);
    ASSERT_TRUE(QTest::qWaitFor([&] { return view.nodeCount() == 4; }, 5000));

    // Hidden: a change must not re-read the vault, only mark it stale.
    emit index.changed();
    view.setGraphData(nodesFor({QStringLiteral("X")}), {});
    emit index.changed();
    EXPECT_EQ(view.nodeCount(), 1);

    view.show();
    EXPECT_TRUE(QTest::qWaitFor([&] { return view.nodeCount() == 4; }, 5000));
}

// A local graph that was hidden while the open note changed and the index
// changed: shown, it has the new note's neighbourhood at once, not the old
// one's (or the whole vault's) until the read on the worker lands.
TEST_F(GraphDataTest, AHiddenLocalGraphShowsTheCurrentNotesNeighbourhoodWhenShown)
{
    Index index;
    build(index);
    GraphView view;
    view.setLocalGraph(true);
    view.setIndex(&index);
    ASSERT_TRUE(QTest::qWaitFor([&] { return view.nodeCount() == 4; }, 5000)); // no current note: all of it

    emit index.changed();
    view.setCurrentNote(QStringLiteral("Orphan.md"));
    view.show();
    EXPECT_EQ(view.nodeCount(), 1) << "before the worker's read lands";
    EXPECT_TRUE(view.containsNode(QStringLiteral("Orphan.md")));
    QTest::qWait(300); // the read lands: still the neighbourhood
    EXPECT_EQ(view.nodeCount(), 1);
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

    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    // Best of three medians for each: load only ever adds time.
    QElapsedTimer clock;
    const double stepMedian = perf::bestOf([&] {
        std::vector<double> stepTimes;
        for (int i = 0; i < 15; ++i) {
            clock.start();
            view.stepSimulation(1);
            stepTimes.push_back(clock.nsecsElapsed() / 1e6);
        }
        return median(stepTimes);
    }, 3, 16.0);

    const double frameMedian = perf::bestOf([&] {
        std::vector<double> frameTimes;
        for (int i = 0; i < 12; ++i) {
            clock.start();
            const QImage image = view.renderFrameNow();
            frameTimes.push_back(clock.nsecsElapsed() / 1e6);
            if (image.isNull())
                return 1e9;
        }
        return median(frameTimes);
    }, 3, 16.0);

    std::printf("[ PERF     ] %d nodes / %d edges: step %.3f ms, frame %.3f ms (best of three medians)\n",
                view.nodeCount(), view.edgeCount(), stepMedian, frameMedian);

    EXPECT_LT(stepMedian, 16.0);
    EXPECT_LT(frameMedian, 16.0);
}

// A rebuild (every note switch in a local graph, every index change while the
// graph is shown) used to wait for the worker job, which with Animate off runs
// for up to a quarter second. A smaller graph than the 20 000-note case is
// enough: it is the wait, not the rebuild itself, that is measured.
TEST(GraphPerformanceTest, ARebuildDoesNotWaitForTheWorkerJob)
{
    constexpr int kNodes = 3000;
    GraphView view;
    view.resize(1600, 1000);
    GraphOptions options = view.options();
    options.animate = false;
    view.setOptions(options);
    const QList<GraphNode> nodes = makeNodes(kNodes);
    const QList<GraphEdge> edges = makeEdges(nodes, 2);
    view.setGraphData(nodes, edges);
    view.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&view));
    QTest::qWait(40); // a settling job is now running

    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    QElapsedTimer clock;
    const double ms = perf::bestOf([&] {
        clock.start();
        view.setGraphData(nodes, edges);
        const double took = clock.nsecsElapsed() / 1e6;
        QTest::qWait(40); // let the next job start
        return took;
    }, 3, 10.0);
    std::printf("[ PERF     ] rebuild of %d nodes with a job running: %.3f ms\n", kNodes, ms);
    EXPECT_LT(ms, 10.0);
    EXPECT_EQ(view.nodeCount(), kNodes);
}

// Typing in a colour group's query rescans every node; per keystroke that is
// work the Filters search box already debounces.
TEST(GraphPerformanceTest, TypingAGroupQueryDoesNotRescanEveryNodePerKeystroke)
{
    constexpr int kNodes = 20000;
    GraphView view;
    view.resize(1600, 1000);
    const QList<GraphNode> nodes = makeNodes(kNodes);
    view.setGraphData(nodes, makeEdges(nodes, 1));
    auto *add = view.findChild<QPushButton *>(QStringLiteral("graphNewGroupButton"));
    ASSERT_NE(add, nullptr);
    add->click();
    QLineEdit *query = nullptr;
    for (auto *edit : view.findChildren<QLineEdit *>()) {
        if (edit->placeholderText() == QStringLiteral("Enter query..."))
            query = edit;
    }
    ASSERT_NE(query, nullptr);

    const QString id = QStringLiteral("note-7");
    const QColor before = view.nodeColor(id);
    QElapsedTimer clock;
    clock.start();
    QString typed;
    for (const QChar ch : QStringLiteral("path:note-7"))
        query->setText(typed += ch);
    const double ms = clock.nsecsElapsed() / 1e6;
    std::printf("[ PERF     ] 11 group-query keystrokes at %d nodes: %.3f ms\n", kNodes, ms);
    EXPECT_EQ(view.nodeColor(id), before) << "the nodes are rescanned after a pause, not per keystroke";
    ASSERT_TRUE(QTest::qWaitFor([&] { return view.nodeColor(id) != before; }, 3000));
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    EXPECT_LT(ms, 10.0);
}

// Reports, does not budget: what a 20 000-note vault costs per step, per
// frame and per rebuild (docs/PERFORMANCE.md). Only finiteness is asserted.
TEST(GraphPerformanceTest, TwentyThousandNodesReportCosts)
{
    constexpr int kNodes = 20000;
    GraphView view;
    view.resize(1600, 1000);
    const QList<GraphNode> nodes = makeNodes(kNodes);
    const QList<GraphEdge> edges = makeEdges(nodes, 2);

    QElapsedTimer clock;
    clock.start();
    view.setGraphData(nodes, edges);
    const double rebuildMs = clock.nsecsElapsed() / 1e6;
    ASSERT_EQ(view.nodeCount(), kNodes);

    view.stepSimulation(100);
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    std::vector<double> stepTimes;
    for (int i = 0; i < 8; ++i) {
        clock.start();
        view.stepSimulation(1);
        stepTimes.push_back(clock.nsecsElapsed() / 1e6);
    }
    std::vector<double> frameTimes;
    for (int i = 0; i < 6; ++i) {
        clock.start();
        const QImage image = view.renderFrameNow();
        frameTimes.push_back(clock.nsecsElapsed() / 1e6);
        ASSERT_FALSE(image.isNull());
    }
    const double step = median(stepTimes);
    const double frame = median(frameTimes);

    // What the UI thread itself pays per paint once a worker has delivered a
    // frame: drawing one finished image.
    view.show();
    QTest::qWait(1500);
    QImage target(1600, 1000, QImage::Format_ARGB32_Premultiplied);
    std::vector<double> blitTimes;
    for (int i = 0; i < 8; ++i) {
        QPainter painter(&target);
        clock.start();
        view.render(&painter);
        painter.end();
        blitTimes.push_back(clock.nsecsElapsed() / 1e6);
    }
    const double blit = median(blitTimes);
    std::printf("[ PERF     ] %d nodes / %d edges: rebuild %.1f ms, step %.2f ms, render %.2f ms (on a worker), UI blit %.2f ms\n",
                view.nodeCount(), view.edgeCount(), rebuildMs, step, frame, blit);
    EXPECT_TRUE(std::isfinite(step) && std::isfinite(frame));
    EXPECT_LT(blit, 5.0) << "the UI thread must only draw a finished image";
}

// Opt-in (NOTEAPPC_GRAPH_BENCH_NOTES=20000): indexes that many linked notes
// and times what opening the graph costs on the UI thread. Prints only.
TEST(GraphPerformanceTest, BigVaultOpenCosts)
{
    const int notes = qEnvironmentVariableIntValue("NOTEAPPC_GRAPH_BENCH_NOTES");
    if (notes <= 0)
        GTEST_SKIP() << "set NOTEAPPC_GRAPH_BENCH_NOTES=20000 to run";
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (int i = 0; i < notes; ++i) {
        QFile file(dir.filePath(QStringLiteral("n%1.md").arg(i)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QStringLiteral("# n%1\n[[n%2]] [[n%3]]\n").arg(i).arg((i * 7 + 1) % notes).arg((i + 97) % notes).toUtf8());
    }
    Index index;
    Indexer indexer(&index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(Vault(dir.path()));
    ASSERT_TRUE(done.wait(120000));

    QElapsedTimer clock;
    clock.start();
    const Index::GraphData data = index.graphData();
    const double dataMs = clock.nsecsElapsed() / 1e6;

    GraphView view;
    view.resize(1600, 1000);
    clock.start();
    view.setIndex(&index);
    const double blockedMs = clock.nsecsElapsed() / 1e6;
    // Longest gap between two event-loop turns while the worker reads and the
    // graph is rebuilt: what the user would feel as a freeze.
    double worstGap = 0;
    clock.start();
    QElapsedTimer sinceReady;
    sinceReady.start();
    while (view.nodeCount() != notes && sinceReady.elapsed() < 20000) {
        QElapsedTimer turn;
        turn.start();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        worstGap = std::max(worstGap, turn.nsecsElapsed() / 1e6);
        QThread::msleep(1);
    }
    ASSERT_EQ(view.nodeCount(), notes);
    const double readyMs = blockedMs + clock.nsecsElapsed() / 1e6;
    std::printf("[ PERF     ] %d notes (%d links): Index::graphData() %.1f ms; setIndex() blocks the UI %.1f ms, "
                "graph ready after %.1f ms, longest UI stall while loading %.1f ms\n",
                notes, int(data.links.size()), dataMs, blockedMs, readyMs, worstGap);
}

TEST(GraphPerformanceTest, StepCostIsNotQuadraticInNodeCount)
{
    constexpr int kSmall = 1000;
    constexpr int kLarge = 5000; // 5x kSmall

    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    const auto medianStepOnce = [](int n) {
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
    // Best of three: load only ever adds time to either side of the ratio.
    const auto medianStepMs = [&](int n) { return perf::bestOf([&] { return medianStepOnce(n); }); };

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
