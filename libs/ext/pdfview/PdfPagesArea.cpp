#include "PdfPagesArea.h"

#include "Lifeline.h"
#include "PdfAnnotationCommands.h"
#include "PdfPage.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QUrl>
#include <QWheelEvent>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>

namespace noteAppC {

PdfPagesArea::PdfPagesArea(QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_lifeline(std::make_shared<Lifeline>())
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    setFrameShape(QFrame::NoFrame);

    connect(&m_undo, &QUndoStack::cleanChanged, this, [this](bool clean) { emit modificationChanged(!clean); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        recomputeCurrentPage();
        scheduleVisibleRenders();
        viewport()->update();
    });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { viewport()->update(); });
}

PdfPagesArea::~PdfPagesArea()
{
    m_lifeline->end();
}

void PdfPagesArea::setTheme(const Theme &theme)
{
    m_theme = theme;
    viewport()->update();
}

bool PdfPagesArea::load(const QString &path, QString *error)
{
    if (!m_document.load(path, error))
        return false;
    m_rasterCache.clear();
    m_pageSizePoints.clear();
    m_pageSizePoints.reserve(m_document.pageCount());
    for (int page = 0; page < m_document.pageCount(); ++page)
        m_pageSizePoints.append(m_document.pageSizePoints(page));
    m_currentPage = 0;
    m_selectedID.clear();
    m_selectedPage = -1;
    m_selectionDragPts = QRectF();
    m_selectionText.clear();
    m_selectionPage = -1;
    findClear();
    m_undo.clear();

    relayout();
    verticalScrollBar()->setValue(0);
    scheduleVisibleRenders();
    return true;
}

bool PdfPagesArea::save(QString *error)
{
    commitCommentEditor();
    if (!m_document.save(error))
        return false;
    m_undo.setClean();
    viewport()->update();
    return true;
}

// --- layout ------------------------------------------------------------

void PdfPagesArea::relayout()
{
    const int pages = m_pageSizePoints.size();
    m_pageLayout.clear();
    m_pageLayout.reserve(pages);
    const double scale = pixelsPerPoint();

    double maxWidth = 0;
    for (const QSizeF &pts : std::as_const(m_pageSizePoints))
        maxWidth = std::max(maxWidth, pts.width() * scale);
    const double frameWidth = std::max(maxWidth, double(viewport()->width()));

    double y = kPageGap;
    for (const QSizeF &pts : std::as_const(m_pageSizePoints)) {
        const double w = pts.width() * scale;
        const double h = pts.height() * scale;
        const double x = (frameWidth - w) / 2.0;
        m_pageLayout.append(QRectF(x, y, w, h));
        y += h + kPageGap;
    }
    m_layoutContentWidth = frameWidth;
    m_layoutTotalHeight = y;
    updateScrollRange();
}

