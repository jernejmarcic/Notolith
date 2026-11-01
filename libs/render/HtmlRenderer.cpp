// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "HtmlRenderer.h"

#include <QRegularExpression> // used by sanitizeHtml() in both branches below

#ifdef NOTEAPPC_HAVE_LITEHTML

#include <QDir>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <atomic>

#include <litehtml.h>
#include <litehtml/num_cvt.h>

#include "ImageLoad.h"

namespace noteAppC {

namespace {

/// Test-only, see HtmlRenderer::renderCount(). Bumped once per litehtml
/// document actually built, not once per call: an early return (empty
/// markup, zero width) never gets this far.
std::atomic<qint64> g_renderCount{0};

QColor toQColor(const litehtml::web_color &c)
{
    return QColor(c.red, c.green, c.blue, c.alpha);
}

QRectF toQRect(const litehtml::position &p)
{
    return QRectF(p.x, p.y, p.width, p.height);
}

std::string toCssColor(const QColor &c)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(c.alphaF())
        .toStdString();
}

/// What create_font hands back as litehtml::uint_ptr: just the QFont it
/// resolved to. text_width/draw_text/delete_font all get the same handle
/// back, so nothing here needs to re-resolve a font_description.
struct FontHandle {
    QFont font;
};

/// The QPainter-backed litehtml::document_container this class owns, so
/// libs/render is the only place that ever mentions litehtml. Deliberately
/// written from scratch rather than adapted from Qt Creator's qlitehtml
/// (code.qt.io/cgit/playground/qlitehtml.git): its container_qpainter.cpp
/// carries `SPDX-License-Identifier: LicenseRef-Qt-Commercial OR
/// GPL-3.0-only WITH Qt-GPL-exception-1.0` - copyleft, and specifically the
/// kind of dependency this project already keeps at arm's length (poppler
/// is GPL too, which is exactly why NOTEAPPC_WITH_PDFVIEW exists and why it
/// links dynamically rather than compiling any of poppler's own code in).
/// litehtml's own document_container.h, which this class implements, is
/// part of the litehtml project and BSD-3-Clause like the rest of it.
///
/// It implements only what a static, once-per-block render needs: nothing
/// here tracks hover, click or scroll state, because the result this
/// produces is a cached QImage, not a live litehtml view.
class PainterContainer : public litehtml::document_container {
public:
    PainterContainer(HtmlRenderer::Theme theme, QString baseDir, std::function<QString(const QString &)> localPath)
        : m_theme(std::move(theme)), m_baseDir(std::move(baseDir)), m_localPath(std::move(localPath))
    {
    }

    QPainter *painter = nullptr;
    int width = 0;

    litehtml::uint_ptr create_font(const litehtml::font_description &descr, const litehtml::document *,
                                   litehtml::font_metrics *fm) override
    {
        QFont font(descr.family.empty() ? m_theme.textFont.family()
                                        : QString::fromUtf8(descr.family.c_str()));
        font.setPixelSize(qMax(1, int(descr.size)));
        font.setBold(descr.weight >= 600);
        font.setItalic(descr.style == litehtml::font_style_italic);
        font.setUnderline(descr.decoration_line & litehtml::text_decoration_line_underline);
        font.setStrikeOut(descr.decoration_line & litehtml::text_decoration_line_line_through);

        auto *handle = new FontHandle{font};
        if (fm) {
            const QFontMetricsF metrics(font);
            fm->height = litehtml::pixel_t(metrics.height());
            fm->ascent = litehtml::pixel_t(metrics.ascent());
            fm->descent = litehtml::pixel_t(metrics.descent());
            fm->x_height =
                litehtml::pixel_t(metrics.xHeight() > 0 ? metrics.xHeight() : metrics.ascent() / 2);
            fm->ch_width = litehtml::pixel_t(metrics.horizontalAdvance(QLatin1Char('0')));
            fm->draw_spaces = true;
        }
        return reinterpret_cast<litehtml::uint_ptr>(handle);
    }

    void delete_font(litehtml::uint_ptr hFont) override
    {
        delete reinterpret_cast<FontHandle *>(hFont);
    }

    litehtml::pixel_t text_width(const char *text, litehtml::uint_ptr hFont) override
    {
        const auto *handle = reinterpret_cast<FontHandle *>(hFont);
        return litehtml::pixel_t(QFontMetricsF(handle->font).horizontalAdvance(QString::fromUtf8(text)));
    }

