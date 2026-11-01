// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// Painting: the dot grid, groups (behind everything), edges (curves with
// arrowheads and labels), nodes (chrome plus a cached content pixmap),
// and the selection/hover/drag overlay on top. Also the small geometry
// (side anchors, edge routing) that hit-testing in CanvasEditing.cpp reuses.

#include "CanvasView.h"
#include "Vault.h"

#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QImage>
#include "ImageLoad.h"
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>

#include <cmath>

#include "Icons.h"
#include "MarkdownEditor.h"
#include "Parser.h"

namespace noteAppC {

/// The part of a note a file card names with its subpath, as Obsidian shows
/// it: `#Heading` is that heading down to the next one at its level or
/// above, `#^id` the line carrying that block id. Headings are Markdown or
/// HTML (headingSection). The whole note when there is no subpath or it
/// names nothing.
QString canvasSectionOf(const QString &text, const QString &subpath)
{
    if (!subpath.startsWith(QLatin1Char('#')) || subpath.size() < 2)
        return text;
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (subpath.at(1) == QLatin1Char('^')) {
        const QString id = subpath.mid(1);
        for (const QString &line : lines) {
            if (line.trimmed().endsWith(id))
                return line.left(line.lastIndexOf(id)).trimmed();
        }
        return text;
    }
    const QString wanted = subpath.mid(1).section(QLatin1Char('#'), -1).trimmed(); // the last level
    const Range section = headingSection(text, wanted);
    if (!section.isValid())
        return text;
    QString part = text.mid(section.start, section.length());
    if (section.end < text.size() && part.endsWith(QLatin1Char('\n')))
        part.chop(1); // the newline before the next heading is not the section's
    return part;
}

namespace {
constexpr qreal kGridMinScreenSpacing = 16.0;
}

void CanvasView::paintGrid(QPainter &painter) const
{
    const qreal spacingScreen = kGridSpacingCanvas * m_zoom;
    if (spacingScreen < kGridMinScreenSpacing)
        return; // dots would be denser than they are useful, and slower to draw
    const QRectF visible = viewportCanvasRect();
    const qreal startX = std::floor(visible.left() / kGridSpacingCanvas) * kGridSpacingCanvas;
    const qreal startY = std::floor(visible.top() / kGridSpacingCanvas) * kGridSpacingCanvas;
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_theme.color(QStringLiteral("--background-modifier-border")));
    for (qreal cy = startY; cy <= visible.bottom(); cy += kGridSpacingCanvas) {
        for (qreal cx = startX; cx <= visible.right(); cx += kGridSpacingCanvas)
            painter.drawEllipse(mapFromCanvasF(QPointF(cx, cy)), 1.2, 1.2);
    }
}

bool CanvasView::isNodeVisible(const CanvasNode &node, QRectF *screenRect) const
{
    const QRectF r = currentRect(node);
    const QRectF screen(mapFromCanvasF(r.topLeft()), mapFromCanvasF(r.bottomRight()));
    if (screenRect)
        *screenRect = screen;
    return screen.intersects(QRectF(rect()));
}

QPointF CanvasView::sideAnchor(const QRectF &rect, const QString &side)
{
    if (side == QLatin1String("top")) return QPointF(rect.center().x(), rect.top());
    if (side == QLatin1String("bottom")) return QPointF(rect.center().x(), rect.bottom());
    if (side == QLatin1String("left")) return QPointF(rect.left(), rect.center().y());
    if (side == QLatin1String("right")) return QPointF(rect.right(), rect.center().y());
    return rect.center();
}

QPointF CanvasView::sideNormal(const QString &side)
{
    if (side == QLatin1String("top")) return QPointF(0, -1);
    if (side == QLatin1String("bottom")) return QPointF(0, 1);
    if (side == QLatin1String("left")) return QPointF(-1, 0);
    if (side == QLatin1String("right")) return QPointF(1, 0);
    return QPointF(0, 0);
}