void PdfPagesArea::updateScrollRange()
{
    verticalScrollBar()->setRange(0, std::max(0, int(std::ceil(m_layoutTotalHeight)) - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(60);
    horizontalScrollBar()->setRange(0, std::max(0, int(std::ceil(m_layoutContentWidth)) - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
}

QRectF PdfPagesArea::pageRectInLayout(int page) const
{
    return m_pageLayout.value(page);
}

int PdfPagesArea::pageAt(const QPointF &layoutPoint) const
{
    for (int page = 0; page < m_pageLayout.size(); ++page) {
        if (m_pageLayout.at(page).contains(layoutPoint))
            return page;
    }
    return -1;
}

QPointF PdfPagesArea::toPagePoint(int page, const QPointF &layoutPoint) const
{
    const QRectF rect = pageRectInLayout(page);
    const double scale = pixelsPerPoint();
    if (scale <= 0)
        return {};
    return (layoutPoint - rect.topLeft()) / scale;
}

QPointF PdfPagesArea::toLayoutPoint(int page, const QPointF &pagePoint) const
{
    const QRectF rect = pageRectInLayout(page);
    return rect.topLeft() + pagePoint * pixelsPerPoint();
}

QRectF PdfPagesArea::toLayoutRect(int page, const QRectF &pageRectPoints) const
{
    return QRectF(toLayoutPoint(page, pageRectPoints.topLeft()), toLayoutPoint(page, pageRectPoints.bottomRight()));
}

QRectF PdfPagesArea::normalizedToLayoutRect(int page, const QRectF &normalized) const
{
    const QSizeF sizePts = m_pageSizePoints.value(page);
    if (sizePts.isEmpty())
        return {};
    const QRectF pts(normalized.x() * sizePts.width(), normalized.y() * sizePts.height(),
                     normalized.width() * sizePts.width(), normalized.height() * sizePts.height());
    return toLayoutRect(page, pts);
}

QPointF PdfPagesArea::viewportToLayout(const QPoint &viewportPoint) const
{
    return QPointF(viewportPoint) + QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
}

void PdfPagesArea::visiblePageRange(int &first, int &last) const
{
    first = -1;
    last = -1;
    const double top = verticalScrollBar()->value() - kPageGap;
    const double bottom = verticalScrollBar()->value() + viewport()->height() + kPageGap;
    for (int page = 0; page < m_pageLayout.size(); ++page) {
        const QRectF &rect = m_pageLayout.at(page);
        if (rect.bottom() < top)
            continue;
        if (rect.top() > bottom)
            break;
        if (first < 0)
            first = page;
        last = page;
    }
}

void PdfPagesArea::recomputeCurrentPage()
{
    const double centerY = verticalScrollBar()->value() + viewport()->height() / 3.0;
    int page = 0;
    for (int i = 0; i < m_pageLayout.size(); ++i) {
        if (m_pageLayout.at(i).top() <= centerY)
            page = i;
        else
            break;
    }
    if (page != m_currentPage) {
        m_currentPage = page;
        emit currentPageChanged(page);
    }
}

void PdfPagesArea::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    relayout();
    scheduleVisibleRenders();
}

// --- zoom and navigation -------------------------------------------------

void PdfPagesArea::setZoom(double zoom)
{
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    if (std::abs(zoom - m_zoom) < 1e-6)
        return;
    // Keep the page under the viewport's vertical centre in place, so
    // zooming under the cursor/centre does not jump the reader elsewhere.
    const double centerLayoutY = verticalScrollBar()->value() + viewport()->height() / 2.0;
    const int anchorPage = pageAt(QPointF(m_layoutContentWidth / 2, centerLayoutY)) >= 0
                               ? pageAt(QPointF(m_layoutContentWidth / 2, centerLayoutY))
                               : m_currentPage;
    const QRectF anchorRectBefore = pageRectInLayout(anchorPage);
    const double fractionInPage = anchorRectBefore.height() > 0
                                      ? (centerLayoutY - anchorRectBefore.top()) / anchorRectBefore.height()
                                      : 0.0;

    m_zoom = zoom;
    relayout();

    const QRectF anchorRectAfter = pageRectInLayout(anchorPage);
    const double newCenterY = anchorRectAfter.top() + fractionInPage * anchorRectAfter.height();
    verticalScrollBar()->setValue(int(newCenterY - viewport()->height() / 2.0));

    scheduleVisibleRenders();
    emit zoomChanged(m_zoom);
    viewport()->update();
}

void PdfPagesArea::fitWidth()
{
    if (m_pageSizePoints.isEmpty())
        return;
    const int page = std::clamp(m_currentPage, 0, int(m_pageSizePoints.size()) - 1);
    const double widthPts = m_pageSizePoints.at(page).width();
    if (widthPts <= 0)
        return;
    // A small margin either side so the page edge is not flush with the
    // viewport border.
    const double target = std::max(1.0, double(viewport()->width()) - 2 * kPageGap);
    setZoom((target / widthPts) * 72.0 / kBaseDpi);
}

void PdfPagesArea::fitPage()
{
    if (m_pageSizePoints.isEmpty())
        return;
    const int page = std::clamp(m_currentPage, 0, int(m_pageSizePoints.size()) - 1);
    const QSizeF pts = m_pageSizePoints.at(page);
    if (pts.width() <= 0 || pts.height() <= 0)
        return;
    const double targetW = std::max(1.0, double(viewport()->width()) - 2 * kPageGap);
    const double targetH = std::max(1.0, double(viewport()->height()) - 2 * kPageGap);
    const double zoomW = (targetW / pts.width()) * 72.0 / kBaseDpi;
    const double zoomH = (targetH / pts.height()) * 72.0 / kBaseDpi;
    setZoom(std::min(zoomW, zoomH));
}

void PdfPagesArea::goToPage(int page, double top)
{
    page = std::clamp(page, 0, std::max(0, pageCount() - 1));
    if (page >= m_pageLayout.size())
        return;
    const QRectF rect = m_pageLayout.at(page);
    verticalScrollBar()->setValue(int(rect.top() + top * rect.height()));
}

// --- rendering -----------------------------------------------------------

QPixmap PdfPagesArea::ensureRasterAsync(int page, int widthPixels)
{
    const QString path = m_document.path();
    const qint64 mtime = QFileInfo(path).lastModified().toSecsSinceEpoch();
    // Bucketed to the nearest 5%: a continuous Ctrl+wheel zoom must not
    // schedule a fresh render for every intermediate frame.
    const int zoomBucket = qRound(m_zoom * 20.0);
    const int dprBucket = qRound(devicePixelRatioF() * 100.0);
    const QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(path).arg(mtime).arg(page).arg(zoomBucket).arg(dprBucket);

    auto it = m_rasterCache.find(key);
    if (it != m_rasterCache.end())
        return it->pending ? QPixmap() : it->image;

    RasterEntry entry;
    entry.pending = true;
    entry.generation = ++m_rasterGeneration;
    m_rasterCache.insert(key, entry);
    const quint64 generation = entry.generation;
    const qreal dpr = devicePixelRatioF();
    const int devicePixels = qRound(widthPixels * dpr);

    (void)QtConcurrent::run([this, lifeline = m_lifeline, key, generation, path, page, devicePixels, dpr] {
        QImage image = PdfPage::render(path, page, devicePixels, /*hideAnnotations=*/true);
        lifeline->post(this, [this, key, generation, image = std::move(image), dpr]() mutable {
            QPixmap pixmap = image.isNull() ? QPixmap() : QPixmap::fromImage(image);
            if (!pixmap.isNull())
                pixmap.setDevicePixelRatio(dpr);
            deliverRaster(key, generation, std::move(pixmap));
        });
    });
    return QPixmap();
}

void PdfPagesArea::deliverRaster(const QString &key, quint64 generation, QPixmap pixmap)
{
    auto it = m_rasterCache.find(key);
    if (it == m_rasterCache.end() || it->generation != generation)
        return;
    it->pending = false;
    it->image = pixmap;
    viewport()->update();
}

void PdfPagesArea::scheduleVisibleRenders()
{
    int first = -1, last = -1;
    visiblePageRange(first, last);
    if (first < 0)
        return;
    // One extra page above and below the viewport, so a small scroll never
    // shows a bare placeholder while its render is still in flight.
    first = std::max(0, first - 1);
    last = std::min(pageCount() - 1, last + 1);
    for (int page = first; page <= last; ++page) {
        const int widthPixels = qRound(pageRectInLayout(page).width());
        if (widthPixels > 0)
            ensureRasterAsync(page, widthPixels);
    }
}

// --- painting --------------------------------------------------------------

void PdfPagesArea::paintEvent(QPaintEvent *)
{
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), m_theme.color(QStringLiteral("--background-secondary")));

    const QPointF scroll(horizontalScrollBar()->value(), verticalScrollBar()->value());
    int first = -1, last = -1;
    visiblePageRange(first, last);
    if (first < 0)
        return;
    first = std::max(0, first - 1);
    last = std::min(pageCount() - 1, last + 1);

    for (int page = first; page <= last; ++page) {
        const QRectF screenRect = pageRectInLayout(page).translated(-scroll);
        paintPage(painter, page, screenRect);
    }

    if (m_tool == Tool::Ink && !m_liveInkStroke.isEmpty() && m_dragPage >= 0) {
        QPainterPath path;
        path.moveTo(toLayoutPoint(m_dragPage, m_liveInkStroke.first()) - scroll);
        for (int i = 1; i < m_liveInkStroke.size(); ++i)
            path.lineTo(toLayoutPoint(m_dragPage, m_liveInkStroke.at(i)) - scroll);
        painter.save();
        QPen pen(m_color, std::max(1.0, m_inkWidth * pixelsPerPoint()));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawPath(path);
        painter.restore();
    }
    if (m_tool == Tool::Rectangle && m_dragging && m_dragPage >= 0) {
        const QRectF rect = toLayoutRect(m_dragPage, QRectF(m_dragStartPagePts, m_dragCurrentPagePts).normalized())
                                .translated(-scroll);
        painter.save();
        painter.setPen(QPen(m_color, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect);
        painter.restore();
    }
    if (m_tool == Tool::Select && m_selectionPage >= 0 && !m_selectionDragPts.isEmpty()) {
        // Not gated on m_dragging: a text selection stays highlighted (and
        // copyable with Ctrl+C) after the mouse comes up, same as any text
        // editor's own selection.
        const auto rows = selectionRows(m_selectionPage, m_selectionDragPts);
        painter.save();
        QColor accent = m_theme.color(QStringLiteral("--text-accent"));
        accent.setAlpha(70);
        painter.setBrush(accent);
        painter.setPen(Qt::NoPen);
        for (const QRectF &row : rows)
            painter.drawRect(toLayoutRect(m_selectionPage, row).translated(-scroll));
        painter.restore();
    }
    if ((m_tool == Tool::Highlight || m_tool == Tool::Underline || m_tool == Tool::StrikeOut) && m_dragging
        && m_dragPage >= 0) {
        const auto rows = selectionRows(m_dragPage, QRectF(m_dragStartPagePts, m_dragCurrentPagePts).normalized());
        painter.save();
        QColor preview = m_color;
        preview.setAlpha(110);
        painter.setBrush(preview);
        painter.setPen(Qt::NoPen);
        for (const QRectF &row : rows)
            painter.drawRect(toLayoutRect(m_dragPage, row).translated(-scroll));
        painter.restore();
    }
}

void PdfPagesArea::paintPage(QPainter &painter, int page, const QRectF &screenRect)
{
    const QColor border = m_theme.color(QStringLiteral("--background-modifier-border"));
    painter.save();
    painter.setPen(QPen(border, 1));
    painter.setBrush(Qt::white); // the page's own colour - see the class comment
    painter.drawRect(screenRect.adjusted(-0.5, -0.5, 0.5, 0.5));
    painter.restore();

    const int widthPixels = qRound(screenRect.width());
    const QPixmap raster = widthPixels > 0 ? ensureRasterAsync(page, widthPixels) : QPixmap();
    if (raster.isNull()) {
        painter.save();
        painter.setPen(m_theme.color(QStringLiteral("--text-faint")));
        painter.setFont(m_theme.uiFont());
        painter.drawText(screenRect, Qt::AlignCenter, tr("Page %1").arg(page + 1));
        painter.restore();
    } else {
        painter.drawPixmap(screenRect.topLeft(), raster);
    }

    for (const PdfAnnotationData &data : m_document.annotations(page)) {
        const bool selected = (page == m_selectedPage && data.id == m_selectedID);
        if (selected && m_movingAnnotation) {
            PdfAnnotationData moved = data;
            moved.boundary.translate(m_moveOffsetNormalized);
            if (moved.kind == PdfAnnotationKind::Ink) {
                for (QPolygonF &stroke : moved.strokes)
                    stroke.translate(m_moveOffsetNormalized);
            }
            paintAnnotation(painter, moved, screenRect, true);
        } else {
            paintAnnotation(painter, data, screenRect, selected);
        }
    }

    // Find matches on this page.
    if (!m_findMatches.isEmpty()) {
        const QSizeF sizePts = m_pageSizePoints.value(page);
        if (!sizePts.isEmpty()) {
            painter.save();
            for (int i = 0; i < m_findMatches.size(); ++i) {
                const auto &match = m_findMatches.at(i);
                if (match.first != page)
                    continue;
                const QRectF n(match.second.x() / sizePts.width(), match.second.y() / sizePts.height(),
                              match.second.width() / sizePts.width(), match.second.height() / sizePts.height());
                const QRectF r(screenRect.x() + n.x() * screenRect.width(),
                              screenRect.y() + n.y() * screenRect.height(), n.width() * screenRect.width(),
                              n.height() * screenRect.height());
                QColor color = (i == m_findCurrent) ? QColor(0xFF, 0x98, 0x00) : QColor(0xFF, 0xEB, 0x3B);
                color.setAlpha(i == m_findCurrent ? 190 : 130);
                painter.fillRect(r, color);
            }
            painter.restore();
        }
    }
}

void PdfPagesArea::paintAnnotation(QPainter &painter, const PdfAnnotationData &data, const QRectF &pageScreenRect,
                                   bool selected) const
{
    const auto toScreen = [&](const QPointF &n) {
        return QPointF(pageScreenRect.x() + n.x() * pageScreenRect.width(),
                       pageScreenRect.y() + n.y() * pageScreenRect.height());
    };
    const auto rectToScreen = [&](const QRectF &n) {
        return QRectF(toScreen(n.topLeft()), toScreen(n.bottomRight()));
    };

    painter.save();
    switch (data.kind) {
    case PdfAnnotationKind::Highlight: {
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        QColor fill = data.color;
        for (const QRectF &q : data.quads)
            painter.fillRect(rectToScreen(q), fill);
        break;
    }
    case PdfAnnotationKind::Underline:
    case PdfAnnotationKind::StrikeOut: {
        QPen pen(data.color, std::max(1.5, data.lineWidthPoints));
        painter.setPen(pen);
        for (const QRectF &q : data.quads) {
            const QRectF r = rectToScreen(q);
            const double y = data.kind == PdfAnnotationKind::Underline ? r.bottom() - 1 : r.center().y();
            painter.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }
        break;
    }
    case PdfAnnotationKind::Ink: {
        QPen pen(data.color, std::max(1.0, data.lineWidthPoints * pixelsPerPoint()));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        for (const QPolygonF &stroke : data.strokes) {
            if (stroke.size() < 2)
                continue;
            QPainterPath path;
            path.moveTo(toScreen(stroke.first()));
            for (int i = 1; i < stroke.size(); ++i)
                path.lineTo(toScreen(stroke.at(i)));
            painter.drawPath(path);
        }
        break;
    }
    case PdfAnnotationKind::Rectangle: {
        painter.setPen(QPen(data.color, std::max(1.0, data.lineWidthPoints * pixelsPerPoint())));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rectToScreen(data.boundary));
        break;
    }
    case PdfAnnotationKind::Note: {
        const QRectF r = rectToScreen(data.boundary);
        QPainterPath fold;
        fold.moveTo(r.topLeft());
        fold.lineTo(r.topRight());
        fold.lineTo(r.bottomRight());
        fold.lineTo(QPointF(r.left(), r.bottom()));
        fold.closeSubpath();
        painter.setPen(QPen(data.color.darker(140), 1));
        painter.setBrush(data.color);
        painter.drawPath(fold);
        break;
    }
    }
    painter.restore();

    if (selected) {
        painter.save();
        QPen dashed(m_theme.color(QStringLiteral("--text-accent")), 1.5, Qt::DashLine);
        painter.setPen(dashed);
        painter.setBrush(Qt::NoBrush);
        QRectF handle = rectToScreen(data.boundary).adjusted(-3, -3, 3, 3);
        if (!data.quads.isEmpty()) {
            QRectF bound;
            for (const QRectF &q : data.quads)
                bound = bound.isNull() ? q : bound.united(q);
            handle = rectToScreen(bound).adjusted(-3, -3, 3, 3);
        }
        painter.drawRect(handle);
        painter.restore();
    }
}

// --- interaction: hit testing -------------------------------------------

std::optional<PdfAnnotationData> PdfPagesArea::annotationAt(int page, const QPointF &pagePointPts) const
{
    const QSizeF sizePts = m_pageSizePoints.value(page);
    if (sizePts.isEmpty() || sizePts.width() <= 0 || sizePts.height() <= 0)
        return std::nullopt;
    const QPointF n(pagePointPts.x() / sizePts.width(), pagePointPts.y() / sizePts.height());
    constexpr double kPad = 0.006; // slack so a thin underline/ink stroke is still easy to hit
    const QList<PdfAnnotationData> annotations = m_document.annotations(page);
    for (auto it = annotations.crbegin(); it != annotations.crend(); ++it) {
        QRectF box = it->boundary;
        if (!it->quads.isEmpty()) {
            for (const QRectF &q : it->quads)
                box = box.isNull() ? q : box.united(q);
        }
        if (box.adjusted(-kPad, -kPad, kPad, kPad).contains(n))
            return *it;
    }
    return std::nullopt;
}

std::optional<PdfAnnotationData> PdfPagesArea::findByID(int page, const QString &id) const
{
    for (const PdfAnnotationData &data : m_document.annotations(page)) {
        if (data.id == id)
            return data;
    }
    return std::nullopt;
}

QList<QRectF> PdfPagesArea::selectionRows(int page, const QRectF &dragRectPts) const
{
    QList<QRectF> rows;
    if (dragRectPts.isEmpty())
        return rows;
    struct Band {
        double centerY;
        QRectF rect;
    };
    QList<Band> bands;
    for (const PdfWordBox &word : m_document.words(page)) {
        if (!word.rect.intersects(dragRectPts))
            continue;
        const double cy = word.rect.center().y();
        bool merged = false;
        for (Band &band : bands) {
            if (std::abs(band.centerY - cy) < std::max(2.0, word.rect.height() * 0.6)) {
                band.rect = band.rect.united(word.rect);
                band.centerY = (band.centerY + cy) / 2.0;
                merged = true;
                break;
            }
        }
        if (!merged)
            bands.append({cy, word.rect});
    }
    for (const Band &band : bands)
        rows.append(band.rect);
    std::sort(rows.begin(), rows.end(), [](const QRectF &a, const QRectF &b) { return a.top() < b.top(); });
    return rows;
}

void PdfPagesArea::commitTextMarkup(int page, const QList<QRectF> &rowsPts)
{
    const QSizeF sizePts = m_pageSizePoints.value(page);
    if (sizePts.isEmpty() || sizePts.width() <= 0 || sizePts.height() <= 0)
        return;
    PdfAnnotationData data;
    data.kind = m_tool == Tool::Highlight ? PdfAnnotationKind::Highlight
              : m_tool == Tool::Underline ? PdfAnnotationKind::Underline
                                          : PdfAnnotationKind::StrikeOut;
    data.page = page;
    data.color = m_color;
    QRectF bound;
    for (const QRectF &r : rowsPts) {
        const QRectF n(r.x() / sizePts.width(), r.y() / sizePts.height(), r.width() / sizePts.width(),
                      r.height() / sizePts.height());
        data.quads.append(n);
        bound = bound.isNull() ? n : bound.united(n);
    }
    data.boundary = bound;
    auto *command = new AddPdfAnnotationCommand(&m_document, data);
    m_undo.push(command);
    m_selectedID = command->annotationID();
    m_selectedPage = page;
    emit selectionChanged();
}

void PdfPagesArea::followInteractionAt(int page, const QPointF &pagePointPts)
{
    const QSizeF sizePts = m_pageSizePoints.value(page);
    if (sizePts.isEmpty() || sizePts.width() <= 0 || sizePts.height() <= 0)
        return;
    const QPointF n(pagePointPts.x() / sizePts.width(), pagePointPts.y() / sizePts.height());
    for (const PdfLinkInfo &link : m_document.links(page)) {
        if (!link.area.contains(n))
            continue;
        if (link.internal)
            goToPage(link.targetPage, link.targetTop);
        else if (!link.url.isEmpty())
            emit openExternalUrl(link.url);
        return;
    }
}

// --- mouse and keyboard ---------------------------------------------------

void PdfPagesArea::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    if (m_commentEditor)
        commitCommentEditor();

    const QPointF layoutPt = viewportToLayout(event->pos());
    const int page = pageAt(layoutPt);

    if (event->button() == Qt::RightButton) {
        if (page >= 0) {
            const auto hit = annotationAt(page, toPagePoint(page, layoutPt));
            if (hit) {
                m_selectedID = hit->id;
                m_selectedPage = page;
                emit selectionChanged();
                viewport()->update();
                showAnnotationMenu(page, hit->id, event->globalPosition().toPoint());
                return;
            }
        }
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;

    if (page < 0) {
        if (!m_selectedID.isEmpty() || !m_selectionDragPts.isEmpty()) {
            m_selectedID.clear();
            m_selectedPage = -1;
            m_selectionDragPts = QRectF();
            m_selectionText.clear();
            m_selectionPage = -1;
            emit selectionChanged();
            viewport()->update();
        }
        return;
    }

    const QPointF pagePt = toPagePoint(page, layoutPt);
    m_dragging = true;
    m_dragStartViewport = event->pos();
    m_dragPage = page;
    m_dragStartPagePts = pagePt;
    m_dragCurrentPagePts = pagePt;
    m_movingAnnotation = false;

    if (m_tool == Tool::Select) {
        // Every fresh press starts a new potential gesture - the previous
        // text selection, if any, does not survive a new click the way it
        // would in any other text view either.
        m_selectionDragPts = QRectF();
        m_selectionText.clear();
        m_selectionPage = -1;
        const auto hit = annotationAt(page, pagePt);
        if (hit && !hit->isTextMarkup()) {
            m_selectedID = hit->id;
            m_selectedPage = page;
            m_movingAnnotation = true;
            m_moveBefore = *hit;
            m_moveOffsetNormalized = QPointF(0, 0);
        } else if (hit) {
            m_selectedID = hit->id;
            m_selectedPage = page;
        } else if (!m_selectedID.isEmpty()) {
            m_selectedID.clear();
            m_selectedPage = -1;
        }
        emit selectionChanged();
    } else if (m_tool == Tool::Ink) {
        m_liveInkStroke.clear();
        m_liveInkStroke << pagePt;
    }
    viewport()->update();
}

void PdfPagesArea::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging || m_dragPage < 0)
        return;
    const QPointF layoutPt = viewportToLayout(event->pos());
    const QPointF pagePt = toPagePoint(m_dragPage, layoutPt);
    m_dragCurrentPagePts = pagePt;

    if (m_tool == Tool::Select && m_movingAnnotation) {
        const QSizeF sizePts = m_pageSizePoints.value(m_dragPage);
        if (!sizePts.isEmpty() && sizePts.width() > 0 && sizePts.height() > 0) {
            m_moveOffsetNormalized = QPointF((pagePt.x() - m_dragStartPagePts.x()) / sizePts.width(),
                                             (pagePt.y() - m_dragStartPagePts.y()) / sizePts.height());
        }
    } else if (m_tool == Tool::Ink) {
        m_liveInkStroke << pagePt;
    } else if (m_tool == Tool::Select) {
        // Kept after the release too (not cleared when m_dragging goes
        // false), so the highlighted rows and Ctrl+C both still work once
        // the mouse comes up - that is the whole point of a text drag.
        m_selectionDragPts = QRectF(m_dragStartPagePts, pagePt).normalized();
        m_selectionText = m_document.text(m_dragPage, m_selectionDragPts);
        m_selectionPage = m_dragPage;
    }
    viewport()->update();
}

