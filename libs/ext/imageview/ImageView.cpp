// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "ImageView.h"

#include <QAction>
#include <QActionGroup>
#include <QFileInfo>
#include <QHelpEvent>
#include "ImageLoad.h"
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QUndoCommand>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace noteAppC {

namespace {

constexpr double kMinZoom = 0.05;
constexpr double kMaxZoom = 16.0;
constexpr double kMinBox = 0.004; ///< smaller than this a drag is a click, not a box

QIcon colorSwatchIcon(const QColor &color)
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(pixmap.rect().adjusted(1, 1, -1, -1));
    return QIcon(pixmap);
}

double distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double length2 = ab.x() * ab.x() + ab.y() * ab.y();
    const double t = length2 == 0 ? 0 : qBound(0.0, QPointF::dotProduct(p - a, ab) / length2, 1.0);
    const QPointF d = p - (a + ab * t);
    return std::hypot(d.x(), d.y());
}

} // namespace

/// One undo step: the marks before and after. The edit has already happened
/// when it is pushed, so the first redo() has nothing to do.
class SnapshotCommand : public QUndoCommand {
public:
    SnapshotCommand(ImageCanvas *canvas, QList<ImageAnnotation> before, QList<ImageAnnotation> after)
        : m_canvas(canvas)
        , m_before(std::move(before))
        , m_after(std::move(after))
    {
    }
    void undo() override { m_canvas->apply(m_before); }
    void redo() override
    {
        if (m_applied)
            m_canvas->apply(m_after);
        m_applied = true;
    }

private:
    ImageCanvas *m_canvas;
    QList<ImageAnnotation> m_before;
    QList<ImageAnnotation> m_after;
    bool m_applied = false;
};

// --- ImageCanvas -------------------------------------------------------

ImageCanvas::ImageCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

bool ImageCanvas::setImage(const QImage &image)
{
    if (image.isNull())
        return false;
    m_image = image;
    m_undo.clear();
    setZoom(m_zoom);
    return true;
}

void ImageCanvas::resetAnnotations(const QList<ImageAnnotation> &annotations)
{
    closeEditor(false);
    m_annotations = annotations;
    m_selected = -1;
    m_undo.clear();
    update();
}

void ImageCanvas::apply(const QList<ImageAnnotation> &annotations)
{
    closeEditor(false);
    m_annotations = annotations;
    if (m_selected >= m_annotations.size())
        m_selected = -1;
    update();
    emit annotationsChanged();
}

void ImageCanvas::push(const QList<ImageAnnotation> &after)
{
    const QList<ImageAnnotation> before = m_annotations;
    m_annotations = after;
    m_undo.push(new SnapshotCommand(this, before, after));
    update();
    emit annotationsChanged();
}

int ImageCanvas::add(const ImageAnnotation &annotation)
{
    QList<ImageAnnotation> after = m_annotations;
    after.append(annotation);
    push(after);
    select(int(after.size()) - 1);
    return int(after.size()) - 1;
}

void ImageCanvas::remove(int index)
{
    if (index < 0 || index >= m_annotations.size())
        return;
    closeEditor(false);
    QList<ImageAnnotation> after = m_annotations;
    after.removeAt(index);
    select(-1);
    push(after);
}

void ImageCanvas::removeSelected()
{
    remove(m_selected);
}

void ImageCanvas::setText(int index, const QString &text)
{
    if (index < 0 || index >= m_annotations.size() || m_annotations.at(index).text == text)
        return;
    QList<ImageAnnotation> after = m_annotations;
    after[index].text = text;
    push(after);
}

void ImageCanvas::select(int index)
{
    if (index == m_selected)
        return;
    m_selected = index;
    update();
    emit selectionChanged();
}

