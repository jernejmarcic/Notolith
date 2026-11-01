// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QAbstractScrollArea>
#include <QColor>
#include <QHash>
#include <QPixmap>
#include <QPolygonF>
#include <QUndoStack>

#include <memory>
#include <optional>

#include "PdfDocument.h"
#include "Theme.h"

class QPlainTextEdit;

namespace noteAppC {

class Lifeline;

/// The page canvas: continuous vertical scroll, zoom, navigation, text
/// selection, find highlighting, link following, and every annotation tool.
/// Everything a PdfView tab needs beyond the toolbar chrome and the outline
/// panel, which PdfView.cpp composes around this.
///
/// Page rasters are cached by (path, mtime, page, zoom bucket) and rendered
/// off the UI thread through PdfPageImpl - the same async/placeholder/Lifeline
/// shape libs/editor/PdfEmbedBlock.cpp already uses for embedded PDF pages -
/// with poppler's own baked-in annotation drawing turned off
/// (PdfPageImpl::render's hideAnnotations): every annotation, existing or just
/// added, is drawn here instead, straight off PdfDocument, so an edit is
/// visible the instant it happens rather than waiting on a re-render.
class PdfPagesArea : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit PdfPagesArea(QWidget *parent = nullptr);
    ~PdfPagesArea() override;

    bool load(const QString &path, QString *error = nullptr);
    bool save(QString *error = nullptr);
    /// The undo stack's clean flag, not PdfDocument's own sticky one (set by
    /// any edit, never cleared by an undo back to the saved state) - matches
    /// CanvasView's convention (`!m_undoStack.isClean()`), which is what
    /// isViewFile()/openView()'s isModified() dispatch expects.
    bool isModified() const { return !m_undo.isClean(); }
    QString path() const { return m_document.path(); }
    /// A rename or move: saves go to `path`, and the pages already drawn stay (the cache is keyed by path).
    void setPath(const QString &path);
    PdfDocument *document() { return &m_document; }

    QUndoStack *undoStack() { return &m_undo; }

    void setTheme(const Theme &theme);

    enum class Tool { Select, Highlight, Underline, StrikeOut, Ink, Note, Rectangle };
    void setTool(Tool tool);
    Tool tool() const { return m_tool; }
    void setAnnotationColor(const QColor &color); ///< recolours the selection if any, else sets the default
    QColor annotationColor() const { return m_color; }
    void setInkWidth(double points) { m_inkWidth = points; }

    int pageCount() const { return m_document.pageCount(); }
    int currentPage() const { return m_currentPage; }
    /// `top` is normalized (0..1) within the page - what a LinkDestination
    /// and an outline item both give.
    void goToPage(int page, double top = 0.0);
    void nextPage() { goToPage(m_currentPage + 1); }
    void previousPage() { goToPage(m_currentPage - 1); }

    double zoom() const { return m_zoom; }
    void setZoom(double zoom);
    void zoomIn() { setZoom(m_zoom * 1.1); }
    void zoomOut() { setZoom(m_zoom / 1.1); }
    void fitWidth();
    void fitPage();
    static constexpr double kMinZoom = 0.25;
    static constexpr double kMaxZoom = 4.0;

    QList<PdfOutlineItem> outline() const { return m_document.outline(); }

    /// A page's rect in layout pixels (unscrolled), from its point size and
    /// the current zoom alone - true the instant load() returns, before any
    /// raster has landed. Public mainly for tests to check that: see
    /// tests/test_pdfview.cpp's PagesLayOutWithRightSizesBeforeAnyRenderLands.
    QRectF pageLayoutRect(int page) const { return pageRectInLayout(page); }

    QString selectedText() const { return m_selectionText; }

    void findStart(const QString &needle);
    void findStep(int direction); ///< +1 next, -1 previous
    void findClear();
    int findMatchCount() const { return m_findMatches.size(); }
    int findCurrentIndex() const { return m_findCurrent; }

signals:
    void modificationChanged(bool modified);
    void currentPageChanged(int page);
    void zoomChanged(double zoom);
    void openExternalUrl(const QString &url);
    void findStatusChanged(int current, int total);
    /// Selecting/deselecting an annotation, or changing tool - PdfView's
    /// toolbar reflects it (e.g. greying out colour swatches with nothing
    /// selected and no drawing tool active).
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override; ///< the comment popup: Escape/Enter/focus-out

private:
    // --- PdfPagesArea.cpp: layout, rendering, painting ---------------------
    double pixelsPerPoint() const { return m_zoom * kBaseDpi / 72.0; }
    void relayout();
    void updateScrollRange();
    QRectF pageRectInLayout(int page) const; ///< device-independent pixels, unscrolled
    int pageAt(const QPointF &layoutPoint) const;
    /// layoutPoint's position within that page, in PDF points.
    QPointF toPagePoint(int page, const QPointF &layoutPoint) const;
    QPointF toLayoutPoint(int page, const QPointF &pagePoint) const; ///< inverse
    QRectF toLayoutRect(int page, const QRectF &pageRectPoints) const;
    /// Same, but for a PdfAnnotationData::boundary - normalized 0..1, not
    /// points - converted through the page's own point size first. A popup
    /// positioned straight off toLayoutRect(page, data.boundary) landed at
    /// the page's top-left corner instead of the annotation, caught by
    /// screenshotting the note tool while building this.
    QRectF normalizedToLayoutRect(int page, const QRectF &normalized) const;
    QPointF viewportToLayout(const QPoint &viewportPoint) const;
    void visiblePageRange(int &first, int &last) const;
    void scheduleVisibleRenders();
    void recomputeCurrentPage();