void PdfPagesArea::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_dragging) {
        m_dragging = false;
        return;
    }
    const bool moved = (event->pos() - m_dragStartViewport).manhattanLength() > 3;
    const int page = m_dragPage;

    switch (m_tool) {
    case Tool::Select:
        if (m_movingAnnotation)
            finishMove();
        else if (!moved)
            followInteractionAt(page, m_dragStartPagePts);
        break;
    case Tool::Highlight:
    case Tool::Underline:
    case Tool::StrikeOut: {
        const QRectF rect = QRectF(m_dragStartPagePts, m_dragCurrentPagePts).normalized();
        const QList<QRectF> rows = selectionRows(page, rect);
        if (!rows.isEmpty())
            commitTextMarkup(page, rows);
        break;
    }
    case Tool::Ink:
        if (m_liveInkStroke.size() > 1) {
            const QSizeF sizePts = m_pageSizePoints.value(page);
            if (sizePts.width() > 0 && sizePts.height() > 0) {
                QPolygonF normalized;
                normalized.reserve(m_liveInkStroke.size());
                for (const QPointF &pt : std::as_const(m_liveInkStroke))
                    normalized << QPointF(pt.x() / sizePts.width(), pt.y() / sizePts.height());
                PdfAnnotationData data;
                data.kind = PdfAnnotationKind::Ink;
                data.page = page;
                data.strokes = {normalized};
                data.color = m_color;
                data.lineWidthPoints = m_inkWidth;
                data.boundary = normalized.boundingRect().adjusted(-0.01, -0.01, 0.01, 0.01);
                auto *command = new AddPdfAnnotationCommand(&m_document, data);
                m_undo.push(command);
                m_selectedID = command->annotationID();
                m_selectedPage = page;
                emit selectionChanged();
            }
        }
        m_liveInkStroke.clear();
        break;
    case Tool::Rectangle: {
        const QRectF rect = QRectF(m_dragStartPagePts, m_dragCurrentPagePts).normalized();
        const QSizeF sizePts = m_pageSizePoints.value(page);
        if (rect.width() > 1.0 && rect.height() > 1.0 && sizePts.width() > 0 && sizePts.height() > 0) {
            PdfAnnotationData data;
            data.kind = PdfAnnotationKind::Rectangle;
            data.page = page;
            data.boundary = QRectF(rect.x() / sizePts.width(), rect.y() / sizePts.height(),
                                   rect.width() / sizePts.width(), rect.height() / sizePts.height());
            data.color = m_color;
            data.lineWidthPoints = m_inkWidth > 0 ? m_inkWidth : 2.0;
            auto *command = new AddPdfAnnotationCommand(&m_document, data);
            m_undo.push(command);
            m_selectedID = command->annotationID();
            m_selectedPage = page;
            emit selectionChanged();
        }
        break;
    }
    case Tool::Note:
        if (!moved) {
            const QSizeF sizePts = m_pageSizePoints.value(page);
            if (sizePts.width() > 0 && sizePts.height() > 0) {
                constexpr double kNoteSizePts = 20.0;
                PdfAnnotationData data;
                data.kind = PdfAnnotationKind::Note;
                data.page = page;
                data.boundary = QRectF(m_dragStartPagePts.x() / sizePts.width(),
                                       m_dragStartPagePts.y() / sizePts.height(), kNoteSizePts / sizePts.width(),
                                       kNoteSizePts / sizePts.height());
                data.color = m_color;
                auto *command = new AddPdfAnnotationCommand(&m_document, data);
                m_undo.push(command);
                m_selectedID = command->annotationID();
                m_selectedPage = page;
                emit selectionChanged();
                openCommentEditor(page, command->annotationID(), normalizedToLayoutRect(page, data.boundary));
            }
        }
        break;
    }

    m_dragging = false;
    m_movingAnnotation = false;
    viewport()->update();
}