void ImageCanvas::setTool(Tool tool)
{
    commitEditor();
    m_tool = tool;
    setCursor(tool == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
}

void ImageCanvas::setColor(const QColor &color)
{
    m_color = color;
    // A chosen mark follows the colour, the way recolouring a selection does elsewhere.
    if (m_selected >= 0 && m_annotations.at(m_selected).color != color) {
        QList<ImageAnnotation> after = m_annotations;
        after[m_selected].color = color;
        push(after);
    }
}

void ImageCanvas::setZoom(double zoom)
{
    m_zoom = qBound(kMinZoom, zoom, kMaxZoom);
    if (m_image.isNull())
        return;
    const QSize size(qMax(1, qRound(m_image.width() * m_zoom)), qMax(1, qRound(m_image.height() * m_zoom)));
    // Shrinking smooth-scales once here, not on every repaint of a drag.
    m_scaled = m_zoom < 1.0 ? m_image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation) : QImage();
    setFixedSize(size);
    update();
    emit zoomChanged(m_zoom);
}

void ImageCanvas::fit(const QSize &available)
{
    if (m_image.isNull() || available.isEmpty())
        return;
    const double zoom = std::min({double(available.width()) / m_image.width(),
                                 double(available.height()) / m_image.height(), 1.0});
    if (std::abs(zoom - m_zoom) > 1e-6 || size() != QSize(qRound(m_image.width() * zoom), qRound(m_image.height() * zoom)))
        setZoom(zoom);
}

void ImageCanvas::setTheme(const Theme &theme)
{
    m_theme = theme;
    update();
}

QPointF ImageCanvas::toWidget(const QPointF &n) const
{
    return QPointF(n.x() * width(), n.y() * height());
}

QPointF ImageCanvas::toNormalized(const QPointF &p) const
{
    if (width() <= 0 || height() <= 0)
        return {};
    return QPointF(qBound(0.0, p.x() / width(), 1.0), qBound(0.0, p.y() / height(), 1.0));
}

int ImageCanvas::hit(const QPointF &position) const
{
    for (int i = int(m_annotations.size()) - 1; i >= 0; --i) {
        const ImageAnnotation &a = m_annotations.at(i);
        switch (a.kind) {
        case ImageAnnotation::Kind::Highlight: {
            const QRectF r(toWidget(a.rect.topLeft()), toWidget(a.rect.bottomRight()));
            if (r.adjusted(-3, -3, 3, 3).contains(position))
                return i;
            break;
        }
        case ImageAnnotation::Kind::Ink:
            for (qsizetype k = 0; k + 1 < a.stroke.size(); ++k) {
                if (distanceToSegment(position, toWidget(a.stroke.at(k)), toWidget(a.stroke.at(k + 1))) < 6)
                    return i;
            }
            break;
        case ImageAnnotation::Kind::Note: {
            const QPointF d = position - toWidget(a.point);
            if (std::hypot(d.x(), d.y()) < 11)
                return i;
            break;
        }
        }
    }
    return -1;
}

void ImageCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), m_theme.color(QStringLiteral("--background-secondary")));
    if (!m_scaled.isNull())
        p.drawImage(QPointF(0, 0), m_scaled);
    else
        p.drawImage(QRectF(rect()), m_image);

    const QColor accent = m_theme.color(QStringLiteral("--text-accent"));
    int noteNumber = 0;
    for (int i = 0; i < m_annotations.size(); ++i) {
        const ImageAnnotation &a = m_annotations.at(i);
        QColor fill = a.color;
        switch (a.kind) {
        case ImageAnnotation::Kind::Highlight: {
            const QRectF r(toWidget(a.rect.topLeft()), toWidget(a.rect.bottomRight()));
            fill.setAlpha(90);
            p.setPen(QPen(a.color, 1.5));
            p.setBrush(fill);
            p.drawRect(r);
            break;
        }
        case ImageAnnotation::Kind::Ink: {
            QPolygonF line;
            for (const QPointF &n : a.stroke)
                line << toWidget(n);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(a.color, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(line);
            break;
        }
        case ImageAnnotation::Kind::Note: {
            const QPointF c = toWidget(a.point);
            p.setPen(QPen(QColor(0x20, 0x20, 0x20), 1.5));
            p.setBrush(a.color);
            p.drawEllipse(c, 9, 9);
            QFont font = p.font();
            font.setBold(true);
            font.setPixelSize(11);
            p.setFont(font);
            p.drawText(QRectF(c.x() - 9, c.y() - 9, 18, 18), Qt::AlignCenter, QString::number(++noteNumber));
            break;
        }
        }
        if (i == m_selected) {
            QRectF b;
            if (a.kind == ImageAnnotation::Kind::Note)
                b = QRectF(toWidget(a.point) - QPointF(12, 12), QSizeF(24, 24));
            else
                b = QRectF(toWidget(a.bounds().topLeft()), toWidget(a.bounds().bottomRight())).adjusted(-4, -4, 4, 4);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(accent, 1.5, Qt::DashLine));
            p.drawRect(b);
        }
    }

    // What is being dragged out right now.
    if (m_drawing && m_tool == Tool::Highlight && !m_dragRect.isNull()) {
        QColor fill = m_color;
        fill.setAlpha(90);
        p.setPen(QPen(m_color, 1.5, Qt::DashLine));
        p.setBrush(fill);
        p.drawRect(QRectF(toWidget(m_dragRect.topLeft()), toWidget(m_dragRect.bottomRight())));
    } else if (m_drawing && m_tool == Tool::Draw && m_dragStroke.size() > 1) {
        QPolygonF line;
        for (const QPointF &n : m_dragStroke)
            line << toWidget(n);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(m_color, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(line);
    }
}

void ImageCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    commitEditor();
    setFocus();
    const QPointF n = toNormalized(event->position());
    switch (m_tool) {
    case Tool::Select:
        select(hit(event->position()));
        break;
    case Tool::Highlight:
        m_drawing = true;
        m_dragStart = n;
        m_dragRect = QRectF();
        break;
    case Tool::Draw:
        m_drawing = true;
        m_dragStroke = QPolygonF() << n;
        break;
    case Tool::Note: {
        ImageAnnotation note;
        note.kind = ImageAnnotation::Kind::Note;
        note.color = m_color;
        note.point = n;
        editText(add(note));
        break;
    }
    }
}

void ImageCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_drawing)
        return;
    const QPointF n = toNormalized(event->position());
    if (m_tool == Tool::Highlight)
        m_dragRect = QRectF(m_dragStart, n).normalized();
    else if (m_tool == Tool::Draw)
        m_dragStroke << n;
    update();
}

void ImageCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_drawing || event->button() != Qt::LeftButton)
        return;
    m_drawing = false;
    ImageAnnotation a;
    a.color = m_color;
    bool keep = false;
    if (m_tool == Tool::Highlight && m_dragRect.width() > kMinBox && m_dragRect.height() > kMinBox) {
        a.kind = ImageAnnotation::Kind::Highlight;
        a.rect = m_dragRect;
        keep = true;
    } else if (m_tool == Tool::Draw && m_dragStroke.size() >= 2
               && (m_dragStroke.boundingRect().width() > kMinBox || m_dragStroke.boundingRect().height() > kMinBox)) {
        a.kind = ImageAnnotation::Kind::Ink;
        a.stroke = m_dragStroke;
        keep = true;
    }
    m_dragRect = QRectF();
    m_dragStroke.clear();
    if (keep)
        add(a);
    update();
}

void ImageCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_tool == Tool::Select) {
        const int index = hit(event->position());
        select(index);
        if (index >= 0)
            editText(index);
    }
}

void ImageCanvas::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && m_selected >= 0) {
        removeSelected();
        event->accept();
    } else if (event->key() == Qt::Key_Escape) {
        select(-1);
        event->accept();
    } else {
        QWidget::keyPressEvent(event);
    }
}

void ImageCanvas::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        emit zoomRequested(event->angleDelta().y() > 0 ? 1 : -1);
        event->accept();
    } else {
        event->ignore(); // the scroll area scrolls
    }
}

bool ImageCanvas::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int index = hit(help->pos());
        if (index >= 0 && !m_annotations.at(index).text.isEmpty())
            QToolTip::showText(help->globalPos(), m_annotations.at(index).text, this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}