    void draw_text(litehtml::uint_ptr, const char *text, litehtml::uint_ptr hFont,
                   litehtml::web_color color, const litehtml::position &pos) override
    {
        if (!text || !*text)
            return;
        const auto *handle = reinterpret_cast<FontHandle *>(hFont);
        if (painter) {
            painter->save();
            painter->setFont(handle->font);
            painter->setPen(toQColor(color));
            const QFontMetricsF metrics(handle->font);
            painter->drawText(QPointF(pos.x, pos.y + metrics.ascent()), QString::fromUtf8(text));
            painter->restore();
        }
        recordRun(text, pos, handle->font);
    }

    /// Public rather than accessed through a setter: render() pokes this and
    /// `painter`/`width` the same way, right after constructing the
    /// container - see the class comment on those two.
    QList<HtmlRenderer::TextRun> *textRunsOut = nullptr;

    void recordRun(const char *text, const litehtml::position &pos, const QFont &font)
    {
        if (!textRunsOut)
            return;
        const QString run = QString::fromUtf8(text);
        if (run.isEmpty())
            return;
        // A run whose top sits clearly below the previous one started a new
        // line; see TextRun's comment on why that costs one synthetic
        // separator in the offset space rather than nothing.
        if (m_havePrevRunY && double(pos.y) > m_prevRunY + 1.0)
            ++m_textOffset;
        textRunsOut->push_back({toQRect(pos), m_textOffset, run, font});
        m_textOffset += run.size();
        m_prevRunY = double(pos.y);
        m_havePrevRunY = true;
    }

    litehtml::pixel_t pt_to_px(float pt) const override { return litehtml::pixel_t(pt * 96.0f / 72.0f); }

    litehtml::pixel_t get_default_font_size() const override
    {
        return litehtml::pixel_t(
            qMax(1, m_theme.textFont.pixelSize() > 0 ? m_theme.textFont.pixelSize() : 14));
    }

    const char *get_default_font_name() const override
    {
        m_defaultFontName = m_theme.textFont.family().toUtf8();
        return m_defaultFontName.isEmpty() ? "sans-serif" : m_defaultFontName.constData();
    }

    void draw_list_marker(litehtml::uint_ptr, const litehtml::list_marker &marker) override
    {
        if (!painter)
            return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRectF box(marker.pos.x, marker.pos.y, marker.pos.width, marker.pos.height);
        switch (marker.marker_type) {
        case litehtml::list_style_type_disc:
            painter->setPen(Qt::NoPen);
            painter->setBrush(toQColor(marker.color));
            painter->drawEllipse(box);
            break;
        case litehtml::list_style_type_circle:
            painter->setBrush(Qt::NoBrush);
            painter->setPen(toQColor(marker.color));
            painter->drawEllipse(box.adjusted(1, 1, -1, -1));
            break;
        case litehtml::list_style_type_square:
            painter->setPen(Qt::NoPen);
            painter->setBrush(toQColor(marker.color));
            painter->drawRect(box);
            break;
        case litehtml::list_style_type_none:
            break;
        default:
            // Numbered and lettered styles, in litehtml's own converters;
            // the rest of CSS's numeral systems fall back to decimal.
            painter->setPen(toQColor(marker.color));
            painter->drawText(box.adjusted(0, 0, 40, 0), markerLabel(marker.marker_type, marker.index));
            break;
        }
        painter->restore();
    }

    void load_image(const char *src, const char *baseurl, bool) override
    {
        const QString path = resolveImagePath(QString::fromUtf8(src), QString::fromUtf8(baseurl));
        if (path.isEmpty() || m_images.contains(path))
            return;
        // Local only - resolveImagePath already refused anything remote.
        // A file that fails to load leaves a null QImage in the cache, which
        // get_image_size and draw_image both already treat as "draw
        // nothing", so a broken local path degrades the same way a remote
        // one does.
        m_images.insert(path, readImage(path));
    }

    void get_image_size(const char *src, const char *baseurl, litehtml::size &sz) override
    {
        const QImage image =
            m_images.value(resolveImagePath(QString::fromUtf8(src), QString::fromUtf8(baseurl)));
        sz.width = litehtml::pixel_t(image.width());
        sz.height = litehtml::pixel_t(image.height());
    }