void PdfPagesArea::finishMove()
{
    if (m_moveOffsetNormalized.isNull()) {
        m_movingAnnotation = false;
        return;
    }
    PdfAnnotationData after = m_moveBefore;
    after.boundary.translate(m_moveOffsetNormalized);
    if (after.kind == PdfAnnotationKind::Ink) {
        for (QPolygonF &stroke : after.strokes)
            stroke.translate(m_moveOffsetNormalized);
    }
    m_undo.push(new EditPdfAnnotationCommand(&m_document, m_moveBefore, after));
    m_movingAnnotation = false;
    m_moveOffsetNormalized = QPointF(0, 0);
}

void PdfPagesArea::mouseDoubleClickEvent(QMouseEvent *event)
{
    const QPointF layoutPt = viewportToLayout(event->pos());
    const int page = pageAt(layoutPt);
    if (page < 0)
        return;
    const auto hit = annotationAt(page, toPagePoint(page, layoutPt));
    if (!hit)
        return;
    m_selectedID = hit->id;
    m_selectedPage = page;
    emit selectionChanged();
    openCommentEditor(page, hit->id, normalizedToLayoutRect(page, hit->boundary));
}

void PdfPagesArea::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const double factor = event->angleDelta().y() > 0 ? 1.1 : (1.0 / 1.1);
        setZoom(m_zoom * factor);
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void PdfPagesArea::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_PageDown:
        verticalScrollBar()->setValue(verticalScrollBar()->value() + int(viewport()->height() * 0.9));
        return;
    case Qt::Key_PageUp:
        verticalScrollBar()->setValue(verticalScrollBar()->value() - int(viewport()->height() * 0.9));
        return;
    case Qt::Key_Home:
        goToPage(0);
        return;
    case Qt::Key_End:
        goToPage(pageCount() - 1);
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (!m_selectedID.isEmpty()) {
            deleteSelectedAnnotation();
            return;
        }
        break;
    case Qt::Key_C:
        if ((event->modifiers() & Qt::ControlModifier) && !m_selectionText.isEmpty()) {
            QApplication::clipboard()->setText(m_selectionText);
            return;
        }
        break;
    case Qt::Key_Escape:
        if (!m_selectedID.isEmpty()) {
            m_selectedID.clear();
            m_selectedPage = -1;
            emit selectionChanged();
            viewport()->update();
            return;
        }
        break;
    default:
        break;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PdfPagesArea::contextMenuEvent(QContextMenuEvent *event)
{
    // A right-click over an annotation is already handled in
    // mousePressEvent (which opens the menu directly, since it needs the
    // annotation the click landed on); nothing else adds a menu here.
    event->accept();
}

