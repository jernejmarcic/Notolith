// Canvas: the JSON Canvas 1.0 model (round trip, colour presets), every
// interactive edit and its undo, hit testing, and a performance guard for
// panning a canvas of 1 000 nodes.

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "CanvasCommands.h"
#include "CanvasDocument.h"
#include "CanvasView.h"
#include "Theme.h"

using namespace noteAppC;

namespace {

/// Mouse events sent the way Qt Widgets delivers them - press, move (button
/// held, per Qt's own convention that a move event's own button() is always
/// NoButton), release - so a test exercises the same event sequence a real
/// drag produces. `button` is whichever one is down for the whole gesture.
void send(QWidget *target, QEvent::Type type, QPoint at, Qt::MouseButton button,
          Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const Qt::MouseButton eventButton = type == QEvent::MouseMove ? Qt::NoButton : button;
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent event(type, QPointF(at), target->mapToGlobal(QPointF(at)), eventButton, held, modifiers);
    QCoreApplication::sendEvent(target, &event);
}

CanvasNode makeTextNode(const QRectF &rect, const QString &text = QString())
{
    CanvasNode node;
    node.setType(CanvasNodeType::Text);
    node.setRect(rect);
    node.setText(text);
    return node;
}

const QByteArray kSpecExample = R"JSON({
  "nodes": [
    {"id": "n1", "type": "text", "text": "Hello, canvas", "x": 0, "y": 0, "width": 250, "height": 120},
    {"id": "n2", "type": "file", "file": "Note.md", "x": 400, "y": 0, "width": 250, "height": 120},
    {"id": "n3", "type": "link", "url": "https://jsoncanvas.org", "x": 0, "y": 200, "width": 250, "height": 120, "color": "4"},
    {"id": "n4", "type": "group", "label": "A group", "x": -40, "y": -40, "width": 720, "height": 400}
  ],
  "edges": [
    {"id": "e1", "fromNode": "n1", "fromSide": "right", "toNode": "n2", "toSide": "left", "color": "1", "label": "see also"}
  ]
}
)JSON";

} // namespace

// --- model: parse, serialize, colours ------------------------------------

TEST(CanvasDocumentTest, RoundTripsSpecExampleByteForByte)
{
    CanvasDocument doc;
    QString error;
    ASSERT_TRUE(doc.parse(kSpecExample, &error)) << error.toStdString();
    EXPECT_EQ(doc.nodes().size(), 4);
    EXPECT_EQ(doc.edges().size(), 1);
    // Nothing was edited: every byte comes back exactly as it went in,
    // whatever this build's own JSON formatting would otherwise have been.
    EXPECT_EQ(doc.serialize(), kSpecExample);
}

TEST(CanvasDocumentTest, LoadAndSaveRoundTripToDiskByteForByte)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("round-trip.canvas"));
    QFile seed(path);
    ASSERT_TRUE(seed.open(QIODevice::WriteOnly));
    seed.write(kSpecExample);
    seed.close();

    CanvasDocument doc;
    QString error;
    ASSERT_TRUE(doc.load(path, &error)) << error.toStdString();
    EXPECT_EQ(doc.path(), path);
    ASSERT_TRUE(doc.save(QString(), &error)) << error.toStdString();

    QFile check(path);
    ASSERT_TRUE(check.open(QIODevice::ReadOnly));
    EXPECT_EQ(check.readAll(), kSpecExample);
}