    void draw_image(litehtml::uint_ptr, const litehtml::background_layer &layer, const std::string &url,
                    const std::string &baseUrl) override
    {
        if (!painter)
            return;
        const QImage image = m_images.value(
            resolveImagePath(QString::fromUtf8(url.c_str()), QString::fromUtf8(baseUrl.c_str())));
        if (image.isNull())
            return; // remote, missing, or unreadable: nothing draws, as documented on the class
        painter->save();
        painter->setClipPath(roundedPath(toQRect(layer.border_box), layer.border_radius), Qt::IntersectClip);
        painter->drawImage(toQRect(layer.border_box), image);
        painter->restore();
    }

    void draw_solid_fill(litehtml::uint_ptr, const litehtml::background_layer &layer,
                         const litehtml::web_color &color) override
    {
        if (!painter || color.alpha == 0)
            return;
        fillBrush(layer, QBrush(toQColor(color)));
    }

    // Gradients as Qt's own: litehtml gives the geometry relative to the
    // layer's origin box and the stops as offsets from 0 to 1, which is
    // QGradient's shape. Clipped to the rounded border box like any fill.
    void draw_linear_gradient(litehtml::uint_ptr, const litehtml::background_layer &layer,
                              const litehtml::background_layer::linear_gradient &gradient) override
    {
        const QPointF origin(layer.origin_box.x, layer.origin_box.y);
        QLinearGradient fill(origin + QPointF(gradient.start.x, gradient.start.y),
                             origin + QPointF(gradient.end.x, gradient.end.y));
        fillGradient(layer, fill, gradient.color_points);
    }
    void draw_radial_gradient(litehtml::uint_ptr, const litehtml::background_layer &layer,
                              const litehtml::background_layer::radial_gradient &gradient) override
    {
        // An ellipse: a circle of the x radius, squashed to the y one.
        const QPointF centre(layer.origin_box.x + gradient.position.x, layer.origin_box.y + gradient.position.y);
        const qreal rx = qMax<qreal>(gradient.radius.x, 0.01);
        const qreal ry = qMax<qreal>(gradient.radius.y, 0.01);
        QRadialGradient fill(QPointF(0, 0), rx);
        QBrush brush = gradientBrush(fill, gradient.color_points);
        brush.setTransform(QTransform::fromTranslate(centre.x(), centre.y()).scale(1, ry / rx));
        fillBrush(layer, brush);
    }
    void draw_conic_gradient(litehtml::uint_ptr, const litehtml::background_layer &layer,
                             const litehtml::background_layer::conic_gradient &gradient) override
    {
        // CSS starts at the top and turns clockwise; Qt starts at three
        // o'clock and turns anticlockwise, and runs its stops the same way.
        const QPointF centre(layer.origin_box.x + gradient.position.x, layer.origin_box.y + gradient.position.y);
        QConicalGradient fill(centre, 90 - gradient.angle);
        litehtml::vector<litehtml::background_layer::color_point> reversed;
        for (auto it = gradient.color_points.rbegin(); it != gradient.color_points.rend(); ++it)
            reversed.emplace_back(1.0f - it->offset, it->color);
        fillGradient(layer, fill, reversed);
    }