bool PdfPagesArea::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_commentEditor) {
        if (event->type() == QEvent::FocusOut) {
            commitCommentEditor();
        } else if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Escape) {
                m_commentEditor->deleteLater();
                m_commentEditor = nullptr;
                m_commentEditingID.clear();
                m_commentEditingPage = -1;
                return true;
            }
            if (key->key() == Qt::Key_Return && !(key->modifiers() & Qt::ShiftModifier)) {
                commitCommentEditor();
                return true;
            }
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

// --- annotation popups and menu ------------------------------------------

void PdfPagesArea::openCommentEditor(int page, const QString &id, const QRectF &boundaryLayoutRect)
{
    commitCommentEditor();
    const auto data = findByID(page, id);
    if (!data)
        return;
    m_commentEditingID = id;
    m_commentEditingPage = page;
    m_commentEditor = new QPlainTextEdit(viewport());
    m_commentEditor->setPlainText(data->contents);
    m_commentEditor->setPlaceholderText(tr("Comment..."));
    m_commentEditor->installEventFilter(this);
    QPalette pal = m_commentEditor->palette();
    pal.setColor(QPalette::Base, m_theme.color(QStringLiteral("--background-primary")));
    pal.setColor(QPalette::Text, m_theme.color(QStringLiteral("--text-normal")));
    m_commentEditor->setPalette(pal);
    const QPointF scroll(horizontalScrollBar()->value(), verticalScrollBar()->value());
    QPoint pos = (boundaryLayoutRect.bottomLeft() - scroll).toPoint();
    pos.setX(std::clamp(pos.x(), 0, std::max(0, viewport()->width() - 220)));
    m_commentEditor->setGeometry(QRect(pos, QSize(220, 90)));
    m_commentEditor->show();
    m_commentEditor->setFocus();
    m_commentEditor->selectAll();
}

