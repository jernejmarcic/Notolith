#include "PdfPageImpl.h"

#include "PdfPageFace.h"
#include "ModuleABI.h"

#include <QFileInfo>

#include <poppler-qt6.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace noteAppC {

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
/// mtime changes - see PdfPage.h. `thread_local` makes
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

/// Poppler's own GfxState.cc lazily fills in two process-global statics the
/// first time any page is rendered: `GfxState::sRGBProfile` and
/// `GfxXYZ2DisplayTransforms::XYZProfile` (both `std::shared_ptr`s wrapping
/// an lcms2 profile), each behind an unguarded `if (!profile) profile =
/// ...;`. Every render builds a `GfxState` (`Page::displaySlice` -> `Gfx` ->
/// `GfxState`), so this is not a per-Document detail - it sits underneath
/// every `Poppler::Document`, in every thread, in the whole process. A
/// `Poppler::Document` is fine one-per-thread as poppler's own docs say, but
/// that does not cover this: the first renders across several worker
/// threads (this file's own `openCached` guarantees separate Documents, not
/// separate processes) can race on writing the same shared_ptr, tearing its
/// internal pointer and crashing later on a garbage address deep in
/// `GfxXYZ2DisplayTransforms` - see TODO.md's "test_demo crashed once in
/// poppler". One tiny render runs, alone, the first time any thread
/// reaches here; every later render, on any thread, concurrent or
/// not, then finds both statics already set and never written to again, so
/// reading them is safe without any lock of our own.
void PdfPageImpl::warmUp(Poppler::Document *document)
{
    // Latched only once a page has really been drawn: a document with no
    // page to draw leaves it for the next one.
    static std::atomic<bool> warmed = false;
    static std::mutex lock;
    if (warmed.load(std::memory_order_acquire) || !document)
        return;
    const std::lock_guard<std::mutex> hold(lock);
    if (warmed.load(std::memory_order_relaxed))
        return;
    const std::unique_ptr<Poppler::Page> page(document->page(0));
    if (!page)
        return;
    page->renderToImage(1, 1, 0, 0, 1, 1);
    warmed.store(true, std::memory_order_release);
}

int PdfPageImpl::pageCount(const QString &path)
{
    Poppler::Document *document = openCached(path);
    return document ? document->numPages() : 0;
}

QSizeF PdfPageImpl::pageSize(const QString &path, int page)
{
    Poppler::Document *document = openCached(path);
    if (!document || page < 0 || page >= document->numPages())
        return {};
    const std::unique_ptr<Poppler::Page> pdfPage(document->page(page));
    return pdfPage ? pdfPage->pageSizeF() : QSizeF();
}

QImage PdfPageImpl::render(const QString &path, int page, int widthPixels, bool hideAnnotations)
{
    Poppler::Document *document = openCached(path);
    if (!document || page < 0 || page >= document->numPages() || widthPixels <= 0)
        return {};

    warmUp(document);

    const std::unique_ptr<Poppler::Page> pdfPage(document->page(page));
    if (!pdfPage)
        return {};

    // The hint lives on the (thread-local, cached) Document, shared by every
    // caller on this thread - toggle it around this one render and restore
    // it before returning, so a PDF-embed render on the same thread right
    // after never inherits it.
    if (hideAnnotations)
        document->setRenderHint(Poppler::Document::HideAnnotations, true);

    // Poppler renders by DPI; 72 is the PDF point, so this is a width ratio.
    const QSizeF size = pdfPage->pageSizeF();
    if (size.width() <= 0) {
        if (hideAnnotations)
            document->setRenderHint(Poppler::Document::HideAnnotations, false);
        return {};
    }
    const double dpi = 72.0 * widthPixels / size.width();
    const QImage image = pdfPage->renderToImage(dpi, dpi);

    if (hideAnnotations)
        document->setRenderHint(Poppler::Document::HideAnnotations, false);
    return image;
}


} // namespace noteAppC

// The C face (PdfPageFace.h): what the host's PdfPage finds by name.
extern "C" {

const char *notolith_pdf_abi()
{
    return NOTEAPPC_ABI_TAG;
}

int notolith_pdf_page_count(const QString &path)
{
    return noteAppC::PdfPageImpl::pageCount(path);
}

QSizeF notolith_pdf_page_size(const QString &path, int page)
{
    return noteAppC::PdfPageImpl::pageSize(path, page);
}

QImage notolith_pdf_render(const QString &path, int page, int widthPixels, bool hideAnnotations)
{
    return noteAppC::PdfPageImpl::render(path, page, widthPixels, hideAnnotations);
}

}