void ImageCanvas::editText(int index)
{
    closeEditor(true);
    if (index < 0 || index >= m_annotations.size())
        return;
    const ImageAnnotation &a = m_annotations.at(index);
    m_editing = index;
    m_editor = new QPlainTextEdit(this);
    m_editor->setPlainText(a.text);
    m_editor->setPlaceholderText(tr("Note..."));
    m_editor->installEventFilter(this);
    QPalette palette = m_editor->palette();
    palette.setColor(QPalette::Base, m_theme.color(QStringLiteral("--background-primary")));
    palette.setColor(QPalette::Text, m_theme.color(QStringLiteral("--text-normal")));
    m_editor->setPalette(palette);
    const QRectF bounds(toWidget(a.bounds().topLeft()), toWidget(a.bounds().bottomRight()));
    // Under the mark, inside the picture when it fits.
    const QPoint where(qBound(0, int(bounds.left()), qMax(0, width() - 220)),
                       qBound(0, int(bounds.bottom()) + 12, qMax(0, height() - 90)));
    m_editor->setGeometry(QRect(where, QSize(220, 90)));
    m_editor->show();
    m_editor->setFocus();
    m_editor->selectAll();
}

void ImageCanvas::commitEditor()
{
    closeEditor(true);
}

void ImageCanvas::closeEditor(bool commit)
{
    if (!m_editor)
        return;
    QPlainTextEdit *editor = m_editor;
    const int index = m_editing;
    const QString text = editor->toPlainText();
    m_editor = nullptr;
    m_editing = -1;
    editor->removeEventFilter(this);
    editor->hide();
    editor->deleteLater();
    if (commit)
        setText(index, text);
}

bool ImageCanvas::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_editor) {
        if (event->type() == QEvent::FocusOut) {
            closeEditor(true);
        } else if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Escape) {
                closeEditor(false);
                setFocus();
                return true;
            }
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
                && (key->modifiers() & Qt::ControlModifier)) {
                closeEditor(true);
                setFocus();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

// --- ImageView ---------------------------------------------------------

ImageView::ImageView(QWidget *parent)
    : QWidget(parent)
{
    m_canvas = new ImageCanvas;
    m_scroll = new QScrollArea(this);
    m_scroll->setWidget(m_canvas);
    m_scroll->setAlignment(Qt::AlignCenter);
    m_scroll->setFrameShape(QFrame::NoFrame);
    connect(m_canvas, &ImageCanvas::annotationsChanged, this, &ImageView::saveSidecar);
    connect(m_canvas, &ImageCanvas::zoomChanged, this, [this](double) { refreshZoomLabel(); });
    connect(m_canvas, &ImageCanvas::zoomRequested, this, [this](int steps) {
        m_fit = false;
        m_canvas->setZoom(m_canvas->zoom() * std::pow(1.1, steps));
    });
    buildToolbar();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_toolbar);
    layout->addWidget(m_scroll, 1);
    setFocusProxy(m_canvas);
}

