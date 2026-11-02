// SPDX-License-Identifier: GPL-3.0-or-later
#include "CanvasCommands.h"

#include <QCoreApplication>

namespace noteAppC {

AddNodeCommand::AddNodeCommand(CanvasDocument *document, CanvasNode node, QUndoCommand *parent)
    : QUndoCommand(QCoreApplication::translate("Canvas", "Add node"), parent)
    , m_document(document)
    , m_node(std::move(node))
{
}

void AddNodeCommand::redo()
{
    if (m_index < 0) {
        const QString id = m_document->addNode(m_node);
        m_node.setID(id);
        m_index = m_document->nodeIndex(id);
    } else {
        m_document->insertNode(m_index, m_node);
    }
}

void AddNodeCommand::undo()
{
    m_index = m_document->nodeIndex(m_node.id());
    m_document->removeNode(m_node.id());
}

RemoveNodeCommand::RemoveNodeCommand(CanvasDocument *document, const QString &id, QUndoCommand *parent)
    : QUndoCommand(QCoreApplication::translate("Canvas", "Remove node"), parent)
    , m_document(document)
    , m_id(id)
{
}

void RemoveNodeCommand::redo()
{
    m_index = m_document->nodeIndex(m_id);
    if (const CanvasNode *node = m_document->node(m_id))
        m_saved = *node;
    m_document->removeNode(m_id);
}

void RemoveNodeCommand::undo()
{
    m_document->insertNode(m_index, m_saved);
}

AddEdgeCommand::AddEdgeCommand(CanvasDocument *document, CanvasEdge edge, QUndoCommand *parent)
    : QUndoCommand(QCoreApplication::translate("Canvas", "Connect nodes"), parent)
    , m_document(document)
    , m_edge(std::move(edge))
{
}

void AddEdgeCommand::redo()
{
    if (m_index < 0) {
        const QString id = m_document->addEdge(m_edge);
        m_edge.setID(id);
        m_index = m_document->edgeIndex(id);
    } else {
        m_document->insertEdge(m_index, m_edge);
    }
}

void AddEdgeCommand::undo()
{
    m_index = m_document->edgeIndex(m_edge.id());
    m_document->removeEdge(m_edge.id());
}

RemoveEdgeCommand::RemoveEdgeCommand(CanvasDocument *document, const QString &id, QUndoCommand *parent)
    : QUndoCommand(QCoreApplication::translate("Canvas", "Disconnect nodes"), parent)
    , m_document(document)
    , m_id(id)
{
}

void RemoveEdgeCommand::redo()
{
    m_index = m_document->edgeIndex(m_id);
    if (const CanvasEdge *edge = m_document->edge(m_id))
        m_saved = *edge;
    m_document->removeEdge(m_id);
}

void RemoveEdgeCommand::undo()
{
    m_document->insertEdge(m_index, m_saved);
}

MoveResizeNodesCommand::MoveResizeNodesCommand(CanvasDocument *document, QHash<QString, QRectF> oldRects,
                                               QHash<QString, QRectF> newRects, const QString &text,
                                               QUndoCommand *parent)
    : QUndoCommand(text, parent)
    , m_document(document)
    , m_old(std::move(oldRects))
    , m_new(std::move(newRects))
{
}

void MoveResizeNodesCommand::redo()
{
    for (auto it = m_new.cbegin(); it != m_new.cend(); ++it)
        m_document->setNodeRect(it.key(), it.value());
}

void MoveResizeNodesCommand::undo()
{
    for (auto it = m_old.cbegin(); it != m_old.cend(); ++it)
        m_document->setNodeRect(it.key(), it.value());
}

EditTextCommand::EditTextCommand(CanvasDocument *document, QString id, QString oldText, QString newText,
                                 QUndoCommand *parent)
    : QUndoCommand(QCoreApplication::translate("Canvas", "Edit text"), parent)
    , m_document(document)
    , m_id(std::move(id))
    , m_old(std::move(oldText))
    , m_new(std::move(newText))
{
}

void EditTextCommand::redo()
{
    m_document->setNodeText(m_id, m_new);
}

void EditTextCommand::undo()
{
    m_document->setNodeText(m_id, m_old);
}

} // namespace noteAppC
