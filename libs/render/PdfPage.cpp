#include "PdfPage.h"

#include <QFileInfo>

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

/// One `Poppler::Document` per thread, reopened only when the path or its
/// mtime changes - see the class comment in PdfPage.h. `thread_local` makes
/// this safe without a lock: nothing outside the owning thread ever touches
/// this instance.
Poppler::Document *openCached(const QString &path)
{
    struct Cached {
        QString path;
        qint64 mtime = 0;
        std::unique_ptr<Poppler::Document> document;
    };
    thread_local Cached cache;

    const qint64 mtime = QFileInfo(path).lastModified().toSecsSinceEpoch();
    if (cache.document && cache.path == path && cache.mtime == mtime)
        return cache.document.get();

    cache.document = open(path);
    cache.path = path;
    cache.mtime = mtime;
    return cache.document.get();
}

} // namespace

int PdfPage::pageCount(const QString &path)
{
    Poppler::Document *document = openCached(path);
    return document ? document->numPages() : 0;
}

QSizeF PdfPage::pageSize(const QString &path, int page)
{
    Poppler::Document *document = openCached(path);
    if (!document || page < 0 || page >= document->numPages())
        return {};
    const std::unique_ptr<Poppler::Page> pdfPage(document->page(page));
    return pdfPage ? pdfPage->pageSizeF() : QSizeF();
}

QImage PdfPage::render(const QString &path, int page, int widthPixels)
{
    Poppler::Document *document = openCached(path);
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

QSizeF PdfPage::pageSize(const QString &, int)
{
    return {};
}

QImage PdfPage::render(const QString &, int, int)
{
    return {};
}

#endif

} // namespace noteAppC