    void draw_borders(litehtml::uint_ptr, const litehtml::borders &borders, const litehtml::position &pos,
                      bool) override
    {
        if (!painter)
            return;
        const litehtml::border *sides[] = {&borders.top, &borders.right, &borders.bottom, &borders.left};
        const auto shown = [](const litehtml::border *side) {
            return side->width > 0 && side->style != litehtml::border_style_none
                   && side->style != litehtml::border_style_hidden && side->color.alpha > 0;
        };
        if (std::none_of(std::begin(sides), std::end(sides), shown))
            return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRectF box = toQRect(pos);
        const bool uniform = std::all_of(std::begin(sides), std::end(sides), [&](const litehtml::border *side) {
            return side->width == borders.top.width && side->style == borders.top.style
                   && side->color == borders.top.color;
        });
        const litehtml::border_style style = borders.top.style;
        if (uniform && (style == litehtml::border_style_solid || style == litehtml::border_style_dotted
                        || style == litehtml::border_style_dashed)) {
            // One stroke round the (maybe rounded) box, inset by half the
            // width so it stays inside it, as a CSS border does.
            const qreal half = borders.top.width / 2.0;
            QPen pen(toQColor(borders.top.color), qreal(borders.top.width));
            pen.setStyle(penStyleFor(style));
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(roundedPath(box, borders.radius, QMarginsF(half, half, half, half)));
        } else {
            // Mismatched sides or a two-band style. The ring between the
            // outer and the inner rounded edge is cut into four wedges, one
            // per side, mitred at the corners; each wedge is clipped to the
            // ring and gets its own side's width, colour and style, so radii
            // and per-side styles compose.
            const QMarginsF width(borders.left.width, borders.top.width, borders.right.width, borders.bottom.width);
            const QPainterPath ring = roundedPath(box, borders.radius).subtracted(roundedPath(box, borders.radius, width));
            const QPointF outer[] = {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()};
            const QPointF inner[] = {box.topLeft() + QPointF(width.left(), width.top()),
                                     box.topRight() + QPointF(-width.right(), width.top()),
                                     box.bottomRight() + QPointF(-width.right(), -width.bottom()),
                                     box.bottomLeft() + QPointF(width.left(), -width.bottom())};
            for (int i = 0; i < 4; ++i) {
                if (!shown(sides[i]))
                    continue;
                const int next = (i + 1) % 4;
                QPainterPath wedge;
                wedge.addPolygon(QPolygonF({outer[i], outer[next], inner[next], inner[i]}));
                wedge.closeSubpath();
                painter->save();
                painter->setClipPath(wedge.intersected(ring));
                drawSide(box, borders.radius, width, *sides[i], i);
                painter->restore();
            }
        }
        painter->restore();
    }

    void set_caption(const char *) override {}
    void set_base_url(const char *baseUrl) override { m_baseUrl = QString::fromUtf8(baseUrl); }
    void link(const std::shared_ptr<litehtml::document> &, const litehtml::element::ptr &) override {}
    // Rendering is static and non-interactive (one draw() into a QImage,
    // then the container is gone), so a click is never dispatched into it in
    // the first place - there is nothing to do here.
    void on_anchor_click(const char *, const litehtml::element::ptr &) override {}
    void on_mouse_event(const litehtml::element::ptr &, litehtml::mouse_event) override {}
    void set_cursor(const char *) override {}

    void transform_text(litehtml::string &text, litehtml::text_transform tt) override
    {
        QString s = QString::fromUtf8(text.c_str());
        switch (tt) {
        case litehtml::text_transform_capitalize: {
            QStringList words = s.split(QLatin1Char(' '));
            for (QString &word : words) {
                if (!word.isEmpty())
                    word.replace(0, 1, word.left(1).toUpper());
            }
            s = words.join(QLatin1Char(' '));
            break;
        }
        case litehtml::text_transform_uppercase:
            s = s.toUpper();
            break;
        case litehtml::text_transform_lowercase:
            s = s.toLower();
            break;
        case litehtml::text_transform_none:
        default:
            return;
        }
        text = s.toUtf8().constData();
    }

    void import_css(litehtml::string &text, const litehtml::string &, litehtml::string &) override
    {
        text.clear(); // @import is a fetch of its own; never followed, see the class comment
    }

    void set_clip(const litehtml::position &pos, const litehtml::border_radiuses &) override
    {
        if (!painter)
            return;
        painter->save();
        painter->setClipRect(toQRect(pos), Qt::IntersectClip);
    }
    void del_clip() override
    {
        if (painter)
            painter->restore();
    }

    void get_viewport(litehtml::position &viewport) const override
    {
        // The height is a stand-in: litehtml asks for the viewport mainly
        // for fixed-position elements and viewport-relative units, neither
        // of which a note's HTML block leans on, and the real height is not
        // known until render() has already run.
        viewport = litehtml::position(0, 0, litehtml::pixel_t(width), litehtml::pixel_t(1000000));
    }

    litehtml::element::ptr create_element(const char *, const litehtml::string_map &,
                                          const std::shared_ptr<litehtml::document> &) override
    {
        return nullptr; // litehtml builds its own default element for the tag
    }

    void get_media_features(litehtml::media_features &media) const override
    {
        media.type = litehtml::media_type_screen;
        media.width = media.device_width = litehtml::pixel_t(width);
        media.height = media.device_height = litehtml::pixel_t(1000000);
        media.color = 8;
        media.monochrome = 0;
        media.resolution = 96;
    }

