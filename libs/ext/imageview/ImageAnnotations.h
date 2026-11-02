// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QList>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>

namespace noteAppC {

/// One mark on an image. Every coordinate is normalized to the image (0..1 on
/// both axes), the way PdfAnnotationData's are for a page, so a mark survives
/// the image being shown at any zoom - and a re-saved copy at another size.
struct ImageAnnotation {
    enum class Kind { Highlight, Ink, Note };

    Kind kind = Kind::Highlight;
    QColor color = QColor(0xFF, 0xEB, 0x3B);
    QRectF rect;         ///< Highlight: the marked box
    QPolygonF stroke;    ///< Ink: one freehand polyline
    QPointF point;       ///< Note: where its marker sits
    QString text;        ///< Note: its text; Highlight/Ink: an optional comment

    /// The box the mark covers, for hit-testing and the readable summary.
    QRectF bounds() const;

    friend bool operator==(const ImageAnnotation &a, const ImageAnnotation &b);
};

/// Reads and writes the sidecar note that holds an image's annotations -
/// "<image file name> annotations.md" beside the image (the extension stays
/// in the name so photo.png and photo.pdf do not share one). The EPUB reader's
/// highlights sidecar is the pattern: the note is the source of truth, every
/// edit is written straight back, and everything outside the generated
/// section is the reader's own and stays as written. The image is never
/// opened for writing.
class ImageAnnotations {
public:
    static QString sidecarPath(const QString &imagePath);

    /// The generated section's own markers, public so a test can look for them.
    static const QString &beginMarker();
    static const QString &endMarker();

    /// Every `%% image-annotation ... %%` entry inside the generated section,
    /// in file order, each with the free text written under it.
    static QList<ImageAnnotation> parse(const QString &markdown);

    /// The section's body: the image embed, then one entry per mark - a
    /// readable blockquote line, the hidden marker that carries the geometry,
    /// and the text.
    static QString build(const QList<ImageAnnotation> &annotations, const QString &imageFileName);

    /// `existing` with the generated section replaced by `generated` (markers
    /// included), or appended when there is none.
    static QString merge(const QString &existing, const QString &generated);

    static QList<ImageAnnotation> load(const QString &sidecarPath);

    /// Rewrites the section through Vault::write, so the file keeps its own
    /// CRLF/BOM. No marks and no sidecar yet writes nothing; no marks left
    /// empties an existing section rather than deleting the user's file.
    static bool save(const QString &sidecarPath, const QString &imageFileName,
                     const QList<ImageAnnotation> &annotations, QString *error = nullptr);

    /// The colours a mark may be, in the order they are offered - the PDF
    /// viewer's own palette.
    static const QList<QColor> &palette();
};

} // namespace noteAppC
