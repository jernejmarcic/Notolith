#pragma once

#include <QImage>
#include <QString>

namespace noteAppC {

/// One page of a PDF, rendered to an image through poppler-qt6.
///
/// Kept behind this small surface so poppler's headers - and its GPL, which
/// is why the CMake component is optional - stay out of the editor.
class PdfPage {
public:
    /// 0 when the file is not a PDF poppler can open.
    static int pageCount(const QString &path);

    /// Renders `page` (0-based) so the result is `widthPixels` wide. A null
    /// image means the file, or that page, could not be rendered.
    static QImage render(const QString &path, int page, int widthPixels);

    /// False in a build configured without poppler.
    static bool isAvailable();
};

} // namespace noteAppC
