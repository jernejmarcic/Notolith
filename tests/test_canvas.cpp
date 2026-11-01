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
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalSpy>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>

#include "CanvasCommands.h"
#include "CanvasDocument.h"
#include "CanvasView.h"
#include "MarkdownEditor.h"
#include "Theme.h"
#include "Perf.h"

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

/// A press, a move and a release: the drag a user makes with the button held.
void drag(QWidget *target, QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    send(target, QEvent::MouseButtonPress, from, Qt::LeftButton, modifiers);
    send(target, QEvent::MouseMove, to, Qt::LeftButton, modifiers);
    send(target, QEvent::MouseButtonRelease, to, Qt::LeftButton, modifiers);
}

void wheel(QWidget *target, QPoint at, int angleDeltaY, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QWheelEvent event(QPointF(at), QPointF(at), QPoint(0, 0), QPoint(0, angleDeltaY), Qt::NoButton, modifiers,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(target, &event);
}

/// Enough paragraphs to run far past a small card.
QString longNote()
{
    QString text;
    for (int i = 0; i < 300; ++i)
        text += QStringLiteral("Paragraph %1 of a long note.\n\n").arg(i);
    return text;
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

// A vault can ship the canvas as a link; saving must not write the link's target.
TEST(CanvasDocumentTest, SavingALinkedCanvasWritesNothingThroughIt)
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(dir.isValid() && outside.isValid());
    const QString real = QDir(outside.path()).filePath(QStringLiteral("real.canvas"));
    QFile seed(real);
    ASSERT_TRUE(seed.open(QIODevice::WriteOnly));
    seed.write(kSpecExample);
    seed.close();
    const QString link = QDir(dir.path()).filePath(QStringLiteral("x.canvas"));
    ASSERT_TRUE(QFile::link(real, link));

    CanvasDocument doc;
    QString error;
    ASSERT_TRUE(doc.load(link, &error)) << error.toStdString();
    EXPECT_FALSE(doc.save(QString(), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(QFileInfo(link).isSymLink());
    QFile check(real);
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

TEST_F(CanvasViewTest, WheelPansCtrlWheelZoomsAndMiddleDragPans)
{
    // Obsidian's canvas: the wheel pans (Shift sideways), Ctrl+wheel zooms.
    const QPointF start = view->origin();
    QWheelEvent pan(QPointF(400, 300), QPointF(400, 300), QPoint(0, 0), QPoint(0, 240), Qt::NoButton,
                    Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(view.get(), &pan);
    EXPECT_LT(view->origin().y(), start.y());
    EXPECT_EQ(view->origin().x(), start.x());
    EXPECT_EQ(view->zoom(), 1.0);

    const QPointF beforeSideways = view->origin();
    QWheelEvent sideways(QPointF(400, 300), QPointF(400, 300), QPoint(0, 0), QPoint(0, 240), Qt::NoButton,
                         Qt::ShiftModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(view.get(), &sideways);
    EXPECT_LT(view->origin().x(), beforeSideways.x());
    EXPECT_EQ(view->origin().y(), beforeSideways.y());

    const qreal before = view->zoom();
    QWheelEvent zoom(QPointF(400, 300), QPointF(400, 300), QPoint(0, 0), QPoint(0, 240), Qt::NoButton,
                     Qt::ControlModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(view.get(), &zoom);
    EXPECT_GT(view->zoom(), before);

    const QPointF originBefore = view->origin();
    send(view.get(), QEvent::MouseButtonPress, QPoint(400, 300), Qt::MiddleButton);
    send(view.get(), QEvent::MouseMove, QPoint(350, 260), Qt::MiddleButton);
    send(view.get(), QEvent::MouseButtonRelease, QPoint(350, 260), Qt::MiddleButton);
    EXPECT_NE(view->origin(), originBefore);
}

TEST_F(CanvasViewTest, APlainDragOnTheEmptyCanvasDrawsASelectionBox)
{
    const QString a = view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));
    const QString farAway = view->document().addNode(makeTextNode(QRectF(2000, 2000, 100, 50)));
    const QPointF origin = view->origin();
    send(view.get(), QEvent::MouseButtonPress, QPoint(-20, -20), Qt::LeftButton);
    send(view.get(), QEvent::MouseMove, QPoint(150, 80), Qt::LeftButton);
    send(view.get(), QEvent::MouseButtonRelease, QPoint(150, 80), Qt::LeftButton);
    EXPECT_EQ(view->selectedNodeIDs(), QStringList{a});
    EXPECT_EQ(view->origin(), origin) << "it selects, it does not pan";
    Q_UNUSED(farAway);
}

TEST_F(CanvasViewTest, MovingAGroupMovesWhatIsInsideItInOneUndoStep)
{
    CanvasNode group = makeTextNode(QRectF(0, 0, 400, 300));
    group.setType(CanvasNodeType::Group);
    const QString frame = view->document().addNode(group);
    const QString inside = view->document().addNode(makeTextNode(QRectF(50, 60, 100, 50)));
    const QString outside = view->document().addNode(makeTextNode(QRectF(600, 60, 100, 50)));

    // Grab the frame by its empty middle, well clear of the card in it.
    send(view.get(), QEvent::MouseButtonPress, QPoint(300, 250), Qt::LeftButton);
    send(view.get(), QEvent::MouseMove, QPoint(340, 270), Qt::LeftButton);
    send(view.get(), QEvent::MouseButtonRelease, QPoint(340, 270), Qt::LeftButton);
    EXPECT_EQ(view->document().node(frame)->rect(), QRectF(40, 20, 400, 300));
    EXPECT_EQ(view->document().node(inside)->rect(), QRectF(90, 80, 100, 50));
    EXPECT_EQ(view->document().node(outside)->rect(), QRectF(600, 60, 100, 50));

    view->undoStack()->undo();
    EXPECT_EQ(view->document().node(frame)->rect(), QRectF(0, 0, 400, 300));
    EXPECT_EQ(view->document().node(inside)->rect(), QRectF(50, 60, 100, 50));
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

// --- snapping: align to other cards while moving or resizing -------------

TEST_F(CanvasViewTest, MovingNearAnotherCardsEdgeSnapsToIt)
{
    view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));
    const QString b = view->document().addNode(makeTextNode(QRectF(200, 104, 100, 50)));

    drag(view.get(), QPoint(250, 130), QPoint(250, 30)); // B's top would land at y=4, 4 px off A's

    EXPECT_EQ(view->document().node(b)->rect(), QRectF(200, 0, 100, 50));
    EXPECT_EQ(view->undoStack()->count(), 1);
}

TEST_F(CanvasViewTest, SnappingAlsoCatchesCentres)
{
    view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));
    const QString b = view->document().addNode(makeTextNode(QRectF(200, 300, 60, 50)));

    drag(view.get(), QPoint(230, 325), QPoint(54, 325)); // B's centre would be 54, A's is 50

    EXPECT_DOUBLE_EQ(view->document().node(b)->rect().center().x(), 50.0);
}

TEST_F(CanvasViewTest, SnapReachIsInScreenPixelsNotCanvasUnits)
{
    view->document().addNode(makeTextNode(QRectF(0, 0, 100, 200)));
    const QString b = view->document().addNode(makeTextNode(QRectF(200, 104, 100, 100)));
    view->setZoom(0.5);
    const QPoint grab = view->mapFromCanvas(QPointF(250, 154));

    drag(view.get(), grab, grab - QPoint(0, 47)); // 94 canvas px up: 10 canvas px (5 on screen) off A's top
    EXPECT_DOUBLE_EQ(view->document().node(b)->rect().top(), 0.0);

    view->undoStack()->undo();
    drag(view.get(), grab, grab - QPoint(0, 42)); // 84 canvas px up: 20 canvas px (10 on screen) off
    EXPECT_DOUBLE_EQ(view->document().node(b)->rect().top(), 20.0);
}

TEST_F(CanvasViewTest, AMultipleSelectionSnapsAsOneBlock)
{
    const QString a = view->document().addNode(makeTextNode(QRectF(100, 0, 100, 50)));
    const QString b = view->document().addNode(makeTextNode(QRectF(260, 0, 100, 50)));
    view->document().addNode(makeTextNode(QRectF(500, 400, 100, 50)));  // the block's left edge lands near this
    view->document().addNode(makeTextNode(QRectF(655, 200, 40, 50)));   // B's own left edge would land near this
    view->selectNode(a, false);
    view->selectNode(b, true);

    drag(view.get(), QPoint(310, 25), QPoint(707, 25)); // 397 right: the block's left edge is 3 short of 500

    EXPECT_DOUBLE_EQ(view->document().node(a)->rect().left(), 500.0);
    EXPECT_DOUBLE_EQ(view->document().node(b)->rect().left(), 660.0); // moved by the same 400
    EXPECT_EQ(view->undoStack()->count(), 1);
}

TEST_F(CanvasViewTest, ResizingSnapsTheDraggedEdge)
{
    view->document().addNode(makeTextNode(QRectF(0, 0, 300, 50)));
    const QString b = view->document().addNode(makeTextNode(QRectF(250, 104, 150, 50)));
    view->selectNode(b, false);

    drag(view.get(), QPoint(400, 129), QPoint(297, 129)); // the right handle, 3 short of A's right edge

    EXPECT_EQ(view->document().node(b)->rect(), QRectF(250, 104, 50, 50));
}

TEST_F(CanvasViewTest, SnapToGridRoundsToTheDotGrid)
{
    view->setSnapToGrid(true);
    view->setSnapToObjects(false);
    const QString id = view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));

    drag(view.get(), QPoint(50, 25), QPoint(87, 70));

    EXPECT_EQ(view->document().node(id)->rect().topLeft(), QPointF(32, 32));
}