    QPixmap ensureRasterAsync(int page, int widthPixels);
    void deliverRaster(const QString &key, quint64 generation, QPixmap pixmap);

    void paintPage(QPainter &painter, int page, const QRectF &screenRect);
    void paintAnnotation(QPainter &painter, const PdfAnnotationData &data, const QRectF &pageScreenRect,
                         bool selected) const;

    // --- PdfPagesArea.cpp: interaction, annotation tools -------------------
    std::optional<PdfAnnotationData> annotationAt(int page, const QPointF &pagePointPts) const;
    std::optional<PdfAnnotationData> findByID(int page, const QString &id) const;
    /// Word boxes (poppler's own) intersecting `dragRectPts`, grouped into
    /// one rect per text row - used both for a plain text selection and for
    /// building a highlight/underline/strike-out's quads.
    QList<QRectF> selectionRows(int page, const QRectF &dragRectPts) const;
    void commitTextMarkup(int page, const QList<QRectF> &rowsPts);
    void beginMoveOrSelect(int page, const QPointF &pagePointPts, const QPoint &viewportPoint);
    void finishMove();
    void openCommentEditor(int page, const QString &id, const QRectF &boundaryLayoutRect);
    void commitCommentEditor();
    void showAnnotationMenu(int page, const QString &id, const QPoint &globalPos);
    void deleteSelectedAnnotation();

    void followInteractionAt(int page, const QPointF &pagePointPts);

    // --- PdfPagesArea.cpp: find --------------------------------------------
    void deliverFindResults(quint64 generation, QList<QPair<int, QRectF>> matches);
    void revealFindCurrent();

    PdfDocument m_document;
    QUndoStack m_undo;
    Theme m_theme = Theme::builtin(Theme::Mode::Dark);
    std::shared_ptr<Lifeline> m_lifeline;

    QList<QSizeF> m_pageSizePoints; ///< cached at load, points
    QList<QRectF> m_pageLayout;     ///< layout-space pixel rects, unscrolled
    double m_layoutContentWidth = 0;
    double m_layoutTotalHeight = 0;
    double m_zoom = 1.0;
    static constexpr double kBaseDpi = 96.0; ///< zoom 1.0 = 96 px/inch, a normal screen reading size
    static constexpr double kPageGap = 14.0;

    int m_currentPage = 0;

    struct RasterEntry {
        QPixmap image;
        bool pending = false;
        quint64 generation = 0;
    };
    QHash<QString, RasterEntry> m_rasterCache;
    quint64 m_rasterGeneration = 0;

    Tool m_tool = Tool::Select;
    QColor m_color = QColor(0xFF, 0xEB, 0x3B);
    double m_inkWidth = 2.0;

    // Selection (Select tool: existing annotation, or a plain text range).
    QString m_selectedID;
    int m_selectedPage = -1;
    QString m_selectionText;
    int m_selectionPage = -1;
    QRectF m_selectionDragPts; ///< page points, while dragging

    // A gesture in progress.
    bool m_dragging = false;
    QPoint m_dragStartViewport;
    int m_dragPage = -1;
    QPointF m_dragStartPagePts;
    QPointF m_dragCurrentPagePts;
    bool m_movingAnnotation = false;
    PdfAnnotationData m_moveBefore;
    QPointF m_moveOffsetNormalized;
    QPolygonF m_liveInkStroke; ///< page points, while drawing

    QPlainTextEdit *m_commentEditor = nullptr;
    QString m_commentEditingID;
    int m_commentEditingPage = -1;

    QString m_findQuery;
    QList<QPair<int, QRectF>> m_findMatches; ///< (page, point-space rect)
    int m_findCurrent = -1;
    quint64 m_findGeneration = 0;
};

} // namespace noteAppC