QString CanvasView::effectiveSide(const QString &explicitSide, const QRectF &ownerRect, const QRectF &otherRect) const
{
    if (!explicitSide.isEmpty())
        return explicitSide;
    const QPointF d = otherRect.center() - ownerRect.center();
    if (std::abs(d.x()) > std::abs(d.y()))
        return d.x() >= 0 ? QStringLiteral("right") : QStringLiteral("left");
    return d.y() >= 0 ? QStringLiteral("bottom") : QStringLiteral("top");
}

QPainterPath CanvasView::edgePath(const CanvasEdge &edge, QPointF *labelAnchor) const
{
    QPainterPath path;
    const CanvasNode *from = m_document.node(edge.fromNode());
    const CanvasNode *to = m_document.node(edge.toNode());
    if (!from || !to)
        return path;
    const QRectF fromRect = currentRect(*from);
    const QRectF toRect = currentRect(*to);
    const QString fromSide = effectiveSide(edge.fromSide(), fromRect, toRect);
    const QString toSide = effectiveSide(edge.toSide(), toRect, fromRect);
    const QPointF a = mapFromCanvasF(sideAnchor(fromRect, fromSide));
    const QPointF b = mapFromCanvasF(sideAnchor(toRect, toSide));
    const qreal handleLength = qBound(24.0, QLineF(a, b).length() * 0.5, 160.0);
    const QPointF c1 = a + sideNormal(fromSide) * handleLength;
    const QPointF c2 = b + sideNormal(toSide) * handleLength;
    path.moveTo(a);
    path.cubicTo(c1, c2, b);
    if (labelAnchor)
        *labelAnchor = path.pointAtPercent(0.5);
    return path;
}

namespace {
/// An arrowhead at parameter `t` of `path`, pointing the way the curve is
/// heading there - built from two nearby points instead of an angle, so
/// there is no degrees-vs-radians, or which-way-is-up, to get wrong.
void paintArrowhead(QPainter &painter, const QPainterPath &path, qreal t, const QColor &color)
{
    const QPointF tip = path.pointAtPercent(t);
    const qreal neighborT = t < 0.5 ? qMin(1.0, t + 0.03) : qMax(0.0, t - 0.03);
    QPointF dir = tip - path.pointAtPercent(neighborT);
    const qreal length = std::hypot(dir.x(), dir.y());
    if (length < 0.01)
        return;
    dir /= length;
    const QPointF normal(-dir.y(), dir.x());
    constexpr qreal kSize = 8.0;
    const QPointF back = tip - dir * kSize;
    const QPolygonF head{tip, back + normal * (kSize * 0.5), back - normal * (kSize * 0.5)};
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPolygon(head);
}
}