TEST(CanvasDocumentTest, KeepsUnknownKeysAfterAnEdit)
{
    const QByteArray withUnknownKeys = R"JSON({
  "nodes": [
    {"id": "n1", "type": "text", "text": "A", "x": 0, "y": 0, "width": 200, "height": 100, "futureField": "keep-me"},
    {"id": "n2", "type": "text", "text": "B", "x": 300, "y": 0, "width": 200, "height": 100}
  ],
  "edges": [
    {"id": "e1", "fromNode": "n1", "toNode": "n2", "aPluginsOwnField": 42}
  ],
  "extraRootField": "keep-me-too"
}
)JSON";
    CanvasDocument doc;
    ASSERT_TRUE(doc.parse(withUnknownKeys));
    ASSERT_TRUE(doc.setNodeText(QStringLiteral("n1"), QStringLiteral("Edited")));

    QString error;
    CanvasDocument reloaded;
    ASSERT_TRUE(reloaded.parse(doc.serialize(), &error)) << error.toStdString();

    const CanvasNode *n1 = reloaded.node(QStringLiteral("n1"));
    ASSERT_NE(n1, nullptr);
    EXPECT_EQ(n1->text(), QStringLiteral("Edited"));
    EXPECT_EQ(n1->json().value(QStringLiteral("futureField")).toString(), QStringLiteral("keep-me"));

    const CanvasNode *n2 = reloaded.node(QStringLiteral("n2"));
    ASSERT_NE(n2, nullptr);
    EXPECT_EQ(n2->text(), QStringLiteral("B")); // untouched node, still exactly as it was

    const CanvasEdge *e1 = reloaded.edge(QStringLiteral("e1"));
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->json().value(QStringLiteral("aPluginsOwnField")).toInt(), 42);

    const QJsonObject root = QJsonDocument::fromJson(doc.serialize()).object();
    EXPECT_EQ(root.value(QStringLiteral("extraRootField")).toString(), QStringLiteral("keep-me-too"));
}

TEST(CanvasDocumentTest, ColorPresetsResolveAgainstTheThemeAndHexPassesThrough)
{
    const Theme theme = Theme::builtin();
    EXPECT_EQ(resolveCanvasColor(QStringLiteral("1"), theme), theme.color(QStringLiteral("--color-red")));
    EXPECT_EQ(resolveCanvasColor(QStringLiteral("6"), theme), theme.color(QStringLiteral("--color-purple")));
    EXPECT_EQ(resolveCanvasColor(QStringLiteral("#123456"), theme), QColor(QStringLiteral("#123456")));
    EXPECT_FALSE(resolveCanvasColor(QString(), theme).isValid());
}

TEST(CanvasDocumentTest, EdgeEndsDefaultPerSpecAndRoundTrip)
{
    CanvasEdge edge;
    EXPECT_EQ(edge.fromEnd(), CanvasEdgeEnd::None);
    EXPECT_EQ(edge.toEnd(), CanvasEdgeEnd::Arrow);
    edge.setFromEnd(CanvasEdgeEnd::Arrow);
    edge.setToEnd(CanvasEdgeEnd::None);
    EXPECT_EQ(edge.fromEnd(), CanvasEdgeEnd::Arrow);
    EXPECT_EQ(edge.toEnd(), CanvasEdgeEnd::None);
}

// --- commands: every edit, and its undo -----------------------------------

TEST(CanvasCommandsTest, AddNodeUndoRedo)
{
    CanvasDocument doc;
    QUndoStack stack;
    auto *command = new AddNodeCommand(&doc, makeTextNode(QRectF(0, 0, 100, 50), QStringLiteral("hi")));
    stack.push(command);
    const QString id = command->nodeID();

    ASSERT_NE(doc.node(id), nullptr);
    EXPECT_EQ(doc.node(id)->text(), QStringLiteral("hi"));

    stack.undo();
    EXPECT_EQ(doc.node(id), nullptr);

    stack.redo();
    ASSERT_NE(doc.node(id), nullptr);
    EXPECT_EQ(doc.node(id)->text(), QStringLiteral("hi"));
}

TEST(CanvasCommandsTest, RemoveNodeUndoRedoRestoresEveryField)
{
    CanvasDocument doc;
    const QString id = doc.addNode(makeTextNode(QRectF(10, 20, 100, 50), QStringLiteral("keep me")));
    QUndoStack stack;
    stack.push(new RemoveNodeCommand(&doc, id));
    EXPECT_EQ(doc.node(id), nullptr);

    stack.undo();
    ASSERT_NE(doc.node(id), nullptr);
    EXPECT_EQ(doc.node(id)->text(), QStringLiteral("keep me"));
    EXPECT_EQ(doc.node(id)->rect(), QRectF(10, 20, 100, 50));

    stack.redo();
    EXPECT_EQ(doc.node(id), nullptr);
}

TEST(CanvasCommandsTest, MoveResizeUndoRedo)
{
    CanvasDocument doc;
    const QString id = doc.addNode(makeTextNode(QRectF(0, 0, 100, 50)));
    QUndoStack stack;
    stack.push(new MoveResizeNodesCommand(&doc, {{id, QRectF(0, 0, 100, 50)}}, {{id, QRectF(20, 20, 140, 70)}},
                                          QStringLiteral("Move")));
    EXPECT_EQ(doc.node(id)->rect(), QRectF(20, 20, 140, 70));
    stack.undo();
    EXPECT_EQ(doc.node(id)->rect(), QRectF(0, 0, 100, 50));
    stack.redo();
    EXPECT_EQ(doc.node(id)->rect(), QRectF(20, 20, 140, 70));
}

