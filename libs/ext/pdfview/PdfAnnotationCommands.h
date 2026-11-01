// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QUndoCommand>

#include "PdfDocument.h"

namespace noteAppC {

/// Adds `data` on redo, removes it on undo - what every annotation tool
/// (highlight, ink stroke, note, box...) creates when the reader finishes a
/// gesture. `data.id` is filled in by the first redo() if left empty, so
/// undo/redo always target the same annotation.
class AddPdfAnnotationCommand : public QUndoCommand {
public:
    AddPdfAnnotationCommand(PdfDocument *document, PdfAnnotationData data,
                            QUndoCommand *parent = nullptr);

    void redo() override;
    void undo() override;
    QString annotationID() const { return m_data.id; }

private:
    PdfDocument *m_document;
    PdfAnnotationData m_data;
};

/// The inverse: removes `data` (already on the page) on redo, adds it back
/// on undo. Same id both ways, so a redo after undo restores the identical
/// annotation rather than a new one.
class RemovePdfAnnotationCommand : public QUndoCommand {
public:
    RemovePdfAnnotationCommand(PdfDocument *document, PdfAnnotationData data,
                               QUndoCommand *parent = nullptr);

    void redo() override;
    void undo() override;

private:
    PdfDocument *m_document;
    PdfAnnotationData m_data;
};

/// Move, recolour, reword or restroke an existing annotation: `before` on
/// undo, `after` on redo, both applied through PdfDocument::updateAnnotation
/// (poppler's own live setters - see PdfDocument.cpp - so this never
/// removes and re-adds).
class EditPdfAnnotationCommand : public QUndoCommand {
public:
    EditPdfAnnotationCommand(PdfDocument *document, PdfAnnotationData before, PdfAnnotationData after,
                             QUndoCommand *parent = nullptr);

    void redo() override;
    void undo() override;

private:
    PdfDocument *m_document;
    PdfAnnotationData m_before;
    PdfAnnotationData m_after;
};

} // namespace noteAppC