TEST_F(CanvasViewTest, ASnapToACardBeatsTheGridWhenBothHit)
{
    view->setSnapToGrid(true);
    view->document().addNode(makeTextNode(QRectF(35, 400, 100, 50)));
    const QString b = view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));

    drag(view.get(), QPoint(50, 25), QPoint(87, 25)); // left edge at 37: 35 is a card, 32 the grid

    EXPECT_DOUBLE_EQ(view->document().node(b)->rect().left(), 35.0);
}

TEST_F(CanvasViewTest, WithSnappingOffACardMovesExactly)
{
    view->setSnapToObjects(false);
    view->setSnapToGrid(false);
    view->document().addNode(makeTextNode(QRectF(0, 0, 100, 50)));
    const QString b = view->document().addNode(makeTextNode(QRectF(200, 104, 100, 50)));

    drag(view.get(), QPoint(250, 130), QPoint(250, 30));

    EXPECT_DOUBLE_EQ(view->document().node(b)->rect().top(), 4.0);
}

TEST_F(CanvasViewTest, GuideLinesAreDrawnWhileSnappedAndGoneAfterRelease)
{
    view->document().addNode(makeTextNode(QRectF(0, 100, 100, 50)));
    view->document().addNode(makeTextNode(QRectF(200, 204, 100, 50)));
    const QColor accent = view->theme().color(QStringLiteral("--interactive-accent"));
    // Along the line the two cards share, between them: nothing else is drawn there.
    const auto guideDrawn = [&] {
        const QImage frame = view->grab().toImage();
        for (int x = 120; x <= 180; ++x) {
            const QColor pixel = frame.pixelColor(x, 100);
            if (std::abs(pixel.red() - accent.red()) < 8 && std::abs(pixel.green() - accent.green()) < 8
                && std::abs(pixel.blue() - accent.blue()) < 8)
                return true;
        }
        return false;
    };
    EXPECT_FALSE(guideDrawn());

    send(view.get(), QEvent::MouseButtonPress, QPoint(250, 230), Qt::LeftButton);
    send(view.get(), QEvent::MouseMove, QPoint(250, 130), Qt::LeftButton); // the top lands 4 off, then snaps
    EXPECT_TRUE(guideDrawn());

    send(view.get(), QEvent::MouseButtonRelease, QPoint(250, 130), Qt::LeftButton);
    EXPECT_FALSE(guideDrawn());
}

