// The poppler-facing half of the PDF module: everything here touches
// poppler-qt6.h so nothing else in libs/ext/pdfview has to (PdfView.cpp
// works entirely off PdfAnnotationData/PdfOutlineItem/PdfLinkInfo).
//
// Coordinate convention, confirmed by round-tripping a highlight through a
// real PDF and rendering it while this module was built: every geometry
// field on Poppler::Annotation - boundary(), quad points, ink path points -
// and on Poppler::Link - linkArea() - and on Poppler::LinkDestination -
// top()/left() - is normalized 0..1 over the page, with y growing downward.
// That is also how a page raster is laid out, so converting between "a rect
// on the rendered page" and "an annotation's geometry" is just dividing or
// multiplying by the page's pixel size - no flip.
//
// Line/border width (Annotation::Style::width()) is the one field that is
// *not* normalized: it is in PDF points, confirmed the same way (a 3pt ink
// stroke visibly thicker than a 2pt rectangle border at the same render
// scale).
#include "PdfDocument.h"
#include "PdfRecovery.h"

#include <poppler-qt6.h>

#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>

using namespace Poppler;

namespace noteAppC {

namespace {

HighlightAnnotation::HighlightType highlightTypeOf(PdfAnnotationKind kind)
{
    switch (kind) {
    case PdfAnnotationKind::Underline:
        return HighlightAnnotation::Underline;
    case PdfAnnotationKind::StrikeOut:
        return HighlightAnnotation::StrikeOut;
    default:
        return HighlightAnnotation::Highlight;
    }
}

PdfAnnotationKind kindOfHighlight(HighlightAnnotation::HighlightType type)
{
    switch (type) {
    case HighlightAnnotation::Underline:
        return PdfAnnotationKind::Underline;
    case HighlightAnnotation::StrikeOut:
        return PdfAnnotationKind::StrikeOut;
    default:
        return PdfAnnotationKind::Highlight;
    }
}

/// PDF's QuadPoints order is corner-by-corner (top-left, top-right,
/// bottom-right, bottom-left) rather than perimeter order - connecting them
/// in the wrong order draws a bowtie instead of a rectangle, caught while
/// probing the API for this module.
QList<HighlightAnnotation::Quad> buildQuads(const QList<QRectF> &rects)
{
    QList<HighlightAnnotation::Quad> quads;
    quads.reserve(rects.size());
    for (const QRectF &r : rects) {
        HighlightAnnotation::Quad q;
        q.points[0] = r.topLeft();
        q.points[1] = r.topRight();
        q.points[2] = r.bottomRight();
        q.points[3] = r.bottomLeft();
        q.capStart = true;
        q.capEnd = true;
        q.feather = 0.1;
        quads.append(q);
    }
    return quads;
}

QList<QRectF> quadsToRects(const QList<HighlightAnnotation::Quad> &quads)
{
    QList<QRectF> rects;
    rects.reserve(quads.size());
    for (const HighlightAnnotation::Quad &q : quads) {
        QRectF r(q.points[0], q.points[2]);
        rects.append(r.normalized());
    }
    return rects;
}

QList<QVector<QPointF>> buildStrokes(const QList<QPolygonF> &strokes)
{
    QList<QVector<QPointF>> paths;
    paths.reserve(strokes.size());
    for (const QPolygonF &stroke : strokes)
        paths.append(QVector<QPointF>(stroke.begin(), stroke.end()));
    return paths;
}

QList<QPolygonF> strokesFromPaths(const QList<QVector<QPointF>> &paths)
{
    QList<QPolygonF> strokes;
    strokes.reserve(paths.size());
    for (const QVector<QPointF> &path : paths)
        strokes.append(QPolygonF(path));
    return strokes;
}

/// Builds a standalone poppler annotation from our data, ready to hand to
/// Page::addAnnotation (which copies what it needs and leaves this object
/// for the caller to destroy - see addAnnotation below).
std::unique_ptr<Annotation> toPoppler(const PdfAnnotationData &data)
{
    std::unique_ptr<Annotation> ann;
    switch (data.kind) {
    case PdfAnnotationKind::Highlight:
    case PdfAnnotationKind::Underline:
    case PdfAnnotationKind::StrikeOut: {
        auto highlight = std::make_unique<HighlightAnnotation>();
        highlight->setHighlightType(highlightTypeOf(data.kind));
        highlight->setHighlightQuads(buildQuads(data.quads));
        ann = std::move(highlight);
        break;
    }
    case PdfAnnotationKind::Ink: {
        auto ink = std::make_unique<InkAnnotation>();
        ink->setInkPaths(buildStrokes(data.strokes));
        ann = std::move(ink);
        break;
    }
    case PdfAnnotationKind::Rectangle: {
        auto rect = std::make_unique<GeomAnnotation>();
        rect->setGeomType(GeomAnnotation::InscribedSquare); // the box shape, not the circle
        ann = std::move(rect);
        break;
    }
    case PdfAnnotationKind::Note: {
        auto note = std::make_unique<TextAnnotation>(TextAnnotation::Linked);
        note->setTextIcon(QStringLiteral("Note"));
        ann = std::move(note);
        break;
    }
    }
    ann->setBoundary(data.boundary);
    ann->setContents(data.contents);
    Annotation::Style style = ann->style();
    style.setColor(data.color);
    style.setWidth(data.lineWidthPoints);
    ann->setStyle(style);
    const QDateTime when = data.modified.isValid() ? data.modified : QDateTime::currentDateTime();
    ann->setModificationDate(when);
    ann->setCreationDate(when);
    if (!data.id.isEmpty())
        ann->setUniqueName(data.id);
    return ann;
}

/// The inverse: reads a live poppler annotation into our own struct. Only
/// the four kinds PdfView's tools produce are understood - an existing PDF
/// may carry other annotation types (typed FreeText, stamps, form widgets),
/// which are left alone (never touched by add/update/remove, so a save
/// still round-trips them) but are not shown or editable in this viewer.
/// `page` mutates: a pre-existing annotation with no NM (unique name, most
/// PDFs not authored by poppler/Acrobat never set one) is assigned one here
/// so later update/remove calls have something stable to find it by; poppler
/// setters mutate the live Annot, so this sticks even before any save.
std::optional<PdfAnnotationData> fromPoppler(Annotation &ann, int page)
{
    PdfAnnotationData data;
    data.id = ann.uniqueName();
    if (data.id.isEmpty()) {
        data.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        ann.setUniqueName(data.id);
    }
    data.page = page;
    data.boundary = ann.boundary();
    data.contents = ann.contents();
    data.modified = ann.modificationDate();
    data.color = ann.style().color();
    data.lineWidthPoints = ann.style().width();

    switch (ann.subType()) {
    case Annotation::AHighlight: {
        auto &highlight = static_cast<HighlightAnnotation &>(ann);
        data.kind = kindOfHighlight(highlight.highlightType());
        data.quads = quadsToRects(highlight.highlightQuads());
        break;
    }
    case Annotation::AInk: {
        auto &ink = static_cast<InkAnnotation &>(ann);
        data.kind = PdfAnnotationKind::Ink;
        data.strokes = strokesFromPaths(ink.inkPaths());
        break;
    }
    case Annotation::AGeom:
        data.kind = PdfAnnotationKind::Rectangle;
        break;
    case Annotation::AText:
        data.kind = PdfAnnotationKind::Note;
        break;
    default:
        return std::nullopt;
    }
    return data;
}

PdfOutlineItem convertOutline(const OutlineItem &item)
{
    PdfOutlineItem out;
    out.title = item.name();
    if (const auto dest = item.destination()) {
        out.page = dest->pageNumber() - 1; // poppler's outline pages are 1-based
        out.top = dest->isChangeTop() ? dest->top() : 0.0;
    }
    const auto children = item.children();
    out.children.reserve(children.size());
    for (const OutlineItem &child : children)
        out.children.append(convertOutline(child));
    return out;
}

/// One Document per calling thread, reopened only when the path or its
/// mtime changes - the exact shape of PdfPage.cpp's own openCached, kept as
/// a separate copy rather than shared: that one is private to libs/render,
/// and duplicating eight lines is cheaper than exporting it.
Document *openForSearch(const QString &path)
{
    struct Cached {
        QString path;
        qint64 mtime = 0;
        std::unique_ptr<Document> document;
    };
    thread_local Cached cache;

    const qint64 mtime = QFileInfo(path).lastModified().toSecsSinceEpoch();
    if (cache.document && cache.path == path && cache.mtime == mtime)
        return cache.document.get();

    cache.document = Document::load(path);
    cache.path = path;
    cache.mtime = mtime;
    return cache.document.get();
}

} // namespace

PdfDocument::PdfDocument() = default;
PdfDocument::~PdfDocument() = default;

bool PdfDocument::load(const QString &path, QString *error)
{
    auto doc = Document::load(path);
    if (!doc) {
        if (error)
            *error = QStringLiteral("could not open as a PDF");
        return false;
    }
    if (doc->isLocked()) {
        if (error)
            *error = QStringLiteral("password-protected PDFs are not supported");
        return false;
    }
    doc->setRenderHint(Document::Antialiasing, true);
    doc->setRenderHint(Document::TextAntialiasing, true);
    m_doc = std::move(doc);
    m_path = path;
    m_modified = false;
    return true;
}

int PdfDocument::pageCount() const
{
    return m_doc ? m_doc->numPages() : 0;
}

QSizeF PdfDocument::pageSizePoints(int page) const
{
    if (!m_doc)
        return {};
    const auto p = m_doc->page(page);
    return p ? p->pageSizeF() : QSizeF();
}

QString PdfDocument::text(int page, const QRectF &rectPoints) const
{
    if (!m_doc)
        return {};
    const auto p = m_doc->page(page);
    return p ? p->text(rectPoints) : QString();
}

QList<PdfWordBox> PdfDocument::words(int page) const
{
    QList<PdfWordBox> out;
    if (!m_doc)
        return out;
    const auto p = m_doc->page(page);
    if (!p)
        return out;
    for (const auto &box : p->textList())
        out.append({box->boundingBox(), box->text(), box->hasSpaceAfter()});
    return out;
}

QList<QRectF> PdfDocument::findOnPage(int page, const QString &needle) const
{
    if (!m_doc || needle.isEmpty())
        return {};
    const auto p = m_doc->page(page);
    if (!p)
        return {};
    return p->search(needle, Page::IgnoreCase);
}

QList<QPair<int, QRectF>> PdfDocument::findAcrossDocument(const QString &path, const QString &needle,
                                                           int pageCount)
{
    QList<QPair<int, QRectF>> matches;
    if (needle.isEmpty())
        return matches;
    Document *document = openForSearch(path);
    if (!document)
        return matches;
    for (int page = 0; page < pageCount; ++page) {
        const auto p = document->page(page);
        if (!p)
            continue;
        for (const QRectF &rect : p->search(needle, Page::IgnoreCase))
            matches.append({page, rect});
    }
    return matches;
}

QList<PdfOutlineItem> PdfDocument::outline() const
{
    QList<PdfOutlineItem> result;
    if (!m_doc)
        return result;
    for (const OutlineItem &item : m_doc->outline())
        result.append(convertOutline(item));
    return result;
}

QList<PdfLinkInfo> PdfDocument::links(int page) const
{
    QList<PdfLinkInfo> out;
    if (!m_doc)
        return out;
    const auto p = m_doc->page(page);
    if (!p)
        return out;
    for (const auto &link : p->links()) {
        if (link->linkType() == Link::Goto) {
            auto *goTo = static_cast<LinkGoto *>(link.get());
            if (goTo->isExternal())
                continue; // linking into another PDF is out of scope
            const LinkDestination dest = goTo->destination();
            PdfLinkInfo info;
            info.area = link->linkArea();
            info.internal = true;
            info.targetPage = dest.pageNumber() - 1;
            info.targetTop = dest.isChangeTop() ? dest.top() : 0.0;
            out.append(info);
        } else if (link->linkType() == Link::Browse) {
            auto *browse = static_cast<LinkBrowse *>(link.get());
            PdfLinkInfo info;
            info.area = link->linkArea();
            info.internal = false;
            info.url = browse->url();
            out.append(info);
        }
    }
    return out;
}

QList<PdfAnnotationData> PdfDocument::annotations(int page) const
{
    QList<PdfAnnotationData> out;
    if (!m_doc)
        return out;
    const auto p = m_doc->page(page);
    if (!p)
        return out;
    for (auto &ann : p->annotations()) {
        if (auto data = fromPoppler(*ann, page))
            out.append(*data);
    }
    return out;
}

QString PdfDocument::addAnnotation(PdfAnnotationData data)
{
    if (!m_doc)
        return {};
    const auto p = m_doc->page(data.page);
    if (!p)
        return {};
    if (data.id.isEmpty())
        data.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    data.modified = QDateTime::currentDateTime();
    const std::unique_ptr<Annotation> ann = toPoppler(data);
    p->addAnnotation(ann.get()); // copies what it needs; ann is ours to free
    m_modified = true;
    return data.id;
}

bool PdfDocument::updateAnnotation(const PdfAnnotationData &data)
{
    if (!m_doc)
        return false;
    const auto p = m_doc->page(data.page);
    if (!p)
        return false;
    for (auto &ann : p->annotations()) {
        if (ann->uniqueName() != data.id)
            continue;
        // Live setters: this object wraps the same underlying Annot as the
        // one actually on the page (poppler-qt6.h: "if you call this method
        // twice, you get different objects pointing to the same
        // annotations"), so mutating it here is the edit - no remove/re-add.
        ann->setBoundary(data.boundary);
        ann->setContents(data.contents);
        Annotation::Style style = ann->style();
        style.setColor(data.color);
        style.setWidth(data.lineWidthPoints);
        ann->setStyle(style);
        ann->setModificationDate(QDateTime::currentDateTime());
        if (ann->subType() == Annotation::AHighlight) {
            auto *highlight = static_cast<HighlightAnnotation *>(ann.get());
            highlight->setHighlightType(highlightTypeOf(data.kind));
            highlight->setHighlightQuads(buildQuads(data.quads));
        } else if (ann->subType() == Annotation::AInk) {
            auto *ink = static_cast<InkAnnotation *>(ann.get());
            ink->setInkPaths(buildStrokes(data.strokes));
        }
        m_modified = true;
        return true;
    }
    return false;
}

bool PdfDocument::removeAnnotation(int page, const QString &id)
{
    if (!m_doc)
        return false;
    const auto p = m_doc->page(page);
    if (!p)
        return false;
    for (auto &ann : p->annotations()) {
        if (ann->uniqueName() != id)
            continue;
        p->removeAnnotation(ann.get());
        // Page::removeAnnotation "destroys the annotation object" itself
        // (poppler-qt6.h) - it has already freed *ann. Our unique_ptr must
        // let go without deleting it a second time, or the temporary
        // vector's destructor double-frees it the moment this loop ends -
        // this crashed with a plain SIGSEGV inside removeAnnotation before
        // the release() was added.
        (void)ann.release();
        m_modified = true;
        return true;
    }
    return false;
}

bool PdfDocument::save(QString *error)
{
    if (!m_doc || m_path.isEmpty()) {
        if (error)
            *error = QStringLiteral("no document open");
        return false;
    }

    // The previous bytes, kept outside the vault, before anything on disk
    // changes - mirrors Snapshots' own ordering in MainWindow's save path.
    PdfRecovery::record(m_path);

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const std::unique_ptr<PDFConverter> converter = m_doc->pdfConverter();
    converter->setPDFOptions(PDFConverter::WithChanges);
    converter->setOutputDevice(&file);
    if (!converter->convert()) {
        if (error)
            *error = QStringLiteral("poppler could not write the PDF");
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }

    // Reopen against the file just written: PDFConverter's output is not
    // guaranteed byte-identical to a fresh load (poppler rewrites the xref
    // structure), and every annotation lookup after this must see exactly
    // what a reopen would - which matters the moment the render cache's
    // (path, mtime) key sees the mtime that commit() just moved.
    const QString path = m_path;
    QString reloadError;
    if (!load(path, &reloadError)) {
        if (error)
            *error = reloadError;
        return false;
    }
    return true;
}

} // namespace noteAppC
