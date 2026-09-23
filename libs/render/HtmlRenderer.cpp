#include "HtmlRenderer.h"

#include <QRegularExpression> // used by sanitize() in both branches below

#ifdef NOTEAPPC_HAVE_LITEHTML

#include <QDir>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>

#include <atomic>

#include <litehtml.h>

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
/// is GPL too, which is exactly why NOTEAPPC_WITH_PDF exists and why it
/// links dynamically rather than compiling any of poppler's own code in).
/// litehtml's own document_container.h, which this class implements, is
/// part of the litehtml project and BSD-3-Clause like the rest of it.
///
/// It implements only what a static, once-per-block render needs: nothing
/// here tracks hover, click or scroll state, because the result this
/// produces is a cached QImage, not a live litehtml view.
class PainterContainer : public litehtml::document_container {
public:
    PainterContainer(HtmlRenderer::Theme theme, QString baseDir)
        : m_theme(std::move(theme)), m_baseDir(std::move(baseDir))
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
            // Numbered/lettered list styles: litehtml gives the item's
            // plain index, not the alphabet or numeral system's rendering
            // of it - a decimal is close enough for a note's own list.
            // ponytail: full roman/alpha/cjk numeral rendering, if a note
            // actually turns up needing one.
            painter->setPen(toQColor(marker.color));
            painter->drawText(box.adjusted(0, 0, 40, 0),
                              QString::number(marker.index) + QLatin1Char('.'));
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
        m_images.insert(path, QImage(path));
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
        painter->drawImage(toQRect(layer.border_box), image);
    }

    void draw_solid_fill(litehtml::uint_ptr, const litehtml::background_layer &layer,
                         const litehtml::web_color &color) override
    {
        if (!painter || color.alpha == 0)
            return;
        painter->fillRect(toQRect(layer.border_box), toQColor(color));
    }

    // A real gradient renderer (colour stops, angle and position math) is a
    // lot of machinery for chrome inside a note's HTML block - Obsidian's
    // own renderer does not treat these specially either. The first colour
    // stop stands in as a flat fill. ponytail: proper QLinearGradient/
    // QRadialGradient/QConicGradient support, if a note turns up that needs
    // the gradient itself rather than just a background colour.
    void draw_linear_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                              const litehtml::background_layer::linear_gradient &gradient) override
    {
        fillWithFirstStop(hdc, layer, gradient.color_points);
    }
    void draw_radial_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                              const litehtml::background_layer::radial_gradient &gradient) override
    {
        fillWithFirstStop(hdc, layer, gradient.color_points);
    }
    void draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                             const litehtml::background_layer::conic_gradient &gradient) override
    {
        fillWithFirstStop(hdc, layer, gradient.color_points);
    }

    void draw_borders(litehtml::uint_ptr, const litehtml::borders &borders, const litehtml::position &pos,
                      bool) override
    {
        if (!painter)
            return;
        // One rectangle in the dominant side's colour, not four independent
        // sides with mitred corners: a note's HTML block borders are simple
        // (a <table> cell, a <div> made to look like a callout), and this is
        // what those look like. ponytail: true per-side/radius drawing, if
        // a block turns up with mismatched border colours or a border-radius
        // worth honouring.
        const litehtml::border *chosen = nullptr;
        for (const litehtml::border *side : {&borders.top, &borders.left, &borders.right, &borders.bottom}) {
            if (side->width > 0 && side->style != litehtml::border_style_none) {
                chosen = side;
                break;
            }
        }
        if (!chosen)
            return;
        painter->save();
        painter->setPen(QPen(toQColor(chosen->color), qreal(chosen->width)));
        painter->setBrush(Qt::NoBrush);
        const qreal half = chosen->width / 2.0;
        painter->drawRect(toQRect(pos).adjusted(half, half, -half, -half));
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
    void fillWithFirstStop(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                           const litehtml::vector<litehtml::background_layer::color_point> &stops)
    {
        if (!stops.empty())
            draw_solid_fill(hdc, layer, stops.front().color);
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
        if (QDir::isRelativePath(resolved)) {
            const QString dir = !baseUrl.isEmpty() ? baseUrl : m_baseDir;
            resolved = QDir(dir).filePath(resolved);
        }
        const QFileInfo info(resolved);
        return info.exists() && info.isFile() ? info.absoluteFilePath() : QString();
    }

    HtmlRenderer::Theme m_theme;
    QString m_baseDir;
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

qint64 HtmlRenderer::renderCount()
{
    return g_renderCount.load();
}

QString HtmlRenderer::sanitize(const QString &html)
{
    return sanitise(html);
}

QImage HtmlRenderer::render(const QString &html, const Theme &theme, int widthPixels,
                            const QString &baseDir, qreal devicePixelRatio,
                            const QString &extraCss, QList<TextRun> *textRuns)
{
    if (widthPixels <= 0 || html.trimmed().isEmpty())
        return {};
    g_renderCount.fetch_add(1);

    PainterContainer container(theme, baseDir);
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

QString HtmlRenderer::sanitize(const QString &html)
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
                            QList<TextRun> *)
{
    return {};
}

} // namespace noteAppC

#endif