TEST(CanvasCommandsTest, EditTextUndoRedo)
{
    CanvasDocument doc;
    const QString id = doc.addNode(makeTextNode(QRectF(0, 0, 100, 50), QStringLiteral("before")));
    QUndoStack stack;
    stack.push(new EditTextCommand(&doc, id, QStringLiteral("before"), QStringLiteral("after")));
    EXPECT_EQ(doc.node(id)->text(), QStringLiteral("after"));
    stack.undo();
    EXPECT_EQ(doc.node(id)->text(), QStringLiteral("before"));
    stack.redo();
    EXPECT_EQ(doc.node(id)->text(), QStringLiteral("after"));
}

TEST(CanvasCommandsTest, AddAndRemoveEdgeUndoRedo)
{
    CanvasDocument doc;
    const QString a = doc.addNode(makeTextNode(QRectF(0, 0, 100, 50)));
    const QString b = doc.addNode(makeTextNode(QRectF(300, 0, 100, 50)));
    QUndoStack stack;

    CanvasEdge edge;
    edge.setFromNode(a);
    edge.setToNode(b);
    auto *add = new AddEdgeCommand(&doc, edge);
    stack.push(add);
    const QString edgeID = add->edgeID();
    ASSERT_NE(doc.edge(edgeID), nullptr);

    stack.push(new RemoveEdgeCommand(&doc, edgeID));
    EXPECT_EQ(doc.edge(edgeID), nullptr);

    stack.undo(); // undoes the remove
    ASSERT_NE(doc.edge(edgeID), nullptr);
    EXPECT_EQ(doc.edge(edgeID)->fromNode(), a);

    stack.undo(); // undoes the add
    EXPECT_EQ(doc.edge(edgeID), nullptr);

    stack.redo(); // redoes the add
    stack.redo(); // redoes the remove
    EXPECT_EQ(doc.edge(edgeID), nullptr);
}

// --- CanvasView: mouse/keyboard editing, undoable through the view --------

class CanvasViewTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        view = std::make_unique<CanvasView>();
        view->resize(800, 600); // zoom 1, origin (0,0): canvas coords == viewport pixels
    }
    std::unique_ptr<CanvasView> view;
};

TEST_F(CanvasViewTest, DoubleClickOnEmptySpaceCreatesAndOpensATextNode)
{
    EXPECT_EQ(view->document().nodes().size(), 0);
    send(view.get(), QEvent::MouseButtonDblClick, QPoint(400, 300), Qt::LeftButton);

    ASSERT_EQ(view->document().nodes().size(), 1);
    const CanvasNode &node = view->document().nodes().first();
    EXPECT_EQ(node.type(), CanvasNodeType::Text);
    EXPECT_TRUE(view->isEditingText());
    EXPECT_EQ(view->editingNodeID(), node.id());
    EXPECT_TRUE(view->isModified());

    view->commitEditingText();
    EXPECT_FALSE(view->isEditingText());
}

TEST_F(CanvasViewTest, ClickingOutsideTheInlineEditorCommitsIt)
{
    const QString id = view->createTextNodeAt(QPointF(100, 100));
    ASSERT_TRUE(view->beginEditingText(id));
    ASSERT_TRUE(view->isEditingText());

    send(view.get(), QEvent::MouseButtonPress, QPoint(700, 500), Qt::LeftButton);
    send(view.get(), QEvent::MouseButtonRelease, QPoint(700, 500), Qt::LeftButton);

    EXPECT_FALSE(view->isEditingText());
}

TEST_F(CanvasViewTest, EscapeCommitsTheInlineEditor)
{
    const QString id = view->createTextNodeAt(QPointF(100, 100));
    ASSERT_TRUE(view->beginEditingText(id));
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(view->findChild<QObject *>(QStringLiteral("canvasTextEditor")), &escape);
    EXPECT_FALSE(view->isEditingText());
}

