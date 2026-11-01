// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "PdfAnnotationCommands.h"

#include <QCoreApplication>

namespace noteAppC {

namespace {

QString labelFor(PdfAnnotationKind kind)
{
    switch (kind) {
    case PdfAnnotationKind::Highlight:
        return QCoreApplication::translate("PdfView", "Highlight");
    case PdfAnnotationKind::Underline:
        return QCoreApplication::translate("PdfView", "Underline");
    case PdfAnnotationKind::StrikeOut:
        return QCoreApplication::translate("PdfView", "Strike out");
    case PdfAnnotationKind::Ink:
        return QCoreApplication::translate("PdfView", "Draw");
    case PdfAnnotationKind::Note:
        return QCoreApplication::translate("PdfView", "Add note");
    case PdfAnnotationKind::Rectangle:
        return QCoreApplication::translate("PdfView", "Draw box");
    }
    return QCoreApplication::translate("PdfView", "Annotate");
}

} // namespace

AddPdfAnnotationCommand::AddPdfAnnotationCommand(PdfDocument *document, PdfAnnotationData data,
                                                 QUndoCommand *parent)
    : QUndoCommand(labelFor(data.kind), parent)
    , m_document(document)
    , m_data(std::move(data))
{
}

void AddPdfAnnotationCommand::redo()
{
    m_data.id = m_document->addAnnotation(m_data);
}

void AddPdfAnnotationCommand::undo()
{
    m_document->removeAnnotation(m_data.page, m_data.id);
}

RemovePdfAnnotationCommand::RemovePdfAnnotationCommand(PdfDocument *document, PdfAnnotationData data,
                                                       QUndoCommand *parent)
    : QUndoCommand(QCoreApplication::translate("PdfView", "Delete annotation"), parent)
    , m_document(document)
    , m_data(std::move(data))
{
}

void RemovePdfAnnotationCommand::redo()
{
    m_document->removeAnnotation(m_data.page, m_data.id);
}

void RemovePdfAnnotationCommand::undo()
{
    // addAnnotation keeps m_data.id as given rather than assigning a new one
    // (PdfDocument.cpp), so a redo after this restores the same annotation.
    m_document->addAnnotation(m_data);
}

EditPdfAnnotationCommand::EditPdfAnnotationCommand(PdfDocument *document, PdfAnnotationData before,
                                                   PdfAnnotationData after, QUndoCommand *parent)
    : QUndoCommand(QCoreApplication::translate("PdfView", "Edit annotation"), parent)
    , m_document(document)
    , m_before(std::move(before))
    , m_after(std::move(after))
{
}

void EditPdfAnnotationCommand::redo()
{
    m_document->updateAnnotation(m_after);
}

void EditPdfAnnotationCommand::undo()
{
    m_document->updateAnnotation(m_before);
}

} // namespace noteAppC