    void get_language(litehtml::string &language, litehtml::string &culture) const override
    {
        language = "en";
        culture.clear();
    }

private:
    /// A box with CSS's per-corner elliptical radii, shrunk by `inset` on each
    /// side (a border's width, or a fraction of it): the box moves in and each
    /// radius loses the inset of the two sides it touches, so the curve stays
    /// concentric with the outer edge.
    static QPainterPath roundedPath(const QRectF &outerBox, const litehtml::border_radiuses &r,
                                    const QMarginsF &inset = QMarginsF())
    {
        const QRectF box = outerBox.marginsRemoved(inset);
        const auto radius = [](litehtml::pixel_t x, litehtml::pixel_t y, qreal dx, qreal dy) {
            return QSizeF(qMax<qreal>(0, x - dx), qMax<qreal>(0, y - dy));
        };
        const QSizeF tl = radius(r.top_left_x, r.top_left_y, inset.left(), inset.top());
        const QSizeF tr = radius(r.top_right_x, r.top_right_y, inset.right(), inset.top());
        const QSizeF br = radius(r.bottom_right_x, r.bottom_right_y, inset.right(), inset.bottom());
        const QSizeF bl = radius(r.bottom_left_x, r.bottom_left_y, inset.left(), inset.bottom());
        QPainterPath path;
        if (tl.isEmpty() && tr.isEmpty() && br.isEmpty() && bl.isEmpty()) {
            path.addRect(box);
            return path;
        }
        path.moveTo(box.left() + tl.width(), box.top());
        path.lineTo(box.right() - tr.width(), box.top());
        path.arcTo(QRectF(box.right() - 2 * tr.width(), box.top(), 2 * tr.width(), 2 * tr.height()), 90, -90);
        path.lineTo(box.right(), box.bottom() - br.height());
        path.arcTo(QRectF(box.right() - 2 * br.width(), box.bottom() - 2 * br.height(), 2 * br.width(),
                          2 * br.height()), 0, -90);
        path.lineTo(box.left() + bl.width(), box.bottom());
        path.arcTo(QRectF(box.left(), box.bottom() - 2 * bl.height(), 2 * bl.width(), 2 * bl.height()), 270, -90);
        path.lineTo(box.left(), box.top() + tl.height());
        path.arcTo(QRectF(box.left(), box.top(), 2 * tl.width(), 2 * tl.height()), 180, -90);
        path.closeSubpath();
        return path;
    }

    void fillBrush(const litehtml::background_layer &layer, const QBrush &brush)
    {
        if (!painter)
            return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(brush);
        painter->drawPath(roundedPath(toQRect(layer.border_box), layer.border_radius));
        painter->restore();
    }

    static QBrush gradientBrush(QGradient &gradient,
                                const litehtml::vector<litehtml::background_layer::color_point> &stops)
    {
        for (const auto &stop : stops)
            gradient.setColorAt(qBound(0.0, qreal(stop.offset), 1.0), toQColor(stop.color));
        return QBrush(gradient);
    }

    void fillGradient(const litehtml::background_layer &layer, QGradient &gradient,
                      const litehtml::vector<litehtml::background_layer::color_point> &stops)
    {
        if (!stops.empty())
            fillBrush(layer, gradientBrush(gradient, stops));
    }

    static Qt::PenStyle penStyleFor(litehtml::border_style style)
    {
        switch (style) {
        case litehtml::border_style_dotted:
            return Qt::DotLine;
        case litehtml::border_style_dashed:
            return Qt::DashLine;
        default:
            return Qt::SolidLine;
        }
    }