TEST_F(CanvasViewTest, ClickSelectsAndShiftClickExtends)
{
    const QString a = view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));
    const QString b = view->document().addNode(makeTextNode(QRectF(200, 0, 100, 50)));

    send(view.get(), QEvent::MouseButtonPress, QPoint(50, 25), Qt::LeftButton);
    send(view.get(), QEvent::MouseButtonRelease, QPoint(50, 25), Qt::LeftButton);
    EXPECT_EQ(view->selectedNodeIDs(), QStringList{a});

    send(view.get(), QEvent::MouseButtonPress, QPoint(250, 25), Qt::LeftButton, Qt::ShiftModifier);
    send(view.get(), QEvent::MouseButtonRelease, QPoint(250, 25), Qt::LeftButton, Qt::ShiftModifier);
    const QStringList selected = view->selectedNodeIDs();
    EXPECT_EQ(selected.size(), 2);
    EXPECT_TRUE(selected.contains(a));
    EXPECT_TRUE(selected.contains(b));
}

TEST_F(CanvasViewTest, ShiftDragRubberBandsOverIntersectingNodes)
{
    const QString a = view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));
    const QString b = view->document().addNode(makeTextNode(QRectF(200, 0, 100, 50)));
    const QString farAway = view->document().addNode(makeTextNode(QRectF(2000, 2000, 100, 50)));

    // Starts a little above and left of node `a` - empty space, not its
    // corner, which would hit the node itself instead of starting a band.
    send(view.get(), QEvent::MouseButtonPress, QPoint(-20, -20), Qt::LeftButton, Qt::ShiftModifier);
    send(view.get(), QEvent::MouseMove, QPoint(320, 80), Qt::LeftButton, Qt::ShiftModifier);
    send(view.get(), QEvent::MouseButtonRelease, QPoint(320, 80), Qt::LeftButton, Qt::ShiftModifier);

    const QStringList selected = view->selectedNodeIDs();
    EXPECT_EQ(selected.size(), 2);
    EXPECT_TRUE(selected.contains(a));
    EXPECT_TRUE(selected.contains(b));
    EXPECT_FALSE(selected.contains(farAway));
}

TEST_F(CanvasViewTest, DraggingASelectedNodeMovesItAndIsUndoable)
{
    const QString id = view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));

    send(view.get(), QEvent::MouseButtonPress, QPoint(50, 25), Qt::LeftButton);
    send(view.get(), QEvent::MouseMove, QPoint(150, 75), Qt::LeftButton);
    send(view.get(), QEvent::MouseButtonRelease, QPoint(150, 75), Qt::LeftButton);

    const QRectF moved = view->document().node(id)->rect();
    EXPECT_NEAR(moved.x(), 100.0, 0.5);
    EXPECT_NEAR(moved.y(), 50.0, 0.5);
    EXPECT_EQ(view->undoStack()->count(), 1);

    view->undoStack()->undo();
    EXPECT_EQ(view->document().node(id)->rect(), QRectF(0, 0, 100, 50));
}

TEST_F(CanvasViewTest, DraggingACornerHandleResizesAndIsUndoable)
{
    const QString id = view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));
    view->selectNode(id, false);

    const QPoint corner(100, 50); // bottom-right, zoom 1, origin (0,0)
    send(view.get(), QEvent::MouseButtonPress, corner, Qt::LeftButton);
    send(view.get(), QEvent::MouseMove, corner + QPoint(40, 20), Qt::LeftButton);
    send(view.get(), QEvent::MouseButtonRelease, corner + QPoint(40, 20), Qt::LeftButton);

    const QRectF resized = view->document().node(id)->rect();
    EXPECT_NEAR(resized.width(), 140.0, 1.0);
    EXPECT_NEAR(resized.height(), 70.0, 1.0);

    view->undoStack()->undo();
    EXPECT_EQ(view->document().node(id)->rect(), QRectF(0, 0, 100, 50));
}

