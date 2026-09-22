#include "PdfPage.h"

#ifdef NOTEAPPC_HAVE_POPPLER
#include <poppler-qt6.h>
#endif

namespace noteAppC {

bool PdfPage::isAvailable()
{
#ifdef NOTEAPPC_HAVE_POPPLER
    return true;
#else
    return false;
#endif
}

#ifdef NOTEAPPC_HAVE_POPPLER

namespace {

std::unique_ptr<Poppler::Document> open(const QString &path)
{
    auto document = Poppler::Document::load(path);
    if (!document || document->isLocked())
        return nullptr;
    document->setRenderHint(Poppler::Document::Antialiasing, true);
    document->setRenderHint(Poppler::Document::TextAntialiasing, true);
    return document;
}

} // namespace

int PdfPage::pageCount(const QString &path)
{
    const auto document = open(path);
    return document ? document->numPages() : 0;
}

QImage PdfPage::render(const QString &path, int page, int widthPixels)
{
    const auto document = open(path);
    if (!document || page < 0 || page >= document->numPages() || widthPixels <= 0)
        return {};

    const std::unique_ptr<Poppler::Page> pdfPage(document->page(page));
    if (!pdfPage)
        return {};

    // Poppler renders by DPI; 72 is the PDF point, so this is a width ratio.
    const QSizeF size = pdfPage->pageSizeF();
    if (size.width() <= 0)
        return {};
    const double dpi = 72.0 * widthPixels / size.width();
    return pdfPage->renderToImage(dpi, dpi);
}

#else

int PdfPage::pageCount(const QString &)
{
    return 0;
}

QImage PdfPage::render(const QString &, int, int)
{
    return {};
}

#endif

} // namespace noteAppC