    /// One side of a border inside its clipped wedge. Bands are strokes along
    /// the ring at a fraction of the side's width: one centred band for
    /// solid, dotted and dashed; two thirds-bands round a gap for double;
    /// two halves for groove and ridge (outer dark then light, ridge the
    /// reverse); inset and outset shade the whole side, top and left dark for
    /// inset, bottom and right dark for outset (CSS 2.1 leaves the colours to
    /// the UA; this is what browsers do).
    void drawSide(const QRectF &box, const litehtml::border_radiuses &radius, const QMarginsF &width,
                  const litehtml::border &side, int index)
    {
        const QColor base = toQColor(side.color);
        // Near black cannot go darker, so its "light" side gets lighter instead.
        const bool nearBlack = base.value() < 48;
        const QColor dark = nearBlack ? base : base.darker(160);
        const QColor light = nearBlack ? QColor(qMin(255, base.red() + 96), qMin(255, base.green() + 96),
                                                qMin(255, base.blue() + 96), base.alpha())
                                      : base;
        const bool topLeft = index == 0 || index == 3; // top, left
        const qreal w = side.width;
        const auto band = [&](qreal centre, qreal thickness, const QColor &colour, Qt::PenStyle penStyle) {
            QPen pen(colour, thickness);
            pen.setStyle(penStyle);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(roundedPath(box, radius, QMarginsF(width.left() * centre, width.top() * centre,
                                                                  width.right() * centre, width.bottom() * centre)));
        };
        switch (side.style) {
        case litehtml::border_style_double:
            band(1.0 / 6.0, w / 3.0, base, Qt::SolidLine);
            band(5.0 / 6.0, w / 3.0, base, Qt::SolidLine);
            break;
        case litehtml::border_style_groove:
        case litehtml::border_style_ridge: {
            // Groove: the outer half is dark on top and left, light on bottom and right.
            const bool outerDark = (side.style == litehtml::border_style_groove) == topLeft;
            band(0.25, w / 2.0, outerDark ? dark : light, Qt::SolidLine);
            band(0.75, w / 2.0, outerDark ? light : dark, Qt::SolidLine);
            break;
        }
        case litehtml::border_style_inset:
        case litehtml::border_style_outset: {
            const bool darkSide = (side.style == litehtml::border_style_inset) == topLeft;
            band(0.5, w, darkSide ? dark : light, Qt::SolidLine);
            break;
        }
        default:
            band(0.5, w, base, penStyleFor(side.style));
            break;
        }
    }

    static QString markerLabel(litehtml::list_style_type type, int index)
    {
        using namespace litehtml::num_cvt;
        std::string label;
        switch (type) {
        case litehtml::list_style_type_lower_alpha:
        case litehtml::list_style_type_lower_latin:
            label = to_latin_lower(index);
            break;
        case litehtml::list_style_type_upper_alpha:
        case litehtml::list_style_type_upper_latin:
            label = to_latin_upper(index);
            break;
        case litehtml::list_style_type_lower_greek:
            label = to_greek_lower(index);
            break;
        case litehtml::list_style_type_lower_roman:
            label = to_roman_lower(index);
            break;
        case litehtml::list_style_type_upper_roman:
            label = to_roman_upper(index);
            break;
        case litehtml::list_style_type_decimal_leading_zero:
            return QStringLiteral("%1.").arg(index, 2, 10, QLatin1Char('0'));
        default:
            return QString::number(index) + QLatin1Char('.');
        }
        return QString::fromStdString(label) + QLatin1Char('.');
    }

    /// A local path relative to `baseUrl` (litehtml's own base-url tracking)
    /// or, failing that, `m_baseDir` (the note's directory) - or empty for
    /// anything with a network scheme or a `data:` URI, which this renderer
    /// never fetches or decodes (see the class comment), and for a path nothing
    /// exists at.
    QString resolveImagePath(const QString &src, const QString &baseUrl) const
    {
        if (src.isEmpty() || src.contains(QStringLiteral("://")) || src.startsWith(QLatin1String("data:")))
            return {};
        QString resolved = src;
        if (m_localPath)
            resolved = m_localPath(src);
        else if (QDir::isRelativePath(resolved)) {
            const QString dir = !baseUrl.isEmpty() ? baseUrl : m_baseDir;
            resolved = QDir(dir).filePath(resolved);
        }
        if (resolved.isEmpty())
            return {};
        const QFileInfo info(resolved);
        return info.exists() && info.isFile() ? info.absoluteFilePath() : QString();
    }

    HtmlRenderer::Theme m_theme;
    QString m_baseDir;
    std::function<QString(const QString &)> m_localPath;
    QString m_baseUrl;
    mutable QByteArray m_defaultFontName;
    QHash<QString, QImage> m_images;