void CanvasView::paintEdges(QPainter &painter) const
{
    const QRectF viewport(rect());
    for (const CanvasEdge &edge : m_document.edges()) {
        QPointF labelAnchor;
        const QPainterPath path = edgePath(edge, &labelAnchor);
        // Padded: a straight vertical or horizontal edge has a zero-width
        // box, and QRectF::intersects() calls an empty rect disjoint from
        // everything, so such an edge was never drawn.
        if (path.isEmpty() || !path.boundingRect().adjusted(-1, -1, 1, 1).intersects(viewport))
            continue;

        const bool selected = m_selectedEdges.contains(edge.id());
        QColor color = resolveCanvasColor(edge.color(), m_theme);
        if (!color.isValid())
            color = m_theme.color(QStringLiteral("--text-muted"));
        if (selected)
            color = m_theme.color(QStringLiteral("--interactive-accent"));
        const bool emphasised = selected || m_hoveredEdge == edge.id();
        painter.setPen(QPen(color, emphasised ? 2.4 : 1.6));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);

        if (edge.toEnd() == CanvasEdgeEnd::Arrow)
            paintArrowhead(painter, path, 1.0, color);
        if (edge.fromEnd() == CanvasEdgeEnd::Arrow)
            paintArrowhead(painter, path, 0.0, color);

        if (!edge.label().isEmpty()) {
            QFont font = m_theme.uiFont();
            font.setPixelSize(qMax(9, int(12 * m_zoom)));
            const QFontMetricsF metrics(font);
            const QRectF textRect = metrics.boundingRect(edge.label())
                .adjusted(-5, -3, 5, 3)
                .translated(labelAnchor - QPointF(metrics.horizontalAdvance(edge.label()) / 2.0, metrics.height() / 2.0));
            painter.setFont(font);
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_theme.color(QStringLiteral("--background-primary")));
            painter.drawRoundedRect(textRect, 4, 4);
            painter.setPen(m_theme.color(QStringLiteral("--text-normal")));
            painter.drawText(textRect, Qt::AlignCenter, edge.label());
        }
    }

    if (m_drag.kind == DragKind::Connect) {
        if (const CanvasNode *from = m_document.node(m_drag.connectFromNode)) {
            const QPointF a = mapFromCanvasF(sideAnchor(currentRect(*from), m_drag.connectFromSide));
            const QPointF b = mapFromCanvasF(m_drag.currentCanvas);
            QPainterPath path(a);
            path.cubicTo(a + sideNormal(m_drag.connectFromSide) * 60.0, b, b);
            painter.setPen(QPen(m_theme.color(QStringLiteral("--interactive-accent")), 2, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
        }
    }
}

void CanvasView::paintGroups(QPainter &painter) const
{
    for (const CanvasNode &node : m_document.nodes()) {
        if (node.type() != CanvasNodeType::Group)
            continue;
        QRectF screen;
        if (!isNodeVisible(node, &screen))
            continue;

        const QColor accent = resolveCanvasColor(node.color(), m_theme);
        const QColor border = accent.isValid() ? accent : m_theme.color(QStringLiteral("--background-modifier-border"));
        QColor fill = accent.isValid() ? accent : m_theme.color(QStringLiteral("--background-secondary"));
        fill.setAlpha(accent.isValid() ? 28 : 130);
        painter.setPen(QPen(border, m_selectedNodes.contains(node.id()) ? 2.0 : 1.2));
        painter.setBrush(fill);
        const qreal radius = qMin(8.0, screen.width() / 4.0);
        painter.drawRoundedRect(screen, radius, radius);

        if (!node.background().isEmpty() && m_fileResolver) {
            const QString absolute = m_fileResolver(node.background());
            const QImage image = absolute.isEmpty() ? QImage() : readImage(absolute);
            if (!image.isNull()) {
                painter.save();
                painter.setClipRect(screen);
                const QString style = node.backgroundStyle();
                const Qt::AspectRatioMode aspect =
                    style == QLatin1String("ratio") ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding;
                if (style == QLatin1String("repeat")) {
                    painter.drawTiledPixmap(screen, QPixmap::fromImage(image));
                } else {
                    const QPixmap scaled =
                        QPixmap::fromImage(image).scaled(screen.size().toSize(), aspect, Qt::SmoothTransformation);
                    painter.drawPixmap(screen.center() - QPointF(scaled.width(), scaled.height()) / 2.0, scaled);
                }
                painter.restore();
            }
        }

        if (!node.label().isEmpty()) {
            QFont font = m_theme.uiFont();
            font.setPixelSize(qMax(10, int(13 * m_zoom)));
            font.setWeight(QFont::DemiBold);
            const QFontMetricsF metrics(font);
            painter.setFont(font);
            painter.setPen(m_theme.color(QStringLiteral("--text-muted")));
            painter.drawText(QPointF(screen.left() + 2, screen.top() - metrics.descent() - 4), node.label());
        }
    }
}

void CanvasView::paintNodeChrome(QPainter &painter, const CanvasNode &node, const QRectF &screenRect) const
{
    const QColor accent = resolveCanvasColor(node.color(), m_theme);
    const bool selected = m_selectedNodes.contains(node.id());
    QColor border = accent.isValid() ? accent : m_theme.color(QStringLiteral("--background-modifier-border"));
    qreal borderWidth = accent.isValid() ? 2.0 : 1.0;
    if (selected) {
        border = m_theme.color(QStringLiteral("--interactive-accent"));
        borderWidth = 2.0;
    }
    painter.setBrush(m_theme.color(QStringLiteral("--background-primary")));
    painter.setPen(QPen(border, borderWidth));
    const qreal radius = qMin(8.0, qMin(screenRect.width(), screenRect.height()) / 4.0);
    painter.drawRoundedRect(screenRect, radius, radius);
}

void CanvasView::paintNodes(QPainter &painter) const
{
    for (const CanvasNode &node : m_document.nodes()) {
        if (node.type() == CanvasNodeType::Group)
            continue;
        QRectF screen;
        if (!isNodeVisible(node, &screen))
            continue;
        paintNodeChrome(painter, node, screen);

        if (!m_alwaysDrawContent && (screen.width() < kLodMinPixels || screen.height() < kLodMinPixels))
            continue; // too small on screen to be worth the content pixmap

        const QPixmap pixmap = contentPixmap(node);
        if (pixmap.isNull())
            continue;
        painter.save();
        const QRectF inner = screen.adjusted(2, 2, -2, -2);
        painter.setClipRect(inner);
        painter.drawPixmap(inner, pixmap, QRectF(QPointF(0, 0), pixmap.size()));
        const QScrollBar *bar = scrollEditorShows(node) && m_scrollY > 0 ? m_scrollEditor->verticalScrollBar() : nullptr;
        if (bar && bar->maximum() > 0) {
            // A thin thumb at the card's edge: how far down, and how much of the note is showing.
            const qreal total = bar->maximum() + bar->pageStep();
            const qreal height = inner.height() * bar->pageStep() / total;
            const qreal top = inner.top() + (inner.height() - height) * bar->value() / bar->maximum();
            painter.fillRect(QRectF(inner.right() - 4, top, 3, height),
                             m_theme.color(QStringLiteral("--background-modifier-border-hover")));
        }
        painter.restore();
    }
}

void CanvasView::paintOverlay(QPainter &painter) const
{
    if (m_selectedNodes.size() == 1) {
        if (const CanvasNode *node = m_document.node(*m_selectedNodes.begin())) {
            const QRectF s(mapFromCanvasF(currentRect(*node).topLeft()), mapFromCanvasF(currentRect(*node).bottomRight()));
            const QPointF handles[] = {
                s.topLeft(), s.topRight(), s.bottomLeft(), s.bottomRight(),
                QPointF((s.left() + s.right()) / 2, s.top()), QPointF((s.left() + s.right()) / 2, s.bottom()),
                QPointF(s.left(), (s.top() + s.bottom()) / 2), QPointF(s.right(), (s.top() + s.bottom()) / 2),
            };
            painter.setPen(QPen(m_theme.color(QStringLiteral("--interactive-accent")), 1));
            painter.setBrush(m_theme.color(QStringLiteral("--background-primary")));
            for (const QPointF &p : handles)
                painter.drawRect(QRectF(p.x() - kResizeHandleSize / 2, p.y() - kResizeHandleSize / 2,
                                        kResizeHandleSize, kResizeHandleSize));
        }
    }

    if (!m_hoveredNode.isEmpty() && m_drag.kind == DragKind::None) {
        if (const CanvasNode *node = m_document.node(m_hoveredNode)) {
            const QRectF s(mapFromCanvasF(currentRect(*node).topLeft()), mapFromCanvasF(currentRect(*node).bottomRight()));
            static const QString sides[] = {QStringLiteral("top"), QStringLiteral("right"), QStringLiteral("bottom"),
                                            QStringLiteral("left")};
            painter.setPen(QPen(m_theme.color(QStringLiteral("--interactive-accent")), 1.5));
            painter.setBrush(m_theme.color(QStringLiteral("--background-primary")));
            for (const QString &side : sides) {
                const QPointF p = sideAnchor(s, side) + sideNormal(side) * kConnectHandleOffset;
                painter.drawEllipse(p, kConnectHitRadius * 0.6, kConnectHitRadius * 0.6);
            }
        }
    }

    if (!m_drag.guides.isEmpty()) {
        // Pixel-centred, so a one-pixel line is not smeared over two.
        painter.setPen(QPen(m_theme.color(QStringLiteral("--interactive-accent")), 1));
        for (const QLineF &guide : std::as_const(m_drag.guides)) {
            const QPointF a = mapFromCanvasF(guide.p1());
            const QPointF b = mapFromCanvasF(guide.p2());
            const QPointF shift(guide.dx() == 0 ? std::floor(a.x()) + 0.5 - a.x() : 0,
                                guide.dy() == 0 ? std::floor(a.y()) + 0.5 - a.y() : 0);
            painter.drawLine(a + shift, b + shift);
        }
    }

    if (m_drag.kind == DragKind::RubberBand && m_drag.moved) {
        const QRectF band(mapFromCanvasF(m_drag.startCanvas), mapFromCanvasF(m_drag.currentCanvas));
        QColor fill = m_theme.color(QStringLiteral("--interactive-accent"));
        fill.setAlpha(40);
        painter.setPen(QPen(m_theme.color(QStringLiteral("--interactive-accent")), 1));
        painter.setBrush(fill);
        painter.drawRect(band.normalized());
    }

    if (m_drag.kind == DragKind::Connect && !m_drag.connectHoverTarget.isEmpty()) {
        if (const CanvasNode *node = m_document.node(m_drag.connectHoverTarget)) {
            const QRectF s(mapFromCanvasF(currentRect(*node).topLeft()), mapFromCanvasF(currentRect(*node).bottomRight()));
            painter.setPen(QPen(m_theme.color(QStringLiteral("--interactive-accent")), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(s.adjusted(-3, -3, 3, 3), 8, 8);
        }
    }
}

// --- node content, cached ------------------------------------------------

std::unique_ptr<MarkdownEditor> CanvasView::makeContentEditor(const QString &text, const QSize &physicalSize) const
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->setTheme(m_theme);
    editor->setMode(MarkdownEditor::Mode::Reading);
    editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->resize(physicalSize);
    editor->setNoteText(text);
    // A large node's text parses on the thread pool, same as a note pane's;
    // take it now rather than grabbing blank text. Not a nested event loop:
    // this runs inside paintEvent, where a timer or a deferred delete (Ctrl+W)
    // firing would free the view mid-paint.
    editor->finishParse();
    return editor;
}

QPixmap CanvasView::renderMarkdownContent(const QString &text, const QSize &physicalSize) const
{
    return makeContentEditor(text, physicalSize)->grab();
}

QPixmap CanvasView::renderImageContent(const QString &absolutePath, const QSize &physicalSize) const
{
    const QImage image = readImage(absolutePath);
    if (image.isNull())
        return QPixmap();
    QPixmap canvas(physicalSize);
    canvas.fill(m_theme.color(QStringLiteral("--background-primary")));
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QPixmap scaled =
        QPixmap::fromImage(image).scaled(physicalSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    painter.drawPixmap((physicalSize.width() - scaled.width()) / 2, (physicalSize.height() - scaled.height()) / 2,
                       scaled);
    return canvas;
}

QPixmap CanvasView::renderLinkCard(const QString &url, const QSize &physicalSize) const
{
    QPixmap canvas(physicalSize);
    canvas.fill(m_theme.color(QStringLiteral("--background-primary")));
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);
    const int pad = int(16 * kContentScale);
    const int iconSize = int(28 * kContentScale);
    Icons::icon(QStringLiteral("external-link"), m_theme.color(QStringLiteral("--text-muted")), iconSize)
        .paint(&painter, QRect(pad, pad, iconSize, iconSize));
    QFont font = m_theme.textFont();
    font.setPixelSize(int(13 * kContentScale));
    painter.setFont(font);
    painter.setPen(m_theme.color(QStringLiteral("--text-normal")));
    const QRect textRect(pad, pad + iconSize + pad / 2, physicalSize.width() - 2 * pad,
                         physicalSize.height() - (pad * 2 + iconSize));
    painter.drawText(textRect, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, url);
    return canvas;
}

QPixmap CanvasView::renderFileCard(const QString &fileName, const QSize &physicalSize, bool missing) const
{
    QPixmap canvas(physicalSize);
    canvas.fill(m_theme.color(QStringLiteral("--background-primary")));
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);
    const int iconSize = qMin(qMin(physicalSize.width(), physicalSize.height()) / 2, int(40 * kContentScale));
    const QColor tint = missing ? m_theme.color(QStringLiteral("--text-error"))
                                : m_theme.color(QStringLiteral("--text-muted"));
    Icons::icon(QStringLiteral("file-text"), tint, iconSize)
        .paint(&painter, QRect((physicalSize.width() - iconSize) / 2,
                               (physicalSize.height() - iconSize) / 2 - int(10 * kContentScale), iconSize, iconSize));
    QFont font = m_theme.uiFont();
    font.setPixelSize(int(12 * kContentScale));
    painter.setFont(font);
    painter.setPen(m_theme.color(QStringLiteral("--text-muted")));
    const QFontMetrics metrics(font);
    const QString elided = metrics.elidedText(QFileInfo(fileName).fileName(), Qt::ElideMiddle,
                                              physicalSize.width() - int(16 * kContentScale));
    painter.drawText(QRect(0, physicalSize.height() / 2 + iconSize / 2, physicalSize.width(), int(20 * kContentScale)),
                     Qt::AlignHCenter, elided);
    return canvas;
}

QString CanvasView::contentSignature(const CanvasNode &node)
{
    switch (node.type()) {
    case CanvasNodeType::Text: return node.text();
    case CanvasNodeType::File: return node.file() + QChar(0x1f) + node.subpath();
    case CanvasNodeType::Link: return node.url();
    default: return QString();
    }
}

namespace {
bool isImageFile(const QString &absolute)
{
    return isPictureFile(absolute); // the first bytes only, not a decode
}

QString markdownFileSection(const QString &absolute, const QString &subpath)
{
    QByteArray bytes;
    const QString text = Vault::readBytes(absolute, &bytes) ? QString::fromUtf8(bytes) : QString();
    return canvasSectionOf(text, subpath);
}
}

bool CanvasView::markdownTextOf(const CanvasNode &node, QString *text) const
{
    if (node.type() == CanvasNodeType::Text) {
        *text = node.text();
        return true;
    }
    if (node.type() != CanvasNodeType::File || !node.file().endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
        return false;
    const QString absolute = m_fileResolver ? m_fileResolver(node.file()) : QString();
    if (absolute.isEmpty() || isImageFile(absolute))
        return false;
    *text = markdownFileSection(absolute, node.subpath());
    return true;
}

QPixmap CanvasView::contentPixmap(const CanvasNode &node) const
{
    const QRectF r = currentRect(node);
    const QSize physical(qMax(1, qRound(r.width())) * kContentScale, qMax(1, qRound(r.height())) * kContentScale);

    // A scrolled card is grabbed from its own editor each frame (a scroll and
    // a paint, no parse or layout of the whole note); at scroll 0 the cache
    // below already holds the same picture.
    if (m_scrollY > 0 && scrollEditorShows(node) && m_scrollEditor->size() == physical) {
        m_scrollEditor->verticalScrollBar()->setValue(qRound(m_scrollY));
        return m_scrollEditor->grab();
    }

    if (node.type() != CanvasNodeType::Text && node.type() != CanvasNodeType::File && node.type() != CanvasNodeType::Link)
        return QPixmap();
    const QString signature = contentSignature(node);

    const auto cached = m_contentCache.constFind(node.id());
    if (cached != m_contentCache.constEnd() && cached->signature == signature && cached->pixmap.size() == physical)
        return cached->pixmap;

    QPixmap pixmap;
    if (node.type() == CanvasNodeType::Text) {
        pixmap = renderMarkdownContent(node.text(), physical);
    } else if (node.type() == CanvasNodeType::Link) {
        pixmap = renderLinkCard(node.url(), physical);
    } else { // File
        const QString absolute = m_fileResolver ? m_fileResolver(node.file()) : QString();
        if (absolute.isEmpty()) {
            pixmap = renderFileCard(node.file(), physical, true);
        } else if (isImageFile(absolute)) {
            pixmap = renderImageContent(absolute, physical);
        } else if (node.file().endsWith(QLatin1String(".md"), Qt::CaseInsensitive)) {
            pixmap = renderMarkdownContent(markdownFileSection(absolute, node.subpath()), physical);
        } else {
            pixmap = renderFileCard(QFileInfo(absolute).fileName(), physical, false);
        }
    }
    m_contentCache.insert(node.id(), {pixmap, signature});
    return pixmap;
}

} // namespace noteAppC