void PdfPagesArea::commitCommentEditor()
{
    if (!m_commentEditor)
        return;
    const auto before = findByID(m_commentEditingPage, m_commentEditingID);
    const QString text = m_commentEditor->toPlainText();
    m_commentEditor->removeEventFilter(this);
    m_commentEditor->deleteLater();
    m_commentEditor = nullptr;
    if (before && before->contents != text) {
        PdfAnnotationData after = *before;
        after.contents = text;
        m_undo.push(new EditPdfAnnotationCommand(&m_document, *before, after));
    }
    m_commentEditingID.clear();
    m_commentEditingPage = -1;
    viewport()->update();
}

void PdfPagesArea::showAnnotationMenu(int page, const QString &id, const QPoint &globalPos)
{
    const auto data = findByID(page, id);
    if (!data)
        return;
    QMenu menu(this);
    static const QList<QColor> kPalette = {QColor(0xFF, 0xEB, 0x3B), QColor(0x8B, 0xC3, 0x4A),
                                           QColor(0x4F, 0xC3, 0xF7), QColor(0xFF, 0x8A, 0x80),
                                           QColor(0xCE, 0x93, 0xD8)};
    for (const QColor &color : kPalette) {
        QPixmap swatch(16, 16);
        swatch.fill(Qt::transparent);
        QPainter p(&swatch);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(swatch.rect().adjusted(1, 1, -1, -1));
        p.end();
        QAction *action = menu.addAction(QIcon(swatch), tr("Colour"));
        connect(action, &QAction::triggered, this, [this, page, id, color] {
            const auto current = findByID(page, id);
            if (!current)
                return;
            PdfAnnotationData after = *current;
            after.color = color;
            m_undo.push(new EditPdfAnnotationCommand(&m_document, *current, after));
            m_color = color;
            viewport()->update();
        });
    }
    menu.addSeparator();
    QAction *editAction = menu.addAction(tr("Edit comment..."));
    connect(editAction, &QAction::triggered, this, [this, page, id] {
        const auto current = findByID(page, id);
        if (current)
            openCommentEditor(page, id, normalizedToLayoutRect(page, current->boundary));
    });
    menu.addSeparator();
    QAction *deleteAction = menu.addAction(tr("Delete"));
    connect(deleteAction, &QAction::triggered, this, &PdfPagesArea::deleteSelectedAnnotation);
    menu.exec(globalPos);
}