// --- wheel over a selected card ------------------------------------------

TEST_F(CanvasViewTest, WheelOverASelectedOverflowingCardScrollsItNotTheCanvas)
{
    const QString id = view->document().addNode(makeTextNode(QRectF(0, 0, 200, 100), longNote()));
    view->selectNode(id, false);
    const QPointF origin = view->origin();
    const QImage before = view->grab().toImage();

    wheel(view.get(), QPoint(100, 50), -240);

    EXPECT_EQ(view->origin(), origin);
    EXPECT_GT(view->cardScrollOffset(), 0.0);
    EXPECT_NE(view->grab().toImage(), before) << "the card's content moved";
}

TEST_F(CanvasViewTest, WheelOverAnUnselectedCardPansTheCanvas)
{
    view->document().addNode(makeTextNode(QRectF(0, 0, 200, 100), longNote()));
    const QPointF origin = view->origin();

    wheel(view.get(), QPoint(100, 50), -240);

    EXPECT_GT(view->origin().y(), origin.y());
    EXPECT_EQ(view->cardScrollOffset(), 0.0);
}

TEST_F(CanvasViewTest, WheelOverASelectedCardThatFitsPans)
{
    const QString id = view->document().addNode(makeTextNode(QRectF(0, 0, 200, 100), QStringLiteral("short")));
    view->selectNode(id, false);
    const QPointF origin = view->origin();

    wheel(view.get(), QPoint(100, 50), -240);

    EXPECT_GT(view->origin().y(), origin.y());
    EXPECT_EQ(view->cardScrollOffset(), 0.0);
}

