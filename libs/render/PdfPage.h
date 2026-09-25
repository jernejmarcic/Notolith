#pragma once

#include <QImage>
#include <QSizeF>
#include <QString>

namespace noteAppC {

/// One page of a PDF, rendered to an image through poppler-qt6.
///
/// Kept behind this small surface so poppler's headers - and its GPL, which
/// is why the CMake component is optional - stay out of the editor.
///
/// Every function here is safe to call from a worker thread: `Poppler::
/// Document` is not itself thread-safe to share, so each calling thread gets
/// its own instance, kept (and reopened only when the file's mtime moves) in
/// thread-local storage rather than reopened on every call - a burst of page
/// requests for the same document, which a column or grid layout produces,
/// pays to open the file once per thread rather than once per page.
///
/// One thing separate Documents do not cover: poppler itself keeps a couple
/// of color-management globals (`GfxState::sRGBProfile`,
/// `GfxXYZ2DisplayTransforms::XYZProfile`) that are process-wide and filled
/// in, unguarded, by whichever thread's render reaches them first. `render`
/// forces that first fill-in to happen alone, before any concurrent render
/// can race it - see the comment on `warmUpColorManagement` in the .cpp.
class PdfPage {
public:
    /// 0 when the file is not a PDF poppler can open.
    static int pageCount(const QString &path);

    /// A page's size in PDF points (72 per inch) - no rasterising, so this
    /// is cheap enough to call while sizing a placeholder box before the
    /// real render (20-50 ms/page) lands off the UI thread. Null when the
    /// file, or that page, cannot be read.
    static QSizeF pageSize(const QString &path, int page);

    /// Renders `page` (0-based) so the result is `widthPixels` wide. A null
    /// image means the file, or that page, could not be rendered. Meant to
    /// be called from a worker thread - see the class comment.
    ///
    /// `hideAnnotations` skips poppler's own baked-in annotation drawing.
    /// Only libs/ext/pdfview passes true: it draws every annotation itself,
    /// live off PdfDocument rather than off this render's on-disk snapshot,
    /// so an edit shows immediately and a deleted annotation does not leave
    /// a stale mark baked into the cached raster underneath the overlay.
    /// Every other caller (PDF embeds) keeps the default and gets poppler's
    /// own rendering, unchanged.
    static QImage render(const QString &path, int page, int widthPixels, bool hideAnnotations = false);

    /// False in a build configured without poppler.
    static bool isAvailable();
};

} // namespace noteAppC