TEST_F(CanvasViewTest, DraggingFromASideHandleConnectsTwoNodesAndIsUndoable)
{
    const QString a = view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));
    const QString b = view->document().addNode(makeTextNode(QRectF(300, 0, 100, 50)));

    send(view.get(), QEvent::MouseMove, QPoint(50, 25), Qt::NoButton); // hovers `a` first
    const QPoint rightHandle(112, 25); // right edge (x=100) + the connect handle's offset
    send(view.get(), QEvent::MouseButtonPress, rightHandle, Qt::LeftButton);
    send(view.get(), QEvent::MouseMove, QPoint(350, 25), Qt::LeftButton);
    send(view.get(), QEvent::MouseButtonRelease, QPoint(350, 25), Qt::LeftButton);

    ASSERT_EQ(view->document().edges().size(), 1);
    const CanvasEdge &edge = view->document().edges().first();
    EXPECT_EQ(edge.fromNode(), a);
    EXPECT_EQ(edge.toNode(), b);
    EXPECT_EQ(edge.fromSide(), QStringLiteral("right"));

    view->undoStack()->undo();
    EXPECT_EQ(view->document().edges().size(), 0);
}

TEST_F(CanvasViewTest, DeletingASelectedNodeCascadesItsEdgesAsOneUndoStep)
{
    const QString a = view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));
    const QString b = view->document().addNode(makeTextNode(QRectF(300, 0, 100, 50)));
    CanvasEdge edge;
    edge.setFromNode(a);
    edge.setToNode(b);
    view->document().addEdge(edge);
    ASSERT_EQ(view->document().edges().size(), 1);

    view->selectNode(a, false);
    view->deleteSelection();

    EXPECT_EQ(view->document().nodes().size(), 1);
    EXPECT_EQ(view->document().edges().size(), 0);
    EXPECT_EQ(view->undoStack()->index(), 1); // one macro, not one command per removed item

    view->undoStack()->undo();
    EXPECT_EQ(view->document().nodes().size(), 2);
    EXPECT_EQ(view->document().edges().size(), 1);
}

TEST_F(CanvasViewTest, DeleteKeyRemovesTheSelection)
{
    const QString id = view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));
    view->selectNode(id, false);
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QCoreApplication::sendEvent(view.get(), &del);
    EXPECT_EQ(view->document().nodes().size(), 0);
}

TEST_F(CanvasViewTest, NodeAtAndEdgeAtFindWhatIsUnderThePoint)
{
    const QString a = view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));
    view->document().addNode(makeTextNode(QRectF(300, 0, 100, 50)));
    CanvasEdge edge;
    edge.setFromNode(a);
    edge.setToNode(view->document().nodes().last().id());
    edge.setFromSide(QStringLiteral("right"));
    edge.setToSide(QStringLiteral("left"));
    const QString edgeID = view->document().addEdge(edge);

    EXPECT_EQ(view->nodeAt(QPoint(50, 25)), a);
    EXPECT_TRUE(view->nodeAt(QPoint(1200, 1200)).isEmpty());
    EXPECT_EQ(view->edgeAt(QPoint(200, 25)), edgeID); // midpoint of a straight left-right curve
}

TEST_F(CanvasViewTest, WheelZoomsAboutThePointerAndPanMovesTheOrigin)
{
    const qreal before = view->zoom();
    QWheelEvent wheel(QPointF(400, 300), QPointF(400, 300), QPoint(0, 0), QPoint(0, 240), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(view.get(), &wheel);
    EXPECT_GT(view->zoom(), before);

    const QPointF originBefore = view->origin();
    send(view.get(), QEvent::MouseButtonPress, QPoint(400, 300), Qt::MiddleButton);
    send(view.get(), QEvent::MouseMove, QPoint(350, 260), Qt::MiddleButton);
    send(view.get(), QEvent::MouseButtonRelease, QPoint(350, 260), Qt::MiddleButton);
    EXPECT_NE(view->origin(), originBefore);
}

TEST_F(CanvasViewTest, DoubleClickingAFileNodeEmitsNoteActivatedWithTheRawRelativePath)
{
    CanvasNode node = makeTextNode(QRectF(0, 0, 100, 50));
    node.setType(CanvasNodeType::File);
    node.setFile(QStringLiteral("Folder/Some Note.md"));
    view->document().addNode(node);

    QSignalSpy spy(view.get(), &CanvasView::noteActivated);
    send(view.get(), QEvent::MouseButtonDblClick, QPoint(50, 25), Qt::LeftButton);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.first().first().toString(), QStringLiteral("Folder/Some Note.md"));
}

