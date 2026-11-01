// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QString>

namespace noteAppC {

class PdfDocument;

/// The generated section's own markers - HTML comments, invisible in
/// rendered Markdown, the same idiom callouts and embeds already rely on to
/// stay inert as plain text until something chooses to render them. Public
/// so a test can look for them without duplicating the literal strings.
extern const QString kPdfAnnotationsSectionBegin;
extern const QString kPdfAnnotationsSectionEnd;

/// Builds the generated section's body (no marker comments) from `document`:
/// one `## [[pdfFileName#page=N]]` heading per page that has at least one
/// highlight/underline/strike-out or sticky note, each markup quoted with
/// its underlying page text (read back through PdfDocument::text over the
/// annotation's own quads - so the wording always matches what is actually
/// marked, not whatever was selected when the tool ran), each note's
/// comment as a plain paragraph. Ink strokes and boxes carry no text of
/// their own and are not extracted. Pages are visited in document order;
/// `pdfFileName` is the link target, normally just the PDF's file name.
QString buildAnnotationsMarkdown(PdfDocument &document, const QString &pdfFileName);

/// Replaces the text between the begin/end markers in `existingText` with
/// `generated`, so a re-run keeps whatever the user wrote around the
/// generated section. No existing markers (including an empty
/// `existingText`, the brand-new-file case) appends a fresh marked section
/// at the end instead of touching anything.
QString mergeAnnotationsMarkdown(const QString &existingText, const QString &generated);

/// Where the command writes absent any vault attachment-folder override:
/// right beside the PDF, `<pdf base name> annotations.md`. Resolving the
/// vault's actual attachment-folder rule needs Settings/Vault, which this
/// module does not own - the caller passes that resolved path in explicitly instead of this default
/// when the vault has one configured.
QString defaultAnnotationsNotePath(const QString &pdfPath);

/// pdf:extract-annotations end to end: builds the section, reads whatever
/// markdown already exists at `notePath` (none is not an error - a brand
/// new note is created), merges, and writes it back through Vault::write,
/// keeping that file's own line-ending/BOM convention if it already existed.
/// False, with `error` filled, on any write failure.
bool extractAnnotationsToNote(PdfDocument &document, const QString &pdfFileName, const QString &notePath,
                              QString *error = nullptr);

} // namespace noteAppC