TEST_F(CanvasViewTest, CtrlWheelOverASelectedCardStillZooms)
{
    const QString id = view->document().addNode(makeTextNode(QRectF(0, 0, 200, 100), longNote()));
    view->selectNode(id, false);
    const qreal zoom = view->zoom();

    wheel(view.get(), QPoint(100, 50), 240, Qt::ControlModifier);

    EXPECT_GT(view->zoom(), zoom);
    EXPECT_EQ(view->cardScrollOffset(), 0.0);
}

TEST_F(CanvasViewTest, CardScrollIsClampedAndResetsOnReselect)
{
    const QString id = view->document().addNode(makeTextNode(QRectF(0, 0, 200, 100), longNote()));
    view->selectNode(id, false);
    const QImage top = view->grab().toImage();

    for (int i = 0; i < 400; ++i) {
        wheel(view.get(), QPoint(100, 50), -240);
        (void)view->grab();
    }
    const qreal bottom = view->cardScrollMaximum();
    ASSERT_GT(bottom, 0.0);
    EXPECT_DOUBLE_EQ(view->cardScrollOffset(), bottom); // stops at the end of the note

    wheel(view.get(), QPoint(100, 50), 240); // one tick back: moves at once, nothing was stored past the end
    (void)view->grab();
    EXPECT_LT(view->cardScrollOffset(), bottom);

    view->clearSelection();
    EXPECT_EQ(view->cardScrollOffset(), 0.0);
    view->selectNode(id, false);
    EXPECT_EQ(view->grab().toImage(), top) << "a card selected again starts from its top";
}

TEST_F(CanvasViewTest, TheInlineEditorKeepsTheWheelWhileEditing)
{
    const QString id = view->document().addNode(makeTextNode(QRectF(0, 0, 200, 100), longNote()));
    ASSERT_TRUE(view->beginEditingText(id));
    auto *editor = view->findChild<MarkdownEditor *>(QStringLiteral("canvasTextEditor"));
    ASSERT_NE(editor, nullptr);
    (void)editor->grab(); // lays it out, which sets the scroll range
    const QPointF origin = view->origin();

    wheel(editor->viewport(), QPoint(100, 50), -240);

    EXPECT_GT(editor->verticalScrollBar()->value(), 0);
    EXPECT_EQ(view->origin(), origin);
}

TEST(CanvasRenderTest, AFileCardsSubpathShowsThatSectionOnly)
{
    const QString note = QStringLiteral("# Top\nintro\n## Plan\nstep one\n```\n# not a heading\n```\n"
                                        "### Detail\nmore\n## Next\nlater\nA line ^abc\n");
    EXPECT_EQ(canvasSectionOf(note, QString()), note);
    EXPECT_EQ(canvasSectionOf(note, QStringLiteral("#Plan")),
              QStringLiteral("## Plan\nstep one\n```\n# not a heading\n```\n### Detail\nmore"));
    EXPECT_EQ(canvasSectionOf(note, QStringLiteral("#Top#Next")), QStringLiteral("## Next\nlater\nA line ^abc\n"));
    EXPECT_EQ(canvasSectionOf(note, QStringLiteral("#^abc")), QStringLiteral("A line"));
    EXPECT_EQ(canvasSectionOf(note, QStringLiteral("#Missing")), note);
}

