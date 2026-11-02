// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QUndoStack>
#include <QWidget>

#include "ImageAnnotations.h"
#include "Theme.h"

class QAction;
class QLabel;
class QPlainTextEdit;
class QScrollArea;
class QToolBar;

namespace noteAppC {

/// The picture and its marks: draws the image at a zoom, the annotations over
/// it, and turns the pointer into highlight boxes, freehand strokes and
/// notes. Every change goes through the undo stack as a before/after
/// snapshot (a list of a few marks is cheaper to copy than a command class
/// per edit is to read) and is announced by annotationsChanged().
class ImageCanvas : public QWidget {
    Q_OBJECT

public:
    enum class Tool { Select, Highlight, Draw, Note };

    explicit ImageCanvas(QWidget *parent = nullptr);

    bool setImage(const QImage &image);
    const QImage &image() const { return m_image; }

    const QList<ImageAnnotation> &annotations() const { return m_annotations; }
    /// Replaces the marks without an undo step - a load, not an edit.
    void resetAnnotations(const QList<ImageAnnotation> &annotations);

    /// Each is one undo step. `add` returns the new mark's index.
    int add(const ImageAnnotation &annotation);
    void remove(int index);
    void setText(int index, const QString &text);

    int selected() const { return m_selected; }
    void select(int index);
    void removeSelected();

    void setTool(Tool tool);
    Tool tool() const { return m_tool; }
    void setColor(const QColor &color);
    QColor color() const { return m_color; }

    /// 1.0 is one image pixel per device-independent pixel; fit shows it all.
    double zoom() const { return m_zoom; }
    void setZoom(double zoom);
    void fit(const QSize &available);

    void setTheme(const Theme &theme);
    QUndoStack *undoStack() { return &m_undo; }

    /// Where a normalized point lands in this widget, and back.
    QPointF toWidget(const QPointF &normalized) const;
    QPointF toNormalized(const QPointF &position) const;
    /// The topmost mark under `position` (widget coordinates), or -1.
    int hit(const QPointF &position) const;

    /// Opens the inline text editor on a mark.
    void editText(int index);
    void commitEditor();

signals:
    void annotationsChanged();
    void zoomChanged(double zoom);
    void selectionChanged();
    /// Ctrl+wheel: positive steps zoom in.
    void zoomRequested(int steps);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    friend class SnapshotCommand;
    void apply(const QList<ImageAnnotation> &annotations); ///< undo/redo target
    void push(const QList<ImageAnnotation> &after);
    void closeEditor(bool commit);

    QImage m_image;
    QImage m_scaled; ///< m_image at the current size, when shrunk
    QList<ImageAnnotation> m_annotations;
    QUndoStack m_undo;
    Tool m_tool = Tool::Select;
    QColor m_color = ImageAnnotations::palette().first();
    double m_zoom = 1.0;
    int m_selected = -1;

    bool m_drawing = false; ///< a box or stroke is being dragged out
    QPointF m_dragStart;
    QPolygonF m_dragStroke;
    QRectF m_dragRect;

    QPlainTextEdit *m_editor = nullptr;
    int m_editing = -1;
    Theme m_theme = Theme::builtin(Theme::Mode::Dark);
};

/// An image opened as a tab: toolbar (tools, colour, delete, undo, zoom,
/// extract), the canvas in a scroll area. The image is never written; the
/// marks are saved to the sidecar note beside it after every edit.
/// MainWindow::openView's image counterpart to PdfView.
class ImageView : public QWidget {
    Q_OBJECT

public:
    explicit ImageView(QWidget *parent = nullptr);

    bool load(const QString &path, QString *error = nullptr);
    QString path() const { return m_path; }
    /// A rename or move: the annotations' sidecar follows the new name from the next save.
    void setPath(const QString &path) { m_path = path; }
    QString sidecarPath() const;
    const QList<ImageAnnotation> &annotations() const { return m_canvas->annotations(); }
    ImageCanvas *canvas() const { return m_canvas; }

    void setTheme(const Theme &theme);

    /// "Extract annotations to a note": writes the readable section - the
    /// image, then each mark with its text - to `notePath`, by default the
    /// sidecar itself (which is already kept current, so this also
    /// recreates one the reader deleted). False with `error` on a write
    /// failure, or when there is nothing to extract.
    bool extractAnnotations(const QString &notePath = QString(), QString *error = nullptr);

signals:
    /// For the status bar, the same signal EpubView has.
    void statusMessage(const QString &text, int timeoutMs);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildToolbar();
    void saveSidecar();
    void refreshZoomLabel();

    ImageCanvas *m_canvas = nullptr;
    QScrollArea *m_scroll = nullptr;
    QToolBar *m_toolbar = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QString m_path;
    bool m_fit = true; ///< follows the window until the reader zooms
    Theme m_theme = Theme::builtin(Theme::Mode::Dark);
};

} // namespace noteAppC