void ImageView::buildToolbar()
{
    m_toolbar = new QToolBar(this);
    m_toolbar->setMovable(false);
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_toolbar->setIconSize(QSize(16, 16));

    auto *tools = new QActionGroup(this);
    tools->setExclusive(true);
    const auto addTool = [&](const QString &text, ImageCanvas::Tool tool) {
        QAction *action = m_toolbar->addAction(text);
        action->setCheckable(true);
        action->setActionGroup(tools);
        connect(action, &QAction::triggered, this, [this, tool] { m_canvas->setTool(tool); });
        return action;
    };
    addTool(tr("Select"), ImageCanvas::Tool::Select)->setChecked(true);
    addTool(tr("Highlight"), ImageCanvas::Tool::Highlight)->setToolTip(tr("Drag a box"));
    addTool(tr("Draw"), ImageCanvas::Tool::Draw)->setToolTip(tr("Draw freehand"));
    addTool(tr("Note"), ImageCanvas::Tool::Note)->setToolTip(tr("Click to place a note"));

    auto *colorButton = new QToolButton(m_toolbar);
    colorButton->setText(tr("Colour"));
    colorButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    colorButton->setIcon(colorSwatchIcon(m_canvas->color()));
    colorButton->setPopupMode(QToolButton::InstantPopup);
    auto *colorMenu = new QMenu(colorButton);
    for (const QColor &color : ImageAnnotations::palette()) {
        QAction *action = colorMenu->addAction(colorSwatchIcon(color), color.name());
        connect(action, &QAction::triggered, this, [this, color, colorButton] {
            m_canvas->setColor(color);
            colorButton->setIcon(colorSwatchIcon(color));
        });
    }
    colorButton->setMenu(colorMenu);
    m_toolbar->addWidget(colorButton);
    m_toolbar->addSeparator();

    QAction *remove = m_toolbar->addAction(tr("Delete"), this, [this] { m_canvas->removeSelected(); });
    remove->setEnabled(false);
    connect(m_canvas, &ImageCanvas::selectionChanged, remove, [this, remove] { remove->setEnabled(m_canvas->selected() >= 0); });
    QAction *undo = m_canvas->undoStack()->createUndoAction(this, tr("Undo"));
    undo->setShortcut(QKeySequence::Undo);
    undo->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    QAction *redo = m_canvas->undoStack()->createRedoAction(this, tr("Redo"));
    redo->setShortcut(QKeySequence::Redo);
    redo->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    m_toolbar->addAction(undo);
    m_toolbar->addAction(redo);
    m_toolbar->addSeparator();

    m_toolbar->addAction(tr("−"), this, [this] {
        m_fit = false;
        m_canvas->setZoom(m_canvas->zoom() / 1.25);
    })->setToolTip(tr("Zoom out"));
    m_zoomLabel = new QLabel(tr("100%"), m_toolbar);
    m_zoomLabel->setFixedWidth(44);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_toolbar->addWidget(m_zoomLabel);
    m_toolbar->addAction(tr("+"), this, [this] {
        m_fit = false;
        m_canvas->setZoom(m_canvas->zoom() * 1.25);
    })->setToolTip(tr("Zoom in"));
    m_toolbar->addAction(tr("Fit"), this, [this] {
        m_fit = true;
        m_canvas->fit(m_scroll->viewport()->size());
    });
    m_toolbar->addAction(tr("100%"), this, [this] {
        m_fit = false;
        m_canvas->setZoom(1.0);
    });
    m_toolbar->addSeparator();
    m_toolbar->addAction(tr("To note"), this, [this] {
        QString error;
        if (extractAnnotations(QString(), &error))
            emit statusMessage(tr("Annotations extracted to %1").arg(QFileInfo(sidecarPath()).fileName()), 3000);
        else
            emit statusMessage(error, 5000);
    })->setToolTip(tr("Extract the annotations to a note beside the image"));

    addAction(undo);
    addAction(redo);
}

bool ImageView::load(const QString &path, QString *error)
{
    const QImage image = readImage(path, 0, true);
    if (image.isNull()) {
        if (error)
            *error = tr("%1 is not a picture that can be read").arg(QFileInfo(path).fileName());
        return false;
    }
    m_path = path;
    m_canvas->setImage(image);
    m_canvas->resetAnnotations(ImageAnnotations::load(sidecarPath()));
    m_canvas->fit(m_scroll->viewport()->size());
    refreshZoomLabel();
    return true;
}

QString ImageView::sidecarPath() const
{
    return ImageAnnotations::sidecarPath(m_path);
}

void ImageView::setTheme(const Theme &theme)
{
    m_theme = theme;
    m_canvas->setTheme(theme);
}

void ImageView::saveSidecar()
{
    QString error;
    if (!ImageAnnotations::save(sidecarPath(), QFileInfo(m_path).fileName(), m_canvas->annotations(), &error))
        emit statusMessage(tr("Could not save the annotations: %1").arg(error), 5000);
}

bool ImageView::extractAnnotations(const QString &notePath, QString *error)
{
    if (m_path.isEmpty() || m_canvas->annotations().isEmpty()) {
        if (error)
            *error = tr("No annotations to extract");
        return false;
    }
    m_canvas->commitEditor();
    return ImageAnnotations::save(notePath.isEmpty() ? sidecarPath() : notePath, QFileInfo(m_path).fileName(),
                                  m_canvas->annotations(), error);
}

void ImageView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_fit)
        m_canvas->fit(m_scroll->viewport()->size());
}

void ImageView::refreshZoomLabel()
{
    m_zoomLabel->setText(tr("%1%").arg(qRound(m_canvas->zoom() * 100)));
}

} // namespace noteAppC