TEST(CanvasRenderTest, AnHTMLHeadingIsASectionToo)
{
    const QString note = QStringLiteral("# Top\nintro\n<h2>Plan &amp; more</h2>\nstep\n<h3>Detail</h3>\nx\n## Next\nlater\n");
    EXPECT_EQ(canvasSectionOf(note, QStringLiteral("#Plan & more")),
              QStringLiteral("<h2>Plan &amp; more</h2>\nstep\n<h3>Detail</h3>\nx"));
    EXPECT_EQ(canvasSectionOf(note, QStringLiteral("#Detail")), QStringLiteral("<h3>Detail</h3>\nx"));
}

TEST(CanvasRenderTest, AStraightVerticalEdgeIsDrawn)
{
    CanvasView view;
    view.resize(400, 600);
    const QString top = view.document().addNode(makeTextNode(QRectF(100, 0, 200, 100)));
    const QString bottom = view.document().addNode(makeTextNode(QRectF(100, 400, 200, 100)));
    QJsonObject edge;
    edge.insert(QStringLiteral("id"), QStringLiteral("e"));
    edge.insert(QStringLiteral("fromNode"), top);
    edge.insert(QStringLiteral("fromSide"), QStringLiteral("bottom"));
    edge.insert(QStringLiteral("toNode"), bottom);
    edge.insert(QStringLiteral("toSide"), QStringLiteral("top"));
    view.document().addEdge(CanvasEdge(edge));

    const QImage frame = view.grab().toImage();
    // Down the middle of the gap, where only the edge can be.
    int drawn = 0;
    const QRgb background = frame.pixel(5, 250);
    for (int x = 195; x <= 205; ++x)
        drawn += frame.pixel(x, 250) != background;
    EXPECT_GT(drawn, 0);
}

TEST(CanvasRenderTest, ABigTextNodeParsesWithoutAnEventLoopInsideThePaint)
{
    // paintEvent must not dispatch timers or deferred deletes: Ctrl+W landing
    // in a nested loop would free the view while it is still painting.
    QString text = QStringLiteral("# Big\n\n");
    while (text.size() < 200 * 1024)
        text += QStringLiteral("A paragraph of words to make the node big enough.\n\n");
    CanvasView view;
    view.resize(400, 300);
    view.document().addNode(makeTextNode(QRectF(20, 20, 300, 200), text));

    bool fired = false;
    QTimer::singleShot(0, &view, [&fired] { fired = true; });
    const QImage frame = view.grab().toImage();
    EXPECT_FALSE(frame.isNull());
    EXPECT_FALSE(fired);
}

TEST(CanvasFitTest, OpeningACanvasShowsAllOfIt)
{
    // Far from the origin, as canvases usually are: Obsidian opens one with
    // every node in view.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("far.canvas"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(R"({"nodes":[)"
               R"({"id":"a","type":"text","text":"a","x":3000,"y":2000,"width":400,"height":300},)"
               R"({"id":"b","type":"text","text":"b","x":5000,"y":2600,"width":400,"height":300}]})");
    file.close();

    CanvasView view;
    view.resize(800, 600);
    ASSERT_TRUE(view.load(path));
    (void)view.grab(); // the first paint frames it
    const QRect viewport = view.rect();
    for (const CanvasNode &node : view.document().nodes()) {
        const QRectF shown(view.mapFromCanvasF(node.rect().topLeft()),
                           view.mapFromCanvasF(node.rect().bottomRight()));
        EXPECT_TRUE(QRectF(viewport).contains(shown)) << node.id().toStdString();
    }
    EXPECT_LE(view.zoom(), 1.0);

    // Loaded while still small (a tab being laid out), it frames at the size
    // it has when it is first painted, not the one it was loaded at.
    CanvasView later;
    later.resize(100, 30);
    ASSERT_TRUE(later.load(path));
    later.resize(800, 600);
    (void)later.grab();
    const QRectF b(later.mapFromCanvasF(QPointF(5000, 2600)), later.mapFromCanvasF(QPointF(5400, 2900)));
    EXPECT_TRUE(QRectF(later.rect()).contains(b));
}