void PdfPagesArea::deleteSelectedAnnotation()
{
    if (m_selectedID.isEmpty())
        return;
    const auto data = findByID(m_selectedPage, m_selectedID);
    if (!data)
        return;
    m_undo.push(new RemovePdfAnnotationCommand(&m_document, *data));
    m_selectedID.clear();
    m_selectedPage = -1;
    emit selectionChanged();
    viewport()->update();
}

// --- tools -----------------------------------------------------------------

void PdfPagesArea::setTool(Tool tool)
{
    if (m_tool == tool)
        return;
    m_tool = tool;
    m_selectedID.clear();
    m_selectedPage = -1;
    m_selectionDragPts = QRectF();
    m_selectionText.clear();
    m_selectionPage = -1;
    emit selectionChanged();
    viewport()->update();
}

void PdfPagesArea::setAnnotationColor(const QColor &color)
{
    m_color = color;
    if (!m_selectedID.isEmpty()) {
        const auto data = findByID(m_selectedPage, m_selectedID);
        if (data) {
            PdfAnnotationData after = *data;
            after.color = color;
            m_undo.push(new EditPdfAnnotationCommand(&m_document, *data, after));
            viewport()->update();
        }
    }
}

// --- find --------------------------------------------------------------

void PdfPagesArea::findStart(const QString &needle)
{
    m_findQuery = needle;
    m_findMatches.clear();
    m_findCurrent = -1;
    if (needle.isEmpty()) {
        emit findStatusChanged(-1, 0);
        viewport()->update();
        return;
    }
    const quint64 generation = ++m_findGeneration;
    const QString path = m_document.path();
    const int pages = pageCount();
    (void)QtConcurrent::run([this, lifeline = m_lifeline, generation, path, pages, needle] {
        QList<QPair<int, QRectF>> matches = PdfDocument::findAcrossDocument(path, needle, pages);
        lifeline->post(this, [this, generation, matches = std::move(matches)]() mutable {
            deliverFindResults(generation, std::move(matches));
        });
    });
}

