// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QPolygonF>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <memory>
#include <optional>

namespace Poppler {
class Document;
}

namespace noteAppC {

/// The kinds of markup PdfView's tools produce. Highlight/Underline/StrikeOut
/// all become one poppler class (HighlightAnnotation, three highlightType()
/// values) - see PdfDocument.cpp.
enum class PdfAnnotationKind { Highlight, Underline, StrikeOut, Ink, Note, Rectangle };

/// One annotation, independent of poppler's own classes so PdfView and the
/// undo commands never touch poppler-qt6.h. Every geometry field is
/// normalized to the page - 0..1 over its width/height, y growing downward -
/// which is what Poppler::Annotation itself uses (confirmed by rendering a
/// round trip while this module was built: a rect equal to a word's own
/// point-space box divided by the page's point size lands exactly on that
/// word, no y-flip needed).
struct PdfAnnotationData {
    QString id; ///< assigned by PdfDocument::addAnnotation if left empty
    PdfAnnotationKind kind = PdfAnnotationKind::Highlight;
    int page = 0; ///< 0-based
    QRectF boundary; ///< normalized; the annotation's bounding box
    QList<QRectF> quads; ///< Highlight/Underline/StrikeOut: one rect per marked line
    QList<QPolygonF> strokes; ///< Ink: one polyline per pen stroke, normalized points
    QColor color = QColor(0xFF, 0xEB, 0x3B);
    double lineWidthPoints = 2.0; ///< Ink stroke width and Rectangle border width, in PDF points
    QString contents; ///< note text, or a highlight's comment
    QDateTime modified;

    bool isTextMarkup() const
    {
        return kind == PdfAnnotationKind::Highlight || kind == PdfAnnotationKind::Underline
            || kind == PdfAnnotationKind::StrikeOut;
    }
};

/// One entry of a PDF's outline (table of contents), recursive.
struct PdfOutlineItem {
    QString title;
    int page = -1; ///< 0-based; -1 when the destination did not resolve
    double top = 0; ///< normalized scroll position within that page
    QList<PdfOutlineItem> children;
};

/// One link on a page, resolved enough to act on without touching poppler.
struct PdfLinkInfo {
    QRectF area; ///< normalized, on the page it was read from
    bool internal = false;
    int targetPage = -1; ///< internal: 0-based
    double targetTop = 0;
    QString url; ///< external
};

/// A word-level text box, for drag-selection hit testing and for building a
/// selection's highlight regions.
struct PdfWordBox {
    QRectF rect; ///< PDF points, page-local
    QString text;
    bool spaceAfter = false;
};

/// The PDF a PdfView tab holds open. Poppler's `Document` is kept alive for
/// the life of the tab rather than reopened per call, because annotation
/// edits are live mutations of it: add/update/remove change the actual
/// in-memory PDF tree, so a later read (even mid-session, before any save)
/// sees them - no parallel model to keep in sync, no re-baking on every
/// keystroke of a note's text.
///
/// Not thread-safe, and not meant to be used from one: every call here runs
/// on the UI thread, same as the rest of the interactive edit. Rendering
/// pixels is a different, read-only concern and goes through PdfPageImpl on a
/// worker thread instead, against the file on disk - see PdfView.cpp for how
/// the two stay visually consistent (PdfPageImpl's hideAnnotations flag).
class PdfDocument {
public:
    PdfDocument();
    ~PdfDocument();
    PdfDocument(const PdfDocument &) = delete;
    PdfDocument &operator=(const PdfDocument &) = delete;

    bool load(const QString &path, QString *error = nullptr);
    bool isOpen() const { return m_doc != nullptr; }
    QString path() const { return m_path; }
    /// The file was renamed or moved: save() writes to `path` from now on (the document is in memory).
    void setPath(const QString &path) { m_path = path; }

    int pageCount() const;
    /// In PDF points (1/72"); null for an out-of-range page.
    QSizeF pageSizePoints(int page) const;

    /// Text under `rectPoints` (page-local points); the whole page when the
    /// rect is null.
    QString text(int page, const QRectF &rectPoints = QRectF()) const;
    /// Word-level boxes in reading order, poppler's own segmentation.
    QList<PdfWordBox> words(int page) const;
    /// Every match of `needle` on the page, as point-space rects.
    QList<QRectF> findOnPage(int page, const QString &needle) const;

    /// One match, as (0-based page, point-space rect on it), page by page in
    /// order. Static and against the file on disk through its own
    /// thread-local Document cache (mirrors PdfPageImpl.cpp's) rather than the
    /// live, possibly mid-edit m_doc - safe to call from a worker thread,
    /// which is the point: PdfView runs this off the UI thread so Ctrl+F on
    /// a big PDF never blocks scrolling. Annotating never changes a page's
    /// text, so a copy that is one edit stale is harmless here.
    static QList<QPair<int, QRectF>> findAcrossDocument(const QString &path, const QString &needle,
                                                         int pageCount);

    QList<PdfOutlineItem> outline() const;
    QList<PdfLinkInfo> links(int page) const;

    // --- annotations: read and mutate the live document -------------------

    QList<PdfAnnotationData> annotations(int page) const;

    /// Adds a new annotation - `data.id` is assigned if left empty - and
    /// marks the document modified. Returns the id.
    QString addAnnotation(PdfAnnotationData data);
    /// Replaces the annotation `data.id` on `data.page` with `data` in
    /// place (poppler's setters mutate the live annotation, no
    /// remove-then-add needed). False when no such annotation exists.
    bool updateAnnotation(const PdfAnnotationData &data);
    bool removeAnnotation(int page, const QString &id);

    bool isModified() const { return m_modified; }

    /// Writes through PdfRecovery (the previous bytes, kept outside the
    /// vault) and then a QSaveFile + poppler's PDFConverter with
    /// WithChanges (temp file, atomic commit()). False, with `error`
    /// filled, on any failure; the file on disk is untouched either way.
    bool save(QString *error = nullptr);

private:
    QString m_path;
    std::unique_ptr<Poppler::Document> m_doc;
    bool m_modified = false;
};

} // namespace noteAppC