TEST_F(CanvasViewTest, EditingMarksTheViewModifiedAndSaveClearsIt)
{
    QSignalSpy spy(view.get(), &CanvasView::modificationChanged);
    EXPECT_FALSE(view->isModified());

    view->createTextNodeAt(QPointF(0, 0));
    EXPECT_TRUE(view->isModified());
    ASSERT_GE(spy.count(), 1);
    EXPECT_TRUE(spy.last().first().toBool());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(view->save(QDir(dir.path()).filePath(QStringLiteral("out.canvas"))));
    EXPECT_FALSE(view->isModified());
}

// --- performance: panning a large canvas ----------------------------------

namespace {

void buildGrid(CanvasView &view, int count)
{
    for (int i = 0; i < count; ++i) {
        const int col = i % 40;
        const int row = i / 40;
        view.document().addNode(makeTextNode(
            QRectF(col * 220.0, row * 140.0, 200, 120),
            QStringLiteral("Node %1\n\nSome body text, so the content cache has real Markdown to lay out.").arg(i)));
    }
}

/// Median wall-clock time of a pan-and-repaint frame into a 1600x1000 image,
/// after one untimed warm-up frame that builds every visible node's content
/// cache - exactly the steady state a real pan gesture runs in.
qint64 medianPanFrameNanos(CanvasView &view, int frames)
{
    view.resize(1600, 1000);
    QImage image(1600, 1000, QImage::Format_ARGB32_Premultiplied);
    QPoint from(800, 500);
    auto panAndRender = [&] {
        const QPoint to = from - QPoint(3, 2);
        send(&view, QEvent::MouseButtonPress, from, Qt::MiddleButton);
        send(&view, QEvent::MouseMove, to, Qt::MiddleButton);
        send(&view, QEvent::MouseButtonRelease, to, Qt::MiddleButton);
        from = to;
        image.fill(Qt::transparent);
        view.render(&image);
    };
    panAndRender();
    QList<qint64> samples;
    samples.reserve(frames);
    for (int i = 0; i < frames; ++i) {
        QElapsedTimer timer;
        timer.start();
        panAndRender();
        samples.append(timer.nsecsElapsed());
    }
    std::sort(samples.begin(), samples.end());
    return samples.at(samples.size() / 2);
}

/// Wall-clock cost of a single *cold* paint - no warm-up, nothing cached yet.
/// This is where a broken cull would actually show up: every node's content
/// pixmap is built lazily, on first paint, only for the ones the cull pass
/// says are visible. A steady-state (post-warm-up) frame cannot tell a lazy
/// cull from an eager one that just happened to run once already, because by
/// then every node's pixmap exists either way.
qint64 firstPaintNanos(CanvasView &view)
{
    view.resize(1600, 1000);
    QImage image(1600, 1000, QImage::Format_ARGB32_Premultiplied);
    QElapsedTimer timer;
    timer.start();
    view.render(&image);
    return timer.nsecsElapsed();
}

} // namespace

TEST(CanvasPerfTest, OnePanFrameOfAThousandNodesStaysUnderBudget)
{
    CanvasView big;
    buildGrid(big, 1000);
    const qint64 medianBig = medianPanFrameNanos(big, 25);
    EXPECT_LT(medianBig, qint64(16) * 1000 * 1000) << "median pan frame: " << medianBig / 1.0e6 << " ms";
}

TEST(CanvasPerfTest, ColdFirstPaintCostTracksVisibleNodesNotTotalNodes)
{
    // A 1 600x1 000 viewport at zoom 1 with 220x140 spacing shows roughly the
    // same ~50-60 nodes regardless of how many thousand more sit off screen -
    // so a *cold* first paint (nothing cached yet) of 1 000 nodes, almost all
    // of them off screen, should cost about the same as one of 60 nodes that
    // mostly fit the viewport already, not ~17x more. Deliberately breaking
    // the cull (isNodeVisible() always returning true) was checked to fail
    // this well before the loose threshold below: it built all 1 000 nodes'
    // Markdown content on that first paint instead of the ~56 actually on
    // screen, which measured at roughly 18x, not under 6x.
    CanvasView big;
    buildGrid(big, 1000);
    const qint64 coldBig = firstPaintNanos(big);

    CanvasView small;
    buildGrid(small, 60);
    const qint64 coldSmall = firstPaintNanos(small);

    EXPECT_LT(double(coldBig), double(coldSmall) * 6.0)
        << "cold first paint: " << coldBig / 1.0e6 << " ms for 1000 nodes vs " << coldSmall / 1.0e6
        << " ms for 60 - looks like every node's content is being built up front instead of lazily";
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
