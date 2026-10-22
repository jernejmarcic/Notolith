#pragma once

#include <QImage>
#include <QWidget>

#include <map>

#include "InkDocument.h"
#include "InkInput.h"

namespace noteAppC::ink {

/// The drawing surface (spec section 7, Canvas): a plain QWidget sized to the
/// document at the zoom, living inside InkView's QScrollArea. Tablet events
/// do not route through QAbstractScrollArea::viewportEvent, which is why the
/// canvas is the widget and not the scroll area. Paints committed items from
/// the document (Track S1 Task 11 adds the tile cache) and the live stroke
/// from an overlay image that only the newest segment is rasterised into.
class InkCanvas : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Pen, Eraser, Lasso, Shape, Text };
    Q_ENUM(Tool)

    explicit InkCanvas(QWidget *parent = nullptr);

    InkDocument &document() { return m_document; }
    const InkDocument &document() const { return m_document; }
    void documentReplaced();

    double zoom() const { return m_zoom; }
    void setZoom(double zoom);
    Tool tool() const { return m_tool; }
    void setTool(Tool tool);
    StrokeStyle penStyle() const { return m_penStyle; }
    void setPenStyle(const StrokeStyle &style) { m_penStyle = style; }
    double eraserWidth() const { return m_eraserWidth; }
    void setEraserWidth(double documentPx) { m_eraserWidth = documentPx; }
    bool drawWithMouse() const { return m_drawWithMouse; }
    void setDrawWithMouse(bool on) { m_drawWithMouse = on; }
    void setRawInput(bool raw) { m_input.setRawInput(raw); }

    QPointF toDocument(const QPointF &widgetPos) const { return widgetPos / m_zoom; }
    QPointF toWidget(const QPointF &documentPos) const { return documentPos * m_zoom; }
    QSize sizeHint() const override;
    bool isDrawing() const { return m_drawing != Drawing::None; }
    /// A read-only canvas (a file written by a newer Notolith) ignores the pen,
    /// mouse drawing and the edit keys but keeps touch pan, wheel and zoom:
    /// disabling the widget would block those too.
    void setReadOnly(bool on) { m_readOnly = on; }
    bool isReadOnly() const { return m_readOnly; }

    /// R14: pressure is divided by a learned maximum per QPointingDevice::name()
    /// (PressureNormaliser, InkInput.h). Tablet pressure only: a mouse stroke stays 0.5.
    double learnedPressure(const QString &device) const;
    void setLearnedPressure(const QString &device, double max);
    void setPressureSeed(double seed) { m_pressureSeed = seed; } ///< for a device nothing is stored for

public slots:
    bool undo();
    bool redo();

signals:
    void documentEdited();
    void toolChanged(Tool tool);
    void learnedPressureChanged(const QString &device, double max);
    void zoomChanged(double zoom);
    void panRequested(QPoint deltaWidgetPx);

protected:
    void tabletEvent(QTabletEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    enum class Drawing { None, Pen, Eraser };
    void pressAt(const QPointF &widgetPos, double pressure, double seconds, bool erase);
    void moveTo(const QPointF &widgetPos, double pressure, double seconds);
    void releaseAt(const QPointF &widgetPos, double pressure, double seconds);
    void rasteriseNewSegments();
    void eraseAround(const QPointF &documentPos);
    void finishEdit();
    QRect widgetRect(const QRectF &documentRect) const;
    PressureNormaliser &normaliserFor(const QString &device);

    InkDocument m_document;
    InkInput m_input;
    Tool m_tool = Tool::Pen;
    StrokeStyle m_penStyle;
    double m_eraserWidth = 20.0;
    double m_zoom = 1.0;
    bool m_drawWithMouse = false;
    Drawing m_drawing = Drawing::None;
    bool m_tabletActive = false; ///< Qt synthesises mouse events from unaccepted tablet events; we accept, this guards the rest
    QImage m_live;               ///< the live stroke, widget px * dpr, transparent
    QRectF m_lastPrediction;     ///< document px: the predicted tail as last painted; repainted when it moves and at pen-up
    bool m_erasedSomething = false;
    bool m_readOnly = false;
    QPointF m_lastTouch;
    bool m_touchPanning = false;
    std::map<QString, PressureNormaliser> m_pressure; ///< by QPointingDevice::name()
    double m_pressureSeed = 1.0;
};

} // namespace noteAppC::ink