// --- performance: panning a large canvas ----------------------------------

namespace {

void buildGrid(CanvasView &view, int count, int columns = 40)
{
    for (int i = 0; i < count; ++i) {
        const int col = i % columns;
        const int row = i / columns;
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
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    CanvasView big;
    buildGrid(big, 1000);
    // Best of three medians: load only ever adds time.
    const qint64 medianBig = perf::bestOf([&] { return medianPanFrameNanos(big, 25); }, 3,
                                          double(qint64(16) * 1000 * 1000));
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
    // screen, which measured at roughly 16x, not under 6x.
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    // A cold paint needs a fresh view each time; best of three for each side,
    // because load only ever adds time to either.
    // The small canvas is 8 columns wide so all 60 really are on screen; at
    // the default 40 only 16 of them were, which left the honest ratio near
    // 4x and the guard at the mercy of one noisy run.
    const auto cold = [](int nodes, int columns) {
        return perf::bestOf([nodes, columns] {
            CanvasView view;
            buildGrid(view, nodes, columns);
            return firstPaintNanos(view);
        });
    };
    const qint64 coldBig = cold(1000, 40);
    const qint64 coldSmall = cold(60, 8);

    EXPECT_LT(double(coldBig), double(coldSmall) * 6.0)
        << "cold first paint: " << coldBig / 1.0e6 << " ms for 1000 nodes vs " << coldSmall / 1.0e6
        << " ms for 60 - looks like every node's content is being built up front instead of lazily";
}

TEST(CanvasPerfTest, SnapCostPerMouseMoveDoesNotGrowWithTheCanvas)
{
    // Every card is a snap target, but they are sorted once per drag and each
    // move binary-searches them: 200 times more cards must cost nowhere near
    // 200 times more per move. Searching them all (or rebuilding the lists)
    // on every move was checked to cost about 200x here; the log-sized
    // search measures a few x.
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    const auto perMove = [](int cards, int columns) {
        CanvasView view;
        view.resize(1600, 1000);
        buildGrid(view, cards, columns);
        send(&view, QEvent::MouseButtonPress, QPoint(100, 60), Qt::LeftButton);
        send(&view, QEvent::MouseMove, QPoint(110, 70), Qt::LeftButton); // the first move builds the lists
        return perf::bestOf([&] {
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < 200; ++i)
                send(&view, QEvent::MouseMove, QPoint(110 + i % 40, 70 + i % 30), Qt::LeftButton);
            return timer.nsecsElapsed() / 200;
        });
    };
    const qint64 small = perMove(100, 10);
    const qint64 big = perMove(20000, 200);
    EXPECT_LT(double(big), double(small) * 20.0)
        << "per mouse move: " << big << " ns with 20 000 cards vs " << small << " ns with 100";
}

TEST(CanvasPerfTest, ScrollingASelectedCardCostsAGrabNotARelayout)
{
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    CanvasView view;
    view.resize(800, 600);
    const QString id = view.document().addNode(makeTextNode(QRectF(0, 0, 200, 100), longNote()));
    view.selectNode(id, false);
    wheel(&view, QPoint(100, 50), -120); // the first tick builds the card's scroll editor
    (void)view.grab();
    ASSERT_EQ(view.cardScrollEditorBuilds(), 1u);

    const qint64 perTick = perf::bestOf([&] {
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 20; ++i) {
            wheel(&view, QPoint(100, 50), -120);
            (void)view.grab();
        }
        return timer.nsecsElapsed() / 20;
    }, 3, 10.0e6);
    EXPECT_EQ(view.cardScrollEditorBuilds(), 1u) << "later ticks reuse the editor";
    EXPECT_LT(perTick, qint64(10) * 1000 * 1000) << "per tick: " << perTick / 1.0e6 << " ms";
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