void PdfPagesArea::deliverFindResults(quint64 generation, QList<QPair<int, QRectF>> matches)
{
    if (generation != m_findGeneration)
        return;
    m_findMatches = std::move(matches);
    m_findCurrent = m_findMatches.isEmpty() ? -1 : 0;
    emit findStatusChanged(m_findCurrent, m_findMatches.size());
    if (m_findCurrent >= 0)
        revealFindCurrent();
    viewport()->update();
}

void PdfPagesArea::findStep(int direction)
{
    if (m_findMatches.isEmpty())
        return;
    m_findCurrent = (m_findCurrent + direction + m_findMatches.size()) % m_findMatches.size();
    emit findStatusChanged(m_findCurrent, m_findMatches.size());
    revealFindCurrent();
    viewport()->update();
}

void PdfPagesArea::findClear()
{
    m_findQuery.clear();
    m_findMatches.clear();
    m_findCurrent = -1;
    ++m_findGeneration;
}

void PdfPagesArea::revealFindCurrent()
{
    if (m_findCurrent < 0 || m_findCurrent >= m_findMatches.size())
        return;
    const auto &match = m_findMatches.at(m_findCurrent);
    const QSizeF sizePts = m_pageSizePoints.value(match.first);
    if (sizePts.isEmpty() || sizePts.height() <= 0)
        return;
    goToPage(match.first, match.second.y() / sizePts.height());
}

} // namespace noteAppC