    qsizetype m_textOffset = 0;
    double m_prevRunY = 0.0;
    bool m_havePrevRunY = false;
};

/// Defence in depth. litehtml has no script interpreter to run `<script>`
/// with in the first place, and its own master.css already keeps
/// `<script>`/`<style>` off screen, but this project does not want that
/// literal text - or an `<iframe>`'s target - reaching litehtml's tree at
/// all, in case some future litehtml version renders unknown content
/// differently. Emptied outright here rather than merely hidden by CSS.
QString sanitise(QString html)
{
    static const QRegularExpression script(
        QStringLiteral("<script\\b[^>]*>.*?</script\\s*>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    html.remove(script);
    static const QRegularExpression onAttribute(
        QStringLiteral("\\son[a-zA-Z]+\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
        QRegularExpression::CaseInsensitiveOption);
    html.remove(onAttribute);
    return html;
}

} // namespace

bool HtmlRenderer::isAvailable()
{
    return true;
}

QString HtmlRenderer::sanitizeHtml(const QString &html)
{
    return sanitise(html);
}

qint64 HtmlRenderer::renderCount()
{
    return g_renderCount.load();
}

QImage HtmlRenderer::render(const QString &html, const Theme &theme, int widthPixels,
                            const QString &baseDir, qreal devicePixelRatio,
                            const QString &extraCss, QList<TextRun> *textRuns,
                            const std::function<QString(const QString &)> &localPath)
{
    if (widthPixels <= 0 || html.trimmed().isEmpty())
        return {};
    g_renderCount.fetch_add(1);

    PainterContainer container(theme, baseDir, localPath);
    container.width = widthPixels;
    container.textRunsOut = textRuns;

    const int fontSize = qMax(1, theme.textFont.pixelSize() > 0 ? theme.textFont.pixelSize() : 14);
    const std::string userCss = "html,body{margin:0;padding:4px;color:" + toCssColor(theme.text)
        + ";background:transparent;font-family:'" + theme.textFont.family().toStdString()
        + "';font-size:" + std::to_string(fontSize) + "px;}"
        + "a{color:" + toCssColor(theme.link) + ";}"
        + "code,kbd,pre{font-family:'" + theme.codeFont.family().toStdString()
        + "';background:" + toCssColor(theme.codeBackground) + ";}"
        + "table,th,td{border:1px solid " + toCssColor(theme.border) + ";border-collapse:collapse;padding:4px;}"
        + "script,style,iframe,object,embed{display:none;}"
        // The document's own stylesheet, cascading after the theme above so
        // a colour/font it actually declares wins - see the header comment.
        + extraCss.toStdString();

    litehtml::document::ptr document;
    try {
        document = litehtml::document::createFromString(sanitise(html).toStdString(), &container,
                                                         litehtml::master_css, userCss);
    } catch (...) {
        return {};
    }
    if (!document)
        return {};

    document->render(litehtml::pixel_t(widthPixels));
    const int height = int(document->height());
    if (height <= 0)
        return {};

    QImage image(QSize(widthPixels, height) * devicePixelRatio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.scale(devicePixelRatio, devicePixelRatio);
    container.painter = &painter;
    const litehtml::position clip(0, 0, litehtml::pixel_t(widthPixels), litehtml::pixel_t(height));
    document->draw(0, 0, 0, &clip);
    painter.end();

    return image;
}

} // namespace noteAppC

#else // !NOTEAPPC_HAVE_LITEHTML

namespace noteAppC {

bool HtmlRenderer::isAvailable()
{
    return false;
}

qint64 HtmlRenderer::renderCount()
{
    return 0;
}

QString HtmlRenderer::sanitizeHtml(const QString &html)
{
    // No litehtml to protect, but the EPUB reader still calls this
    // unconditionally before layout of its own (a plain-text fallback) - so
    // the stripping happens either way rather than being a second thing
    // every caller has to gate on NOTEAPPC_HAVE_HTML too.
    QString out = html;
    static const QRegularExpression script(
        QStringLiteral("<script\\b[^>]*>.*?</script\\s*>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    out.remove(script);
    static const QRegularExpression onAttribute(
        QStringLiteral("\\son[a-zA-Z]+\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
        QRegularExpression::CaseInsensitiveOption);
    out.remove(onAttribute);
    return out;
}

QImage HtmlRenderer::render(const QString &, const Theme &, int, const QString &, qreal, const QString &,
                            QList<TextRun> *, const std::function<QString(const QString &)> &)
{
    return {};
}

} // namespace noteAppC

#endif
