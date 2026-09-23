#pragma once

#include <QHash>
#include <QRectF>
#include <QString>
#include <QUndoCommand>

#include "CanvasDocument.h"

namespace noteAppC {

/// Every command below follows the same shape: capture just enough to redo
/// and undo one mechanical change to a CanvasDocument. A user gesture that
/// touches several nodes or edges at once - deleting a selection, say - is
/// not its own command class; CanvasView pushes several of these inside one
/// QUndoStack::beginMacro()/endMacro() pair instead, which is what makes them
/// undo and redo together as the single step the user actually took.

class AddNodeCommand : public QUndoCommand {
public:
    AddNodeCommand(CanvasDocument *document, CanvasNode node, QUndoCommand *parent = nullptr);
    void undo() override;
    void redo() override;
    QString nodeID() const { return m_node.id(); }

private:
    CanvasDocument *m_document;
    CanvasNode m_node;
    int m_index = -1;
};

class RemoveNodeCommand : public QUndoCommand {
public:
    RemoveNodeCommand(CanvasDocument *document, const QString &id, QUndoCommand *parent = nullptr);
    void undo() override;
    void redo() override;

private:
    CanvasDocument *m_document;
    QString m_id;
    CanvasNode m_saved;
    int m_index = -1;
};

class AddEdgeCommand : public QUndoCommand {
public:
    AddEdgeCommand(CanvasDocument *document, CanvasEdge edge, QUndoCommand *parent = nullptr);
    void undo() override;
    void redo() override;
    QString edgeID() const { return m_edge.id(); }

private:
    CanvasDocument *m_document;
    CanvasEdge m_edge;
    int m_index = -1;
};

class RemoveEdgeCommand : public QUndoCommand {
public:
    RemoveEdgeCommand(CanvasDocument *document, const QString &id, QUndoCommand *parent = nullptr);
    void undo() override;
    void redo() override;

private:
    CanvasDocument *m_document;
    QString m_id;
    CanvasEdge m_saved;
    int m_index = -1;
};

/// A move drags every selected node's rect by the same delta; a resize
/// changes one. Both are "some nodes land at new rects", pushed once per
/// drag - on mouse release, not per mouse-move event - so the whole drag is
/// one undo step.
class MoveResizeNodesCommand : public QUndoCommand {
public:
    MoveResizeNodesCommand(CanvasDocument *document, QHash<QString, QRectF> oldRects,
                           QHash<QString, QRectF> newRects, const QString &text,
                           QUndoCommand *parent = nullptr);
    void undo() override;
    void redo() override;

private:
    CanvasDocument *m_document;
    QHash<QString, QRectF> m_old;
    QHash<QString, QRectF> m_new;
};

class EditTextCommand : public QUndoCommand {
public:
    EditTextCommand(CanvasDocument *document, QString id, QString oldText, QString newText,
                    QUndoCommand *parent = nullptr);
    void undo() override;
    void redo() override;

private:
    CanvasDocument *m_document;
    QString m_id;
    QString m_old;
    QString m_new;
};

} // namespace noteAppC
